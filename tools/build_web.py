#!/usr/bin/env python3
"""Builds the web files of web/ for one firmware (docs/ui/web/web-design.md section 2):

    python tools/build_web.py build/<board>/main/sdkconfig build/<board>/fs/web

tools/build_fs.py runs it with the Python of build/littlefs_venv (rjsmin and rcssmin live there)
before it gzips the result into the image. The output keeps the names of web/, so index.html's
references and the web server's content types hold. What happens to the files:

* .js and .html: the lines between `// #if CONFIG_X` and `// #endif` (`<!-- #if CONFIG_X -->` in
  HTML) are dropped when the firmware's sdkconfig lacks `CONFIG_X=y`: the pages of a board option
  (the SD card, the camera) are not in the image of a board without it. No nesting, no #else. The
  sources stay valid as they are, the markers are comments.
* vendor/*.css (Bootstrap): only the rules whose classes the stripped sources use, plus SAFELIST
  (classes the pages build from a variable, `alert-${kind}`). Rules without a class (:root, the
  dark mode's variables, elements) stay, so do @font-face and the @keyframes still referenced.
* .js minified with rjsmin, .css with rcssmin: comments and whitespace only, no renaming, so a
  browser's error line still reads. Everything else (icons.svg, index.html) is copied.

The same sources give the same bytes. The file is the same in esp32-pixeled.
"""

import argparse
import re
import shutil
import sys
from pathlib import Path

try:
    import rcssmin
    import rjsmin
except ImportError:
    sys.exit("build_web.py: needs rjsmin and rcssmin: run it with build/littlefs_venv's python (tools/build_fs.py installs them)")

ROOT = Path(__file__).resolve().parent.parent
WEB = ROOT / "web"
VENDOR = "vendor"
# Bootstrap classes the pages assemble from a variable (`alert-${kind}`, `text-bg-${kind}`): the
# sources name no complete class, so every variant stays. Each pattern must match a rule.
SAFELIST = (r"^alert-", r"^text-bg-")

MARKER = re.compile(r"^\s*(?://|<!--)\s*#(?:if\s+(CONFIG_\w+)|(endif))\s*(?:-->)?\s*$")
TOKEN = re.compile(r"[A-Za-z0-9_-]+")
CLASS = re.compile(r"\.(-?[_a-zA-Z][_a-zA-Z0-9-]*)")
NESTED_AT = ("@media", "@supports", "@layer", "@container")


def fail(message: str) -> None:
    sys.exit(f"build_web.py: {message}")


def read_options(sdkconfig: Path) -> set[str]:
    """The CONFIG_* options set to y in the firmware's sdkconfig."""
    if not sdkconfig.is_file():
        fail(f"{sdkconfig} is missing: build the firmware first")
    return set(re.findall(r"^(CONFIG_\w+)=y$", sdkconfig.read_text(encoding="utf-8"), re.M))


def strip_conditionals(text: str, options: set[str], name: str) -> str:
    """Drops the lines between `// #if CONFIG_X` and `// #endif` when the option is not set."""
    out, keep, option = [], True, None
    for number, line in enumerate(text.splitlines(keepends=True), 1):
        match = MARKER.match(line)
        if not match:
            if keep:
                out.append(line)
            continue
        if match.group(1):
            if option:
                fail(f"{name}:{number}: #if inside #if {option}")
            option = match.group(1)
            keep = option in options
        else:
            if not option:
                fail(f"{name}:{number}: #endif without #if")
            option, keep = None, True
    if option:
        fail(f"{name}: #if {option} without #endif")
    return "".join(out)


# ---------------------------------------------------------------------------------- css purge


def blocks(css: str):
    """Yields (prelude, body) of a css text's top-level blocks; body None for `@charset ...;`."""
    i, n = 0, len(css)
    while i < n:
        depth, j = 0, i
        while j < n:
            c = css[j]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    break
            elif c == ";" and depth == 0:
                break
            j += 1
        chunk, i = css[i:j + 1], j + 1
        if not chunk.strip():
            continue
        if chunk.endswith(";"):
            yield chunk[:-1].strip(), None
        elif "{" in chunk:
            k = chunk.index("{")
            yield chunk[:k].strip(), chunk[k + 1:-1]
        else:
            fail(f"css: text without a block: {chunk[:40]!r}")


def selectors(prelude: str) -> list[str]:
    """Splits a rule's selector list at the commas outside parentheses."""
    out, depth, current = [], 0, ""
    for c in prelude:
        depth += (c == "(") - (c == ")")
        if c == "," and depth == 0:
            out.append(current.strip())
            current = ""
        else:
            current += c
    out.append(current.strip())
    return out


