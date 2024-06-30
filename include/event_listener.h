#ifndef EVENT_LISTENER_H__
#define EVENT_LISTENER_H__

typedef struct event_data {
	int fd;
	void *arg;
} *EventData;

typedef struct event_listener *EventListener;

typedef void (*EventCallback)(int , void *);

EventListener event_listener_create(void);

int event_listener_add(EventListener , int , void *);
int event_listener_del(EventListener , int );

void event_listener_set_handler(EventListener , EventCallback );

int event_listener_start(EventListener );
int event_listener_stop(EventListener );

int event_listener_destroy(EventListener );

#endif
