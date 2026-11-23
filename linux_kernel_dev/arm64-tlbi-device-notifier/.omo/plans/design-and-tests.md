# Revised ARM64 TLBI Status: Detailed Design and Test Design

## Goal and Limit

Produce the phase-5 detailed design and phase-6 test design from the user's new
hardware layout and SMMUv3 consumer decisions. This execution plan authorizes
documentation only, not kernel implementation, fabricated DT hardware, commits,
or claims of successful runtime testing. Human confirmation gates are preapproved;
technical review and verification remain required.

Artifacts live under /home/chenjun/workspace/arm64-tlbi-device-notifier/.
Kernel root: /home/chenjun/Git/OLK-6.6.
Workflow: /home/chenjun/my-env/AI/linux_kernel_dev/需求开发.md.
The earlier continuation.md covers a historical blocked handoff, not approval of
the revised feature. New requirements supersede old unknowns in historical 02-04.

## Confirmed Inputs

- User-mm Stage-1 TLBI, issuing CPU only, read after its completion DSB.
- Bit 0 accumulates across the current logical invalidation; after DSB its state
  is readable without additional hardware synchronization.
- Independent per-CPU 32-bit little-endian register; the entire register read-clears.
- A dedicated DT node has a reg array with one register resource per CPU.
- Actual board addresses remain TBD; software can draft the CPU association ABI.
- SMMUv3 SVA reuses arm_smmu_mm_arch_invalidate_secondary_tlbs(). A ready, supported
  SMMU subscribes to ONLY the new conditional callback; off/unsupported uses its
  old callback. Other subscribers and SMMU release/free remain intact.

Whole-register read-clear does not by itself establish concurrent event-wins
atomicity. Hardware event ownership, other bits, NMI/higher-EL interference,
initial/reset state, power access and the set of covered SMMUs remain distinct
prerequisites; do not call them user-confirmed. Early immutable readiness,
parallel CPU phandles and per-SMMU opt-in are design proposals, not hardware facts.

## Tasks and Dependencies

1. Update 01-需求描述.md and 00-进度与阻塞.md with new confirmed inputs, user-selected
   exclusive SMMU routing, historical review scope and remaining prerequisites.
   Do not repeat the obsolete no-consumer, unknown-endian or unknown-DSB-visibility
   blockers. Preserve original automatic human approval and no-commit rule.
2. Obtain Oracle design decisions, then write 05-实现方案.md. Define draft APIs,
   data structures, files, DT/resource rules, readiness/lifetime state machine,
   CPU/IRQ critical interval, logical operation/range semantics, error handling,
   SMMU routing, unchanged generic subscribers, and function-level change inventory.
   Cover young/contpte, full-mm fallback and reclaim deferral explicitly. Do not
   depend on single-mm reclaim batches or remote DSB completing prior CPU work.
3. Using 05's function inventory, write 06-测试设计.md and 覆盖率.md. Every proposed
   new/modified function and branch must have a TC or an explicitly blocked reason.
   Design KUnit as independent modules and QEMU software integration separately
   from real MMIO/ATS/TLB hardware qualification. No execution results are invented.
4. Submit this plan and linked design artifacts to Momus; fix concrete review
   issues, record exact approval scope. Hardware prerequisites may remain in a
   clearly conditional design, but they prevent production implementation approval.
5. Read all changed files, verify references, test/coverage traceability and kernel
   tree integrity. Update 00 with the design/test-review outcome and next boundary.

Dependency graph: 1 -> Oracle -> 2 -> 3 -> 4 -> 5. DT and SMMU evidence gathering
can run independently before 2; no implementation depends on an unfinished review.

## Source Anchors

Relative to the kernel root:

- arch/arm64/include/asm/tlbflush.h:253,265,327,418,500: logical full-mm/page/range,
  no-sync and reclaim batch completion; arch/arm64/include/asm/tlbbatch.h:5.
- arch/arm64/include/asm/pgtable.h:974 and arch/arm64/mm/contpte.c:511: young-bit
  invalidations omit a final DSB today and must not be silently excluded.
- mm/rmap.c:648,1791,2154: per-mapping mm and synchronous/deferred branch selection.
- include/linux/mmu_notifier.h:190 and mm/mmu_notifier.c:588,620: callback context,
  SRCU dispatch and existing registration exclusions.
- drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3-sva.c:132,201,362,391: existing flush,
  ops, lifetime and registration. Reuse does not erase its existing error policy.
- drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:1222,2321,2409 and CMDQ sync paths:
  distinguish successful synchronous completion from ignored/logged timeout errors.
