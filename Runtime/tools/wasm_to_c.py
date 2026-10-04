#!/usr/bin/env python3
"""Turn the linked GBA guest module (.wasm) into C for the host compilers, plus the game
data file.

1. Moves every active data segment out of the module into <name>.agbdata (format in
   Runtime/wasm/agbw_guest.c), trimming zero runs at both ends (linear memory starts
   zeroed). A GBA game's ROM data is tens of MB: as C arrays it would take ages to
   compile and live in RAM twice (in the program and in the guest memory).
2. Writes agbw_config.h with the guest memory size: what the module uses (up to its
   exported __heap_base, plus a little), see Runtime/wasm/agbw.h.
3. Runs wasm2c, split into several files so they compile in parallel.
4. Makes the generated helpers follow the runtime's memory model by including
   wasm/agbw_mem_ops.h and agbw_ops.h at the right places of the generated -impl.h
   (masked addresses, little-endian layout on any host, GBA division results, no
   call_indirect type check).
5. Replaces the traps wasm-ld puts in place of direct calls whose signature does not
   match the callee (K&R declarations in decomps) with calls that pass the arguments the
   callee takes, as an ARM call would.

Usage: wasm_to_c.py <wasm2c> <in.wasm> <out_dir> <name> <num_outputs> <data_out>
Writes <out_dir>/<name>.h, <name>-impl.h, <name>_0.c ..., agbw_config.h, <name>_stubs.txt
and <data_out>.
"""
import os
import re
import struct
import subprocess
import sys


# ---- wasm binary -------------------------------------------------------------------------
def read_uleb(b, p):
    result = shift = 0
    while True:
        byte = b[p]
        p += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return result, p


def read_sleb(b, p):
    result = shift = 0
    while True:
        byte = b[p]
        p += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            if byte & 0x40:
                result -= 1 << shift
            return result, p


