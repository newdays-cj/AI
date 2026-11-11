# Pre-HVO v2 test runner and old-kernel red baseline

## Result and revision boundary

The runner now accepts validated `RAM_RESERVE=on|off`, default `on`.
The default 3072 MiB lifecycle passed (exit 0). The 131072 MiB virtual
guest requested and expected 126 1 GiB pages, obtained 125, and failed the
initial pool assertion (exit 1). An invalid option exited 2 before the
compiler or QEMU could run. These are baseline results, not v2 green
validation. The parent owns the budget fix, candidate build, and final
runtime QA, including two-node backend coverage.

All commands below ran from `/home/chenjun/Git/OLK-6.6-sp`, sequentially,
with one VM at a time. The Image is the previously built first-five-commit
tip `dc55ffbf8c05`, as identified by the existing validation report and the
supplied baseline identity; it was not rebuilt from the current source.

Exact identity command and output:

```sh
sha256sum .omo/pre-hvo-validation/arm64/arch/arm64/boot/Image
```

```text
406205e4eec00321451c1d8d406212750db09072e7fd97036eec10285ebd4f3d  .omo/pre-hvo-validation/arm64/arch/arm64/boot/Image
```

Tools observed: `/usr/bin/aarch64-linux-gnu-gcc` (Ubuntu
13.3.0-6ubuntu2~24.04.1, version 13.3.0), `/usr/bin/qemu-system-aarch64`
(8.2.2, Debian 1:8.2.2+ds-0ubuntu1.16), `/usr/bin/timeout`, and executable
`.omo/pre-hvo-validation/arm64/usr/gen_init_cpio`. Every VM invocation
compiled the unchanged guest init using `-O2 -static -Wall -Wextra -Werror`
and built its initramfs with that `gen_init_cpio`.

The retained build configuration has `CONFIG_ARM64_4K_PAGES=y`,
`CONFIG_DEFERRED_STRUCT_PAGE_INIT=y`,
`CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP=y`,
`CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP_BOOT=y`, and `CONFIG_CMA=y`.
The high-RAM case therefore explicitly passes `cma=0`.

## Scoped changes

- `tools/testing/selftests/mm/hugetlb-pre-hvo/run.sh`: validate the new
  option before temporary files, compilation, or VM execution; print its
  value; use an explicit backend and `-machine memory-backend=ram0` for
  one-node `off`; apply the property to both two-node backend objects.
  One-node default `on` keeps the original implicit backend. All values
  are quoted shell arguments; there is no eval or shell array.
- `Documentation/admin-guide/mm/hugetlbpage.rst`: document the option and
  high-RAM recipe, distinguish virtual reservation from resident RAM,
  record the baseline failure and pending fixed-revision validation.
  The existing conservative-budget description is unchanged pending the
  parent's settled implementation contract.
- This report and the ignored raw logs/console captures retain evidence.

`init.c` and `config.fragment` needed no changes. All pool, pattern,
compound-flag, shrink, negative, diagnostic, and skip assertions remain
unchanged. No kernel source, headers, Kconfig, root `.config`, or
`lib/test_fortify.log` was edited. No commit, staging, or host mount/sysctl
operation was performed.

## Invalid option: exit 2 before compilation or VM

Exact command:

```sh
RAM_RESERVE=invalid CC=/nonexistent/pre-hvo-cc QEMU=/nonexistent/pre-hvo-qemu sh tools/testing/selftests/mm/hugetlb-pre-hvo/run.sh .omo/pre-hvo-validation/arm64/arch/arm64/boot/Image .omo/pre-hvo-validation/arm64/usr/gen_init_cpio .omo/pre-hvo-validation/v2-invalid.log; status=$?; printf 'runner exit=%s\n' "$status"; test "$status" -eq 2
```

Complete output:

```text
RAM_RESERVE must be on or off
runner exit=2
```

The deliberately nonexistent compiler/QEMU paths establish that option
validation precedes both. The enclosing status assertion exited 0.

## Default small lifecycle: exit 0

Exact command:

```sh
sh tools/testing/selftests/mm/hugetlb-pre-hvo/run.sh .omo/pre-hvo-validation/arm64/arch/arm64/boot/Image .omo/pre-hvo-validation/arm64/usr/gen_init_cpio .omo/pre-hvo-validation/v2-small.log > .omo/pre-hvo-validation/v2-small.console 2>&1; status=$?; printf 'small runner exit=%s\n' "$status"; test "$status" -eq 0
```

Terminal output:

```text
small runner exit=0
```

Exact relevant console output (intervening boot messages omitted; complete
output retained in `v2-small.console`, raw serial output in `v2-small.log`):

