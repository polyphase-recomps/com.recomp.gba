/*
 * The game as a wasm2c guest (AGB_GUEST=wasm): instantiates the translated module
 * (agb_guest.h / agb_guest_N.c), fills its memory from the game data file and serves
 * its imports - the agb_host.h functions - by turning guest addresses into pointers into
 * the guest memory (agbw.h) and calling the platform host.
 *
 * The host sees the same entry point as with the native guest: agb_game_entry().
 *
 * Game data file (written by tools/wasm_to_c.py; the module's data segments, which
 * would otherwise be compiled into the program as C arrays):
 *   "AGBD", u32 version (1), u32 segment count, then per segment u32 offset, u32 size
 *   and the bytes; all little-endian.
 *
 * With AGBW_PAGED (small consoles, agbw.h) the read-only part of the data stays in the
 * file and is read a page at a time when the game first touches it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agb_guest.h"

#include "agb_host.h"
#include "agbw.h"

/* imports come from the "env" module; nothing to keep per instance */
struct w2c_env
{
    int unused;
};

/* the few WASI functions wasi-libc may reference (stdio, time); see the end */
struct w2c_wasi__snapshot__preview1
{
    int unused;
};

static w2c_agb sGuest;
static struct w2c_env sEnv;
static struct w2c_wasi__snapshot__preview1 sWasi;
static int sInstantiated;

static void agbw_fatal(const char *what)
{
    agb_host_fatal(what);
    abort();
}

/* ---- paged read-only data ------------------------------------------------------------------ */
#if AGBW_PAGED
#define CACHE_FRAMES (AGBW_ROM_CACHE_BYTES >> AGBW_PAGE_SHIFT)
#define MAX_ROM_SEGMENTS 16

uint8_t *agbw_ram;
uint8_t *agbw_rom_pages[AGBW_ROM_PAGES];
static uint8_t *sFrames;                /* CACHE_FRAMES pages */
static int32_t sFrameOwner[CACHE_FRAMES]; /* page in each frame, -1 = free */
static uint8_t sPagePinned[AGBW_ROM_PAGES];
static u32 sNextFrame;
static u32 sPinnedFrames;
static u32 sFaults;
static struct
{
    u32 offset, size, file;
} sRomSeg[MAX_ROM_SEGMENTS];
static int sRomSegCount;

uint8_t *agbw_rom_fault(u32 addr)
{
    u32 page = addr >> AGBW_PAGE_SHIFT, base = page << AGBW_PAGE_SHIFT, frame, tries;
    uint8_t *dst;
    int i;

    /* round robin over the frames, skipping the pinned ones */
    for (tries = 0; tries < CACHE_FRAMES; tries++)
    {
        frame = sNextFrame;
        sNextFrame = (sNextFrame + 1) % CACHE_FRAMES;
        if (sFrameOwner[frame] < 0 || !sPagePinned[sFrameOwner[frame]]) break;
    }
    if (tries == CACHE_FRAMES) agbw_fatal("paged data cache full of pinned pages");
    if (sFrameOwner[frame] >= 0) agbw_rom_pages[sFrameOwner[frame]] = NULL;
    dst = sFrames + (size_t)frame * AGBW_PAGE_BYTES;
    memset(dst, 0, AGBW_PAGE_BYTES);
    for (i = 0; i < sRomSegCount; i++)
    {
        u32 lo = sRomSeg[i].offset > base ? sRomSeg[i].offset : base;
        u32 hi = sRomSeg[i].offset + sRomSeg[i].size;

        if (hi > base + AGBW_PAGE_BYTES) hi = base + AGBW_PAGE_BYTES;
        if (lo >= hi) continue;
        if (agb_host_read_data(sRomSeg[i].file + (lo - sRomSeg[i].offset), dst + (lo - base), hi - lo) != (int)(hi - lo))
            agbw_fatal("game data file truncated");
    }
    sFrameOwner[frame] = (int32_t)page;
    agbw_rom_pages[page] = dst;
    if ((++sFaults & 4095) == 0)
    {
        char line[96];

        snprintf(line, sizeof(line), "paged data: %u page loads (%u KB cache)", (unsigned)sFaults,
                 (unsigned)(AGBW_ROM_CACHE_BYTES >> 10));
        agb_host_log(line);
    }
    return dst;
}

/* host address of guest offset a for an access that stays inside one page (the host side's
 * version of agbw_addr in agbw_mem_ops.h, which only the generated code includes) */
static uint8_t *paged_addr(u32 a)
{
    uint8_t *page;

    if (a >= AGBW_RAM_BASE) return agbw_ram + (a - AGBW_RAM_BASE);
    page = agbw_rom_pages[a >> AGBW_PAGE_SHIFT];
    if (page == NULL) page = agbw_rom_fault(a);
    return page + (a & (AGBW_PAGE_BYTES - 1u));
}

void agbw_rom_pin(u32 addr)
{
    u32 page = addr >> AGBW_PAGE_SHIFT;

    if (sPagePinned[page]) return;
    sPagePinned[page] = 1;
    if (++sPinnedFrames * 2 > CACHE_FRAMES) agbw_fatal("too many paged data pages written to");
}

