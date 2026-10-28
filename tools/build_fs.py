#!/usr/bin/env python3
"""Builds the file system image (LittleFS) of a board from web/ (docs/ui/web/web-recovery.md) and, for a
board built with CONFIG_HC_AUDIO_MP3, the sounds of sounds/ (docs/architecture.md section 9.6).

    python tools/build_fs.py <board>                 build/<board>/fs/storage.bin
    python tools/build_fs.py <board> -p COM9         and write it to the board over serial, keeping
                                                     the board's own files (the dashboard)
    python tools/build_fs.py <board> -p COM9 --fresh the same without keeping anything
    python tools/build_fs.py <board> --device 192.168.0.233
                                                     and upload it over the network, keeping
                                                     the device's own files the same way

<board> is the name given to tools/build.py, a variant included (wt32_sc01.kitchen). The web files
are built by tools/build_web.py first (build/<board>/fs/web: the pages of options the firmware
lacks left out, Bootstrap purged to the rules in use, JS and CSS minified) and go gzipped to /www
(the web server sends file.gz with gzip encoding), the MP3 files of sounds/ as they are to /sounds.
The image has the size of the "storage" partition of the partition table; the partition table, the
options and the audio options come from the firmware's build/<board>/main/sdkconfig, so the
firmware is built first. Upload it on the System page of the web UI, or with tools/update.py
<device> filesystem <image>.

The image replaces the whole partition. With -p the partition of the board is read first and
every file outside /www and /sounds (dashboard.json) goes into the new image, so a web UI update keeps the
dashboard. With --device the same files come from the device's /api/fs/files.zip (login as
tools/update.py: --user, --password or HC_PASSWORD), and the image goes up as with update.py; a
file the device could not read stops it, nothing is uploaded. An upload through the web UI or
`update.py ... filesystem --fresh` keeps nothing: export the dashboard first.
The web UI belongs to the firmware: the dashboard editor copies rules of the firmware, so a
firmware with a change to the dashboard goes onto a device together with its web image.

littlefs-python (the version the littlefs component uses for its images), rjsmin and rcssmin are
installed into build/littlefs_venv on the first run. Writing over serial needs the ESP-IDF
environment.
"""

import argparse
import csv
import gzip
import hashlib
import io
import os
import zipfile
import re
import shutil
import subprocess
import sys
import venv
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOUNDS = ROOT / "sounds"
# the directories of the image that come from the firmware's sources, not from the device
FIRMWARE_DIRS = ("www", "sounds")
# the littlefs component's image tool, the minifiers of tools/build_web.py
PACKAGES = ("littlefs-python==0.15.0", "rjsmin==1.3.0", "rcssmin==1.3.0")
# CONFIG_LITTLEFS_OBJ_NAME_LEN and the flash sector size
NAME_MAX = 64
BLOCK_SIZE = 4096


def fail(message: str) -> None:
    sys.exit(f"build_fs.py: {message}")


def firmware_sdkconfig(board: str) -> Path:
    """The generated sdkconfig of the main firmware: the partition table, the options, the audio options."""
    sdkconfig = ROOT / "build" / board / "main" / "sdkconfig"
    if not sdkconfig.is_file():
        fail(f"{sdkconfig.relative_to(ROOT)} is missing: build the firmware first (python tools/build.py {board})")
    return sdkconfig


def partition_table(sdkconfig: str) -> Path:
    match = re.search(r'^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="([^"]+)"', sdkconfig, re.M)
    if not match:
        fail("the firmware's sdkconfig names no partition table")
    return ROOT / match.group(1)


def storage_size(table: Path) -> int:
    lines = [line for line in table.read_text(encoding="utf-8").splitlines() if line.strip() and not line.startswith("#")]
    for row in csv.reader(lines):
        row = [cell.strip() for cell in row]
        if row[0] == "storage":
            return int(row[4], 0)
    fail(f"{table} has no 'storage' partition")
    return 0


def tools_venv() -> Path:
    """The venv with PACKAGES, made on the first run; packages.txt says what is in it."""
    env = ROOT / "build" / "littlefs_venv"
    marker = env / "packages.txt"
    wanted = "\n".join(PACKAGES) + "\n"
    if not marker.is_file() or marker.read_text(encoding="utf-8") != wanted:
        print(f"build_fs.py: installing {', '.join(PACKAGES)} into build/littlefs_venv", flush=True)
        venv.create(env, with_pip=True)
        pip = env / ("Scripts/pip.exe" if os.name == "nt" else "bin/pip")
        subprocess.run([str(pip), "install", "-q", *PACKAGES], check=True)
        marker.write_text(wanted, encoding="utf-8")
    return env


