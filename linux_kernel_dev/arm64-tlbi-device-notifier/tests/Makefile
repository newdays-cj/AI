# SPDX-License-Identifier: GPL-2.0
ifeq ($(PROTOTYPE_STUB_ONLY),1)
obj-m += mmu-notifier-stub-probe.o
else
obj-m += mmu-notifier-test.o
obj-m += smmu-sva-test.o
obj-m += smmu-sva-callback-test.o
smmu-sva-callback-test-y := smmu-sva-callback-cases.o smmu-sva-callback-fixture.o
endif

ccflags-y += -I$(srctree)/drivers/iommu/arm/arm-smmu-v3
