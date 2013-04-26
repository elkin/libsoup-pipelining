/*
 * Copyright (C) 2012, LG Electronics
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>
#include <string.h>
#include "soup-io-dispatcher.h"
#include "soup-io-dispatcher-misc.h"
#include "soup-marshal.h"
#include "soup-message.h"
#include "soup-message-body.h"
#include "soup-message-private.h"
#include "soup-misc.h"
#include "soup-socket.h"
#include "soup-uri.h"


typedef void (*SyncFuncFn) (SoupIODispatcher *io_disp, GRecMutex *mtx);
typedef struct {
	SoupSocket *socket;
	GMainContext *async_context;
	SoupURI *host;
	GSource *idle_timeout_src;
	GQueue *read_io_queue, *write_io_queue, *paused_io_queue, *io_data_mtx_pool,
	*input_msg_queue;
	guchar *read_buf;
	guint max_pipelined_requests, response_block_size, finished_requests,
	idle_timeout;
	guint read_io_queue_length, write_io_queue_length, paused_io_queue_length, input_msg_queue_length;
	GRecMutex queue_mtx, io_disp_mtx;

	SyncFuncFn lock, unlock;

	gboolean is_pipelining_supported:1, idle:1, is_queue_full:1,
	disconnect_if_empty:1, is_thread_safe:1, is_via_proxy:3;
} SoupIODispatcherPrivate;

#define SOUP_IO_DISPATCHER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), SOUP_TYPE_IO_DISPATCHER, SoupIODispatcherPrivate))

static gboolean process_queue (SoupIODispatcher *io_disp, SoupProcessIOQueueFn io_func,
		GQueue *queue, guint *queue_length,
		ItemState* (*get_item_state)(SoupMessageIOData*));

static void cancel_message (SoupIODispatcher *io_disp, SoupMessage *msg);

static void pause_message (SoupIODispatcher *io_disp, SoupMessage *msg);
static void unpause_message (SoupIODispatcher *io_disp, SoupMessage *msg);
static gboolean unpause_cb (gpointer user_data);

static void readable_cb (SoupSocket *socket, SoupIODispatcher *io_disp);
static void writable_cb (SoupSocket *socket, SoupIODispatcher *io_disp);

static void set_property (GObject *object, guint prop_id,
		const GValue *value, GParamSpec *pspec);
static void get_property (GObject *object, guint prop_id,
		GValue *value, GParamSpec *pspec);

static gpointer io_data_new (SoupIODispatcher *io_disp, SoupMessage *msg, GCancellable *cancellable,
		SoupMessageCompletionFn completion_cb, gpointer user_data);
static void io_data_error (SoupIODispatcher *io_disp, SoupMessageIOData *io_data);
static void io_data_cleanup (SoupIODispatcher *io_disp, SoupMessageIOData *io_data);

static SoupBuffer *content_decode_one (SoupBuffer *buf, GConverter *converter, GError **error);
static SoupBuffer *content_decode (SoupMessage *msg, SoupBuffer *buf);
static void properties_changed (SoupIODispatcher *io_disp);
static inline void stub_lock (SoupIODispatcher *io_disp, GRecMutex *mtx);
static inline void lock (SoupIODispatcher *io_disp, GRecMutex *mtx);
static inline void unlock (SoupIODispatcher *io_disp, GRecMutex *mtx);
static inline void push_link (GQueue *queue, SoupMessageIOData *io_data, GList **link, SoupIODispatcher *io_disp);
static inline void delete_link (GQueue *queue, GList **link, SoupIODispatcher *io_disp);

static void reset (SoupIODispatcherPrivate *priv);
static void start_idle_timer (SoupIODispatcher *io_disp);
static void stop_idle_timer (SoupIODispatcher *io_disp);
static gboolean idle_timeout (gpointer io_disp);

#define SOUP_MAX_PIPELINED_REQ_CONSTRAINT 20
#define SOUP_IO_DISPATCHER_RESPONSE_BLOCK_SIZE_CONSTRAINT 65536
#define SOUP_IO_DISPATCHER_MAX_PIPELINED_REQ_DEFAULT 1
#define SOUP_IO_DISPATCHER_IS_PIPELINING_SUPPORTED_DEFAULT TRUE
#define SOUP_IO_DISPATCHER_IS_VIA_PROXY_DEFAULT FALSE
#define SOUP_IO_DISPATCHER_IS_THREAD_SAFE_DEFAULT FALSE
#define SOUP_IO_DISPATCHER_RESPONSE_BLOCK_SIZE_DEFAULT 8192

/* Number of seconds after which we close a connection that hasn't yet
 * been used.
 */
#define SOUP_IO_DISPATCHER_IDLE_TIMEOUT_DEFAULT 3

G_DEFINE_ABSTRACT_TYPE (SoupIODispatcher, soup_io_dispatcher, G_TYPE_OBJECT)

enum {
	IO_MSG_RESTART,
	IDLE_TIMEOUT,
	IO_ERROR,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

enum {
	PROP_0,

	PROP_HOST,
	PROP_SOCKET,
	PROP_ASYNC_CONTEXT,
	PROP_IS_QUEUE_EMPTY,
	PROP_IS_QUEUE_FULL,
	PROP_IS_PIPELINING_SUPPORTED,
	PROP_IS_VIA_PROXY,
	PROP_IS_THREAD_SAFE,
	PROP_MAX_PIPELINED_REQ,
	PROP_RESPONSE_BLOCK_SIZE,
	PROP_IDLE_TIMEOUT,
	LAST_PROP
};


static void
soup_io_dispatcher_init(SoupIODispatcher *io_disp)
{
	SoupIODispatcherPrivate *priv = SOUP_IO_DISPATCHER_GET_PRIVATE (io_disp);
	priv->is_thread_safe = SOUP_IO_DISPATCHER_IS_THREAD_SAFE_DEFAULT;
	if (priv->is_thread_safe) {
		priv->lock = lock;
		priv->unlock = unlock;
	} else {
		priv->lock = stub_lock;
		priv->unlock = stub_lock;
	}
	priv->host = NULL;
	priv->io_data_mtx_pool = g_queue_new ();
	priv->read_io_queue = g_queue_new ();
	priv->write_io_queue = g_queue_new ();
	priv->paused_io_queue = g_queue_new ();
	priv->input_msg_queue = g_queue_new ();
	reset (SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp));
}

static void
finalize(GObject *object)
{
	SoupIODispatcher *io_disp;
	SoupIODispatcherPrivate *priv;
	GRecMutex *mtx;

	g_return_if_fail (SOUP_IS_IO_DISPATCHER (object));
	io_disp = SOUP_IO_DISPATCHER (object);
	priv = SOUP_IO_DISPATCHER_GET_PRIVATE (object);

	if (priv->host) {
		soup_uri_free (priv->host);
		priv->host = NULL;
	}

	if (priv->socket) {
		g_signal_handlers_disconnect_by_func(priv->socket, readable_cb, io_disp);
		g_signal_handlers_disconnect_by_func(priv->socket, writable_cb, io_disp);
		g_object_unref(priv->socket);
		priv->socket = NULL;
	}

	while (!g_queue_is_empty(priv->read_io_queue)) {
		io_data_cleanup(SOUP_IO_DISPATCHER(object), (SoupMessageIOData*)g_queue_peek_head (priv->read_io_queue));
	}

	while (!g_queue_is_empty (priv->write_io_queue)) {
		io_data_cleanup(SOUP_IO_DISPATCHER(object), (SoupMessageIOData*)g_queue_peek_head (priv->write_io_queue));
	}

	if (priv->read_io_queue) {
		g_queue_free (priv->read_io_queue);
		priv->read_io_queue = NULL;
	}

	if (priv->write_io_queue) {
		g_queue_free (priv->write_io_queue);
		priv->write_io_queue = NULL;
	}

	if (priv->paused_io_queue) {
		g_queue_free (priv->paused_io_queue);
		priv->paused_io_queue = NULL;
	}

	if (priv->input_msg_queue) {
		g_queue_free (priv->input_msg_queue);
		priv->input_msg_queue = NULL;
	}

	g_slice_free1 (priv->response_block_size, priv->read_buf);

	if (priv->io_data_mtx_pool) {
		while ((mtx = (GRecMutex *)g_queue_pop_tail(priv->io_data_mtx_pool))) {
			g_rec_mutex_clear (mtx);
			g_slice_free1 (sizeof(GRecMutex), mtx);
		}

		g_queue_free (priv->io_data_mtx_pool);
		priv->io_data_mtx_pool = NULL;
	}


	if (priv->is_thread_safe) {
		g_rec_mutex_clear (&priv->queue_mtx);
		g_rec_mutex_clear (&priv->io_disp_mtx);
	}

	G_OBJECT_CLASS(soup_io_dispatcher_parent_class)->finalize (object);
}

