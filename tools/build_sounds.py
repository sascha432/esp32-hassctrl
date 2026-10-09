#!/usr/bin/env python3
"""Makes the alarm sounds of sounds/alarms/ (docs/architecture.md section 9.6).

    python tools/build_sounds.py

Downloads alarm sounds of the Android Open Source Project (frameworks/base, data/sounds/alarms,
Apache License 2.0, see sounds/NOTICE) and converts them with ffmpeg (libmp3lame) to MP3 for the
speaker: mono, 22.05 kHz, 48 kbit/s, about 6 KB per second. tools/build_fs.py puts them into the
file system image of boards built with CONFIG_HC_AUDIO_MP3, at /fs/sounds/alarms/.

The MP3 files are committed; run this again only to change the selection or the format. Needs
ffmpeg on the PATH.
"""

import base64
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "sounds" / "alarms"
SOURCE = "https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/data/sounds/alarms/ogg"
# AOSP name -> file name on the device (the name the alarm page shows)
SOUNDS = {
    "Argon": "Argon",
    "Carbon": "Carbon",
    "Helium": "Helium",
    "Oxygen": "Oxygen",
    "Platinum": "Platinum",
    "Timing": "Timing",
}


def main() -> None:
    if shutil.which("ffmpeg") is None:
        sys.exit("build_sounds.py: ffmpeg is not on the PATH")
    OUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temp:
        for name, target in SOUNDS.items():
            # googlesource sends file contents base64 encoded with ?format=TEXT
            with urllib.request.urlopen(f"{SOURCE}/{name}.ogg?format=TEXT") as response:
                ogg = Path(temp) / f"{name}.ogg"
                ogg.write_bytes(base64.b64decode(response.read()))
            mp3 = OUT / f"{target}.mp3"
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(ogg), "-ac", "1", "-ar", "22050",
                            "-codec:a", "libmp3lame", "-b:a", "48k", "-map_metadata", "-1", str(mp3)], check=True)
            print(f"  {mp3.relative_to(ROOT).as_posix():32} {mp3.stat().st_size / 1024:6.1f} KB")


if __name__ == "__main__":
    main()
