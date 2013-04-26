/*
 * Copyright (C) 2012, LG Electronics
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include "soup-address.h"
#include "soup-io-dispatcher.h"
#include "soup-io-dispatcher-misc.h"
#include "soup-io-dispatcher-server.h"
#include "soup-message.h"
#include "soup-message-private.h"
#include "soup-misc.h"
#include "soup-multipart.h"
#include "soup-socket.h"
#include "soup-status.h"
#include "soup-uri.h"


static void process_message (SoupIODispatcher *io_disp, SoupMessage *msg);

static void handle_partial_get (SoupMessage *msg);
static void get_headers(SoupMessage *msg,
        GString *headers, SoupEncoding  *encoding);

static guint parse_headers(SoupIODispatcher *io_disp, SoupMessage *msg,
        char *headers, guint headers_len, SoupEncoding *encoding);

static void io_data_new(SoupIODispatcher *io_disp, SoupMessage *msg,
        gpointer io_data);
static gboolean io_data_read(SoupIODispatcher *io_disp, gpointer io_data);
static gboolean io_data_write(SoupIODispatcher *io_disp, gpointer io_data);

G_DEFINE_TYPE (SoupIODispatcherServer, soup_io_dispatcher_server, SOUP_TYPE_IO_DISPATCHER)

static void
soup_io_dispatcher_server_init(SoupIODispatcherServer *io_disp_server)
{
}

static void
soup_io_dispatcher_server_class_init(SoupIODispatcherServerClass *io_disp_server_class)
{
    SoupIODispatcherClass *io_disp_class = SOUP_IO_DISPATCHER_CLASS(io_disp_server_class);

    io_disp_class->process_message = process_message;
    io_disp_class->io_data_new = io_data_new;

    io_disp_class->io_data_read = io_data_read;
    io_disp_class->io_data_write = io_data_write;
}

static void
process_message (SoupIODispatcher *io_disp, SoupMessage *msg)
{
    soup_io_dispatcher_process_input_queue (SOUP_IO_DISPATCHER (io_disp));
}


static void
handle_partial_get (SoupMessage *msg)
{
    SoupRange *ranges;
    int nranges;
    SoupBuffer *full_response;

    /* Make sure the message is set up right for us to return a
     * partial response; it has to be a GET, the status must be
     * 200 OK (and in particular, NOT already 206 Partial
     * Content), and the SoupServer must have already filled in
     * the response body
     */
    if (msg->method != SOUP_METHOD_GET ||
        msg->status_code != SOUP_STATUS_OK ||
        soup_message_headers_get_encoding (msg->response_headers) !=
        SOUP_ENCODING_CONTENT_LENGTH ||
        msg->response_body->length == 0 ||
        !soup_message_body_get_accumulate (msg->response_body))
        return;

    /* Oh, and there has to have been a valid Range header on the
     * request, of course.
     */
    if (!soup_message_headers_get_ranges (msg->request_headers,
                          msg->response_body->length,
                          &ranges, &nranges))
        return;

    full_response = soup_message_body_flatten (msg->response_body);
    if (!full_response) {
        soup_message_headers_free_ranges (msg->request_headers, ranges);
        return;
    }

    soup_message_set_status (msg, SOUP_STATUS_PARTIAL_CONTENT);
    soup_message_body_truncate (msg->response_body);

    if (nranges == 1) {
        SoupBuffer *range_buf;

        /* Single range, so just set Content-Range and fix the body. */

        soup_message_headers_set_content_range (msg->response_headers,
                            ranges[0].start,
                            ranges[0].end,
                            full_response->length);
        range_buf = soup_buffer_new_subbuffer (full_response,
                               ranges[0].start,
                               ranges[0].end - ranges[0].start + 1);
        soup_message_body_append_buffer (msg->response_body, range_buf);
        soup_buffer_free (range_buf);
    } else {
        SoupMultipart *multipart;
        SoupMessageHeaders *part_headers;
        SoupBuffer *part_body;
        const char *content_type;
        int i;

        /* Multiple ranges, so build a multipart/byteranges response
         * to replace msg->response_body with.
         */

        multipart = soup_multipart_new ("multipart/byteranges");
        content_type = soup_message_headers_get_one (msg->response_headers,
                                 "Content-Type");
        for (i = 0; i < nranges; i++) {
            part_headers = soup_message_headers_new (SOUP_MESSAGE_HEADERS_MULTIPART);
            if (content_type) {
                soup_message_headers_append (part_headers,
                                 "Content-Type",
                                 content_type);
            }
            soup_message_headers_set_content_range (part_headers,
                                ranges[i].start,
                                ranges[i].end,
                                full_response->length);
            part_body = soup_buffer_new_subbuffer (full_response,
                                   ranges[i].start,
                                   ranges[i].end - ranges[i].start + 1);
            soup_multipart_append_part (multipart, part_headers,
                            part_body);
            soup_message_headers_free (part_headers);
            soup_buffer_free (part_body);
        }

        soup_multipart_to_message (multipart, msg->response_headers,
                       msg->response_body);
        soup_multipart_free (multipart);
    }

    soup_buffer_free (full_response);
    soup_message_headers_free_ranges (msg->request_headers, ranges);
}


