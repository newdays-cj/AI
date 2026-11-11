// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/kernel-page-flags.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

#define POOL "/sys/kernel/mm/hugepages/hugepages-1048576kB/"
#define SMALL "/sys/kernel/mm/hugepages/hugepages-2048kB/"
#define GIG (1UL << 30)

static _Noreturn void finish(const char *result, const char *reason)
{
	printf("PRE_HVO: %s %s\n", result, reason);
	fflush(stdout);
	sync();
	reboot(RB_POWER_OFF);
	/* PID 1 must not exit, even when firmware cannot power off. */
	for (;;)
		pause();
}

static _Noreturn void fail(const char *reason)
{
	printf("PRE_HVO: errno=%d (%s)\n", errno, strerror(errno));
	finish("FAIL", reason);
}

static void caught(int sig)
{
	(void)sig;
	finish("FAIL", "fatal signal while accessing huge page");
}

static unsigned long number(const char *path)
{
	unsigned long value;
	FILE *f = fopen(path, "r");

	if (!f || fscanf(f, "%lu", &value) != 1)
		fail(path);
	fclose(f);
	printf("PRE_HVO: %s=%lu\n", path, value);
	return value;
}

static void store(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);
	size_t len = strlen(value);

	if (fd < 0 || write(fd, value, len) != (ssize_t)len)
		fail(path);
	if (close(fd))
		fail("close sysfs");
}

static void equal(const char *path, unsigned long expected)
{
	if (number(path) != expected)
		fail("unexpected pool counter");
}

static void page_flags(void *mapping)
{
	static const unsigned long tails[] = { 1, 64, 4096, 262143 };
	uint64_t entry, head, tail;
	size_t i;
	int pm = open("/proc/self/pagemap", O_RDONLY);
	int kf = open("/proc/kpageflags", O_RDONLY);
	off_t offset = ((uintptr_t)mapping / 4096) * sizeof(entry);

	if (pm < 0 || kf < 0) {
		puts("PRE_HVO: CAPABILITY_SKIP pagemap/kpageflags unavailable");
		if (pm >= 0)
			close(pm);
		if (kf >= 0)
			close(kf);
		return;
	}
	if (pread(pm, &entry, sizeof(entry), offset) != sizeof(entry) ||
	    !(entry & (1ULL << 63)))
		fail("pagemap present bit");
	entry &= (1ULL << 55) - 1;
	if (!entry) {
		puts("PRE_HVO: CAPABILITY_SKIP pagemap PFN hidden");
	} else {
		if (pread(kf, &head, sizeof(head), entry * 8) != sizeof(head))
			fail("read kpageflags");
		if (!(head & (1ULL << KPF_COMPOUND_HEAD)) ||
		    !(head & (1ULL << KPF_HUGE)))
			fail("hugetlb head flags");
		for (i = 0; i < sizeof(tails) / sizeof(tails[0]); i++) {
			if (pread(kf, &tail, sizeof(tail), (entry + tails[i]) * 8) != sizeof(tail))
				fail("read tail kpageflags");
			printf("PRE_HVO: head_flags=%#" PRIx64 " tail[%lu]=%#" PRIx64 "\n",
			       head, tails[i], tail);
			if (!(tail & (1ULL << KPF_COMPOUND_TAIL)) ||
			    !(tail & (1ULL << KPF_HUGE)) ||
			    (tail & (1ULL << KPF_COMPOUND_HEAD)))
				fail("hugetlb tail flags");
		}
	}
	close(pm);
	close(kf);
}

