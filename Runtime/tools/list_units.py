#!/usr/bin/env python3
"""
Prints a GBA decomp's link units for the native build, one absolute path per line.

  list_units.py <units.json> <decomp_root> c|s [exclude_regex]

<units.json> is {"units": [{"src": "src/main.c", ...}, ...]} in link order, as written by
the game package's generator (e.g. com.recomp.khcom/Native/tools/kh_gen.py). Units without
a source (libgcc/libc archive members) are skipped: the native toolchain provides them.
"""
import json
import os
import re
import sys


def main():
    units_json, root, kind = sys.argv[1:4]
    exclude = re.compile(sys.argv[4]) if len(sys.argv) > 4 and sys.argv[4] else None
    ext = ".c" if kind == "c" else ".s"
    for unit in json.load(open(units_json, encoding="utf-8"))["units"]:
        src = unit.get("src")
        if not src or not src.endswith(ext):
            continue
        path = os.path.normpath(os.path.join(root, src)).replace("\\", "/")
        if exclude and exclude.search(path):
            continue
        print(path)


if __name__ == "__main__":
    main()
