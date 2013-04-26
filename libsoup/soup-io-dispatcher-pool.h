/*
 * Copyright (C) 2012, LG Electronics
 */

#ifndef SOUP_IO_DISPATCHER_POOL_H
#define SOUP_IO_DISPATCHER_POOL_H 1

#include <libsoup/soup-types.h>

#define SOUP_TYPE_IO_DISPATCHER_POOL            (soup_io_dispatcher_pool_get_type ())
#define SOUP_IO_DISPATCHER_POOL(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), SOUP_TYPE_IO_DISPATCHER_POOL, SoupIODispatcherPool))
#define SOUP_IO_DISPATCHER_POOL_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), SOUP_TYPE_IO_DISPATCHER_POOL, SoupIODispatcherPoolClass))
#define SOUP_IS_IO_DISPATCHER_POOL(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), SOUP_TYPE_IO_DISPATCHER_POOL))
#define SOUP_IS_IO_DISPATCHER_POOL_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((obj), SOUP_TYPE_IO_DISPATCHER_POOL))
#define SOUP_IO_DISPATCHER_POOL_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), SOUP_TYPE_IO_DISPATCHER_POOL, SoupIODispatcherPoolClass))

struct _SoupIODispatcherPool{
	GObject parent;
};

typedef struct {
	GObjectClass parent;
	SoupIODispatcher* (*get_io_dispatcher) (SoupIODispatcherPool *io_disp_pool,
							  SoupMessage *msg,
							  gboolean via_https,
							  gboolean via_proxy);
} SoupIODispatcherPoolClass;

GType soup_io_dispatcher_pool_get_type (void);

#define SOUP_IO_DISPATCHER_POOL_IDLE_TIMEOUT		   "idle-timeout"
#define SOUP_IO_DISPATCHER_POOL_IS_THREAD_SAFE		   "is-thread-safe"
#define SOUP_IO_DISPATCHER_POOL_MAKE_ALL_CONNS_FIRSTLY "make-all-conns-firstly"
#define SOUP_IO_DISPATCHER_POOL_MAX_IO_DISPS           "max-io-disps"
#define SOUP_IO_DISPATCHER_POOL_MAX_IO_DISPS_PER_HOST  "max-io-disps-per-host"
#define SOUP_IO_DISPATCHER_POOL_MAX_PIPELINED_MSGS     "max-pipelined-msgs"
#define SOUP_IO_DISPATCHER_POOL_PIPELINE_VIA_PROXY     "pipeline-via-proxy"
#define SOUP_IO_DISPATCHER_POOL_PIPELINE_VIA_HTTPS     "pipeline-via-https"
#define SOUP_IO_DISPATCHER_POOL_RESPONSE_BLOCK_SIZE    "response-block-size"
#define SOUP_IO_DISPATCHER_POOL_USE_FIRST_AVAIL_CONN   "use-first-avail-conn"
#define SOUP_IO_DISPATCHER_POOL_USE_CACHE              "use-cache"

SoupIODispatcherPool *soup_io_dispatcher_pool_new				              (
							  const char*           propname1, ...) G_GNUC_NULL_TERMINATED;
void                  soup_io_dispatcher_pool_enable_spdy_support             (
							  SoupIODispatcherPool *io_disp_pool,
							  const char 			*host,
							  int   				 version);

void 				  soup_io_dispatcher_pool_set_max_pipelined_msgs_for_host (
							  SoupIODispatcherPool *io_disp_pool,
							  const char *host,
							  int max_queue_length);

SoupIODispatcher*     soup_io_dispatcher_pool_alloc_io_dispatcher             (
							  SoupIODispatcherPool *pool,
							  SoupURI				*uri,
							  GMainContext         *async_context,
							  SoupConnection       *conn,
							  gboolean             via_proxy);
SoupConnection*       soup_io_dispatcher_pool_get_conn                        (
							  SoupIODispatcherPool *io_disp_pool,
							  SoupIODispatcher *io_disp);
SoupIODispatcher*     soup_io_dispatcher_pool_get_io_dispatcher               (
							  SoupIODispatcherPool *pool,
							  SoupMessage			*msg,
							  gboolean				via_https,
							  gboolean 			via_proxy);

//TODO: M.b. add some info functions
#endif /* SOUP_IO_DISPATCHER_POOL_H */