/* an access that spans two pages: read through a scratch buffer (stores to read-only data
 * that cross a page are dropped) */
uint8_t *agbw_rom_cross(u32 addr, u32 n, int store)
{
    static uint8_t scratch[16];
    u32 i;

    if (!store)
    {
        for (i = 0; i < n && i < sizeof(scratch); i++) scratch[i] = *paged_addr(AGBW_OFFSET(addr + i));
    }
    return scratch;
}

static void paging_start(void)
{
    u32 i;

    if (!sFrames) sFrames = (uint8_t *)malloc((size_t)CACHE_FRAMES * AGBW_PAGE_BYTES);
    if (!sFrames) agbw_fatal("cannot allocate the paged data cache");
    for (i = 0; i < CACHE_FRAMES; i++) sFrameOwner[i] = -1;
    memset(agbw_rom_pages, 0, sizeof(agbw_rom_pages));
    memset(sPagePinned, 0, sizeof(sPagePinned));
    sNextFrame = sPinnedFrames = sFaults = 0;
    sRomSegCount = 0;
}

static void paging_add_segment(u32 offset, u32 size, u32 file)
{
    if (sRomSegCount == MAX_ROM_SEGMENTS) agbw_fatal("too many read-only data segments");
    sRomSeg[sRomSegCount].offset = offset;
    sRomSeg[sRomSegCount].size = size;
    sRomSeg[sRomSegCount].file = file;
    sRomSegCount++;
}
#endif

/* releases the guest (memory, tables) after a run that was stopped */
void agbw_guest_free(void)
{
    if (!sInstantiated) return;
    wasm2c_agb_free(&sGuest);
    wasm_rt_free();
    sInstantiated = 0;
}

void agbw_report_trap(const char *what)
{
    char line[128];

    snprintf(line, sizeof(line), "wasm trap: %s", what);
    agbw_fatal(line);
}

/* host pointer to a guest buffer (frame, audio, save data: all writable guest memory) */
static unsigned char *guest_ptr(u32 addr, u32 len)
{
    u32 offset = AGBW_OFFSET(addr);

    if ((unsigned long long)offset + len > AGBW_ALLOC_BYTES) agbw_fatal("guest buffer runs past the guest memory");
#if AGBW_PAGED
    if (offset < AGBW_RAM_BASE)
    {
        if ((offset & (AGBW_PAGE_BYTES - 1)) + len > AGBW_PAGE_BYTES) agbw_fatal("guest buffer in paged data");
        return paged_addr(offset);
    }
    return agbw_ram + (offset - AGBW_RAM_BASE);
#else
    return sGuest.w2c_memory.data + offset;
#endif
}