static void
get_headers(SoupMessage *msg, GString *headers,
        SoupEncoding  *encoding)
{
    SoupEncoding claimed_encoding;
    SoupMessageHeadersIter iter;
    const char *name, *value;

    handle_partial_get (msg);

    g_string_append_printf (headers, "HTTP/1.%c %d %s\r\n",
                soup_message_get_http_version (msg) == SOUP_HTTP_1_0 ? '0' : '1',
                msg->status_code, msg->reason_phrase);

    claimed_encoding = soup_message_headers_get_encoding (msg->response_headers);
    if ((msg->method == SOUP_METHOD_HEAD ||
         msg->status_code  == SOUP_STATUS_NO_CONTENT ||
         msg->status_code  == SOUP_STATUS_NOT_MODIFIED ||
         SOUP_STATUS_IS_INFORMATIONAL (msg->status_code)) ||
        (msg->method == SOUP_METHOD_CONNECT &&
         SOUP_STATUS_IS_SUCCESSFUL (msg->status_code)))
        *encoding = SOUP_ENCODING_NONE;
    else
        *encoding = claimed_encoding;

    if (claimed_encoding == SOUP_ENCODING_CONTENT_LENGTH &&
        !soup_message_headers_get_content_length (msg->response_headers)) {
        soup_message_headers_set_content_length (msg->response_headers,
                             msg->response_body->length);
    }

    soup_message_headers_iter_init (&iter, msg->response_headers);
    while (soup_message_headers_iter_next (&iter, &name, &value))
        g_string_append_printf (headers, "%s: %s\r\n", name, value);
    g_string_append (headers, "\r\n");
}

