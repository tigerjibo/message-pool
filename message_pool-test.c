#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <assert.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <getopt.h>

#include "message_pool.h"

/* config */
#define MAX_MSG_SIZE	4
#define MAX_N_WORKERS	10
int serv_time_max_us = 0;
int nr_workers = 0;

void parse_args(int argc, char* argv[]) 
{
	while (1) {
		switch (getopt(argc, argv, "n:s:")) {
		case 'n': nr_workers = atoi(optarg); break;
		case 's': serv_time_max_us = atoi(optarg) * 1000; break; /* in ms */
		case -1: goto end;
		default:
			fprintf(stderr, "Usage: %s [-s service-max-time-ms] [nr-worker-threads]\n", argv[0]);
			exit(EXIT_FAILURE);
		}
	}
end:
	if (nr_workers > MAX_N_WORKERS) {
		nr_workers = MAX_N_WORKERS;
	}
	return;
}

/* message define */
typedef struct msg_s {
	int datalen;
	char data[0];
} msg_t;
#define MSG_SIZE(datalen)	(sizeof(msg_t) + (datalen))

msg_t* alloc_msg(msg_pool_t* mp, int datalen)
{
	msg_t* msg = msg_pool_alloc(mp, MSG_SIZE(datalen));
	if (msg) msg->datalen = datalen;
	return msg;
}
msg_t* make_msg(msg_pool_t* mp, const char* str)
{
	msg_t* msg = alloc_msg(mp, strlen(str) + 1);
	if (msg) memcpy(msg->data, str, msg->datalen);
	return msg;
}
void free_msg(msg_pool_t* mp, msg_t* msg)
{
	if (msg) {
		msg_pool_free(mp, msg, MSG_SIZE(msg->datalen));
	}
}
#define MSG_FMT	"%p/%d: %s"
#define MSG_STR(msg)	msg, msg->datalen, msg->data

static int deal_stdin_msg(msg_pool_t* mp);
static int deal_downstream_msg(msg_pool_t* mp);
static int deal_upstream_msg(msg_pool_t* mp);
static void on_recv_upstream_msg(msg_pool_t* mp, msg_t* msg, int id, int usleep_time);

static msg_pool_t* message_pool;

void* io_thread(void* args)
{
	struct pollfd fdset[2] = {
		{ .fd = 0, .events = POLLIN }, /* stdin */
		{ .fd = -1, .events = POLLIN },  /* eventfd */
	};
	if ((fdset[1].fd = msg_pool_get_event_fd(message_pool, MSG_CHANNEL_DOWNSTREAM)) < 0) {
		fprintf(stderr, "[io] not eventfd to wait on\n");
		return NULL;
	}
	while (1) {
		if (poll(fdset, 2, -1) <= 0) {
			perror("poll");
			continue;
		}
		if (fdset[0].revents & POLLIN) {
			deal_stdin_msg(message_pool);
		}
		if (fdset[1].revents & POLLIN) {
			deal_downstream_msg(message_pool);
		}
	}
}

void* timer_thread(void* args);

void cleanup(void *arg) {
	pthread_mutex_t* lock = arg;
	pthread_mutex_unlock(lock);
}
void* worker_thread(void* args)
{
	int id = (long)args;
	char buff[MAX_MSG_SIZE];
	pthread_cleanup_push(cleanup, &message_pool->equeue[MSG_CHANNEL_UPSTREAM].lock);
	msg_t* msg;
	while (1) {
 		if (msg_pool_wait(message_pool, MSG_CHANNEL_UPSTREAM, &msg) < 0) {
			perror("[worker] msg_pool_wait(UP)");
			continue;
		}
		on_recv_upstream_msg(message_pool, msg, id, serv_time_max_us);
	}
	pthread_cleanup_pop(1);
}

int multi_thread_test(int nr_workers)
{
	struct msg_pool_cfg mpcfg = {
		.use_event_fd = { 0, 1 },
		.allocator_cfg = { MAX_MSG_SIZE },
	};
	if ((message_pool = msg_pool_new(&mpcfg)) == NULL) {
		perror("msg_pool_new");
		return 1;
	}

	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigprocmask(SIG_SETMASK, &set, NULL);

	pthread_t ioth, workerth[MAX_N_WORKERS];
	pthread_create(&ioth, NULL, io_thread, NULL);
	int i;
	for (i = 0; i < nr_workers; i++) {
		pthread_create(&workerth[i], NULL, worker_thread, (void*)(long)i);
	}

	printf("[main]: started. waiting SIGINT to exit\n");
	sigwaitinfo(&set, NULL);
	printf("[main]: recv SIGINT, cancel and join threads\n");
	
	/* for (--i; i >= 0; --i) { */
	for (i = 0; i < nr_workers; i++) {
		pthread_cancel(workerth[i]);
		pthread_join(workerth[i], NULL);
		printf("[main] joined [%d]\n", i);
	}
	pthread_cancel(ioth);
	pthread_join(ioth, NULL);
	printf("[main]: all threads joined\n");

	msg_pool_del(message_pool);
	return 0;
}

