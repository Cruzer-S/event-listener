#ifndef EVENT_LISTENER_H__
#define EVENT_LISTENER_H__

typedef struct event_listener *EventListener;

typedef void (*EventCallback)(int fd, void *);

EventListener event_listener_create(void);

int event_listener_add(EventListener , int , void *, EventCallback );
int event_listener_del(EventListener , int );

int event_listener_set_callback(EventListener , EventCallback );

int event_listener_start(EventListener );
int event_listener_stop(EventListener );

int event_listener_destroy(EventListener );

#endif
