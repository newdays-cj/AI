#!/bin/sh
set -eu

OUT=/mnt
TRACE=/sys/kernel/tracing
mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
mount -t tracefs none "$TRACE" 2>/dev/null || true
EVENT="$TRACE/events/iommu/iopf_latency"
test -f "$EVENT/format"
for field in device pasid pasid_valid grpid addr response duration_ns; do
	grep -q "$field" "$EVENT/format"
done
if grep -q cmd_ret "$EVENT/format"; then
	echo TEST_FAILURE_UNEXPECTED_CMD_RET
	exit 1
fi
cp "$EVENT/format" "$OUT/event-format.txt"
echo 0 > /sys/kernel/debug/failslab/ignore-gfp-wait
echo 0 > /sys/kernel/debug/failslab/verbose
echo 0 > "$EVENT/enable"
insmod "$OUT/kunit.ko"
insmod "$OUT/smmu_iopf_latency_test.ko"
rmmod smmu_iopf_latency_test
dmesg > "$OUT/kunit.log"
if grep -q 'not ok' "$OUT/kunit.log"; then
	echo TEST_FAILURE_KUNIT
	exit 1
fi
grep -q 'ok .*smmu-iopf-latency' "$OUT/kunit.log"

echo 1 > "$TRACE/tracing_on"
echo 1 > "$EVENT/enable"
echo > "$TRACE/trace"
insmod "$OUT/smmu_iopf_latency_test.ko" tracefs_only=1
rmmod smmu_iopf_latency_test
cat "$TRACE/trace" > "$OUT/trace-enabled.txt"
grep -q 'iopf_latency: device=smmu-iopf-latency-test' "$OUT/trace-enabled.txt"
echo 0 > "$EVENT/enable"
echo > "$TRACE/trace"
insmod "$OUT/smmu_iopf_latency_test.ko" tracefs_only=1
rmmod smmu_iopf_latency_test
cat "$TRACE/trace" > "$OUT/trace-disabled.txt"
if grep -q 'iopf_latency:' "$OUT/trace-disabled.txt"; then
	echo TEST_FAILURE_DISABLED
	exit 1
fi
dmesg > "$OUT/kunit-all.log"
if grep -q 'not ok' "$OUT/kunit-all.log"; then
	echo TEST_FAILURE_KUNIT_TRACEFS
	exit 1
fi

GCOV=/sys/kernel/debug/gcov/home/chenjun/Git/OLK-6.6
for file in drivers/iommu/io-pgfault drivers/iommu/iommu-traces drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3; do
	mkdir -p "$OUT/gcov/$(dirname "$file")"
	cp "$GCOV/$file.gcda" "$OUT/gcov/$file.gcda"
done
sync
echo SMMU_IOPF_TESTS_PASS
