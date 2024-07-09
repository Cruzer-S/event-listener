#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <threads.h>

#include <unistd.h>
#include <fcntl.h>

#include "event_listener.h"

#define err(MSG) perror(MSG), exit(EXIT_FAILURE)

#define PIPE_SIZE	100
#define MAX_THREADS	8

EventListener listener;

int remain = PIPE_SIZE;

int write_fd[PIPE_SIZE];
int read_fd[PIPE_SIZE];

const char message[] = "hello, world!";

struct argument {
	thrd_t tid;

	int *fds;
	int nfd;
};

static int get_random_fd_index(void)
{
	int start = rand() % PIPE_SIZE;

	for (int i = start; i < PIPE_SIZE; i++) {
		if (write_fd[i] > 0)
			return i;
	}

	for (int i = 0; i < start; i++)
		if (write_fd[i] > 0)
			return i;

	return -1;
}

int closer(void *__)
{
	while (remain > 0) {
		int idx = get_random_fd_index();
		if (idx == -1)
			break;

		fprintf(stderr, "\tclose %d! (%d)\n", write_fd[idx], --remain);
		close(write_fd[idx]); write_fd[idx] = -1;

		usleep(100000);
	}

	fprintf(stderr, "\t\tcloser end!\n");

	return 0;
}

int producer(void *argument)
{
	struct argument *arg = argument;

	while (remain > 0) {
		for (int i = 0; i < arg->nfd; i++) {
			if (arg->fds[i] == -1)
				continue;

			int len = write(arg->fds[i], message, sizeof(message));
			if (len == -1)
				exit(EXIT_FAILURE);
		}
			
		usleep(10000);
	}

	fprintf(stderr, "\t\tproducer end!\n");

	return 0;
}

void callback(int fd, void *arg)
{
	char buffer[BUFSIZ];

	int len = read(fd, buffer, BUFSIZ);
	if (len == -1) {
		if (errno == EAGAIN)
			return ;

		err("failed to read(): ");
	}

	if (len == 0) {
		fprintf(stderr, "[%05ld][%d] close request!\n", thrd_current() % 10000, fd);
		event_listener_del(listener, fd);
		close(fd);

		return ;
	}

	fprintf(stderr, "[%05ld][%d] %.*s\n", thrd_current() % 10000, fd, len, buffer);
}

int main(void)
{
	struct argument arguments[MAX_THREADS];
	
	listener = event_listener_create();
	if (listener == NULL)
		err("failed to event_listener_create(): ");
	
	for (int i = 0; i < PIPE_SIZE; i++) {
		int pipe_fd[2]; // 0 read, 1 write

		if (pipe(pipe_fd) == -1)
			err("failed to pipe(): ");

		int flags = fcntl(pipe_fd[0], F_GETFD);
		if (flags == -1)
			err("failed to fcntl()");

		if (fcntl(pipe_fd[0], F_SETFD, O_NONBLOCK) == -1)
			err("failed to fcntl()");

		read_fd[i] = pipe_fd[0];
		write_fd[i] = pipe_fd[1];

		event_listener_add(listener, read_fd[i], NULL, callback);
	}

	event_listener_start(listener);

	for (int i = 0; i < MAX_THREADS; i++) {
		int nfd = PIPE_SIZE / MAX_THREADS;

		arguments[i].nfd = nfd;
		arguments[i].fds = &write_fd[nfd * i];

		thrd_create(&arguments[i].tid, producer, &arguments);
	}

	thrd_t t;
	thrd_create(&t, closer, NULL);

	thrd_join(t, NULL);
	for (int i = 0; i < MAX_THREADS; i++)
		thrd_join(arguments[i].tid, NULL);

	event_listener_stop(listener);

	event_listener_destroy(listener);

	return 0;
}
