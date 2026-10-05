/*
 * The frame: BIOS VBlankIntrWait runs the 228 scanlines that the CPU would have spent
 * waiting, with HBlank DMA and HBlank/VCount/VBlank interrupts at the right lines, then
 * presents the picture, mixes the frame's audio and paces to 59.73 Hz.
 *
 * Game code that polls REG_VCOUNT / REG_DISPSTAT goes through agb_vcount_ptr() /
 * agb_dispstat_ptr() (compat io_reg.h), which advance a simulated beam on every read so
 * busy-wait loops terminate.
 */
#include "agb.h"
#include "agb_dma.h"
#include "agb_bridge.h"
#include "agb_host.h"
#include "agb_internal.h"

#include <string.h>

typedef unsigned short u16;
typedef unsigned int u32;

static AgbIrqDispatch sDispatch;
static unsigned int sFrames;
static u16 sFrame[240 * 160];
static int sInFrame; /* re-entrancy guard: an IRQ handler calling VBlankIntrWait */
static int sBeam = 160; /* simulated scanline between frames */

void agb_set_irq_dispatch(AgbIrqDispatch dispatch)
{
    sDispatch = dispatch;
}

unsigned int agb_frame_count(void)
{
    return sFrames;
}

/* Raises one interrupt the way the hardware + crt0 would. */
void agb_raise_irq(unsigned int bit)
{
    AGB_IO16(0x202) |= (u16)bit; /* IF */
    if (!(AGB_IO16(0x208) & 1)) return; /* IME */
    if (!(AGB_IO16(0x200) & bit)) return; /* IE */
    AGB_IO16(0x202) &= (u16)~bit;
    AGB_IO16(0x208) = 0; /* the handler runs with IME off, as crt0's does */
    if (sDispatch) sDispatch(bit);
    AGB_IO16(0x208) = 1;
}

static void set_beam(int line)
{
    u16 stat = AGB_IO16(0x04);
    int vcountTarget = stat >> 8;

    stat &= ~7;
    if (line >= 160 && line < 227) stat |= 1;
    if (line == vcountTarget) stat |= 4;
    AGB_IO16(0x04) = stat;
    AGB_IO16(0x06) = (u16)line;
}

static void run_frame(int force_irqs);

/* One step of the simulated beam for polling code; entering VBlank runs the frame, so a
 * game loop that waits on VCOUNT (instead of VBlankIntrWait) still sees time pass. */
static void beam_step(void)
{
    sBeam = (sBeam + 1) % 228;
    if (sBeam == 160)
    {
        run_frame(1);
        return;
    }
    set_beam(sBeam);
}

volatile unsigned short *agb_vcount_ptr(void)
{
    if (!sInFrame) beam_step();
    return (volatile unsigned short *)(agb_io + 0x06);
}

volatile unsigned short *agb_dispstat_ptr(void)
{
    if (!sInFrame)
    {
        static int sHblank;

        /* alternate the HBlank flag per read and step the beam every other read */
        sHblank ^= 1;
        if (!sHblank) beam_step();
        else set_beam(sBeam);
        if (sHblank) AGB_IO16(0x04) |= 2;
    }
    return (volatile unsigned short *)(agb_io + 0x04);
}

static void scanline_irqs(int line, int hblank)
{
    u16 stat = AGB_IO16(0x04);

    if (hblank)
    {
        AGB_IO16(0x04) |= 2;
        if (stat & 0x10) agb_raise_irq(AGB_IRQ_HBLANK);
        AGB_IO16(0x04) &= ~2;
    }
    else if ((stat & 0x20) && line == (stat >> 8))
    {
        agb_raise_irq(AGB_IRQ_VCOUNT);
    }
}

void agb_frame_reset(void)
{
    sInFrame = 0;
    sBeam = 160;
}

void agb_vblank_intr_wait(void)
{
    if (sInFrame) return;
    run_frame(0);
}

/* force_irqs: the frame was reached by polling, often inside an IME=0 window (reading
 * VCOUNT together with a counter the VBlank handler advances); the interrupts are
 * delivered anyway, as they would be a moment later on the console. */
static void run_frame(int force_irqs)
{
    int line;
    u16 ime = AGB_IO16(0x208);

    sInFrame = 1;
    if (force_irqs) AGB_IO16(0x208) = 1;

    /* visible lines: draw, then HBlank (DMA + IRQ) */
    for (line = 0; line < 160; line++)
    {
        set_beam(line);
        scanline_irqs(line, 0);
        agb_ppu_render_line(line, sFrame + line * 240);
        agb_dma_timing(2);
        scanline_irqs(line, 1);
    }

    /* VBlank: VBlank DMA, then the VBlank interrupt (where the game runs m4aSoundMain etc.) */
    set_beam(160);
    scanline_irqs(160, 0);
    agb_dma_timing(1);
    agb_raise_irq(AGB_IRQ_VBLANK);
    for (line = 161; line < 228; line++)
    {
        set_beam(line);
        scanline_irqs(line, 0);
        if (line < 227) scanline_irqs(line, 1); /* HBlank IRQs also fire in VBlank */
    }

    agb_audio_frame();
    agb_save_poll(0);
    if ((int)sFrames == agb_host_debug_frame())
    {
        agb_host_debug_dump("io", agb_io, AGB_IO_SIZE);
        agb_host_debug_dump("pltt", agb_pltt, AGB_PLTT_SIZE);
        agb_host_debug_dump("vram", agb_vram, AGB_VRAM_SIZE);
        agb_host_debug_dump("oam", agb_oam, AGB_OAM_SIZE);
        agb_host_debug_dump("ewram", agb_ewram, AGB_EWRAM_SIZE);
        agb_host_debug_dump("iwram", agb_iwram, AGB_IWRAM_SIZE);
        {
            /* where the regions are, to compare pointers between builds */
            unsigned bases[6] = {(unsigned)(unsigned long)agb_io, (unsigned)(unsigned long)agb_pltt,
                                 (unsigned)(unsigned long)agb_vram, (unsigned)(unsigned long)agb_oam,
                                 (unsigned)(unsigned long)agb_ewram, (unsigned)(unsigned long)agb_iwram};

            agb_host_debug_dump("bases", bases, sizeof(bases));
        }
    }

    agb_bridge_pump(); /* script bridge: requests run here, between two frames */
    agb_host_present(sFrame);
    sFrames++;
    agb_host_wait_frame();
    AGB_IO16(0x130) = (u16)(~agb_host_keys() & 0x3FF);

    /* The game resumes inside VBlank, as after the real VBlankIntrWait. */
    sBeam = 160;
    set_beam(160);
    if (force_irqs) AGB_IO16(0x208) = ime;
    sInFrame = 0;
}
