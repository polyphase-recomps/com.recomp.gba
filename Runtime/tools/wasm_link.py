#!/usr/bin/env python3
"""wasm-ld wrapper for the guest module.

wasm-ld replaces a direct call whose signature differs from the callee's (common
in decomps: K&R declarations, prototypes that disagree between files) with a trap
stub and may then drop the callee as unused. wasm_to_c.py turns those stubs back
into calls, so every such callee is kept: the link is repeated with each of them
exported when the first link reports mismatches.

The arguments go through a response file (hundreds of objects exceed the Windows
command-line limit).

Usage: wasm_link.py <wasm-ld> <wasm-ld args...> [--inputs-from=<list file>]  (needs -o <out>)
"""
import re
import subprocess
import sys


def quote(arg):
    return '"' + arg.replace("\\", "/").replace('"', '\\"') + '"'


def run(ld, args, rsp):
    with open(rsp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(quote(a) for a in args) + "\n")
    r = subprocess.run([ld, "@" + rsp], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    ld, args = sys.argv[1], []
    for a in sys.argv[2:]:
        # --inputs-from=<file>: one input per line (keeps the build tool's command short)
        if a.startswith("--inputs-from="):
            args += [l.strip() for l in open(a.split("=", 1)[1], encoding="utf-8") if l.strip()]
        else:
            args.append(a)
    rsp = args[args.index("-o") + 1] + ".rsp"
    code, out = run(ld, args, rsp)
    names = sorted(set(re.findall(r"function signature mismatch: (\S+)", out)))
    if code == 0 and names:
        code, out = run(ld, args + [f"--export-if-defined={n}" for n in names], rsp)
    # keep the output short: one line per mismatch instead of wasm-ld's three
    lines = [l for l in out.splitlines() if not l.startswith(">>>") and "signature mismatch" not in l]
    if names:
        lines.append(f"wasm_link: {len(names)} signature mismatches (adapted by wasm_to_c.py): " + " ".join(names))
    if lines:
        print("\n".join(lines))
    return code


if __name__ == "__main__":
    sys.exit(main())
