// SPDX-License-Identifier: GPL-2.0-only
#include <linux/sched/mm.h>

#include "smmu-sva-callback-fixture.h"

void * const callback_targets[3] = {
	arm_smmu_tlb_inv_asid, arm_smmu_tlb_inv_range_asid, arm_smmu_atc_inv_domain,
};

static struct callback_fixture *
callback_owner(struct arm_smmu_device *smmu, struct arm_smmu_domain *domain)
{
	struct kunit *test;

	if (!in_task() || !arm_smmu_sva_test_owns(smmu, domain))
		return NULL;
	test = kunit_get_current_test();
	return test->priv;
}

static void callback_record(struct callback_fixture *f, struct callback_event event)
{
	if (f->count == ARRAY_SIZE(f->events)) {
		f->overflow = true;
		return;
	}
	event.order = f->count + 1;
	f->events[f->count++] = event;
}

static void callback_asid(struct arm_smmu_device *smmu, u16 asid)
{
	struct callback_fixture *f = callback_owner(smmu, NULL);

	if (!f)
		return;
	callback_record(f, (struct callback_event) {
		.kind = LEAF_ASID, .smmu = smmu, .asid = asid,
	});
}

/* The six arguments intentionally match the existing typed command leaf. */
static void callback_range(unsigned long start, size_t size, int asid,
			   size_t granule, bool leaf, struct arm_smmu_domain *domain)
{
	struct callback_fixture *f = callback_owner(NULL, domain);

	if (!f)
		return;
	callback_record(f, (struct callback_event) {
		.kind = LEAF_RANGE, .domain = domain, .start = start, .size = size,
		.asid = asid, .granule = granule, .leaf = leaf,
	});
}

static int callback_atc(struct arm_smmu_domain *domain, unsigned long start, size_t size)
{
	struct callback_fixture *f = callback_owner(NULL, domain);

	if (!f)
		return -EINVAL;
	callback_record(f, (struct callback_event) {
		.kind = LEAF_ATC, .domain = domain, .start = start, .size = size,
		.result = f->atc_ret,
	});
	return f->atc_ret;
}

bool callback_install(struct kunit *test, unsigned int count)
{
	struct callback_fixture *f = test->priv;
	void *replacement;
	unsigned int i;

	f->scope.run_enabled = false;
	if (count > 3 || f->installed || !kunit_hooks.get_static_stub_address)
		return false;
	for (i = 0; i < count; i++) {
		if (kunit_hooks.get_static_stub_address(test, callback_targets[i]))
			return false;
		switch (i) {
		case LEAF_ASID:
			kunit_activate_static_stub(test, arm_smmu_tlb_inv_asid, callback_asid);
			replacement = callback_asid;
			break;
		case LEAF_RANGE:
			kunit_activate_static_stub(test, arm_smmu_tlb_inv_range_asid,
						   callback_range);
			replacement = callback_range;
			break;
		case LEAF_ATC:
			kunit_activate_static_stub(test, arm_smmu_atc_inv_domain, callback_atc);
			replacement = callback_atc;
			break;
		default:
			return false;
		}
		/* Activation is void; only exact hook verification earns a bit. */
		if (kunit_hooks.get_static_stub_address(test, callback_targets[i]) != replacement)
			return false;
		f->installed |= BIT(i);
	}
	f->scope.run_enabled = f->installed == CALLBACK_ALL_STUBS;
	return true;
}

int callback_fixture_init(struct kunit *test)
{
	struct callback_fixture *f;
	int ret;

	/* LIFO cleanup must remove the embedded resource before this storage. */
	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	test->priv = f;
	f->scope.smmu = kunit_kzalloc(test, sizeof(*f->scope.smmu), GFP_KERNEL);
	f->scope.domain = kunit_kzalloc(test, sizeof(*f->scope.domain), GFP_KERNEL);
	if (!f->scope.smmu || !f->scope.domain)
		return -ENOMEM;
	f->scope.domain->smmu = f->scope.smmu;
	f->scope.domain->cd.asid = CALLBACK_ASID;
	ret = kunit_add_named_resource(test, NULL, NULL, &f->resource,
				       ARM_SMMU_SVA_TEST_RESOURCE, &f->scope);
	if (ret)
		return ret;
	f->mm = mmu_notifier_test_alloc_mm();
	return f->mm ? 0 : -ENOMEM;
}

void callback_fixture_exit(struct kunit *test)
{
	struct callback_fixture *f = test->priv;
	int i;

	if (!f)
		return;
	f->scope.run_enabled = false;
	for (i = ARRAY_SIZE(callback_targets) - 1; i >= 0; i--) {
		if (!(f->installed & BIT(i)))
			continue;
		kunit_deactivate_static_stub(test, callback_targets[i]);
		f->installed &= ~BIT(i);
	}
	/* This mm never had the fake domain's notifier registered. */
	if (f->mm) {
		mmput(f->mm);
		f->mm = NULL;
	}
}

bool callback_run(struct kunit *test, const struct callback_row *row)
{
	struct callback_fixture *f = test->priv;
	const struct mmu_notifier_ops *ops;
	struct kunit_resource *resource;
	struct mmu_notifier *mn;
	bool owned;

	if (!in_task() || kunit_get_current_test() != test || !f || !f->mm ||
	    !f->scope.run_enabled || f->installed != CALLBACK_ALL_STUBS)
		return false;
	resource = kunit_find_named_resource(test, ARM_SMMU_SVA_TEST_RESOURCE);
	if (!resource)
		return false;
	owned = resource->data == &f->scope;
	kunit_put_resource(resource);
	if (!owned || !arm_smmu_sva_test_owns(f->scope.smmu, NULL) ||
	    !arm_smmu_sva_test_owns(NULL, f->scope.domain))
		return false;
	ops = arm_smmu_sva_test_notifier_ops(row->mode != CALLBACK_LEGACY);
	if (!ops || !ops->arch_invalidate_secondary_tlbs ||
	    (row->mode != CALLBACK_LEGACY &&
	     !ops->arch_invalidate_secondary_tlbs_conditional))
		return false;
	mn = &f->scope.domain->mmu_notifier;
	switch (row->mode) {
	case CALLBACK_TLB:
		ops->arch_invalidate_secondary_tlbs_conditional(mn, f->mm,
							       row->start, row->end);
		break;
	case CALLBACK_ATC:
	case CALLBACK_LEGACY:
		ops->arch_invalidate_secondary_tlbs(mn, f->mm, row->start, row->end);
		break;
	case CALLBACK_SPLIT:
		ops->arch_invalidate_secondary_tlbs_conditional(mn, f->mm,
							       row->start, row->end);
		ops->arch_invalidate_secondary_tlbs(mn, f->mm, row->start, row->end);
		break;
	default:
		return false;
	}
	return true;
}
