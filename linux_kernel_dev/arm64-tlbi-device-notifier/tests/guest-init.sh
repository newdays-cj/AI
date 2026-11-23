#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
export PATH=/sbin:/bin:/usr/sbin:/usr/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
exec </dev/console >/dev/console 2>&1
sh /prototype/run_tests.sh
status=$?
printf 'PROTOTYPE_EXIT=%s\n' "$status"
sync
poweroff -f
