#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
set -eu

if [ "$#" -ne 3 ]; then
	echo "Usage: sh $0 IMAGE GEN_INIT_CPIO LOG" >&2
	echo "Environment: CASE=lifecycle|nohuge|demote HVO=on|off EXPECTED_POOL=1 TIMEOUT=300 EXPECT_LOG='literal text'" >&2
	echo "             REQUESTED_POOL=EXPECTED_POOL EXTRA_CMDLINE='kernel parameters' MEMORY=3072 (MiB) CPUS=2 NODES=1|2" >&2
	echo "             RAM_RESERVE=on|off (default on)" >&2
	exit 2
fi
kernel=$(realpath "$1")
cpio=$(realpath "$2")
log=$3
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
case_name=${CASE:-lifecycle}
hvo=${HVO:-on}
expected=${EXPECTED_POOL:-1}
requested=${REQUESTED_POOL:-$expected}
memory=${MEMORY:-3072}
cpus=${CPUS:-2}
nodes=${NODES:-1}
ram_reserve=${RAM_RESERVE-on}
case "$ram_reserve" in
	on|off) ;;
	*) echo 'RAM_RESERVE must be on or off' >&2; exit 2 ;;
esac
for value in "$memory" "$cpus"; do
	case "$value" in
		''|0*|*[!0-9]*)
			echo 'MEMORY and CPUS must be positive decimal integers (no leading zeros)' >&2
			exit 2
			;;
	esac
done
case "$case_name" in lifecycle|nohuge|demote) ;; *) exit 2 ;; esac
case "$hvo" in on|off) ;; *) exit 2 ;; esac
case "$expected" in ''|*[!0-9]*) exit 2 ;; esac
case "$requested" in ''|*[!0-9:,]*) exit 2 ;; esac
case "$nodes" in 1|2) ;; *) exit 2 ;; esac
if [ "$nodes" = 2 ] && { [ "$((memory % 2))" -ne 0 ] || [ "$((cpus % 2))" -ne 0 ]; }; then
	echo 'Two NUMA nodes require even MEMORY and CPUS values' >&2
	exit 2
fi
if [ "$case_name" = nohuge ] && [ "$expected" != 0 ]; then
	echo 'nohuge requires EXPECTED_POOL=0' >&2
	exit 2
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
"${CC:-aarch64-linux-gnu-gcc}" -O2 -static -Wall -Wextra -Werror \
	-o "$tmp/init" "$src/init.c"
printf 'dir /proc 0555 0 0\ndir /sys 0555 0 0\nfile /init %s 0755 0 0\n' \
	"$tmp/init" > "$tmp/list"
"$cpio" "$tmp/list" > "$tmp/initramfs.cpio"
append="console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init panic=-1 hugepagesz=1G hugepages=$requested hugetlb_free_vmemmap=$hvo pre_hvo.expected=$expected pre_hvo.case=$case_name pre_hvo.nodes=$nodes"
if [ -n "${EXTRA_CMDLINE:-}" ]; then
	append="$append $EXTRA_CMDLINE"
fi
printf 'Image: %s\nCommand line: %s\n' "$kernel" "$append"
printf 'Guest memory: %s MiB; CPUs: %s; NUMA nodes: %s\n' "$memory" "$cpus" "$nodes"
printf 'RAM reserve: %s\n' "$ram_reserve"
set -- -machine virt -cpu max -m "$memory" -smp "$cpus" -nographic -no-reboot -monitor none
if [ "$nodes" = 2 ]; then
	set -- "$@" \
		-object "memory-backend-ram,id=ram0,size=$((memory / 2))M,reserve=$ram_reserve" \
		-object "memory-backend-ram,id=ram1,size=$((memory / 2))M,reserve=$ram_reserve" \
		-numa "node,nodeid=0,memdev=ram0,cpus=0-$((cpus / 2 - 1))" \
		-numa "node,nodeid=1,memdev=ram1,cpus=$((cpus / 2))-$((cpus - 1))"
elif [ "$ram_reserve" = off ]; then
	set -- "$@" \
		-object "memory-backend-ram,id=ram0,size=${memory}M,reserve=off" \
		-machine memory-backend=ram0
fi
status=0
timeout --kill-after=10 "${TIMEOUT:-300}" "${QEMU:-qemu-system-aarch64}" \
	"$@" -kernel "$kernel" -initrd "$tmp/initramfs.cpio" \
	-append "$append" > "$log" 2>&1 || status=$?
cat "$log"
tr -d '\r' < "$log" > "$tmp/check.log"
if [ "$status" -ne 0 ]; then
	echo "QEMU failed or timed out: status=$status" >&2
	exit 1
fi
if grep -Eq 'PRE_HVO: FAIL|Kernel panic|BUG:|WARNING:|Oops:|Unable to handle kernel' "$log"; then
	echo 'Guest failure or kernel diagnostic detected' >&2
	exit 1
fi
if grep -Eq '^PRE_HVO: SKIP ' "$log"; then
	echo 'Guest capability skip (not a pass)' >&2
	exit 4
fi
if ! grep -qx "PRE_HVO: PASS $case_name" "$tmp/check.log"; then
	echo 'Missing guest PASS marker' >&2
	exit 1
fi
if [ -n "${EXPECT_LOG:-}" ] && ! grep -Fq -- "$EXPECT_LOG" "$log"; then
	echo 'Missing requested kernel log text' >&2
	exit 1
fi