static void
soup_io_dispatcher_class_init(SoupIODispatcherClass *io_disp_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS(io_disp_class);
	io_disp_class->cancel_message = cancel_message;
	io_disp_class->pause_message = pause_message;
	io_disp_class->unpause_message = unpause_message;

	g_type_class_add_private (io_disp_class, sizeof (SoupIODispatcherPrivate));

	object_class->set_property = set_property;
	object_class->get_property = get_property;
	object_class->finalize = finalize;

	/* signals */
	signals[IO_MSG_RESTART] =
			g_signal_new("io-msg-restart",
					G_OBJECT_CLASS_TYPE (object_class),
					G_SIGNAL_RUN_FIRST,
					G_STRUCT_OFFSET(SoupIODispatcherClass, io_msg_restart),
					NULL, NULL,
					_soup_marshal_NONE__OBJECT,
					G_TYPE_NONE, 1,
					SOUP_TYPE_MESSAGE);

	signals[IDLE_TIMEOUT] =
			g_signal_new ("idle-timeout",
					G_OBJECT_CLASS_TYPE (object_class),
					G_SIGNAL_RUN_FIRST,
					G_STRUCT_OFFSET (SoupIODispatcherClass, idle_timeout),
					NULL, NULL,
					_soup_marshal_NONE__NONE,
					G_TYPE_NONE, 0);

/* properties */
	g_object_class_install_property (
			object_class, PROP_ASYNC_CONTEXT,
			g_param_spec_pointer (SOUP_IO_DISPATCHER_ASYNC_CONTEXT,
					"Async GMainContext",
					"The GMainContext to dispatch async I/O in",
					G_PARAM_READWRITE));


	g_object_class_install_property (
			object_class, PROP_HOST,
			g_param_spec_boxed (SOUP_IO_DISPATCHER_HOST,
					"Hostname",
					"Hostname",
					SOUP_TYPE_URI,
					G_PARAM_READWRITE));

	g_object_class_install_property (
			object_class, PROP_SOCKET,
			g_param_spec_object(SOUP_IO_DISPATCHER_SOCKET,
					"Socket",
					"SoupSocket for I/O operations",
					SOUP_TYPE_SOCKET,
					G_PARAM_READWRITE));

	g_object_class_install_property (
			object_class, PROP_IS_QUEUE_EMPTY,
			g_param_spec_boolean (SOUP_IO_DISPATCHER_IS_QUEUE_EMPTY,
					"Is idle",
					"Is IO dispatcher in idle state",
					TRUE,
					G_PARAM_READABLE));

	g_object_class_install_property (
			object_class, PROP_IS_QUEUE_FULL,
			g_param_spec_boolean (SOUP_IO_DISPATCHER_IS_QUEUE_FULL,
					"Is queue full",
					"Is IO dispatcher queue full",
					FALSE,
					G_PARAM_READABLE));

	g_object_class_install_property (
			object_class, PROP_IS_PIPELINING_SUPPORTED,
			g_param_spec_boolean (SOUP_IO_DISPATCHER_IS_PIPELINING_SUPPORTED,
					"Is pipelining supported",
					"Does IO dispatcher support pipelining",
					SOUP_IO_DISPATCHER_IS_PIPELINING_SUPPORTED_DEFAULT,
					G_PARAM_READWRITE));

	g_object_class_install_property (
			object_class, PROP_IS_VIA_PROXY,
			g_param_spec_boolean (SOUP_IO_DISPATCHER_IS_VIA_PROXY,
					"Is via proxy",
					"Does IO dispatcher process requests via proxy",
					SOUP_IO_DISPATCHER_IS_VIA_PROXY_DEFAULT,
					G_PARAM_READWRITE));


	g_object_class_install_property (
			object_class, PROP_IS_THREAD_SAFE,
			g_param_spec_boolean (SOUP_IO_DISPATCHER_IS_THREAD_SAFE,
					"Is thread safe",
					"Whether IO dispatcher methods should be thread-safe",
					SOUP_IO_DISPATCHER_IS_THREAD_SAFE_DEFAULT,
					G_PARAM_READWRITE));


	g_object_class_install_property (
			object_class, PROP_MAX_PIPELINED_REQ,
			g_param_spec_uint (SOUP_IO_DISPATCHER_MAX_PIPELINED_REQ,
					"Max pipelining requests",
					"Max pipelined requests in I/O queue",
					0, SOUP_MAX_PIPELINED_REQ_CONSTRAINT, SOUP_IO_DISPATCHER_MAX_PIPELINED_REQ_DEFAULT,
					G_PARAM_READWRITE));


	g_object_class_install_property(
			object_class, PROP_RESPONSE_BLOCK_SIZE,
			g_param_spec_uint (SOUP_IO_DISPATCHER_RESPONSE_BLOCK_SIZE,
					"Response block size",
					"Size of the reading block of IO dispatcher",
					1, SOUP_IO_DISPATCHER_RESPONSE_BLOCK_SIZE_CONSTRAINT,
					SOUP_IO_DISPATCHER_RESPONSE_BLOCK_SIZE_DEFAULT,
					G_PARAM_READWRITE));

	g_object_class_install_property(
			object_class, PROP_IDLE_TIMEOUT,
			g_param_spec_uint (SOUP_IO_DISPATCHER_IDLE_TIMEOUT,
					"Idle timeout",
					"Idle timeout duration",
					0, G_MAXUINT, SOUP_IO_DISPATCHER_IDLE_TIMEOUT_DEFAULT,
					G_PARAM_READWRITE));
}

void
soup_io_dispatcher_queue_message(SoupIODispatcher *io_disp, SoupMessage *msg)
{
	SoupIODispatcherPrivate *priv;
	g_return_if_fail (SOUP_IS_IO_DISPATCHER(io_disp));
	g_return_if_fail (SOUP_IS_MESSAGE (msg));

	priv = SOUP_IO_DISPATCHER_GET_PRIVATE (io_disp);
	priv->lock (io_disp, &priv->queue_mtx);

	g_atomic_int_inc(&priv->input_msg_queue_length);
	g_queue_push_tail (priv->input_msg_queue, msg);

	priv->unlock (io_disp, &priv->queue_mtx);
}

void
soup_io_dispatcher_process_message (SoupIODispatcher *io_disp,
		SoupMessage          			*msg,
		GCancellable                     *cancellable,
		SoupMessageCompletionFn        	 completion_cb,
		gpointer                       	 user_data)

{
	SoupIODispatcherPrivate *priv;
	SoupMessageIOData *io_data;
	g_return_if_fail (SOUP_IS_IO_DISPATCHER(io_disp));
	g_return_if_fail (SOUP_IS_MESSAGE (msg));
	g_return_if_fail (soup_io_dispatcher_get_socket (io_disp));

	priv = SOUP_IO_DISPATCHER_GET_PRIVATE (io_disp);

	priv->lock (io_disp, &priv->queue_mtx);

#if 0
	if (!(input_msg_removed = g_queue_remove (priv->input_msg_queue, msg)) &&
			soup_io_dispatcher_is_queue_full (io_disp)) {
		g_warning("SoupIODispatcher: Trying to send message through overfilled connection!");
		return;
	}
#endif

	io_data = io_data_new (io_disp, msg, cancellable, completion_cb, user_data);
	if (io_data) {
		SOUP_IO_DISPATCHER_GET_CLASS(io_disp)->io_data_new(io_disp, msg, io_data);
	}

	// We need to do it after pushing the msg to read_io_queue and write_io_queue
	if (g_queue_remove (priv->input_msg_queue, msg))
		g_atomic_int_add (&priv->input_msg_queue_length, -1);

	priv->unlock (io_disp, &priv->queue_mtx);

	SOUP_IO_DISPATCHER_GET_CLASS (io_disp)->process_message (io_disp, msg);
}

void
soup_io_dispatcher_cancel_message(SoupIODispatcher *io_disp, SoupMessage *msg)
{
	g_return_if_fail(SOUP_IS_IO_DISPATCHER(io_disp));
	g_return_if_fail(SOUP_IS_MESSAGE(msg));

	SOUP_IO_DISPATCHER_GET_CLASS(io_disp)->cancel_message (io_disp, msg);
}

void
soup_io_dispatcher_pause_message (SoupIODispatcher *io_disp, SoupMessage *msg)
{
	g_return_if_fail(SOUP_IS_IO_DISPATCHER(io_disp));
	g_return_if_fail(SOUP_IS_MESSAGE(msg));

	SOUP_IO_DISPATCHER_GET_CLASS(io_disp)->pause_message (io_disp, msg);
}

