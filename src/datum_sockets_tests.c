#include "datum_sockets.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <assert.h>

static uint64_t now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000ull;
}

/* Functional check that a write to the eventfd wakes an epoll_wait(-1).
 * Prints measured wake time but does not fail on it. */
static void test_eventfd_wake_latency(void) {
	int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	assert(efd >= 0);

	int epfd = epoll_create1(EPOLL_CLOEXEC);
	assert(epfd >= 0);

	struct epoll_event wev = { .events = EPOLLIN, .data.u64 = (uint64_t)-1 };
	assert(epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &wev) == 0);

	/* One measured wake */
	uint64_t start = now_us();
	uint64_t one = 1;
	assert(write(efd, &one, sizeof(one)) == (ssize_t)sizeof(one));

	struct epoll_event ev;
	int nfds = epoll_wait(epfd, &ev, 1, 1000);
	assert(nfds == 1);
	uint64_t elapsed = now_us() - start;

	printf("eventfd wake latency: %llu us\n", (unsigned long long)elapsed);

	assert(nfds == 1);
	assert(ev.data.u64 == (uint64_t)-1);

	/* Drain so a second write is not coalesced into an already-ready fd */
	uint64_t v;
	while (read(efd, &v, sizeof(v)) > 0) {}

	close(efd);
	close(epfd);
}

/* Same measurement, but the write happens from another thread while we are
 * blocked in epoll_wait. Prints the time; asserts only that the wake occurred. */
static void *writer_thread(void *arg) {
	int efd = *(int *)arg;
	usleep(20000); /* let the waiter reach epoll_wait */
	uint64_t one = 1;
	write(efd, &one, sizeof(one));
	return NULL;
}

static void test_eventfd_wake_from_other_thread(void) {
	int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	assert(efd >= 0);

	int epfd = epoll_create1(EPOLL_CLOEXEC);
	assert(epfd >= 0);

	struct epoll_event wev = { .events = EPOLLIN, .data.u64 = (uint64_t)-1 };
	assert(epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &wev) == 0);

	pthread_t th;
	pthread_create(&th, NULL, writer_thread, &efd);

	uint64_t start = now_us();
	struct epoll_event ev;
	int nfds = epoll_wait(epfd, &ev, 1, 1000);
	assert(nfds == 1);
	uint64_t elapsed = now_us() - start;

	printf("eventfd cross-thread wake latency: %llu us\n", (unsigned long long)elapsed);

	assert(nfds == 1);
	assert(ev.data.u64 == (uint64_t)-1);

	pthread_join(th, NULL);
	close(efd);
	close(epfd);
}

void datum_sockets_eventfd_tests(void) {
	test_eventfd_wake_latency();
	test_eventfd_wake_from_other_thread();
}
