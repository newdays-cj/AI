// SPDX-License-Identifier: GPL-2.0
/* Pure SVA normalization and notifier-table inspection; no device commands. */
#include <kunit/test.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/module.h>

#include "arm-smmu-v3.h"
#include "arm-smmu-v3-sva-test.h"

static void smmu_sva_size_normalization(struct kunit *test)
{
	/* One command per page, with a command budget of PAGE_SIZE / 8. */
	const unsigned long threshold = (PAGE_SIZE / 8) * PAGE_SIZE;
	const struct {
		unsigned long start;
		unsigned long end;
		size_t without_range;
		size_t with_range;
	} cases[] = {
		{ 0, 0, 0, 0 },
		{ PAGE_SIZE, PAGE_SIZE, 0, 0 },
		{ PAGE_SIZE, 3 * PAGE_SIZE, 2 * PAGE_SIZE, 2 * PAGE_SIZE },
		{ PAGE_SIZE, PAGE_SIZE + threshold - 1,
		  threshold - 1, threshold - 1 },
		{ PAGE_SIZE, PAGE_SIZE + threshold, 0, threshold },
		{ PAGE_SIZE, PAGE_SIZE + threshold + 1, 0, threshold + 1 },
		{ 0, ULONG_MAX, 0, 0 },
		{ 0, ULONG_MAX - 1, 0, ULONG_MAX - 1 },
	};
	struct arm_smmu_device *smmu;
	struct arm_smmu_domain *domain;
	size_t size, expected;
	unsigned int i, range;

	/* Ordinary memory only: never register this domain or invoke its ops. */
	smmu = kunit_kzalloc(test, sizeof(*smmu), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, smmu);
	domain = kunit_kzalloc(test, sizeof(*domain), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, domain);
	domain->smmu = smmu;

	for (range = 0; range < 2; range++) {
		smmu->features = range ? ARM_SMMU_FEAT_RANGE_INV : 0;
		for (i = 0; i < ARRAY_SIZE(cases); i++) {
			if (range)
				expected = cases[i].with_range;
			else
				expected = cases[i].without_range;

			size = arm_smmu_sva_invalidate_size(domain, cases[i].start,
							    cases[i].end);

			KUNIT_EXPECT_EQ_MSG(test, size, expected,
					    "case %u RANGE_INV=%u", i, range);
		}
	}
}

static void smmu_sva_notifier_ops_shape(struct kunit *test)
{
	const struct mmu_notifier_ops *legacy;
	const struct mmu_notifier_ops *conditional;

	legacy = arm_smmu_sva_test_notifier_ops(false);
	conditional = arm_smmu_sva_test_notifier_ops(true);

	/* Structural checks only, not callback runtime or ordering coverage. */
	KUNIT_ASSERT_NOT_NULL(test, legacy);
	KUNIT_ASSERT_NOT_NULL(test, conditional);
	KUNIT_EXPECT_PTR_NE(test, legacy, conditional);
	KUNIT_EXPECT_TRUE(test, legacy->arch_invalidate_secondary_tlbs);
	KUNIT_EXPECT_FALSE(test, legacy->arch_invalidate_secondary_tlbs_conditional);
	KUNIT_EXPECT_TRUE(test, conditional->arch_invalidate_secondary_tlbs);
	KUNIT_EXPECT_TRUE(test, conditional->arch_invalidate_secondary_tlbs_conditional);
	KUNIT_EXPECT_TRUE(test, legacy->arch_invalidate_secondary_tlbs !=
		conditional->arch_invalidate_secondary_tlbs);
	KUNIT_EXPECT_TRUE(test, legacy->arch_invalidate_secondary_tlbs !=
		conditional->arch_invalidate_secondary_tlbs_conditional);
	KUNIT_EXPECT_TRUE(test, conditional->arch_invalidate_secondary_tlbs !=
		conditional->arch_invalidate_secondary_tlbs_conditional);
	KUNIT_EXPECT_TRUE(test, legacy->release);
	KUNIT_EXPECT_TRUE(test, legacy->free_notifier);
	KUNIT_EXPECT_TRUE(test, legacy->release == conditional->release);
	KUNIT_EXPECT_TRUE(test, legacy->free_notifier == conditional->free_notifier);
	KUNIT_EXPECT_FALSE(test, legacy->invalidate_range_start);
	KUNIT_EXPECT_FALSE(test, legacy->invalidate_range_end);
	KUNIT_EXPECT_FALSE(test, conditional->invalidate_range_start);
	KUNIT_EXPECT_FALSE(test, conditional->invalidate_range_end);
}

static struct kunit_case smmu_sva_test_cases[] = {
	KUNIT_CASE(smmu_sva_size_normalization),
	KUNIT_CASE(smmu_sva_notifier_ops_shape),
	{}
};

static struct kunit_suite smmu_sva_test_suite = {
	.name = "tlbi-smmu-prototype",
	.test_cases = smmu_sva_test_cases,
};

kunit_test_suite(smmu_sva_test_suite);

MODULE_IMPORT_NS(EXPORTED_FOR_KUNIT_TESTING);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Isolated SMMUv3 SVA pure helper tests");