void
soup_io_dispatcher_unpause_message(SoupIODispatcher *io_disp, SoupMessage *msg)
{
	g_return_if_fail(SOUP_IS_IO_DISPATCHER(io_disp));
	g_return_if_fail(SOUP_IS_MESSAGE(msg));

	SOUP_IO_DISPATCHER_GET_CLASS(io_disp)->unpause_message (io_disp, msg);
}

gboolean
soup_io_dispatcher_is_msg_in_progress (SoupIODispatcher *io_disp,
		SoupMessage *msg)
{
	SoupIODispatcherPrivate *io_disp_priv;
	SoupMessagePrivate *msg_priv;
	SoupMessageIOData *io_data;
	gboolean result;

	g_return_val_if_fail (io_disp, FALSE);
	g_return_val_if_fail (msg, FALSE);

	io_disp_priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);
	msg_priv = SOUP_MESSAGE_GET_PRIVATE (msg);

	io_data = (SoupMessageIOData*)msg_priv->io_data;
	if (!io_data)
		return FALSE;

	io_disp_priv->lock (io_disp, io_data->io_data_mtx);
	result = msg_priv->io_disp == io_disp && io_data ? TRUE : FALSE;
//			(io_data->write_state >= SOUP_MESSAGE_IO_STATE_HEADERS ||
//					io_data->read_state >= SOUP_MESSAGE_IO_STATE_HEADERS) ? TRUE : FALSE;
	io_disp_priv->unlock (io_disp, io_data->io_data_mtx);
	return result;
}

gboolean
soup_io_dispatcher_is_queue_empty(SoupIODispatcher *io_disp)
{
	g_return_val_if_fail(SOUP_IS_IO_DISPATCHER(io_disp), TRUE);

	return soup_io_dispatcher_get_queue_length (io_disp) ?  FALSE : TRUE;
}

gboolean
soup_io_dispatcher_is_queue_full(SoupIODispatcher *io_disp)
{
	SoupIODispatcherPrivate *priv;
	gboolean result;
	g_return_val_if_fail(SOUP_IS_IO_DISPATCHER(io_disp), TRUE);
	priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);

	priv->lock (io_disp, &priv->io_disp_mtx);

	// First part of logical condition for server implementation
	result = priv->max_pipelined_requests && (
			soup_io_dispatcher_get_queue_length (io_disp) >= priv->max_pipelined_requests ||
			(SOUP_IO_DISPATCHER_GET_CLASS(io_disp)->is_queue_full &&
					SOUP_IO_DISPATCHER_GET_CLASS(io_disp)->is_queue_full(io_disp))) ? TRUE : FALSE;

	priv->unlock (io_disp, &priv->io_disp_mtx);

	return result;
}

void
soup_io_dispatcher_set_pipelining_support (SoupIODispatcher *io_disp,
		gboolean value)
{
	SoupIODispatcherPrivate *priv;
	g_return_if_fail (SOUP_IS_IO_DISPATCHER (io_disp));

	priv = SOUP_IO_DISPATCHER_GET_PRIVATE (io_disp);
	priv->lock (io_disp, &priv->io_disp_mtx);

	if ((!value && priv->is_pipelining_supported) ||
			(value && !priv->is_pipelining_supported)) {
		priv->is_pipelining_supported = value;
		if (!value)
			soup_io_dispatcher_set_max_pipelined_requests(io_disp, 1);

		g_object_notify (G_OBJECT (io_disp), SOUP_IO_DISPATCHER_IS_PIPELINING_SUPPORTED);
	}

	priv->unlock (io_disp, &priv->io_disp_mtx);
}

gboolean
soup_io_dispatcher_is_pipelining_supported (SoupIODispatcher *io_disp)
{
	SoupIODispatcherPrivate *priv;
	gboolean result;

	g_return_val_if_fail(SOUP_IS_IO_DISPATCHER(io_disp), FALSE);
	priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);
	priv->lock (io_disp, &priv->io_disp_mtx);

	result = priv->is_pipelining_supported;

	priv->unlock (io_disp, &priv->io_disp_mtx);

	return result;
}

gboolean
soup_io_dispatcher_is_via_proxy (SoupIODispatcher *io_disp)
{
	SoupIODispatcherPrivate *priv;
	gboolean result;

	g_return_val_if_fail (SOUP_IS_IO_DISPATCHER (io_disp), FALSE);
	priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);
	priv->lock (io_disp, &priv->io_disp_mtx);

	result = priv->is_via_proxy;

	priv->unlock (io_disp, &priv->io_disp_mtx);

	return result;
}

guint
soup_io_dispatcher_get_max_pipelined_requests(SoupIODispatcher *io_disp)
{
	SoupIODispatcherPrivate *priv;
	guint result;

	g_return_val_if_fail(SOUP_IS_IO_DISPATCHER(io_disp), 0);
	priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);
	priv->lock (io_disp, &priv->io_disp_mtx);

	result = priv->max_pipelined_requests;

	priv->unlock (io_disp, &priv->io_disp_mtx);

	return result;
}

void
soup_io_dispatcher_set_max_pipelined_requests(SoupIODispatcher *io_disp, guint value)
{
	SoupIODispatcherPrivate *priv;
	g_return_if_fail(SOUP_IS_IO_DISPATCHER(io_disp));

	priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);
	priv->lock (io_disp, &priv->queue_mtx);

	if (priv->is_pipelining_supported &&
			value <= SOUP_MAX_PIPELINED_REQ_CONSTRAINT &&
			value != priv->max_pipelined_requests) {
		priv->max_pipelined_requests = value;
		priv->unlock (io_disp, &priv->queue_mtx);

		g_object_notify(G_OBJECT(io_disp), SOUP_IO_DISPATCHER_MAX_PIPELINED_REQ);
		return;
	}

	priv->unlock (io_disp, &priv->queue_mtx);
}


guint
soup_io_dispatcher_get_response_block_size (SoupIODispatcher *io_disp)
{
	SoupIODispatcherPrivate *priv;
	guint response_block_size;
	g_return_val_if_fail (SOUP_IS_IO_DISPATCHER (io_disp), 0);

	priv = SOUP_IO_DISPATCHER_GET_PRIVATE (io_disp);

	priv->lock (io_disp, &priv->io_disp_mtx);
	response_block_size = priv->response_block_size;
	priv->unlock (io_disp, &priv->io_disp_mtx);

	return response_block_size;
}

void
soup_io_dispatcher_set_response_block_size (SoupIODispatcher *io_disp, guint value)
{
	SoupIODispatcherPrivate *priv;
	g_return_if_fail (SOUP_IS_IO_DISPATCHER (io_disp));

	priv = SOUP_IO_DISPATCHER_GET_PRIVATE (io_disp);

	if (value <= SOUP_IO_DISPATCHER_RESPONSE_BLOCK_SIZE_CONSTRAINT &&
			priv->response_block_size != value) {
		priv->lock (io_disp, &priv->io_disp_mtx);

		g_slice_free1 (priv->response_block_size, priv->read_buf);
		priv->response_block_size = value;
		priv->read_buf = g_slice_alloc (priv->response_block_size);

		priv->unlock (io_disp, &priv->io_disp_mtx);
		g_object_notify(G_OBJECT(io_disp), SOUP_IO_DISPATCHER_RESPONSE_BLOCK_SIZE);
	}
}

guint
soup_io_dispatcher_get_queue_length (SoupIODispatcher *io_disp)
{
	SoupIODispatcherPrivate *priv;
	g_return_val_if_fail(SOUP_IS_IO_DISPATCHER(io_disp), 0);

	priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);

	return g_atomic_int_get (&priv->input_msg_queue_length) +
			g_atomic_int_get (&priv->paused_io_queue_length) +
			MAX (g_atomic_int_get (&priv->read_io_queue_length), g_atomic_int_get (&priv->write_io_queue_length));
}

SoupSocket*
soup_io_dispatcher_get_socket (SoupIODispatcher *io_disp)
{
	SoupIODispatcherPrivate *priv;
	SoupSocket *socket;
	g_return_val_if_fail (SOUP_IS_IO_DISPATCHER (io_disp), NULL);

	priv = SOUP_IO_DISPATCHER_GET_PRIVATE (io_disp);
	priv->lock (io_disp, &priv->io_disp_mtx);

	socket = priv->socket;

	priv->unlock (io_disp, &priv->io_disp_mtx);

	return socket;
}

