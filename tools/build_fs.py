#!/usr/bin/env python3
"""Builds the file system image (LittleFS) of a board from web/ (docs/ui/web/web-recovery.md) and, for a
board built with CONFIG_HC_AUDIO_MP3, the sounds of sounds/ (docs/architecture.md section 9.6).

    python tools/build_fs.py <board>                 build/<board>/fs/storage.bin
    python tools/build_fs.py <board> -p COM9         and write it to the board over serial, keeping
                                                     the board's own files (the dashboard)
    python tools/build_fs.py <board> -p COM9 --fresh the same without keeping anything

Every file of web/ goes gzipped to /www (the web server sends file.gz with gzip encoding), the MP3
files of sounds/ as they are to /sounds (the board's build/<board>/main/sdkconfig tells). The
image has the size of the "storage" partition of the board's partition table. Upload it on the
System page of the web UI, or with tools/update.py <device> filesystem <image>.

The image replaces the whole partition. With -p the partition of the board is read first and
every file outside /www and /sounds (dashboard.json) goes into the new image, so a web UI update keeps the
dashboard. An upload through the web UI or update.py does not: export the dashboard first.
The web UI belongs to the firmware: the dashboard editor copies rules of the firmware, so a
firmware with a change to the dashboard goes onto a device together with its web image.

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
SOUNDS = ROOT / "sounds"
# the directories of the image that come from the firmware's sources, not from the device
FIRMWARE_DIRS = ("www", "sounds")
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


def has_mp3(board: str) -> bool:
    sdkconfig = ROOT / "build" / board / "main" / "sdkconfig"
    if not sdkconfig.is_file():
        fail(f"{sdkconfig.relative_to(ROOT)} is missing: build the firmware first (the sounds depend on it)")
    return re.search(r"^CONFIG_HC_AUDIO_MP3=y$", sdkconfig.read_text(encoding="utf-8"), re.M) is not None


def stage_sounds(root: Path) -> list[tuple[str, int, int]]:
    """Copies the MP3 files of sounds/ to root/sounds (MP3 does not compress)."""
    files = []
    for source in sorted(SOUNDS.rglob("*.mp3")):
        relative = source.relative_to(SOUNDS)
        target = root / "sounds" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        size = source.stat().st_size
        files.append(("sounds/" + relative.as_posix(), size, size))
    return files


def parttool(port: str, *arguments: str) -> None:
    idf = os.environ.get("IDF_PATH")
    if not idf:
        fail("IDF_PATH is not set, run the ESP-IDF activation script first")
    python = os.environ.get("IDF_PYTHON_ENV_PATH")
    python = str(Path(python) / ("Scripts/python.exe" if os.name == "nt" else "bin/python")) if python else sys.executable
    tool = Path(idf) / "components" / "partition_table" / "parttool.py"
    subprocess.run([python, str(tool), "--port", port, *arguments], check=True)


def keep_device_files(port: str, out_dir: Path, root: Path) -> list[str]:
    """Reads the board's storage partition and copies every file outside www/ into root."""
    device_image = out_dir / "device.bin"
    device_root = out_dir / "device"
    parttool(port, "read_partition", "--partition-name", "storage", "--output", str(device_image))
    if device_root.exists():
        shutil.rmtree(device_root)
    result = subprocess.run([str(littlefs_tool()), "extract", str(device_image), str(device_root),
                             f"--name-max={NAME_MAX}", f"--block-size={BLOCK_SIZE}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if result.returncode != 0:
        print("build_fs.py: the board's file system could not be read (not formatted?), nothing kept")
        return []
    kept = []
    for source in sorted(p for p in device_root.rglob("*") if p.is_file()):
        relative = source.relative_to(device_root)
        if relative.parts[0] in FIRMWARE_DIRS:
            continue
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        kept.append(relative.as_posix())
    return kept


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("board")
    parser.add_argument("-p", "--port", help="write the image to the board over this serial port")
    parser.add_argument("--fresh", action="store_true", help="with -p: do not keep the board's own files")
    args = parser.parse_args()

    size = storage_size(partition_table(args.board))
    out_dir = ROOT / "build" / args.board / "fs"
    root = out_dir / "root"
    image = out_dir / "storage.bin"
    out_dir.mkdir(parents=True, exist_ok=True)
    files = stage(root)
    if has_mp3(args.board):
        files += stage_sounds(root)
    if args.port and not args.fresh:
        for name in keep_device_files(args.port, out_dir, root):
            print(f"  kept from the board: {name}")
    total = sum(z for _, _, z in files)
    for name, plain, zipped in files:
        print(f"  {name:32} {plain / 1024:7.1f} KB  gzip {zipped / 1024:6.1f} KB")
    print(f"  {len(files)} files, {total / 1024:.1f} KB of {size // 1024} KB")

    out_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(littlefs_tool()), "create", str(root), str(image), f"--fs-size={size}",
                    f"--name-max={NAME_MAX}", f"--block-size={BLOCK_SIZE}"], check=True, stdout=subprocess.DEVNULL)
    print(f"build_fs.py: {image.relative_to(ROOT)}")

    if args.port:
        parttool(args.port, "write_partition", "--partition-name", "storage", "--input", str(image))


if __name__ == "__main__":
    main()
