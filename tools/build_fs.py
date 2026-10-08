#!/usr/bin/env python3
"""Builds the file system image (LittleFS) of a board from web/ (docs/ui/web.md section 7).

    python tools/build_fs.py <board>                 build/<board>/fs/storage.bin
    python tools/build_fs.py <board> -p COM9         and write it to the board over serial

Every file of web/ goes gzipped to /www (the web server sends file.gz with gzip encoding). The
image has the size of the "storage" partition of the board's partition table. Upload it on the
System page of the web UI, or with tools/update.py <device> filesystem <image>.

littlefs-python is installed into build/littlefs_venv on the first run (the version the littlefs
component uses for its images). Writing over serial needs the ESP-IDF environment.
"""

import argparse
import csv
import gzip
import io
import os
import re
import shutil
import subprocess
import sys
import venv
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WEB = ROOT / "web"
LITTLEFS_PYTHON = "littlefs-python==0.15.0"
# CONFIG_LITTLEFS_OBJ_NAME_LEN and the flash sector size
NAME_MAX = 64
BLOCK_SIZE = 4096


def fail(message: str) -> None:
    sys.exit(f"build_fs.py: {message}")


def partition_table(board: str) -> Path:
    defaults = ROOT / "boards" / board / "sdkconfig.defaults"
    if not defaults.is_file():
        fail(f"unknown board '{board}'")
    match = re.search(r'^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="([^"]+)"', defaults.read_text(encoding="utf-8"), re.M)
    if not match:
        fail(f"{defaults} names no partition table")
    return ROOT / match.group(1)


def storage_size(table: Path) -> int:
    lines = [line for line in table.read_text(encoding="utf-8").splitlines() if line.strip() and not line.startswith("#")]
    for row in csv.reader(lines):
        row = [cell.strip() for cell in row]
        if row[0] == "storage":
            return int(row[4], 0)
    fail(f"{table} has no 'storage' partition")
    return 0


def littlefs_tool() -> Path:
    env = ROOT / "build" / "littlefs_venv"
    tool = env / ("Scripts/littlefs-python.exe" if os.name == "nt" else "bin/littlefs-python")
    if not tool.is_file():
        print("build_fs.py: installing littlefs-python into build/littlefs_venv", flush=True)
        venv.create(env, with_pip=True)
        pip = env / ("Scripts/pip.exe" if os.name == "nt" else "bin/pip")
        subprocess.run([str(pip), "install", "-q", LITTLEFS_PYTHON], check=True)
    return tool


def stage(root: Path) -> list[tuple[str, int, int]]:
    """Copies web/ gzipped to root/www; returns (name, size, gzipped size)."""
    if root.exists():
        shutil.rmtree(root)
    files = []
    for source in sorted(p for p in WEB.rglob("*") if p.is_file()):
        relative = source.relative_to(WEB)
        target = root / "www" / relative.parent / (relative.name + ".gz")
        target.parent.mkdir(parents=True, exist_ok=True)
        data = source.read_bytes()
        buffer = io.BytesIO()
        # mtime 0: the same sources give the same image
        with gzip.GzipFile(fileobj=buffer, mode="wb", compresslevel=9, mtime=0) as zipped:
            zipped.write(data)
        target.write_bytes(buffer.getvalue())
        files.append((relative.as_posix(), len(data), len(buffer.getvalue())))
    return files


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("board")
    parser.add_argument("-p", "--port", help="write the image to the board over this serial port")
    args = parser.parse_args()

    size = storage_size(partition_table(args.board))
    out_dir = ROOT / "build" / args.board / "fs"
    root = out_dir / "root"
    image = out_dir / "storage.bin"
    files = stage(root)
    total = sum(z for _, _, z in files)
    for name, plain, zipped in files:
        print(f"  {name:32} {plain / 1024:7.1f} KB  gzip {zipped / 1024:6.1f} KB")
    print(f"  {len(files)} files, {total / 1024:.1f} KB of {size // 1024} KB")

    out_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(littlefs_tool()), "create", str(root), str(image), f"--fs-size={size}",
                    f"--name-max={NAME_MAX}", f"--block-size={BLOCK_SIZE}"], check=True, stdout=subprocess.DEVNULL)
    print(f"build_fs.py: {image.relative_to(ROOT)}")

    if args.port:
        idf = os.environ.get("IDF_PATH")
        if not idf:
            fail("IDF_PATH is not set, run the ESP-IDF activation script first")
        python = os.environ.get("IDF_PYTHON_ENV_PATH")
        python = str(Path(python) / ("Scripts/python.exe" if os.name == "nt" else "bin/python")) if python else sys.executable
        parttool = Path(idf) / "components" / "partition_table" / "parttool.py"
        subprocess.run([python, str(parttool), "--port", args.port, "write_partition", "--partition-name", "storage",
                        "--input", str(image)], check=True)


if __name__ == "__main__":
    main()
