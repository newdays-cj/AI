/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SMMU_SVA_CALLBACK_FIXTURE_H
#define SMMU_SVA_CALLBACK_FIXTURE_H

#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>

#include "arm-smmu-v3.h"
#include "arm-smmu-v3-sva-test.h"

#define CALLBACK_ALL_STUBS 7U
#define CALLBACK_ASID 0x5a3U

enum callback_mode { CALLBACK_TLB, CALLBACK_ATC, CALLBACK_LEGACY, CALLBACK_SPLIT };
enum callback_leaf { LEAF_ASID, LEAF_RANGE, LEAF_ATC };

struct callback_row {
	unsigned long start, end;
	size_t expected_size;
	bool range_inv;
	enum callback_mode mode;
	int atc_ret;
};

struct callback_event {
	enum callback_leaf kind;
	struct arm_smmu_device *smmu;
	struct arm_smmu_domain *domain;
	unsigned long start;
	size_t size, granule;
	int asid, result;
	bool leaf;
	unsigned int order;
};

struct callback_fixture {
	struct arm_smmu_sva_test_scope scope;
	struct kunit_resource resource;
	struct mm_struct *mm;
	struct callback_event events[4];
	unsigned int installed, count;
	int atc_ret;
	bool overflow;
};

extern void * const callback_targets[3];
int callback_fixture_init(struct kunit *test);
void callback_fixture_exit(struct kunit *test);
bool callback_install(struct kunit *test, unsigned int count);
bool callback_run(struct kunit *test, const struct callback_row *row);

#endif /* SMMU_SVA_CALLBACK_FIXTURE_H */
