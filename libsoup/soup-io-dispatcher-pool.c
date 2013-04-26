/*
 * soup-io-dispatcher-pool.c
 *
 *  Created on: 05.12.2012
 *      Author: vladimir
 */
#include <stdio.h>
#include <glib.h>
#include "soup-connection.h"
#include "soup-io-dispatcher-client.h"
#include "soup-io-dispatcher-pool.h"
#include "soup-uri.h"

typedef void (*SyncFuncFn) (SoupIODispatcherPool *io_disp_pool);

typedef struct {
	SyncFuncFn lock, unlock;

	GHashTable *hosts_io_dispatchers;
	GQueue *idle_io_dispatchers;

	GRecMutex mtx;
	guint idle_timeout;
	guint max_io_disps;
	guint max_io_disps_per_host;
	guint max_pipelined_msgs;
	guint response_block_size;

	gboolean make_all_conns_firstly;
	gboolean use_first_avail_conn;
	gboolean pipeline_via_proxy;
	gboolean pipeline_via_https;
	gboolean is_thread_safe;
} SoupIODispatcherPoolPrivate;

typedef struct {
	int spdy_supported_version;
	int max_pipelined_msgs;
	gboolean supports_http_pipelining;

	GQueue *io_dispatchers;
	SoupIODispatcher *spdy_io_dispatcher;
} HostInfo;

#define SOUP_IO_DISPATCHER_POOL_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), SOUP_TYPE_IO_DISPATCHER_POOL, SoupIODispatcherPoolPrivate))

G_DEFINE_TYPE (SoupIODispatcherPool, soup_io_dispatcher_pool, G_TYPE_OBJECT)

enum {
	PROP_0,
	PROP_IDLE_TIMEOUT,
	PROP_MAX_IO_DISPS,
	PROP_MAX_IO_DISPS_PER_HOST,
	PROP_MAX_PIPELINED_MSGS,
	PROP_MAKE_ALL_CONNS_FIRSTLY,
	PROP_USE_FIRST_AVAIL_CONN,
	PROP_PIPELINE_VIA_PROXY,
	PROP_PIPELINE_VIA_HTTPS,
	PROP_RESPONSE_BLOCK_SIZE,
	PROP_IS_THREAD_SAFE,
	LAST_PROP
};

/* static SoupIODispatcher* alloc_io_dispatcher (SoupIODispatcher *io_disp_pool, char *host); */
static void set_property (GObject *object, guint prop_id, const GValue *value,
		GParamSpec *pspec);
static void get_property (GObject *object, guint prop_id, GValue *value,
		GParamSpec *pspec);
static SoupIODispatcher* get_io_dispatcher (SoupIODispatcherPool *io_disp_pool,
		SoupMessage *msg, gboolean via_https, gboolean via_proxy);
static HostInfo* get_host_info (SoupIODispatcherPool *io_disp_pool,
		const char *host, guint port); // Lazy allocation of HostInfo
static void connection_established(SoupConnection* conn, SoupIODispatcher *io_disp);
static void connection_disconnected (SoupConnection *io_disp, gpointer *io_disp_and_pool);
static void io_dispatcher_idle_timeout (SoupIODispatcher *io_disp, SoupConnection *conn);
static void pipelining_is_not_supported (SoupIODispatcher *io_disp, GParamSpec  *pspec,
		SoupIODispatcherPool *io_disp_pool);

static inline void stub_lock (SoupIODispatcherPool *io_disp_pool);
static inline void lock (SoupIODispatcherPool *io_disp_pool);
static inline void unlock (SoupIODispatcherPool *io_disp_pool);