int single_thread_test()
{
	struct msg_pool_cfg mpcfg = {
		.use_event_fd = { 1, 1 },
		.allocator_cfg = { MAX_MSG_SIZE },
	};
	if ((message_pool = msg_pool_new(&mpcfg)) == NULL) {
		perror("msg_pool_new");
		return 1;
	}

	struct pollfd fdset[4] = {
		{ .fd = 0, .events = POLLIN }, /* stdin */
		{ .fd = msg_pool_get_event_fd(message_pool, MSG_CHANNEL_DOWNSTREAM), .events = POLLIN },  /* eventfd down */
		{ .fd = msg_pool_get_event_fd(message_pool, MSG_CHANNEL_UPSTREAM), .events = POLLIN },  /* eventfd up */
	};
	while (1) {
		if (poll(fdset, 3, -1) <= 0) {
			perror("poll");
			continue;
		}
		if (fdset[0].revents & POLLIN) {
			deal_stdin_msg(message_pool);
		}
		if (fdset[1].revents & POLLIN) {
			deal_downstream_msg(message_pool);
		}
		if (fdset[2].revents & POLLIN) {
			deal_upstream_msg(message_pool);
		}
	}
	
	msg_pool_del(message_pool);
}

int main(int argc, char* argv[])
{
	parse_args(argc, argv);
	printf("[main]: %d worker ths, %d us max delay\n", nr_workers, serv_time_max_us);
	
	if (fcntl(0, F_SETFL, O_NONBLOCK) < 0)  {
		perror("fcntl(stdin, O_NONBLOCK)");
		return 1;
	}

	return (nr_workers > 0) ?
		multi_thread_test(nr_workers) :
		single_thread_test();
}

static int deal_stdin_msg(msg_pool_t* mp)
{
	char buff[MAX_MSG_SIZE];
	int len;
	int eof = 1;
	while ((len = read(0, &buff, sizeof(buff))) > 0) {
		eof = 0;
		buff[len - 1] = '\0'; /* replace end char (\n if input no longer than MAX_MSG_SIZE) to \0 */
		msg_t* msg = make_msg(mp, buff);
		if (!msg) {
			perror("[io] make_msg");
			continue;
		}
		printf("[io] S U " MSG_FMT "\n", MSG_STR(msg));
		msg_pool_post(message_pool, MSG_CHANNEL_UPSTREAM, msg);
	}
	if (eof) {
		fprintf(stderr, "[io] read(stdin) EOF\n");
	} else if (errno != EAGAIN) {
		perror("[io] read(stdin) ERR");
	}
	return 0;
}

static int deal_downstream_msg(msg_pool_t* mp)
{
	int fd = msg_pool_get_event_fd(mp, MSG_CHANNEL_DOWNSTREAM);
	while (msg_pool_efd_trywait(fd) >= 0) {
		msg_t* msg;
		if (msg_pool_trywait(message_pool, MSG_CHANNEL_DOWNSTREAM, &msg) < 0) {
			perror("[io] msg_pool_trywait() when wait_efd(DOWN) succeed");
			continue;
		}
		printf("[io] R D " MSG_FMT "\n", MSG_STR(msg));
		free_msg(message_pool, msg);
		printf("\n");
	}
	if (errno != EAGAIN) {
		perror("[io] read(efd[DOWN]) ERR");
	}
	return 0;
}

static void str_to_upper(char* dst, const char* src)
{
	while (*src) *dst++ = toupper(*src++);
	*dst ='\0';
}

static void on_recv_upstream_msg(msg_pool_t* mp, msg_t* rmsg, int id, int usleep_maxtime)
{
	msg_t* smsg;
	printf("[%d] R U " MSG_FMT "\n", id, MSG_STR(rmsg));
	/* generate */
	smsg = alloc_msg(message_pool, rmsg->datalen);
	str_to_upper(smsg->data, rmsg->data);
	/* consume time */
	if (usleep_maxtime) {
		usleep(rand() % usleep_maxtime);
	}
	free_msg(message_pool, rmsg);
	/* send */
	printf("[%d] S D " MSG_FMT "\n", id, MSG_STR(smsg));
	msg_pool_post(message_pool, MSG_CHANNEL_DOWNSTREAM, smsg);
}

int deal_upstream_msg(msg_pool_t* mp)
{
	int fd = msg_pool_get_event_fd(mp, MSG_CHANNEL_UPSTREAM);
	msg_t *msg;
	while (msg_pool_efd_trywait(fd) >= 0) {
		/* recv */
		if (msg_pool_trywait(message_pool, MSG_CHANNEL_UPSTREAM, &msg) < 0) {
			perror("[io] msg_pool_trywait() when wait_efd(DOWN) succeed");
			continue;
		}
		on_recv_upstream_msg(message_pool, msg, 0, serv_time_max_us);
	}
	if (errno != EAGAIN) {
		perror("[io] read(efd[DOWN]) ERR");
	}
}
