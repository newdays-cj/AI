// SPDX-License-Identifier: GPL-2.0-only
#include <linux/limits.h>
#include <linux/module.h>

#include "smmu-sva-callback-fixture.h"

/* Independent command budget: PAGE_SIZE bytes, eight bytes per command. */
#define CALLBACK_THRESHOLD ((PAGE_SIZE / 8) * PAGE_SIZE)
static const struct {
	unsigned long start, end;
	size_t expected[2];
} callback_vectors[] = {
	{ 0, 0, { 0, 0 } },
	{ PAGE_SIZE, PAGE_SIZE, { 0, 0 } },
	{ PAGE_SIZE, 3 * PAGE_SIZE, { 2 * PAGE_SIZE, 2 * PAGE_SIZE } },
	{ PAGE_SIZE, PAGE_SIZE + CALLBACK_THRESHOLD - 1,
	  { CALLBACK_THRESHOLD - 1, CALLBACK_THRESHOLD - 1 } },
	{ PAGE_SIZE, PAGE_SIZE + CALLBACK_THRESHOLD, { 0, CALLBACK_THRESHOLD } },
	{ PAGE_SIZE, PAGE_SIZE + CALLBACK_THRESHOLD + 1, { 0, CALLBACK_THRESHOLD + 1 } },
	{ 0, ULONG_MAX, { 0, 0 } },
	{ PAGE_SIZE, ULONG_MAX, { 0, ULONG_MAX - PAGE_SIZE } },
	{ 0, ULONG_MAX - 1, { 0, ULONG_MAX - 1 } },
};

static struct callback_row callback_row(unsigned int index)
{
	unsigned int vector = index / 16;
	bool range = (index / 8) % 2;

	return (struct callback_row) {
		.start = callback_vectors[vector].start,
		.end = callback_vectors[vector].end,
		.expected_size = callback_vectors[vector].expected[range],
		.range_inv = range, .mode = (index / 2) % 4,
		.atc_ret = index % 2 ? -ETIMEDOUT : 0,
	};
}

static const unsigned int callback_indices[] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
	16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
	32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
	48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
	64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
	80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,
	96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
	112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127,
	128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143,
};

static void callback_row_desc(const unsigned int *index, char *desc)
{
	struct callback_row row = callback_row(*index);

	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "start=%lx_end=%lx_range=%u_mode=%u_atc=%d",
		 row.start, row.end, row.range_inv, row.mode, row.atc_ret);
}

KUNIT_ARRAY_PARAM(callback, callback_indices, callback_row_desc);

static void callback_behavior(struct kunit *test)
{
	const unsigned int *index = test->param_value;
	const struct callback_row input = callback_row(*index);
	const struct callback_row *row = &input;
	struct callback_fixture *f = test->priv;
	bool tlb = row->mode != CALLBACK_ATC;
	bool atc = row->mode != CALLBACK_TLB;
	struct callback_event *event;

	f->scope.smmu->features = row->range_inv ? ARM_SMMU_FEAT_RANGE_INV : 0;
	f->atc_ret = row->atc_ret;
	KUNIT_ASSERT_TRUE(test, callback_install(test, 3));

	KUNIT_ASSERT_TRUE(test, callback_run(test, row));

	KUNIT_EXPECT_FALSE(test, f->overflow);
	KUNIT_ASSERT_EQ(test, f->count, (unsigned int)tlb + (unsigned int)atc);
	if (tlb) {
		event = &f->events[0];
		KUNIT_EXPECT_EQ(test, event->order, 1U);
		KUNIT_EXPECT_EQ(test, event->asid, (int)CALLBACK_ASID);
		if (row->expected_size) {
			KUNIT_EXPECT_EQ(test, event->kind, LEAF_RANGE);
			KUNIT_EXPECT_PTR_EQ(test, event->domain, f->scope.domain);
			KUNIT_EXPECT_EQ(test, event->start, row->start);
			KUNIT_EXPECT_EQ(test, event->size, row->expected_size);
			KUNIT_EXPECT_EQ(test, event->granule, (size_t)PAGE_SIZE);
			KUNIT_EXPECT_FALSE(test, event->leaf);
		} else {
			KUNIT_EXPECT_EQ(test, event->kind, LEAF_ASID);
			KUNIT_EXPECT_PTR_EQ(test, event->smmu, f->scope.smmu);
		}
	}
	if (atc) {
		event = &f->events[tlb ? 1 : 0];
		KUNIT_EXPECT_EQ(test, event->kind, LEAF_ATC);
		KUNIT_EXPECT_EQ(test, event->order, tlb ? 2U : 1U);
		KUNIT_EXPECT_PTR_EQ(test, event->domain, f->scope.domain);
		KUNIT_EXPECT_EQ(test, event->start, row->start);
		KUNIT_EXPECT_EQ(test, event->size, row->expected_size);
		KUNIT_EXPECT_EQ(test, event->result, row->atc_ret);
	}
}