// Equal to max connections value in soup-session.c
#define SOUP_IO_DISPATCHER_POOL_MAX_IO_DISPS_DEFAULT 10
#define SOUP_IO_DISPATCHER_POOL_MAX_IO_DISPS_PER_HOST_DEFAULT 2
#define SOUP_IO_DISPATCHER_POOL_MAX_PIPELINED_MSGS_DEFAULT 4
#define SOUP_IO_DISPATCHER_POOL_MAX_PIPELINED_MSGS_CONSTRAINT 20
#define SOUP_IO_DISPATCHER_POOL_MAKE_ALL_CONNS_FIRSTLY_DEFAULT FALSE
#define SOUP_IO_DISPATCHER_POOL_USE_FIRST_AVAIL_CONN_DEFAULT FALSE
#define SOUP_IO_DISPATCHER_POOL_PIPELINE_VIA_PROXY_DEFAULT FALSE
#define SOUP_IO_DISPATCHER_POOL_PIPELINE_VIA_HTTPS_DEFAULT FALSE
#define SOUP_IO_DISPATCHER_POOL_RESPONSE_BLOCK_SIZE_DEFAULT 8192
#define SOUP_IO_DISPATCHER_POOL_RESPONSE_BLOCK_SIZE_CONSTRAINT 65536
#define SOUP_IO_DISPATCHER_POOL_IDLE_TIMEOUT_DEFAULT 3
#define SOUP_IO_DISPATCHER_POOL_IS_THREAD_SAFE_DEFAULT FALSE

static void
free_hash_table (GHashTable *hash_table)
{
	g_hash_table_unref (hash_table);
}

static void
free_host_info (HostInfo *host_info)
{
	g_queue_free_full (host_info->io_dispatchers, g_object_unref);
	g_free (host_info);
}

static void
soup_io_dispatcher_pool_init (SoupIODispatcherPool *io_disp_pool)
{
	SoupIODispatcherPoolPrivate *priv =
			SOUP_IO_DISPATCHER_POOL_GET_PRIVATE (io_disp_pool);

	priv->hosts_io_dispatchers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)free_hash_table);
	priv->idle_io_dispatchers = g_queue_new ();

	priv->idle_timeout = SOUP_IO_DISPATCHER_POOL_IDLE_TIMEOUT_DEFAULT;
	priv->max_io_disps = SOUP_IO_DISPATCHER_POOL_MAX_IO_DISPS_DEFAULT;
	priv->max_io_disps_per_host = SOUP_IO_DISPATCHER_POOL_MAX_IO_DISPS_PER_HOST_DEFAULT;
	priv->max_pipelined_msgs = SOUP_IO_DISPATCHER_POOL_MAX_PIPELINED_MSGS_DEFAULT;
	priv->response_block_size = SOUP_IO_DISPATCHER_POOL_RESPONSE_BLOCK_SIZE_DEFAULT;

	priv->make_all_conns_firstly = SOUP_IO_DISPATCHER_POOL_MAKE_ALL_CONNS_FIRSTLY_DEFAULT;
	priv->use_first_avail_conn = SOUP_IO_DISPATCHER_POOL_USE_FIRST_AVAIL_CONN_DEFAULT;
	priv->pipeline_via_proxy = SOUP_IO_DISPATCHER_POOL_PIPELINE_VIA_PROXY_DEFAULT;
	priv->pipeline_via_https = SOUP_IO_DISPATCHER_POOL_PIPELINE_VIA_HTTPS_DEFAULT;
	priv->is_thread_safe = SOUP_IO_DISPATCHER_POOL_IS_THREAD_SAFE_DEFAULT;

	priv->lock = priv->unlock = stub_lock;
}

static void
finalize (GObject *object)
{
	SoupIODispatcherPoolPrivate *priv = SOUP_IO_DISPATCHER_POOL_GET_PRIVATE (object);

	if (priv->is_thread_safe) {
		g_hash_table_destroy (priv->hosts_io_dispatchers);
		g_queue_free_full (priv->idle_io_dispatchers, g_object_unref);
		g_rec_mutex_clear (&priv->mtx);
	}

	G_OBJECT_CLASS(soup_io_dispatcher_pool_parent_class)->finalize(object);
}


