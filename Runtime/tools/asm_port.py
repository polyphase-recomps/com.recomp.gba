"""Turns a GBA decomp data unit (ARM GNU-as syntax) into target-neutral GNU-as.

Data units only hold labels, data directives, .incbin and macro invocations, so
they assemble on any GNU-compatible assembler once the ARM specifics are gone:
  * ARM/Thumb mode and ELF type/size directives are dropped;
  * `.align n` (power of two on ARM) becomes `.p2align n`; `.word`/`.long` become
    `.4byte`, `.hword`/`.short` become `.2byte` (their sizes differ per target);
  * `.include` files are inlined (converted the same way) and `.incbin` paths are
    made absolute, so the output can live anywhere;
  * with --underscore, every symbol gets the leading underscore that C symbols carry
    on targets such as i386 COFF;
  * `.equ/.set name, other_symbol` aliases are substituted at their uses (COFF cannot
    hold a symbol that aliases an undefined one), unless the name is exported.

For wasm32 the output is assembled for i686 ELF and translated by elf2wasm.py.

Usage: asm_port.py <decomp_root> <input.s> <output.s> [--underscore]
"""
import os
import re
import sys

IDENT = re.compile(r"(?<![\w.$\\])([A-Za-z_$][\w.$]*|\.[A-Za-z_$][\w.$]*)")  # a lone "." is the location counter
LABEL = re.compile(r"^(\s*)([A-Za-z_.$][\w.$]*):(.*)$")
DROP = {".syntax", ".thumb", ".arm", ".thumb_func", ".code", ".type", ".size", ".force_thumb", ".fpu", ".cpu",
        ".eabi_attribute", ".ident"}
DATA = {".byte", ".2byte", ".4byte", ".8byte", ".fill", ".space", ".skip", ".zero"}
ALIAS_IDENT = re.compile(r"(?<![\w.$\\])([A-Za-z_$][\w.$]*)")
SYMBOL_ONLY = re.compile(r"^\s*[A-Za-z_$][\w.$]*\s*$")
RENAME = {".word": ".4byte", ".long": ".4byte", ".int": ".4byte", ".hword": ".2byte", ".short": ".2byte"}


def prefix_expr(expr):
    """Prefixes symbol identifiers in an operand expression (not numbers, not \\params)."""
    out, pos = [], 0
    for m in re.finditer(r'"[^"]*"', expr):
        out.append(IDENT.sub(lambda i: "_" + i.group(1), expr[pos:m.start()]))
        out.append(m.group(0))
        pos = m.end()
    out.append(IDENT.sub(lambda i: "_" + i.group(1), expr[pos:]))
    return "".join(out)


class Converter:
    def __init__(self, root, underscore):
        self.root = root
        self.underscore = underscore
        self.in_macro = 0
        self.aliases = {}
        self.exported = set()

    def resolve(self, path, base):
        for cand in (os.path.join(base, path), os.path.join(self.root, path), os.path.join(self.root, "include", path)):
            if os.path.exists(cand):
                return os.path.abspath(cand)
        raise FileNotFoundError(path)

    def sym(self, name):
        if not self.underscore or name.startswith("\\") or name == ".":
            return name
        return "_" + name

    def expr(self, text):
        text = prefix_expr(text) if self.underscore else text
        if self.aliases:
            text = ALIAS_IDENT.sub(lambda m: self.aliases.get(m.group(1), m.group(1)), text)
        return text

    def line(self, raw, base, out):
        code, comment = raw, ""
        if "@" in raw and '"' not in raw:
            code, comment = raw.split("@", 1)
        code = code.rstrip("\n").rstrip()
        stripped = code.strip()
        if not stripped:
            return
        m = LABEL.match(code)
        if m and not stripped.startswith("."):
            out.append(f"{m.group(1)}{self.sym(m.group(2))}:")
            rest = m.group(3).strip()
            if rest:
                self.line(rest, base, out)
            return
        if m and stripped.startswith(".") and ":" in stripped.split()[0]:
            # local label such as .L123:
            out.append(f"{m.group(1)}{self.sym(m.group(2))}:")
            return
        parts = stripped.split(None, 1)
        word, args = parts[0], (parts[1] if len(parts) > 1 else "")
        if word.startswith("."):
            low = word.lower()
            if low in DROP:
                return
            if low == ".include":
                path = self.resolve(args.strip().strip('"'), base)
                for sub in open(path, encoding="utf-8", errors="surrogateescape"):
                    self.line(sub, os.path.dirname(path), out)
                return
            if low == ".incbin":
                m2 = re.match(r'"([^"]+)"(.*)$', args.strip())
                out.append(f'\t.incbin "{self.resolve(m2.group(1), base).replace(os.sep, "/")}"{m2.group(2)}')
                return
            if low == ".align":
                n = args.split(",")[0].strip()
                out.append(f"\t.p2align {n}")
                return
            if low in (".macro",):
                self.in_macro += 1
                out.append(f"\t{word} {args}")
                return
            if low in (".endm",):
                self.in_macro -= 1
                out.append(f"\t{word}")
                return
            if low in (".global", ".globl", ".weak", ".local"):
                if low != ".local":
                    self.exported.update(self.sym(a.strip()) for a in args.split(","))
                out.append(f"\t{word} " + ", ".join(self.sym(a.strip()) for a in args.split(",")))
                return
            if low in (".equ", ".set", ".equiv"):
                name, value = args.split(",", 1)
                if not self.in_macro and SYMBOL_ONLY.match(value) and self.sym(name.strip()) not in self.exported:
                    self.aliases[self.sym(name.strip())] = self.expr(value.strip())
                    return
                out.append(f"\t{word} {self.sym(name.strip())}, {self.expr(value)}")
                return
            if low in RENAME:
                out.append(f"\t{RENAME[low]} {self.expr(args)}")
                return
            if low in DATA or low in (".if", ".ifdef", ".ifndef", ".elseif"):
                if low in (".ifdef", ".ifndef"):
                    out.append(f"\t{word} {self.sym(args.strip())}")
                else:
                    out.append(f"\t{word} {self.expr(args)}")
                return
            out.append(f"\t{word} {args}".rstrip())
            return
        # macro invocation: keep the macro name, rename symbols in the operands
        out.append(f"\t{word} {self.expr(args)}".rstrip())


def main():
    root, src, dst = sys.argv[1], sys.argv[2], sys.argv[3]
    conv = Converter(os.path.abspath(root), "--underscore" in sys.argv[4:])
    out = []
    base = os.path.dirname(os.path.abspath(src))
    for raw in open(src, encoding="utf-8", errors="surrogateescape"):
        conv.line(raw, base, out)
    text = "\n".join(out) + "\n"
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    if not os.path.exists(dst) or open(dst, encoding="utf-8", errors="surrogateescape").read() != text:
        with open(dst, "w", encoding="utf-8", errors="surrogateescape", newline="\n") as f:
            f.write(text)


if __name__ == "__main__":
    main()
