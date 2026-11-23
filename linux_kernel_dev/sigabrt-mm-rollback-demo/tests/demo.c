// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <gnu/libc-version.h>
#include <linux/futex.h>

#define PR_TASK_CHECKPOINT_DEMO 0x43504b54
#define PREPARE 1
#define ARRIVE 2
#define DROP 3
#define STATUS 4
#define CASE_COUNT 14

static atomic_long epoch;
static atomic_int ready;
static atomic_int restored;
static atomic_int phase;
static atomic_int sleeping;
static atomic_int futex_word;
static int tids[3];
static pthread_t workers[2];
static __thread volatile int tls_value;
static volatile int global_value = 123;
static int *heap_value;
static int test_mode;
static int report_fd;
static int *mapped_value;

static inline void fp_write(uint64_t value)
{
	__asm__ volatile("fmov d8, %0" : : "r" (value) : "v8");
}

static inline uint64_t fp_read(void)
{
	uint64_t value;

	__asm__ volatile("fmov %0, d8" : "=r" (value));
	return value;
}

static long control(int op, unsigned long arg)
{
	return prctl(PR_TASK_CHECKPOINT_DEMO, (unsigned long)op, arg, 0UL, 0UL);
}

static void fail(const char *what)
{
	dprintf(2, "FAIL: %s errno=%d\n", what, errno);
	_exit(1);
}

static void check(int condition, const char *what)
{
	if (!condition)
		fail(what);
}

static void *warmup(void *unused)
{
	pthread_exit(NULL);
}

static void *worker(void *arg)
{
	int id = (int)(intptr_t)arg;
	volatile int stack_value = 1000 + id;
	long ret, generation;

	tls_value = 2000 + id;
	tids[id] = syscall(SYS_gettid);
	atomic_fetch_add(&ready, 1);
	while (!(generation = atomic_load(&epoch)))
		sched_yield();
	fp_write(0x1234567800ULL + id);
	ret = control(ARRIVE, generation);
	if (ret < 0)
		fail("worker arrive");
	if (!ret) {
		stack_value = -id;
		tls_value = -id;
		fp_write(0xff00 + id);
		if (id == 1 && test_mode == 11) {
			while (!atomic_load(&phase))
				sched_yield();
			syscall(SYS_exit, 0);
			fail("raw exit unexpectedly returned");
		}
		if (id == 1)
			return (void *)(intptr_t)41;
		if (test_mode == 11) {
			atomic_store(&sleeping, 1);
			for (;;)
				syscall(SYS_futex, &futex_word, FUTEX_WAIT, 0, NULL, NULL, 0);
		}
		while (!atomic_load(&phase))
			sched_yield();
		if (test_mode == 2)
			abort();
		if (test_mode == 4)
			return (void *)(intptr_t)42;
		if (test_mode == 6)
			return (void *)(intptr_t)62;
		for (;;)
			sched_yield();
	}
	check(ret == 1, "worker restore return");
	check(syscall(SYS_gettid) == tids[id], "worker original tid");
	check(stack_value == 1000 + id, "worker stack");
	check(tls_value == 2000 + id, "worker tls");
	check(fp_read() == 0x1234567800ULL + id, "worker fp register");
	check(global_value == 123 && *heap_value == 456, "worker shared memory");
	atomic_fetch_add(&restored, 1);
	return (void *)(intptr_t)(50 + id);
}

static void *new_worker(void *unused)
{
	volatile int new_stack[128];

	new_stack[0] = 999;
	check(new_stack[0] == 999, "new thread stack");
	if (test_mode == 1)
		abort();
	if (test_mode == 3 || test_mode == 4 || test_mode == 10) {
		int id;

		for (id = 0; id <= (test_mode == 4 ? 2 : 0); id += 2) {
			for (;;) {
				errno = 0;
				if (syscall(SYS_tgkill, getpid(), tids[id], 0) < 0 &&
				    errno == ESRCH)
					break;
				sched_yield();
			}
		}
		if (test_mode == 10) {
			check(control(DROP, atomic_load(&epoch)) == 0, "drop retained leader");
			check(write(report_fd, "R", 1) == 1, "leader drop report");
			_exit(0);
		}
		abort();
	}
	for (;;)
		sched_yield();
}

static void controls_case(void)
{
	long generation;
	uint32_t word = 0;

	errno = 0;
	check(control(99, 0) == -1 && errno == EINVAL, "unknown operation");
	generation = control(PREPARE, 1);
	check(generation > 0, "control prepare");
	errno = 0;
	check(control(PREPARE, 1) == -1 && errno == EBUSY, "duplicate prepare");
	check(control(STATUS, generation) == 1, "collecting status");
	check(control(ARRIVE, generation) == 0, "single arrive");
	errno = 0;
	check(syscall(SYS_eventfd2, 0, 0) == -1 && errno == EOPNOTSUPP,
	      "unknown syscall rejected");
	errno = 0;
	check(syscall(SYS_futex, &word, FUTEX_LOCK_PI, 0, NULL, NULL, 0) == -1 &&
	      errno == EOPNOTSUPP, "PI futex rejected");
	errno = 0;
	check(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
		   -1, 0) == MAP_FAILED && errno == EOPNOTSUPP, "shared mmap rejected");
	check(control(DROP, generation) == 0, "single drop");
	errno = 0;
	check(control(STATUS, generation) == -1 && errno == ESTALE, "drop stale");
	check(write(report_fd, "R", 1) == 1, "control report");
	_exit(0);
}