- drivers/misc/zcopy/zcopy.c:133 and drivers/ub/obmm/obmm_cache.c:152: downstream
  user-mm raw-TLBI producers. The revised design includes GPL-exported active-or-
  fallback range/mm wrappers (P9/P10) plus Z1/O1 caller changes so modular users do
  not reference the unexported raw notifier dispatcher. Inactive paths stay intact.
- include/linux/of.h, include/linux/of_address.h and drivers/of/cpu.c: CPU/resource
  parsing; DT array order is not a Linux CPU number.

## Executable QA

QA-1 (task 1): Use Read on 00 and 01. Use Bash/rg for '小端|整个|累积|SMMU|DSB'
against those files. Expected: each confirmed input appears and old reviews are
historical; no assertion that whole-readclear proves concurrent atomicity.

QA-2 (task 2): Read the actual Oracle output and 05. Compare each source anchor
with Read and rg. Expected: explicit function signatures, transition/error tables,
reg/CPU pairing, config-off behavior and all user invalidation paths. CPU pin and
IRQ exclusion begin before the first TLBI and end after one status read; callbacks
run after restoration of the caller's states. Reject dual registration for the
same conditional SMMU and global suppression of other old subscribers. Document
DSB-relative timing changes and existing SMMU timeout semantics accurately.

QA-3 (task 3): Use Read and rg 'TC-|blocked|未执行' on 06 and 覆盖率.md. Cross-check
every 05 inventory entry against the matrix. Expected: all functions/branches
mapped, hierarchical TC identifiers, concrete setup/action/observable expectation,
separate software-mock versus hardware proof and unexecuted results. Do not label
all tests as passed or call modelled memory real read-clear hardware.

Concrete traceability commands, run from the dossier root:

```sh
comm -23 <(rg --no-filename -o 'TC[0-9]{2}(\.[0-9]{2}){2}' 覆盖率.md | sort -u) <(rg '^\| TC[0-9]{2}(\.[0-9]{2}){2} \| 场景' 06-测试设计.md | rg -o 'TC[0-9]{2}(\.[0-9]{2}){2}' | sort -u)
comm -23 <(rg '^\| (P[0-9]+|N[0-9]+|T[0-9]+|S[0-9]+|K[0-9]+|Z1|O1) ' 05-实现方案.md | rg -o '^\| [A-Z][0-9]+' | sort -u) <(rg '^\| (P[0-9]+|N[0-9]+|T[0-9]+|S[0-9]+|K[0-9]+|Z1|O1) ' 覆盖率.md | rg -o '^\| [A-Z][0-9]+' | sort -u)
```

Expected: both commands exit 0 with empty output; all 28 inventory IDs are present
in the matrix. Also manually verify branch expectations, notably >= versus >
range-limit boundaries, logical full-mm sentinel handling and module-safe stubs.
Matching identifiers alone does not prove test execution or branch correctness.

G1-G4 in 06 identify missing KUnit-only seam designs; G5 records unsafe original
failure branches. They remain visible technical gates, not completed tests. This
plan requests review of a conditional documentation milestone, not permission to
start implementation with unresolved seam or hardware prerequisites.

QA-4 (task 4): Use background_output only after actual completion notification.
Compare real Momus/Oracle results to 00's transcription. Expected: exact verdict,
scope, and remaining blockers. Plan approval is not runtime feature validation.

QA-5 (task 5): Read all updated/new Markdown and inspect links. From the kernel
root run GIT_MASTER=1 git status --short and GIT_MASTER=1 git diff --stat HEAD --.
The historical baseline was only ?? AGENTS.md, but concurrent work now has staged
drivers/iommu/io-pgtable-arm.c and include/linux/io-pgtable.h, plus untracked
Documentation/admin-guide/bootperf.rst. Record these observations without claiming
the tree is clean or attributing their authorship. This documentation task writes
only the external dossier. Preserve the observed kernel files; SHA256 them now
and again at handoff if useful, and record rather than reset further concurrent
differences. Expected: no kernel changes made by this task and no unrelated
changes reverted/staged/committed. A nonzero diff against HEAD for the noted work
is not a failed kernel test. No C diagnostics or build/test claim is made for
documentation-only changes.

## Done Criteria and Future Work

05, 06 and the branch matrix exist, are mutually consistent, reviewed and linked
from the progress summary. All confirmed facts are archived without inventions.
Any unconfirmed hardware requirements or failure-policy decisions are named as
implementation gates. The user receives the concrete DT pairing and design result
without another interview about already supplied facts.

Kernel coding, DT binding installation, KUnit implementation, build and QEMU runs
are FUTURE work under a separately reviewed implementation plan and resolved
prerequisites. No commit is authorized. Test design may specify commands, but must
not report their execution or hardware success.
