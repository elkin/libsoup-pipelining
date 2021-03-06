/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * soup-connection.c: A single HTTP/HTTPS connection
 *
 * Copyright (C) 2000-2003, Ximian, Inc.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <glib.h>

#include <fcntl.h>
#include <sys/types.h>

#include "soup-address.h"
#include "soup-connection.h"
#include "soup-marshal.h"
#include "soup-message.h"
#include "soup-message-private.h"
#include "soup-message-queue.h"
#include "soup-misc.h"
#include "soup-misc-private.h"
#include "soup-socket.h"
#include "soup-uri.h"
#include "soup-enum-types.h"

typedef struct {
	SoupSocket  *socket;
	SoupIODispatcher *io_disp;

	SoupAddress *remote_addr, *tunnel_addr;
	SoupURI     *proxy_uri;
	GTlsDatabase *tlsdb;
	gboolean     ssl, ssl_strict, ssl_fallback;

	GMainContext *async_context;
	gboolean      use_thread_context;

	SoupConnectionState state;
	guint        io_timeout;
	SoupSession *session;
} SoupConnectionPrivate;
#define SOUP_CONNECTION_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), SOUP_TYPE_CONNECTION, SoupConnectionPrivate))

G_DEFINE_TYPE (SoupConnection, soup_connection, G_TYPE_OBJECT)

enum {
	EVENT,
	CONNECTED,
	DISCONNECTED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

enum {
	PROP_0,

	PROP_IO_DISPATCHER,
	PROP_REMOTE_ADDRESS,
	PROP_TUNNEL_ADDRESS,
	PROP_PROXY_URI,
	PROP_SSL,
	PROP_SSL_CREDS,
	PROP_SSL_STRICT,
	PROP_SSL_FALLBACK,
	PROP_ASYNC_CONTEXT,
	PROP_USE_THREAD_CONTEXT,
	PROP_TIMEOUT,
	PROP_STATE,
	PROP_MESSAGE,

