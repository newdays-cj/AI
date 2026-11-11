# pre-HVO review-revision QA ledger

Starting tip: dc55ffbf8c0527cb2d3adba637e9ad0df0de09d0.
Root .config and user lib/test_fortify.log remain out of scope.

## Red evidence

- v2-high-pool.red.log: unmodified baseline, 128 GiB virtual RAM,
  RAM_RESERVE=off, cma=0, requested and expected 126; actual 125, FAIL.
- v2-key-order-red.log: old ordering with the debug precondition inserted
  at boot_finish before its first BBM update; BUG at that assertion.
  This observes disabled protection, not a concurrent hardware panic.

## Test-authoring contract

- High-pool lifecycle protects exact requested pool capacity and usability.
  Restoring the complete unchanged budget makes it fail at 125. Existing
  small-pool cases did not cover this pressure. The same guest assertions
  are reused; no kernel-only test flag or exported getter is introduced.
- Static-key debug invariant protects a production precondition at the
  mutation boundary. Moving the increment after updates makes it fail.
  It is not a source grep, copied call-order inventory, or panic race proof.
- RAM_RESERVE controls an actual QEMU backend property needed to execute
  the large-address-space scenario; invalid values are rejected before
  compiling or running the guest. It does not alter host sysctls.

## Required green rows

| Scenario | Expected | Status |
| --- | --- | --- |
| First pre-HVO page with debug invariant | No BUG; data, flags and shrink PASS | PASS: v2-key-order-green.log and final high-pool log |
| 128 GiB, 126 pages, cma=0 | Exact total/free 126, lifecycle PASS | PASS: v2-high-pool.final.log |
| 48-bit VA, 128 GiB, 126 pages | Four-level table path and lifecycle PASS | PASS: v2-48-high-pool.log |
| Small one-page and demote | Existing assertions PASS | PASS: v2-key-order-green.log, v2-numa-final.log |
| HVO off | Legacy path and lifecycle PASS | PASS: v2-hvo-off.log |
| Two NUMA nodes, node-specific count | One page per node, lifecycle PASS | PASS: v2-numa-final.log |
| Fixed/DMA/CMA reservations | Existing priorities preserved | PASS: v2-numa-final.log |
| Ordinary-memory reserve rejection | Expected empty pool, clean boot | PASS: v2-reserve-rejection.log |
| Head allocation failure | No credit; ordinary fallback works | PASS: v2-head-failure.log |
| Partial table failure, enough ordinary budget | No aliases; ordinary fallback works | PASS: v2-table-failure.log, T=8192 bytes |
| Late candidate failure, ordinary budget insufficient | Reservation withdrawn before publication | PASS: v2-budget-withdrawal.log |
| No-init failure | No unearned saving or published invalid candidate | PASS: v2-noinit-failure.log |
| Oversized request on sparse RAM | Short allocation without prospective-budget BUG | PASS: v2-oversized-request.green.log; red BUG in .red.log |
| Mirrored block plus plain fragment | Actual mirrored backing selected first | PASS: v2-mirror-policy.valid-green.log; valid-red.log selects plain RAM |
| x86 HVO / disabled boot capability | Compile clean apart from recorded baseline warnings | PASS: v2-x86-build.log |

## Scratch injections planned before use

- Fail after one table path in vmemmap_populate_hugetlb to exercise retained
  tables and table-byte accounting, with no aliases published.
- Force only candidate 126 to fail, with hugepage_prohibit_sz near 1988M,
  to distinguish legal optimized admission from illegal ordinary fallback.
  Compare the same command without injection and keep exact counts.
- Force noinit failure without introducing an always-on production knob.
- Remove every scratch injection and diagnostic before the final build.

## Actual findings and verification boundary

- The initial v2 build failed because the worker used the private, undeclared
  memblock_alloc_range_nid_flags API. The parent replaced it with the public
  memblock_alloc_range_nid API constrained to the exact fragment already
  selected by the flags-filtered iterator. The next build passed.
- The parent also kept the universal future-budget guard scoped to a live
  boot metadata budget, preserving the legacy allocator behavior outside
  the optimized replay phase.
- v2-final-arm64-build.log and v2-48-build.log both report successful Image
  links; v2-x86-build.log reports a successful x86_64 SMP vmlinux link.
- Fault injection is scratch-only. Head-call 1 failed before any table;
  metadata-call 4 retained 8192 bytes of empty tables and ordinary fallback
  passed. No generic or DAX allocation hook was changed.
- With hugepage_prohibit_sz=1988M, the unmodified revised control passes
  126 pages (v2-budget-control.log). Failing only candidate 126 returns
  exactly 125 and explicitly logs full physical unreserve of 0x80000000
  through 0xbfffffff, preserving the normal-memory reserve. The guest then
  maps and checks one page and shrinks the entire retained pool successfully.
- All scratch injection code/logs were removed. cmp verified the four
  changed kernel source files in the build tree match the production tree.
- The first gate rejected a resource-exhaustion BUG and a mirror-preference
  bypass. Both were reproduced and corrected before a fresh review.
- A sparse layout supplied by existing memmap=exactmap has one aligned
  1 GiB bank plus small kernel/metadata banks. Requesting two BUGed at the
  prospective-credit check. The check now returns allocation failure before
  taking credit; the same layout gets one page and passes its lifecycle.
- The mirror test marks only an initially free aligned 1 GiB bank at 4 GiB,
  after payload reservation, and checks metadata backing placement. The
  initial fixture was invalid because local mark_mirror is a no-op unless
  kernelcore=mirror is set; its early logs are not used as evidence. Valid
  red/green logs both show fixture reserved=0 and active=1. Old selection
  was 0x40000000 (plain); corrected selection is 0x13ffff000 (mirrored),
  with full lifecycle PASS. This verifies synthetic flag selection, not
  physical RAM mirroring or a reliable-memory hardware configuration.
- The metadata allocator now falls back to the standard allocator after
  mirrored fragments are exhausted, including its intact mirrored-block
  preference, before ordinary memory fallback. It does not downgrade to
  plain fragments first.
- Kernel checkpatch previously reported zero errors and five BUG warnings.
  The prospective-credit resource BUG has now been removed; four deliberate
  accounting/release/debug invariants remain for review. They are not
  suppressed or described as a warning-free result.
- Hardware and whole-system peak claims remain out of scope. The key red
  proof is a runtime precondition violation, not a reproduced race panic.
