// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define COMMAND 0x43504b54

static void require(int valid, const char *description)
{
	if (!valid) {
		dprintf(2, "FAIL external: %s errno=%d\n", description, errno);
		_exit(1);
	}
}

static int write_value(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);
	int error = 0;

	if (fd < 0)
		return errno;
	if (write(fd, value, strlen(value)) != (ssize_t)strlen(value))
		error = errno;
	close(fd);
	return error;
}

static void exchange(int output, int input, char send, char expected)
{
	char value = 0;

	require(write(output, &send, 1) == 1, "protocol write");
	require(read(input, &value, 1) == 1 && value == expected, "protocol read");
}

static void run_hierarchy(const char *root, int legacy)
{
	char first[512], second[512], first_procs[600], second_procs[600];
	char first_freeze[600], second_freeze[600], id[32], marker;
	const char *tasks = legacy ? "tasks" : "cgroup.procs";
	const char *freeze = legacy ? "freezer.state" : "cgroup.freeze";
	const char *freeze_value = legacy ? "FROZEN\n" : "1\n";
	const char *thaw_value = legacy ? "THAWED\n" : "0\n";
	int request[2], response[2], status;
	pid_t pid;

	snprintf(first, sizeof(first), "%s/ckpt-a", root);
	snprintf(second, sizeof(second), "%s/ckpt-b", root);
	snprintf(first_procs, sizeof(first_procs), "%s/%s", first, tasks);
	snprintf(second_procs, sizeof(second_procs), "%s/%s", second, tasks);
	snprintf(first_freeze, sizeof(first_freeze), "%s/%s", first, freeze);
	snprintf(second_freeze, sizeof(second_freeze), "%s/%s", second, freeze);
	require(!mkdir(first, 0700) && !mkdir(second, 0700), "create cgroups");
	require(!pipe(request) && !pipe(response), "protocol pipes");
	pid = fork();
	require(pid >= 0, "external fork");
	if (!pid) {
		long epoch;

		close(request[1]);
		close(response[0]);
		exchange(response[1], request[0], 'P', 'G');
		epoch = prctl(COMMAND, 1UL, 1UL, 0UL, 0UL);
		require(epoch > 0, "prepare external target");
		require(prctl(COMMAND, 2UL, (unsigned long)epoch, 0UL, 0UL) == 0,
			"arrive external target");
		exchange(response[1], request[0], 'A', 'D');
		require(prctl(COMMAND, 3UL, (unsigned long)epoch, 0UL, 0UL) == 0,
			"drop external target");
		exchange(response[1], request[0], 'D', 'Q');
		_exit(0);
	}
	close(request[0]);
	close(response[1]);
	require(read(response[0], &marker, 1) == 1 && marker == 'P', "child before prepare");
	snprintf(id, sizeof(id), "%d\n", pid);
	require(write_value(first_procs, id) == 0, "native migration before prepare");
	exchange(request[1], response[0], 'G', 'A');
	errno = 0;
	require(ptrace(PTRACE_ATTACH, pid, NULL, NULL) == -1 && errno == EBUSY,
		"ptrace target rejected");
	require(write_value(second_procs, id) == EBUSY, "enrolled migration rejected");
	require(write_value(first_freeze, freeze_value) == EBUSY, "enrolled freeze rejected");
	require(write_value(first_freeze, thaw_value) == 0, "thaw permitted");
	exchange(request[1], response[0], 'D', 'D');
	require(write_value(second_procs, id) == 0, "native migration after drop");
	require(write_value(second_freeze, freeze_value) == 0, "native freeze after drop");
	require(write_value(second_freeze, thaw_value) == 0, "native thaw after drop");
	require(write(request[1], "Q", 1) == 1, "finish child");
	require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
		WEXITSTATUS(status) == 0, "external target exit");
	close(request[1]);
	close(response[0]);
	require(!rmdir(first) && !rmdir(second), "release cgroups");
	printf("PASS external=%s ptrace/migrate/freeze/thaw\n", legacy ? "v1" : "v2");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	run_hierarchy("/sys/fs/cgroup", 0);
	run_hierarchy("/tmp/ckpt-freezer", 1);
	puts("CKPT_EXTERNAL PASS 2/2");
	return 0;
}
