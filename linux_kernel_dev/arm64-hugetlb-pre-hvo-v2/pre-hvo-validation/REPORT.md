# arm64 boot-time HugeTLB pre-HVO validation

## Result

Implementation and functional verification are complete for arm64, 4 KiB
base pages, boot-configured 1 GiB HugeTLB pages, with the existing
`hugetlb_free_vmemmap=on` control. Active HugeTLB CMA uses the previous path.
This section records the implementation-stage validation. The subsequent
local commit and patch-series packaging is recorded at the end of this report.

The final NUMA boot reports two optimized pages and 8190 avoided vmemmap
backing pages (33546240 bytes). Additional page-table memory is excluded.
This is backing-allocation avoidance, not a measured whole-system peak.

## Retained final-build artifacts

| Check | Result | Artifact |
| --- | --- | --- |
| arm64 Image, NUMA/SMP/DEBUG_VM/deferred init/CMA/KEXEC_FILE/MEMORY_RELIABLE/CMA_FOLIO/HUGETLB_ALLOC_LIMIT | build exit 0 | arm64-build.log; arm64/.config |
| x86_64 SMP vmlinux, ordinary HVO and HUGETLB_ALLOC_LIMIT | build exit 0 | x86-build.log; x86/.config |
| Two NUMA nodes, node-specific 1 GiB pool, crashkernel/memmap/folio-CMA/ordinary-memory reservations | runner exit 0, lifecycle PASS, one page per node | numa-reservations.log |
| One optimized 1 GiB page, demote to 512 2 MiB pages, shrink both pools | runner exit 0, demote PASS | demote.log |
| HVO disabled | runner exit 0, lifecycle PASS, no pre-HVO diagnostic | hvo-off.log |
| Cross-zone allocation rejected with kernelcore=1536M | runner exit 0, expected empty pool PASS | zone-rejection.log |
| Final diff whitespace | exit 0 | terminal transcript |
| Kernel diff checkpatch | zero errors/warnings | terminal transcript |

The runner recompiles the guest init with static aarch64 GCC and
`-Wall -Wextra -Werror` on each invocation. Tests read and write both ends
of every 4 KiB page across a 1 GiB mapping, check compound flags including
aliased tail offsets, unmap and shrink, then power off the disposable guest.
It never changes the host hugepage pool or host sysctls.

The two boot failures found during implementation were fixed before final
validation: a cross-zone hugepage reaching ordinary HVO and an empty PTE
table being mistaken for a populated PMD block during fallback. The latter
was reproduced using scratch-only allocation-failure injection; the same
injection passed after the block-type fix and was removed.

## Prior-session evidence and limitations

The original `/tmp/opencode` artifacts were externally cleared before this
final pass. Earlier terminal evidence includes baseline boots, valid
HugeTLB-CMA fallback, ordinary-memory-budget rejection, unsupported 64 KiB
configuration build/boot to an explicit test SKIP, and the before/after
allocation-failure injection. Those raw logs are not retained here and are
not presented as final-pass artifacts.

One independent oracle gate reviewed the complete implementation and the
then-available runtime logs. Its verdict was APPROVE, with high confidence
for the inspected code and QEMU scope. It explicitly did not approve claims
of physical-hardware validation or whole-system peak-memory measurement.
The subsequent kernel source is unchanged; final x86 SMP compilation and
the retained runtime rows close its build-artifact coverage qualification.

QEMU does not supply mirrored RAM in these tests, so the reliable-memory
placement policy was compiled and source-reviewed but not exercised with
real mirrored memory. No arm64 physical machine was tested.

Existing warnings remain in include/linux/iommu.h, mm/memory.c, and mm/gmem.c;
these files were not changed. An initial x86 tiny non-SMP build hit existing
topology-symbol link failures; the final supported SMP configuration built
successfully without altering those unrelated sources.

## Workspace preservation

- Root .config SHA256: f12d713daa73a00e618caaef073f290d5cb115546a37082afbc47e0d7ba15a2c.
- User lib/test_fortify.log SHA256: 81a6fd2b7b070bfedeb1289d9bbaf97a40501da5206f33fa628be1066bc23955.
- arm64 Image SHA256: 406205e4eec00321451c1d8d406212750db09072e7fd97036eec10285ebd4f3d.
- x86 vmlinux SHA256: 57cb6b828f1720cad1af387e2fdc35ce47c3ed04457ba06b0eb2a86a78261f0c.
- No fault injection is present in the submitted source.
- This directory is ignored by git and does not form part of the patch series.

## Local commit and patch-series packaging

The validated implementation was split into five local commits on OLK-6.6:

1. 61df41a5cf94 - arm64 vmemmap PTE-table fallback fix.
2. 0c6b2d1280bc - allocation-atomic shared vmemmap backing preparation.
3. 52fd66e1201b - boot-page HVO completion and duplicate-optimization guard.
4. f358c1c0a8ab - arm64 boot pre-HVO activation and direct selftests.
5. dc55ffbf8c05 - configuration, behavior and regression documentation.

Each code-bearing commit was independently cross-built as an arm64 Image.
Build logs: series-01-build.log through series-04-build.log. The last commit
changes documentation only; no implementation changes were made during
packaging.

The expected implementation tree saved before committing, the final local
commit tree, and the tree produced by applying all five exported patches to
the base all equal 0e53aa2152b0d25535ead238942769aa7fb3139b.

Export: .omo/patches/arm64-pre-hvo/, containing a cover letter, five numbered
patches, series, README.md and SHA256SUMS. All export checksums passed.
Base commit: 46cc79517493c0a1528fb39b566790c73366ec64.
No push or external posting was performed. The only remaining untracked
file is the pre-existing user-owned lib/test_fortify.log.
