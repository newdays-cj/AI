#!/usr/bin/env python3
"""Build a static test and a non-destructive overlay on the existing initramfs."""
import os
from pathlib import Path
import subprocess
import shutil
import sys

HERE = Path(__file__).resolve().parent
KERNEL = Path(os.environ.get("CKPT_KERNEL", str(Path.home() / "Git/OLK-6.6-2")))
BR = Path.home() / "tools/buildroot"
CC = BR / "output/host/bin/aarch64-buildroot-linux-gnu-gcc"
CASE_COUNT = 14


def cpio_entry(name, data, mode, ino):
    name = name.encode() + b"\0"
    fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name), 0)
    header = b"070701" + b"".join(f"{v:08x}".encode() for v in fields)
    out = header + name
    out += b"\0" * (-len(out) % 4)
    out += data
    out += b"\0" * (-len(data) % 4)
    return out


def main():
    rounds = int(os.environ.get("CKPT_ROUNDS", "1"))
    disabled = os.environ.get("CKPT_DISABLED") == "1"
    binary = HERE / "checkpoint-demo"
    subprocess.run([str(CC), "-static", "-pthread", "-O2", "-g", "-Wall",
                    "-Wextra", "-Werror", "-Wno-unused-parameter",
                    str(HERE / "demo.c"), "-o", str(binary)], check=True)
    edges = HERE / "checkpoint-edges"
    subprocess.run([str(CC), "-static", "-pthread", "-O2", "-g", "-Wall",
                    "-Wextra", "-Werror", "-Wno-unused-parameter",
                    str(HERE / "edges.c"), "-o", str(edges)],
                   check=True)
    external = HERE / "checkpoint-external"
    subprocess.run([str(CC), "-static", "-O2", "-g", "-Wall", "-Wextra",
                    "-Werror", str(HERE / "external.c"), "-o", str(external)], check=True)
    disabled_binary = HERE / "checkpoint-disabled"
    subprocess.run([str(CC), "-static", "-pthread", "-O2", "-g", "-Wall",
                    "-Wextra", "-Werror", "-Wno-unused-parameter",
                    str(HERE / "disabled.c"), "-o", str(disabled_binary)], check=True)
    base = (BR / "output/images/rootfs.cpio").read_bytes()
    base += b"\0" * (-len(base) % 512)
    overlay = cpio_entry("checkpoint-demo", binary.read_bytes(), 0o100755, 900001)
    init = HERE / ("init_disabled.sh" if disabled else "init.sh")
    overlay += cpio_entry("ckpt-init", init.read_bytes(), 0o100755, 900002)
    overlay += cpio_entry("checkpoint-edges", edges.read_bytes(), 0o100755, 900005)
    overlay += cpio_entry("checkpoint-external", external.read_bytes(), 0o100755, 900006)
    overlay += cpio_entry("checkpoint-disabled", disabled_binary.read_bytes(), 0o100755, 900007)
    if not disabled:
        overlay += cpio_entry("task_checkpoint_test.ko",
                              (KERNEL / "kernel/task_checkpoint_test.ko").read_bytes(),
                              0o100644, 900004)
    overlay += cpio_entry("TRAILER!!!", b"", 0, 900003)
    image = HERE / "initramfs.cpio"
    image.write_bytes(base + overlay)
    command = ["qemu-system-aarch64", "-machine", "virt,gic-version=3",
               "-cpu", os.environ.get("CKPT_CPU", "cortex-a72"),
               "-smp", "4", "-m", "2G",
               "-kernel", str(KERNEL / "arch/arm64/boot/Image"),
               "-initrd", str(image), "-append",
               f"console=ttyAMA0 rdinit=/ckpt-init nokaslr panic=-1 loglevel=5 CKPT_ROUNDS={rounds}",
               "-virtfs", f"local,path={HERE},mount_tag=checkpoint,security_model=none",
               "-nographic", "-no-reboot"]
    log = HERE / ("qemu_disabled.log" if disabled else "qemu_output.log")
    try:
        with log.open("wb") as output:
            result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                                    timeout=int(os.environ.get("CKPT_QEMU_TIMEOUT", "180")))
    except subprocess.TimeoutExpired:
        print(f"QEMU timed out; log: {log}", file=sys.stderr)
        return 1
    text = log.read_text(errors="replace")
    for line in text.splitlines():
        selected = ("PASS round=0 " in line or f"PASS round={rounds - 1} " in line)
        if any(word in line for word in ("CKPT_", "PASS edge", "PASS external", "FAIL", "checkpoint demo", "WARNING", "BUG:", "Oops", "task-checkpoint", "checkpoint_clone", "checkpoint_futex", "not ok")) or selected:
            print(line)
    print(f"QEMU exit={result.returncode}; complete log: {log}")
    if disabled:
        return 0 if "CKPT_DISABLED PASS" in text and "CKPT_DISABLED_EXIT=0" in text else 1
    summaries = []
    for data in (HERE / "gcov").rglob("*.gcda"):
        original = Path("/") / data.relative_to(HERE / "gcov")
        notes = original.with_suffix(".gcno")
        if notes.is_file():
            shutil.copy2(notes, data.with_suffix(".gcno"))
            for directory in ("arch", "include", "kernel"):
                link = data.parent / directory
                if not link.exists():
                    link.symlink_to(KERNEL / directory, target_is_directory=True)
            proc = subprocess.run([str(CC).replace("-gcc", "-gcov"), "-b", "-c", str(data)],
                                  cwd=data.parent, capture_output=True, text=True)
            summaries.append(proc.stdout + proc.stderr)
    (HERE / "coverage_summary.txt").write_text("\n".join(summaries))
    good = (f"CKPT_RESULT PASS {CASE_COUNT * rounds}/{CASE_COUNT * rounds}" in text and "CKPT_QEMU_EXIT=0" in text
            and "CKPT_EDGES PASS 10/10" in text
            and "CKPT_EXTERNAL PASS 2/2" in text
            and "CKPT_KUNIT_LOADED" in text and "task-checkpoint-contract" in text
            and "not ok" not in text and "CKPT_GCOV_COLLECTED" in text
            and "CKPT_LIVE_GROUPS=0" in text and "CKPT_HELD_TASKS=0" in text
            and "WARNING:" not in text and "BUG:" not in text and "Oops:" not in text)
    if good:
        commands = [
            ["lcov", "--capture", "--directory", str(HERE / "gcov"),
             "--base-directory", str(KERNEL), "--gcov-tool",
             str(CC).replace("-gcc", "-gcov"), "--output-file",
             str(HERE / "coverage.info"), "--branch-coverage",
             "--rc", "geninfo_unexecuted_blocks=1",
             "--include", "*/kernel/task_checkpoint.c",
             "--include", "*/kernel/task_checkpoint_test.c"],
            ["genhtml", str(HERE / "coverage.info"), "--output-directory",
             str(HERE / "gcov/coverage_html"), "--branch-coverage"],
        ]
        with (HERE / "lcov.log").open("w") as output:
            for command in commands:
                proc = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT)
                if proc.returncode:
                    print(f"Coverage processing failed; see {HERE / 'lcov.log'}")
                    good = False
                    break
    return 0 if good else 1


if __name__ == "__main__":
    raise SystemExit(main())
