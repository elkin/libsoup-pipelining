/*
 * soup-io-dispatcher-misc.c
 *
 *  Created on: Apr 6, 2012
 *      Author: vladimir
 */

#include "soup-io-dispatcher-misc.h"

gboolean
io_handle_sniffing(SoupMessageIOData *io, gboolean done_reading)
{
    SoupMessage *msg = io->msg;
    SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
    SoupBuffer *sniffed_buffer;
    char *sniffed_mime_type;
    GHashTable *params = NULL;

    if (!priv->sniffer)
        return TRUE;

    if (!io->sniff_data) {
        io->sniff_data = soup_message_body_new ();
        io->need_content_sniffed = TRUE;
    }

    if (io->need_content_sniffed) {
        if (io->sniff_data->length < priv->bytes_for_sniffing &&
                !done_reading)
            return TRUE;

        io->need_content_sniffed = FALSE;
        sniffed_buffer = soup_message_body_flatten (io->sniff_data);
        sniffed_mime_type = soup_content_sniffer_sniff (priv->sniffer, msg, sniffed_buffer, &params);

        SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
        soup_message_content_sniffed (msg, sniffed_mime_type, params);
        g_free (sniffed_mime_type);
        if (params)
            g_hash_table_destroy (params);
        if (sniffed_buffer)
            soup_buffer_free (sniffed_buffer);
        SOUP_MESSAGE_IO_RETURN_VAL_IF_CANCELLED_OR_PAUSED (FALSE);
    }

    if (io->need_got_chunk) {
        io->need_got_chunk = FALSE;
        sniffed_buffer = soup_message_body_flatten (io->sniff_data);

        SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
        soup_message_got_chunk (msg, sniffed_buffer);
        soup_buffer_free (sniffed_buffer);
        SOUP_MESSAGE_IO_RETURN_VAL_IF_CANCELLED_OR_PAUSED (FALSE);
    }

    return TRUE;
}

SoupMessageIOState
io_body_state (SoupEncoding encoding)
{
    if (encoding == SOUP_ENCODING_CHUNKED)
        return SOUP_MESSAGE_IO_STATE_CHUNK_SIZE;
    else
        return SOUP_MESSAGE_IO_STATE_BODY;
}

inline gboolean
is_io_data_finished (SoupMessageIOData *io)
{
	return io->read_state == SOUP_MESSAGE_IO_STATE_DONE &&
				io->write_state == SOUP_MESSAGE_IO_STATE_DONE;
}
