// SPDX-License-Identifier: GPL-2.0
#include <linux/mmu_notifier.h>
#include <linux/module.h>

#ifdef CONFIG_MMU_NOTIFIER
#error "The stub compile probe requires CONFIG_MMU_NOTIFIER=n"
#endif

static int __init notifier_stub_probe_init(void)
{
	mmu_notifier_arch_invalidate_secondary_tlbs_post_tlbi(NULL, 0, ~0UL,
							      false);
	mmu_notifier_arch_invalidate_secondary_tlbs_post_tlbi(NULL, 0, ~0UL,
							      true);
	return 0;
}

static void __exit notifier_stub_probe_exit(void)
{
}

module_init(notifier_stub_probe_init);
module_exit(notifier_stub_probe_exit);
MODULE_LICENSE("GPL");
