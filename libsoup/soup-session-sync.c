/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * soup-session-sync.c
 *
 * Copyright (C) 2000-2003, Ximian, Inc.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define LIBSOUP_I_HAVE_READ_BUG_594377_AND_KNOW_SOUP_PASSWORD_MANAGER_MIGHT_GO_AWAY

#include "soup-address.h"
#include "soup-session-sync.h"
#include "soup-session-private.h"
#include "soup-address.h"
#include "soup-io-dispatcher.h"
#include "soup-io-dispatcher-pool.h"
#include "soup-message-private.h"
#include "soup-message-queue.h"
#include "soup-misc.h"
#include "soup-misc-private.h"
#include "soup-password-manager.h"
#include "soup-proxy-uri-resolver.h"
#include "soup-uri.h"

/**
 * SECTION:soup-session-sync
 * @short_description: Soup session for blocking I/O in multithreaded
 * programs.
 *
 * #SoupSessionSync is an implementation of #SoupSession that uses
 * synchronous I/O, intended for use in multi-threaded programs.
 *
 * You can use #SoupSessionSync from multiple threads concurrently.
 * Eg, you can send a #SoupMessage in one thread, and then while
 * waiting for the response, send another #SoupMessage from another
 * thread. You can also send a message from one thread and then call
 * soup_session_cancel_message() on it from any other thread (although
 * you need to be careful to avoid race conditions, where the message
 * finishes and is then unreffed by the sending thread just before you
 * cancel it).
 *
 * However, the majority of other types and methods in libsoup are not
 * MT-safe. In particular, you <emphasis>cannot</emphasis> modify or
 * examine a #SoupMessage while it is being transmitted by
 * #SoupSessionSync in another thread. Once a message has been handed
 * off to #SoupSessionSync, it can only be manipulated from its signal
 * handler callbacks, until I/O is complete.
 **/

typedef struct {
	SoupIODispatcherPool *io_disp_pool;
	GMutex lock;
	GCond cond;
} SoupSessionSyncPrivate;
#define SOUP_SESSION_SYNC_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), SOUP_TYPE_SESSION_SYNC, SoupSessionSyncPrivate))

static void  queue_message  (SoupSession *session, SoupMessage *msg,
			     SoupSessionCallback callback, gpointer user_data);
static guint send_message   (SoupSession *session, SoupMessage *msg);
static void  cancel_message (SoupSession *session, SoupMessage *msg,
			     guint status_code);
static void  auth_required  (SoupSession *session, SoupMessage *msg,
			     SoupAuth *auth, gboolean retrying);
static void  flush_queue    (SoupSession *session);
static void  kick           (SoupSession *session);
SoupIODispatcherPool *get_io_dispatcher_pool (SoupSession *session);

G_DEFINE_TYPE (SoupSessionSync, soup_session_sync, SOUP_TYPE_SESSION)

static void
soup_session_sync_init (SoupSessionSync *ss)
{
	SoupSessionSyncPrivate *priv = SOUP_SESSION_SYNC_GET_PRIVATE (ss);

	priv->io_disp_pool = NULL;
	g_mutex_init (&priv->lock);
	g_cond_init (&priv->cond);
}

static void
finalize (GObject *object)
{
	SoupSessionSyncPrivate *priv = SOUP_SESSION_SYNC_GET_PRIVATE (object);

	g_object_unref (priv->io_disp_pool);
	g_mutex_clear (&priv->lock);
	g_cond_clear (&priv->cond);

	G_OBJECT_CLASS (soup_session_sync_parent_class)->finalize (object);
}

static void
soup_session_sync_class_init (SoupSessionSyncClass *session_sync_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (session_sync_class);
	SoupSessionClass *session_class = SOUP_SESSION_CLASS (session_sync_class);

	g_type_class_add_private (session_sync_class, sizeof (SoupSessionSyncPrivate));

	/* virtual method override */
	session_class->queue_message = queue_message;
	session_class->send_message = send_message;
	session_class->cancel_message = cancel_message;
	session_class->auth_required = auth_required;
	session_class->flush_queue = flush_queue;
	session_class->kick = kick;
	session_class->get_io_dispatcher_pool = get_io_dispatcher_pool;

	object_class->finalize = finalize;
}


/**
 * soup_session_sync_new:
 *
 * Creates an synchronous #SoupSession with the default options.
 *
 * Return value: the new session.
 **/
