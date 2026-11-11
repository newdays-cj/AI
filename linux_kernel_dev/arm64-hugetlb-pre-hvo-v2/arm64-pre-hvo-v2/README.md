# arm64 boot-time HugeTLB pre-HVO v2

Base: `46cc79517493c0a1528fb39b566790c73366ec64`.
Branch: `OLK-6.6`.
Tip: `94b8f6cbd9250478f62d47d322e0659d0e2c62fb`.
Expected final tree: `da7f4924ffcccf8357f6f818ff435f1b40c3b293`.

Apply all eight numbered patches listed in `series`. The cover letter is
descriptive and must not be passed to git am. Original v1 commits remain
unchanged; the final three commits add the validated RAM backend option,
early static-key protection, and candidate-based budget/allocation fixes.

From the original repository, use a new checkout path:

```sh
PATCH_DIR="$PWD/.omo/patches/arm64-pre-hvo-v2"
GIT_MASTER=1 git worktree add --detach /tmp/opencode/pre-hvo-v2-replay 46cc79517493c0a1528fb39b566790c73366ec64
GIT_MASTER=1 git -C /tmp/opencode/pre-hvo-v2-replay am "$PATCH_DIR"/v2-000[1-8]-*.patch
GIT_MASTER=1 git -C /tmp/opencode/pre-hvo-v2-replay rev-parse 'HEAD^{tree}'
```

The resulting tree must equal the expected tree above. Commit IDs may
change on replay. Use a different new path if the example path exists.
From this export directory, `sha256sum -c SHA256SUMS` verifies the cover
letter, eight patches, README and series file.

Scope is arm64 4 KiB base pages and boot-configured 1 GiB HugeTLB pages,
using the existing `hugetlb_free_vmemmap=on` control. Active HugeTLB CMA and
PREEMPT_RT remain excluded. No new user-visible kernel parameter is added.

Final review approved the six-file v2 delta. Runtime evidence covers exact
126-page capacity in a 128 GiB guest at both 39-bit and 48-bit VA, lifecycle
data/flags/release, NUMA/demotion, HVO off, normal-memory reserve rejection,
failed head/table/noinit preparation, full candidate withdrawal, oversized
requests, and synthetic mirrored allocation preference. Large-pool tests
check one mapped 1 GiB page and release the entire pool; they do not touch
every byte of the pool or establish whole-system peak-memory reduction.

Physical arm64 and real mirrored RAM were not tested. The local vendor
BBM/fault-repair foundation is reused. The review-fix kernel diff has zero
checkpatch errors and four deliberate accounting/release/debug warnings.
Detailed evidence is retained in `.omo/pre-hvo-validation/REPORT-v2.md` and
`v2-QA.md` in the original workspace.

This export excludes root configuration changes, generated objects,
verification logs, and user-owned `lib/test_fortify.log`. All commits are
local; no patches were pushed or posted externally.
