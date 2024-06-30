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

#define EVENT(E, D) (&(struct epoll_event) { .events = (E), .data.ptr = (D) })
#define HANDLER(L, F) (&((L)->handler[F % (L)->n_handler]))

typedef struct event_data
{
	int fd;
	void *arg;

	struct list list;
} *EventData;

typedef struct event_handler {
	thrd_t tid;

	int epfd;
	int evfd;

	struct list event_data_list;

	bool is_running;
	bool await;

	EventCallback callback;
} *EventHandler;

struct event_listener {
	EventHandler handler;
	int n_handler;
};

static EventData event_data_create(int fd, void *arg)
{
	EventData data = malloc(sizeof(struct event_data));
	if (data == NULL)
		return NULL;

	data->fd = fd;
	data->arg = arg;

	return data;
}

static int event_handler_init(EventHandler handler) 
{
	handler->epfd = epoll_create1(0);
	if (handler->epfd == -1)
		goto RETURN_ERROR;

	handler->evfd = eventfd(0, 0);
	if (handler->evfd == -1)
		goto CLOSE_EPOLL;

	EventData data = event_data_create(handler->evfd, NULL);
	if (data == NULL)
		goto CLOSE_EVENT;

	if (epoll_ctl(handler->epfd, EPOLL_CTL_ADD, handler->evfd,
		      EVENT(EPOLLIN, data)) == -1)
		goto DESTROY_DATA;

	handler->is_running = false;
	handler->await = true;

	list_init_head(&handler->event_data_list);

	return 0;

DESTROY_DATA:	free(data);
CLOSE_EVENT:	close(handler->evfd);
CLOSE_EPOLL:	close(handler->epfd);
RETURN_ERROR:	return -1;
}

static int event_handler_cleanup(EventHandler handler)
{
	if (epoll_ctl(handler->epfd, EPOLL_CTL_DEL, handler->evfd, NULL) == -1)
		return -1;

	close(handler->evfd);
	close(handler->epfd);

	return 0;
}

static int event_handler(void *arg)
{
	EventHandler handler = arg;

	int n_events = MAX_EVENTS;
	struct epoll_event events[n_events];

	while (handler->await) sleep(1);

	while (handler->is_running)
	{
		int ret = epoll_wait(handler->epfd, events, n_events, -1);
		if (ret == -1)
			return -1;

		for (int i = 0; i < ret; i++) {
			EventData data = events[i].data.ptr;

			if (data->fd < 0)
				handler->is_running = false;

			handler->callback(data->fd, data->arg);
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

	for (int i = 0; i < MAX_HANDLER; i++) {
		if (event_handler_init(&listener->handler[i]) == -1) {
			for (int j = i - 1; j >= 0; j++)
				event_handler_cleanup(&listener->handler[j]);

			goto FREE_HANDLER;
		}
	}

	return listener;

FREE_HANDLER:	free(listener->handler);
FREE_LISTENER:	free(listener);
RETURN_NULL:	return NULL;
}

void event_listener_set_handler(
	EventListener listener, EventCallback callback)
{
	for (int i = 0; i < listener->n_handler; i++)
		HANDLER(listener, i)->callback = callback;
}

int event_listener_add(EventListener listener, int fd, void *argument)
{
	EventHandler handler = HANDLER(listener, fd);
	EventData data = event_data_create(fd, argument);

	if (data == NULL)
		return -1;

	list_add(&handler->event_data_list, &data->list);

	if (epoll_ctl(handler->epfd, EPOLL_CTL_ADD, fd,
	       	      EVENT(EPOLLIN | EPOLLET, data)) == -1)
		return -1;

	return 0;
}

int event_listener_del(EventListener listener, int fd)
{
	EventHandler handler = HANDLER(listener, fd);

	if (epoll_ctl(handler->epfd, EPOLL_CTL_DEL, fd, NULL) == -1)
		return -1;

	LIST_FOREACH_ENTRY_SAFE(&handler->event_data_list, cur,
			 	struct event_data, list)
	{
		if (cur->fd != fd)
			continue;

		list_del(&cur->list);
		free(cur);

		return 0;
	}

	return -1;
}

int event_listener_start(EventListener listener)
{
	for (int i = 0; i < listener->n_handler; i++)
	{
		EventHandler h = HANDLER(listener, i);

		h->await = true;
		if (thrd_create(&h->tid, event_handler, h) != thrd_success) {
			goto STOP_THREAD;
		}

		continue;

	STOP_THREAD:
		for (int j = i - 1; j >= 0; j--) {
			EventHandler h = HANDLER(listener, i);
			h->is_running = false;
			h->await = false;

			if (thrd_join(h->tid, NULL) != thrd_success)
				continue;
		}

		return -1;
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
		EventHandler h = HANDLER(listener, i);

		if (eventfd_write(h->evfd, 1) == -1)
			return -1;
	}

	for (int i = 0; i < listener->n_handler; i++) {
		EventHandler h = HANDLER(listener, i);

		if (thrd_join(h->tid, NULL) != thrd_success)
			return -1;
	}

	return 0;
}

int event_listener_destroy(EventListener listener)
{
	for (int i = 0; i < listener->n_handler; i++)
		if (event_handler_cleanup(&listener->handler[i]) == -1)
			return -1;

	free(listener->handler);

	free(listener);

	return 0;
}