SoupSession *
soup_session_sync_new (void)
{
	return g_object_new (SOUP_TYPE_SESSION_SYNC, NULL);
}

/**
 * soup_session_sync_new_with_options:
 * @optname1: name of first property to set
 * @...: value of @optname1, followed by additional property/value pairs
 *
 * Creates an synchronous #SoupSession with the specified options.
 *
 * Return value: the new session.
 **/
SoupSession *
soup_session_sync_new_with_options (const char *optname1, ...)
{
	SoupSession *session;
	va_list ap;

	va_start (ap, optname1);
	session = (SoupSession *)g_object_new_valist (SOUP_TYPE_SESSION_SYNC,
						      optname1, ap);
	va_end (ap);

	return session;
}

static guint
tunnel_connect (SoupSession *session, SoupMessageQueueItem *related)
{
	SoupConnection *conn = related->conn;
	SoupMessageQueueItem *item;
	SoupSocket *socket = soup_connection_get_socket (conn);
	guint status;

	g_object_ref (conn);

	item = soup_session_make_connect_message (session, conn);
	g_signal_emit_by_name (conn, "event", 0,
		       G_SOCKET_CLIENT_PROXY_NEGOTIATING,
		       soup_socket_get_iostream (socket));
	do {
		soup_session_send_queue_item (session, item, NULL);
		status = item->msg->status_code;
		if (item->state == SOUP_MESSAGE_RESTARTING &&
				item->io_disp &&
				soup_io_dispatcher_is_msg_in_progress(item->io_disp,  item->msg)) {
			soup_message_restarted (item->msg);
			item->state = SOUP_MESSAGE_RUNNING;
		} else {
			if (item->state == SOUP_MESSAGE_RESTARTING)
				status = SOUP_STATUS_TRY_AGAIN;
			item->state = SOUP_MESSAGE_FINISHED;
			soup_message_finished (item->msg);
		}
	} while (item->state == SOUP_MESSAGE_STARTING);
	soup_session_unqueue_item (session, item);
	soup_message_queue_item_unref (item);

	if (SOUP_STATUS_IS_SUCCESSFUL (status)) {
		g_signal_emit_by_name (conn, "event", 0,
			       G_SOCKET_CLIENT_PROXY_NEGOTIATING,
			       soup_socket_get_iostream (socket));
		if (!soup_connection_start_ssl_sync (conn, related->cancellable))
			status = SOUP_STATUS_SSL_FAILED;
		soup_message_set_https_status (related->msg, conn);
	}

	if (!SOUP_STATUS_IS_SUCCESSFUL (status))
		soup_connection_disconnect (conn);

	g_object_unref (conn);
	return status;
}

static void
get_connection (SoupMessageQueueItem *item)
{
	SoupSession *session = item->session;
	SoupMessage *msg = item->msg;
	gboolean try_pruning = FALSE;
	guint status;

try_again:
	soup_session_cleanup_connections (session, FALSE);

	if (!soup_session_get_connection (session, item, &try_pruning)) {
		if (!try_pruning)
			return;
		soup_session_cleanup_connections (session, TRUE);
		if (!soup_session_get_connection (session, item, &try_pruning))
			return;
		try_pruning = FALSE;
	}

	if (soup_connection_get_state (item->conn) != SOUP_CONNECTION_NEW) {
		item->state = SOUP_MESSAGE_READY;
		return;
	}

	status = soup_connection_connect_sync (item->conn, item->cancellable);
	if (status == SOUP_STATUS_TRY_AGAIN) {
		soup_connection_disconnect (item->conn);
		soup_message_queue_item_set_connection (item, NULL);
		goto try_again;
	}

	soup_message_set_https_status (msg, item->conn);

	if (!SOUP_STATUS_IS_SUCCESSFUL (status)) {
		if (!msg->status_code)
			soup_session_set_item_status (session, item, status);
		item->state = SOUP_MESSAGE_FINISHING;
		soup_connection_disconnect (item->conn);
		soup_message_queue_item_set_connection (item, NULL);
		return;
	}

	if (soup_connection_get_tunnel_addr (item->conn)) {
		status = tunnel_connect (session, item);
		if (!SOUP_STATUS_IS_SUCCESSFUL (status)) {
			soup_connection_disconnect (item->conn);
			soup_message_queue_item_set_connection (item, NULL);
			if (status == SOUP_STATUS_TRY_AGAIN)
				goto try_again;
			soup_session_set_item_status (session, item, status);
			item->state = SOUP_MESSAGE_FINISHING;
			return;
		}
	}

	item->state = SOUP_MESSAGE_READY;
}