static void run_case(int mode)
{
	pthread_t warm, newborn;
	pthread_attr_t attributes;
	volatile int stack_value = 789;
	pid_t pid = getpid();
	void *result;
	long generation, ret;

	test_mode = mode;
	if (mode == 12) {
		struct rlimit core = { 8 * 1024 * 1024, 8 * 1024 * 1024 };

		check(!setrlimit(RLIMIT_CORE, &core), "enable native core dump");
	}
	if (mode == 7)
		controls_case();
	check(!pthread_create(&warm, NULL, warmup, NULL), "warmup create");
	check(!pthread_join(warm, NULL), "warmup join");
	heap_value = malloc(sizeof(*heap_value));
	check(heap_value != NULL, "malloc");
	*heap_value = 456;
	mapped_value = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	check(mapped_value != MAP_FAILED, "original mapping");
	*mapped_value = 777;
	tls_value = 900;
	tids[0] = syscall(SYS_gettid);
	check(!pthread_attr_init(&attributes), "attr init");
	if (mode == 5)
		check(!pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED),
		      "detached attribute");
	check(!pthread_create(&workers[0], &attributes, worker, (void *)1), "create original1");
	check(!pthread_attr_destroy(&attributes), "attr destroy");
	check(!pthread_create(&workers[1], NULL, worker, (void *)2), "create original2");
	while (atomic_load(&ready) != 2)
		sched_yield();
	generation = control(PREPARE, 3);
	if (generation < 0)
		fail("prepare");
	atomic_store(&epoch, generation);
	fp_write(0x1234567800ULL);
	ret = control(ARRIVE, generation);
	if (ret < 0)
		fail("main arrive");
	if (!ret) {
		if (mode == 11) {
			sigset_t mask;

			sigemptyset(&mask);
			sigaddset(&mask, SIGABRT);
			check(!pthread_sigmask(SIG_BLOCK, &mask, NULL), "block leader ABRT");
			while (!atomic_load(&sleeping))
				sched_yield();
			check(write(report_fd, "P", 1) == 1, "shared ABRT race ready");
			atomic_store(&phase, 1);
			for (;;)
				syscall(SYS_futex, &futex_word, FUTEX_WAIT, 0, NULL, NULL, 0);
		}
		if (mode == 5) {
			for (;;) {
				errno = 0;
				if (syscall(SYS_tgkill, getpid(), tids[1], 0) < 0 &&
				    errno == ESRCH)
					break;
				sched_yield();
			}
		} else {
			check(!pthread_join(workers[0], &result), "join logically exited original");
			check((intptr_t)result == 41, "first generation result");
		}
		global_value = -123;
		*heap_value = -456;
		stack_value = -789;
		tls_value = -900;
		fp_write(0xff00);
		check(!munmap(mapped_value, 4096), "unmap original mapping");
		if (mode == 6) {
			check(control(DROP, generation) == 0, "drop retained task");
			atomic_store(&phase, 1);
			check(!pthread_join(workers[1], &result), "join after drop");
			check((intptr_t)result == 62, "drop worker result");
			check(write(report_fd, "R", 1) == 1, "drop report");
			_exit(0);
		}
		if (mode == 8) {
			check(write(report_fd, "K", 1) == 1, "kill report");
			kill(getpid(), SIGKILL);
			_exit(99);
		}
		if (mode == 12 || mode == 13) {
			check(write(report_fd, "F", 1) == 1, "native fatal report");
			raise(mode == 12 ? SIGQUIT : SIGTERM);
			_exit(99);
		}
		if (mode == 9) {
			check(write(report_fd, "S", 1) == 1, "stop report");
			raise(SIGSTOP);
		}
		check(!pthread_create(&newborn, NULL, new_worker, NULL), "create new thread");
		if (mode == 3 || mode == 4 || mode == 10) {
			atomic_store(&phase, 1);
			pthread_exit(NULL);
		}
		if (mode == 2) {
			atomic_store(&phase, 1);
			for (;;)
				sched_yield();
		}
		if (mode == 1)
			for (;;)
				sched_yield();
		abort();
	}
	check(ret == 1, "main restored return");
	check(getpid() == pid && syscall(SYS_gettid) == tids[0], "main identity");
	check(stack_value == 789 && tls_value == 900, "main stack/tls");
	check(fp_read() == 0x1234567800ULL, "main fp register");
	check(*mapped_value == 777, "original mapping restored");
	check(global_value == 123 && *heap_value == 456, "main heap/global");
	if (mode != 5) {
		check(!pthread_join(workers[0], &result), "second generation join1");
		check((intptr_t)result == 51, "restored result1");
	}
	check(!pthread_join(workers[1], &result), "second generation join2");
	check((intptr_t)result == 52, "restored result2");
	while (atomic_load(&restored) != 2)
		sched_yield();
	errno = 0;
	check(control(STATUS, generation) == -1 && errno == ESTALE, "consumed epoch");
	dprintf(1, "RESTORED mode=%d pid=%d tids=%d,%d,%d\n", mode, pid,
		tids[0], tids[1], tids[2]);
	check(write(report_fd, "R", 1) == 1, "external restore report");
	/* An independent parent verifies this is a real second SIGABRT exit. */
	abort();
}