void
soup_io_dispatcher_set_socket (SoupIODispatcher *io_disp, SoupSocket *socket)
{
	SoupIODispatcherPrivate *priv;
	g_return_if_fail (SOUP_IS_IO_DISPATCHER (io_disp));

	priv = SOUP_IO_DISPATCHER_GET_PRIVATE (io_disp);

	priv->lock (io_disp, &priv->io_disp_mtx);
	g_object_ref (io_disp);

	//FIXME: Looks like dirty hack
	if (!socket || priv->socket != socket) {
		if (priv->socket) {
			g_signal_handlers_disconnect_by_func (priv->socket, readable_cb, io_disp);
			g_signal_handlers_disconnect_by_func (priv->socket, writable_cb, io_disp);
			stop_idle_timer(io_disp);

			reset (priv);
			g_object_unref (priv->socket);
		}

		priv->socket = socket ? g_object_ref (socket) : NULL;

		priv->lock (io_disp, &priv->queue_mtx);
		while (!g_queue_is_empty (priv->read_io_queue)) {
			io_data_cleanup (io_disp, (SoupMessageIOData*)g_queue_peek_head (priv->read_io_queue));
		}

		while (!g_queue_is_empty (priv->write_io_queue)) {
			io_data_cleanup (io_disp, (SoupMessageIOData*)g_queue_peek_head (priv->write_io_queue));
		}

		if (!socket)
			while (!g_queue_is_empty (priv->paused_io_queue)) {
				io_data_cleanup (io_disp, (SoupMessageIOData*)g_queue_peek_head (priv->paused_io_queue));
			}

		priv->unlock (io_disp, &priv->queue_mtx);

		if (priv->socket) {
			g_signal_connect (priv->socket, "readable", G_CALLBACK(readable_cb), io_disp);
			g_signal_connect (priv->socket, "writable", G_CALLBACK(writable_cb), io_disp);
			start_idle_timer(io_disp);
		}

		g_object_notify (G_OBJECT(io_disp), SOUP_IO_DISPATCHER_SOCKET);
	}

	g_object_unref (io_disp);
	priv->unlock (io_disp, &priv->io_disp_mtx);
}

static gboolean
process_queue (SoupIODispatcher *io_disp, SoupProcessIOQueueFn io_func,
		GQueue *queue, guint *queue_length,
		ItemState* (*get_item_state)(SoupMessageIOData*))
{
	SoupIODispatcherPrivate *priv;
	guint finished_req = 0;
	gboolean io_wait_continue = FALSE;

	g_return_val_if_fail(SOUP_IS_IO_DISPATCHER(io_disp), FALSE);

	priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);

	if (!soup_io_dispatcher_get_socket (io_disp))
		return FALSE;

	g_object_ref (io_disp);

	while (TRUE) {
		SoupMessage *msg;
		SoupMessageIOData *io_data;
		GRecMutex *io_data_mtx;
		ItemState *item_state;

		priv->lock (io_disp, &priv->queue_mtx);
		if (g_queue_is_empty (queue)) {
			priv->unlock (io_disp, &priv->queue_mtx);
			break;
		}

		io_data = (SoupMessageIOData*)g_queue_peek_head (queue);
		io_data_mtx = io_data->io_data_mtx;

		priv->lock (io_disp, io_data_mtx);

		item_state = get_item_state (io_data);

		if (io_data->cancelled ||
				io_data->paused ||
				item_state->blocked ||
				item_state->state == SOUP_MESSAGE_IO_STATE_BLOCKING) {
			priv->unlock (io_disp, io_data_mtx);
			break;
		}

		msg = io_data->msg;
		g_object_ref (msg);

		priv->lock (io_disp, &priv->io_disp_mtx);
		if (io_func(io_disp, io_data)) {
			finished_req++;
			g_atomic_int_add (queue_length, -1);
			delete_link (queue, &item_state->queue_link, io_disp);

			if (is_io_data_finished(io_data)) {
				gboolean keepalive = soup_message_is_keepalive (msg);
//				gboolean pipelined = ABS (g_queue_get_length (priv->read_io_queue) - g_queue_get_length (priv->write_io_queue)) > 1;
				priv->finished_requests++;
				io_data_cleanup (io_disp, io_data);
				if (!keepalive) {
					priv->unlock (io_disp, &priv->io_disp_mtx);
					priv->unlock (io_disp, io_data_mtx);

//					if (pipelined)
						soup_io_dispatcher_set_pipelining_support (io_disp, FALSE);

					priv->unlock (io_disp, &priv->queue_mtx);

					if (soup_io_dispatcher_get_socket (io_disp))
						soup_socket_disconnect (soup_io_dispatcher_get_socket (io_disp));

					g_object_unref (msg);
					break;
				}
			}
		} else {

			if (io_data->io_error)
				io_data_error (io_disp, io_data);
			else if (item_state->state == SOUP_MESSAGE_IO_STATE_BLOCKING)
				io_wait_continue = TRUE;

			priv->unlock (io_disp, &priv->io_disp_mtx);
			priv->unlock (io_disp, io_data_mtx);
			priv->unlock (io_disp, &priv->queue_mtx);
			g_object_unref (msg);

			break;
		}

		priv->unlock (io_disp, &priv->io_disp_mtx);
		priv->unlock (io_disp, io_data_mtx);
		priv->unlock (io_disp, &priv->queue_mtx);

		g_object_unref (msg);
	}

	g_object_unref (io_disp);

	return ((finished_req > 0) || io_wait_continue);
}

static inline ItemState*
get_input_item_state (SoupMessageIOData *io_data)
{
	return (ItemState*)(&io_data->read_queue);
}

static inline ItemState*
get_output_item_state (SoupMessageIOData *io_data)
{
	return (ItemState*)(&io_data->write_queue);
}

void
soup_io_dispatcher_process_input_queue(SoupIODispatcher *io_disp)
{
	SoupIODispatcherPrivate *priv = SOUP_IO_DISPATCHER_GET_PRIVATE (io_disp);

	g_object_ref (io_disp);
	while (process_queue (io_disp, SOUP_IO_DISPATCHER_GET_CLASS(io_disp)->io_data_read,
			priv->read_io_queue, &priv->read_io_queue_length, get_input_item_state) &&
			process_queue (io_disp, SOUP_IO_DISPATCHER_GET_CLASS(io_disp)->io_data_write,
					priv->write_io_queue, &priv->write_io_queue_length, get_output_item_state));
	g_object_unref (io_disp);
}

void
soup_io_dispatcher_process_output_queue(SoupIODispatcher *io_disp)
{
	SoupIODispatcherPrivate *priv = SOUP_IO_DISPATCHER_GET_PRIVATE (io_disp);

	g_object_ref (io_disp);
	while (process_queue (io_disp, SOUP_IO_DISPATCHER_GET_CLASS(io_disp)->io_data_write,
			priv->write_io_queue, &priv->write_io_queue_length, get_output_item_state) &&
			process_queue (io_disp, SOUP_IO_DISPATCHER_GET_CLASS(io_disp)->io_data_read,
					priv->read_io_queue, &priv->read_io_queue_length, get_input_item_state));
	g_object_unref (io_disp);
}


static void
cancel_message(SoupIODispatcher *io_disp, SoupMessage *msg)
{
	SoupIODispatcherPrivate *io_disp_priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);
	SoupMessagePrivate *msg_priv = SOUP_MESSAGE_GET_PRIVATE(msg);
	SoupMessageIOData *io_data = msg_priv->io_data;
	gboolean not_started;
	GRecMutex* io_data_mtx;

	g_return_if_fail(io_data);
	g_return_if_fail(io_data->read_queue || io_data->write_queue || io_data->paused_queue);

	g_object_ref(io_disp);

	io_disp_priv->lock (io_disp, io_data->io_data_mtx);
	io_data_mtx = io_data->io_data_mtx;

	if (io_data->cancelled) {
		// If cancel_message is called out of io_data_cleanup signal handler
		io_disp_priv->unlock (io_disp, io_data_mtx);
		return;
	}

	io_data->cancelled = TRUE;

	io_disp_priv->unlock (io_disp, io_data_mtx);

	io_data_cleanup (io_disp, io_data);

#if 0
	not_started = io_data->write_state == SOUP_MESSAGE_IO_STATE_NOT_STARTED  &&
			io_data->read_state == SOUP_MESSAGE_IO_STATE_NOT_STARTED ? TRUE : FALSE;
	io_disp_priv->unlock (io_disp, io_data_mtx);
	if (not_started)
		/* Remove 1 request from queue */
		io_data_cleanup(io_disp, io_data);
	else {
		/* Remove all latter requests from queue */
		SoupSocket *sock = soup_io_dispatcher_get_socket (io_disp);
		if (sock)
			soup_socket_disconnect(sock);
	}
#endif
	g_object_unref(io_disp);
}

