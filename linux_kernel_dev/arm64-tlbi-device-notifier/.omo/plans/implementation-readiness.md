# Implementation readiness and gated execution

## Scope and authority

Dossier: `/home/chenjun/workspace/arm64-tlbi-device-notifier`.
Source: `/home/chenjun/Git/OLK-6.6`.
Workflow: `/home/chenjun/my-env/AI/linux_kernel_dev/需求开发.md`.

Continue the selected Option A documented in 05 and 06: conditional TLB-only
followed by old-slot ATC-only within each subscription, one SRCU traversal;
legacy remains combined. The current inventory contains 88 tracking IDs.
Manual workflow confirmations are preapproved. Commits are not authorized.
Technical review, hardware facts and runtime evidence cannot be preapproved.

This plan authorizes its documentation readiness phase only. Production code
starts only after the prerequisites below are actually satisfied. No fake vendor
compatible, physical address, hardware model or production activation bypass is
an acceptable substitute. A plan executability verdict is not design approval.

## Immediate readiness tasks

1. Audit compatibility gate C against current source. Read every old-slot
   registration and architecture Kconfig dependency, then follow reachable
   SMMUv3/Logic UMMU callback helpers and zcopy/OBMM callers. Record file:line,
   locks, completion and atomic-context dependencies in
   `07-实施准备审查.md`. Tools: local source reading/search. Expected evidence:
   a bounded registration/caller inventory, explicit existing risks, and separate
   software conclusions versus hardware dependencies. Do not fix unrelated code.
2. Obtain Oracle technical review of 05/06 against current KUnit, notifier and
   arm64/SMMUv3 source contracts. Review pin/read/restore, fallback, split
   normalization, release concurrency and same-TU task-scoped static stubs.
   Tools: background Oracle read-only review, collect after completion.
   Expected evidence: exact verdict, session ID, actionable defects and minimal
   amendments; unknown platform facts stay unknown.
3. Address established software defects in 05/06 and the coverage matrix if
   needed, without changing the selected direction or inventing hardware facts.
   Tools: Read and apply_patch. If signatures/branches change, propagate all
   affected tracking IDs and leaf TC references; check set differences in both
   directions and record results. Re-review substantive corrected design with
   the same Oracle session. Expected: no unresolved software blocker concealed
   as a hardware exception. If no design edit is needed, do not repeat already
   completed traceability checks.
4. Submit this plan to Momus using this absolute path as its sole prompt. Tools:
   background task and completed result collection. Expected: an actual
   executability verdict, with findings addressed in this file. Archive that
   scope separately from Oracle's technical verdict.
5. Update 00 with completed evidence, exact outstanding decisions, next action
   and review locations; update 01 only if requirements or next-stage state
   changed. Tools: Read/apply_patch and final artifact readback. Expected: no
   claim of kernel edits, builds, tests or measured coverage from document work.

Tasks 1 and 2 run independently; 3 depends on 2, and 4 follows the plan and any
necessary corrections. Task 5 follows both reviews. Changes stay in the dossier.

## Production entry prerequisites

Every item needs a recorded source and accountable evidence, not a new DT flag:

- H1: bit0 covers every listed SMMU TLB and its zero value permits omitting the
  corresponding software invalidation; ATC remains independent.
- H2: eligible events and non-user/global/reserved-ASID/Stage-2, broadcast,
  IRQ/NMI/SError/higher-EL producers have a sound attribution/isolation contract.
- H3: concurrent set/read-clear cannot lose a required event. High bits are
  already confirmed reserved with no other reader; do not ask again.
- H4: reset/initial activation, hotplug, power and suspend preserve accessibility
  and attribution without an extra destructive drain.
- V: actual registered vendor compatible and platform CPU/resource ABI.
- C: source audit and remaining platform-specific completion/locking review.
- Technical approval of detailed software and test design, separately recorded
  from Momus plan executability. G1-G4 are designs, not executed tests.

Stop the readiness phase when its reports and reviewed plan are archived. If any
production prerequisite is missing, report its precise missing evidence and do
not install activatable code. The present host has no target MMIO model or
provided target platform, so it cannot supply those hardware facts.

## Conditional implementation sequence

The following units are queued, not started by approval of this readiness plan.
Read all applicable subtree instructions and inspect shared changes first. Never
revert/stage unrelated edits. Build output is under the dossier, not source root.
Before creating output directories, verify their parents; before copying the
baseline, verify a new output tree or preserve an existing configuration.

1. Provider and G1: implement 05's Kconfig/Makefile, common header and
   `arch/arm64/mm/tlbi-mmio-status.c`, persistent candidate build/query/destroy,
   validated real binding and resource ownership. Keep publication in init only.
   Test candidate classification, bounds and rollback with typed isolated leaves;
   no real request/map or production-key mutation in fixtures.