static void
process_queue_item (SoupMessageQueueItem *item)
{
	SoupSession *session = item->session;
	SoupSessionSyncPrivate *priv = SOUP_SESSION_SYNC_GET_PRIVATE (session);
	SoupMessage *msg = item->msg;
	SoupProxyURIResolver *proxy_resolver;
	guint status;

	item->state = SOUP_MESSAGE_STARTING;
	do {
		if (item->paused) {
			g_mutex_lock (&priv->lock);
			while (item->paused)
				g_cond_wait (&priv->cond, &priv->lock);
			g_mutex_unlock (&priv->lock);
		}

		switch (item->state) {
		case SOUP_MESSAGE_STARTING:
			proxy_resolver = (SoupProxyURIResolver *)soup_session_get_feature_for_message (session, SOUP_TYPE_PROXY_URI_RESOLVER, msg);
			if (!proxy_resolver) {
				item->state = SOUP_MESSAGE_AWAITING_CONNECTION;
				break;
			}

			status = soup_proxy_uri_resolver_get_proxy_uri_sync (
				proxy_resolver, soup_message_get_uri (msg),
				item->cancellable, &item->proxy_uri);
			if (!SOUP_STATUS_IS_SUCCESSFUL (status)) {
				soup_session_set_item_status (session, item, status);
				item->state = SOUP_MESSAGE_FINISHING;
				break;
			}
			if (!item->proxy_uri) {
				item->state = SOUP_MESSAGE_AWAITING_CONNECTION;
				break;
			}

			item->proxy_addr = soup_address_new (
				item->proxy_uri->host, item->proxy_uri->port);
			status = soup_address_resolve_sync (item->proxy_addr,
							    item->cancellable);
			if (SOUP_STATUS_IS_SUCCESSFUL (status))
				item->state = SOUP_MESSAGE_AWAITING_CONNECTION;
			else {
				soup_session_set_item_status (session, item, soup_status_proxify (status));
				item->state = SOUP_MESSAGE_FINISHING;
			}
			break;

		case SOUP_MESSAGE_AWAITING_CONNECTION:
			g_mutex_lock (&priv->lock);
			do {
				get_connection (item);
				if (item->state == SOUP_MESSAGE_AWAITING_CONNECTION)
					g_cond_wait (&priv->cond, &priv->lock);
			} while (item->state == SOUP_MESSAGE_AWAITING_CONNECTION);
			g_mutex_unlock (&priv->lock);
			break;

		case SOUP_MESSAGE_READY:
			item->state = SOUP_MESSAGE_RUNNING;
			soup_session_send_queue_item (item->session, item, NULL);
			if (item->state != SOUP_MESSAGE_RESTARTING)
				item->state = SOUP_MESSAGE_FINISHING;
			break;

		case SOUP_MESSAGE_RESTARTING:
			item->state = SOUP_MESSAGE_STARTING;
			soup_message_restarted (item->msg);
			break;

		case SOUP_MESSAGE_FINISHING:
			item->state = SOUP_MESSAGE_FINISHED;
			soup_message_finished (item->msg);
			soup_session_unqueue_item (session, item);
			g_cond_broadcast (&priv->cond);
			break;

		default:
			g_warn_if_reached ();
			item->state = SOUP_MESSAGE_FINISHING;
			break;
		}
	} while (item->state != SOUP_MESSAGE_FINISHED);
}

static gboolean
queue_message_callback (gpointer data)
{
	SoupMessageQueueItem *item = data;

	item->callback (item->session, item->msg, item->callback_data);
	g_object_unref (item->session);
	g_object_unref (item->msg);
	soup_message_queue_item_unref (item);
	return FALSE;
}

static gpointer
queue_message_thread (gpointer data)
{
	SoupMessageQueueItem *item = data;

	process_queue_item (item);
	if (item->callback) {
		soup_add_completion (soup_session_get_async_context (item->session),
				     queue_message_callback, item);
	} else {
		g_object_unref (item->session);
		g_object_unref (item->msg);
		soup_message_queue_item_unref (item);
	}

	return NULL;
}

