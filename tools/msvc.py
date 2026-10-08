"""MSVC of Visual Studio 2022 with the cmake and ninja that ESP-IDF installs (docs/architecture.md
section 2). Shared by tools/host_test.py and tools/sim.py.

MSVC needs the environment that vcvars64.bat sets: msvc_environment() reads it once, the caller
passes it to every step.
"""
import glob
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ESPRESSIF_TOOLS = Path(os.environ.get("IDF_TOOLS_PATH", r"C:\Espressif\tools"))


def newest(pattern):
    found = sorted(glob.glob(str(pattern)))
    if not found:
        sys.exit(f"not found: {pattern}")
    return Path(found[-1])


def cmake():
    return newest(ESPRESSIF_TOOLS / "cmake/*/bin/cmake.exe")


def ninja():
    return newest(ESPRESSIF_TOOLS / "ninja/*/ninja.exe")


def vcvars():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio/Installer/vswhere.exe"
    if not vswhere.exists():
        sys.exit("Visual Studio not found (vswhere.exe is missing)")
    install = subprocess.run(
        [str(vswhere), "-latest", "-products", "*", "-requires",
         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
        capture_output=True, text=True, check=True).stdout.strip()
    if not install:
        sys.exit("Visual Studio has no C++ tools (workload 'Desktop development with C++')")
    return Path(install) / "VC/Auxiliary/Build/vcvars64.bat"


def msvc_environment():
    """The environment after vcvars64.bat (PATH, INCLUDE, LIB of MSVC and the Windows SDK)."""
    output = subprocess.run(f'"{vcvars()}" >nul && set', shell=True, capture_output=True, text=True,
                            check=True).stdout
    env = {}
    for line in output.splitlines():
        name, sep, value = line.partition("=")
        if sep:
            env[name] = value
    return env