	LAST_PROP
};

static void set_property (GObject *object, guint prop_id,
			  const GValue *value, GParamSpec *pspec);
static void get_property (GObject *object, guint prop_id,
			  GValue *value, GParamSpec *pspec);
static void set_state (SoupConnection *conn, SoupConnectionState state);

static void
soup_connection_init (SoupConnection *conn)
{
	;
}

static void
finalize (GObject *object)
{
	SoupConnectionPrivate *priv = SOUP_CONNECTION_GET_PRIVATE (object);

	if (priv->io_disp)
		g_object_unref (priv->io_disp);
	if (priv->remote_addr)
		g_object_unref (priv->remote_addr);
	if (priv->tunnel_addr)
		g_object_unref (priv->tunnel_addr);
	if (priv->proxy_uri)
		soup_uri_free (priv->proxy_uri);
	if (priv->tlsdb)
		g_object_unref (priv->tlsdb);
	if (priv->async_context)
		g_main_context_unref (priv->async_context);

	G_OBJECT_CLASS (soup_connection_parent_class)->finalize (object);
}

static void
dispose (GObject *object)
{
	SoupConnection *conn = SOUP_CONNECTION (object);
	SoupConnectionPrivate *priv = SOUP_CONNECTION_GET_PRIVATE (conn);

	if (priv->socket) {
		g_warning ("Disposing connection while connected");
		soup_connection_disconnect (conn);
	}

	G_OBJECT_CLASS (soup_connection_parent_class)->dispose (object);
}

static void
soup_connection_class_init (SoupConnectionClass *connection_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (connection_class);

	g_type_class_add_private (connection_class, sizeof (SoupConnectionPrivate));

	/* virtual method override */
	object_class->dispose = dispose;
	object_class->finalize = finalize;
	object_class->set_property = set_property;
	object_class->get_property = get_property;

	/* signals */
	signals[EVENT] =
		g_signal_new ("event",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      0,
			      NULL, NULL,
			      NULL,
			      G_TYPE_NONE, 2,
			      G_TYPE_SOCKET_CLIENT_EVENT,
			      G_TYPE_IO_STREAM);

	signals[CONNECTED] =
			g_signal_new ("connected",
				      G_OBJECT_CLASS_TYPE (object_class),
				      G_SIGNAL_RUN_FIRST,
				      G_STRUCT_OFFSET (SoupConnectionClass, connected),
				      NULL, NULL,
				      _soup_marshal_NONE__OBJECT,
				      G_TYPE_NONE, 0);



	signals[DISCONNECTED] =
		g_signal_new ("disconnected",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (SoupConnectionClass, disconnected),
			      NULL, NULL,
			      _soup_marshal_NONE__NONE,
			      G_TYPE_NONE, 0);

	/* properties */
	g_object_class_install_property (
		object_class, PROP_IO_DISPATCHER,
		g_param_spec_object (SOUP_CONNECTION_IO_DISPATCHER,
				     "I/O Dispatcher",
				     "I/O Dispatcher for this connection",
				     G_TYPE_TLS_DATABASE,
				     G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class, PROP_REMOTE_ADDRESS,
		g_param_spec_object (SOUP_CONNECTION_REMOTE_ADDRESS,
				     "Remote address",
				     "The address of the HTTP or proxy server",
				     SOUP_TYPE_ADDRESS,
				     G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (
		object_class, PROP_TUNNEL_ADDRESS,
		g_param_spec_object (SOUP_CONNECTION_TUNNEL_ADDRESS,
				     "Tunnel address",
				     "The address of the HTTPS server this tunnel connects to",
				     SOUP_TYPE_ADDRESS,
				     G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property (
		object_class, PROP_PROXY_URI,
		g_param_spec_boxed (SOUP_CONNECTION_PROXY_URI,
				    "Proxy URI",
				    "URI of the HTTP proxy this connection connects to",
				    SOUP_TYPE_URI,
				    G_PARAM_READWRITE));
	g_object_class_install_property (
		object_class, PROP_SSL,
		g_param_spec_boolean (SOUP_CONNECTION_SSL,
				      "SSL",
				      "Whether this is an SSL connection",
				      FALSE,
				      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (
		object_class, PROP_SSL_CREDS,
		g_param_spec_object (SOUP_CONNECTION_SSL_CREDENTIALS,
				     "SSL credentials",
				     "SSL credentials for this connection",
				     G_TYPE_TLS_DATABASE,
				     G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (
		object_class, PROP_SSL_STRICT,
		g_param_spec_boolean (SOUP_CONNECTION_SSL_STRICT,
				      "Strictly validate SSL certificates",
				      "Whether certificate errors should be considered a connection error",
				      TRUE,
				      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (
		object_class, PROP_SSL_FALLBACK,
		g_param_spec_boolean (SOUP_CONNECTION_SSL_FALLBACK,
				      "SSLv3 fallback",
				      "Use SSLv3 instead of TLS",
				      FALSE,
				      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (
		object_class, PROP_ASYNC_CONTEXT,
		g_param_spec_pointer (SOUP_CONNECTION_ASYNC_CONTEXT,
				      "Async GMainContext",
				      "GMainContext to dispatch this connection's async I/O in",
				      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (
		object_class, PROP_USE_THREAD_CONTEXT,
		g_param_spec_boolean (SOUP_CONNECTION_USE_THREAD_CONTEXT,
				      "Use thread context",
				      "Use g_main_context_get_thread_default",
				      FALSE,
				      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (
		object_class, PROP_TIMEOUT,
		g_param_spec_uint (SOUP_CONNECTION_TIMEOUT,
				   "Timeout value",
				   "Value in seconds to timeout a blocking I/O",
				   0, G_MAXUINT, 0,
				   G_PARAM_READWRITE));
	g_object_class_install_property (
		object_class, PROP_STATE,
		g_param_spec_enum (SOUP_CONNECTION_STATE,
				   "Connection state",
				   "Current state of connection",
				   SOUP_TYPE_CONNECTION_STATE, SOUP_CONNECTION_NEW,
				   G_PARAM_READABLE));
}


SoupConnection *
soup_connection_new (const char *propname1, ...)
{
	SoupConnection *conn;
	va_list ap;

	va_start (ap, propname1);
	conn = (SoupConnection *)g_object_new_valist (SOUP_TYPE_CONNECTION,
						      propname1, ap);
	va_end (ap);

	return conn;
}

static void
set_property (GObject *object, guint prop_id,
	      const GValue *value, GParamSpec *pspec)
{
	SoupConnectionPrivate *priv = SOUP_CONNECTION_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_IO_DISPATCHER:
		if (priv->io_disp)
			g_object_unref (priv->io_disp);

		priv->io_disp = g_object_ref (g_value_get_object (value));
		break;
	case PROP_REMOTE_ADDRESS:
		priv->remote_addr = g_value_dup_object (value);
		break;
	case PROP_TUNNEL_ADDRESS:
		priv->tunnel_addr = g_value_dup_object (value);
		break;
	case PROP_PROXY_URI:
		if (priv->proxy_uri)
			soup_uri_free (priv->proxy_uri);
		priv->proxy_uri = g_value_dup_boxed (value);
		break;
	case PROP_SSL:
		priv->ssl = g_value_get_boolean (value);
		break;
	case PROP_SSL_CREDS:
		if (priv->tlsdb)
			g_object_unref (priv->tlsdb);
		priv->tlsdb = g_value_dup_object (value);
		break;
	case PROP_SSL_STRICT:
		priv->ssl_strict = g_value_get_boolean (value);
		break;
	case PROP_SSL_FALLBACK:
		priv->ssl_fallback = g_value_get_boolean (value);
		break;
	case PROP_ASYNC_CONTEXT:
		priv->async_context = g_value_get_pointer (value);
		if (priv->async_context)
			g_main_context_ref (priv->async_context);
		break;
	case PROP_USE_THREAD_CONTEXT:
		priv->use_thread_context = g_value_get_boolean (value);
		break;
	case PROP_TIMEOUT:
		priv->io_timeout = g_value_get_uint (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
get_property (GObject *object, guint prop_id,
	      GValue *value, GParamSpec *pspec)
{
	SoupConnectionPrivate *priv = SOUP_CONNECTION_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_IO_DISPATCHER:
		g_value_set_object (value, priv->io_disp);
		break;
	case PROP_REMOTE_ADDRESS:
		g_value_set_object (value, priv->remote_addr);
		break;
	case PROP_TUNNEL_ADDRESS:
		g_value_set_object (value, priv->tunnel_addr);
		break;
	case PROP_PROXY_URI:
		g_value_set_boxed (value, priv->proxy_uri);
		break;
	case PROP_SSL:
		g_value_set_boolean (value, priv->ssl);
		break;
	case PROP_SSL_CREDS:
		g_value_set_object (value, priv->tlsdb);
		break;
	case PROP_SSL_STRICT:
		g_value_set_boolean (value, priv->ssl_strict);
		break;
	case PROP_SSL_FALLBACK:
		g_value_set_boolean (value, priv->ssl_fallback);
		break;
	case PROP_ASYNC_CONTEXT:
		g_value_set_pointer (value, priv->async_context ? g_main_context_ref (priv->async_context) : NULL);
		break;
	case PROP_USE_THREAD_CONTEXT:
		g_value_set_boolean (value, priv->use_thread_context);
		break;
	case PROP_TIMEOUT:
		g_value_set_uint (value, priv->io_timeout);
		break;
	case PROP_STATE:
		g_value_set_enum (value, priv->state);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
set_state (SoupConnection *conn, SoupConnectionState state)
{
	SoupConnectionPrivate *priv;

	g_return_if_fail (SOUP_IS_CONNECTION (conn));
	g_return_if_fail (state >= SOUP_CONNECTION_NEW &&
			  state <= SOUP_CONNECTION_DISCONNECTED);

	priv = SOUP_CONNECTION_GET_PRIVATE (conn);
	priv->state = state;

	g_object_notify (G_OBJECT (conn), "state");
}


static void
soup_connection_event (SoupConnection      *conn,
		       GSocketClientEvent   event,
		       GIOStream           *connection)
{
	SoupConnectionPrivate *priv = SOUP_CONNECTION_GET_PRIVATE (conn);

	if (!connection && priv->socket)
		connection = soup_socket_get_iostream (priv->socket);

	g_signal_emit (conn, signals[EVENT], 0,
		       event, connection);
}

static void
proxy_socket_event (SoupSocket          *socket,
		    GSocketClientEvent   event,
		    GIOStream           *connection,
		    gpointer             user_data)
{
	SoupConnection *conn = user_data;

	/* We handle COMPLETE ourselves */
	if (event != G_SOCKET_CLIENT_COMPLETE)
		soup_connection_event (conn, event, connection);
}

static void
socket_disconnected (SoupSocket *sock, gpointer conn)
{
	soup_connection_disconnect (conn);
}

typedef struct {
	SoupConnection *conn;
	SoupConnectionCallback callback;
	gpointer callback_data;
	GCancellable *cancellable;
	guint event_id;
	gboolean tls_handshake;
} SoupConnectionAsyncConnectData;

static void
socket_connect_finished (SoupSocket *socket, guint status, gpointer user_data)
{
	SoupConnectionAsyncConnectData *data = user_data;
	SoupConnectionPrivate *priv = SOUP_CONNECTION_GET_PRIVATE (data->conn);

	g_signal_handler_disconnect (socket, data->event_id);

	if (SOUP_STATUS_IS_SUCCESSFUL (status)) {
		g_signal_connect (priv->socket, "disconnected",
				  G_CALLBACK (socket_disconnected), data->conn);

		if (data->tls_handshake) {
			soup_connection_event (data->conn,
					       G_SOCKET_CLIENT_TLS_HANDSHAKED,
					       NULL);
		}
		if (!priv->ssl || !priv->tunnel_addr) {
			soup_connection_event (data->conn,
					       G_SOCKET_CLIENT_COMPLETE,
					       NULL);
		}

		set_state (data->conn, SOUP_CONNECTION_CONNECTED);

		g_signal_emit (data->conn, signals[CONNECTED], 0, socket);

	} else if (status == SOUP_STATUS_TLS_FAILED) {
		priv->ssl_fallback = TRUE;
		status = SOUP_STATUS_TRY_AGAIN;
	}

	if (data->callback) {
		if (priv->proxy_uri != NULL)
			status = soup_status_proxify (status);
		data->callback (data->conn, status, data->callback_data);
	}
	g_object_unref (data->conn);
	if (data->cancellable)
		g_object_unref (data->cancellable);
	g_slice_free (SoupConnectionAsyncConnectData, data);
}

static void
socket_connect_result (SoupSocket *sock, guint status, gpointer user_data)
{
	SoupConnectionAsyncConnectData *data = user_data;

	if (SOUP_STATUS_IS_SUCCESSFUL (status) &&
	    data->tls_handshake) {
		if (soup_socket_start_ssl (sock, data->cancellable)) {
			soup_connection_event (data->conn,
					       G_SOCKET_CLIENT_TLS_HANDSHAKING,
					       NULL);
			soup_socket_handshake_async (sock, data->cancellable,
						     socket_connect_finished, data);
			return;
		}

		status = SOUP_STATUS_SSL_FAILED;
	}

	socket_connect_finished (sock, status, data);
}

void
soup_connection_connect_async (SoupConnection *conn,
			       GCancellable *cancellable,
			       SoupConnectionCallback callback,
			       gpointer user_data)
{
	SoupConnectionAsyncConnectData *data;
	SoupConnectionPrivate *priv;

	g_return_if_fail (SOUP_IS_CONNECTION (conn));
	priv = SOUP_CONNECTION_GET_PRIVATE (conn);
	g_return_if_fail (priv->socket == NULL);

	set_state (conn, SOUP_CONNECTION_CONNECTING);

	data = g_slice_new (SoupConnectionAsyncConnectData);
	data->conn = g_object_ref (conn);
	data->callback = callback;
	data->callback_data = user_data;
	data->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
	data->tls_handshake = (priv->ssl && !priv->tunnel_addr);

	priv->socket =
		soup_socket_new (SOUP_SOCKET_REMOTE_ADDRESS, priv->remote_addr,
				 SOUP_SOCKET_SSL_CREDENTIALS, priv->tlsdb,
				 SOUP_SOCKET_SSL_STRICT, priv->ssl_strict,
				 SOUP_SOCKET_SSL_FALLBACK, priv->ssl_fallback,
				 SOUP_SOCKET_ASYNC_CONTEXT, priv->async_context,
				 SOUP_SOCKET_USE_THREAD_CONTEXT, priv->use_thread_context,
				 SOUP_SOCKET_TIMEOUT, priv->io_timeout,
				 "clean-dispose", TRUE,
				 NULL);
	data->event_id = g_signal_connect (priv->socket, "event",
					   G_CALLBACK (proxy_socket_event),
					   conn);
	soup_socket_connect_async (priv->socket, cancellable,
				   socket_connect_result, data);
}

guint
soup_connection_connect_sync (SoupConnection *conn, GCancellable *cancellable)
{
	SoupConnectionPrivate *priv;
	guint status, event_id;

	g_return_val_if_fail (SOUP_IS_CONNECTION (conn), SOUP_STATUS_MALFORMED);
	priv = SOUP_CONNECTION_GET_PRIVATE (conn);
	g_return_val_if_fail (priv->socket == NULL, SOUP_STATUS_MALFORMED);

	set_state (conn, SOUP_CONNECTION_CONNECTING);

	priv->socket =
		soup_socket_new (SOUP_SOCKET_REMOTE_ADDRESS, priv->remote_addr,
				 SOUP_SOCKET_SSL_CREDENTIALS, priv->tlsdb,
				 SOUP_SOCKET_SSL_STRICT, priv->ssl_strict,
				 SOUP_SOCKET_SSL_FALLBACK, priv->ssl_fallback,
				 SOUP_SOCKET_FLAG_NONBLOCKING, FALSE,
				 SOUP_SOCKET_TIMEOUT, priv->io_timeout,
				 "clean-dispose", TRUE,
				 NULL);

	event_id = g_signal_connect (priv->socket, "event",
				     G_CALLBACK (proxy_socket_event), conn);
	status = soup_socket_connect_sync (priv->socket, cancellable);

	if (!SOUP_STATUS_IS_SUCCESSFUL (status))
		goto fail;
		
	if (priv->ssl && !priv->tunnel_addr) {
		if (!soup_socket_start_ssl (priv->socket, cancellable))
			status = SOUP_STATUS_SSL_FAILED;
		else {
			soup_connection_event (conn,
					       G_SOCKET_CLIENT_TLS_HANDSHAKING,
					       NULL);
			status = soup_socket_handshake_sync (priv->socket, cancellable);
			if (status == SOUP_STATUS_OK) {
				soup_connection_event (conn,
						       G_SOCKET_CLIENT_TLS_HANDSHAKED,
						       NULL);
			} else if (status == SOUP_STATUS_TLS_FAILED) {
				priv->ssl_fallback = TRUE;
				status = SOUP_STATUS_TRY_AGAIN;
			}
		}
	}

	if (SOUP_STATUS_IS_SUCCESSFUL (status)) {
		g_signal_connect (priv->socket, "disconnected",
				  G_CALLBACK (socket_disconnected), conn);

		if (!priv->ssl || !priv->tunnel_addr) {
			soup_connection_event (conn,
					       G_SOCKET_CLIENT_COMPLETE,
					       NULL);
		}

		g_signal_emit (conn, signals[CONNECTED], 0, priv->socket);

	} else {
	fail:
		if (priv->socket) {
			soup_socket_disconnect (priv->socket);
			g_object_unref (priv->socket);
			priv->socket = NULL;
		}
	}

	if (priv->socket)
		g_signal_handler_disconnect (priv->socket, event_id);

	if (priv->proxy_uri != NULL)
		status = soup_status_proxify (status);
	return status;
}

SoupAddress *
soup_connection_get_tunnel_addr (SoupConnection *conn)
{
	SoupConnectionPrivate *priv;

	g_return_val_if_fail (SOUP_IS_CONNECTION (conn), NULL);
	priv = SOUP_CONNECTION_GET_PRIVATE (conn);

	return priv->tunnel_addr;
}

guint
soup_connection_start_ssl_sync (SoupConnection *conn,
				GCancellable   *cancellable)
{
	SoupConnectionPrivate *priv;
	const char *server_name;
	guint status;

	g_return_val_if_fail (SOUP_IS_CONNECTION (conn), FALSE);
	priv = SOUP_CONNECTION_GET_PRIVATE (conn);

	server_name = soup_address_get_name (priv->tunnel_addr ?
					     priv->tunnel_addr :
					     priv->remote_addr);
	if (!soup_socket_start_proxy_ssl (priv->socket, server_name,
					  cancellable))
		return SOUP_STATUS_SSL_FAILED;

	soup_connection_event (conn, G_SOCKET_CLIENT_TLS_HANDSHAKING, NULL);
	status = soup_socket_handshake_sync (priv->socket, cancellable);
	if (status == SOUP_STATUS_OK)
		soup_connection_event (conn, G_SOCKET_CLIENT_TLS_HANDSHAKED, NULL);
	else if (status == SOUP_STATUS_TLS_FAILED) {
		priv->ssl_fallback = TRUE;
		status = SOUP_STATUS_TRY_AGAIN;
	}

	return status;
}

static void
start_ssl_completed (SoupSocket *socket, guint status, gpointer user_data)
{
	SoupConnectionAsyncConnectData *data = user_data;
	SoupConnectionPrivate *priv = SOUP_CONNECTION_GET_PRIVATE (data->conn);

	if (status == SOUP_STATUS_OK)
		soup_connection_event (data->conn, G_SOCKET_CLIENT_TLS_HANDSHAKED, NULL);
	else if (status == SOUP_STATUS_TLS_FAILED) {
		priv->ssl_fallback = TRUE;
		status = SOUP_STATUS_TRY_AGAIN;
	}

	data->callback (data->conn, status, data->callback_data);
	g_object_unref (data->conn);
	g_slice_free (SoupConnectionAsyncConnectData, data);
}

static gboolean
idle_start_ssl_completed (gpointer user_data)
{
	SoupConnectionAsyncConnectData *data = user_data;

	start_ssl_completed (NULL, SOUP_STATUS_SSL_FAILED, data);
	return FALSE;
}

void
soup_connection_start_ssl_async (SoupConnection   *conn,
				 GCancellable     *cancellable,
				 SoupConnectionCallback callback,
				 gpointer          user_data)
{
	SoupConnectionPrivate *priv;
	const char *server_name;
	SoupConnectionAsyncConnectData *data;
	GMainContext *async_context;

	g_return_if_fail (SOUP_IS_CONNECTION (conn));
	priv = SOUP_CONNECTION_GET_PRIVATE (conn);

	data = g_slice_new (SoupConnectionAsyncConnectData);
	data->conn = g_object_ref (conn);
	data->callback = callback;
	data->callback_data = user_data;

	if (priv->use_thread_context)
		async_context = g_main_context_get_thread_default ();
	else
		async_context = priv->async_context;

	server_name = soup_address_get_name (priv->tunnel_addr ?
					     priv->tunnel_addr :
					     priv->remote_addr);
	if (!soup_socket_start_proxy_ssl (priv->socket, server_name,
					  cancellable)) {
		soup_add_completion (async_context,
				     idle_start_ssl_completed, data);
		return;
	}

	soup_connection_event (conn, G_SOCKET_CLIENT_TLS_HANDSHAKING, NULL);
	soup_socket_handshake_async (priv->socket, cancellable,
				     start_ssl_completed, data);
}

/**
 * soup_connection_disconnect:
 * @conn: a connection
 *
 * Disconnects @conn's socket and emits a %disconnected signal.
 * After calling this, @conn will be essentially useless.
 **/
void
soup_connection_disconnect (SoupConnection *conn)
{
	SoupConnectionPrivate *priv;
	SoupConnectionState old_state;

	g_return_if_fail (SOUP_IS_CONNECTION (conn));
	priv = SOUP_CONNECTION_GET_PRIVATE (conn);

	old_state = priv->state;
	if (old_state != SOUP_CONNECTION_DISCONNECTED)
		set_state (conn, SOUP_CONNECTION_DISCONNECTED);

	if (priv->socket) {
		g_signal_handlers_disconnect_by_func (priv->socket,
						      socket_disconnected, conn);
		soup_socket_disconnect (priv->socket);
		g_object_unref (priv->socket);
		priv->socket = NULL;
	}

	if (old_state != SOUP_CONNECTION_DISCONNECTED)
		g_signal_emit (conn, signals[DISCONNECTED], 0);
}

SoupSocket *
soup_connection_get_socket (SoupConnection *conn)
{
	g_return_val_if_fail (SOUP_IS_CONNECTION (conn), NULL);

	return SOUP_CONNECTION_GET_PRIVATE (conn)->socket;
}

void
soup_connection_set_io_dispatcher (SoupConnection *conn, SoupIODispatcher *io_disp)
{
	SoupConnectionPrivate *priv;
	g_return_if_fail (SOUP_IS_CONNECTION (conn));
	g_return_if_fail (SOUP_IS_IO_DISPATCHER (io_disp));

	priv = SOUP_CONNECTION_GET_PRIVATE (conn);
	if (priv->io_disp)
		g_object_unref (io_disp);

	priv->io_disp = g_object_ref (io_disp);

	g_object_notify (G_OBJECT (conn), SOUP_CONNECTION_IO_DISPATCHER);
}


SoupIODispatcher*
soup_connection_get_io_dispatcher (SoupConnection *conn)
{
	g_return_val_if_fail (SOUP_IS_CONNECTION (conn), NULL);

	return SOUP_CONNECTION_GET_PRIVATE (conn)->io_disp;
}

SoupURI *
soup_connection_get_proxy_uri (SoupConnection *conn)
{
	g_return_val_if_fail (SOUP_IS_CONNECTION (conn), NULL);

	return SOUP_CONNECTION_GET_PRIVATE (conn)->proxy_uri;
}

gboolean
soup_connection_is_via_proxy (SoupConnection *conn)
{
	g_return_val_if_fail (SOUP_IS_CONNECTION (conn), FALSE);

	return SOUP_CONNECTION_GET_PRIVATE (conn)->proxy_uri != NULL;
}

SoupConnectionState
soup_connection_get_state (SoupConnection *conn)
{
	SoupConnectionPrivate *priv;

	g_return_val_if_fail (SOUP_IS_CONNECTION (conn),
			      SOUP_CONNECTION_DISCONNECTED);
	priv = SOUP_CONNECTION_GET_PRIVATE (conn);

	if (priv->state != SOUP_CONNECTION_DISCONNECTED && soup_io_dispatcher_is_queue_empty (priv->io_disp) &&
	    g_socket_condition_check (soup_socket_get_gsocket (priv->socket), G_IO_IN))
		set_state (conn, SOUP_CONNECTION_REMOTE_DISCONNECTED);

	return priv->state;
}

gboolean
soup_connection_get_ssl_fallback (SoupConnection *conn)
{
	return SOUP_CONNECTION_GET_PRIVATE (conn)->ssl_fallback;
}

