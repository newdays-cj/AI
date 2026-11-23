# Isolated software prototype

## Authorization and boundary

The user explicitly selected "先做隔离软件原型" after the hardware-input gate.
This changes the earlier all-production-code gate for this bounded prototype.
Source root: /home/chenjun/Git/OLK-6.6.
Dossier: /home/chenjun/workspace/arm64-tlbi-device-notifier.
No commits, real MMIO provider/DT binding, arch TLBI interception, production
conditional SVA selection, or hardware qualification are included.

The selected TLB/ATC split remains unchanged. The prototype implements generic
conditional dispatch and the SMMUv3 decomposition while production SVA continues
using its legacy combined ops. There is no status register read or activation.

## Work units

1. MM unit owns include/linux/mmu_notifier.h and mm/mmu_notifier.c plus
   dossier tests/mmu-notifier-test.c. Add the four-argument conditional ops slot,
   post_tlbi wrapper with bool need_tlb and MMU_NOTIFIER=n stub, and core one-SRCU
   traversal calling conditional then old per subscription. Preserve the old
   dispatcher. Extend registration exclusion to either TLB slot versus start/end.
   Core gets EXPORT_SYMBOL_IF_KUNIT, no production export. Tests use real owned
   mm and counter-only subscribers, cover empty/old/new/both/neither, false/true,
   mixed subscriptions, exact arguments, per-subscriber order, legal registration,
   all nine invalid combinations and no callback after unregister. Cleanup tracks
   successful registrations and unregisters before mm/storage release. Expected
   WARN_ON_ONCE is at most once for the exact registration site on dedicated boot.
   External modules cannot call unexported mm_alloc directly; use a narrow
   KUnit-only mmu_notifier_test_alloc_mm wrapper in the core with the same test
   namespace, without generally exporting mm_alloc.
2. SMMU unit owns drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3-sva.c, a narrow
   driver-private test header and dossier tests/smmu-sva-test.c. Extract shared
   size normalization, TLB-only and ATC-only callbacks; combined legacy calls
   TLB then ATC. Keep existing allocation ops assignment and release/free intact.
   Add KUnit-only conditional table/accessor and size helper visibility using
   VISIBLE_IF_KUNIT/EXPORT_SYMBOL_IF_KUNIT. Independent test module checks real
   size normalization boundaries and ops split/lifecycle shape. Fake domain and
   SMMU objects are used only for pure size computation and pointer inspection;
   never invoke their notifier/release/device operations. This minimum prototype
   does not claim S4-S6 callback runtime or device command coverage.
3. Parent owns tests/Makefile, tests/run_tests.sh, tests/run_qemu.sh, tests/README.md
   and evidence documents. Build independent external .ko modules with Kbuild;
   add no in-image tests. Use baseline ~/tools/qemu_arm64_defconfig and dedicated
   O=build-prototype output. Enable KUNIT, MODULES, SMMUv3/SVA, inspect final
   olddefconfig values, then build ARM64 Image/modules and external modules.
   Test suites run in QEMU with no provider/SMMU hardware declaration. Use a
   disposable initramfs derived from existing buildroot rootfs.cpio, adding only
   matching modules and runner; never modify the existing rootfs image. Require
   complete KTAP for both expected suites and successful unload. Preserve logs.

Units 1 and 2 run in parallel after this plan's Momus review; their source paths
are disjoint. Unit 2 may refer to the new slot contract but builds wait for both.
Read root instructions; no subordinate instructions were found by the planning
agent beneath these units. Preserve unrelated user files.

## Validation

Tools: local source reads and apply_patch for code; git diff/status only after
git-master; checkpatch --strict on the intended patch and new tests; Kbuild with
ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-; QEMU 8.2.2.
No generated files in source root. Each build/test command saves log and status.

1. Compile Image/modules from full baseline plus necessary prototype test options.
2. Compile the two independent test modules; require modpost namespace resolution.
3. Compile relevant production objects with KUNIT disabled in a separate output
   configuration. Exercise the public no-op header in a legal MMU_NOTIFIER=n
   configuration, recording effective values rather than assuming selects allow it.
4. Boot virt,gic-version=3/cpu=max/4 CPUs/2G with new Image and disposable
   initramfs, load dependencies and test modules, require complete expected KTAP
   and no unexpected WARN/OOPS/lockdep/module errors. Unload and rerun both suites.
5. Review actual diff for correctness, lifetime and isolation; submit plan and
   implementation evidence to Momus for workflow consistency, recording its scope.
6. Archive feature/test diffs without commits, configs, build logs, serial logs,
   self-review and test report. Update 00 and requirement authorization history.

No gcov or full 88-ID runtime claim belongs to this minimum milestone: measured
evidence covers N1-N4 and S3, with S2 structural checks only. P/T/K hardware and
seam paths, actual split callback commands, allocation selection and concurrency
stress remain outside this milestone and explicitly unmeasured. Future tests
must not mistake table inspection for executed TLB/ATC behavior.

Stop when both independent modules build and pass observable QEMU software tests,
required compilation/style checks pass or unrelated failures are explained, and
the scope/coverage report is archived. If environment prevents an intended check,
finish other units and state the precise blocker without claiming that check.

Final style disposition: strict full-patch checkpatch records 0 errors,
1 FILE_PATH_CHANGES advisory and 0 checks, exit 1. The new private header is
already covered by MAINTAINERS ARM SMMU DRIVERS F: drivers/iommu/arm/.
Record this accepted ownership advisory explicitly without suppressing it or
claiming zero warnings. Local unpacked Python dependencies enable SPDX checks;
final logs must contain no dependency traceback.

## Completion evidence and consistency checkpoint

Review the bounded plan against actual kernel delta `prototype.patch`, external
test sources in tests/, `07-自审结论.md`, `测试报告.md`, and prototype evidence
appendix in 覆盖率.md. Confirm that the implementation/test deliverable matches
this explicitly smaller prototype and that exclusions are not marked passed.
Check runtime evidence in qemu-prototype.log and qemu-prototype-restored.log,
mutation-summary.txt, build-status and off/stub logs, and the truthful final
checkpatch diagnostic. Record whether this checkpoint is only a plan
executability verdict or also discusses implementation/test/report consistency;
never relabel plan approval as hardware or complete production acceptance.
