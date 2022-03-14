/*
 * Build with

gcc cyclictest.c -lpthread

 * or

musl-gcc cyclictest.c -lpthread -DUSE_MUSL

 *
 * NOTE THAT FOR ACCURATE RESULTS    /dev/cpu_dma_latency    NEEDS TO BE SET TO 0.
 * See Documentation/power/pm_qos_interface.txt .
 *
 * DEFINES: USE_MUSL    MINIMAL
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <assert.h>
#include <fcntl.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/syscall.h>

/*
 * number of timerthreads
 */
#define NUM_THREADS 1
#define MAX_CPUS 12
#define DEFAULT_INTERVAL 1000 // in usecs
#define DEFAULT_DISTANCE 500
#define DEFAULT_PRIORITY 0
#define DEFAULT_POLICY SCHED_OTHER // SCHED_FIFO
#define USEC_PER_SEC 1000000
#define NSEC_PER_SEC 1000000000
#define DEFAULT_CLOCK CLOCK_MONOTONIC
#define MAX_CYCLES 5000

struct thread_param {
	int id;
	pthread_t thread;
	unsigned long interval;
	int prio;
	int policy;
	int cpu; // which cpu to run on
};

struct thread_stat {
	int tid;
	long max;
	long min;
	long act;
	long sum; // not using `double avg`
	int cycles;
};

static int interval = DEFAULT_INTERVAL;
static int priority = DEFAULT_PRIORITY;
static struct thread_param thrpar[NUM_THREADS];
static struct thread_stat thrstat[NUM_THREADS];
static int shutdown = 0;

static void set_latency_target(void)
{
	struct stat s;
	int err;
	int latency_target_fd;
	int latency_target_value = 0;

	errno = 0;
	err = stat("/dev/cpu_dma_latency", &s);
	if (err == -1) {
		printf("WARN: stat /dev/cpu_dma_latency failed\n");
		return;
	}

	errno = 0;
	latency_target_fd = open("/dev/cpu_dma_latency", O_RDWR);
	if (latency_target_fd == -1) {
		printf("WARN: open /dev/cpu_dma_latency\n");
		return;
	}

	errno = 0;
	err = write(latency_target_fd, &latency_target_value, 4);
	if (err < 1) {
		printf("# error setting cpu_dma_latency to %d!\n", latency_target_value);
		close(latency_target_fd);
		return;
	}
	printf("# /dev/cpu_dma_latency set to %dus\n", latency_target_value);
}

static inline void tsnorm(struct timespec *ts)
{
	while (ts->tv_nsec >= NSEC_PER_SEC) {
		ts->tv_nsec -= NSEC_PER_SEC;
		ts->tv_sec++;
	}
}

static inline void tsinc(struct timespec *dst, const struct timespec *delta)
{
	dst->tv_sec += delta->tv_sec;
	dst->tv_nsec += delta->tv_nsec;
	tsnorm(dst);
}

// delta in usecs
static inline long tsdelta(const struct timespec *t1, const struct timespec *t2)
{
	int64_t diff =
		(long)USEC_PER_SEC * ((long)t1->tv_sec - (long)t2->tv_sec);
	diff += ((long)t1->tv_nsec - (long)t2->tv_nsec) / 1000;
	return diff;
}

static inline int tsgreater(struct timespec *a, struct timespec *b)
{
	return ((a->tv_sec > b->tv_sec) ||
		(a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec));
}

static void *timerthread(void *param)
{
	int err;

	// init
	struct thread_param *par = param;
	struct thread_stat *stat = &thrstat[par->id];
#ifdef USE_MUSL
	// musl does not have gettid
	stat->tid = 0;
#else
	stat->tid = gettid();
#endif

	// WHY BLOCK SIGALRM
	// XXX

	// priority
#ifndef USE_MUSL
	struct sched_param schedp;
	memset(&schedp, 0, sizeof(schedp));
	schedp.sched_priority = par->prio; // 0
	// printf("%d %d\n", par->policy, schedp.sched_priority);
	err = sched_setscheduler(0, par->policy, &schedp);
	assert(!err);
#endif

	// affinity
	// cpu_set_t mask;
	// CPU_ZERO(&mask);
	// CPU_SET(par->cpu, &mask);
	// err = pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
	// assert(!err && "cannot pthread_setaffinity_np");

	// INIT TIMER
	struct timespec now, next, interval;
	interval.tv_sec = par->interval / USEC_PER_SEC;
	interval.tv_nsec = (par->interval % USEC_PER_SEC) * 1000;

	err = clock_gettime(DEFAULT_CLOCK, &now);
	assert(!err);
	next = now;
	tsinc(&next, &interval);

	while (!shutdown) {
		err = clock_nanosleep(DEFAULT_CLOCK, TIMER_ABSTIME, &next,
				      NULL);
		if (err == EINTR)
			break;
		assert(!err && "cannot clock_nanosleep");

		err = clock_gettime(DEFAULT_CLOCK, &now);
		assert(!err);

		long diff = tsdelta(&now, &next);

		if (diff < stat->min)
			stat->min = diff;
		if (diff > stat->max)
			stat->max = diff;
		stat->act = diff;
		stat->sum += diff;
		stat->cycles++;

		tsinc(&next, &interval);
		while (tsgreater(&now, &next))
			tsinc(&next, &interval);
	}
}

static void sighand(int sig)
{
	shutdown = 1;
}

static void print_stat(FILE *fp, struct thread_param *par,
		       struct thread_stat *stat)
{
	int index = par->id;

	char *fmt = "T:%2d (%5d) P:%2d I:%ld C:%7lu "
		    "Min:%7ld Act:%5ld Avg:%5ld Max:%8ld";

	fprintf(fp, fmt, index, stat->tid, par->prio, par->interval,
		stat->cycles, stat->min, stat->act,
		stat->cycles ? (long)(stat->sum / stat->cycles) : 0, stat->max);

	// fprintf(fp, "\n");
	fprintf(fp, "\n"); // reuse the same line
}

int main()
{
	int err;

#ifndef MINIMAL
	signal(SIGINT, sighand);
	set_latency_target();
#endif

	for (int i = 0; i < NUM_THREADS; i++) {
		struct thread_param *par = &thrpar[i];
		struct thread_stat *stat = &thrstat[i];
		par->id = i;
		par->cpu = i % MAX_CPUS;
		par->prio = priority;
		par->policy = DEFAULT_POLICY;
		par->interval = interval;
		interval += DEFAULT_DISTANCE;

		stat->min = 10000;
		stat->max = 0;
		stat->sum = 0;
		stat->cycles = 0;

		err = pthread_create(&par->thread, NULL, timerthread, par);
		assert(!err && "cannot pthread_create");
	}

	while (!shutdown) {
		int allstopped = 0;
		for (int i = 0; i < NUM_THREADS; i++) {
			print_stat(stdout, &thrpar[i], &thrstat[i]);
			if (thrstat[i].cycles >= MAX_CYCLES)
				allstopped++;
		}

		usleep(10000);
		if (shutdown || allstopped)
			break;
		printf("\033[%dA", NUM_THREADS);
	}
	shutdown = 1;

	for (int i = 0; i < NUM_THREADS; i++) {
		pthread_join(thrpar[i].thread, NULL);
	}
}