def uleb(v):
    out = bytearray()
    while True:
        byte = v & 0x7F
        v >>= 7
        if v:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def split_data(wasm):
    """Returns (module without its active data segments, [(offset, bytes)], memory bytes,
    start of the writable data: the first data segment not named .rodata, or None)."""
    assert wasm[:8] == b"\0asm\1\0\0\0", "not a wasm module"
    out = bytearray(wasm[:8])
    segments = []
    memory = None
    globals_init = []
    heap_base_global = None
    seg_names = {}
    seg_offsets = []
    p = 8
    while p < len(wasm):
        sec_id = wasm[p]
        size, body = read_uleb(wasm, p + 1)
        end = body + size
        if sec_id == 5:  # memory
            count, q = read_uleb(wasm, body)
            flags, q = read_uleb(wasm, q)
            initial, q = read_uleb(wasm, q)
            memory = initial * 65536
        if sec_id == 6:  # globals: remember i32.const initialisers
            count, q = read_uleb(wasm, body)
            for _ in range(count):
                q += 2  # value type, mutability
                value = None
                if wasm[q] == 0x41:
                    value, q = read_sleb(wasm, q + 1)
                else:
                    while wasm[q] != 0x0B:
                        q += 1
                assert wasm[q] == 0x0B
                q += 1
                globals_init.append(value)
        if sec_id == 7:  # exports: __heap_base
            count, q = read_uleb(wasm, body)
            for _ in range(count):
                n, q = read_uleb(wasm, q)
                name = wasm[q:q + n]
                q += n
                kind = wasm[q]
                index, q = read_uleb(wasm, q + 1)
                if name == b"__heap_base" and kind == 3:
                    heap_base_global = index
        if sec_id == 12:  # data count: goes with the data section
            p = end
            continue
        if sec_id == 0:
            nlen, q = read_uleb(wasm, body)
            if wasm[q:q + nlen] == b"name":
                # drop the data segment names (subsection 9): the segments are gone
                q += nlen
                kept = bytearray(uleb(4) + b"name")
                while q < end:
                    sub = wasm[q]
                    ssize, r = read_uleb(wasm, q + 1)
                    if sub != 9:
                        kept += wasm[q:r + ssize]
                    else:
                        count, t = read_uleb(wasm, r)
                        for _ in range(count):
                            index, t = read_uleb(wasm, t)
                            n, t = read_uleb(wasm, t)
                            seg_names[index] = wasm[t:t + n].decode("utf-8", "replace")
                            t += n
                    q = r + ssize
                out += bytes([0]) + uleb(len(kept)) + kept
                p = end
                continue
        if sec_id != 11:
            out += wasm[p:end]
            p = end
            continue
        count, q = read_uleb(wasm, body)
        for _ in range(count):
            flags, q = read_uleb(wasm, q)
            if flags not in (0, 2):
                sys.exit("wasm_to_c: passive data segments are not supported (link without threads)")
            if flags == 2:
                _, q = read_uleb(wasm, q)
            assert wasm[q] == 0x41, "data offset must be i32.const"
            offset, q = read_sleb(wasm, q + 1)
            assert wasm[q] == 0x0B
            q += 1
            n, q = read_uleb(wasm, q)
            data = wasm[q:q + n]
            q += n
            seg_offsets.append(offset & 0xFFFFFFFF)
            lead = len(data) - len(data.lstrip(b"\0"))
            data = data[lead:].rstrip(b"\0")
            if data:
                segments.append(((offset + lead) & 0xFFFFFFFF, data))
        p = end
    if heap_base_global is not None and globals_init[heap_base_global]:
        # what the module uses: data, BSS and stack end at __heap_base; a little room for
        # the libc heap (the game has its own), rounded to wasm pages
        used = globals_init[heap_base_global] + 256 * 1024
        used = (used + 65535) & ~65535
        # a power of two lets every access mask instead of compare (agbw.h): worth up to 1/8 more
        pow2 = 1 << (used - 1).bit_length()
        if pow2 * 8 <= used * 9:
            used = pow2
        memory = min(memory, used)
    writable = [o for i, o in enumerate(seg_offsets) if seg_names.get(i, ".rodata") != ".rodata"]
    ram_base = min(writable) if writable and seg_names else None
    return bytes(out), segments, memory, ram_base


def write_data(path, segments):
    with open(path, "wb") as f:
        f.write(b"AGBD" + struct.pack("<II", 1, len(segments)))
        for offset, data in segments:
            f.write(struct.pack("<II", offset, len(data)))
            f.write(data)


# ---- generated C: overrides and signature-mismatch stubs -----------------------------
DECL_RE = re.compile(r"^(?:static )?(void|u32|u64|f32|f64) (w2c_\w+)\((w2c_\w+\*[^)]*)\);", re.M)
STUB_RE = re.compile(
    r"^(static )?(void|u32|u64|f32|f64) (w2c_\w+?)_signature_mismatch0x3A(\w+)\((w2c_\w+\* instance[^)]*)\) \{\n.*?^\}\n",
    re.M | re.S)


def param_types(params):
    # "w2c_agb* instance, u32 var_p0" or "w2c_agb*, u32" -> ["u32", ...] (instance dropped)
    parts = [x.strip() for x in params.split(",")][1:]
    return [x.split()[0] for x in parts if x]


def patch_impl(path):
    text = open(path, encoding="utf-8").read()
    anchor = "DEFINE_LOAD(i32_load,"
    assert anchor in text, "wasm2c output changed: no " + anchor
    text = text.replace(anchor, '#include "agbw_mem_ops.h"\n' + anchor, 1)
    m = re.search(r"^#define REM_U\(x, y\).*$", text, re.M)
    assert m, "wasm2c output changed: no REM_U"
    text = text[:m.end()] + '\n#include "agbw_ops.h"' + text[m.end():]
    # agbw_mem_ops.h has its own memory.fill / memory.copy (paged data)
    for fn in ("memory_fill", "memory_copy"):
        decl = "static inline void %s(" % fn
        assert text.count(decl) == 1, "wasm2c output changed: no " + fn
        text = text.replace(decl, "static inline void %s_wasm2c(" % fn)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return text


