#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

tests=$(dirname "$(readlink -f "$0")")
project=$(dirname "$tests")
build=${1:-"$project/build-prototype"}
rootfs=${2:-"$HOME/tools/buildroot/output/images/rootfs.cpio"}
cpu=${QEMU_CPU:-cortex-a72}
image="$build/arch/arm64/boot/Image"
test -f "$image"
test -f "$rootfs"
test -f "$tests/mmu-notifier-test.ko"
test -f "$tests/smmu-sva-test.ko"
test -f "$tests/smmu-sva-callback-test.ko"
stage=$(mktemp -d "$project/prototype-initramfs.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
mkdir "$stage/prototype"
cp "$tests/"{mmu-notifier-test.ko,smmu-sva-test.ko,smmu-sva-callback-test.ko,run_tests.sh} "$stage/prototype/"
cp "$tests/guest-init.sh" "$stage/prototype-init"
chmod 755 "$stage/prototype-init"
cp "$rootfs" "$stage/rootfs.cpio"
(
	cd "$stage"
	printf '%s\n' prototype prototype/mmu-notifier-test.ko \
		prototype/smmu-sva-test.ko prototype/smmu-sva-callback-test.ko \
		prototype/run_tests.sh prototype-init |
		cpio -o -H newc --owner=0:0 >> rootfs.cpio
)
log=${PROTOTYPE_LOG:-"$project/qemu-callback-runtime.log"}
timeout 300 qemu-system-aarch64 \
	-machine virt,gic-version=3 -cpu "$cpu" -smp 4 -m 2G \
	-kernel "$image" -initrd "$stage/rootfs.cpio" \
	-append 'console=ttyAMA0 rdinit=/prototype-init panic=-1' \
	-nographic -no-reboot 2>&1 | tee "$log"
grep -q '^PROTOTYPE_EXIT=0' "$log"
grep -q '^PROTOTYPE_PASS' "$log"
for suite in tlbi-notifier-prototype tlbi-smmu-prototype tlbi-smmu-callbacks; do
	count=$(grep -Ec "ok [0-9]+ $suite" "$log")
	test "$count" -eq 2
done
if grep -Eq 'not ok|# SKIP|BUG:|Oops:|Kernel panic|Unknown symbol|scheduling while atomic|possible circular locking' "$log"; then
	exit 1
fi
# Only the registration validation's shared WARN_ON_ONCE site is expected.
warnings=$(grep 'WARNING:' "$log" || true)
if test -n "$warnings"; then
	test "$(printf '%s\n' "$warnings" | wc -l)" -eq 1
	printf '%s\n' "$warnings" | grep -q '__mmu_notifier_register'
fi
printf 'QEMU prototype validation passed; log: %s\n' "$log"
