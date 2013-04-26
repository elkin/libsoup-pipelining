/*
 * Copyright (C) 2012, LG Electronics
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include "soup-io-dispatcher.h"
#include "soup-io-dispatcher-client.h"
#include "soup-io-dispatcher-misc.h"
#include "soup-message.h"
#include "soup-message-private.h"
#include "soup-misc.h"
#include "soup-status.h"
#include "soup-uri.h"


static void process_message (SoupIODispatcher *io_disp, SoupMessage *msg);

static void get_headers (SoupIODispatcher *io_disp, SoupMessage *msg,
		GString *header, SoupEncoding  *encoding);

static guint parse_headers (SoupMessage *msg,
		char *headers, guint headers_len, SoupEncoding *encoding);

static void io_data_new (SoupIODispatcher *io_disp, SoupMessage *msg,
        gpointer io_data);
static gboolean io_data_read (SoupIODispatcher *io_disp, gpointer io_data);
static gboolean io_data_write (SoupIODispatcher *io_disp, gpointer io_data);

G_DEFINE_TYPE (SoupIODispatcherClient, soup_io_dispatcher_client, SOUP_TYPE_IO_DISPATCHER)

static void
soup_io_dispatcher_client_init(SoupIODispatcherClient *io_disp_client)
{
}

static void
soup_io_dispatcher_client_class_init(SoupIODispatcherClientClass *io_disp_client_class)
{
	SoupIODispatcherClass *io_disp_class = SOUP_IO_DISPATCHER_CLASS(io_disp_client_class);

	io_disp_class->process_message = process_message;
	io_disp_class->io_data_new = io_data_new;

	io_disp_class->io_data_read = io_data_read;
	io_disp_class->io_data_write = io_data_write;
}


static void
process_message (SoupIODispatcher *io_disp, SoupMessage *msg)
{
	soup_message_cleanup_response (msg);
	soup_io_dispatcher_process_output_queue (SOUP_IO_DISPATCHER (io_disp));
}

static void
get_headers(SoupIODispatcher *io_disp, SoupMessage *req, GString *header,
		SoupEncoding  *encoding)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (req);
	SoupURI *uri = soup_message_get_uri (req);
	char *uri_host;
	char *uri_string;
	SoupMessageHeadersIter iter;
	const char *name, *value;

	if (strchr (uri->host, ':'))
		uri_host = g_strdup_printf ("[%s]", uri->host);
	else if (g_hostname_is_non_ascii (uri->host))
		uri_host = g_hostname_to_ascii (uri->host);
	else
		uri_host = uri->host;

	if (req->method == SOUP_METHOD_CONNECT) {
		/* CONNECT URI is hostname:port for tunnel destination */
		uri_string = g_strdup_printf ("%s:%d", uri_host, uri->port);
	} else {
		gboolean proxy = soup_io_dispatcher_is_via_proxy(io_disp);

		/* Proxy expects full URI to destination. Otherwise
		 * just the path.
		 */
		uri_string = soup_uri_to_string (uri, !proxy);

		if (proxy && uri->fragment) {
			/* Strip fragment */
			char *fragment = strchr (uri_string, '#');
			if (fragment)
				*fragment = '\0';
		}
	}

	if (priv->http_version == SOUP_HTTP_1_0) {
		g_string_append_printf (header, "%s %s HTTP/1.0\r\n",
					req->method, uri_string);
	} else {
		g_string_append_printf (header, "%s %s HTTP/1.1\r\n",
					req->method, uri_string);
		if (!soup_message_headers_get_one (req->request_headers, "Host")) {
			if (soup_uri_uses_default_port (uri)) {
				g_string_append_printf (header, "Host: %s\r\n",
							uri_host);
			} else {
				g_string_append_printf (header, "Host: %s:%d\r\n",
							uri_host, uri->port);
			}
		}
	}
	g_free (uri_string);
	if (uri_host != uri->host)
		g_free (uri_host);

	*encoding = soup_message_headers_get_encoding (req->request_headers);
	if ((*encoding == SOUP_ENCODING_CONTENT_LENGTH ||
	     *encoding == SOUP_ENCODING_NONE) &&
	    (req->request_body->length > 0 ||
	     soup_message_headers_get_one (req->request_headers, "Content-Type")) &&
	    !soup_message_headers_get_content_length (req->request_headers)) {
		*encoding = SOUP_ENCODING_CONTENT_LENGTH;
		soup_message_headers_set_content_length (req->request_headers,
							 req->request_body->length);
	}

	soup_message_headers_iter_init (&iter, req->request_headers);
	while (soup_message_headers_iter_next (&iter, &name, &value))
		g_string_append_printf (header, "%s: %s\r\n", name, value);
	g_string_append (header, "\r\n");
}

