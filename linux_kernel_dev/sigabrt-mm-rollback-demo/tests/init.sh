#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mount -t debugfs debugfs /sys/kernel/debug
mkdir -p /sys/fs/cgroup /tmp/ckpt-freezer
mount -t cgroup2 none /sys/fs/cgroup
mount -t cgroup -o freezer none /tmp/ckpt-freezer
echo "CKPT_QEMU_START"
if insmod /task_checkpoint_test.ko; then
    echo "CKPT_KUNIT_LOADED"
    dmesg
else
    echo "CKPT_KUNIT_FAILED"
fi
/checkpoint-demo
result=$?
/checkpoint-edges || result=1
/checkpoint-external || result=1
echo "CKPT_QEMU_EXIT=$result"
attempt=0
while [ "$(cat /sys/kernel/debug/task_checkpoint_demo/live_groups)" != 0 ] && [ "$attempt" -lt 8 ]; do
    sleep 1
    attempt=$((attempt + 1))
done
echo "CKPT_LIVE_GROUPS=$(cat /sys/kernel/debug/task_checkpoint_demo/live_groups)"
echo "CKPT_HELD_TASKS=$(cat /sys/kernel/debug/task_checkpoint_demo/held_tasks)"
mkdir -p /mnt/checkpoint
if mount -t 9p -o trans=virtio,version=9p2000.L checkpoint /mnt/checkpoint; then
    for file in $(find /sys/kernel/debug/gcov -name task_checkpoint.gcda -o -name task_checkpoint_test.gcda); do
        relative=${file#/sys/kernel/debug/gcov/}
        mkdir -p "/mnt/checkpoint/gcov/$(dirname "$relative")"
        cp "$file" "/mnt/checkpoint/gcov/$relative"
    done
    echo "CKPT_GCOV_COLLECTED"
else
    echo "CKPT_GCOV_FAILED"
fi
sync
poweroff -f
