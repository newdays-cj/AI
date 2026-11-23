// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static void *thread(void *unused)
{
	return (void *)42;
}

int main(void)
{
	struct rlimit limit = { 0, 0 };
	int status;
	pid_t pid;

	setrlimit(RLIMIT_CORE, &limit);
	pid = fork();
	if (pid < 0)
		return 1;
	if (!pid) {
		pthread_t id;
		void *result;

		errno = 0;
		if (prctl(0x43504b54, 1UL, 1UL, 0UL, 0UL) != -1 || errno != EINVAL)
			_exit(1);
		if (pthread_create(&id, NULL, thread, NULL) ||
		    pthread_join(id, &result) || result != (void *)42)
			_exit(2);
		abort();
	}
	if (waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) ||
	    WTERMSIG(status) != SIGABRT)
		return 1;
	puts("CKPT_DISABLED PASS");
	return 0;
}
