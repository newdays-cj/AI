# Selected TLB/ATC Split: Software Design Closure

## Scope

The user selected the recommended split: bit0 gates only software SMMU TLB
invalidation; ATC is independently evaluated; required main-TLB completion
precedes ATC; inactive platforms retain legacy combined behavior. Close software
API and KUnit seam design in the external dossier, review and hand off. No kernel
code, commits, builds, tests or hardware qualification are authorized by this
documentation execution plan. Human confirmation is preapproved, not repeated.

Dossier: /home/chenjun/workspace/arm64-tlbi-device-notifier/.
Kernel: /home/chenjun/Git/OLK-6.6.
Workflow: /home/chenjun/my-env/AI/linux_kernel_dev/需求开发.md.

## Settled Software Direction

Oracle session ses_e7a5d3b8affe2ptiSIHscUdVhX chose old-slot ATC-only plus
conditional-slot TLB-only for active SMMU subscriptions. Legacy subscriptions
retain the combined callback. An internal post-TLBI dispatcher takes the saved
need_tlb boolean, not a new unconditional consumer callback with status.
One SRCU traversal invokes conditional then old per subscription; the conditional
callback is never invoked when bit0=0. Old-only subscribers still run once.
Old+conditional is permitted; either slot excludes invalidate_range_start/end.
Generic registration does not infer callback semantics or compare pointers.

SRCU protects object/ops memory across the pair, not atomicity against release:
mm/mmu_notifier.c:802-850 explicitly permits release/unregister concurrency.
Do not repeat the Oracle shorthand that unregister cannot begin between calls.
The active transaction restores the caller's IRQ/preemption state before any
notifier traversal. Existing SMMU timeout/error policy is retained, not upgraded
to a universal hardware completion guarantee.

## Current Tasks

1. Update 05-实现方案.md with the single selected API, slot/registration rules,
   per-subscription order, shared normalization, TLB-only/ATC-only/legacy combined
   helpers, module-safe zcopy/OBMM bridges and exact function inventory.
2. Specify G1-G4 test seams: persistent candidate build/cleanup separate from
   __init publication; current-test, in_task and target-object scoped static
   stubs; same-TU bridge for actual inline control flow; typed SMMU leaf fault
   injection; bounded event recording and KUnit namespace exports. Do not mutate
   production static keys, mappings, CPU capabilities or real hardware queues.
3. Update 06-测试设计.md and 覆盖率.md from the final inventory. Carry G5 as an
   explicit original failure-path exception. Raw issue-helper replacement cannot
   establish raw-opcode runtime coverage; name that exception E_RAW and preserve
   build/disassembly and hardware obligations instead of claiming 100% runtime.
4. Submit this plan plus the actual 05/06/matrix to Momus; address concrete
   findings. Record exactly whether documentation/design is accepted, not kernel
   implementation or hardware success.
5. Verify references, read every changed artifact and update 00/01 with the
   selected direction, actual review result and next coding boundary.

Dependencies: Oracle and existing source evidence -> 1/2 -> 3 -> 4 -> 5.
All changes stay outside the kernel tree. No disjoint writer edits the same file.

## Evidence

- mm/mmu_notifier.c:588-604,620-628,802-850: old dispatch, existing exclusion,
  SRCU and allowed release/unregister concurrency.
- drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3-sva.c:132-205,391-431: normalization,
  combined TLB/ATC, lifecycle, ops and allocation.
- drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:2234-2383: ATC conditions, encoding,
  locking and submission; :1090-1113,1180-1184: CMD_SYNC and error handling.
- include/kunit/static_stub.h:25-98, test-bug.h:29-47, visibility.h:13-30,
  resource.h:323-340: task-scoped redirection, visibility and lookup locking.
- arch/arm64/include/asm/tlbflush.h:253-453: active/inactive logical operations;
  drivers/misc/zcopy/zcopy.c:133 and drivers/ub/obmm/obmm_cache.c:152: bypasses.

## Per-Task QA

QA1 (task 1): Read 05 and the cited source slices. Expected: one selected split,
no A/B choice left to the user, no combined callback gated by bit0, no global
old-subscriber suppression, and TLB then ATC within one SRCU-protected subscription.
Check exact signatures and generic exclusion predicate; release concurrency must
not be described as serialized by SRCU.

QA2 (task 2): Read 05 seam section and include/kunit/static_stub.h,
test-bug.h, visibility.h and resource.h. Expected: named entry/install/run
signatures, persistent versus __init boundary, same-TU function address identity,
target-mm/device/candidate scoping and in_task guard; stubs installed before the
IRQ-off region, no allocations/assertions/printing there. Worker tests use
explicit notifier context, not automatic inheritance of current->kunit_test.

QA3 (task 3): Use Read and Bash/rg on 05, 06 and 覆盖率.md. From the dossier root:

```sh
comm -23 <(rg --no-filename -o 'TC[0-9]{2}(\.[0-9]{2}){2}' 覆盖率.md | sort -u) <(rg '^\| TC[0-9]{2}(\.[0-9]{2}){2} \| 场景' 06-测试设计.md | rg -o 'TC[0-9]{2}(\.[0-9]{2}){2}' | sort -u)
comm -23 <(rg '^\| [PNTSKZO][0-9]+ ' 05-实现方案.md | rg -o '^\| [PNTSKZO][0-9]+' | sort -u) <(rg '^\| [PNTSKZO][0-9]+ ' 覆盖率.md | rg -o '^\| [PNTSKZO][0-9]+' | sort -u)
```

Expected: exit 0 and empty output for both, and manual branch-by-branch mapping
including new split/seam functions. This is traceability, not executed coverage.
E_RAW/E_KEY/E_BYPASS_RAW/E_IO/E_ENTRY and G5 remain visible with their evidence requirements. The function-inventory expression excludes H1-H4 hardware-gate rows; the current inventory is 88 tracking IDs, not 92 functions. G1-G4 may be closed
as software design only, not treated as implemented/passing KUnit tests.

QA4 (task 4): Read actual completed Oracle/Momus results with background_output
only after notification. Compare transcription in 00 to actual verdict and scope.
Expected: no invented approval, no hardware-gate waiver, no repeated human signoff
request in conflict with the user's automatic approval.

QA5 (task 5): Read every changed document and inspect links. Kernel changes from
other tasks were observed in drivers/iommu/io-pgtable-arm.c, include/linux/io-pgtable.h
and untracked AGENTS.md/Documentation/admin-guide/bootperf.rst. Use git status/diff
with git-master loaded only if needed; record concurrent changes, never revert or
commit them. Expected: this task only writes the external dossier, no claim of a
clean whole worktree or successful kernel build, diagnostics or runtime tests.

## Remaining Boundary

This closes software choices only. DT-listed SMMU scope, other-bit ownership,
concurrent read-clear, NMI/SError/higher-EL/global producers, reset/power/hotplug
and the real vendor ABI remain hardware/platform gates. The confirmed 32-bit LE,
whole-register clearing, accumulation and DSB visibility are not re-questioned.
Production coding requires a subsequent reviewed implementation plan and the
unresolved prerequisites; neither test designs nor this review count as hardware
qualification. No kernel commit is authorized.
