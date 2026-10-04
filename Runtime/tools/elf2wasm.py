#!/usr/bin/env python3
"""
Turns a 32-bit little-endian ELF data object (a GBA data unit assembled for i686 ELF,
where GNU-as features such as macros and .set variables all work) into wasm32
assembly with the same bytes, symbols and pointers:

  * each allocated section becomes a wasm section `.section .<kind>.<unit>_<n>,"",@`;
  * its bytes are written to <out>.bin and pulled in with `.incbin file, offset, size`
    between relocations;
  * each R_386_32 relocation becomes `.4byte symbol+addend` (implicit addend read from
    the section bytes); relocations against section symbols point at a local label;
  * every defined symbol becomes a label with the `.size` wasm requires (up to the next
    symbol in the section, or the section end).

  * aliases ("alias target" lines from gen_memory.py --alias-list) become second labels
    on their targets.

  * pointers to functions listed in functypes.txt (wasm_functypes.py) are emitted with a
    `.functype` declaration, so they become function-table indices.

Usage: elf2wasm.py <in.o> <out.s> <unit-name> [aliases.txt] [functypes.txt]
"""
import os
import struct
import sys

SHT_PROGBITS, SHT_SYMTAB, SHT_NOBITS, SHT_REL = 1, 2, 8, 9
SHF_WRITE, SHF_ALLOC = 1, 2
R_386_32 = 1


def cstr(blob, off):
    end = blob.index(b"\0", off)
    return blob[off:end].decode("utf-8", "surrogateescape")


def main():
    src, dst, unit = sys.argv[1:4]
    aliases = {}
    if len(sys.argv) > 4 and sys.argv[4]:
        for line in open(sys.argv[4], encoding="utf-8"):
            parts = line.split()
            if len(parts) == 2:
                aliases.setdefault(parts[1], []).append(parts[0])
    functypes = {}
    if len(sys.argv) > 5:
        for line in open(sys.argv[5], encoding="utf-8"):
            parts = line.split(None, 1)
            if len(parts) == 2:
                functypes[parts[0]] = parts[1].strip()
    declared = set()
    elf = open(src, "rb").read()
    assert elf[:4] == b"\x7fELF" and elf[4] == 1 and elf[5] == 1, "need a 32-bit little-endian ELF object"
    shoff, = struct.unpack_from("<I", elf, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x2E)
    sections = []
    for i in range(shnum):
        name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from("<10I", elf, shoff + i * shentsize)
        sections.append(dict(name=name, type=typ, flags=flags, off=off, size=size, link=link, info=info, align=align,
                             entsize=entsize))
    shstr = sections[shstrndx]
    for s in sections:
        s["sname"] = cstr(elf, shstr["off"] + s["name"])

    # symbols
    symtab = next((s for s in sections if s["type"] == SHT_SYMTAB), None)
    syms = []
    if symtab:
        strtab = sections[symtab["link"]]
        for i in range(symtab["size"] // 16):
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from("<IIIBBH", elf, symtab["off"] + i * 16)
            syms.append(dict(name=cstr(elf, strtab["off"] + st_name) if st_name else "", value=st_value,
                             bind=st_info >> 4, type=st_info & 15, shndx=st_shndx))

    # relocations per target section
    relocs = {}
    for s in sections:
        if s["type"] != SHT_REL:
            continue
        lst = relocs.setdefault(s["info"], [])
        for i in range(s["size"] // 8):
            r_off, r_info = struct.unpack_from("<II", elf, s["off"] + i * 8)
            typ, symi = r_info & 0xFF, r_info >> 8
            if typ != R_386_32:
                sys.exit(f"elf2wasm: {src}: unsupported relocation type {typ}")
            lst.append((r_off, symi))
        lst.sort()

    blob = bytearray()
    out = []
    bin_path = dst[:-2] + ".bin" if dst.endswith(".s") else dst + ".bin"
    bin_ref = os.path.abspath(bin_path).replace("\\", "/")
    counter = 0

    def sec_label(idx):
        return f".Lsec{idx}"

    for idx, s in enumerate(sections):
        if not (s["flags"] & SHF_ALLOC) or s["type"] not in (SHT_PROGBITS, SHT_NOBITS):
            continue
        if s["size"] == 0 and not any(sy["shndx"] == idx and sy["name"] and sy["type"] != 3 for sy in syms):
            continue
        kind = "bss" if s["type"] == SHT_NOBITS else ("data" if s["flags"] & SHF_WRITE else "rodata")
        counter += 1
        out.append(f'\t.section .{kind}.{unit}_{counter},"",@')
        align = max(1, s["align"])
        out.append(f"\t.p2align {align.bit_length() - 1}")
        out.append(f"{sec_label(idx)}:")
        # labels at offsets
        here = sorted((sy["value"], sy["name"], sy["bind"]) for sy in syms
                      if sy["shndx"] == idx and sy["name"] and sy["type"] in (0, 1, 2))
        offsets = sorted(set(v for v, _, _ in here) | {s["size"]})
        events = {}
        for v, n, b in here:
            events.setdefault(v, []).append((n, b))
        rel = relocs.get(idx, [])
        data = elf[s["off"]:s["off"] + s["size"]] if s["type"] != SHT_NOBITS else None
        pos = 0
        ri = 0
        stops = sorted(set(offsets) | {r for r, _ in rel})

        def emit_bytes(a, b):
            if b <= a:
                return
            if data is None:
                out.append(f"\t.space {b - a}")
            else:
                start = len(blob)
                blob.extend(data[a:b])
                out.append(f'\t.incbin "{bin_ref}", {start}, {b - a}')

        open_syms = []
        for stop in stops:
            emit_bytes(pos, stop)
            pos = max(pos, stop)
            if stop in events:
                for n, b in events[stop]:
                    if b != 0:
                        out.append(f"\t.globl {n}")
                    out.append(f"{n}:")
                    nxt = next((o for o in offsets if o > stop), s["size"])
                    open_syms.append((n, nxt - stop))
                    for alias in aliases.get(n, []):
                        out.append(f"\t.globl {alias}")
                        out.append(f"{alias}:")
                        open_syms.append((alias, nxt - stop))
            while ri < len(rel) and rel[ri][0] == stop:
                r_off, symi = rel[ri]
                sy = syms[symi]
                addend, = struct.unpack_from("<i", data, r_off)
                if sy["type"] == 3 or not sy["name"]:  # section symbol
                    target = sec_label(sy["shndx"])
                else:
                    target = sy["name"]
                    if target in functypes and target not in declared:
                        # a function's address: the table index, declared with its signature
                        out.insert(0, f"\t.functype {target} {functypes[target]}")
                        declared.add(target)
                out.append(f"\t.4byte {target}{addend:+d}" if addend else f"\t.4byte {target}")
                pos = r_off + 4
                ri += 1
        emit_bytes(pos, s["size"])
        end = f".Lend{idx}"
        out.append(f"{end}:")
        out.append(f"\t.size {sec_label(idx)}, {s['size']}")  # referenced by section-relative relocations
        # sizes: each symbol runs to the next symbol offset (or the section end)
        for n, size in open_syms:
            out.append(f"\t.size {n}, {size}")

    with open(bin_path, "wb") as f:
        f.write(blob)
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* Generated by elf2wasm.py. Do not edit. */\n" + "\n".join(out) + "\n")


if __name__ == "__main__":
    main()
