#include "event_listener.h"

#include <stdlib.h>
#include <stdbool.h>
#include <threads.h>

#include <unistd.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>

#include "Cruzer-S/list/list.h"

#define MAX_HANDLER	8
#define MAX_EVENTS	128

#define EVENT(E, P) (&(struct epoll_event) { .events = (E), .data.ptr = (P)})
#define HANDLER(L, F) (&((L)->handler[F % (L)->n_handler]))

typedef struct event_data {
	int fd;
	int ev;
	void *ptr;
	EventCallback callback;

	bool do_close;

	struct list list;
} *EventData;

typedef struct event_handler {
	thrd_t tid;

	int epfd;
	int evfd;

	int n_event;
	struct list events;

	bool is_running;
	bool await;
} *EventHandler;

struct event_listener {
	EventHandler handler;
	int n_handler;
};

static EventHandler find_least_event_handler(EventListener listener)
{
	EventHandler least;

	if (listener->n_handler <= 0)
		return NULL;

	least = &listener->handler[0];

	for (int i = 1; i < listener->n_handler; i++)
		if (least->n_event > listener->handler[i].n_event)
			least = &listener->handler[i];

	return least;
}

static EventData find_event_data_by_fd(EventHandler handler, int fd)
{
	LIST_FOREACH_ENTRY(&handler->events, cur, struct event_data, list)
		if (cur->fd == fd)
			return cur;

	return NULL;
}

static EventHandler find_handler_by_fd(EventListener listener, int fd)
{
	for (int i = 0; i < listener->n_handler; i++)
		if (find_event_data_by_fd(&listener->handler[i], fd))
			return &listener->handler[i];

	return NULL;
}

static EventHandler event_handler_create(void) 
{
	EventHandler handler;
	int evfd, epfd;

	handler = malloc(sizeof(struct event_handler));
	if (handler == NULL)
		goto RETURN_NULL;

	epfd = epoll_create1(0);
	if (epfd == -1)
		goto FREE_HANDLER;

	evfd = eventfd(0, 0);
	if (evfd == -1)
		goto CLOSE_EPOLL;
	
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, 
	       	      evfd, EVENT(EPOLLIN, handler)) == -1)
		goto CLOSE_EVENT;

	handler->epfd = epfd;
	handler->is_running = false;
	handler->await = true;

	handler->n_event = 0;

	list_init_head(&handler->events);

	return 0;

CLOSE_EVENT:	close(evfd);
CLOSE_EPOLL:	close(epfd);
FREE_HANDLER:	free(handler);
RETURN_NULL:	return NULL;
}

static void event_handler_destroy(EventHandler handler)
{
	close(handler->evfd);
	close(handler->epfd);

	free(handler);
}

static void delete_event(EventHandler handler, EventData data)
{
	list_del(&data->list);

	epoll_ctl(handler->epfd, EPOLL_CTL_DEL, data->ev, NULL);
	epoll_ctl(handler->epfd, EPOLL_CTL_DEL, data->fd, NULL);

	close(data->ev);

	free(data);

	handler->n_event--;
}

static int event_handler(void *arg)
{
	EventHandler handler = arg;

	int n_events = MAX_EVENTS;
	struct epoll_event events[n_events];
	int epfd = handler->epfd;

	while (handler->await) sleep(1);

	while (handler->is_running) {
		int ret = epoll_wait(epfd, events, n_events, -1);
		if (ret == -1)
			return -1;

		for (int i = 0; i < ret; i++) {
			EventData data = events[i].data.ptr;
			if (data == (void *) handler) {
				handler->is_running = false;
				continue;
			}

			if (data->do_close) {
				delete_event(handler, data);
				continue;
			}

			if (data->callback != NULL)
				data->callback(data->fd, data->ptr);
		}
	}

	return 0;
}

EventListener event_listener_create(void)
{
	EventListener listener;

	listener = malloc(sizeof(struct event_listener));
	if (listener == NULL)
		goto RETURN_NULL;

	listener->n_handler = MAX_HANDLER;
	listener->handler = malloc(
		sizeof(struct event_handler) * listener->n_handler
	);
	if (listener->handler == NULL)
		goto FREE_LISTENER;

	for (int i = 0; i < listener->n_handler; i++) {
		EventHandler handler = event_handler_create();
		if (handler == NULL) {
			for (int j = i - 1; j >= 0; j--)
				event_handler_destroy(&listener->handler[j]);

			goto FREE_HANDLER;
		}
	}

	return listener;

FREE_HANDLER:	free(listener->handler);
FREE_LISTENER:	free(listener);
RETURN_NULL:	return NULL;
}

int event_listener_add(EventListener listener,
		       int fd, void *ptr, EventCallback callback)
{
	EventHandler handler = find_least_event_handler(listener);
	EventData data = malloc(sizeof(struct event_data));
	int evfd;

	if (data == NULL)
		goto RETURN_ERROR;

	evfd = eventfd(0, 0);
	if (evfd == -1)
		goto FREE_DATA;

	data->fd = fd;
	data->ev = evfd;
	data->ptr = ptr;
	data->callback = callback;

	data->do_close = false;

	if (epoll_ctl(handler->epfd, EPOLL_CTL_ADD, fd,
	       	      EVENT(EPOLLIN | EPOLLET, data)) == -1)
		goto DELETE_EVENT;

	if (epoll_ctl(handler->epfd, EPOLL_CTL_ADD, evfd,
	       	      EVENT(EPOLLIN | EPOLLET, data)) == -1)
		goto CLOSE_EVENT;

	handler->n_event++;

	list_add(&handler->events, &data->list);

	return 0;

DELETE_EVENT:	epoll_ctl(handler->epfd, EPOLL_CTL_DEL, fd, NULL);
CLOSE_EVENT:	close(data->ev);
FREE_DATA:	free(data);
RETURN_ERROR:	return -1;
}

int event_listener_del(EventListener listener, int fd)
{
	EventHandler handler = find_handler_by_fd(listener, fd);
	if (handler == NULL)
		return -1;

	EventData data = find_event_data_by_fd(handler, fd);
	if (data == NULL)
		return -1;

	data->do_close = true;
	eventfd_write(data->ev, 1);

	return 0;
}

int event_listener_start(EventListener listener)
{
	for (int i = 0; i < listener->n_handler; i++)
	{
		EventHandler h = HANDLER(listener, i);

		h->await = true;
		if (thrd_create(&h->tid, event_handler, h) != thrd_success) {
			for (int j = i - 1; j >= 0; j--) {
				EventHandler h = HANDLER(listener, i);
				h->is_running = false;
				h->await = false;

				if (thrd_join(h->tid, NULL) != thrd_success)
					continue;
			}

			return -1;
		}
	}

	for (int i = 0; i < listener->n_handler; i++) {
		HANDLER(listener, i)->is_running = true;
		HANDLER(listener, i)->await = false;
	}

	return 0;
}

int event_listener_stop(EventListener listener)
{
	for (int i = 0; i < listener->n_handler; i++) {
		EventHandler handler = HANDLER(listener, i);

		if (eventfd_write(handler->evfd, 1) == -1)
			return -1;
	}

	for (int i = 0; i < listener->n_handler; i++) {
		EventHandler handler = HANDLER(listener, i);

		if (thrd_join(handler->tid, NULL) != thrd_success)
			return -1;
	}

	return 0;
}

int event_listener_destroy(EventListener listener)
{
	for (int i = 0; i < listener->n_handler; i++)
		event_handler_destroy(&listener->handler[i]);

	free(listener->handler);
	free(listener);

	return 0;
}