static void
pause_message (SoupIODispatcher *io_disp, SoupMessage *msg)
{
	SoupMessageIOData *io_data = SOUP_MESSAGE_GET_PRIVATE(msg)->io_data;

	g_return_if_fail(io_data);

	soup_io_dispatcher_pause_io_data(io_disp, io_data);
}

typedef struct {
	SoupIODispatcher *io_disp;
	SoupMessageIOData *io_data;
} UnpauseData;

static void
unpause_message (SoupIODispatcher *io_disp, SoupMessage *msg)
{
	SoupIODispatcherPrivate *io_disp_priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);
	SoupMessageIOData *io_data = SOUP_MESSAGE_GET_PRIVATE(msg)->io_data;
	SoupSocket *socket = soup_io_dispatcher_get_socket (io_disp);
	GMainContext *async_context;
	UnpauseData *unpause_data;
	gboolean use_thread_context, non_blocking, paused;
	GRecMutex *io_data_mtx;

	g_return_if_fail (io_data != NULL);
#if 0
	g_return_if_fail(io_data->paused);
#endif

	if (!socket)
		return;

	io_disp_priv->lock (io_disp, &io_disp_priv->io_disp_mtx);
	io_disp_priv->lock (io_disp, io_data->io_data_mtx);
	io_data_mtx = io_data->io_data_mtx;
	paused = io_data->paused;

	if (!paused) {
		io_disp_priv->unlock (io_disp, io_data_mtx);
		return;
	}

	g_object_get (io_disp_priv->socket,
			SOUP_SOCKET_FLAG_NONBLOCKING, &non_blocking,
			SOUP_SOCKET_USE_THREAD_CONTEXT, &use_thread_context,
			NULL);

	if (use_thread_context)
		async_context = g_main_context_ref_thread_default ();
	else {
		g_object_get (io_disp_priv->socket,
				SOUP_SOCKET_ASYNC_CONTEXT, &async_context,
				NULL);
	}

	unpause_data = g_slice_new (UnpauseData);
	unpause_data->io_disp = io_disp;
	unpause_data->io_data = io_data;

	if (non_blocking) {
		if (!io_data->unpause_source) {
			io_data->unpause_source = soup_add_completion (
					async_context, (GSourceFunc)unpause_cb, unpause_data);
		}
	} else
		unpause_cb (unpause_data);
	if (async_context)
		g_main_context_unref (async_context);

	io_disp_priv->unlock (io_disp, io_data_mtx);
	io_disp_priv->unlock (io_disp, &io_disp_priv->io_disp_mtx);

}

static gboolean
unpause_cb (gpointer user_data)
{
	UnpauseData *unpause_data = (UnpauseData*)user_data;
	SoupIODispatcher *io_disp;
	SoupMessageIOData *io_data;
	SoupIODispatcherPrivate *priv;
	SoupMessageIOState write_state, read_state;
	GRecMutex *io_data_mtx;
	g_return_val_if_fail(unpause_data != NULL, FALSE);

	io_disp = unpause_data->io_disp;
	io_data = unpause_data->io_data;
	g_slice_free (UnpauseData, unpause_data);

	g_return_val_if_fail(io_disp && io_data, FALSE);

	priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);

	priv->lock(io_disp, io_data->io_data_mtx);
	io_data_mtx = io_data->io_data_mtx;

	write_state = io_data->write_state;
	read_state = io_data->read_state;

	io_data->unpause_source = NULL;
	io_data->paused = FALSE;

	if (read_state == SOUP_MESSAGE_IO_STATE_NOT_STARTED && write_state == SOUP_MESSAGE_IO_STATE_NOT_STARTED) {
		priv->lock (io_disp, &priv->queue_mtx);

		if (io_data->paused_queue) {
			g_atomic_int_add (&priv->paused_io_queue_length, -1);
			delete_link (priv->paused_io_queue, &io_data->paused_queue, io_disp);

			g_atomic_int_inc (&priv->read_io_queue_length);
			push_link (priv->read_io_queue, io_data, &io_data->read_queue, io_disp);
			g_atomic_int_inc (&priv->write_io_queue_length);
			push_link (priv->write_io_queue, io_data, &io_data->write_queue, io_disp);
		}

		priv->unlock (io_disp, &priv->queue_mtx);
	}

	priv->unlock (io_disp, io_data_mtx);

	if (SOUP_MESSAGE_IO_STATE_ACTIVE (write_state))
		soup_io_dispatcher_process_output_queue(io_disp);
	else if (SOUP_MESSAGE_IO_STATE_ACTIVE (read_state))
		soup_io_dispatcher_process_input_queue(io_disp);

	return FALSE;
}


static gpointer
io_data_new (SoupIODispatcher *io_disp, SoupMessage *msg, GCancellable *cancellable,
		SoupMessageCompletionFn completion_cb, gpointer user_data)
{
	SoupIODispatcherPrivate *io_disp_priv;
	SoupMessagePrivate *msg_priv;
	SoupMessageIOData *io_data;

	g_return_val_if_fail(SOUP_IS_MESSAGE(msg), NULL);

	io_disp_priv = SOUP_IO_DISPATCHER_GET_PRIVATE (io_disp);
	msg_priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	io_data = g_slice_new0 (SoupMessageIOData);
	if (!io_data)
		return NULL;

	io_data->msg = msg;
	io_data->cancellable = cancellable;
	io_data->completion_cb = completion_cb;
	io_data->completion_data = user_data;

	io_data->read_meta_buf    = g_byte_array_new ();
	io_data->write_buf        = g_string_new (NULL);

	io_data->read_state  = SOUP_MESSAGE_IO_STATE_NOT_STARTED;
	io_data->write_state = SOUP_MESSAGE_IO_STATE_NOT_STARTED;

	io_disp_priv->lock (io_disp, &io_disp_priv->queue_mtx);

	g_atomic_int_inc (&io_disp_priv->read_io_queue_length);
	push_link (io_disp_priv->read_io_queue, io_data, &io_data->read_queue, io_disp);

	g_atomic_int_inc (&io_disp_priv->write_io_queue_length);
	push_link (io_disp_priv->write_io_queue, io_data, &io_data->write_queue, io_disp);

	if (io_disp_priv->is_thread_safe) {
		if (g_queue_is_empty (io_disp_priv->io_data_mtx_pool)) {
			io_data->io_data_mtx = g_slice_alloc (sizeof(GRecMutex));
			g_rec_mutex_init (io_data->io_data_mtx);
		} else {
			io_data->io_data_mtx = (GRecMutex*)g_queue_pop_head (io_disp_priv->io_data_mtx_pool);
		}
	}

	io_disp_priv->unlock (io_disp, &io_disp_priv->queue_mtx);

	msg_priv->io_disp = io_disp;
	msg_priv->io_data = io_data;

	return io_data;
}

static void
io_data_error (SoupIODispatcher *io_disp, SoupMessageIOData *io_data)
{
	SoupIODispatcherPrivate *priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);
	SoupMessage *msg = io_data->msg;
	GError *error = io_data->error;

	if (error && error->domain == G_TLS_ERROR) {
		soup_message_set_status_full (msg,
				SOUP_STATUS_SSL_FAILED,
				error->message);
	} else if (io_data->read_state <= SOUP_MESSAGE_IO_STATE_HEADERS &&
			io_data->read_meta_buf->len == 0 &&
			priv->finished_requests &&
			!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) &&
			msg->method == SOUP_METHOD_GET) {
		/* Connection got closed, but we can safely try again */
		g_signal_emit(io_disp, signals[IO_MSG_RESTART], 0, msg);
	} else if (!SOUP_STATUS_IS_TRANSPORT_ERROR (msg->status_code))
		soup_message_set_status (msg, SOUP_STATUS_IO_ERROR);

	if (error)
		g_error_free (error);

	io_data_cleanup (io_disp, io_data);
}

static void
readable_cb (SoupSocket *socket, SoupIODispatcher *io_disp)
{
	SoupIODispatcherPrivate *priv;
	SoupMessageIOData *io_data;
	GRecMutex *io_data_mtx;

	g_return_if_fail(io_disp);
	g_return_if_fail(socket);

	if (!soup_socket_is_connected(socket))
		return;

	priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);

	priv->lock(io_disp, &priv->queue_mtx);

	if (g_queue_is_empty (priv->read_io_queue)) {
		priv->unlock(io_disp, &priv->queue_mtx);
		return;
	}

	io_data = (SoupMessageIOData*)g_queue_peek_head (priv->read_io_queue);

	priv->lock (io_disp, io_data->io_data_mtx);
	io_data_mtx = io_data->io_data_mtx;

	g_return_if_fail (io_data->read_state != SOUP_MESSAGE_IO_STATE_DONE);

	if (io_data->read_blocked) {
		io_data->read_blocked = FALSE;

		priv->unlock (io_disp, io_data->io_data_mtx);
		priv->unlock (io_disp, &priv->queue_mtx);
		soup_io_dispatcher_process_input_queue(io_disp);
	} else
		g_warning("SoupIODispatcher: unexpected signal:readable from SoupSocket");


	priv->unlock(io_disp, io_data_mtx);
	priv->unlock (io_disp, &priv->queue_mtx);
}