def fix_stubs(c_files, impl_text):
    decls = {name: (ret, param_types(params)) for ret, name, params in DECL_RE.findall(impl_text)}
    report = []
    for path in c_files:
        text = open(path, encoding="utf-8").read()

        def repl(m):
            static, ret, prefix, target, params = m.groups()
            names = [p.split()[1] for p in params.split(",")[1:] if p.strip()]
            types = param_types(params)
            # a second stub for the same callee is named "<callee>.1" (0x2E = '.')
            callee = prefix + "_" + re.sub(r"0x2E\d+$", "", target)
            # wasm_link.py exports every callee; wasm2c then names the export wrapper (which
            # runs the module constructors) <callee> and the function itself <callee>_0
            if callee + "_0" in decls:
                callee += "_0"
            if callee not in decls:
                report.append(f"{target}: callee not found, left as a trap")
                return m.group(0)
            cret, ctypes = decls[callee]
            args = ["instance"] + [f"({t}){names[i]}" if i < len(names) else f"({t})0" for i, t in enumerate(ctypes)]
            call = f"{callee}({', '.join(args)})"
            if ret == "void":
                body = f"  {call};\n"
            elif cret == "void":
                body = f"  {call};\n  return 0;\n"
            else:
                body = f"  return ({ret}){call};\n"
            report.append(f"{target}: caller ({', '.join(types) or 'void'}) -> {ret}, callee ({', '.join(ctypes) or 'void'}) -> {cret}")
            return f"{static or ''}{ret} {prefix}_signature_mismatch0x3A{target}({params}) {{\n{body}}}\n"

        new = STUB_RE.sub(repl, text)
        if new != text:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(new)
    return report


def main():
    wasm2c, src, out_dir, name, outputs, data_out = sys.argv[1:7]
    os.makedirs(out_dir, exist_ok=True)
    module, segments, memory, ram_base = split_data(open(src, "rb").read())
    if not memory:
        sys.exit("wasm_to_c: the module declares no memory")
    write_data(data_out, segments)
    with open(os.path.join(out_dir, "agbw_config.h"), "w", newline="\n") as f:
        # read-only data below AGBW_RAM_BASE can be paged in from the data file on small
        # consoles (agbw.h); the page holding its end and the writable data stays resident
        page = 1 << 14
        rom_end = (ram_base // page) * page if ram_base else 0
        f.write("/* Generated by wasm_to_c.py. */\n#define AGBW_MEM_BYTES 0x%Xu\n" % memory)
        f.write("#define AGBW_RAM_BASE 0x%Xu\n#define AGBW_PAGE_SHIFT 14\n" % rom_end)
    stripped = os.path.join(out_dir, name + ".code.wasm")
    with open(stripped, "wb") as f:
        f.write(module)
    out_c = os.path.join(out_dir, name + ".c")
    cmd = [wasm2c, stripped, "-n", "agb", "--num-outputs=" + outputs, "--disable-tail-call", "-o", out_c]
    if subprocess.call(cmd) != 0:
        sys.exit("wasm2c failed")
    impl_text = patch_impl(os.path.join(out_dir, name + "-impl.h"))
    c_files = [os.path.join(out_dir, f"{name}_{i}.c") for i in range(int(outputs))]
    report = fix_stubs(c_files, impl_text)
    with open(os.path.join(out_dir, name + "_stubs.txt"), "w", newline="\n") as f:
        f.write("\n".join(report) + "\n")
    total = sum(len(d) for _, d in segments)
    print(f"wasm_to_c: {len(segments)} data segments ({total // 1024} KB) -> {data_out}, "
          f"memory {memory >> 20} MB, {len(report)} signature-mismatch calls adapted")


if __name__ == "__main__":
    main()
