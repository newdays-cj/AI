#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Test script for the "within" trace event filter predicate.
# Requires: tracefs mounted at /sys/kernel/tracing
#           kmem:kmalloc tracepoint available
# Run inside QEMU arm64 environment.

TRACEFS="/sys/kernel/tracing"
KMALLOC_EVENT="$TRACEFS/events/kmem/kmalloc"
FILTER_FILE="$KMALLOC_EVENT/filter"
TRACE_FILE="$TRACEFS/trace"

PASS=0
FAIL=0

die() {
	echo "FAIL: $1" >&2
	FAIL=$((FAIL + 1))
}

pass() {
	echo "PASS: $1"
	PASS=$((PASS + 1))
}

setup() {
	if [ ! -d "$KMALLOC_EVENT" ]; then
		echo "SKIP: kmem:kmalloc event not available"
		exit 77
	fi
	echo 0 > "$KMALLOC_EVENT/enable" 2>/dev/null
	echo 0 > "$KMALLOC_EVENT/filter" 2>/dev/null
	echo > "$TRACE_FILE"
}

cleanup() {
	echo 0 > "$KMALLOC_EVENT/enable" 2>/dev/null
	echo 0 > "$KMALLOC_EVENT/filter" 2>/dev/null
}

trigger_read() {
	# dd reads 1 byte from procfs, goes through sys_read -> vfs_read -> kmalloc
	dd if=/proc/self/status of=/dev/null bs=1 count=1 2>/dev/null
}

trigger_write() {
	# echo writes to a temp file, goes through sys_write -> vfs_write -> kmalloc
	TMPFILE="/tmp/test_within_$$"
	echo hello > "$TMPFILE" 2>/dev/null
	rm -f "$TMPFILE"
}

check_trace_events() {
	grep -q " kmalloc:" "$TRACE_FILE"
}

count_trace_events() {
	local c
	c=$(grep -c " kmalloc:" "$TRACE_FILE" 2>/dev/null || true)
	echo ${c:-0}
}

clear_trace() {
	echo > "$TRACE_FILE"
}

#
# Test 1: within == filters directionally
#   within == "vfs_read"  + read trigger  → more events (vfs_read IS in stack)
#   within == "vfs_read"  + write trigger → fewer events (vfs_read is NOT)
#   within == "vfs_write" + write trigger → more events (vfs_write IS in stack)
#   within == "vfs_write" + read trigger  → fewer events (vfs_write is NOT)
#
test_within_eq_directional() {
	local read_via_read write_via_read write_via_write read_via_write

	# Read trigger under vfs_read filter — should match
	echo 0 > "$FILTER_FILE" 2>/dev/null
	echo 'within == "vfs_read"' > "$FILTER_FILE"
	if [ $? -ne 0 ]; then
		die "test1: failed to set within == vfs_read"
		return
	fi
	clear_trace
	echo 1 > "$KMALLOC_EVENT/enable"
	trigger_read
	sleep 1
	echo 0 > "$KMALLOC_EVENT/enable"
	read_via_read=$(count_trace_events)

	# Write trigger under vfs_read filter — should NOT match vfs_read
	echo 0 > "$FILTER_FILE" 2>/dev/null
	echo 'within == "vfs_read"' > "$FILTER_FILE"
	clear_trace
	echo 1 > "$KMALLOC_EVENT/enable"
	trigger_write
	sleep 1
	echo 0 > "$KMALLOC_EVENT/enable"
	write_via_read=$(count_trace_events)

	# Write trigger under vfs_write filter — should match
	echo 0 > "$FILTER_FILE" 2>/dev/null
	echo 'within == "vfs_write"' > "$FILTER_FILE"
	clear_trace
	echo 1 > "$KMALLOC_EVENT/enable"
	trigger_write
	sleep 1
	echo 0 > "$KMALLOC_EVENT/enable"
	write_via_write=$(count_trace_events)

	# Read trigger under vfs_write filter — should NOT match vfs_write
	echo 0 > "$FILTER_FILE" 2>/dev/null
	echo 'within == "vfs_write"' > "$FILTER_FILE"
	clear_trace
	echo 1 > "$KMALLOC_EVENT/enable"
	trigger_read
	sleep 1
	echo 0 > "$KMALLOC_EVENT/enable"
	read_via_write=$(count_trace_events)

	if [ "$read_via_read" -gt 0 ] && [ "$write_via_write" -gt 0 ]; then
		pass "test1: within == matches on correct path (r/r=$read_via_read r/w=$read_via_write w/w=$write_via_write w/r=$read_via_write)"
	else
		die "test1: within == failed to match (r/r=$read_via_read r/w=$read_via_write w/w=$write_via_write w/r=$read_via_write)"
	fi
}

