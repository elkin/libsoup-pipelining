/*
 * Copyright (C) 2012, LG Electronics
 */


#ifndef SOUP_IO_DISPATCHER_SERVER_H
#define SOUP_IO_DISPATCHER_SERVER_H 1

#include <libsoup/soup-io-dispatcher.h>

G_BEGIN_DECLS

#define SOUP_TYPE_IO_DISPATCHER_SERVER            (soup_io_dispatcher_server_get_type ())
#define SOUP_IO_DISPATCHER_SERVER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), SOUP_TYPE_IO_DISPATCHER_SERVER, SoupIODispatcherServer))
#define SOUP_IO_DISPATCHER_SERVER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), SOUP_TYPE_IO_DISPATCHER_SERVER, SoupIODispatcherServerClass))
#define SOUP_IS_IO_DISPATCHER_SERVER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), SOUP_TYPE_IO_DISPATCHER_SERVER))
#define SOUP_IS_IO_DISPATCHER_SERVER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((obj), SOUP_TYPE_IO_DISPATCHER_SERVER))
#define SOUP_IO_DISPATCHER_SERVER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), SOUP_TYPE_IO_DISPATCHER_SERVER, SoupIODispatcherServerClass))

struct _SoupIODispatcherServer {
    SoupIODispatcher parent;
};

typedef struct {
    SoupIODispatcherClass parent_class;

} SoupIODispatcherServerClass;

GType soup_io_dispatcher_server_get_type(void);

G_END_DECLS


#endif /* SOUP_IO_DISPATCHER_SERVER_H */
