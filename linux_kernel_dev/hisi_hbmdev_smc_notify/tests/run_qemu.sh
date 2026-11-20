#!/bin/bash
# run_qemu.sh — TC-08: QEMU 平台门控验证（非 HIP11 模块不加载）
# v3: 配置使用 CONFIG_COMPILE_TEST=y + CONFIG_KVM=n（绕过预存 IPIV 链接错误）
# 用法: bash tests/run_qemu.sh
set -uo pipefail

KERNEL_DIR="${KERNEL_DIR:-/home/chenjun/Git/OLK-5.10}"
ROOTFS="${ROOTFS:-/home/chenjun/tools/buildroot/output/images/rootfs.ext4}"
KERNEL_IMAGE="$KERNEL_DIR/arch/arm64/boot/Image"
LOGFILE="${LOGFILE:-/home/chenjun/workspace/hisi_hbmdev_smc_notify/tests/qemu_output.log}"
TIMEOUT=60

cd "$KERNEL_DIR"

echo "=========================================="
echo "TC-08: QEMU 平台门控验证"
echo "=========================================="

# 确保内核已构建
# 预存问题绕过：qemu_arm64_defconfig 的 CONFIG_ARM64_HISI_IPIV=y 在 ARCH_HISI=n 时
# 引用未编译符号，导致 vmlinux 链接失败。禁用 CONFIG_KVM 绕过（IPIV 引用集中在 arch/arm64/kvm/）。
# 注意：make olddefconfig 必须指定 ARCH=arm64，否则默认 x86_64 重写 .config
if [ ! -f "$KERNEL_IMAGE" ]; then
    echo "配置 + 构建内核 Image..."
    cp ~/tools/qemu_arm64_defconfig .config
    ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- scripts/config --disable CONFIG_KVM
    ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- scripts/config --enable CONFIG_COMPILE_TEST
    ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- scripts/config --enable CONFIG_HISI_HBMDEV
    ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- make olddefconfig >/dev/null 2>&1
    ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- make -j$(nproc) Image 2>&1 | tail -3
fi

test -f "$KERNEL_IMAGE" || { echo "FAIL: $KERNEL_IMAGE 不存在，构建失败"; exit 1; }

echo "启动 QEMU（超时 ${TIMEOUT}s）..."
timeout "$TIMEOUT" qemu-system-aarch64 \
    -machine virt,gic-version=3 \
    -cpu max \
    -smp 4 \
    -m 2G \
    -kernel "$KERNEL_IMAGE" \
    -drive file="$ROOTFS",format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -append "console=ttyAMA0 root=/dev/vda rw" \
    -nographic 2>&1 | tee "$LOGFILE" || true

echo ""
echo "检查 hbmdev 是否加载（预期: 无输出 = 未加载 = TC-08 PASS）..."
if grep -qi "hbmdev" "$LOGFILE"; then
    echo "FAIL: hbmdev 出现在日志中（预期非 HIP11 平台不加载）"
    grep -i "hbmdev" "$LOGFILE"
    exit 1
else
    echo "PASS: hbmdev 未加载（非 HIP11 平台门控生效）"
fi

echo ""
echo "=========================================="
echo "TC-08 通过"
echo "=========================================="
