# pre-HVO v2 review fixes

Status: final independent gate APPROVE, with no remaining blocking defect
in the reviewed six-file delta. The initial gate's two edge cases were
reproduced and corrected before approval. Original five commits through
dc55ffbf8c05 are preserved, followed by three local commits:

- 83a29f2d3d0c: validated RAM_RESERVE support for capacity tests.
- 9781eabc5e19: early static-key protection for shared boot descriptors.
- 94b8f6cbd925: candidate-based budget, fallback and mirror-policy fixes.

Final tree: da7f4924ffcccf8357f6f818ff435f1b40c3b293.
Export: .omo/patches/arm64-pre-hvo-v2/, containing the cover letter,
eight numbered patches, series, README and SHA256SUMS. All eleven checksums
passed. Applying all eight patches on base 46cc79517493 in a fresh detached
worktree reproduced the exact final tree. No push or external posting occurred.

## Fixes

1. Count each pre-HVO page before initializing its shared fake-head metadata
   and before the first arm64 BBM update. Keep the successful-restore decrement
   unchanged and assert the protection precondition with DEBUG_VM.
2. Prepare each candidate mapping before publishing the reservation. Use
   prospective saving only for admission; retire full backing and actual paid
   page-table bytes only after mapping succeeds. Failed preparation earns no
   backing credit and either fits the ordinary budget or withdraws the entire
   unpublished candidate. Preserve the ordinary-memory reserve.
3. Place tiny metadata in existing partial 1 GiB blocks where possible, while
   preserving normal mirrored-memory preference, including intact mirrored
   blocks before plain fallback. Avoid needless loss of eligible huge blocks.
4. Extend the standalone runner with validated RAM_RESERVE=on|off (default on)
   so high-address-space QEMU tests need not reserve that much host swap.

Production files: mm/hugetlb.c, mm/hugetlb_vmemmap.c,
mm/sparse-vmemmap.c, mm/hugetlb_vmemmap.h; runner and hugetlbpage.rst.
Initial pre-gate tree (superseded): d3c6fd0c201327e7859d04c95931ed3371dcc463.

## Evidence

All paths below are relative to this report's directory. Exact runner commands
for the old-kernel baseline are recorded in v2-test-report.md. QA assertions
and scope are recorded in v2-QA.md.

| Scenario | Result | Artifact |
| --- | --- | --- |
| Old ordering plus the same debug key precondition | BUG at boot_finish, expected red | v2-key-order-red.log |
| Old 128 GiB kernel, request/expect 126 | Gets 125, expected red | v2-high-pool.red.log |
| Revised first page with DEBUG_VM key precondition | PASS lifecycle | v2-key-order-green.log |
| Revised 128 GiB, cma=0, exact 126 pages | PASS lifecycle and full-pool shrink after both edge fixes | v2-high-pool.revised-final.log |
| 48-bit VA, 128 GiB, exact 126 pages | PASS lifecycle after both edge fixes | v2-48-high-pool.final.log |
| Two NUMA nodes, requested 0:1,1:1, RAM_RESERVE=off, crashkernel/memmap/folio-CMA/prohibit reservations | PASS node counts, data, demotion and shrink | v2-numa-final.log |
| HVO off | PASS legacy lifecycle | v2-hvo-off.log |
| 3 GiB guest, 2 GiB normal reserve | PASS expected empty pool | v2-reserve-rejection.log |
| Head allocation fails before tables | PASS ordinary fallback | v2-head-failure.log |
| Fourth metadata allocation fails, T=8192 retained | PASS ordinary population and lifecycle | v2-table-failure.log |
| 128 GiB with prohibit=1988M, no injection | PASS exact 126 | v2-budget-control.final.log |
| Same pressure, only candidate 126 forced to fail | PASS exact 125 and full unreserve before publication | v2-budget-withdrawal.log |
| noinit marking forced to fail without changing flags | PASS ordinary fallback, no saving claimed | v2-noinit-failure.log |
| Oversized request on sparse RAM | Old guard BUGs; revised guard returns a short pool and lifecycle PASS | v2-oversized-request.red.log; v2-oversized-request.green.log |
| Valid synthetic mirrored bank with an ordinary fragment | Old helper selects plain RAM; revision selects mirrored backing and passes | v2-mirror-policy.valid-red.log; v2-mirror-policy.valid-green.log |
| arm64 final Image | Build exit 0 | v2-revised-final-build.log |
| arm64 48-bit VA Image | Build exit 0 | v2-48-revised-build.log |
| x86_64 SMP HVO, boot optimization compiled out | Build exit 0 | v2-x86-revised-build.log |
| Intermediate static-key commit 9781eabc5e19 | arm64 Image build exit 0 | v2-intermediate-key-build.log |

The large test maps and checks one 1 GiB page, then releases the entire pool.
It does not read/write every byte of 126 GiB. Pool and flag assertions are not
weakened. Only one VM runs at a time.

The static-key red case observes a violated runtime precondition, not an actual
concurrent-hardware panic. The budget green case demonstrates honored pool
capacity and usability, not whole-system peak-memory reduction.

## Build and cleanup details

- A worker initially called an undeclared private memblock API. The parent
  confirmed the compile error and replaced it with the public range allocator
  constrained to the exact flags-filtered fragment; the next build passed.
- The parent kept the future-budget guard scoped to a live boot budget so
  unsupported/disabled boot paths retain their prior allocation semantics.
- Scratch-only head/table/noinit/last-candidate injections and diagnostics
  were removed. The build-source files were compared against production.
- The first gate correctly rejected the prospective-credit BUG as normal
  resource exhaustion, not an invariant. It now returns allocation failure.
  The other blocker was mirror preference; standard mirrored allocation
  now precedes plain fallback. The fresh gate reviewed and approved those
  deltas against the actual source snapshot and execution logs.
- Early mirror-fixture logs did not actually mark mirror memory because
  local mark_mirror requires kernelcore=mirror. They are invalid as mirror
  evidence; only the explicitly named valid-red/valid-green logs are used.
- Four deliberate BUG-invariant warnings remain for accounting/unreserve
  and the DEBUG_VM key check. They are not suppressed or called clean.
- Existing unrelated warnings in iommu.h, memory.c and gmem.c remain.
- Root .config and user-owned lib/test_fortify.log were not changed.
  Final SHA256 values respectively:
  f12d713daa73a00e618caaef073f290d5cb115546a37082afbc47e0d7ba15a2c;
  81a6fd2b7b070bfedeb1289d9bbaf97a40501da5206f33fa628be1066bc23955.
- No physical arm64 machine or real mirrored RAM was tested. Existing vendor
  arm64 BBM/fault-repair semantics are reused, not replaced by the newer
   mailing-list AF-based proposal.

## Final review

Oracle gate bg_09207e55, session ses_e417bae4cffe5ve8BgX89d4lk7,
returned APPROVE with high confidence for the scoped v2 changes and no
required code changes. Review used the locked source snapshot at
/tmp/opencode/pre-hvo-v2-gate and inspected the existing build/runtime logs;
the reviewer did not rerun those commands. It confirmed resource-exhaustion
handling, mirrored allocation preference, early key accounting, actual
page-table accounting and full candidate withdrawal. Its approval does not
extend to physical mirroring, hardware BBM races or whole-system peak memory.