static void
soup_io_dispatcher_pool_class_init (
		SoupIODispatcherPoolClass *io_disp_pool_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS(io_disp_pool_class);

	object_class->finalize = finalize;
	object_class->get_property = get_property;
	object_class->set_property = set_property;


	g_type_class_add_private (io_disp_pool_class,
			sizeof(SoupIODispatcherPoolPrivate));
	io_disp_pool_class->get_io_dispatcher = get_io_dispatcher;

	g_object_class_install_property (object_class, PROP_MAX_IO_DISPS,
			g_param_spec_int(SOUP_IO_DISPATCHER_POOL_MAX_IO_DISPS,
					"Max IO dispatchers count",
					"The maximum number of IO dispatchers that the IO dispatcher pool can have at once",
					1, G_MAXINT, SOUP_IO_DISPATCHER_POOL_MAX_IO_DISPS_DEFAULT,
					G_PARAM_READWRITE));

	g_object_class_install_property (object_class, PROP_MAX_IO_DISPS_PER_HOST,
			g_param_spec_int(SOUP_IO_DISPATCHER_POOL_MAX_IO_DISPS_PER_HOST,
					"Max IO dispatchers count per host",
					"The maximum number of IO dispatchers that the IO dispatcher pool can allocate per host",
					1, G_MAXINT,
					SOUP_IO_DISPATCHER_POOL_MAX_IO_DISPS_PER_HOST_DEFAULT,
					G_PARAM_READWRITE));

	g_object_class_install_property (object_class, PROP_MAX_PIPELINED_MSGS,
			g_param_spec_uint(SOUP_IO_DISPATCHER_POOL_MAX_PIPELINED_MSGS,
					"Max pipelined messages", "Max pipelined messages", 1,
					SOUP_IO_DISPATCHER_POOL_MAX_PIPELINED_MSGS_CONSTRAINT,
					SOUP_IO_DISPATCHER_POOL_MAX_PIPELINED_MSGS_DEFAULT,
					G_PARAM_READWRITE));

	g_object_class_install_property (object_class, PROP_MAKE_ALL_CONNS_FIRSTLY,
			g_param_spec_boolean(SOUP_IO_DISPATCHER_POOL_MAKE_ALL_CONNS_FIRSTLY,
					"Make all IO dispatchers firstly",
					"Distribute requests over existing connections or make new connection",
					SOUP_IO_DISPATCHER_POOL_MAKE_ALL_CONNS_FIRSTLY_DEFAULT,
					G_PARAM_READWRITE));

	g_object_class_install_property (object_class, PROP_USE_FIRST_AVAIL_CONN,
			g_param_spec_boolean(SOUP_IO_DISPATCHER_POOL_USE_FIRST_AVAIL_CONN,
					"First suitable connection",
					"Use first suitable connection else find connection with minimal queue length",
					SOUP_IO_DISPATCHER_POOL_USE_FIRST_AVAIL_CONN_DEFAULT,
					G_PARAM_READWRITE));

	g_object_class_install_property (object_class, PROP_PIPELINE_VIA_PROXY,
			g_param_spec_boolean(SOUP_IO_DISPATCHER_POOL_PIPELINE_VIA_PROXY,
					"Pipeline via proxy", "Do HTTP pipelining via proxy",
					SOUP_IO_DISPATCHER_POOL_PIPELINE_VIA_PROXY_DEFAULT,
					G_PARAM_READWRITE));

	g_object_class_install_property (object_class, PROP_PIPELINE_VIA_HTTPS,
			g_param_spec_boolean(SOUP_IO_DISPATCHER_POOL_PIPELINE_VIA_HTTPS,
					"Pipeline via HTTPS", "Do HTTP pipelining via HTTPS",
					SOUP_IO_DISPATCHER_POOL_PIPELINE_VIA_HTTPS_DEFAULT,
					G_PARAM_READWRITE));

	g_object_class_install_property (object_class, PROP_RESPONSE_BLOCK_SIZE,
			g_param_spec_uint(SOUP_IO_DISPATCHER_POOL_RESPONSE_BLOCK_SIZE,
					"Response block size", "IO response block size", 1,
					SOUP_IO_DISPATCHER_POOL_RESPONSE_BLOCK_SIZE_CONSTRAINT,
					SOUP_IO_DISPATCHER_POOL_RESPONSE_BLOCK_SIZE_DEFAULT,
					G_PARAM_READWRITE));

	g_object_class_install_property (object_class, PROP_IDLE_TIMEOUT,
			g_param_spec_uint(SOUP_IO_DISPATCHER_POOL_IDLE_TIMEOUT,
					"Idle timeout", "IO idle timeout", 0, G_MAXUINT,
					SOUP_IO_DISPATCHER_POOL_IDLE_TIMEOUT_DEFAULT,
					G_PARAM_READWRITE));

	g_object_class_install_property (object_class, PROP_IS_THREAD_SAFE,
			g_param_spec_boolean (SOUP_IO_DISPATCHER_POOL_IS_THREAD_SAFE,
					"Is SoupIODispatcherPool thread-safe", "Is SoupIODispatcherPool thread-safe",
					SOUP_IO_DISPATCHER_POOL_IS_THREAD_SAFE_DEFAULT,
					G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

}

SoupIODispatcherPool*
soup_io_dispatcher_pool_new (const char *propname1, ...)
{
	SoupIODispatcherPool *io_disp_pool;

	va_list ap;

	va_start(ap, propname1);
	io_disp_pool = (SoupIODispatcherPool*)g_object_new_valist (SOUP_TYPE_IO_DISPATCHER_POOL, propname1, ap);

	va_end(ap);

	return io_disp_pool;
}

void
soup_io_dispatcher_pool_enable_spdy_support (
		SoupIODispatcherPool *io_disp_pool, const char *host, int version)
{
	SoupIODispatcherPoolPrivate *priv;
	HostInfo * host_info;

	g_return_if_fail(SOUP_IS_IO_DISPATCHER_POOL (io_disp_pool));
	g_return_if_fail(host);
	g_return_if_fail(version);

	priv = SOUP_IO_DISPATCHER_POOL_GET_PRIVATE (io_disp_pool);

	priv->lock (io_disp_pool);

	priv = SOUP_IO_DISPATCHER_POOL_GET_PRIVATE (io_disp_pool);
	if ((host_info = (HostInfo*) g_hash_table_lookup(priv->hosts_io_dispatchers, host))) {
		host_info->spdy_supported_version = version;
	}

	priv->unlock (io_disp_pool);
}

void
soup_io_dispatcher_pool_set_max_pipelined_msgs_for_host (SoupIODispatcherPool *io_disp_pool,
		const char *host, int max_queue_length)
{
	SoupIODispatcherPoolPrivate *priv;

	g_return_if_fail (SOUP_IS_IO_DISPATCHER_POOL (io_disp_pool));
	g_return_if_fail (host);

	priv = SOUP_IO_DISPATCHER_POOL_GET_PRIVATE (io_disp_pool);

	priv->lock (io_disp_pool);
	do {
		HostInfo *host_info;

		host_info = (HostInfo*)g_hash_table_lookup (priv->hosts_io_dispatchers, host);
		if (!host_info)
			break;

		host_info->max_pipelined_msgs = max_queue_length;

		if (host_info->io_dispatchers) {
			GList *head = g_queue_peek_head_link (host_info->io_dispatchers);
			while (head) {
				soup_io_dispatcher_set_max_pipelined_requests ((SoupIODispatcher*)head->data, max_queue_length);
				head = head->next;
			}
		}

	} while (1);
	priv->unlock (io_disp_pool);
}

SoupIODispatcher*
soup_io_dispatcher_pool_alloc_io_dispatcher (SoupIODispatcherPool *io_disp_pool,
		SoupURI *uri, GMainContext *async_context, SoupConnection *conn, gboolean via_proxy)
{
	SoupIODispatcherPoolPrivate *priv;
	SoupIODispatcher *io_disp;
	HostInfo *host_info;
	gpointer *io_disp_and_pool;
	g_return_val_if_fail (SOUP_IS_IO_DISPATCHER_POOL(io_disp_pool), NULL);
	g_return_val_if_fail (SOUP_IS_CONNECTION (conn), NULL);

	priv = SOUP_IO_DISPATCHER_POOL_GET_PRIVATE (io_disp_pool);

	priv->lock (io_disp_pool);
	host_info = get_host_info (io_disp_pool, uri->host, uri->port);

	io_disp = g_queue_pop_head (priv->idle_io_dispatchers);

	if (!io_disp) {
		io_disp = g_object_new (SOUP_TYPE_IO_DISPATCHER_CLIENT, NULL);
	}

	g_object_set (G_OBJECT (io_disp), SOUP_IO_DISPATCHER_IS_VIA_PROXY, via_proxy,
			SOUP_IO_DISPATCHER_HOST, uri,
			SOUP_IO_DISPATCHER_MAX_PIPELINED_REQ, priv->max_pipelined_msgs,
			SOUP_IO_DISPATCHER_POOL_RESPONSE_BLOCK_SIZE, priv->response_block_size,
			SOUP_IO_DISPATCHER_IS_THREAD_SAFE, priv->is_thread_safe,
			SOUP_IO_DISPATCHER_ASYNC_CONTEXT, async_context,
			SOUP_IO_DISPATCHER_IDLE_TIMEOUT, priv->idle_timeout,
			NULL);

	g_queue_push_tail (host_info->io_dispatchers, io_disp);

	g_signal_connect (io_disp, "notify::is-pipelining-supported", G_CALLBACK (pipelining_is_not_supported), io_disp_pool);
//	g_signal_connect (io_disp, "notify::is-queue-empty", G_CALLBACK (optimize_distribution_of_msgs), io_disp_pool);
	g_signal_connect (conn, "connected", G_CALLBACK (connection_established), io_disp);
	io_disp_and_pool = (gpointer*)g_malloc(2 * sizeof (gpointer));
	io_disp_and_pool[0] = io_disp_pool;
	io_disp_and_pool[1] = io_disp;
	g_signal_connect (conn, "disconnected", G_CALLBACK (connection_disconnected), io_disp_and_pool);
	g_signal_connect (io_disp, "idle-timeout", G_CALLBACK (io_dispatcher_idle_timeout), conn);

	g_object_set_data (G_OBJECT (io_disp), "conn", conn);
	soup_connection_set_io_dispatcher (conn, io_disp);

	priv->unlock (io_disp_pool);
	return io_disp;
}

SoupConnection*
soup_io_dispatcher_pool_get_conn (SoupIODispatcherPool *io_disp_pool, SoupIODispatcher *io_disp)
{
	g_return_val_if_fail (SOUP_IS_IO_DISPATCHER_POOL (io_disp_pool), NULL);
	g_return_val_if_fail (SOUP_IS_IO_DISPATCHER (io_disp), NULL);

	return (SoupConnection*)g_object_get_data (G_OBJECT (io_disp), "conn");
}

SoupIODispatcher*
soup_io_dispatcher_pool_get_io_dispatcher (SoupIODispatcherPool *io_disp_pool,
		SoupMessage *msg, gboolean via_https, gboolean via_proxy)
{
	g_return_val_if_fail(SOUP_IS_IO_DISPATCHER_POOL(io_disp_pool), NULL);
	g_return_val_if_fail(SOUP_IS_MESSAGE (msg), NULL);

	return SOUP_IO_DISPATCHER_POOL_GET_CLASS (io_disp_pool)->get_io_dispatcher(
			io_disp_pool, msg, via_https, via_proxy);
}

static void
iterate_over_dispatchers (GHashTable *hash_table,
		void (*io_disp_func) (SoupIODispatcher*, guint), guint new_value)
{
	GHashTableIter host_iter;
	gpointer host_key, host_value;
	g_hash_table_iter_init (&host_iter, hash_table);
	while (g_hash_table_iter_next (&host_iter, &host_key, &host_value)) {
		GHashTableIter port_iter;
		gpointer port_key, port_value;
		g_hash_table_iter_init (&port_iter, (GHashTable*)host_value);
		while (g_hash_table_iter_next (&port_iter, &port_key, &port_value)) {
			GList *link = g_queue_peek_head_link(
					((HostInfo*) port_value)->io_dispatchers);
			while (link) {
				SoupIODispatcher *io_disp = (SoupIODispatcher*) (link->data);
				io_disp_func (io_disp, new_value);
				link = link->next;
			}
		}
	}
}

static void
set_max_pipelined_requests (gpointer data, gpointer user_data)
{
	soup_io_dispatcher_set_max_pipelined_requests ((SoupIODispatcher*)data, *(guint*)user_data);
}

static void
set_response_block_size (gpointer data, gpointer user_data)
{
	soup_io_dispatcher_set_response_block_size ((SoupIODispatcher*)data, *(guint*)user_data);
}

static void
set_property (GObject *object, guint prop_id, const GValue *value,
		GParamSpec *pspec)
{
	SoupIODispatcherPool *io_disp_pool = SOUP_IO_DISPATCHER_POOL (object);
	SoupIODispatcherPoolPrivate *priv = SOUP_IO_DISPATCHER_POOL_GET_PRIVATE(io_disp_pool);
	gboolean is_thread_safe = priv->is_thread_safe;

	if (is_thread_safe)
		priv->lock (io_disp_pool);

	switch (prop_id) {
	case PROP_MAX_IO_DISPS:
		priv->max_io_disps = g_value_get_int(value);
		break;
	case PROP_MAX_IO_DISPS_PER_HOST:
		priv->max_io_disps_per_host = g_value_get_int(value);
		break;
	case PROP_MAX_PIPELINED_MSGS: {
		guint new_value = g_value_get_uint(value);
		if (priv->max_pipelined_msgs == new_value)
			break;

		priv->max_pipelined_msgs = new_value;
		iterate_over_dispatchers (priv->hosts_io_dispatchers,
				&soup_io_dispatcher_set_max_pipelined_requests,
				priv->max_pipelined_msgs);

		g_queue_foreach(priv->idle_io_dispatchers,
				(GFunc)set_max_pipelined_requests,
				&priv->max_pipelined_msgs);

		break;
	}
	case PROP_MAKE_ALL_CONNS_FIRSTLY:
		priv->make_all_conns_firstly = g_value_get_boolean(value);
		break;
	case PROP_USE_FIRST_AVAIL_CONN:
		priv->use_first_avail_conn = g_value_get_boolean(value);
		break;
	case PROP_PIPELINE_VIA_PROXY:
		priv->pipeline_via_proxy = g_value_get_boolean(value);
		break;
	case PROP_PIPELINE_VIA_HTTPS:
		priv->pipeline_via_https = g_value_get_boolean(value);
		break;
	case PROP_RESPONSE_BLOCK_SIZE: {
		guint new_value = g_value_get_uint(value);
		if (priv->response_block_size == new_value)
			break;

		priv->response_block_size = new_value;

		iterate_over_dispatchers(priv->hosts_io_dispatchers,
				&soup_io_dispatcher_set_response_block_size,
				priv->response_block_size);

		g_queue_foreach (priv->idle_io_dispatchers,
				(GFunc)set_response_block_size,
				&priv->response_block_size);

		break;
	}
	case PROP_IDLE_TIMEOUT:
		priv->idle_timeout = g_value_get_uint(value);
		break;

	case PROP_IS_THREAD_SAFE:
		priv->is_thread_safe = g_value_get_boolean (value);

		if (priv->is_thread_safe) {
			priv->lock = lock;
			priv->unlock = unlock;

			g_rec_mutex_init (&priv->mtx);
		} else {
			priv->lock = priv->unlock = stub_lock;
		}
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}

	if (is_thread_safe)
		priv->unlock (io_disp_pool);

}

static void
get_property (GObject *object, guint prop_id, GValue *value,
		GParamSpec *pspec)
{
	SoupIODispatcherPool *io_disp_pool = SOUP_IO_DISPATCHER_POOL (object);
	SoupIODispatcherPoolPrivate *priv =
			SOUP_IO_DISPATCHER_POOL_GET_PRIVATE (io_disp_pool);

	priv->lock (io_disp_pool);

	switch (prop_id) {
	case PROP_MAX_IO_DISPS:
		g_value_set_int(value, priv->max_io_disps);
		break;
	case PROP_MAX_IO_DISPS_PER_HOST:
		g_value_set_int(value, priv->max_io_disps_per_host);
		break;
	case PROP_MAX_PIPELINED_MSGS:
		g_value_set_uint(value, priv->max_pipelined_msgs);
		break;
	case PROP_MAKE_ALL_CONNS_FIRSTLY:
		g_value_set_boolean(value, priv->make_all_conns_firstly);
		break;
	case PROP_USE_FIRST_AVAIL_CONN:
		g_value_set_boolean(value, priv->use_first_avail_conn);
		break;
	case PROP_PIPELINE_VIA_PROXY:
		g_value_set_boolean(value, priv->pipeline_via_proxy);
		break;
	case PROP_PIPELINE_VIA_HTTPS:
		g_value_set_boolean(value, priv->pipeline_via_https);
		break;
	case PROP_RESPONSE_BLOCK_SIZE:
		g_value_set_uint(value, priv->response_block_size);
		break;
	case PROP_IDLE_TIMEOUT:
		g_value_set_uint(value, priv->idle_timeout);
		break;
	case PROP_IS_THREAD_SAFE:
		g_value_set_boolean (value, priv->is_thread_safe);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}

	priv->unlock (io_disp_pool);

}

static SoupIODispatcher*
get_io_dispatcher (SoupIODispatcherPool *io_disp_pool, SoupMessage *msg,
		gboolean via_https, gboolean via_proxy)
{
	SoupIODispatcherPoolPrivate *priv =
			SOUP_IO_DISPATCHER_POOL_GET_PRIVATE (io_disp_pool);
	GList *iter, *result_link = NULL;
	SoupIODispatcher *io_disp;
	SoupURI *uri;
	guint io_disp_queue_length = 0;
	gboolean dont_use_http_pipelining;
	HostInfo *host_info;
	const char *c_conn;

	priv->lock (io_disp_pool);
	uri = soup_message_get_uri(msg);
	host_info = get_host_info (io_disp_pool, uri->host, uri->port);

	if (host_info->spdy_supported_version) {
		priv->unlock (io_disp_pool);
		return host_info->spdy_io_dispatcher;
	}

	if (priv->make_all_conns_firstly
			&& (g_queue_get_length(host_info->io_dispatchers) < priv->max_io_disps_per_host)) {
		priv->unlock (io_disp_pool);
		return NULL ;
	}

	c_conn = soup_message_headers_get_list (msg->request_headers,
					 "Connection");

	dont_use_http_pipelining = (via_proxy && !priv->pipeline_via_proxy)
									|| (via_https && !priv->pipeline_via_https)
									|| !host_info->supports_http_pipelining
									|| (c_conn && soup_header_contains (c_conn, "close"));

	iter = g_queue_peek_head_link (host_info->io_dispatchers);
	for (; iter; iter = iter->next) {
		io_disp = (SoupIODispatcher*)iter->data;
		if (soup_io_dispatcher_get_socket (io_disp)
				&& ((dont_use_http_pipelining && soup_io_dispatcher_is_queue_empty(io_disp))
				|| (!dont_use_http_pipelining && !soup_io_dispatcher_is_queue_full(io_disp)))) {

			guint queue_length = soup_io_dispatcher_get_queue_length(io_disp);
			if (!result_link || (queue_length < io_disp_queue_length)) {
				result_link = iter;
				io_disp_queue_length = queue_length;

				if (priv->use_first_avail_conn || !queue_length)
					break;
			}
		}
	}

	if (!result_link) {
		priv->unlock (io_disp_pool);
		return NULL;
	}

	io_disp = (SoupIODispatcher*) result_link->data;

	//TODO: Please, don't notify SoupIODispatcherPool or SoupSession about this!
	soup_io_dispatcher_set_pipelining_support (io_disp, !dont_use_http_pipelining);

	//	g_queue_delete_link (host_info->io_dispatchers, result_link);
	priv->unlock (io_disp_pool);

	return io_disp;
}

static HostInfo*
get_host_info (SoupIODispatcherPool *io_disp_pool, const char *host, guint port)
{
	SoupIODispatcherPoolPrivate *priv =
			SOUP_IO_DISPATCHER_POOL_GET_PRIVATE (io_disp_pool);
	GHashTable *hosts;
	HostInfo *host_info;

	priv->lock (io_disp_pool);

	if (!(hosts = (GHashTable*)g_hash_table_lookup (priv->hosts_io_dispatchers, host))) {
		hosts = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)free_host_info);
		g_hash_table_insert (priv->hosts_io_dispatchers, g_strdup (host), hosts);
	}

	if (!(host_info = (HostInfo*)g_hash_table_lookup (hosts, GUINT_TO_POINTER (port)))) {
		host_info = (HostInfo*) g_malloc0 (sizeof(HostInfo));
		host_info->io_dispatchers = g_queue_new();
		host_info->max_pipelined_msgs = priv->max_pipelined_msgs;
		host_info->supports_http_pipelining = TRUE;
		g_hash_table_insert (hosts, GUINT_TO_POINTER (port), host_info);
	}

	priv->unlock (io_disp_pool);

	return host_info;
}

