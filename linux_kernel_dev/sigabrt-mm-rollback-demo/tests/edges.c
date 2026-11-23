// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/aio_abi.h>
#include <linux/io_uring.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define COMMAND 0x43504b54
static int report;
static atomic_long fault_epoch;
static atomic_int fault_ready;

static long control(int op, unsigned long arg)
{
	return prctl(COMMAND, (unsigned long)op, arg, 0UL, 0UL);
}

static void verify(int condition, const char *description)
{
	if (!condition) {
		dprintf(2, "FAIL edge: %s errno=%d\n", description, errno);
		_exit(1);
	}
}

static void done(void)
{
	verify(write(report, "R", 1) == 1, "completion report");
	_exit(0);
}

static void *fault_worker(void *unused)
{
	long epoch;

	atomic_fetch_add(&fault_ready, 1);
	while (!(epoch = atomic_load(&fault_epoch)))
		sched_yield();
	verify(control(2, epoch) == 0, "fault worker arrive");
	for (;;)
		sched_yield();
	return NULL;
}

static void exercise(int which)
{
	long epoch, result;
	void *mapping = NULL;
	aio_context_t aio = 0;
	struct io_uring_params params = { 0 };
	int ring = -1;
	pthread_t workers[2];
	unsigned int i;

	if (which == 0) {
		errno = 0;
		verify(control(1, 0) == -1 && errno == E2BIG, "zero members");
		errno = 0;
		verify(control(1, 65) == -1 && errno == E2BIG, "member upper bound");
		errno = 0;
		verify(control(1, 2) == -1 && errno == EAGAIN, "member mismatch");
	}
	if (which == 1) {
		mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		verify(mapping != MAP_FAILED, "prepare shared mapping");
	}
	if (which == 3) {
		verify(!seteuid(1000), "drop privilege");
		errno = 0;
		verify(control(1, 1) == -1 && errno == EPERM, "privilege rejection");
		done();
	}
	if (which == 4)
		verify(syscall(SYS_io_setup, 2, &aio) == 0, "existing AIO setup");
	if (which == 5) {
		ring = syscall(SYS_io_uring_setup, 2, &params);
		verify(ring >= 0, "existing io_uring setup");
	}
	if (which >= 6 && which <= 8) {
		for (i = 0; i < 2; i++)
			verify(!pthread_create(&workers[i], NULL, fault_worker, NULL),
			       "create fault workers");
		while (atomic_load(&fault_ready) != 2)
			sched_yield();
	}
	epoch = control(1, which >= 6 && which <= 8 ? 3 : 1);
	if ((which == 4 || which == 5) && epoch < 0) {
		verify(errno == EOPNOTSUPP, "early asynchronous resource rejection");
		if (aio)
			verify(!syscall(SYS_io_destroy, aio), "release early AIO");
		if (ring >= 0)
			close(ring);
		done();
	}
	verify(epoch > 0, "prepare epoch");
	if (which == 0) {
		errno = 0;
		verify(prctl(COMMAND, 2UL, (unsigned long)epoch, 1UL, 0UL) == -1 &&
		       errno == EINVAL, "reserved argument");
		errno = 0;
		verify(control(2, epoch + 1) == -1 && errno == ESTALE, "wrong epoch");
		verify(control(3, epoch) == 0, "cancel collection");
		done();
	}
	if (which == 2)
		sleep(6);
	if (which >= 6 && which <= 8)
		atomic_store(&fault_epoch, epoch);
	errno = 0;
	result = control(2, epoch);
	if (which == 1 || which == 4 || which == 5) {
		verify(result == -1 && errno == EOPNOTSUPP, "incompatible resource rejection");
		if (mapping)
			verify(!munmap(mapping, 4096), "release shared mapping");
		if (aio)
			verify(!syscall(SYS_io_destroy, aio), "release AIO");
		if (ring >= 0)
			close(ring);
		done();
	}
	if (which == 2 || which == 9) {
		verify(result == -1 && errno == (which == 2 ? ETIMEDOUT : ENOMEM),
		       "collection timeout/allocation failure");
		errno = 0;
		verify(control(4, epoch) == -1 && errno == ESTALE, "failed epoch released");
		done();
	}
	verify(result == 0, "fault test initial return");
	abort();
}

static void fault_stage(int stage)
{
	char value[16];
	int fd = open("/sys/kernel/debug/task_checkpoint_demo/fail_stage", O_WRONLY);
	int len = snprintf(value, sizeof(value), "%d\n", stage);

	verify(fd >= 0, "open fault stage");
	verify(write(fd, value, len) == len, "arm fault stage");
	close(fd);
}

int main(void)
{
	struct rlimit core = { 0, 0 };
	int which, passed = 0;

	setrlimit(RLIMIT_CORE, &core);
	setvbuf(stdout, NULL, _IONBF, 0);
	for (which = 0; which < 10; which++) {
		int fds[2], status;
		pid_t pid;
		char marker = 0;

		if (which >= 6)
			fault_stage(which - 5);
		verify(!pipe(fds), "edge report pipe");
		pid = fork();
		verify(pid >= 0, "edge fork");
		if (!pid) {
			close(fds[0]);
			report = fds[1];
			exercise(which);
		}
		close(fds[1]);
		verify(waitpid(pid, &status, 0) == pid, "edge wait");
		verify(read(fds[0], &marker, 1) >= 0, "edge report read");
		close(fds[0]);
		if ((which >= 6 && which <= 8 && marker == 0 &&
		     WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) ||
		    ((which < 6 || which == 9) && marker == 'R' &&
		     WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
			printf("PASS edge=%d\n", which);
			passed++;
		} else {
			printf("FAIL edge=%d status=%#x\n", which, status);
		}
		if (which >= 6) {
			int fd = open("/sys/kernel/debug/task_checkpoint_demo/fail_stage", O_RDONLY);
			char value[16] = { 0 };

			verify(fd >= 0 && read(fd, value, sizeof(value) - 1) > 0,
			       "read consumed fault");
			verify(atoi(value) == 0, "fault stage consumed");
			close(fd);
		}
	}
	printf("CKPT_EDGES %s %d/10\n", passed == 10 ? "PASS" : "FAIL", passed);
	return passed == 10 ? 0 : 1;
}