2. Notifier: add the conditional slot, wrapper and one-SRCU dispatcher in
   `include/linux/mmu_notifier.h` and `mm/mmu_notifier.c`. Extend the existing
   start/end exclusion; allow orthogonal dual slots. Verify old/new/both/neither,
   false/true, registration rejection and bounded concurrent release/unregister.
3. TLBI and G2/G4: integrate mm/page/range and wrapper/defer changes in
   `arch/arm64/include/asm/tlbflush.h`; include required young comments.
   Preserve inactive timing and raw instruction semantics. Test same-TU real
   control flow, single read, restore, fallback and nested saved values.
   Actual young/contpte/fullmm/reclaim caller integration remains E_CALLER
   source/build plus qualified-platform evidence; provider-TU stubs do not cover
   other translation units.
   Inspect generated code for raw paths excluded by whole-helper replacement.
4. SMMUv3 and G3: implement shared size normalization, split callbacks, two ops
   tables and allocation mode selection in SVA; add narrow KUnit visibility and
   typed command leaves in the main driver. Preserve release/free and original
   error policy. Test actual ATC encoding/traversal with fake command leaves,
   including bit0=0, nested/full/range/PASID/SID and allocation rollback.
5. Bypasses: integrate GPL range/mm bridges with zcopy and OBMM. Audit all
   propagated limit-helper signatures, including kernel-range NULL. Test active
   paths through actual callers and modules; audit inactive raw code separately.
6. Independent tests: implement the modules and `tests/run_tests.sh` and
   `tests/run_qemu.sh` from 06; mirror sources in the dossier. Extend inventory
   for every new test helper rather than freezing the count at 88. Collect
   per-vector KTAP, module status, complete serial logs and resource balances.

Units have source dependencies and are not independent writers of shared headers.
Test-source work may run beside its corresponding production unit only with
disjoint explicit ownership and a settled interface. Integrate and verify each
unit before its dependent unit. Without commit authorization, retain local diffs
and per-unit patch/check evidence; do not create commits or claim a committed
patch series. The workflow's commit-based archive step remains pending authority.

## Required validation for each future code unit

Use `~/tools/qemu_arm64_defconfig` as the full base `.config`, ARM64 with
`CROSS_COMPILE=aarch64-linux-gnu-`, and dedicated `O=` directories. Run
olddefconfig, inspect effective option values, then compile `Image modules` with
`make -j$(nproc)`. Run `scripts/checkpatch.pl --strict` on each intended unit's
patch; fix new errors/warnings before dependent work. Save logs and exit codes.
No diagnostics tool result may be claimed if that tool is unavailable.

Execute 06's A/B/C/D/E/F matrix: feature off/on; legal MMU_NOTIFIER=n stub
configuration; SMMUv3 built-in/module; independent tests=m; PAGEATTACH/OBMM
off/built-in/module; and a legal non-arm64 common-header stub compile. Check
modpost and all KUnit namespace importers. Matrix rows require actual commands
and results, not only olddefconfig success.

After all units, perform workflow self-review and design/implementation review.
For patch applicability use an isolated base tree and the supported git apply
check or isolated git am procedure; `git am --check` in the workflow must not be
assumed supported. Never apply a generated series onto its already modified
source or rewrite the shared tree. No commits without explicit authorization.

QEMU software tests use the documented virt/4 CPU/2G setup after confirming
matching Image/modules/rootfs. Require expected complete KTAP and zero unexpected
WARN/OOPS/lockdep/atomic-sleep/module errors; missing, skipped or timed-out cases
are not passes. Enable gcov in a separate preserved configuration, collect before
module unload, and match gcda/gcno/source/toolchain. Update the coverage matrix
with measurements and retain E_RAW/E_KEY/E_BYPASS_RAW/E_IO/E_ENTRY/E_CALLER/
E_KUNIT_OOM/G5 and H/V/C
exceptions. Real MMIO read-clear, CMD_SYNC completion and hardware isolation
remain platform qualification, never inferred from mock tests.

KUnit installation uses this tree's internal exact-address lookup hook, an
explicit version-pinned dependency. Activation return alone does not prove
success because resource allocation failure is unchecked in static_stub.c.
Keep the run gate closed until every replacement is verified; teardown only
verified installations. Per-case suite .exit performs ordered protected cleanup
before generic resource cleanup. A KUnit-core private-context leak on the
uninjected resource-allocation failure path remains recorded as E_KUNIT_OOM;
no feature-owned cleanup can free the inaccessible framework allocation.
