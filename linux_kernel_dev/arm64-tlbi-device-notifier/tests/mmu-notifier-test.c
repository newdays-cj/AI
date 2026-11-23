// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/module.h>
#include <linux/sched/mm.h>

struct delivery {
	struct mm_struct *mm;
	unsigned long start, end;
	unsigned int count, order;
};

struct subscriber {
	struct mmu_notifier mn;
	struct mmu_notifier_ops ops;
	struct delivery conditional, legacy;
	unsigned int sequence;
	bool registered;
};

struct notifier_fixture {
	struct mm_struct *mm;
	struct subscriber subscribers[4];
};

static void conditional_callback(struct mmu_notifier *mn, struct mm_struct *mm,
				 unsigned long start, unsigned long end)
{
	struct subscriber *s = container_of(mn, struct subscriber, mn);

	s->conditional.mm = mm;
	s->conditional.start = start;
	s->conditional.end = end;
	s->conditional.count++;
	s->conditional.order = ++s->sequence;
}

static void legacy_callback(struct mmu_notifier *mn, struct mm_struct *mm,
			    unsigned long start, unsigned long end)
{
	struct subscriber *s = container_of(mn, struct subscriber, mn);

	s->legacy.mm = mm;
	s->legacy.start = start;
	s->legacy.end = end;
	s->legacy.count++;
	s->legacy.order = ++s->sequence;
}

static int range_start(struct mmu_notifier *mn,
		       const struct mmu_notifier_range *range)
{
	return 0;
}

static void range_end(struct mmu_notifier *mn,
		      const struct mmu_notifier_range *range)
{
}

static int notifier_init(struct kunit *test)
{
	struct notifier_fixture *f;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	test->priv = f;
	f->mm = mmu_notifier_test_alloc_mm();
	return f->mm ? 0 : -ENOMEM;
}

static void notifier_exit(struct kunit *test)
{
	struct notifier_fixture *f = test->priv;
	int i;

	if (!f)
		return;
	for (i = 0; i < ARRAY_SIZE(f->subscribers); i++) {
		struct subscriber *s = &f->subscribers[i];

		if (s->registered) {
			mmu_notifier_unregister(&s->mn, f->mm);
			s->registered = false;
		}
	}
	if (f->mm)
		mmput(f->mm);
}

/* Bits select legacy, conditional, range-start and range-end, respectively. */
static int register_subscriber(struct notifier_fixture *f,
			       struct subscriber *s, unsigned int mask)
{
	int ret;

	s->ops.arch_invalidate_secondary_tlbs = mask & 1 ? legacy_callback : NULL;
	s->ops.arch_invalidate_secondary_tlbs_conditional =
		mask & 2 ? conditional_callback : NULL;
	s->ops.invalidate_range_start = mask & 4 ? range_start : NULL;
	s->ops.invalidate_range_end = mask & 8 ? range_end : NULL;
	s->mn.ops = &s->ops;
	ret = mmu_notifier_register(&s->mn, f->mm);
	s->registered = !ret;
	return ret;
}

static void expect_delivery(struct kunit *test, struct delivery *d,
			    unsigned int count)
{
	struct notifier_fixture *f = test->priv;

	KUNIT_EXPECT_EQ(test, d->count, count);
	if (count) {
		KUNIT_EXPECT_PTR_EQ(test, d->mm, f->mm);
		KUNIT_EXPECT_EQ(test, d->start, 0x12345000UL);
		KUNIT_EXPECT_EQ(test, d->end, 0x12349000UL);
	}
}

struct dispatch_vector {
	const char *name;
	unsigned int mask;
	bool need_tlb;
	unsigned int conditional, legacy;
};

static const struct dispatch_vector dispatch_vectors[] = {
	{ "neither_false", 0, false, 0, 0 },
	{ "old_false", 1, false, 0, 1 },
	{ "new_false", 2, false, 0, 0 },
	{ "both_false", 3, false, 0, 1 },
	{ "neither_true", 0, true, 0, 0 },
	{ "old_true", 1, true, 0, 1 },
	{ "new_true", 2, true, 1, 0 },
	{ "both_true", 3, true, 1, 1 },
};

