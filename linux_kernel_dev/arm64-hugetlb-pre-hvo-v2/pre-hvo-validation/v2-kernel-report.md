# Pre-HVO v2 kernel implementation report

Status: production edits and local static checks complete. No builds or runtime
tests have been run by this worker. The parent owns cross-builds, QEMU, commits,
and series export. No files were staged and no commits were created.

The implementation follows the settled Oracle contract: increment the HVO
static key once per pre-HVO page before initializing its shared descriptors;
admit candidates using a local prospective metadata budget; populate their
mapping before publication; retire full backing and actual new page-table bytes
only after successful mapping. Failed mappings retain only their page-table
charge and must pass ordinary-page admission or release the complete payload.

Metadata placement now prefers free fragments outside complete aligned 1 GB
blocks, on the requested node first. This prevents interleaved small metadata
allocations from unnecessarily consuming the next hugepage block. The existing
zone/node/mirror/reliable payload allocator is unchanged.

Source inspection confirms that the skip-cache callers are deferred sparse
population after replay and early descriptor initialization after prepare.
Neither is called by reservation or the mapping helper, so no cache bypass is
needed while the boot list is being built.

Allowed production files: `mm/hugetlb.c`, `mm/hugetlb_vmemmap.c`,
`mm/sparse-vmemmap.c`, `mm/hugetlb_vmemmap.h`. No interface change in
`mm/sparse.c` is needed. Existing documentation/selftest worker edits and
`lib/test_fortify.log` were present on entry and are not this worker's changes.

Parent-provided red evidence, not newly executed here:

- `v2-key-order-red.log`: the old code with the boot-finish assertion BUGs.
  This proves the violated runtime precondition, not a concurrent panic.
- `v2-high-pool.red.log`: old image hash prefix `406205`, 128 GB, reserve off,
  CMA disabled, requested 126 pages but obtained 125.

## Implemented design

- `mm/hugetlb.c`: both admission checks receive the same local effective budget
  `B - (D - PAGE_SIZE)` for replayed 1 GB candidates. Bounds are checked before
  subtraction; reserve comparisons subtract instead of adding. Fresh allocation
  passes a zero metadata budget and retains the existing reserve policy.
- Candidate headers initialize `hstate` and `pre_hvo = false` before noinit and
  node/zone validation. Mapping happens during replay, before list publication
  or the caller's allocation-count increment. On success, the committed budget
  becomes `B - T - D`; on mapping failure it becomes `B - T`. Raw allocation,
  noinit, and validation failures gain no speculative backing credit.
- Failed candidates retain their ordinary allocation only if the remaining
  memory covers the committed metadata budget and normal reserve. Otherwise
  the entire memblock reservation is removed, including noinit attributes,
  before returning zero. Accounting underflows and failed full unreserve are
  explicit internal BUG invariants, never silent clamps.
- `mm/sparse-vmemmap.c`: the helper initializes the table-byte output before
  allocating the retained head. Each missing level allocates exactly one
  PAGE_SIZE page, counts it only after allocation succeeds, and uses the same
  architecture initialization/population hooks as the ordinary helpers.
  Existing and folded levels count zero. Allocated tables remain charged even
  if a later level fails; the head is freed on failure and excluded from `T`.
  All allocation and validation precedes installation of the first alias.
- The pre-HVO-only allocator tries the requested node's partial 1 GB free
  ranges, then partial ranges across nodes, preferring mirrored fragments
  before general fragments. It clips to the memblock accessible limit and
  allocates one page outside complete aligned 1 GB blocks. Only when no such
  fragment exists does it use ordinary vmemmap allocation. No allocator
  direction or global budget is temporarily changed. Generic/DAX vmemmap
  allocation and reuse functions are unchanged.
- `mm/hugetlb_vmemmap.h`: the private helper gains `unsigned long *pgtable_bytes`.
- `gather_bootmem_prealloc()` increments the static key once for each pre-HVO
  page before `hugetlb_folio_init_vmemmap()`. `mm/hugetlb_vmemmap.c` asserts the
  enabled key at boot-finish entry, before the BBM loop, and removes the old
  late increment. The existing restore decrement remains success-only.
- Late prepare counts successful mappings without remapping, clears the
  admission budget after replay, and populates only the remaining ordinary
  sections. The conservative table allowance is retained except for actual
  newly allocated table bytes, which are retired once.

## Call-graph and lifetime review

