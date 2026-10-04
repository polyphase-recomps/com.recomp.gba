/*
 * Minimal x86-32 instruction decoding for the fault handler: when game code reads or
 * writes a low GBA address through an absolute operand (a NULL pointer folded into a
 * constant), the access is carried out on a stand-in buffer and the instruction is
 * skipped; integer division by zero gets the GBA result. Covers the plain loads/stores
 * the compiler emits; anything else is only skipped.
 */
#if defined(_M_IX86) || defined(__i386__) /* the native (32-bit x86) guest only */
#include <windows.h>

typedef struct
{
    int length;
    int has_modrm;
    int reg;    /* ModRM reg field */
    int opsize; /* 1, 2 or 4 */
    enum { OP_OTHER, OP_LOAD, OP_LOAD_ZX, OP_LOAD_SX, OP_STORE_REG, OP_STORE_IMM } kind;
    unsigned long imm;
} X86Insn;

static const unsigned char *modrm_end(const unsigned char *p)
{
    int mod = p[0] >> 6, rm = p[0] & 7;

    p++;
    if (mod == 3) return p;
    if (rm == 4)
    {
        int base = p[0] & 7;

        p++;
        if (mod == 0 && base == 5) return p + 4;
    }
    else if (mod == 0 && rm == 5)
    {
        return p + 4;
    }
    if (mod == 1) return p + 1;
    if (mod == 2) return p + 4;
    return p;
}

static unsigned long read_le(const unsigned char *p, int size)
{
    unsigned long v = 0;
    int i;

    for (i = 0; i < size; i++) v |= (unsigned long)p[i] << (8 * i);
    return v;
}

static int decode(const unsigned char *start, X86Insn *in)
{
    const unsigned char *p = start;
    int op16 = 0;
    unsigned char op;

    memset(in, 0, sizeof *in);
    for (;; p++)
    {
        unsigned char b = *p;

        if (b == 0x66) op16 = 1;
        else if (b == 0x67 || b == 0xF2 || b == 0xF3 || b == 0xF0 || b == 0x2E || b == 0x36 || b == 0x3E ||
                 b == 0x26 || b == 0x64 || b == 0x65) {}
        else break;
    }
    op = *p++;
    in->opsize = op16 ? 2 : 4;
    if (op == 0x0F)
    {
        unsigned char op2 = *p++;

        if (op2 == 0x38)
        {
            p++;
            p = modrm_end(p);
        }
        else if (op2 == 0x3A)
        {
            p++;
            p = modrm_end(p) + 1;
        }
        else if (op2 >= 0x80 && op2 <= 0x8F)
        {
            p += 4;
        }
        else if (op2 == 0x31 || op2 == 0xA2 || (op2 >= 0xC8 && op2 <= 0xCF) || op2 == 0x0B)
        {
        }
        else
        {
            in->has_modrm = 1;
            in->reg = (p[0] >> 3) & 7;
            if (op2 == 0xB6 || op2 == 0xB7)
            {
                in->kind = OP_LOAD_ZX;
                in->opsize = op2 == 0xB6 ? 1 : 2;
            }
            else if (op2 == 0xBE || op2 == 0xBF)
            {
                in->kind = OP_LOAD_SX;
                in->opsize = op2 == 0xBE ? 1 : 2;
            }
            p = modrm_end(p);
            if (op2 == 0x70 || op2 == 0x71 || op2 == 0x72 || op2 == 0x73 || op2 == 0xA4 || op2 == 0xAC ||
                op2 == 0xBA || op2 == 0xC2 || op2 == 0xC4 || op2 == 0xC5 || op2 == 0xC6)
            {
                p++;
            }
        }
        in->length = (int)(p - start);
        return in->length;
    }
    if (op >= 0xA0 && op <= 0xA3)
    {
        in->kind = op <= 0xA1 ? OP_LOAD : OP_STORE_REG;
        in->reg = 0;
        in->opsize = (op & 1) ? (op16 ? 2 : 4) : 1;
        p += 4;
        in->length = (int)(p - start);
        return in->length;
    }
    if ((op < 0x40 && (op & 7) < 4) || op == 0x62 || op == 0x63 || op == 0x69 || op == 0x6B ||
        (op >= 0x80 && op <= 0x8F) || op == 0xC0 || op == 0xC1 || op == 0xC4 || op == 0xC5 || op == 0xC6 ||
        op == 0xC7 || (op >= 0xD0 && op <= 0xD3) || (op >= 0xD8 && op <= 0xDF) || op == 0xF6 || op == 0xF7 ||
        op == 0xFE || op == 0xFF)
    {
        int reg = (p[0] >> 3) & 7;
        int immsize = 0;

        in->has_modrm = 1;
        in->reg = reg;
        p = modrm_end(p);
        switch (op)
        {
        case 0x80: case 0x82: case 0x83: case 0x6B: case 0xC0: case 0xC1: case 0xC6: immsize = 1; break;
        case 0x81: case 0x69: case 0xC7: immsize = op16 ? 2 : 4; break;
        case 0xF6: immsize = reg < 2 ? 1 : 0; break;
        case 0xF7: immsize = reg < 2 ? (op16 ? 2 : 4) : 0; break;
        default: break;
        }
        if (op == 0x8A || op == 0x8B)
        {
            in->kind = OP_LOAD;
            in->opsize = op == 0x8A ? 1 : (op16 ? 2 : 4);
        }
        else if (op == 0x88 || op == 0x89)
        {
            in->kind = OP_STORE_REG;
            in->opsize = op == 0x88 ? 1 : (op16 ? 2 : 4);
        }
        else if ((op == 0xC6 || op == 0xC7) && reg == 0)
        {
            in->kind = OP_STORE_IMM;
            in->opsize = op == 0xC6 ? 1 : (op16 ? 2 : 4);
            in->imm = read_le(p, immsize);
        }
        p += immsize;
        in->length = (int)(p - start);
        return in->length;
    }
    if ((op >= 0xA4 && op <= 0xA7) || (op >= 0xAA && op <= 0xAF))
    {
        in->length = (int)(p - start);
        return in->length;
    }
    return 0;
}