static guint
parse_headers(SoupMessage *req, char *headers,
		guint headers_len, SoupEncoding *encoding)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (req);
	SoupHTTPVersion version;

	g_free(req->reason_phrase);
	req->reason_phrase = NULL;
	if (!soup_headers_parse_response (headers, headers_len,
					  req->response_headers,
					  &version,
					  &req->status_code,
					  &req->reason_phrase))
		return SOUP_STATUS_MALFORMED;

	g_object_notify (G_OBJECT (req), SOUP_MESSAGE_STATUS_CODE);
	g_object_notify (G_OBJECT (req), SOUP_MESSAGE_REASON_PHRASE);

	if (version < priv->http_version) {
		priv->http_version = version;
		g_object_notify (G_OBJECT (req), SOUP_MESSAGE_HTTP_VERSION);
	}

	if ((req->method == SOUP_METHOD_HEAD ||
	     req->status_code  == SOUP_STATUS_NO_CONTENT ||
	     req->status_code  == SOUP_STATUS_NOT_MODIFIED ||
	     SOUP_STATUS_IS_INFORMATIONAL (req->status_code)) ||
	    (req->method == SOUP_METHOD_CONNECT &&
	     SOUP_STATUS_IS_SUCCESSFUL (req->status_code)))
		*encoding = SOUP_ENCODING_NONE;
	else
		*encoding = soup_message_headers_get_encoding (req->response_headers);

	if (*encoding == SOUP_ENCODING_UNRECOGNIZED)
		return SOUP_STATUS_MALFORMED;

	return SOUP_STATUS_OK;
}

static void
io_data_new(SoupIODispatcher *io_disp, SoupMessage *msg,
        gpointer io_data)
{
    SoupMessageIOData *io = io_data;
	g_return_if_fail(io_data);

	io->read_headers = msg->response_headers;
	io->write_headers = msg->request_headers;

	io->read_body = msg->response_body;
	io->write_body = msg->request_body;
}

//------------------------------------------------------------------------------

static gboolean
io_data_write (SoupIODispatcher *io_disp, gpointer io_data)
{
    SoupMessageIOData *io = (SoupMessageIOData*)io_data;
    SoupMessage *msg = io->msg;
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE(msg);

	if (io->write_state == SOUP_MESSAGE_IO_STATE_NOT_STARTED)
		io->write_state = SOUP_MESSAGE_IO_STATE_HEADERS;

 write_more:
	switch (io->write_state) {
	case SOUP_MESSAGE_IO_STATE_NOT_STARTED:
		return FALSE;


	case SOUP_MESSAGE_IO_STATE_HEADERS:
		if (!io->write_buf->len) {
			get_headers (io_disp, msg, io->write_buf,
					    &io->write_encoding);
			if (!io->write_buf->len) {
				soup_io_dispatcher_pause_io_data(io_disp, io);
				return FALSE;
			}
		}

		if (!soup_io_dispatcher_write_data (io_disp, io, io->write_buf->str,
				 io->write_buf->len, FALSE)) {

			return FALSE;
		}


		g_string_truncate (io->write_buf, 0);

		if (io->write_encoding == SOUP_ENCODING_CONTENT_LENGTH) {
			io->write_length = soup_message_headers_get_content_length (io->write_headers);
		}

		if (soup_message_headers_get_expectations (msg->request_headers) & SOUP_EXPECTATION_CONTINUE) {
			/* Need to wait for the Continue response */
			io->write_state = SOUP_MESSAGE_IO_STATE_BLOCKING;
			io->read_state = SOUP_MESSAGE_IO_STATE_HEADERS;
		} else {
			io->write_state = io_body_state (io->write_encoding);
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
				 io->write_chunk->length, TRUE)) {

			return FALSE;
		}

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
		io->read_state = SOUP_MESSAGE_IO_STATE_HEADERS;

		return TRUE;

	case SOUP_MESSAGE_IO_STATE_DONE:
	default:
		g_return_val_if_reached(TRUE);
	}

	goto write_more;
}

static gboolean
io_data_read (SoupIODispatcher *io_disp, gpointer io_data)
{
    SoupMessageIOData *io = (SoupMessageIOData*)io_data;
    SoupMessage *msg = io->msg;
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE(msg);

	guint status;

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
		status = parse_headers (msg, (char *)io->read_meta_buf->data,
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

			if (!soup_message_is_keepalive (msg)) {
				/* Some servers suck and send
				 * incorrect Content-Length values, so
				 * allow EOF termination in this case
				 * (iff the message is too short) too.
				 */
				io->read_eof_ok = TRUE;
			}
		}

		if (SOUP_STATUS_IS_INFORMATIONAL (msg->status_code)) {
			if (msg->status_code == SOUP_STATUS_CONTINUE &&
			    io->write_state == SOUP_MESSAGE_IO_STATE_BLOCKING) {
				/* Pause the reader, unpause the writer */
				io->read_state =
					SOUP_MESSAGE_IO_STATE_BLOCKING;
				io->write_state =
					io_body_state (io->write_encoding);
			} else {
				/* Just stay in HEADERS */
				io->read_state = SOUP_MESSAGE_IO_STATE_HEADERS;
			}
		} else {
			io->read_state = io_body_state (io->read_encoding);

			/* If the client was waiting for a Continue
			 * but got something else, then it's done
			 * writing.
			 */
			if (io->write_state == SOUP_MESSAGE_IO_STATE_BLOCKING)
				io->write_state = SOUP_MESSAGE_IO_STATE_DONE;
		}

		if (SOUP_STATUS_IS_INFORMATIONAL (msg->status_code)) {
			SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
			soup_message_got_informational (msg);
			soup_message_cleanup_response (msg);
			SOUP_MESSAGE_IO_RETURN_VAL_IF_CANCELLED_OR_PAUSED(FALSE);
		} else {
			SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
			soup_message_got_headers (msg);
			SOUP_MESSAGE_IO_RETURN_VAL_IF_CANCELLED_OR_PAUSED(FALSE);
		}
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

		return TRUE;


	case SOUP_MESSAGE_IO_STATE_DONE:
	default:
		g_return_val_if_reached (TRUE);
	}

	goto read_more;
}

