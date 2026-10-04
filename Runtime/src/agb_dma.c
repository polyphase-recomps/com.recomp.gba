/*
 * DMA channels. The registers in agb_io are the source of truth, as on the console:
 * DmaSet (compat gba/macro.h) runs immediate transfers at once, and HBlank/VBlank-timed
 * channels are picked up by the frame loop from whatever the registers hold then, so a
 * game that writes REG_DMAxCNT directly (DmaStop, "REG_DMA0CNT = 0", ...) behaves too.
 * Registers hold native pointers (32-bit targets).
 */
#include "agb.h"
#include "agb_dma.h"

#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

AgbDmaChannel agb_dma[4];

/* register values the internal pointers were loaded from */
static u32 sLatchSad[4], sLatchDad[4];

void *agb_translate(unsigned int a)
{
    switch (a >> 24)
    {
    case 0x00: return a < 0x4000 ? agb_bios + a : 0;
    case 0x02: return agb_ewram + (a & (AGB_EWRAM_SIZE - 1));
    case 0x03: return agb_iwram + (a & (AGB_IWRAM_SIZE - 1));
    case 0x04: return (a & 0xFFFFFF) < AGB_IO_SIZE ? agb_io + (a & 0x3FF) : 0;
    case 0x05: return agb_pltt + (a & (AGB_PLTT_SIZE - 1));
    case 0x06:
        a &= 0x1FFFF;
        if (a >= AGB_VRAM_SIZE) a -= 0x8000;
        return agb_vram + a;
    case 0x07: return agb_oam + (a & (AGB_OAM_SIZE - 1));
    case 0x0E: return agb_sram + (a & (AGB_SRAM_SIZE - 1));
    default: return 0;
    }
}

static int is_fifo(const void *p)
{
    return p == (void *)(agb_io + 0xA0) || p == (void *)(agb_io + 0xA4);
}

/* Runs one transfer of `count` units with the channel's address modes. */
void agb_dma_transfer(AgbDmaChannel *ch, u32 count)
{
    u32 control = ch->control;
    int wide = (control >> 26) & 1;
    int unit = wide ? 4 : 2;
    int dstMode = (control >> 21) & 3, srcMode = (control >> 23) & 3;
    u8 *src = (u8 *)ch->src, *dst = (u8 *)ch->dst;
    int srcStep = srcMode == 1 ? -unit : srcMode == 2 ? 0 : unit;
    int dstStep = dstMode == 1 ? -unit : dstMode == 2 ? 0 : unit;
    u32 i;

    if (is_fifo(dst) || src == 0 || dst == 0) return;
    if (srcStep == unit && dstStep == unit && (src + count * unit <= dst || dst + count * unit <= src))
    {
        memcpy(dst, src, count * unit);
        src += count * unit;
        dst += count * unit;
    }
    else
    {
        for (i = 0; i < count; i++)
        {
            if (wide)
                *(u32 *)dst = *(const u32 *)src;
            else
                *(u16 *)dst = *(const u16 *)src;
            src += srcStep;
            dst += dstStep;
        }
    }
    ch->src = src;
    if (dstMode != 3) ch->dst = dst; /* mode 3 = increment/reload: the destination restarts */
}

static void load_from_regs(int n)
{
    u32 base = 0xB0 + n * 12;
    AgbDmaChannel *ch = &agb_dma[n];

    sLatchSad[n] = AGB_IO32(base);
    sLatchDad[n] = AGB_IO32(base + 4);
    ch->src = (const void *)(unsigned long)sLatchSad[n];
    ch->dst = (void *)(unsigned long)sLatchDad[n];
    ch->reload = ch->dst;
    ch->control = AGB_IO32(base + 8);
}

static u32 unit_count(int n, u32 control)
{
    u32 count = control & 0xFFFF;

    if (count == 0) count = n == 3 ? 0x10000 : 0x4000;
    return count;
}

void agb_dma_set(int n, const void *src, void *dst, unsigned int control)
{
    u32 base;

    if (n < 0 || n > 3) return;
    base = 0xB0 + n * 12;
    AGB_IO32(base) = (u32)(unsigned long)src;
    AGB_IO32(base + 4) = (u32)(unsigned long)dst;
    AGB_IO32(base + 8) = control;
    load_from_regs(n);
    if ((control & 0x80000000u) && ((control >> 28) & 3) == 0)
    {
        agb_dma_transfer(&agb_dma[n], unit_count(n, control));
        AGB_IO32(base + 8) = control & ~0x80000000u;
        agb_dma[n].control = AGB_IO32(base + 8);
    }
}

void agb_dma_sync(int n)
{
    if (n < 0 || n > 3) return;
    agb_dma_set(n, (const void *)(unsigned long)AGB_IO32(0xB0 + n * 12),
                (void *)(unsigned long)AGB_IO32(0xB4 + n * 12), AGB_IO32(0xB8 + n * 12));
}

/* Called by the frame loop at each HBlank (lines 0..159) and once at VBlank. */
void agb_dma_timing(int timing)
{
    int n;

    for (n = 0; n < 4; n++)
    {
        AgbDmaChannel *ch = &agb_dma[n];
        u32 base = 0xB0 + n * 12;
        u32 control = AGB_IO32(base + 8);

        if (!(control & 0x80000000u))
        {
            ch->control = control;
            continue;
        }
        if ((int)((control >> 28) & 3) != timing) continue;
        /* (re)enabled or reprogrammed behind our back: reload the internal pointers */
        if (control != ch->control || AGB_IO32(base) != sLatchSad[n] || AGB_IO32(base + 4) != sLatchDad[n])
            load_from_regs(n);
        if (((control >> 21) & 3) == 3) ch->dst = ch->reload;
        agb_dma_transfer(ch, unit_count(n, control));
        if (!(control & (1u << 25))) /* not repeating */
        {
            AGB_IO32(base + 8) = control & ~0x80000000u;
            ch->control = AGB_IO32(base + 8);
        }
    }
}