int main(void)
{
	struct rlimit core = { 0, 0 };
	int mode, status, passed = 0, round, rounds = 1;
	const char *rounds_env = getenv("CKPT_ROUNDS");
	const char *names[] = {
		"leader abort", "new thread abort", "original worker abort",
		"retained leader restore", "all originals held", "detached restore",
		"drop held worker", "admission errors", "SIGKILL releases held tasks",
		"STOP/CONT with held worker", "DROP with retained leader",
		"shared ABRT/HELD race",
		"SIGQUIT preserves native fatal cause", "SIGTERM preserves native fatal cause",
	};
	pid_t pid;

	setvbuf(stdout, NULL, _IONBF, 0);
	setrlimit(RLIMIT_CORE, &core);
	printf("checkpoint demo libc=%s\n", gnu_get_libc_version());
	if (rounds_env)
		rounds = atoi(rounds_env);
	check(rounds > 0 && rounds <= 100, "round count");
	for (round = 0; round < rounds; round++) {
	for (mode = 0; mode < CASE_COUNT; mode++) {
		int reports[2];
		char marker = 0;
		int pause_fd = -1;

		check(!pipe(reports), "report pipe");
		if (mode == 12)
			unlink("core");
		if (mode == 11) {
			pause_fd = open("/sys/kernel/debug/task_checkpoint_demo/exit_pause", O_RDWR);
			check(pause_fd >= 0, "open exit pause");
			check(write(pause_fd, "1\n", 2) == 2, "arm exit race");
		}
		pid = fork();
		if (pid < 0)
			fail("fork supervisor");
		if (!pid) {
			if (pause_fd >= 0)
				close(pause_fd);
			close(reports[0]);
			report_fd = reports[1];
			run_case(mode);
		}
		close(reports[1]);
		if (mode == 11) {
			int waiters_fd, tries;
			char value[32];

			check(read(reports[0], &marker, 1) == 1 && marker == 'P', "race ready");
			for (tries = 0; tries < 10000; tries++) {
				int len;

				waiters_fd = open("/sys/kernel/debug/task_checkpoint_demo/exit_waiters", O_RDONLY);
				check(waiters_fd >= 0, "open exit waiters");
				len = read(waiters_fd, value, sizeof(value) - 1);
				close(waiters_fd);
				check(len > 0, "read exit waiters");
				value[len] = 0;
				if (atoi(value) > 0)
					break;
				usleep(1000);
			}
			check(tries < 10000, "exit race pause reached");
			check(!kill(pid, SIGABRT), "send shared ABRT");
			close(pause_fd);
			pause_fd = open("/sys/kernel/debug/task_checkpoint_demo/exit_pause", O_WRONLY);
			check(pause_fd >= 0, "reopen exit pause");
			check(write(pause_fd, "0\n", 2) == 2, "release exit race");
			close(pause_fd);
		}
		if (mode == 9) {
			check(read(reports[0], &marker, 1) == 1 && marker == 'S', "stop ready");
			check(waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status),
			      "group stopped with retained task");
			check(!kill(pid, SIGCONT), "continue group");
		}
		if (waitpid(pid, &status, 0) != pid)
			fail("waitpid");
		check(read(reports[0], &marker, 1) >= 0, "read report");
		close(reports[0]);
		if ((marker == 'R' && WIFSIGNALED(status) &&
		     WTERMSIG(status) == SIGABRT) ||
		    ((mode == 6 || mode == 7 || mode == 10) && marker == 'R' &&
		     WIFEXITED(status) && WEXITSTATUS(status) == 0) ||
		    (mode == 8 && marker == 'K' && WIFSIGNALED(status) &&
		     WTERMSIG(status) == SIGKILL) ||
		    ((mode == 12 || mode == 13) && marker == 'F' && WIFSIGNALED(status) &&
		     WTERMSIG(status) == (mode == 12 ? SIGQUIT : SIGTERM) &&
		     (mode != 12 || WCOREDUMP(status)))) {
			printf("PASS round=%d mode=%d %s\n", round, mode, names[mode]);
			passed++;
		} else {
			printf("FAIL mode=%d status=%#x\n", mode, status);
		}
		if (mode == 12)
			unlink("core");
	}
	}
	printf("CKPT_RESULT %s %d/%d\n", passed == CASE_COUNT * rounds ? "PASS" : "FAIL",
	       passed, CASE_COUNT * rounds);
	return passed == CASE_COUNT * rounds ? 0 : 1;
}
