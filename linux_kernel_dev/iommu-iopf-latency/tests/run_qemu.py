#!/usr/bin/env python3
"""Boot a private initramfs and run the tests using only Python's stdlib."""
import os
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parent
KERNEL = Path("/home/chenjun/Git/OLK-6.6")


def main():
    shutil.copy2(KERNEL / "lib/kunit/kunit.ko", ROOT / "kunit.ko")
    cmd = [
        "qemu-system-aarch64", "-machine", "virt,gic-version=3,iommu=smmuv3",
        "-cpu", "cortex-a76", "-smp", "4", "-m", "3G", "-nographic", "-no-reboot",
        "-kernel", str(KERNEL / "arch/arm64/boot/Image"),
        "-initrd", "/home/chenjun/tools/buildroot/output/images/rootfs.cpio.gz",
        "-append", "console=ttyAMA0 rdinit=/bin/sh nokaslr panic=0",
        "-virtfs", f"local,path={ROOT},mount_tag=tests,security_model=none,id=tests",
    ]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=0)
    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    buf = b""
    sent = False
    deadline = time.monotonic() + 600
    try:
        with (ROOT / "qemu_output.log").open("wb") as log:
            while time.monotonic() < deadline:
                for key, _ in selector.select(timeout=1):
                    data = os.read(key.fd, 65536)
                    if not data:
                        raise RuntimeError(f"QEMU exited early: {proc.poll()}")
                    log.write(data)
                    log.flush()
                    buf += data
                    if b"Kernel panic" in buf:
                        raise RuntimeError("guest kernel panic; see qemu_output.log")
                    if not sent and b"# " in buf:
                        setup = (
                            "mount -t proc none /proc; mount -t sysfs none /sys; "
                            "mount -t devtmpfs none /dev; mkdir -p /mnt; "
                            "mount -t 9p -o trans=virtio,version=9p2000.L tests /mnt; "
                            "sh /mnt/run_tests.sh; echo RUN_EXIT=$?\n"
                        )
                        proc.stdin.write(setup.encode())
                        proc.stdin.flush()
                        sent = True
                        buf = b""
                    if sent and re.search(rb"(?:\r?\n)RUN_EXIT=\d+", buf):
                        status = int(re.search(rb"(?:\r?\n)RUN_EXIT=(\d+)", buf)[1])
                        if status or b"SMMU_IOPF_TESTS_PASS" not in buf:
                            raise RuntimeError(f"guest tests failed, exit={status}")
                        verify_results()
                        print("QEMU: KUnit and tracefs checks passed")
                        return
                if proc.poll() is not None:
                    raise RuntimeError(f"QEMU exited: {proc.returncode}")
            raise TimeoutError("QEMU tests exceeded 600 seconds")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        selector.close()


def verify_results():
    verify_ktap()
    text = (ROOT / "trace-enabled.txt").read_text()
    records = [line for line in text.splitlines() if "iopf_latency:" in line]
    assert len(records) == 1, records
    fields = dict(re.findall(r"(\w+)=([^\s]+)", records[0]))
    expected = dict(device="smmu-iopf-latency-test", grpid="30",
                    addr="0xabcde000", response="0")
    for key, value in expected.items():
        assert fields[key] == value, (key, fields)
    marker = re.search(r"\bpasid\(([vi])\)=(\d+)\b", records[0])
    assert marker and marker.groups() == ("v", "7"), records[0]
    assert "pasid_valid=" not in records[0], records[0]
    assert "cmd_ret" not in fields, fields
    assert int(fields["duration_ns"]) >= 10_000_000, fields
    assert "iopf_latency:" not in (ROOT / "trace-disabled.txt").read_text()
    for stem in ["drivers/iommu/io-pgfault", "drivers/iommu/iommu-traces",
                 "drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3"]:
        shutil.copy2(KERNEL / (stem + ".gcno"), ROOT / "gcov" / (stem + ".gcno"))


def verify_ktap():
    text = (ROOT / "kunit-all.log").read_text()
    text = re.sub(r"(?m)^\[\s*[\d.]+\](?:\[\s*[TC]\d+\])?\s?", "", text)
    blocks = text.split("# Subtest: smmu-iopf-latency")[1:]
    assert len(blocks) == 3, f"expected 3 suites, got {len(blocks)}"
    names = {"kabi_layout", "delayed_success", "error_responses", "handler_failure",
             "allocation_failure", "enable_transitions", "concurrent_responses",
             "tag_reuse", "generic_response", "partial_group", "tracefs_sample"}
    for phase, block in enumerate(blocks):
        assert re.search(r"(?m)^\s+1\.\.11\s*$", block), block
        found = re.findall(r"(?m)^\s+(ok|not ok) \d+ (\w+)([^\n]*)", block)
        assert len(found) == 11 and {row[1] for row in found} == names, found
        for status, name, suffix in found:
            assert status == "ok", (phase, name, status)
            expect_skip = name == "tracefs_sample" if phase == 0 else name != "tracefs_sample"
            assert ("# SKIP" in suffix.upper()) == expect_skip, (phase, name, suffix)
        assert re.search(r"(?m)^ok \d+ smmu-iopf-latency\s*$", block), block


if __name__ == "__main__":
    main()
