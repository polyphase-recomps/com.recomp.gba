/*
 * agb runtime: what a natively compiled GBA game needs from "the hardware".
 *
 * Portable C99. No OS calls, no fixed addresses: the GBA memory regions are plain
 * arrays (defined by the per-game generated agb_memory.s, so that absolute-address
 * symbols of the decomp can be labels inside them), and the decomp's address macros
 * (VRAM, PLTT, OAM, REG_BASE, ...) are redirected to them by compat/pret headers.
 * Little-endian targets only (the game reads little-endian data directly).
 *
 * The game links this runtime and a host backend (agb_host.h): Win32/Polyphase today,
 * any platform that can show a 240x160 picture, play audio and read a pad tomorrow.
 */
#ifndef AGB_H
#define AGB_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- memory (agb_memory.s) ------------------------------------------------------------ */
#define AGB_EWRAM_SIZE 0x40000
#define AGB_IWRAM_SIZE 0x8000
#define AGB_IO_SIZE    0x400
#define AGB_PLTT_SIZE  0x400
#define AGB_VRAM_SIZE  0x18000
#define AGB_OAM_SIZE   0x400
#define AGB_SRAM_SIZE  0x10000

extern unsigned char agb_ewram[];
extern unsigned char agb_iwram[];
extern unsigned char agb_io[];
extern unsigned char agb_pltt[];
extern unsigned char agb_vram[];
extern unsigned char agb_oam[];
extern unsigned char agb_sram[];
/* Low BIOS area the game may read through NULL-ish pointers; zeroed. */
extern unsigned char agb_bios[];

#define AGB_IO16(off) (*(volatile unsigned short *)(agb_io + (off)))
#define AGB_IO32(off) (*(volatile unsigned int *)(agb_io + (off)))

/* Translates a real GBA address (0x02000000...) into the runtime's memory; 0 if unmapped. */
void *agb_translate(unsigned int gba_address);

/* ---- interrupts --------------------------------------------------------------------- */
/* IRQ flag bits as in REG_IE/REG_IF. */
enum
{
    AGB_IRQ_VBLANK = 1 << 0,
    AGB_IRQ_HBLANK = 1 << 1,
    AGB_IRQ_VCOUNT = 1 << 2,
    AGB_IRQ_TIMER0 = 1 << 3,
    AGB_IRQ_TIMER1 = 1 << 4,
    AGB_IRQ_TIMER2 = 1 << 5,
    AGB_IRQ_TIMER3 = 1 << 6,
    AGB_IRQ_SERIAL = 1 << 7,
    AGB_IRQ_DMA0 = 1 << 8,
    AGB_IRQ_KEYPAD = 1 << 12,
    AGB_IRQ_GAMEPAK = 1 << 13,
};

/* The game's IRQ dispatcher (what crt0's handler did): called with one IRQ bit when it
 * is enabled in IE and IME is set. Installed by the per-game glue. */
typedef void (*AgbIrqDispatch)(unsigned int irq_bit);
void agb_set_irq_dispatch(AgbIrqDispatch dispatch);

/* ---- DMA ------------------------------------------------------------------------------- */
/* Writes one DMA channel's SAD/DAD/CNT and acts on it (immediate transfers run now,
 * HBlank/VBlank ones are remembered for the frame). Addresses are native pointers. */
void agb_dma_set(int channel, const void *src, void *dst, unsigned int control);
/* Re-reads the DMA registers after the game poked them directly. */
void agb_dma_sync(int channel);

/* ---- frame ---------------------------------------------------------------------------- */
/* BIOS VBlankIntrWait: finishes the frame (scanlines with HBlank/VCount IRQs and HBlank DMA,
 * VBlank IRQ, present, pacing) and returns at the start of the next one. */
void agb_vblank_intr_wait(void);
/* Number of frames presented. */
unsigned int agb_frame_count(void);
/* The current scanline for REG_VCOUNT reads made through agb_vcount_ptr(). */
volatile unsigned short *agb_vcount_ptr(void);
/* Same for REG_DISPSTAT (the VBlank/HBlank/VCount flags follow the simulated beam). */
volatile unsigned short *agb_dispstat_ptr(void);

/* ---- video ---------------------------------------------------------------------------- */
/* Renders one scanline of the current PPU state into a BGR555 line buffer. */
void agb_ppu_render_line(int line, unsigned short *out240);

/* ---- audio ----------------------------------------------------------------------------- */
/* Hands the m4a mixer's output for one frame to the host (signed 8-bit L and R, `count`
 * samples at `rate` Hz). Called by the native SoundMain. */
void agb_audio_submit_s8(const signed char *left, const signed char *right, int count, int rate);
/* The m4a mixer's DMA buffer: sound-FIFO DMA from inside it is not streamed again. */
void agb_audio_m4a_buffer(const void *buffer, unsigned int size);

/* ---- BIOS ------------------------------------------------------------------------------- */
void agb_cpu_set(const void *src, void *dst, unsigned int control);
void agb_cpu_fast_set(const void *src, void *dst, unsigned int control);
void agb_lz77_uncomp(const void *src, void *dst, int vram);
void agb_rl_uncomp(const void *src, void *dst, int vram);
void agb_huff_uncomp(const void *src, void *dst);
void agb_register_ram_reset(unsigned int flags);
unsigned int agb_sqrt(unsigned int value);
int agb_div(int num, int den);
int agb_mod(int num, int den);
unsigned short agb_arctan2(int x, int y);
/* BgAffineSet / ObjAffineSet with the BIOS's struct layouts. */
void agb_bg_affine_set(const void *src, void *dst, int count);
void agb_obj_affine_set(const void *src, void *dst, int count, int offset);

/* ---- boot ------------------------------------------------------------------------------ */
/* Loads the save, then runs the game's main function (AgbMain). Called by the game glue. */
void agb_boot(void (*game_main)(void));
/* BIOS SoftReset: clears the game's RAM variables and restarts the main function. */
void agb_soft_reset(void);

/* ---- logging ----------------------------------------------------------------------------- */
void agb_log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* AGB_H */
