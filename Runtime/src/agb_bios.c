/* GBA BIOS calls (SWIs) in portable C. */
#include "agb.h"
#include "agb_host.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed short s16;
typedef signed int s32;

void agb_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    agb_host_log(buf);
}

void agb_cpu_set(const void *src, void *dst, unsigned int control)
{
    u32 count = control & 0x1FFFFF;
    int fixed = (control >> 24) & 1;

    if (control & (1u << 26))
    {
        const u32 *s = (const u32 *)src;
        u32 *d = (u32 *)dst;
        u32 v = *s;

        while (count--)
        {
            *d++ = fixed ? v : *s++;
        }
    }
    else
    {
        const u16 *s = (const u16 *)src;
        u16 *d = (u16 *)dst;
        u16 v = *s;

        while (count--)
        {
            *d++ = fixed ? v : *s++;
        }
    }
}

void agb_cpu_fast_set(const void *src, void *dst, unsigned int control)
{
    /* 32-bit units, length rounded up to 8 words */
    u32 count = ((control & 0x1FFFFF) + 7) & ~7u;
    int fixed = (control >> 24) & 1;
    const u32 *s = (const u32 *)src;
    u32 *d = (u32 *)dst;
    u32 v = *s;

    while (count--)
    {
        *d++ = fixed ? v : *s++;
    }
}

void agb_lz77_uncomp(const void *src, void *dst, int vram)
{
    const u8 *s = (const u8 *)src;
    u8 *d = (u8 *)dst;
    u32 size = s[1] | (s[2] << 8) | (s[3] << 16);
    u32 out = 0;

    (void)vram;
    s += 4;
    while (out < size)
    {
        u8 flags = *s++;
        int i;

        for (i = 0; i < 8 && out < size; i++, flags <<= 1)
        {
            if (flags & 0x80)
            {
                u32 len = (s[0] >> 4) + 3;
                u32 disp = (((s[0] & 0xF) << 8) | s[1]) + 1;

                s += 2;
                while (len-- && out < size)
                {
                    d[out] = d[out - disp];
                    out++;
                }
            }
            else
            {
                d[out++] = *s++;
            }
        }
    }
}

void agb_rl_uncomp(const void *src, void *dst, int vram)
{
    const u8 *s = (const u8 *)src;
    u8 *d = (u8 *)dst;
    u32 size = s[1] | (s[2] << 8) | (s[3] << 16);
    u32 out = 0;

    (void)vram;
    s += 4;
    while (out < size)
    {
        u8 flag = *s++;

        if (flag & 0x80)
        {
            u32 len = (flag & 0x7F) + 3;
            u8 v = *s++;

            while (len-- && out < size) d[out++] = v;
        }
        else
        {
            u32 len = (flag & 0x7F) + 1;

            while (len-- && out < size) d[out++] = *s++;
        }
    }
}

void agb_huff_uncomp(const void *src, void *dst)
{
    const u8 *s = (const u8 *)src;
    u32 header = s[0] | (s[1] << 8) | (s[2] << 16) | ((u32)s[3] << 24);
    u32 bits = header & 0xF;
    u32 size = header >> 8;
    const u8 *tree = s + 4;
    const u32 *stream = (const u32 *)(tree + (tree[0] + 1) * 2);
    u32 *out = (u32 *)dst;
    u32 acc = 0, accBits = 0, written = 0;
    const u8 *node = tree + 1;
    u32 word = 0;
    int left = 0;

    while (written < size)
    {
        int bit;

        if (left == 0)
        {
            word = *stream++;
            left = 32;
        }
        bit = (word >> 31) & 1;
        word <<= 1;
        left--;
        {
            u32 offset = ((*node & 0x3F) + 1) * 2;
            const u8 *child = (const u8 *)(((unsigned long)node & ~1ul) + offset + bit);
            int isLeaf = (*node >> (7 - bit)) & 1;

            if (isLeaf)
            {
                acc |= (u32)(*child) << accBits;
                accBits += bits;
                node = tree + 1;
                if (accBits == 32)
                {
                    *out++ = acc;
                    acc = 0;
                    accBits = 0;
                    written += 4;
                }
            }
            else
            {
                node = child;
            }
        }
    }
}

