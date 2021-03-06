/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2000-2003, Ximian, Inc.
 */

#ifndef SOUP_MESSAGE_PRIVATE_H
#define SOUP_MESSAGE_PRIVATE_H 1

#include "soup-message.h"
#include "soup-auth.h"
#include "soup-content-sniffer.h"

typedef struct {
	gpointer                 io_data;
	SoupIODispatcher        *io_disp;

	SoupChunkAllocator       chunk_allocator;
	gpointer                 chunk_allocator_data;
	GDestroyNotify           chunk_allocator_dnotify;

	guint                    msg_flags;
	gboolean                 server_side;

	SoupContentSniffer      *sniffer;
	gsize                    bytes_for_sniffing;

	SoupHTTPVersion          http_version, orig_http_version;

	SoupURI                 *uri;
	SoupAddress             *addr;

	SoupAuth                *auth, *proxy_auth;

	GSList                  *disabled_features;
	GSList                  *decoders;

	SoupURI                 *first_party;

	GTlsCertificate         *tls_certificate;
	GTlsCertificateFlags     tls_errors;
} SoupMessagePrivate;
#define SOUP_MESSAGE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), SOUP_TYPE_MESSAGE, SoupMessagePrivate))

void             soup_message_cleanup_response (SoupMessage      *req);


typedef void     (*SoupMessageGetHeadersFn)  (SoupMessage      *msg,
					      GString          *headers,
					      SoupEncoding     *encoding,
					      gpointer          user_data);
typedef guint    (*SoupMessageParseHeadersFn)(SoupMessage      *msg,
					      char             *headers,
					      guint             header_len,
					      SoupEncoding     *encoding,
					      gpointer          user_data);
typedef void     (*SoupMessageCompletionFn)  (SoupMessage      *msg,
					      gpointer          user_data);

/* Auth handling */
void           soup_message_set_auth       (SoupMessage *msg,
					    SoupAuth    *auth);
SoupAuth      *soup_message_get_auth       (SoupMessage *msg);
void           soup_message_set_proxy_auth (SoupMessage *msg,
					    SoupAuth    *auth);
SoupAuth      *soup_message_get_proxy_auth (SoupMessage *msg);

gboolean soup_message_disables_feature (SoupMessage *msg,
					gpointer     feature);

void soup_message_set_https_status (SoupMessage    *msg,
				    SoupConnection *conn);

void soup_message_network_event (SoupMessage         *msg,
				 GSocketClientEvent   event,
				 GIOStream           *connection);

#endif /* SOUP_MESSAGE_PRIVATE_H */
