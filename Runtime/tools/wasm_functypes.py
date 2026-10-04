#!/usr/bin/env python3
"""
Lists the signatures of the functions defined in wasm object files, as `.functype`
lines for elf2wasm.py: a data unit that stores a function's address (a handler table)
must declare the function's wasm signature so the assembler emits a function-table
index instead of a memory address.

Usage: wasm_functypes.py <out.txt> [--inputs-from=<list>] [objects...]
Output lines: `<name> (<params>) -> (<results>)`
"""
import sys

VALTYPES = {0x7F: "i32", 0x7E: "i64", 0x7D: "f32", 0x7C: "f64", 0x7B: "v128", 0x70: "funcref", 0x6F: "externref"}


def uleb(b, p):
    result = shift = 0
    while True:
        byte = b[p]
        p += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return result, p


def name(b, p):
    n, p = uleb(b, p)
    return b[p:p + n].decode("utf-8", "surrogateescape"), p + n


def functypes(path):
    b = open(path, "rb").read()
    if b[:4] != b"\0asm":
        return {}
    types, func_types, imported = [], [], 0
    symbols = []
    p = 8
    while p < len(b):
        sec, p = b[p], p + 1
        size, p = uleb(b, p)
        end = p + size
        if sec == 1:  # types
            count, q = uleb(b, p)
            for _ in range(count):
                assert b[q] == 0x60
                q += 1
                np, q = uleb(b, q)
                params = [VALTYPES.get(b[q + i], "i32") for i in range(np)]
                q += np
                nr, q = uleb(b, q)
                results = [VALTYPES.get(b[q + i], "i32") for i in range(nr)]
                q += nr
                types.append(f"({', '.join(params)}) -> ({', '.join(results)})")
        elif sec == 2:  # imports
            count, q = uleb(b, p)
            for _ in range(count):
                _, q = name(b, q)
                _, q = name(b, q)
                kind, q = b[q], q + 1
                if kind == 0:
                    _, q = uleb(b, q)
                    imported += 1
                elif kind == 1:  # table: reftype + limits
                    q += 1
                    flags, q = uleb(b, q)
                    _, q = uleb(b, q)
                    if flags & 1:
                        _, q = uleb(b, q)
                elif kind == 2:  # memory: limits
                    flags, q = uleb(b, q)
                    _, q = uleb(b, q)
                    if flags & 1:
                        _, q = uleb(b, q)
                elif kind == 3:  # global
                    q += 2
                elif kind == 4:  # tag
                    q += 1
                    _, q = uleb(b, q)
        elif sec == 3:  # function
            count, q = uleb(b, p)
            for _ in range(count):
                t, q = uleb(b, q)
                func_types.append(t)
        elif sec == 0:  # custom: linking
            sname, q = name(b, p)
            if sname == "linking":
                _, q = uleb(b, q)  # version
                while q < end:
                    sub, q = b[q], q + 1
                    ssize, q = uleb(b, q)
                    send = q + ssize
                    if sub == 8:  # symbol table
                        count, r = uleb(b, q)
                        for _ in range(count):
                            kind, r = b[r], r + 1
                            flags, r = uleb(b, r)
                            undefined = flags & 0x10
                            if kind in (0, 2, 4, 5):
                                index, r = uleb(b, r)
                                sym = None
                                if not undefined or flags & 0x40:
                                    sym, r = name(b, r)
                                if kind == 0 and not undefined and sym and not flags & 0x02:
                                    symbols.append((sym, index))
                            elif kind == 1:
                                _, r = name(b, r)
                                if not undefined:
                                    for _ in range(3):
                                        _, r = uleb(b, r)
                            elif kind == 3:
                                _, r = uleb(b, r)
                    q = send
        p = end
    out = {}
    for sym, index in symbols:
        local = index - imported
        if 0 <= local < len(func_types):
            out[sym] = types[func_types[local]]
    return out


def main():
    dst, inputs = sys.argv[1], []
    for a in sys.argv[2:]:
        if a.startswith("--inputs-from="):
            inputs += [l.strip() for l in open(a.split("=", 1)[1], encoding="utf-8") if l.strip()]
        else:
            inputs.append(a)
    sigs = {}
    for path in inputs:
        sigs.update(functypes(path))
    text = "".join(f"{n} {t}\n" for n, t in sorted(sigs.items()))
    try:
        if open(dst, encoding="utf-8").read() == text:
            return
    except OSError:
        pass
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


if __name__ == "__main__":
    main()