def venv_program(name: str) -> Path:
    env = tools_venv()
    return env / (f"Scripts/{name}.exe" if os.name == "nt" else f"bin/{name}")


def littlefs_tool() -> Path:
    return venv_program("littlefs-python")


def stage(root: Path, sdkconfig: Path) -> list[tuple[str, int, int]]:
    """Builds the web files (tools/build_web.py) and copies them gzipped to root/www; returns
    (name, built size, gzipped size)."""
    if root.exists():
        shutil.rmtree(root)
    web = root.parent / "web"
    print("build_fs.py: building the web files", flush=True)
    subprocess.run([str(venv_program("python")), str(ROOT / "tools" / "build_web.py"), str(sdkconfig), str(web)], check=True)
    files = []
    # one ETag for the image: the web server answers 304 to a browser that has these files
    digest = hashlib.sha256()
    for source in sorted(p for p in web.rglob("*") if p.is_file()):
        relative = source.relative_to(web)
        target = root / "www" / relative.parent / (relative.name + ".gz")
        target.parent.mkdir(parents=True, exist_ok=True)
        data = source.read_bytes()
        buffer = io.BytesIO()
        # mtime 0: the same sources give the same image
        with gzip.GzipFile(fileobj=buffer, mode="wb", compresslevel=9, mtime=0) as zipped:
            zipped.write(data)
        target.write_bytes(buffer.getvalue())
        digest.update(relative.as_posix().encode())
        digest.update(buffer.getvalue())
        files.append((relative.as_posix(), len(data), len(buffer.getvalue())))
    (root / "www" / "etag").write_text(digest.hexdigest()[:16], encoding="ascii")
    return files


def has_mp3(sdkconfig: str) -> bool:
    return re.search(r"^CONFIG_HC_AUDIO_MP3=y$", sdkconfig, re.M) is not None


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


def keep_files_over_network(device, root: Path) -> list[str]:
    """Copies every file of the device's /api/fs/files.zip outside www/ and sounds/ into root."""
    with device.opener.open(device.base + "/api/fs/files.zip", timeout=120) as response:
        archive = zipfile.ZipFile(io.BytesIO(response.read()))
    names = [n for n in archive.namelist() if not n.endswith("/")]
    # the archive names what it could not read (or that the file system did not mount) in this note
    if "unreadable.txt" in names:
        fail("the device could not read all of its files, nothing uploaded:\n"
             + archive.read("unreadable.txt").decode(errors="replace"))
    kept = []
    for name in sorted(names):
        if name.split("/")[0] in FIRMWARE_DIRS:
            continue
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(archive.read(name))
        kept.append(name)
    return kept


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("board", help="the name given to tools/build.py, a variant included (wt32_sc01.kitchen)")
    parser.add_argument("-p", "--port", help="write the image to the board over this serial port")
    parser.add_argument("--device", help="upload the image over the network to this address or host name")
    parser.add_argument("--user", default="admin", help="with --device: the web UI login")
    parser.add_argument("--password", help="with --device: else HC_PASSWORD or asked for")
    parser.add_argument("--fresh", action="store_true", help="with -p or --device: do not keep the board's own files")
    args = parser.parse_args()
    if args.port and args.device:
        fail("-p or --device, not both")
    device = None
    if args.device:
        sys.path.insert(0, str(ROOT / "tools"))
        import update

        device = update.Device(args.device)
        update.login(device, args.user, args.password)

    sdkconfig_path = firmware_sdkconfig(args.board)
    sdkconfig = sdkconfig_path.read_text(encoding="utf-8")
    size = storage_size(partition_table(sdkconfig))
    out_dir = ROOT / "build" / args.board / "fs"
    root = out_dir / "root"
    image = out_dir / "storage.bin"
    out_dir.mkdir(parents=True, exist_ok=True)
    files = stage(root, sdkconfig_path)
    if has_mp3(sdkconfig):
        files += stage_sounds(root)
    if args.port and not args.fresh:
        for name in keep_device_files(args.port, out_dir, root):
            print(f"  kept from the board: {name}")
    if device is not None and not args.fresh:
        for name in keep_files_over_network(device, root):
            print(f"  kept from the device: {name}")
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
    if device is not None:
        print(f"uploading {image.stat().st_size // 1024} KB ...")
        status, answer = device.request("/api/update/filesystem", data=image.read_bytes(), timeout=300,
                                        content_type="application/octet-stream")
        if status != 200:
            fail(f"upload failed: {answer.get('error', status)}")
        print("build_fs.py: uploaded")


if __name__ == "__main__":
    main()