static DWORD *gpr(CONTEXT *ctx, int r)
{
    switch (r & 7)
    {
    case 0: return &ctx->Eax;
    case 1: return &ctx->Ecx;
    case 2: return &ctx->Edx;
    case 3: return &ctx->Ebx;
    case 4: return &ctx->Esp;
    case 5: return &ctx->Ebp;
    case 6: return &ctx->Esi;
    default: return &ctx->Edi;
    }
}

static unsigned long reg_value(CONTEXT *ctx, int r, int size)
{
    if (size == 1) return r < 4 ? (*gpr(ctx, r) & 0xFF) : ((*gpr(ctx, r - 4) >> 8) & 0xFF);
    if (size == 2) return *gpr(ctx, r) & 0xFFFF;
    return *gpr(ctx, r);
}

static void set_reg(CONTEXT *ctx, int r, int size, unsigned long v)
{
    if (size == 1)
    {
        if (r < 4) *gpr(ctx, r) = (*gpr(ctx, r) & ~0xFFul) | (v & 0xFF);
        else *gpr(ctx, r - 4) = (*gpr(ctx, r - 4) & ~0xFF00ul) | ((v & 0xFF) << 8);
    }
    else if (size == 2)
    {
        *gpr(ctx, r) = (*gpr(ctx, r) & ~0xFFFFul) | (v & 0xFFFF);
    }
    else
    {
        *gpr(ctx, r) = v;
    }
}

/* Emulates the faulting instruction against `mirror` (the RAM view of the address).
 * Returns 1 if the instruction was handled and EIP advanced. */
int port_x86_emulate_access(CONTEXT *ctx, unsigned char *mirror, int *was_skipped)
{
    X86Insn in;
    unsigned long v;

    *was_skipped = 0;
    if (!decode((const unsigned char *)ctx->Eip, &in)) return 0;
    switch (in.kind)
    {
    case OP_LOAD:
        set_reg(ctx, in.reg, in.opsize, read_le(mirror, in.opsize));
        break;
    case OP_LOAD_ZX:
        *gpr(ctx, in.reg) = read_le(mirror, in.opsize);
        break;
    case OP_LOAD_SX:
        v = read_le(mirror, in.opsize);
        *gpr(ctx, in.reg) = in.opsize == 1 ? (unsigned long)(long)(signed char)v : (unsigned long)(long)(short)v;
        break;
    case OP_STORE_REG:
        v = reg_value(ctx, in.reg, in.opsize);
        memcpy(mirror, &v, (size_t)in.opsize);
        break;
    case OP_STORE_IMM:
        memcpy(mirror, &in.imm, (size_t)in.opsize);
        break;
    default:
        *was_skipped = 1;
        break;
    }
    ctx->Eip += (DWORD)in.length;
    return 1;
}

/*
 * Integer division by zero (or INT_MIN / -1) does not trap on the GBA: the game's
 * libgcc __divsi3/__modsi3 call __div0 and return 0. Gives div/idiv that result and
 * skips it. Returns 1 if handled.
 */
int port_x86_emulate_divide(CONTEXT *ctx, int overflow)
{
    X86Insn in;
    const unsigned char *p = (const unsigned char *)ctx->Eip;
    int op16 = 0;
    DWORD dividend = ctx->Eax;

    while (*p == 0x66 || *p == 0x2E || *p == 0x3E || *p == 0x26 || *p == 0x36 || *p == 0x64 || *p == 0x65)
    {
        if (*p == 0x66) op16 = 1;
        p++;
    }
    if ((*p != 0xF7 && *p != 0xF6) || op16) return 0;
    if (!decode((const unsigned char *)ctx->Eip, &in) || (in.reg != 6 && in.reg != 7)) return 0;
    (void)dividend;
    if (*p == 0xF6)
    {
        /* 8-bit: AL = quotient, AH = remainder */
        ctx->Eax &= ~0xFFFFul;
    }
    else if (overflow)
    {
        ctx->Eax = 0x80000000u; /* INT_MIN / -1 wraps on ARM */
        ctx->Edx = 0;
    }
    else
    {
        ctx->Eax = 0;
        ctx->Edx = 0;
    }
    ctx->Eip += (DWORD)in.length;
    return 1;
}

#endif