static void
connection_established (SoupConnection* conn, SoupIODispatcher *io_disp)
{
	soup_io_dispatcher_set_socket (io_disp, soup_connection_get_socket (conn));
}

static void
connection_disconnected (SoupConnection *conn, gpointer *io_disp_and_pool)
{
	SoupIODispatcherPool *io_disp_pool = (SoupIODispatcherPool*)io_disp_and_pool[0];
	SoupIODispatcher *io_disp = (SoupIODispatcher*)io_disp_and_pool[1];
	SoupIODispatcherPoolPrivate *priv = SOUP_IO_DISPATCHER_POOL_GET_PRIVATE (io_disp_pool);
	SoupURI *uri;
	GHashTable *hosts;
	HostInfo *host_info;

	g_free (io_disp_and_pool);

	priv->lock (io_disp_pool);

	g_signal_handlers_disconnect_by_func (io_disp, G_CALLBACK (io_dispatcher_idle_timeout), conn);

	g_object_get (io_disp, SOUP_IO_DISPATCHER_HOST, &uri, NULL);
	hosts = g_hash_table_lookup (priv->hosts_io_dispatchers, uri->host);
	host_info = g_hash_table_lookup (hosts, GUINT_TO_POINTER (uri->port));
	g_queue_remove (host_info->io_dispatchers, io_disp);
	g_queue_push_tail (priv->idle_io_dispatchers, io_disp);

	priv->unlock (io_disp_pool);
	soup_io_dispatcher_set_socket (io_disp, NULL);
}

