/*
 * Copyright (C) 2012, LG Electronics
 */

#ifndef SOUP_IO_DISPATCHER_MISC_H
#define SOUP_IO_DISPATCHER_MISC_H 1

#include "soup-io-dispatcher.h"

typedef enum {
    SOUP_MESSAGE_IO_STATE_NOT_STARTED,
    SOUP_MESSAGE_IO_STATE_HEADERS,
    SOUP_MESSAGE_IO_STATE_BLOCKING,
    SOUP_MESSAGE_IO_STATE_BODY,
    SOUP_MESSAGE_IO_STATE_CHUNK_SIZE,
    SOUP_MESSAGE_IO_STATE_CHUNK,
    SOUP_MESSAGE_IO_STATE_CHUNK_END,
    SOUP_MESSAGE_IO_STATE_TRAILERS,
    SOUP_MESSAGE_IO_STATE_FINISHING,
    SOUP_MESSAGE_IO_STATE_DONE
} SoupMessageIOState;

#define SOUP_MESSAGE_IO_STATE_ACTIVE(state) \
	(state != SOUP_MESSAGE_IO_STATE_NOT_STARTED && \
	 state != SOUP_MESSAGE_IO_STATE_BLOCKING && \
	 state != SOUP_MESSAGE_IO_STATE_DONE)

typedef struct {
	GList *queue_link;
	SoupMessageIOState state;
	gboolean blocked;
} ItemState;

typedef struct {
    GCancellable         *cancellable;
	SoupMessage			 *msg;
	GError               *error;

	GList                *read_queue;
    SoupMessageIOState    read_state;
    gboolean              read_blocked;

	GList                *write_queue;
    SoupMessageIOState    write_state;
    gboolean              write_blocked;


	GList				 *paused_queue;
	GRecMutex            *io_data_mtx;

    SoupEncoding          read_encoding;
    GByteArray           *read_meta_buf;
    SoupMessageHeaders   *read_headers;
    SoupMessageBody      *read_body;
    goffset               read_length;
    gboolean              read_eof_ok;


    SoupMessageBody      *sniff_data;

    SoupEncoding          write_encoding;
    GString              *write_buf;
    SoupMessageHeaders   *write_headers;
    SoupMessageBody      *write_body;
    SoupBuffer           *write_chunk;
    goffset               write_body_offset;
    goffset               write_length;
    goffset               written;

    GSource 			 *unpause_source;
    GClosure			 *msg_invalidation_closure;

    SoupMessageCompletionFn   completion_cb;
    gpointer                  completion_data;

    gboolean              need_content_sniffed, need_got_chunk;
    gboolean			  io_error;
    gboolean              paused;

    gboolean              cancelled;
} SoupMessageIOData;

#define dummy_to_make_emacs_happy {
#define SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK { gboolean cancelled; g_object_ref (msg);
#define SOUP_MESSAGE_IO_RETURN_IF_CANCELLED_OR_PAUSED cancelled = (priv->io_data != io || io->cancelled); g_object_unref (msg); if (cancelled || io->paused) return; }
#define SOUP_MESSAGE_IO_RETURN_VAL_IF_CANCELLED_OR_PAUSED(val) cancelled = (priv->io_data != io || io->cancelled); g_object_unref (msg); if (cancelled || io->paused) return val; }

#define SOUP_MESSAGE_IO_EOL            "\r\n"
#define SOUP_MESSAGE_IO_EOL_LEN        2

gboolean io_handle_sniffing(SoupMessageIOData *io, gboolean done_reading);
SoupMessageIOState io_body_state (SoupEncoding encoding);
inline gboolean is_io_data_finished (SoupMessageIOData *io);

#endif /* SOUP_IO_DISPATCHER_MISC_H */