enum predicate_input { GATE_CLOSED, RESOURCE_MISSING, WRONG_SMMU, WRONG_DOMAIN,
		       BOTH_OBJECTS, NEITHER_OBJECT };
static const enum predicate_input predicate_rows[] = {
	GATE_CLOSED, RESOURCE_MISSING, WRONG_SMMU, WRONG_DOMAIN,
	BOTH_OBJECTS, NEITHER_OBJECT,
};

static void predicate_desc(const enum predicate_input *input, char *desc)
{
	static const char * const names[] = {
		"closed_gate", "missing_resource", "wrong_smmu", "wrong_domain",
		"ambiguous_both", "neither_object",
	};

	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "%s", names[*input]);
}

KUNIT_ARRAY_PARAM(predicate, predicate_rows, predicate_desc);

static void callback_predicate_rejects(struct kunit *test)
{
	const enum predicate_input *input = test->param_value;
	struct callback_fixture *f = test->priv;
	struct arm_smmu_device *smmu = f->scope.smmu;
	struct arm_smmu_domain *domain = NULL;

	/* Predicate-only: even the enabled cases have no replacements installed. */
	f->scope.run_enabled = true;
	switch (*input) {
	case GATE_CLOSED:
		f->scope.run_enabled = false;
		break;
	case RESOURCE_MISSING:
		kunit_remove_resource(test, &f->resource);
		break;
	case WRONG_SMMU:
		smmu = kunit_kzalloc(test, sizeof(*smmu), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, smmu);
		break;
	case WRONG_DOMAIN:
		smmu = NULL;
		domain = kunit_kzalloc(test, sizeof(*domain), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, domain);
		break;
	case BOTH_OBJECTS:
		domain = f->scope.domain;
		break;
	case NEITHER_OBJECT:
		smmu = NULL;
		break;
	}

	KUNIT_EXPECT_FALSE(test, arm_smmu_sva_test_owns(smmu, domain));
	KUNIT_EXPECT_FALSE(test, callback_run(test, &(struct callback_row) {}));
	KUNIT_EXPECT_EQ(test, f->count, 0U);
}

static const unsigned int partial_rows[] = { 0, 1, 2, 3 };

static void partial_desc(const unsigned int *count, char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "verified_installs=%u", *count);
}

KUNIT_ARRAY_PARAM(partial, partial_rows, partial_desc);

static void callback_partial_cleanup(struct kunit *test)
{
	const unsigned int *count = test->param_value;
	struct callback_fixture *f = test->priv;
	unsigned int i;

	KUNIT_ASSERT_TRUE(test, callback_install(test, *count));
	KUNIT_EXPECT_EQ(test, f->installed, BIT(*count) - 1);
	KUNIT_EXPECT_EQ(test, f->scope.run_enabled, *count == 3);
	if (*count < 3)
		KUNIT_EXPECT_FALSE(test, callback_run(test, &(struct callback_row) {}));

	callback_fixture_exit(test);
	callback_fixture_exit(test);

	KUNIT_EXPECT_FALSE(test, f->scope.run_enabled);
	KUNIT_EXPECT_EQ(test, f->installed, 0U);
	KUNIT_EXPECT_PTR_EQ(test, f->mm, NULL);
	KUNIT_ASSERT_TRUE(test, kunit_hooks.get_static_stub_address);
	for (i = 0; i < ARRAY_SIZE(callback_targets); i++) {
		void *replacement = kunit_hooks.get_static_stub_address(test, callback_targets[i]);

		KUNIT_EXPECT_PTR_EQ(test, replacement, NULL);
	}
	KUNIT_EXPECT_EQ(test, f->count, 0U);
}

static struct kunit_case callback_cases[] = {
	KUNIT_CASE_PARAM(callback_behavior, callback_gen_params),
	KUNIT_CASE_PARAM(callback_predicate_rejects, predicate_gen_params),
	KUNIT_CASE_PARAM(callback_partial_cleanup, partial_gen_params),
	{}
};

static struct kunit_suite callback_suite = {
	.name = "tlbi-smmu-callbacks",
	.init = callback_fixture_init,
	.exit = callback_fixture_exit,
	.test_cases = callback_cases,
};

kunit_test_suite(callback_suite);

MODULE_IMPORT_NS(EXPORTED_FOR_KUNIT_TESTING);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Isolated SMMUv3 SVA callback runtime tests");