void agb_register_ram_reset(unsigned int flags)
{
    if (flags & 0x01) memset(agb_ewram, 0, AGB_EWRAM_SIZE);
    if (flags & 0x02) memset(agb_iwram, 0, AGB_IWRAM_SIZE - 0x200);
    if (flags & 0x04) memset(agb_pltt, 0, AGB_PLTT_SIZE);
    if (flags & 0x08) memset(agb_vram, 0, AGB_VRAM_SIZE);
    if (flags & 0x10) memset(agb_oam, 0, AGB_OAM_SIZE);
    if (flags & 0x80)
    {
        u16 keys = AGB_IO16(0x130);

        memset(agb_io, 0, 0x60);
        memset(agb_io + 0xB0, 0, 0x50);
        AGB_IO16(0x130) = keys;
        AGB_IO16(0x00) = 0x80; /* forced blank */
        AGB_IO16(0x20) = 0x100;
        AGB_IO16(0x26) = 0x100;
        AGB_IO16(0x30) = 0x100;
        AGB_IO16(0x36) = 0x100;
    }
}

unsigned int agb_sqrt(unsigned int value)
{
    unsigned int r = (unsigned int)sqrt((double)value);

    while (r * r > value) r--;
    while ((r + 1) * (r + 1) <= value) r++;
    return r;
}

int agb_div(int num, int den)
{
    if (den == 0) return num < 0 ? 1 : -1;
    return num / den;
}

int agb_mod(int num, int den)
{
    if (den == 0) return num;
    return num % den;
}

unsigned short agb_arctan2(int x, int y)
{
    double a = atan2((double)y, (double)x);

    if (a < 0) a += 2 * 3.14159265358979323846;
    return (unsigned short)(a * 65536.0 / (2 * 3.14159265358979323846));
}

static double sin_turn(unsigned int angle)
{
    return sin((angle & 0xFFFF) * (2 * 3.14159265358979323846) / 65536.0);
}

/* BgAffineSrcData: s32 texX, texY; s16 scrX, scrY, sx, sy; u16 alpha (high byte used) */
void agb_bg_affine_set(const void *srcv, void *dstv, int count)
{
    const u8 *src = (const u8 *)srcv;
    u8 *dst = (u8 *)dstv;

    while (count-- > 0)
    {
        s32 texX = *(const s32 *)(src + 0), texY = *(const s32 *)(src + 4);
        s16 scrX = *(const s16 *)(src + 8), scrY = *(const s16 *)(src + 10);
        s16 sx = *(const s16 *)(src + 12), sy = *(const s16 *)(src + 14);
        u16 alpha = *(const u16 *)(src + 16);
        s32 c = (s32)(cos(((alpha >> 8) & 0xFF) * (2 * 3.14159265358979323846) / 256.0) * 16384.0);
        s32 s = (s32)(sin(((alpha >> 8) & 0xFF) * (2 * 3.14159265358979323846) / 256.0) * 16384.0);
        s16 pa = (s16)((sx * c) >> 14), pb = (s16)(-(sx * s) >> 14);
        s16 pc = (s16)((sy * s) >> 14), pd = (s16)((sy * c) >> 14);

        *(s16 *)(dst + 0) = pa;
        *(s16 *)(dst + 2) = pb;
        *(s16 *)(dst + 4) = pc;
        *(s16 *)(dst + 6) = pd;
        *(s32 *)(dst + 8) = texX - (pa * scrX + pb * scrY);
        *(s32 *)(dst + 12) = texY - (pc * scrX + pd * scrY);
        src += 20;
        dst += 16;
    }
}

/* ObjAffineSrcData: s16 sx, sy; u16 alpha; padding; output every `offset` bytes */
void agb_obj_affine_set(const void *srcv, void *dstv, int count, int offset)
{
    const u8 *src = (const u8 *)srcv;
    u8 *dst = (u8 *)dstv;

    while (count-- > 0)
    {
        s16 sx = *(const s16 *)(src + 0), sy = *(const s16 *)(src + 2);
        u16 alpha = *(const u16 *)(src + 4);
        s32 c = (s32)(cos(((alpha >> 8) & 0xFF) * (2 * 3.14159265358979323846) / 256.0) * 16384.0);
        s32 s = (s32)(sin(((alpha >> 8) & 0xFF) * (2 * 3.14159265358979323846) / 256.0) * 16384.0);

        *(s16 *)(dst + 0 * offset) = (s16)((sx * c) >> 14);
        *(s16 *)(dst + 1 * offset) = (s16)(-(sx * s) >> 14);
        *(s16 *)(dst + 2 * offset) = (s16)((sy * s) >> 14);
        *(s16 *)(dst + 3 * offset) = (s16)((sy * c) >> 14);
        src += 8;
        dst += 4 * offset;
    }
    (void)sin_turn;
}