def classes_of(selector: str) -> list[str]:
    # a class inside :not() is not required: `.btn:not(.btn-check)` needs btn only
    return CLASS.findall(re.sub(r":not\([^)]*\)", "", selector))


def purge_css(css: str, tokens: set[str], name: str) -> str:
    """Keeps the rules whose classes are tokens of the sources or match SAFELIST."""
    css = re.sub(r"/\*.*?\*/", "", css, flags=re.S)
    safe = [re.compile(p) for p in SAFELIST]
    matched = [False] * len(safe)

    def used(cls: str) -> bool:
        if cls in tokens:
            return True
        for i, pattern in enumerate(safe):
            if pattern.search(cls):
                matched[i] = True
                return True
        return False

    keyframes: dict[str, str] = {}

    def purge(text: str) -> str:
        out = []
        for prelude, body in blocks(text):
            if body is None:
                out.append(prelude + ";")
            elif prelude.startswith(NESTED_AT):
                inner = purge(body)
                if inner:
                    out.append(f"{prelude}{{{inner}}}")
            elif prelude.startswith("@keyframes"):
                keyframes[prelude.split()[1]] = f"{prelude}{{{body}}}"
            elif prelude.startswith("@"):
                out.append(f"{prelude}{{{body}}}")
            else:
                kept = [s for s in selectors(prelude) if "\\" in s or all(used(c) for c in classes_of(s))]
                if kept:
                    out.append(f"{','.join(kept)}{{{body}}}")
        return "".join(out)

    result = purge(css)
    result += "".join(block for key, block in keyframes.items() if re.search(rf"\b{re.escape(key)}\b", result))
    for pattern, hit in zip(SAFELIST, matched):
        if not hit:
            fail(f"{name}: the safelist pattern {pattern!r} matches no rule, is it stale?")

    before, after = set(CLASS.findall(css)), set(CLASS.findall(result))
    print(f"  {name}: {len(before)} -> {len(after)} classes", flush=True)
    # a class of the sources whose every rule needs another class the sources lack (`.nav-item` only
    # under `.nav-fill`): those rules would not apply anyway, the line is there for a look
    gone = sorted((before & tokens) - after)
    if gone:
        print(f"  {name}: no rule left for {' '.join(gone)} (used with classes the pages lack)", flush=True)
    return result


# --------------------------------------------------------------------------------------- build


def build(sdkconfig: Path, out: Path) -> list[tuple[str, int, int]]:
    """Writes the built web files to out/; returns (name, source size, built size)."""
    options = read_options(sdkconfig)
    if out.exists():
        shutil.rmtree(out)
    sources = sorted(p for p in WEB.rglob("*") if p.is_file())
    built: dict[str, bytes] = {}
    tokens: set[str] = set()
    for source in sources:
        name = source.relative_to(WEB).as_posix()
        if name.startswith(VENDOR + "/"):
            continue
        if source.suffix in (".js", ".html"):
            text = strip_conditionals(source.read_text(encoding="utf-8"), options, name)
            built[name] = (rjsmin.jsmin(text) if source.suffix == ".js" else text).encode("utf-8")
            tokens |= set(TOKEN.findall(text))
        elif source.suffix == ".css":
            text = source.read_text(encoding="utf-8")
            built[name] = rcssmin.cssmin(text).encode("utf-8")
            tokens |= set(TOKEN.findall(text))
        else:
            built[name] = source.read_bytes()
    for source in sources:
        name = source.relative_to(WEB).as_posix()
        if name.startswith(VENDOR + "/"):
            if source.suffix == ".css":
                text = purge_css(source.read_text(encoding="utf-8"), tokens, name)
                built[name] = rcssmin.cssmin(text).encode("utf-8")
            else:
                built[name] = source.read_bytes()

    files = []
    for source in sources:
        name = source.relative_to(WEB).as_posix()
        target = out / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(built[name])
        files.append((name, source.stat().st_size, len(built[name])))
    return files


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("sdkconfig", type=Path, help="the firmware's generated sdkconfig (build/<board>/main/sdkconfig)")
    parser.add_argument("out", type=Path, help="the directory for the built files")
    args = parser.parse_args()
    for name, plain, built in build(args.sdkconfig, args.out):
        print(f"  {name:32} {plain / 1024:7.1f} KB -> {built / 1024:6.1f} KB")


if __name__ == "__main__":
    main()
