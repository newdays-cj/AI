#!/bin/sh
set -eu
directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
python3 "$directory/build.py" build
python3 "$directory/run_qemu.py"
python3 "$directory/build.py" coverage