static void
io_dispatcher_idle_timeout (SoupIODispatcher *io_disp, SoupConnection *conn)
{
	soup_connection_disconnect (conn);
}

static void
pipelining_is_not_supported (SoupIODispatcher *io_disp, GParamSpec  *pspec,
		SoupIODispatcherPool *io_disp_pool)
{
	SoupIODispatcherPoolPrivate *priv = SOUP_IO_DISPATCHER_POOL_GET_PRIVATE (io_disp_pool);

	if (!soup_io_dispatcher_is_pipelining_supported (io_disp)) {
		SoupURI *host;
		HostInfo *host_info;

		priv->lock (io_disp_pool);

		g_object_get (io_disp, SOUP_IO_DISPATCHER_HOST, &host, NULL);
		host_info = get_host_info (io_disp_pool, host->host, host->port);
		host_info->supports_http_pipelining = FALSE;
		g_signal_handlers_disconnect_by_func (io_disp, G_CALLBACK (pipelining_is_not_supported), io_disp_pool);

		priv->unlock (io_disp_pool);
	}
}

static inline void
stub_lock (SoupIODispatcherPool *io_disp_pool)
{
}

static inline void
lock (SoupIODispatcherPool *io_disp_pool)
{
	SoupIODispatcherPoolPrivate *priv = SOUP_IO_DISPATCHER_POOL_GET_PRIVATE (io_disp_pool);
	g_object_ref (io_disp_pool);
	g_rec_mutex_lock (&priv->mtx);
}

static inline void
unlock (SoupIODispatcherPool *io_disp_pool)
{
	SoupIODispatcherPoolPrivate *priv = SOUP_IO_DISPATCHER_POOL_GET_PRIVATE (io_disp_pool);
	g_rec_mutex_unlock (&priv->mtx);
	g_object_unref (io_disp_pool);
}