static u32 read_le32(const unsigned char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static void load_game_data(void)
{
    unsigned char header[12];
    u32 pos = 12, count, i;

    if (agb_host_read_data(0, header, 12) != 12 || memcmp(header, "AGBD", 4) != 0 || read_le32(header + 4) != 1)
        agbw_fatal("game data file missing or not an AGBD file");
    count = read_le32(header + 8);
    for (i = 0; i < count; i++)
    {
        unsigned char seg[8];
        u32 offset, size;

        if (agb_host_read_data(pos, seg, 8) != 8) agbw_fatal("game data file truncated");
        offset = read_le32(seg);
        size = read_le32(seg + 4);
        pos += 8;
        if ((unsigned long long)offset + size > AGBW_MEM_BYTES) agbw_fatal("game data segment outside the guest memory");
#if AGBW_PAGED
        {
            /* the read-only part stays in the file; the rest is loaded */
            u32 split = offset + size <= AGBW_RAM_BASE ? size : offset >= AGBW_RAM_BASE ? 0 : AGBW_RAM_BASE - offset;

            if (split) paging_add_segment(offset, split, pos);
            if (split < size &&
                agb_host_read_data(pos + split, agbw_ram + (offset + split - AGBW_RAM_BASE), size - split) != (int)(size - split))
                agbw_fatal("game data file truncated");
        }
#else
        if (agb_host_read_data(pos, sGuest.w2c_memory.data + offset, size) != (int)size) agbw_fatal("game data file truncated");
#endif
        pos += size;
    }
}

/* the host's entry point, as with the native guest */
void agb_game_entry(void)
{
    agbw_guest_free();
    wasm_rt_init();
    wasm2c_agb_instantiate(&sGuest, &sEnv, &sWasi);
    sInstantiated = 1;
#if AGBW_PAGED
    paging_start();
#endif
    load_game_data();
    w2c_agb_agb_game_entry(&sGuest);
}

/* ---- imports -------------------------------------------------------------------------- */
#if AGBW_BIG_ENDIAN
static const unsigned short *swapped16(u32 addr, u32 count)
{
    static unsigned short *buf;
    static u32 cap;
    const unsigned char *src = guest_ptr(addr, count * 2);
    u32 i;

    if (count > cap)
    {
        free(buf);
        buf = (unsigned short *)malloc(count * 2);
        cap = count;
    }
    for (i = 0; i < count; i++) buf[i] = (unsigned short)(src[i * 2] | (src[i * 2 + 1] << 8));
    return buf;
}
#define GUEST_U16S(addr, count) swapped16(addr, count)
#else
#define GUEST_U16S(addr, count) ((const unsigned short *)guest_ptr(addr, (count) * 2))
#endif

void w2c_env_agb_host_present(struct w2c_env *env, u32 bgr555)
{
    (void)env;
    agb_host_present(GUEST_U16S(bgr555, 240 * 160));
}

void w2c_env_agb_host_wait_frame(struct w2c_env *env)
{
    (void)env;
    agb_host_wait_frame();
}

u32 w2c_env_agb_host_keys(struct w2c_env *env)
{
    (void)env;
    return agb_host_keys();
}

void w2c_env_agb_host_audio(struct w2c_env *env, u32 stereo, u32 frames)
{
    (void)env;
    agb_host_audio((const short *)GUEST_U16S(stereo, frames * 2), (int)frames);
}

u32 w2c_env_agb_host_load_save(struct w2c_env *env, u32 data, u32 size)
{
    (void)env;
    return (u32)agb_host_load_save(guest_ptr(data, size), (int)size);
}

void w2c_env_agb_host_store_save(struct w2c_env *env, u32 data, u32 size)
{
    (void)env;
    agb_host_store_save(guest_ptr(data, size), (int)size);
}

static const char *guest_str(u32 addr)
{
    static char text[512];
    u32 i;

    for (i = 0; i + 1 < sizeof(text); i++)
    {
        text[i] = (char)*guest_ptr(addr + i, 1);
        if (!text[i]) break;
    }
    text[i] = 0;
    return text;
}

void w2c_env_agb_host_log(struct w2c_env *env, u32 line)
{
    (void)env;
    agb_host_log(guest_str(line));
}

u32 w2c_env_agb_host_debug_frame(struct w2c_env *env)
{
    (void)env;
    return (u32)agb_host_debug_frame();
}

void w2c_env_agb_host_debug_dump(struct w2c_env *env, u32 name, u32 data, u32 size)
{
    (void)env;
    agb_host_debug_dump(guest_str(name), guest_ptr(data, size), size);
}

void w2c_env_agb_host_fatal(struct w2c_env *env, u32 line)
{
    (void)env;
    agb_host_fatal(guest_str(line));
}

/* ---- WASI (wasi-libc internals the game never relies on) ----------------------------------
 * stdout/stderr writes go to the log; files and clocks do not exist. */
#define WASI_EBADF 8

static void store_le32(u32 addr, u32 value)
{
    unsigned char *p = guest_ptr(addr, 4);

    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

u32 w2c_wasi__snapshot__preview1_fd_write(struct w2c_wasi__snapshot__preview1 *wasi, u32 fd, u32 iovs, u32 count,
                                          u32 written)
{
    u32 total = 0, i;

    (void)wasi;
    for (i = 0; i < count; i++)
    {
        const unsigned char *iov = guest_ptr(iovs + i * 8, 8);
        u32 base = read_le32(iov), len = read_le32(iov + 4);

        if ((fd == 1 || fd == 2) && len)
        {
            char line[256];
            u32 n = len < sizeof(line) - 1 ? len : (u32)sizeof(line) - 1;

            memcpy(line, guest_ptr(base, n), n);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
            line[n] = 0;
            if (n) agb_host_log(line);
        }
        total += len;
    }
    store_le32(written, total);
    return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_seek(struct w2c_wasi__snapshot__preview1 *wasi, u32 fd, u64 offset, u32 whence,
                                         u32 result)
{
    (void)wasi; (void)fd; (void)offset; (void)whence; (void)result;
    return WASI_EBADF;
}

u32 w2c_wasi__snapshot__preview1_fd_close(struct w2c_wasi__snapshot__preview1 *wasi, u32 fd)
{
    (void)wasi; (void)fd;
    return 0;
}

u32 w2c_wasi__snapshot__preview1_clock_time_get(struct w2c_wasi__snapshot__preview1 *wasi, u32 id, u64 precision,
                                                u32 result)
{
    (void)wasi; (void)id; (void)precision;
    store_le32(result, 0);
    store_le32(result + 4, 0);
    return 0;
}

#ifdef AGBW_WATCH
void agbw_watch_store(u64 addr, u32 size, u64 value)
{
    static int init, hits;
    static u32 lo, hi;
    u32 a = AGBW_OFFSET(addr);

    if (!init)
    {
        const char *w = getenv("AGBW_WATCH");

        init = 1;
        if (w) sscanf(w, "%x-%x", &lo, &hi);
    }
    if (a + size <= lo || a >= hi || hits >= 20) return;
    hits++;
    {
        char line[128];

        snprintf(line, sizeof(line), "WATCH: store of %u bytes at %08X = %llX", (unsigned)size, a, (unsigned long long)value);
        agb_host_log(line);
    }
    agb_host_debug_backtrace();
}
#endif
