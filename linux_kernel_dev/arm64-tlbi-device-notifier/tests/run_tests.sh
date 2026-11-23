#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
set -eu

cd /prototype
dmesg -n 1
dmesg -c > /tmp/prototype-boot.log
cat /tmp/prototype-boot.log
for round in 1 2; do
	for module in mmu-notifier-test smmu-sva-test smmu-sva-callback-test; do
		case "$module" in
		mmu-notifier-test) suite=tlbi-notifier-prototype ;;
		smmu-sva-test) suite=tlbi-smmu-prototype ;;
		smmu-sva-callback-test) suite=tlbi-smmu-callbacks ;;
		esac
		printf 'PROTOTYPE_LOAD round=%s module=%s\n' "$round" "$module"
		insmod "./$module.ko"
		log="/tmp/$module-$round.log"
		dmesg -c > "$log"
		cat "$log"
		grep -Eq "ok [0-9]+ $suite$" "$log"
		awk '
		{
			sub(/^(\[[^]]*\])+ /, "")
			match($0, /[^ ]/)
			depth = RSTART - 1
			if ($0 ~ /^ *1\.\.[1-9][0-9]*$/) {
				line = $0
				sub(/^ *1\.\./, "", line)
				plans[depth] += line
				seen++
			}
			if ($0 ~ /^ *ok [0-9]+ /)
				results[depth]++
		}
		END {
			if (!seen)
				exit 1
			for (depth in plans)
				if (results[depth] != plans[depth])
					exit 1
		}' "$log"
		if grep -Eq 'not ok|# SKIP|BUG:|Oops:|Kernel panic|Unknown symbol' "$log"; then
			exit 1
		fi
		rmmod "$module"
		printf 'PROTOTYPE_UNLOAD round=%s module=%s\n' "$round" "$module"
	done
done
dmesg
printf 'PROTOTYPE_PASS\n'
