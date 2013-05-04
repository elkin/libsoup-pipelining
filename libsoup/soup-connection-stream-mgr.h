/*
 * soup-connection-stream-mgr.h
 *
 *  Created on: 05.05.2013
 *      Author: vladimir
 */

#ifndef SOUP_CONN_STREAM_MGR_H_
#define SOUP_CONN_STREAM_MGR_H_ 1

#include <libsoup/soup-types.h>

G_BEGIN_DECLS

#define SOUP_TYPE_CONNECTION_STREAM_MGR            (soup_conn_stream_mgr_get_type ())
#define SOUP_CONN_STREAM_MGR(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), SOUP_TYPE_CONNECTION_STREAM_MGR, SoupConnectionStreamMgr))
#define SOUP_CONN_STREAM_MGR_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), SOUP_TYPE_CONNECTION_STREAM_MGR, SoupConnectionStreamMgrClass))
#define SOUP_IS_CONNECTION_STREAM_MGR(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), SOUP_TYPE_CONNECTION_STREAM_MGR))
#define SOUP_IS_CONNECTION_STREAM_MGR_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((obj), SOUP_TYPE_CONNECTION_STREAM_MGR))
#define SOUP_CONN_STREAM_MGR_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), SOUP_TYPE_CONNECTION_STREAM_MGR, SoupConnectionStreamMgrClass))


struct _SoupConnStreamMgr {
	GObject parent;

};

typedef struct {
	GObjectClass parent_class;

} SoupConnStreamMgrClass;

typedef SoupConnStream* (*AllocConnStreamFn)(SoupConnStreamMgr *mgr, const char *host);
typedef SoupConnStream* (*GetConnStreamFn) (SoupConnStreamMgr *mgr, const char *host);

SoupConnStream* soup_conn_stream_mgr_get_connection_stream(SoupConnStreamMgr *mgr,
    const char *host, SoupProtocolVersion protocol, GCallback callback);

void soup_conn_stream_mgr_set_max_connection_stream_count(SoupConnStreamMgr *mgr,
    const char *host, SoupProtocolVersion protocol, int max_conn_stream_count);

int soup_conn_stream_mgr_get_max_conn_stream_count(SoupConnStreamMgr *mgr,
    const char *host, SoupProtocolVersion protocol);

void soup_conn_stream_mgr_set_max_pending_connection_streams(SoupConnStreamMgr *mgr,
    const char *host, SoupProtocolVersion, int msgs_per_conn);

int  soup_conn_stream_mgr_get_max_pending_connection_streams(SoupConnStreamMgr *mgr,
    const char *host, SoupProtocolVersion);


void soup_conn_stream_mgr_set_max_idle_time (SoupConnStreamMgr *mgr,
    const char *host, SoupProtocolVersion, int ms);

int  soup_conn_stream_mgr_get_max_idle_time (SoupConnStreamMgr *mgr,
    const char *host, SoupProtocolVersion);

void soup_conn_stream_mgr_set_max_pooled_connection_streams (SoupConnStreamMgr *mgr,
    const char *host, SoupProtocolVersion protocol, int count);

int  soup_conn_stream_mgr_get_max_pooled_connection_streams (SoupConnStreamMgr *mgr,
    const char *host, SoupProtocolVersion protocol);

void soup_conn_stream_mgr_set_alloc_strategy (SoupConnStreamMgr *mgr,
    AllocConnStreamFn strategy);

void soup_conn_stream_mgr_set_reuse_strategy (SoupConnStreamMgr *mgr,
    GetConnStreamFn strategy);


#endif /* SOUP_CONN_STREAM_MGR_H_ */