static void dispatch_desc(const struct dispatch_vector *v, char *desc)
{
	strscpy(desc, v->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(dispatch, dispatch_vectors, dispatch_desc);

static void dispatch_slots(struct kunit *test)
{
	const struct dispatch_vector *v = test->param_value;
	struct notifier_fixture *f = test->priv;
	struct subscriber *s = &f->subscribers[0];

	KUNIT_ASSERT_EQ(test, register_subscriber(f, s, v->mask), 0);
	mmu_notifier_arch_invalidate_secondary_tlbs_post_tlbi(f->mm, 0x12345000,
							      0x12349000, v->need_tlb);
	expect_delivery(test, &s->conditional, v->conditional);
	expect_delivery(test, &s->legacy, v->legacy);
	if (v->conditional)
		KUNIT_EXPECT_EQ(test, s->conditional.order, 1U);
	if (v->legacy)
		KUNIT_EXPECT_EQ(test, s->legacy.order, v->conditional + 1);
}

static void dispatch_empty(struct kunit *test)
{
	struct notifier_fixture *f = test->priv;

	KUNIT_EXPECT_FALSE(test, mm_has_notifiers(f->mm));
	mmu_notifier_arch_invalidate_secondary_tlbs_post_tlbi(f->mm, 0, ~0UL, false);
	mmu_notifier_arch_invalidate_secondary_tlbs_post_tlbi(f->mm, 0, ~0UL, true);
}

static void dispatch_mixed_then_unregister(struct kunit *test)
{
	struct notifier_fixture *f = test->priv;
	int i, need;

	for (i = 0; i < ARRAY_SIZE(f->subscribers); i++)
		KUNIT_ASSERT_EQ(test, register_subscriber(f, &f->subscribers[i], i), 0);
	for (need = 0; need <= 1; need++) {
		mmu_notifier_arch_invalidate_secondary_tlbs_post_tlbi(f->mm, 0x12345000,
								      0x12349000, need);
		for (i = 0; i < ARRAY_SIZE(f->subscribers); i++) {
			struct subscriber *s = &f->subscribers[i];

			expect_delivery(test, &s->conditional, need && (i == 2 || i == 3));
			expect_delivery(test, &s->legacy, i == 1 || i == 3 ? need + 1 : 0);
			if (need && i == 3)
				KUNIT_EXPECT_LT(test, s->conditional.order, s->legacy.order);
		}
	}
	for (i = 0; i < ARRAY_SIZE(f->subscribers); i++) {
		mmu_notifier_unregister(&f->subscribers[i].mn, f->mm);
		f->subscribers[i].registered = false;
	}
	mmu_notifier_arch_invalidate_secondary_tlbs_post_tlbi(f->mm, 0x12345000,
							      0x12349000, false);
	mmu_notifier_arch_invalidate_secondary_tlbs_post_tlbi(f->mm, 0x12345000,
							      0x12349000, true);
	for (i = 0; i < ARRAY_SIZE(f->subscribers); i++) {
		expect_delivery(test, &f->subscribers[i].conditional, i == 2 || i == 3);
		expect_delivery(test, &f->subscribers[i].legacy, i == 1 || i == 3 ? 2 : 0);
	}
}

static void registration_exclusion(struct kunit *test)
{
	/* All 16 combinations: seven legal, nine rejected at one WARN_ON_ONCE site. */
	static const int expected[] = {
		0, 0, 0, 0, 0, -EINVAL, -EINVAL, -EINVAL,
		0, -EINVAL, -EINVAL, -EINVAL, 0, -EINVAL, -EINVAL, -EINVAL,
	};
	struct notifier_fixture *f = test->priv;
	struct subscriber *s = &f->subscribers[0];
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(expected); i++) {
		ret = register_subscriber(f, s, i);
		KUNIT_EXPECT_EQ_MSG(test, ret, expected[i], "slot mask %#x", i);
		if (s->registered) {
			mmu_notifier_unregister(&s->mn, f->mm);
			s->registered = false;
		}
	}
}

static struct kunit_case notifier_cases[] = {
	KUNIT_CASE(dispatch_empty),
	KUNIT_CASE_PARAM(dispatch_slots, dispatch_gen_params),
	KUNIT_CASE(dispatch_mixed_then_unregister),
	KUNIT_CASE(registration_exclusion),
	{}
};

static struct kunit_suite notifier_suite = {
	.name = "tlbi-notifier-prototype",
	.init = notifier_init,
	.exit = notifier_exit,
	.test_cases = notifier_cases,
};

kunit_test_suite(notifier_suite);

MODULE_IMPORT_NS(EXPORTED_FOR_KUNIT_TESTING);
MODULE_LICENSE("GPL");