`free_area_init()` initializes node zones and masks, then calls
`hugetlb_bootmem_pre_hvo_prepare()`. Prepare replays the saved parameters into
`hugetlb_hstate_alloc_pages()` / the per-node variant, then
`alloc_bootmem_huge_page()` and the raw zoned allocator. Each valid candidate
now maps immediately. After replay finishes, prepare calls
`sparse_boot_memmap_populate()`, the first skip-cache caller. `memmap_init()`
follows prepare and reaches the other skip-cache caller in early descriptor
initialization. No reservation/helper path calls the cache. The list is stable
before either cached lookup. No defensive cache bypass was added.

The existing restore path remaps backing before clearing the optimized flag
and decrementing the key. Moving the increment preserves one reference per
optimized hugepage, including the existing shrink/demotion restoration paths.
Runtime verification of those paths remains the parent's responsibility.

## Actual checks

1. Reviewed the complete four-file production diff, the ordinary page-table
   allocation/initialization hooks, the memblock range allocation and release
   interfaces, and the reservation-to-sparse-population call sequence.
2. `GIT_MASTER=1 git diff --check -- mm/hugetlb.c mm/hugetlb_vmemmap.c mm/sparse-vmemmap.c mm/hugetlb_vmemmap.h`
   passed with no output.
3. `GIT_MASTER=1 git diff -- mm/hugetlb.c mm/hugetlb_vmemmap.c mm/sparse-vmemmap.c mm/hugetlb_vmemmap.h | perl scripts/checkpatch.pl --no-tree --no-signoff -`
   reported **0 errors, 5 warnings, 323 lines checked**. All five warnings are
   the generic warning against adding BUG variants: the three budget bounds,
   the full-reservation release check, and the required static-key assertion.
   They are retained intentionally. Budget underflow cannot safely continue
   with speculative credit; failed release must not leave an unaccounted
   payload; the key assertion is the requested debug contract. No checkpatch
   warning was suppressed or claimed to be clean.
4. Final `GIT_MASTER=1 git status --short` showed the four production edits,
   the existing documentation/selftest worker edits, and the existing
   untracked `lib/test_fortify.log`. No staged files appeared. The report lives
   under the existing `.omo/pre-hvo-validation/` directory.
5. `GIT_MASTER=1 git rev-parse HEAD` returned
   `dc55ffbf8c0527cb2d3adba637e9ad0df0de09d0`, preserving the original tip.

No compile, syntax-only compile, LSP diagnostics, QEMU, or fault-injection run
was performed. This worker has no LSP diagnostics tool. Per the task boundary,
the in-tree x86 artifacts, `.config`, the parent's root debug journal, and
`/tmp/opencode/pre-hvo-v2-src` were not modified or used for a build.
`mm/sparse.c` and the test/docs worker's files were not edited.

## Parent verification still required

- Cross-build from the parent's clean clone, including the boot optimization
  disabled configuration and allocation-limit configuration coverage.
- Run the unchanged strict pool assertion with `RAM_RESERVE=off MEMORY=131072
  EXPECTED_POOL=126 EXTRA_CMDLINE=cma=0`. A pool of 125 is still a failure.
- Run the boot-finish key assertion against the new code, then full-pool shrink
  and demotion checks. No concurrent panic claim is made from the red assertion.
- Inject head-allocation failure and each table-allocation failure in the new
  `vmemmap_alloc_hugetlb()` path. A hook only on generic
  `vmemmap_alloc_block()` will miss successful fragment allocations. Verify
  retained empty tables, no aliases on failure, exact `T`, sufficient-budget
  ordinary fallback, and insufficient-budget full candidate release.
- Exercise noinit/validation rejection, normal reserve on/off, and NUMA
  placement. These are requirements for runtime acceptance, not results
  asserted by this report.

The placement choice is deliberately confined to pre-HVO metadata: preserve
complete aligned blocks by consuming already unusable fragments first, instead
of interleaving ordinary greedy metadata allocation with hugepage allocation.
The arithmetic proof is local: a chosen address lies either in the leading
partial block of a free range or its trailing partial block. Neither address
belongs to any complete aligned 1 GB block in that range. If no fragment is
available on any permitted node, ordinary allocation remains necessary.

The implementation assumes the settled early-boot contract: replay is serial,
node/zone boundaries are established, the conservative initial budget covers
all deferred sections, and ordinary population can reuse retained empty page
tables. Those contracts were traced in source; runtime acceptance awaits the
parent checks above.