static void
writable_cb(SoupSocket *socket, SoupIODispatcher *io_disp)
{
	SoupIODispatcherPrivate *priv;
	SoupMessageIOData *io_data;
	GRecMutex *io_data_mtx;

	g_return_if_fail(io_disp);
	g_return_if_fail(socket);

	priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);

	priv->lock(io_disp, &priv->queue_mtx);

	if (g_queue_is_empty (priv->write_io_queue)) {
		g_message ("SoupIODispatcher: Socket is writable but output queue is empty");

		priv->unlock(io_disp, &priv->queue_mtx);
		return;
	}

	io_data = (SoupMessageIOData*)g_queue_peek_head (priv->write_io_queue);
	priv->lock (io_disp, io_data->io_data_mtx);
	io_data_mtx = io_data->io_data_mtx;

	g_return_if_fail(io_data->write_state != SOUP_MESSAGE_IO_STATE_DONE);

	if (io_data->write_blocked) {
		io_data->write_blocked = FALSE;

		priv->unlock (io_disp, io_data->io_data_mtx);
		priv->unlock (io_disp, &priv->queue_mtx);
		soup_io_dispatcher_process_output_queue(io_disp);
	} else
		g_warning("SoupIODispatcher: unexpected signal:writable from SoupSocket");

	priv->unlock(io_disp, io_data_mtx);
	priv->unlock (io_disp, &priv->queue_mtx);
}

static void
set_property (GObject *object, guint prop_id,
		const GValue *value, GParamSpec *pspec)
{
	SoupIODispatcher *io_disp = SOUP_IO_DISPATCHER(object);
	SoupIODispatcherPrivate *priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);
	gboolean is_thread_safe;

	g_object_freeze_notify (object);
	priv->lock(io_disp, &priv->io_disp_mtx);
	is_thread_safe = priv->is_thread_safe;

	switch (prop_id) {
	case PROP_SOCKET:
		soup_io_dispatcher_set_socket (io_disp, g_value_get_object (value));
		break;
	case PROP_ASYNC_CONTEXT:
		priv->async_context = g_value_get_pointer (value);
		if (priv->async_context)
			g_main_context_ref (priv->async_context);
		break;
	case PROP_HOST:
	{
		SoupURI *new_host;
		if (priv->host)
			soup_uri_free (priv->host);

		new_host = (SoupURI*)g_value_get_boxed (value);

		if (new_host)
			priv->host = soup_uri_copy_host (new_host);

		break;
	}

	case PROP_IS_PIPELINING_SUPPORTED:
		soup_io_dispatcher_set_pipelining_support (io_disp, g_value_get_boolean(value));
		break;

	case PROP_IS_VIA_PROXY:
	{
		gboolean new_value = g_value_get_boolean(value);
		if ((priv->is_via_proxy && new_value) ||
				(!priv->is_via_proxy && !new_value))
			break;

		priv->is_via_proxy = !priv->is_via_proxy;
		g_object_notify (object, SOUP_IO_DISPATCHER_IS_VIA_PROXY);
		break;
	}

	case PROP_IS_THREAD_SAFE:
	{
		gboolean new_value = g_value_get_boolean(value);
		if (!new_value)
			break;

		priv->is_thread_safe = new_value;
		if (priv->is_thread_safe) {
			g_rec_mutex_init (&priv->queue_mtx);
			g_rec_mutex_init (&priv->io_disp_mtx);

			priv->lock = lock;
			priv->unlock = unlock;
		}

		break;
	}

	case PROP_MAX_PIPELINED_REQ:
		soup_io_dispatcher_set_max_pipelined_requests (io_disp, g_value_get_uint(value));
		break;

	case PROP_RESPONSE_BLOCK_SIZE:
		soup_io_dispatcher_set_response_block_size (io_disp, g_value_get_uint (value));
		break;

	case PROP_IDLE_TIMEOUT:
		priv->idle_timeout = g_value_get_uint (value);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
	}

	if (is_thread_safe)
		priv->unlock (io_disp, &priv->io_disp_mtx);
	g_object_thaw_notify (object);
}

static void
get_property (GObject *object, guint prop_id,
		GValue *value, GParamSpec *pspec)
{
	SoupIODispatcher *io_disp = SOUP_IO_DISPATCHER(object);
	SoupIODispatcherPrivate *priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);

	priv->lock(io_disp, &priv->io_disp_mtx);

	switch (prop_id) {
	case PROP_SOCKET:
		g_value_set_object (value, priv->socket);
		break;
	case PROP_ASYNC_CONTEXT:
		g_value_set_pointer (value, priv->async_context);
		break;
	case PROP_HOST:
		g_value_set_boxed (value, priv->host);
		break;

	case PROP_IS_QUEUE_EMPTY:
		g_value_set_boolean (value, soup_io_dispatcher_is_queue_empty(io_disp));
		break;

	case PROP_IS_QUEUE_FULL:
		g_value_set_boolean (value, soup_io_dispatcher_is_queue_full(io_disp));
		break;

	case PROP_IS_PIPELINING_SUPPORTED:
		g_value_set_boolean (value, soup_io_dispatcher_is_pipelining_supported (io_disp));
		break;

	case PROP_IS_VIA_PROXY:
		g_value_set_boolean (value, priv->is_via_proxy);
		break;

	case PROP_IS_THREAD_SAFE:
		g_value_set_boolean (value, priv->is_thread_safe);
		break;

	case PROP_MAX_PIPELINED_REQ:
		g_value_set_uint(value, priv->max_pipelined_requests);
		break;

	case PROP_RESPONSE_BLOCK_SIZE:
		g_value_set_uint (value, soup_io_dispatcher_get_response_block_size(io_disp));
		break;

	case PROP_IDLE_TIMEOUT:
		g_value_set_uint (value, priv->idle_timeout);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
	}

	priv->unlock (io_disp, &priv->io_disp_mtx);
}

static void
io_data_cleanup(SoupIODispatcher *io_disp, SoupMessageIOData *io_data)
{
	SoupIODispatcherPrivate *io_disp_priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);
	SoupMessage *msg;
	SoupMessagePrivate *msg_priv;
	SoupMessageCompletionFn completion_cb;
	gpointer completion_data;
	gboolean done;
	GRecMutex *io_data_mtx;
	g_return_if_fail(io_data);

	io_disp_priv->lock (io_disp, &io_disp_priv->queue_mtx);
	io_disp_priv->lock (io_disp, io_data->io_data_mtx);
	io_data_mtx = io_data->io_data_mtx;

	msg = io_data->msg;
	msg_priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	completion_cb = io_data->completion_cb;
	completion_data = io_data->completion_data;

	if (io_data->read_queue) {
		g_atomic_int_add (&io_disp_priv->read_io_queue_length, -1);
		delete_link (io_disp_priv->read_io_queue, &io_data->read_queue, io_disp);
	}

	if (io_data->write_queue) {
		g_atomic_int_add (&io_disp_priv->write_io_queue_length, -1);
		delete_link (io_disp_priv->write_io_queue, &io_data->write_queue, io_disp);
	}

	if (io_data->paused_queue) {
		g_atomic_int_add (&io_disp_priv->paused_io_queue_length, -1);
		delete_link (io_disp_priv->paused_io_queue, &io_data->paused_queue, io_disp);
	}

	if (io_disp_priv->is_thread_safe) {
		g_queue_push_tail (io_disp_priv->io_data_mtx_pool, io_data_mtx);
	}

	io_disp_priv->unlock (io_disp, &io_disp_priv->queue_mtx);

	msg_priv->io_data = NULL;
	msg_priv->io_disp = NULL;

	if (io_data->unpause_source) {
		g_source_destroy (io_data->unpause_source);
		io_data->unpause_source = NULL;
	}

	g_byte_array_free (io_data->read_meta_buf, TRUE);

	g_string_free (io_data->write_buf, TRUE);
	if (io_data->write_chunk)
		soup_buffer_free (io_data->write_chunk);

	if (io_data->sniff_data)
		soup_message_body_free (io_data->sniff_data);

	done = io_data->read_state == SOUP_MESSAGE_IO_STATE_DONE && io_data->write_state == SOUP_MESSAGE_IO_STATE_DONE;

	if (!done && !io_data->io_error && !io_data->cancelled) {
		g_signal_emit(io_disp, signals[IO_MSG_RESTART], 0, msg);
	}


	if (io_disp_priv->is_thread_safe) {
		io_data->io_data_mtx = NULL;
	}

	io_disp_priv->unlock (io_disp, io_data_mtx);

	if ((io_data->read_state != SOUP_MESSAGE_IO_STATE_NOT_STARTED || io_data->write_state != SOUP_MESSAGE_IO_STATE_NOT_STARTED) &&
			(io_data->read_state < SOUP_MESSAGE_IO_STATE_FINISHING) &&
			soup_io_dispatcher_get_socket (io_disp))
		soup_socket_disconnect (io_disp_priv->socket);

	if (completion_cb)
		completion_cb(msg, completion_data);

	g_slice_free (SoupMessageIOData, io_data);
}


