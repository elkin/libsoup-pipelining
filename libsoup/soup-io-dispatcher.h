/*
 * Copyright (C) 2012, LG Electronics
 */

#ifndef SOUP_IO_DISPATCHER_H
#define SOUP_IO_DISPATCHER_H 1

#include <libsoup/soup-types.h>
#include <libsoup/soup-message-private.h>

G_BEGIN_DECLS

#define SOUP_TYPE_IO_DISPATCHER            (soup_io_dispatcher_get_type ())
#define SOUP_IO_DISPATCHER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), SOUP_TYPE_IO_DISPATCHER, SoupIODispatcher))
#define SOUP_IO_DISPATCHER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), SOUP_TYPE_IO_DISPATCHER, SoupIODispatcherClass))
#define SOUP_IS_IO_DISPATCHER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), SOUP_TYPE_IO_DISPATCHER))
#define SOUP_IS_IO_DISPATCHER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((obj), SOUP_TYPE_IO_DISPATCHER))
#define SOUP_IO_DISPATCHER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), SOUP_TYPE_IO_DISPATCHER, SoupIODispatcherClass))

struct _SoupIODispatcher {
	GObject parent;
};

/* It must return boolean whether the request/response was blocked or not */
typedef gboolean (*SoupProcessIOQueueFn)(SoupIODispatcher *io_disp, gpointer io_data);

typedef struct {
	GObjectClass parent;

	/* virtual methods */

	void                 (*process_message) (SoupIODispatcher *io_disp,
								 SoupMessage *msg);

	void                 (*cancel_message)  (SoupIODispatcher *io_disp,
								 SoupMessage *msg);

	void                 (*pause_message)   (SoupIODispatcher *io_disp,
								 SoupMessage *msg);

	void                 (*unpause_message) (SoupIODispatcher *io_disp,
								 SoupMessage *msg);

	gboolean             (*is_queue_full)   (SoupIODispatcher *io_disp);

	/*< protected >*/
	/* virtual protected functions are used inside IO dispatcher */
	SoupProcessIOQueueFn io_data_read;
	SoupProcessIOQueueFn io_data_write;

	void                 (*io_data_new)     (SoupIODispatcher *io_disp,
								 SoupMessage *msg, gpointer io_data);

	/*< signals >*/
	void                 (*io_msg_restart)  (SoupIODispatcher *io_disp,
								 SoupMessage *msg);
	void                 (*idle_timeout)    (SoupIODispatcher *io_disp);

} SoupIODispatcherClass;

GType soup_io_dispatcher_get_type (void);

#define SOUP_IO_DISPATCHER_HOST					   "host"
#define SOUP_IO_DISPATCHER_SOCKET				   "socket"
#define SOUP_IO_DISPATCHER_IS_QUEUE_EMPTY          "is-queue-empty"
#define SOUP_IO_DISPATCHER_IS_QUEUE_FULL           "is-queue-full"
#define SOUP_IO_DISPATCHER_IS_PIPELINING_SUPPORTED "is-pipelining-supported"
#define SOUP_IO_DISPATCHER_IS_VIA_PROXY             "is-via-proxy"
#define SOUP_IO_DISPATCHER_IS_THREAD_SAFE           "is-thread-safe"
#define SOUP_IO_DISPATCHER_MAX_PIPELINED_REQ        "max-pipelined-requests"
#define SOUP_IO_DISPATCHER_RESPONSE_BLOCK_SIZE      "response-block-size"
#define SOUP_IO_DISPATCHER_IDLE_TIMEOUT             "idle-timeout"
#define SOUP_IO_DISPATCHER_ASYNC_CONTEXT            "async-context"

void        soup_io_dispatcher_queue_message   (SoupIODispatcher       *io_disp,
					SoupMessage               *msg);

void        soup_io_dispatcher_process_message (SoupIODispatcher       *io_disp,
					SoupMessage               *msg,
					GCancellable              *cancellable,
					SoupMessageCompletionFn    completion_cb,
					gpointer                   user_data);

void        soup_io_dispatcher_cancel_message  (SoupIODispatcher       *io_disp,
					SoupMessage               *msg);

void        soup_io_dispatcher_pause_message   (SoupIODispatcher       *io_disp,
					SoupMessage               *msg);

void        soup_io_dispatcher_unpause_message (SoupIODispatcher       *io_disp,
					SoupMessage               *msg);

/* properties */
gboolean    soup_io_dispatcher_is_msg_in_progress         (SoupIODispatcher *io_disp,
					SoupMessage       *msg);
gboolean    soup_io_dispatcher_is_queue_empty             (SoupIODispatcher *io_disp);
gboolean    soup_io_dispatcher_is_queue_full              (SoupIODispatcher *io_disp);
void        soup_io_dispatcher_set_pipelining_support     (SoupIODispatcher *io_disp,
					gboolean           value);
gboolean    soup_io_dispatcher_is_pipelining_supported    (SoupIODispatcher *io_disp);
gboolean    soup_io_dispatcher_is_via_proxy               (SoupIODispatcher *io_disp);

guint       soup_io_dispatcher_get_max_pipelined_requests (SoupIODispatcher *io_disp);
void        soup_io_dispatcher_set_max_pipelined_requests (SoupIODispatcher *io_disp,
					guint              value);

guint       soup_io_dispatcher_get_response_block_size    (SoupIODispatcher *io_disp);
void        soup_io_dispatcher_set_response_block_size    (SoupIODispatcher *io_disp,
					guint              value);

guint       soup_io_dispatcher_get_queue_length           (SoupIODispatcher *io_disp);
SoupSocket *soup_io_dispatcher_get_socket                 (SoupIODispatcher *io_disp);
void        soup_io_dispatcher_set_socket                 (SoupIODispatcher *io_disp,
					SoupSocket        *socket);

/*< protected */
void        soup_io_dispatcher_process_input_queue        (SoupIODispatcher *io_disp);
void        soup_io_dispatcher_process_output_queue       (SoupIODispatcher *io_disp);

void        soup_io_dispatcher_pause_io_data              (SoupIODispatcher *io_disp,
					gpointer           io_data);
gboolean    soup_io_dispatcher_write_data                 (SoupIODispatcher *io_disp,
					gpointer           io_data,
					const char        *data,
					guint              len,
					gboolean           body);
gboolean    soup_io_dispatcher_read_body_chunk            (SoupIODispatcher *io_disp,
					gpointer           io_data);
gboolean    soup_io_dispatcher_read_metadata              (SoupIODispatcher *io_disp,
					gpointer           io_data,
					gboolean           to_blank);

G_END_DECLS

#endif /* SOUP_IO_DISPATCHER_H_ */
