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
#include "event_queue_watcher.h"

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

typedef int (*poll_msg_handler)(msg_pool_t*);
static int deal_stdin_msg(msg_pool_t* mp);
static int deal_downstream_msg(msg_pool_t* mp);
static int deal_upstream_msg(msg_pool_t* mp);
static void on_recv_upstream_msg(msg_pool_t* mp, msg_t* msg, int id, int usleep_time);

static msg_pool_t* message_pool;

void* io_thread(void* args)
{
	struct pollfd fdset[4] = {
		{ .fd = 0, .events = POLLIN }, /* stdin */
		{ .fd = msg_pool_get_event_fd(message_pool, MSG_CHANNEL_DOWNSTREAM), .events = POLLIN },  /* eventfd down */
		{ .fd = msg_pool_get_event_fd(message_pool, MSG_CHANNEL_UPSTREAM), .events = POLLIN },  /* eventfd up */
	};
	poll_msg_handler msg_handle_fn[] = {
		deal_stdin_msg,
		deal_downstream_msg,
		deal_upstream_msg
	};
	int i;
	for (i = 0; i < 3; i++) {
		if (fdset[i].fd < 0) {
			fdset[i].events = fdset[i].revents = 0;
		} else {
			printf("[io] watch on fd [%d]\n", fdset[i].fd);
		}
	}
	while (1) {
		if (poll(fdset, 3, -1) <= 0) {
			perror("poll");
			continue;
		}
		for (i = 0; i < 3; i++) {
			if (fdset[i].revents & POLLIN) {
				msg_handle_fn[i](message_pool);
			}
			if (fdset[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { /* err:0x8, hup:0x10, nval:0x20 */
				/* usually have POLLNVAL closed stdin elsewhere;
				 * hup if stdin is a pipe and the other peer closed */
				printf("[io] fd %d recv event %x, close\n", fdset[i].fd, fdset[i].revents);
				close(fdset[i].fd);
				fdset[i].fd = -1;
				fdset[i].events = fdset[i].revents = 0;
			}
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

	struct equeue_signal_watcher watcher = {
		.signo = SIGRTMAX-1,
		.limit = { 0, 10 },
		.dylimit_inc = 3,
		.dylimit_max = -1,
	};

	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, watcher.signo);
	sigprocmask(SIG_SETMASK, &set, NULL);

	event_queue_register_watcher(&message_pool->equeue[MSG_CHANNEL_UPSTREAM],
				     &watcher, equeue_signal_watcher_cb);
	
	pthread_t ioth, workerth[MAX_N_WORKERS];
	pthread_create(&ioth, NULL, io_thread, NULL);
	int i;
	for (i = 0; i < nr_workers; i++) {
		pthread_create(&workerth[i], NULL, worker_thread, (void*)(long)i);
	}

	printf("[main]: started. waiting SIGINT to exit\n");
	siginfo_t info;
	int st;
	while (!(sigwaitinfo(&set, &info) < 0)) {
		if (info.si_signo == watcher.signo) {
			if (info.si_int < 0) {
				printf("[main]: equeue[UP] empty\n");
			} else {
				printf("[main]: equeue[UP] exceed %d\n", info.si_int);
				if (nr_workers < MAX_N_WORKERS) {
					pthread_create(&workerth[nr_workers], NULL, worker_thread, (void*)(long)nr_workers);
					printf("[main]: start up new worker thread %d\n", nr_workers);	
					nr_workers++;
				}
			}
		} else if (info.si_signo == SIGINT) {
			printf("[main]: recv SIGINT, close all input and exit\n");
			break;	/* exit  */
		} else {
			printf("[main]: ERROR recv none-waited signal %d, errno %d\n", info.si_signo, info.si_errno);
		}
	}
	
	close(0);
	sleep(2);
	printf("[main]: cancel and join threads\n");
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

	io_thread(NULL);
	
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
		/* close(0); /\* we can still read stdin after recv EOF *\/ */
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
