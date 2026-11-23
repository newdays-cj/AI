# Isolated TLBI notifier prototype

Minimal-archive note: generated objects/modules and the complete source/build
snapshots are intentionally omitted. Recreate a clean kernel tree at the base
commit recorded in `../00-暂停与恢复.md`, apply `../callback-runtime.patch`, and
rebuild using the preserved configs in `../archive-inputs/` before running.

This prototype contains no MMIO status provider, DT binding, or architecture TLBI
hook. Production SMMUv3 SVA still selects its legacy combined notifier. The
conditional SMMU table is available only to KUnit tests.

## Build

Use the complete `~/tools/qemu_arm64_defconfig` baseline with `MODULES=y`,
`KUNIT=y`, `ARM_SMMU_V3=y` and `ARM_SMMU_V3_SVA=y`. Keep
`KUNIT_ALL_TESTS=n`. Tests are separate external `.ko` modules. Use a clean source
snapshot if the shared source tree has in-tree generated files; do not clean or
overwrite another task's build outputs.

From a clean source tree, with absolute output and test paths:

```sh
make O=/home/chenjun/workspace/arm64-tlbi-device-notifier/build-prototype \
    ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j20 Image modules
make O=/home/chenjun/workspace/arm64-tlbi-device-notifier/build-prototype \
    ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
    M=/home/chenjun/workspace/arm64-tlbi-device-notifier/tests modules
```

## Run

```sh
bash /home/chenjun/workspace/arm64-tlbi-device-notifier/tests/run_qemu.sh
```

The host runner combines the existing Buildroot `rootfs.cpio` with a temporary
archive containing the matching test modules and a dedicated init. It does not
modify the original rootfs. KUnit and SMMUv3 must be built into the matching
Image so that the test modules need no extra module dependencies. The guest loads
and unloads all three suites twice. Full serial output is saved as
`qemu-callback-runtime.log`; host timeout, module failures, failed/skipped tests or
unexpected warnings fail the run. The dedicated registration-exclusion test may
emit the original `__mmu_notifier_register` WARN_ON_ONCE once per boot.

The runner defaults to `cortex-a72`. On this host QEMU 8.2.2 aborts internally
with `cpu=max` before kernel console output; the same Image passes with
`cortex-a72`. `QEMU_CPU` can select a different model explicitly.
`PROTOTYPE_LOG` can select a separate serial log, for example for mutation tests.

## Evidence scope

- `tlbi-notifier-prototype` executes the real registration and post-TLBI
  dispatcher with owned mm objects and counting subscribers. It covers false/true
  delivery, old/new/both/neither, exact arguments, per-subscriber order,
  registration exclusion and removal.
- `tlbi-smmu-prototype` executes shared size normalization against independent
  boundary vectors. Its ops checks are structural only; they never invoke fake
  domain callbacks or issue device commands.
- `tlbi-smmu-callbacks` executes the actual split and legacy invalidation
  callbacks against fake ordinary-memory objects. Exactly three command leaves
  are redirected only for the current test's named owned scope, after exact
  replacement-address verification. It covers normalized arguments, ASID/range
  selection, TLB/ATC counts and order, ignored mock ATC error returns, predicate
  rejection and partial-install cleanup. No fake notifier is registered and no
  release/free callback or real device command runs.

The prototype does not measure the original 88-ID design's complete coverage.
Actual SMMU TLB/ATC commands, ATC helper body/encoding, concurrency stress,
MMIO/read-clear, CPU TLBI ordering and hardware qualification remain unverified.
This milestone does not collect gcov. Test counts do not imply branch coverage.

The initial two-suite milestone evidence remains in `qemu-prototype.log`; the
later three-suite callback evidence is separate. Consult `08-SVA回调验证.md` for
the extended milestone and `测试报告.md` for the initial milestone.