static void
queue_message (SoupSession *session, SoupMessage *msg,
	       SoupSessionCallback callback, gpointer user_data)
{
	SoupMessageQueueItem *item;
	GThread *thread;

	SOUP_SESSION_CLASS (soup_session_sync_parent_class)->
		queue_message (g_object_ref (session), msg, callback, user_data);

	item = soup_message_queue_lookup (soup_session_get_queue (session), msg);
	g_return_if_fail (item != NULL);

	thread = g_thread_new ("SoupSessionSync:queue_message",
			       queue_message_thread, item);
	g_thread_unref (thread);
}

static guint
send_message (SoupSession *session, SoupMessage *msg)
{
	SoupMessageQueueItem *item;
	guint status;

	SOUP_SESSION_CLASS (soup_session_sync_parent_class)->queue_message (session, msg, NULL, NULL);

	item = soup_message_queue_lookup (soup_session_get_queue (session), msg);
	g_return_val_if_fail (item != NULL, SOUP_STATUS_MALFORMED);

	process_queue_item (item);
	status = msg->status_code;
	soup_message_queue_item_unref (item);
	return status;
}

static void
cancel_message (SoupSession *session, SoupMessage *msg, guint status_code)
{
	SoupSessionSyncPrivate *priv = SOUP_SESSION_SYNC_GET_PRIVATE (session);

	g_mutex_lock (&priv->lock);
	SOUP_SESSION_CLASS (soup_session_sync_parent_class)->cancel_message (session, msg, status_code);
	g_cond_broadcast (&priv->cond);
	g_mutex_unlock (&priv->lock);
}

static void
auth_required (SoupSession *session, SoupMessage *msg,
	       SoupAuth *auth, gboolean retrying)
{
	SoupSessionFeature *password_manager;

	password_manager = soup_session_get_feature_for_message (
		session, SOUP_TYPE_PASSWORD_MANAGER, msg);
	if (password_manager) {
		soup_password_manager_get_passwords_sync (
			SOUP_PASSWORD_MANAGER (password_manager),
			msg, auth, NULL); /* FIXME cancellable */
	}

	SOUP_SESSION_CLASS (soup_session_sync_parent_class)->
		auth_required (session, msg, auth, retrying);
}

static void
flush_queue (SoupSession *session)
{
	SoupSessionSyncPrivate *priv = SOUP_SESSION_SYNC_GET_PRIVATE (session);
	SoupMessageQueue *queue;
	SoupMessageQueueItem *item;
	GHashTable *current;
	gboolean done = FALSE;

	/* Record the current contents of the queue */
	current = g_hash_table_new (NULL, NULL);
	queue = soup_session_get_queue (session);
	for (item = soup_message_queue_first (queue);
	     item;
	     item = soup_message_queue_next (queue, item))
		g_hash_table_insert (current, item, item);

	/* Cancel everything */
	SOUP_SESSION_CLASS (soup_session_sync_parent_class)->flush_queue (session);

	/* Wait until all of the items in @current have been removed
	 * from the queue. (This is not the same as "wait for the
	 * queue to be empty", because the app may queue new requests
	 * in response to the cancellation of the old ones. We don't
	 * try to cancel those requests as well, since we'd likely
	 * just end up looping forever.)
	 */
	g_mutex_lock (&priv->lock);
	do {
		done = TRUE;
		for (item = soup_message_queue_first (queue);
		     item;
		     item = soup_message_queue_next (queue, item)) {
			if (g_hash_table_lookup (current, item))
				done = FALSE;
		}

		if (!done)
			g_cond_wait (&priv->cond, &priv->lock);
	} while (!done);
	g_mutex_unlock (&priv->lock);

	g_hash_table_destroy (current);
}

static void
kick (SoupSession *session)
{
	SoupSessionSyncPrivate *priv = SOUP_SESSION_SYNC_GET_PRIVATE (session);

	g_mutex_lock (&priv->lock);
	g_cond_broadcast (&priv->cond);
	g_mutex_unlock (&priv->lock);
}

SoupIODispatcherPool*
get_io_dispatcher_pool (SoupSession *session)
{
	SoupSessionSyncPrivate *priv = SOUP_SESSION_SYNC_GET_PRIVATE (session);

	if (G_UNLIKELY (!priv->io_disp_pool))
		priv->io_disp_pool = soup_io_dispatcher_pool_new (SOUP_IO_DISPATCHER_POOL_IS_THREAD_SAFE, TRUE, NULL);

	return priv->io_disp_pool;

}