static guint
parse_headers(SoupIODispatcher *io_disp, SoupMessage *msg, char *headers,
        guint headers_len, SoupEncoding *encoding)
{
    SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
    char *req_method, *req_path, *url;
    SoupHTTPVersion version;
    const char *req_host;
    guint status;
    SoupURI *uri;
    SoupSocket *sock;
    g_object_get(io_disp, SOUP_IO_DISPATCHER_SOCKET, &sock, NULL);

    status = soup_headers_parse_request (headers, headers_len,
                         msg->request_headers,
                         &req_method,
                         &req_path,
                         &version);
    if (!SOUP_STATUS_IS_SUCCESSFUL (status))
        return status;

    g_object_set (G_OBJECT (msg),
              SOUP_MESSAGE_METHOD, req_method,
              SOUP_MESSAGE_HTTP_VERSION, version,
              NULL);
    g_free (req_method);

    /* Handle request body encoding */
    *encoding = soup_message_headers_get_encoding (msg->request_headers);
    if (*encoding == SOUP_ENCODING_UNRECOGNIZED) {
        if (soup_message_headers_get_list (msg->request_headers, "Transfer-Encoding"))
            return SOUP_STATUS_NOT_IMPLEMENTED;
        else
            return SOUP_STATUS_BAD_REQUEST;
    }

    /* Generate correct context for request */
    req_host = soup_message_headers_get_one (msg->request_headers, "Host");
    if (req_host && strchr (req_host, '/')) {
        g_free (req_path);
        return SOUP_STATUS_BAD_REQUEST;
    }

    if (!strcmp (req_path, "*") && req_host) {
        /* Eg, "OPTIONS * HTTP/1.1" */
        url = g_strdup_printf ("%s://%s",
                       soup_socket_is_ssl (sock) ? "https" : "http",
                       req_host);
        uri = soup_uri_new (url);
        if (uri)
            soup_uri_set_path (uri, "*");
        g_free (url);
    } else if (*req_path != '/') {
        /* Must be an absolute URI */
        uri = soup_uri_new (req_path);
    } else if (req_host) {
        url = g_strdup_printf ("%s://%s%s",
                soup_socket_is_ssl (sock) ? "https" : "http",
                       req_host, req_path);
        uri = soup_uri_new (url);
        g_free (url);
    } else if (priv->http_version == SOUP_HTTP_1_0) {
		/* No Host header, no AbsoluteUri */
		SoupAddress *addr = soup_socket_get_local_address (sock);

		uri = soup_uri_new (NULL);
		soup_uri_set_scheme (uri, soup_socket_is_ssl (sock) ?
				     SOUP_URI_SCHEME_HTTPS :
				     SOUP_URI_SCHEME_HTTP);
		soup_uri_set_host (uri, soup_address_get_physical (addr));
		soup_uri_set_port (uri, soup_address_get_port (addr));
		soup_uri_set_path (uri, req_path);
	} else
		uri = NULL;

	g_free (req_path);

	if (!SOUP_URI_VALID_FOR_HTTP (uri)) {
		/* certainly not "a valid host on the server" (RFC2616 5.2.3)
		 * SOUP_URI_VALID_FOR_HTTP also guards against uri == NULL
		 */
		if (uri)
			soup_uri_free (uri);
		return SOUP_STATUS_BAD_REQUEST;
	}

	soup_message_set_uri (msg, uri);
	soup_uri_free (uri);

	return SOUP_STATUS_OK;
}

static void
io_data_new(SoupIODispatcher *io_disp, SoupMessage *msg,
        gpointer io_data)
{
    SoupMessageIOData *io = io_data;
    g_return_if_fail(io_data);

    io->read_headers = msg->request_headers;
    io->write_headers = msg->response_headers;

    io->read_body = msg->request_body;
    io->write_body = msg->response_body;
}