```text
Image: /home/chenjun/Git/OLK-6.6-sp/.omo/pre-hvo-validation/arm64/arch/arm64/boot/Image
Command line: console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init panic=-1 hugepagesz=1G hugepages=1 hugetlb_free_vmemmap=on pre_hvo.expected=1 pre_hvo.case=lifecycle pre_hvo.nodes=1
Guest memory: 3072 MiB; CPUs: 2; NUMA nodes: 1
RAM reserve: on
HugeTLB: pre-HVO: 1 1GB pages, 4095 vmemmap backing pages saved (16773120 bytes)
PRE_HVO: cmdline=console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init panic=-1 hugepagesz=1G hugepages=1 hugetlb_free_vmemmap=on pre_hvo.expected=1 pre_hvo.case=lifecycle pre_hvo.nodes=1
PRE_HVO: /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages=1
PRE_HVO: /sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages=1
PRE_HVO: pattern check across 1 GiB passed
PRE_HVO: /sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages=0
PRE_HVO: head_flags=0x400029808 tail[1]=0x31910
PRE_HVO: head_flags=0x400029808 tail[64]=0x400031808
PRE_HVO: head_flags=0x400029808 tail[4096]=0x400031808
PRE_HVO: head_flags=0x400029808 tail[262143]=0x31800
PRE_HVO: /sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages=1
PRE_HVO: /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages=0
PRE_HVO: /sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages=0
PRE_HVO: /sys/kernel/mm/hugepages/hugepages-1048576kB/surplus_hugepages=0
PRE_HVO: PASS lifecycle
```

## High-RAM regression: exit 1, 125 actual versus 126 expected

Exact command:

```sh
RAM_RESERVE=off MEMORY=131072 EXPECTED_POOL=126 EXTRA_CMDLINE='cma=0' sh tools/testing/selftests/mm/hugetlb-pre-hvo/run.sh .omo/pre-hvo-validation/arm64/arch/arm64/boot/Image .omo/pre-hvo-validation/arm64/usr/gen_init_cpio .omo/pre-hvo-validation/v2-high-pool.red.log > .omo/pre-hvo-validation/v2-high-pool.red.console 2>&1; status=$?; printf 'high-RAM runner exit=%s\n' "$status"; test "$status" -eq 1
```

Terminal output:

```text
high-RAM runner exit=1
```

Exact relevant console output (intervening boot messages omitted; complete
output retained in `v2-high-pool.red.console`, raw serial output in
`v2-high-pool.red.log`):

```text
Image: /home/chenjun/Git/OLK-6.6-sp/.omo/pre-hvo-validation/arm64/arch/arm64/boot/Image
Command line: console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init panic=-1 hugepagesz=1G hugepages=126 hugetlb_free_vmemmap=on pre_hvo.expected=126 pre_hvo.case=lifecycle pre_hvo.nodes=1 cma=0
Guest memory: 131072 MiB; CPUs: 2; NUMA nodes: 1
RAM reserve: off
HugeTLB: allocating 126 of page size 1.00 GiB failed.  Only allocated 125 hugepages.
HugeTLB: pre-HVO: 125 1GB pages, 511875 vmemmap backing pages saved (2096640000 bytes)
HugeTLB: registered 1.00 GiB page size, pre-allocated 125 pages
PRE_HVO: cmdline=console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init panic=-1 hugepagesz=1G hugepages=126 hugetlb_free_vmemmap=on pre_hvo.expected=126 pre_hvo.case=lifecycle pre_hvo.nodes=1 cma=0
PRE_HVO: /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages=125
PRE_HVO: errno=0 (Success)
PRE_HVO: FAIL unexpected pool counter
Guest failure or kernel diagnostic detected
```

`REQUESTED_POOL` remained unset and defaulted to `EXPECTED_POOL=126`.
The enclosing shell assertion exited 0 because the runner correctly
returned the expected red status 1. The guest failed before mapping or
shrinking; high-RAM lifecycle success is not claimed. This independently
reproduces the short allocation in `review-high-pool-cma-off.log` without
the earlier `memblock=debug` argument. The allocation-policy root cause and
its fix remain the kernel owner's work.

## Checks and remaining parent QA

`sh -n tools/testing/selftests/mm/hugetlb-pre-hvo/run.sh` exited 0.
The unchanged C init compiled successfully during both real VM runs.
No kernel build was attempted. LSP diagnostics are unavailable in this
tool session; no diagnostics result is claimed. No additional negative,
skip, or two-node VM matrix was run here; their existing implementation
was preserved. The parent must validate the candidate Image, the final
budget documentation contract, and final runtime coverage. The pre-HVO
saved-backing diagnostic above is not a whole-system peak measurement;
128 GiB denotes guest virtual capacity, not resident host allocation.