void
soup_io_dispatcher_pause_io_data (SoupIODispatcher *io_disp,
		gpointer io_data)
{
	SoupIODispatcherPrivate *priv = SOUP_IO_DISPATCHER_GET_PRIVATE (io_disp);
	SoupMessageIOData *io = (SoupMessageIOData*)io_data;
	priv->lock (io_disp, io->io_data_mtx);
	if (io->unpause_source) {
		g_source_destroy (io->unpause_source);
		io->unpause_source = NULL;
	}

	/* Optimization */
	if (io->read_state == SOUP_MESSAGE_IO_STATE_NOT_STARTED && io->write_state == SOUP_MESSAGE_IO_STATE_NOT_STARTED) {
		SoupIODispatcherPrivate *priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);

		priv->lock (io_disp, &priv->queue_mtx);

		if (!io->paused_queue) {
			g_atomic_int_add(&priv->read_io_queue_length, -1);
			delete_link (priv->read_io_queue, &io->read_queue, io_disp);

			g_atomic_int_add(&priv->write_io_queue_length, -1);
			delete_link (priv->write_io_queue, &io->write_queue, io_disp);

			g_atomic_int_inc(&priv->paused_io_queue_length);
			push_link (priv->paused_io_queue, io, &io->paused_queue, io_disp);
		}

		priv->unlock (io_disp, &priv->queue_mtx);
	}

	io->paused = TRUE;

	priv->unlock (io_disp, io->io_data_mtx);
}


/* Attempts to write @len bytes from @data. See the note at
 * read_metadata() for an explanation of the return value.
 */

gboolean
soup_io_dispatcher_write_data (SoupIODispatcher *io_disp, gpointer io_data,
		const char *data, guint len, gboolean body)
{
	SoupSocket *sock = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp)->socket;
	SoupSocketIOStatus status;
	gsize nwrote;
	GError *error = NULL;
	SoupBuffer *chunk;
	const char *start;
	SoupMessageIOData *io = (SoupMessageIOData*)io_data;
	SoupMessage *msg = io->msg;
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE(msg);

	while (len > io->written) {
		status = soup_socket_write (sock,
				data + io->written,
				len - io->written,
				&nwrote,
				io->cancellable, &error);
		switch (status) {
		case SOUP_SOCKET_EOF:
		case SOUP_SOCKET_ERROR:
			io->io_error = TRUE;
			if (error) {
				io->error = g_error_copy(error);
				g_error_free(error);
			}

			return FALSE;

		case SOUP_SOCKET_WOULD_BLOCK:
			io->write_blocked = TRUE;
			return FALSE;

		case SOUP_SOCKET_OK:
			start = data + io->written;
			io->written += nwrote;

			if (body) {
				if (io->write_length)
					io->write_length -= nwrote;

				chunk = soup_buffer_new (SOUP_MEMORY_TEMPORARY,
						start, nwrote);
				SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
				soup_message_wrote_body_data (msg, chunk);
				soup_buffer_free (chunk);
				SOUP_MESSAGE_IO_RETURN_VAL_IF_CANCELLED_OR_PAUSED (FALSE);
			}
			break;
		}
	}

	io->written = 0;
	return TRUE;
}

gboolean
soup_io_dispatcher_read_body_chunk (SoupIODispatcher *io_disp, gpointer io_data)
{
	SoupIODispatcherPrivate *io_disp_priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);
	SoupSocket *sock = io_disp_priv->socket;
	SoupMessageIOData *io = (SoupMessageIOData*)io_data;
	SoupMessage* msg = io->msg;
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE(msg);

	SoupSocketIOStatus status;
	gsize len;
	gboolean read_to_eof = (io->read_encoding == SOUP_ENCODING_EOF);
	gsize nread;
	GError *error = NULL;
	SoupBuffer *buffer;

	if (!io_handle_sniffing (io, FALSE))
		return FALSE;

	while (read_to_eof || io->read_length > 0) {
		if (priv->chunk_allocator) {
			buffer = priv->chunk_allocator (msg, io->read_length, priv->chunk_allocator_data);
			if (!buffer) {
				soup_io_dispatcher_pause_message(io_disp, msg);
				return FALSE;
			}
		} else {
			buffer = soup_buffer_new (SOUP_MEMORY_TEMPORARY,
					io_disp_priv->read_buf,
					io_disp_priv->response_block_size);
		}

		if (read_to_eof)
			len = buffer->length;
		else
			len = MIN (buffer->length, io->read_length);

		status = soup_socket_read (sock,
				(guchar *)buffer->data, len,
				&nread, io->cancellable, &error);

		if (status == SOUP_SOCKET_OK && nread) {
			buffer->length = nread;
			io->read_length -= nread;

			buffer = content_decode (msg, buffer);
			if (!buffer)
				continue;

			soup_message_body_got_chunk (io->read_body, buffer);

			if (io->need_content_sniffed) {
				soup_message_body_append_buffer (io->sniff_data, buffer);
				soup_buffer_free (buffer);
				io->need_got_chunk = TRUE;
				if (!io_handle_sniffing (io, FALSE))
					return FALSE;
				continue;
			}

			SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
			soup_message_got_chunk (msg, buffer);
			soup_buffer_free (buffer);
			SOUP_MESSAGE_IO_RETURN_VAL_IF_CANCELLED_OR_PAUSED (FALSE);
			continue;
		}

		soup_buffer_free (buffer);
		switch (status) {
		case SOUP_SOCKET_OK:
			break;

		case SOUP_SOCKET_EOF:
			if (io->read_eof_ok) {
				io->read_length = 0;
				return TRUE;
			}
			/* else fall through */

		case SOUP_SOCKET_ERROR:
			io->io_error = TRUE;
			if (error) {
				io->error = g_error_copy(error);
				g_error_free(error);
			}

			return FALSE;

		case SOUP_SOCKET_WOULD_BLOCK:
			io->read_blocked = TRUE;
			return FALSE;
		}
	}

	return TRUE;
}


gboolean
soup_io_dispatcher_read_metadata (SoupIODispatcher *io_disp, gpointer io_data, gboolean to_blank)
{
	SoupIODispatcherPrivate *priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);
	SoupSocket *sock = priv->socket;

	SoupSocketIOStatus status;
	gsize nread;
	gboolean got_lf;
	GError *error = NULL;
	SoupMessageIOData *io = (SoupMessageIOData*)io_data;

	while (1) {
		status = soup_socket_read_until (sock, priv->read_buf,
				priv->response_block_size,
				"\n", 1, &nread, &got_lf,
				io->cancellable, &error);
		switch (status) {
		case SOUP_SOCKET_OK:
			g_byte_array_append (io->read_meta_buf, priv->read_buf, nread);
			break;

		case SOUP_SOCKET_EOF:
			/* More lame server handling... deal with
			 * servers that don't send the final chunk.
			 */
			if (io->read_state == SOUP_MESSAGE_IO_STATE_CHUNK_SIZE &&
					io->read_meta_buf->len == 0) {
				g_byte_array_append (io->read_meta_buf,
						(guchar *)"0\r\n", 3);
				got_lf = TRUE;
				break;
			} else if (io->read_state == SOUP_MESSAGE_IO_STATE_TRAILERS &&
					io->read_meta_buf->len == 0) {
				g_byte_array_append (io->read_meta_buf,
						(guchar *)"\r\n", 2);
				got_lf = TRUE;
				break;
			}
			/* else fall through */

		case SOUP_SOCKET_ERROR:
			io->io_error = TRUE;
			if (error) {
				io->error = g_error_copy(error);
				g_error_free(error);
			}


			return FALSE;

		case SOUP_SOCKET_WOULD_BLOCK:
			io->read_blocked = TRUE;
			return FALSE;
		}

		if (got_lf) {
			if (!to_blank)
				break;
			if (nread == 1 && io->read_meta_buf->len >= 2 &&
					!strncmp ((char *)io->read_meta_buf->data +
							io->read_meta_buf->len - 2,
							"\n\n", 2))
				break;
			else if (nread == 2 && io->read_meta_buf->len >= 3 &&
					!strncmp ((char *)io->read_meta_buf->data +
							io->read_meta_buf->len - 3,
							"\n\r\n", 3))
				break;
		}
	}

	return TRUE;
}

