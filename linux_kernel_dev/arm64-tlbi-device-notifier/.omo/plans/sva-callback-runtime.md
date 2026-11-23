# Isolated SVA callback runtime evidence

## Scope

Continue the user-authorized isolated software prototype after its first passing
milestone. Execute the actual TLB-only, ATC-only and legacy SVA callbacks while
intercepting exactly three device-command leaves. No real MMIO/provider, DT,
architecture TLBI changes, production conditional selection, fake SVA notifier
registration, release/free callback execution, or real device commands.

Source: /home/chenjun/Git/OLK-6.6.
Dossier: /home/chenjun/workspace/arm64-tlbi-device-notifier.
Design evidence: Oracle session ses_e41dfcb16ffep5EqU6YOfbnQ1z confirmed the
three-leaf design and readiness-before-callback invariant against this tree.
No git commits or staging authorized. Preserve the completed first milestone.

## Units and ownership

1. Driver unit owns arm-smmu-v3.c and arm-smmu-v3-sva-test.h in
   drivers/iommu/arm/arm-smmu-v3/. Extend the private KUNIT-only header with a
   unique resource-name constant, scope {owned smmu, owned domain, run_enabled}
   and shared inline ownership predicate. Check in_task first, current KUnit test,
   named resource, enabled gate and exactly one non-NULL identity; put lookup
   reference. Never cast arbitrary test->priv or dereference input objects.
   Add guarded KUNIT_STATIC_STUB_REDIRECT and EXPORT_SYMBOL_IF_KUNIT at existing
   arm_smmu_tlb_inv_asid, arm_smmu_tlb_inv_range_asid and arm_smmu_atc_inv_domain.
   Move TLB cmd initializers to compound assignment after the guard, otherwise
   preserve them exactly. ATC redirect precedes its first feature access.
   KUNIT=n removes all guards/lookups/exports. No fallback or production change.
2. Test unit owns new external test sources under dossier tests/ only:
   smmu-sva-callback-cases.c, smmu-sva-callback-fixture.c and a private fixture
   header. Build as one independent smmu-sva-callback-test.ko suite named
   tlbi-smmu-callbacks. Allocate fixture before its embedded named resource;
   checked kunit_add_named_resource has NULL free and points to initialized scope.
   Ordinary fake smmu/domain are KUnit allocations with no device destructor.
   Use existing mmu_notifier_test_alloc_mm for a valid owned mm; per-case exit
   mmputs it after closing the gate and deactivating verified stubs.
   Require no preexisting replacement, exact internal-hook verification after
   each typed activation, record only verified leaves, enable only after all 3.
   A local run gate checks task/current-test, scope, all ledger bits and ownership
   immediately before actual ops callbacks. No work/async/stub change during run.
   Exit tolerates partial setup and deactivates only verified entries. Embedded
   resource remains alive for generic cleanup, which removes it before fixture
   allocation is freed. Retain E_KUNIT_OOM, never claim activation return proves
   success. No global state or real hardware access.
3. Parent owns tests/Makefile, run_tests.sh, run_qemu.sh, README and dossier
   reports/patches. Add the third module/suite to each build/archive/guest loop/
   host acceptance list; require two load/unload cycles for all three. Use
   PROTOTYPE_LOG=qemu-callback-runtime.log to preserve previous serial evidence.
   Sync changed source files to the existing clean source-prototype only after
   edits complete, build existing ARM64 baseline Image/modules and external .ko.
   Shared in-tree generated files remain untouched.

Units 1 and 2 run in parallel after plan review; they share only the exact scope
header contract defined above. Parent integrates and handles any interface
mismatch. Root AGENTS/workflow applies. Read before edits, use apply_patch.

## Test matrix

Exercise both RANGE_INV settings and independently expected zero/ordinary,
threshold-1/equal/+1, ULONG_MAX and ULONG_MAX-1 ranges. For each relevant vector:
TLB-only emits exactly ASID or range; ATC-only emits exactly ATC; legacy emits
one TLB then ATC; explicit split emits the same pair without duplicate TLB.
Use ATC replacement returns 0 and -ETIMEDOUT to prove existing void callback
return policy only, not device timeout handling. Capture fixed-size events,
overflow flag, exact smmu/domain identity, ASID/start/normalized size and range
granule=PAGE_SIZE/leaf=false. Full ATC keeps original start with normalized size0.

Predicate-only tests cover gate closed, missing resource, wrong identities and
ambiguous arguments. Partial installs of 0/1/2/3 verified stubs exercise cleanup
without invoking fake callbacks. Never probe guard fallthrough via a real leaf
with a fake object. IRQ/no-test negative fallthrough stays source-audited.

## Validation and stop

- Tools: source reads/search, apply_patch, strict checkpatch on final intended
  patch with local checkpatch-tools Python dependencies, git diff/check only
  with git-master; no suppression of warnings or edits to unrelated files.
- Compile ARM64 Image/modules plus three independent test modules; require clean
  modpost and namespace imports. Incrementally recompile main driver and SVA with
  KUNIT=n in separate existing off output and keep legal notifier-off probe.
- Parent executes existing QEMU cortex-a72/virt/GICv3/4CPU/2GiB runner, requires
  complete KTAP, successful unload/reload, no failures/skips/unexpected warnings.
  Keep the exact original registration WARN_ON_ONCE whitelist.
- Mutation-test actual callback behavior in the task-owned snapshot, e.g. omit
  ATC call from real ATC-only callback or swap legacy ordering. Require intended
  runtime assertions fail, restore bytes, rebuild and validate restored runtime.
  Preserve previous first-milestone logs and patch; no mutation in shared source.
- Source/self-review and one final correctness gate review inspect isolation,
  partial failure, test effectiveness and real QA artifacts. Momus plan approval
  is separately scoped to executability.
- Archive callback-runtime patch, self-review/report delta, new test source and
  logs. Do not convert mock returns/events into real CMD_SYNC, ATC body, device
  completion, CPU TLBI/MMIO or hardware qualification claims. No gcov/full88 claim.

Stop after current and new suites pass two cycles, mutation proof and restoration
complete, KUNIT=n compilation is checked, actual diagnostics are explained, and
the callback-evidence milestone is archived with final review. Finish and explain
any environment-blocked check rather than claiming success.
