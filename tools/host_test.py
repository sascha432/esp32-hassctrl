#!/usr/bin/env python3
"""Builds and runs the host tests (docs/architecture.md section 2): the pure components compiled
for the PC with MSVC of Visual Studio 2022, using the cmake and ninja that ESP-IDF installs.

    python tools/host_test.py              build and run every test
    python tools/host_test.py url_         only the tests whose name contains "url_"
    python tools/host_test.py --clean      build from scratch

The build directory is build/host. MSVC needs the environment that vcvars64.bat sets: it is read
once and passed to every step.
"""
import argparse
import subprocess
import sys

from msvc import ROOT, cmake, msvc_environment, ninja

BUILD = ROOT / "build" / "host"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("filter", nargs="?", help="run only tests whose name contains this")
    parser.add_argument("--clean", action="store_true", help="delete build/host first")
    args = parser.parse_args()

    configure = [str(cmake()), "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja()}", "-DCMAKE_BUILD_TYPE=Debug",
                 "-S", str(ROOT / "host"), "-B", str(BUILD)]
    if args.clean:
        configure.append("--fresh")
    build = [str(cmake()), "--build", str(BUILD)]
    run = [str(BUILD / "hc_tests.exe")] + ([args.filter] if args.filter else [])

    env = msvc_environment()
    for step in (configure, build, run):
        result = subprocess.call(step, cwd=ROOT, env=env)
        if result != 0:
            sys.exit(result)


if __name__ == "__main__":
    main()