static gboolean
io_data_read(SoupIODispatcher *io_disp, gpointer io_data)
{
    SoupMessageIOData *io = (SoupMessageIOData*)io_data;
    SoupMessage *msg = io->msg;
    SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE(msg);
    guint status;

    if (io->read_state == SOUP_MESSAGE_IO_STATE_NOT_STARTED)
    	io->read_state = SOUP_MESSAGE_IO_STATE_HEADERS;

 read_more:
    switch (io->read_state) {
    case SOUP_MESSAGE_IO_STATE_NOT_STARTED:
        return FALSE;

    case SOUP_MESSAGE_IO_STATE_HEADERS:
        if (!soup_io_dispatcher_read_metadata (io_disp, io, TRUE))
            return FALSE;

        /* We need to "rewind" io->read_meta_buf back one line.
         * That SHOULD be two characters (CR LF), but if the
         * web server was stupid, it might only be one.
         */
        if (io->read_meta_buf->len < 3 ||
            io->read_meta_buf->data[io->read_meta_buf->len - 2] == '\n')
            io->read_meta_buf->len--;
        else
            io->read_meta_buf->len -= 2;
        io->read_meta_buf->data[io->read_meta_buf->len] = '\0';
        status = parse_headers (io_disp, msg, (char *)io->read_meta_buf->data,
                           io->read_meta_buf->len,
                           &io->read_encoding);
        g_byte_array_set_size (io->read_meta_buf, 0);

        if (status != SOUP_STATUS_OK) {
            /* Either we couldn't parse the headers, or they
             * indicated something that would mean we wouldn't
             * be able to parse the body. (Eg, unknown
             * Transfer-Encoding.). Skip the rest of the
             * reading, and make sure the connection gets
             * closed when we're done.
             */
            soup_message_set_status (msg, status);
            soup_message_headers_append (msg->request_headers,
                             "Connection", "close");
            io->read_state = SOUP_MESSAGE_IO_STATE_FINISHING;
            break;
        }

        if (io->read_encoding == SOUP_ENCODING_EOF)
            io->read_eof_ok = TRUE;

        if (io->read_encoding == SOUP_ENCODING_CONTENT_LENGTH) {
            io->read_length = soup_message_headers_get_content_length (io->read_headers);
        }

        if (soup_message_headers_get_expectations (msg->request_headers) & SOUP_EXPECTATION_CONTINUE) {
            /* The client requested a Continue response. The
             * got_headers handler may change this to something
             * else though.
             */
            soup_message_set_status (msg, SOUP_STATUS_CONTINUE);
            io->write_state = SOUP_MESSAGE_IO_STATE_HEADERS;
            io->read_state = SOUP_MESSAGE_IO_STATE_BLOCKING;
        } else {
            io->read_state = io_body_state (io->read_encoding);
        }

        SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
        soup_message_got_headers (msg);
        SOUP_MESSAGE_IO_RETURN_VAL_IF_CANCELLED_OR_PAUSED(FALSE);
        break;

    case SOUP_MESSAGE_IO_STATE_BLOCKING:
        /* As in the io_data_write case, we *must* return here. */
        return FALSE;

    case SOUP_MESSAGE_IO_STATE_BODY:
        if (!soup_io_dispatcher_read_body_chunk (io_disp, io))
            return FALSE;

    got_body:
        if (!io_handle_sniffing (io, TRUE)) {
            /* If the message was paused (as opposed to
             * cancelled), we need to make sure we wind up
             * back here when it's unpaused, even if it
             * was doing a chunked or EOF-terminated read
             * before.
             */
            if (io == priv->io_data) {
                io->read_state = SOUP_MESSAGE_IO_STATE_BODY;
                io->read_encoding = SOUP_ENCODING_CONTENT_LENGTH;
                io->read_length = 0;
            }
            return FALSE;
        }

        io->read_state = SOUP_MESSAGE_IO_STATE_FINISHING;

        SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
        soup_message_got_body (msg);
        SOUP_MESSAGE_IO_RETURN_VAL_IF_CANCELLED_OR_PAUSED(FALSE);
        break;

    case SOUP_MESSAGE_IO_STATE_CHUNK_SIZE:
        if (!soup_io_dispatcher_read_metadata (io_disp, io, FALSE))
            return FALSE;

        io->read_length = strtoul ((char *)io->read_meta_buf->data, NULL, 16);
        g_byte_array_set_size (io->read_meta_buf, 0);

        if (io->read_length > 0)
            io->read_state = SOUP_MESSAGE_IO_STATE_CHUNK;
        else
            io->read_state = SOUP_MESSAGE_IO_STATE_TRAILERS;
        break;

    case SOUP_MESSAGE_IO_STATE_CHUNK:
        if (!soup_io_dispatcher_read_body_chunk (io_disp, io))
            return FALSE;

        io->read_state = SOUP_MESSAGE_IO_STATE_CHUNK_END;
        break;

    case SOUP_MESSAGE_IO_STATE_CHUNK_END:
        if (!soup_io_dispatcher_read_metadata (io_disp, io, FALSE))
            return FALSE;

        g_byte_array_set_size (io->read_meta_buf, 0);
        io->read_state = SOUP_MESSAGE_IO_STATE_CHUNK_SIZE;
        break;

    case SOUP_MESSAGE_IO_STATE_TRAILERS:
        if (!soup_io_dispatcher_read_metadata (io_disp, io, FALSE))
            return FALSE;

        if (io->read_meta_buf->len <= SOUP_MESSAGE_IO_EOL_LEN)
            goto got_body;

        /* FIXME: process trailers */
        g_byte_array_set_size (io->read_meta_buf, 0);
        break;

    case SOUP_MESSAGE_IO_STATE_FINISHING:
        io->read_state = SOUP_MESSAGE_IO_STATE_DONE;
        io->write_state = SOUP_MESSAGE_IO_STATE_HEADERS;

        return TRUE;

    case SOUP_MESSAGE_IO_STATE_DONE:
    default:
        g_return_val_if_reached (TRUE);
    }

    goto read_more;

}