static SoupBuffer *
content_decode_one (SoupBuffer *buf, GConverter *converter, GError **error)
{
	gsize outbuf_length, outbuf_used, outbuf_cur, input_used, input_cur;
	char *outbuf;
	GConverterResult result;
	gboolean dummy_zlib_header_used = FALSE;


	outbuf_length = MAX (buf->length * 2, 1024);
	outbuf = g_malloc (outbuf_length);
	outbuf_cur = input_cur = 0;

	do {
		result = g_converter_convert (
				converter,
				buf->data + input_cur, buf->length - input_cur,
				outbuf + outbuf_cur, outbuf_length - outbuf_cur,
				0, &input_used, &outbuf_used, error);
		input_cur += input_used;
		outbuf_cur += outbuf_used;

		if (g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_NO_SPACE) ||
				(!*error && outbuf_cur == outbuf_length)) {
			g_clear_error (error);
			outbuf_length *= 2;
			outbuf = g_realloc (outbuf, outbuf_length);
		} else if (input_cur == 0 &&
				!dummy_zlib_header_used &&
				G_IS_ZLIB_DECOMPRESSOR (converter) &&
				g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA)) {

			GZlibCompressorFormat format;
			g_object_get (G_OBJECT (converter), "format", &format, NULL);

			if (format == G_ZLIB_COMPRESSOR_FORMAT_ZLIB) {
				/* Some servers (especially Apache with mod_deflate)
				 * return RAW compressed data without the zlib headers
				 * when the client claims to support deflate. For
				 * those cases use a dummy header (stolen from
				 * Mozilla's nsHTTPCompressConv.cpp) and try to
				 * continue uncompressing data.
				 */
				static char dummy_zlib_header[2] = { 0x78, 0x9C };

				g_converter_reset (converter);
				result = g_converter_convert (converter,
						dummy_zlib_header, sizeof(dummy_zlib_header),
						outbuf + outbuf_cur, outbuf_length - outbuf_cur,
						0, &input_used, &outbuf_used, NULL);
				dummy_zlib_header_used = TRUE;
				if (result == G_CONVERTER_CONVERTED) {
					g_clear_error (error);
					continue;
				}
			}

			g_free (outbuf);
			return NULL;
		} else if (*error) {
			/* GZlibDecompressor can't ever return
			 * G_IO_ERROR_PARTIAL_INPUT unless we pass it
			 * input_length = 0, which we don't. Other
			 * converters might of course, so eventually
			 * this code needs to be rewritten to deal
			 * with that.
			 */
			g_free (outbuf);
			return NULL;
		}
	} while (input_cur < buf->length && result != G_CONVERTER_FINISHED);

	if (outbuf_cur)
		return soup_buffer_new (SOUP_MEMORY_TAKE, outbuf, outbuf_cur);
	else {
		g_free (outbuf);
		return NULL;
	}
}

static SoupBuffer *
content_decode (SoupMessage *msg, SoupBuffer *buf)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	GConverter *decoder;
	SoupBuffer *decoded;
	GError *error = NULL;
	GSList *d;

	for (d = priv->decoders; d; d = d->next) {
		decoder = d->data;

		decoded = content_decode_one (buf, decoder, &error);
		if (error) {
			if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED))
				g_warning ("Content-Decoding error: %s\n", error->message);
			g_error_free (error);

			soup_message_set_flags (msg, priv->msg_flags & ~SOUP_MESSAGE_CONTENT_DECODED);
			break;
		}
		if (buf)
			soup_buffer_free (buf);

		if (decoded)
			buf = decoded;
		else
			return NULL;
	}

	return buf;
}

#define XOR(A, B) (((A) && (!(B))) || (!(A) && (B)))

static void
properties_changed(SoupIODispatcher *io_disp)
{
	SoupIODispatcherPrivate *priv;
	gboolean idle, is_queue_full;
	g_return_if_fail(SOUP_IS_IO_DISPATCHER(io_disp));

	priv = SOUP_IO_DISPATCHER_GET_PRIVATE(io_disp);

	idle = soup_io_dispatcher_is_queue_empty(io_disp);
	priv->lock (io_disp, &priv->io_disp_mtx);
	if XOR(priv->idle, idle) {
		priv->idle = idle;

		//TODO: M.b. put it in some callback?
		if (idle)
			start_idle_timer (io_disp);
		else
			stop_idle_timer (io_disp);

		g_object_notify(G_OBJECT(io_disp), SOUP_IO_DISPATCHER_IS_QUEUE_EMPTY);
	}

	is_queue_full = soup_io_dispatcher_is_queue_full(io_disp);
	if XOR(priv->is_queue_full, soup_io_dispatcher_is_queue_full(io_disp)) {
		priv->is_queue_full = is_queue_full;
		g_object_notify(G_OBJECT(io_disp), SOUP_IO_DISPATCHER_IS_QUEUE_FULL);
	}
	priv->unlock (io_disp, &priv->io_disp_mtx);
}

static inline void
stub_lock(SoupIODispatcher *io_disp, GRecMutex *mtx)
{
}

static inline void
lock (SoupIODispatcher *io_disp, GRecMutex *mtx)
{
	if (G_UNLIKELY (!mtx))
		return;

	g_object_ref (io_disp);
	g_rec_mutex_lock (mtx);
}

static inline void
unlock (SoupIODispatcher *io_disp, GRecMutex *mtx)
{
	if (G_UNLIKELY (!mtx))
		return;

	g_rec_mutex_unlock (mtx);
	g_object_unref (io_disp);
}

static inline void
push_link (GQueue *queue, SoupMessageIOData *io_data, GList **link, SoupIODispatcher *io_disp)
{
	*link = g_list_append (NULL, io_data);
	g_queue_push_tail_link (queue, *link);

	properties_changed (io_disp);
}

static inline void
delete_link (GQueue *queue, GList **link, SoupIODispatcher *io_disp)
{
	g_queue_delete_link (queue, *link);
	*link = NULL;

	properties_changed (io_disp);
}

static void
reset (SoupIODispatcherPrivate *priv)
{
	if (priv->idle_timeout_src) {
		g_source_destroy (priv->idle_timeout_src);
		priv->idle_timeout_src = NULL;
	}

	priv->async_context = NULL;
	priv->max_pipelined_requests = SOUP_IO_DISPATCHER_MAX_PIPELINED_REQ_DEFAULT;

	if (priv->response_block_size != SOUP_IO_DISPATCHER_RESPONSE_BLOCK_SIZE_DEFAULT) {
		g_slice_free1 (priv->response_block_size, priv->read_buf);
		priv->response_block_size = SOUP_IO_DISPATCHER_RESPONSE_BLOCK_SIZE_DEFAULT;
		priv->read_buf = g_slice_alloc (priv->response_block_size);
	}

	priv->finished_requests = priv->read_io_queue_length = priv->write_io_queue_length =
			priv->paused_io_queue_length = priv->input_msg_queue_length = 0;

	priv->is_pipelining_supported = SOUP_IO_DISPATCHER_IS_PIPELINING_SUPPORTED_DEFAULT;
	priv->idle = TRUE;
	priv->is_queue_full = priv->disconnect_if_empty = FALSE;
	priv->is_via_proxy = SOUP_IO_DISPATCHER_IS_VIA_PROXY_DEFAULT;
}

static void
start_idle_timer (SoupIODispatcher *io_disp)
{
	SoupIODispatcherPrivate *priv = SOUP_IO_DISPATCHER_GET_PRIVATE (io_disp);

	if (priv->idle_timeout_src || !priv->idle_timeout)
		return;

	priv->idle_timeout_src = soup_add_timeout (priv->async_context,
			priv->idle_timeout * 1000, idle_timeout,io_disp);
}

static void
stop_idle_timer (SoupIODispatcher *io_disp)
{
	SoupIODispatcherPrivate *priv = SOUP_IO_DISPATCHER_GET_PRIVATE (io_disp);

	if (!priv->idle_timeout_src)
		return;

	g_source_destroy (priv->idle_timeout_src);
	priv->idle_timeout_src = NULL;
}

static gboolean
idle_timeout (gpointer io_disp)
{
	stop_idle_timer (io_disp);
	g_signal_emit (io_disp, signals[IDLE_TIMEOUT], 0);
	return FALSE;
}