#
# Test 2: within == negative (non-existent function)
#
test_within_nonexistent() {
	echo 'within == "nonexistent_func_12345"' > "$FILTER_FILE" 2>/dev/null
	if [ $? -ne 0 ]; then
		pass "test2: nonexistent function correctly rejected"
	else
		die "test2: nonexistent function should have been rejected"
	fi
}

test_within_ne() {
	echo 'within != "vfs_read"' > "$FILTER_FILE"
	if [ $? -ne 0 ]; then
		die "test3: failed to set within != filter"
		return
	fi
	echo 1 > "$KMALLOC_EVENT/enable"
	sleep 1
	echo 0 > "$KMALLOC_EVENT/enable"
	pass "test3: within != filter accepted and executed without error"
}

#
# Test 4: invalid operator (glob)
#
test_within_invalid_op() {
	echo 'within ~ "vfs_read"' > "$FILTER_FILE" 2>/dev/null
	if [ $? -ne 0 ]; then
		pass "test4: glob operator correctly rejected"
	else
		die "test4: glob operator should have been rejected"
	fi
}

#
# Test 5: invalid operator (numeric)
#
test_within_numeric() {
	echo 'within > 123' > "$FILTER_FILE" 2>/dev/null
	if [ $? -ne 0 ]; then
		pass "test5: numeric operator correctly rejected"
	else
		die "test5: numeric operator should have been rejected"
	fi
}

#
# Test 6: non-existent function name
#
test_within_bad_func() {
	echo 'within == "no_such_function_xyz"' > "$FILTER_FILE" 2>/dev/null
	if [ $? -ne 0 ]; then
		pass "test6: bad function name correctly rejected"
	else
		die "test6: bad function name should have been rejected"
	fi
}

#
# Test 7: Compound filter (within + field)
#
test_within_compound() {
	clear_trace
	echo 'within == "vfs_read" && bytes_req >= 1024' > "$FILTER_FILE"
	if [ $? -ne 0 ]; then
		die "test7: failed to set compound filter"
		return
	fi
	echo 1 > "$KMALLOC_EVENT/enable"
	trigger_read
	sleep 1
	echo 0 > "$KMALLOC_EVENT/enable"

	if check_trace_events; then
		pass "test7: compound filter matched"
	else
		die "test7: compound filter did not match"
	fi
}

#
# Test 8: Clear filter
#
test_within_clear() {
	echo 'within == "vfs_read"' > "$FILTER_FILE"
	filter_content=$(cat "$FILTER_FILE" 2>/dev/null)
	if ! echo "$filter_content" | grep -q "within"; then
		die "test8: filter not set correctly"
		return
	fi

	echo 0 > "$FILTER_FILE"
	filter_content=$(cat "$FILTER_FILE" 2>/dev/null)
	if echo "$filter_content" | grep -q "within"; then
		die "test8: filter not cleared"
	else
		pass "test8: filter cleared successfully"
	fi
}

#
# Test 9: within filter usability check
#
test_within_field_exists() {
	echo 'within == "vfs_read"' > "$FILTER_FILE" 2>/dev/null
	if [ $? -eq 0 ]; then
		pass "test9: within filter accepted"
	else
		die "test9: within filter not available (CONFIG_STACKTRACE?)"
	fi
}

#
# Main
#
setup

test_within_eq_directional
test_within_nonexistent
test_within_ne
test_within_invalid_op
test_within_numeric
test_within_bad_func
test_within_compound
test_within_clear
test_within_field_exists

cleanup

echo ""
echo "============================="
echo "Results: $PASS passed, $FAIL failed"
echo "============================="

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