static gboolean
io_data_write(SoupIODispatcher *io_disp, gpointer io_data)
{
    SoupMessageIOData *io = (SoupMessageIOData*)io_data;
    SoupMessage *msg = io->msg;
    SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE(msg);

 write_more:
    switch (io->write_state) {
    case SOUP_MESSAGE_IO_STATE_NOT_STARTED:
        return FALSE;


    case SOUP_MESSAGE_IO_STATE_HEADERS:
        if (!io->write_buf->len) {
            get_headers (msg, io->write_buf, &io->write_encoding);
            if (!io->write_buf->len) {
                soup_io_dispatcher_pause_io_data(io_disp, io);
                return FALSE;
            }
        }

        if (!soup_io_dispatcher_write_data (io_disp, io, io->write_buf->str,
                 io->write_buf->len, FALSE))
            return FALSE;

        g_string_truncate (io->write_buf, 0);

        if (io->write_encoding == SOUP_ENCODING_CONTENT_LENGTH) {
            io->write_length = soup_message_headers_get_content_length (io->write_headers);
        }

        if (SOUP_STATUS_IS_INFORMATIONAL (msg->status_code)) {
            if (msg->status_code == SOUP_STATUS_CONTINUE) {
                /* Stop and wait for the body now */
                io->write_state =
                    SOUP_MESSAGE_IO_STATE_BLOCKING;
                io->read_state = io_body_state (io->read_encoding);
            } else {
                /* We just wrote a 1xx response
                 * header, so stay in STATE_HEADERS.
                 * (The caller will pause us from the
                 * wrote_informational callback if he
                 * is not ready to send the final
                 * response.)
                 */
            }
        } else {
            io->write_state = io_body_state (io->write_encoding);

            /* If the client was waiting for a Continue
             * but we sent something else, then they're
             * now done writing.
             */
            if (io->read_state == SOUP_MESSAGE_IO_STATE_BLOCKING)
                io->read_state = SOUP_MESSAGE_IO_STATE_DONE;
        }

        SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
        if (SOUP_STATUS_IS_INFORMATIONAL (msg->status_code)) {
            soup_message_wrote_informational (msg);
            soup_message_cleanup_response (msg);
        } else
            soup_message_wrote_headers (msg);
        SOUP_MESSAGE_IO_RETURN_VAL_IF_CANCELLED_OR_PAUSED(FALSE);
        break;


    case SOUP_MESSAGE_IO_STATE_BLOCKING:
        /* If io_data_read reached a point where we could write
         * again, it would have recursively called io_data_write.
         * So (a) we don't need to try to keep writing, and
         * (b) we can't anyway, because msg may have been
         * destroyed.
         */
        return FALSE;


    case SOUP_MESSAGE_IO_STATE_BODY:
        if (!io->write_length && io->write_encoding != SOUP_ENCODING_EOF) {
        wrote_body:
            io->write_state = SOUP_MESSAGE_IO_STATE_FINISHING;

            SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
            soup_message_wrote_body (msg);
            SOUP_MESSAGE_IO_RETURN_VAL_IF_CANCELLED_OR_PAUSED(FALSE);
            break;
        }

        if (!io->write_chunk) {
            io->write_chunk = soup_message_body_get_chunk (io->write_body, io->write_body_offset);
            if (!io->write_chunk) {
                soup_io_dispatcher_pause_io_data(io_disp, io);
                return FALSE;
            }
            if (io->write_chunk->length > io->write_length &&
                io->write_encoding != SOUP_ENCODING_EOF) {
                /* App is trying to write more than it
                 * claimed it would; we have to truncate.
                 */
                SoupBuffer *truncated =
                    soup_buffer_new_subbuffer (io->write_chunk,
                                   0, io->write_length);
                soup_buffer_free (io->write_chunk);
                io->write_chunk = truncated;
            } else if (io->write_encoding == SOUP_ENCODING_EOF &&
                   !io->write_chunk->length)
                goto wrote_body;
        }

        if (!soup_io_dispatcher_write_data (io_disp, io, io->write_chunk->data,
                 io->write_chunk->length, TRUE))
            return FALSE;

		if (priv->msg_flags & SOUP_MESSAGE_CAN_REBUILD)
			soup_message_body_wrote_chunk (io->write_body, io->write_chunk);

        io->write_body_offset += io->write_chunk->length;
        soup_buffer_free (io->write_chunk);
        io->write_chunk = NULL;

        SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
        soup_message_wrote_chunk (msg);
        SOUP_MESSAGE_IO_RETURN_VAL_IF_CANCELLED_OR_PAUSED(FALSE);
        break;

    case SOUP_MESSAGE_IO_STATE_CHUNK_SIZE:
        if (!io->write_chunk) {
            io->write_chunk = soup_message_body_get_chunk (io->write_body, io->write_body_offset);
            if (!io->write_chunk) {
                soup_io_dispatcher_pause_io_data(io_disp, io);
                return FALSE;
            }
            g_string_append_printf (io->write_buf, "%lx\r\n",
                        (unsigned long) io->write_chunk->length);
            io->write_body_offset += io->write_chunk->length;
        }

        if (!soup_io_dispatcher_write_data (io_disp, io, io->write_buf->str,
                 io->write_buf->len, FALSE))
            return FALSE;

        g_string_truncate (io->write_buf, 0);

        if (io->write_chunk->length == 0) {
            /* The last chunk has no CHUNK_END... */
            io->write_state = SOUP_MESSAGE_IO_STATE_TRAILERS;
            break;
        }

        io->write_state = SOUP_MESSAGE_IO_STATE_CHUNK;
        /* fall through */


    case SOUP_MESSAGE_IO_STATE_CHUNK:
        if (!soup_io_dispatcher_write_data (io_disp, io, io->write_chunk->data,
                 io->write_chunk->length, TRUE))
            return FALSE;

		if (priv->msg_flags & SOUP_MESSAGE_CAN_REBUILD)
			soup_message_body_wrote_chunk (io->write_body, io->write_chunk);

        soup_buffer_free (io->write_chunk);
        io->write_chunk = NULL;

        io->write_state = SOUP_MESSAGE_IO_STATE_CHUNK_END;

        SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
        soup_message_wrote_chunk (msg);
        SOUP_MESSAGE_IO_RETURN_VAL_IF_CANCELLED_OR_PAUSED(FALSE);

        /* fall through */


    case SOUP_MESSAGE_IO_STATE_CHUNK_END:
        if (!soup_io_dispatcher_write_data (io_disp, io, SOUP_MESSAGE_IO_EOL,
                 SOUP_MESSAGE_IO_EOL_LEN, FALSE))
            return FALSE;

        io->write_state = SOUP_MESSAGE_IO_STATE_CHUNK_SIZE;
        break;


    case SOUP_MESSAGE_IO_STATE_TRAILERS:
        if (!soup_io_dispatcher_write_data (io_disp, io, SOUP_MESSAGE_IO_EOL,
                 SOUP_MESSAGE_IO_EOL_LEN, FALSE))
            return FALSE;

        io->write_state = SOUP_MESSAGE_IO_STATE_FINISHING;

        SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
        soup_message_wrote_body (msg);
        SOUP_MESSAGE_IO_RETURN_VAL_IF_CANCELLED_OR_PAUSED(FALSE);
        /* fall through */


    case SOUP_MESSAGE_IO_STATE_FINISHING:
        io->write_state = SOUP_MESSAGE_IO_STATE_DONE;

        return TRUE;


    case SOUP_MESSAGE_IO_STATE_DONE:
    default:
        g_return_val_if_reached (TRUE);
    }

    goto write_more;
}
