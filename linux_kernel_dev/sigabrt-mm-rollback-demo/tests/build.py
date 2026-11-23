#!/usr/bin/env python3
"""Build/test helpers and documents-only packaging for the checkpoint demo."""
import argparse
import csv
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tarfile

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent
KERNEL = Path(os.environ.get("CKPT_KERNEL", str(Path.home() / "Git/OLK-6.6-2")))
BASELINE = Path.home() / "tools/qemu_arm64_defconfig"
CROSS = str(Path.home() / "tools/buildroot/output/host/bin/aarch64-buildroot-linux-gnu-")


def build(disabled=False):
    config = PROJECT / (".config.disabled" if disabled else ".config")
    fragment = PROJECT / ("disabled.config" if disabled else "feature.config")
    env = dict(os.environ, KCONFIG_CONFIG=str(config))
    evidence = PROJECT / "evidence"
    evidence.mkdir(exist_ok=True)
    log = evidence / ("build-disabled.log" if disabled else "build-enabled.log")
    args = [f"ARCH=arm64", f"CROSS_COMPILE={CROSS}", f"KCONFIG_CONFIG={config}"]
    print(f"Building {'disabled' if disabled else 'enabled'} Image; log: {log}", flush=True)
    with log.open("w") as output:
        commands = [
            ["scripts/kconfig/merge_config.sh", "-m", str(BASELINE), str(fragment)],
            ["make", "-s", *args, "olddefconfig"],
            ["make", "-s", f"-j{os.environ.get('CKPT_JOBS', '16')}", *args, "Image"],
        ]
        if not disabled:
            commands[-1].append("kernel/task_checkpoint_test.ko")
        for command in commands:
            subprocess.run(command, cwd=KERNEL, env=env, stdout=output,
                           stderr=subprocess.STDOUT, check=True)


def coverage_inventory():
    text = (HERE / "coverage.info").read_text()
    report = ["# 实际函数和分支覆盖明细", "",
              "来自配套 gcda/gcno 和 LCOV；仅统计三个新增 C 文件，不冒充所有旧文件修改分支的全覆盖。", "",
              "| 文件 | 行 | 函数 | 命中次数 |", "|---|---:|---|---:|"]
    with (HERE / "uncovered-branches.csv").open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(["file", "line", "block", "branch", "count", "source"])
        for record in text.split("end_of_record"):
            values = record.strip().splitlines()
            source = next((line[3:] for line in values if line.startswith("SF:")), None)
            if not source:
                continue
            path = Path(source)
            names = {}
            counts = {}
            lines = path.read_text().splitlines()
            for value in values:
                if value.startswith("FN:"):
                    start, end, name = value[3:].split(",", 2)
                    names[name] = start
                elif value.startswith("FNDA:"):
                    count, name = value[5:].split(",", 1)
                    counts[name] = count
                elif value.startswith("BRDA:"):
                    number, block, branch, count = value[5:].split(",")
                    if count in ("0", "-"):
                        writer.writerow([str(path.relative_to(KERNEL)), number,
                                         block, branch, count, lines[int(number) - 1]])
            for name, start in names.items():
                report.append(f"| {path.relative_to(KERNEL)} | {start} | `{name}` | {counts.get(name, '0')} |")
    (HERE / "函数覆盖明细.md").write_text("\n".join(report) + "\n")


def pack():
    files = sorted(PROJECT.glob("*.md")) + sorted(HERE.glob("*.md"))
    files.append(PROJECT / "COMMIT.txt")
    expected = {}
    for path in files:
        if not path.is_file() or path.is_symlink():
            raise RuntimeError(f"Not a regular document: {path}")
        expected[str(path.relative_to(PROJECT))] = hashlib.sha256(path.read_bytes()).hexdigest()
    temporary = PROJECT / ".documents.tar.gz.tmp"
    try:
        with tarfile.open(temporary, "w:gz") as archive:
            for path in files:
                archive.add(path, arcname=str(path.relative_to(PROJECT)), recursive=False)
        actual = {}
        with tarfile.open(temporary, "r:gz") as archive:
            for member in archive.getmembers():
                if not member.isfile() or member.name in actual or member.name not in expected:
                    raise RuntimeError(f"Unexpected document archive entry: {member.name}")
                with archive.extractfile(member) as source:
                    actual[member.name] = hashlib.sha256(source.read()).hexdigest()
        if actual != expected:
            raise RuntimeError("Document archive content mismatch")
        checksum = hashlib.sha256(temporary.read_bytes()).hexdigest()
        temporary.replace(PROJECT / "documents.tar.gz")
        (PROJECT / "documents.tar.gz.sha256").write_text(f"{checksum}  documents.tar.gz\n")
    finally:
        if temporary.exists():
            temporary.unlink()
    print(f"Documents archived: {len(expected)} files; all member hashes matched")
    print(f"Archive: {PROJECT / 'documents.tar.gz'}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("build", "disabled-check", "coverage", "pack"))
    mode = parser.parse_args().mode
    if mode == "build":
        build()
    elif mode == "disabled-check":
        try:
            build(disabled=True)
            subprocess.run([sys.executable, str(HERE / "run_qemu.py")],
                           env=dict(os.environ, CKPT_DISABLED="1"), check=True)
        finally:
            build()
    elif mode == "coverage":
        coverage_inventory()
    else:
        pack()


if __name__ == "__main__":
    main()
