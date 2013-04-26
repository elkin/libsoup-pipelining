/*
 * Copyright (C) 2012, LG Electronics
 */

#ifndef SOUP_IO_DISPATCHER_CLIENT_H
#define SOUP_IO_DISPATCHER_CLIENT_H 1

#include <libsoup/soup-io-dispatcher.h>

G_BEGIN_DECLS

#define SOUP_TYPE_IO_DISPATCHER_CLIENT            (soup_io_dispatcher_client_get_type ())
#define SOUP_IO_DISPATCHER_CLIENT(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), SOUP_TYPE_IO_DISPATCHER_CLIENT, SoupIODispatcherClient))
#define SOUP_IO_DISPATCHER_CLIENT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), SOUP_TYPE_IO_DISPATCHER_CLIENT, SoupIODispatcherClientClass))
#define SOUP_IS_IO_DISPATCHER_CLIENT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), SOUP_TYPE_IO_DISPATCHER_CLIENT))
#define SOUP_IS_IO_DISPATCHER_CLIENT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((obj), SOUP_TYPE_IO_DISPATCHER_CLIENT))
#define SOUP_IO_DISPATCHER_CLIENT_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), SOUP_TYPE_IO_DISPATCHER_CLIENT, SoupIODispatcherClientClass))

struct _SoupIODispatcherClient {
	SoupIODispatcher parent;
};

typedef struct {
	SoupIODispatcherClass parent_class;

} SoupIODispatcherClientClass;

GType soup_io_dispatcher_client_get_type(void);

G_END_DECLS

#endif /* SOUP_IO_DISPATCHER_CLIENT_H */