int main(void)
{
	char cmdline[4096], test[32] = "lifecycle", *token, *save;
	unsigned long expected = 1, nodes = 1, i;
	volatile uint64_t *mapping;
	FILE *f;

	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGBUS, caught);
	signal(SIGSEGV, caught);
	if (getpid() != 1)
		return 1;
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("sysfs", "/sys", "sysfs", 0, NULL))
		fail("mount proc/sysfs");
	f = fopen("/proc/cmdline", "r");
	if (!f || !fgets(cmdline, sizeof(cmdline), f))
		fail("read cmdline");
	fclose(f);
	printf("PRE_HVO: cmdline=%s\n", cmdline);
	for (token = strtok_r(cmdline, " \n", &save); token;
	     token = strtok_r(NULL, " \n", &save)) {
		if (!strncmp(token, "pre_hvo.expected=", 17)) {
			char *end;

			errno = 0;
			expected = strtoul(token + 17, &end, 10);
			if (errno || *end)
				fail("invalid expected count");
		}
		if (!strncmp(token, "pre_hvo.case=", 13))
			snprintf(test, sizeof(test), "%s", token + 13);
		if (!strncmp(token, "pre_hvo.nodes=", 14))
			nodes = strtoul(token + 14, NULL, 10);
	}
	if (sysconf(_SC_PAGESIZE) != 4096)
		finish("SKIP", "requires 4 KiB base pages");
	if (access(POOL "nr_hugepages", F_OK))
		finish("SKIP", "1 GiB huge pages unavailable");
	if (strcmp(test, "lifecycle") && strcmp(test, "nohuge") && strcmp(test, "demote"))
		fail("unknown case");
	equal(POOL "nr_hugepages", expected);
	equal(POOL "free_hugepages", expected);
	if (nodes == 2) {
		char path[160];
		unsigned long total = 0;

		for (i = 0; i < nodes; i++) {
			snprintf(path, sizeof(path), "/sys/devices/system/node/node%lu/"
				 "hugepages/hugepages-1048576kB/nr_hugepages", i);
			if (access(path, F_OK))
				finish("SKIP", "NUMA node pool unavailable");
			total += number(path);
		}
		if (total != expected)
			fail("NUMA pool sum");
	}
	if (!strcmp(test, "nohuge")) {
		if (expected)
			fail("nohuge requires expected=0");
		mapping = mmap(NULL, GIG, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB |
			       (30 << MAP_HUGE_SHIFT), -1, 0);
		if (mapping != MAP_FAILED || errno != ENOMEM)
			fail("empty pool mmap did not fail with ENOMEM");
		finish("PASS", "nohuge");
	}
	if (!expected)
		fail("mapping case requires a nonempty pool");
	mapping = mmap(NULL, GIG, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB |
		       (30 << MAP_HUGE_SHIFT), -1, 0);
	if (mapping == MAP_FAILED)
		fail("mmap 1 GiB");
	/* Check both ends of every base page, including the last byte of 1 GiB. */
	for (i = 0; i < GIG / sizeof(*mapping); i += 4096 / sizeof(*mapping)) {
		mapping[i] = 0x123456789abcdef0ULL ^ i;
		mapping[i + 511] = ~mapping[i];
	}
	for (i = 0; i < GIG / sizeof(*mapping); i += 4096 / sizeof(*mapping))
		if (mapping[i] != (0x123456789abcdef0ULL ^ i) ||
		    mapping[i + 511] != ~(0x123456789abcdef0ULL ^ i))
			fail("data mismatch");
	puts("PRE_HVO: pattern check across 1 GiB passed");
	equal(POOL "free_hugepages", expected - 1);
	page_flags((void *)mapping);
	if (munmap((void *)mapping, GIG))
		fail("munmap");
	equal(POOL "free_hugepages", expected);
	if (!strcmp(test, "demote")) {
		unsigned long small;

		if (access(POOL "demote", F_OK))
			finish("SKIP", "demote interface unavailable");
		store(POOL "demote_size", "2M");
		equal(POOL "demote_size", 2048);
		small = number(SMALL "nr_hugepages");
		store(POOL "demote", "1");
		equal(POOL "nr_hugepages", expected - 1);
		equal(SMALL "nr_hugepages", small + 512);
		store(SMALL "nr_hugepages", "0");
		equal(SMALL "nr_hugepages", 0);
		equal(SMALL "free_hugepages", 0);
	}
	store(POOL "nr_hugepages", "0");
	equal(POOL "nr_hugepages", 0);
	equal(POOL "free_hugepages", 0);
	equal(POOL "surplus_hugepages", 0);
	finish("PASS", test);
	return 0;
}
