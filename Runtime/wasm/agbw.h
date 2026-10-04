/*
 * Guest memory of the wasm2c build (AGB_GUEST=wasm).
 *
 * The game, its data and the portable runtime core are one wasm32 module, so every
 * pointer the game stores is 32-bit whatever the host's word size. Its linear memory is
 * one buffer of AGBW_MEM_BYTES: what the module actually uses (data, BSS, stack, a little
 * heap), measured by tools/wasm_to_c.py and written to agbw_config.h, so small-memory
 * consoles only pay for the game. Every address goes through AGBW_OFFSET, so no access
 * can leave the buffer and no bounds checks are needed: a power-of-two size masks, any
 * other size sends out-of-range addresses to the zeroed low page (like NULL). Address 0
 * and the first KB are inside the buffer and zero, like the GBA's BIOS area.
 *
 * The buffer is little-endian on every host; big-endian hosts (GameCube, Wii) swap on
 * load/store (agbw_mem_ops.h), and the host imports swap pixels and samples.
 */
#ifndef AGBW_H
#define AGBW_H

#include "agbw_config.h"

#if (AGBW_MEM_BYTES & (AGBW_MEM_BYTES - 1u)) == 0
#define AGBW_OFFSET(a) ((u32)(a) & (AGBW_MEM_BYTES - 1u))
#else
#define AGBW_OFFSET(a) ((u32)(a) < AGBW_MEM_BYTES ? (u32)(a) : ((u32)(a) & 0x3FFu))
#endif
/* room for an access of up to 16 bytes at the last address */
#define AGBW_ALLOC_BYTES (AGBW_MEM_BYTES + 16u)

/*
 * Paged read-only data, for consoles that cannot hold the whole game: everything below
 * AGBW_RAM_BASE (the game's assets, tables and strings; agbw_config.h) stays in the data
 * file and is read in AGBW_PAGE_BYTES pages into a cache of AGBW_ROM_CACHE_BYTES, round
 * robin; only the writable data, BSS and stack above AGBW_RAM_BASE are allocated. Pages
 * the game writes to (rare: NULL-pointer writes land in the first page) stay pinned so
 * the writes are kept. Define AGBW_ROM_CACHE_BYTES (0 = everything resident) to override.
 * KH:CoM's working set is small: a 2 MB cache reloads fewer than 4096 pages in 40k frames
 * of play (and stays bit-identical down to 256 KB).
 */
#ifndef AGBW_ROM_CACHE_BYTES
#if defined(_arch_dreamcast) /* 16 MB: the translated code alone is ~6.5 MB */
#define AGBW_ROM_CACHE_BYTES (2u << 20)
#elif defined(GEKKO) && !defined(HW_RVL) /* 24 MB */
#define AGBW_ROM_CACHE_BYTES (4u << 20)
#elif defined(__3DS__)
#define AGBW_ROM_CACHE_BYTES (16u << 20)
#else
#define AGBW_ROM_CACHE_BYTES 0u
#endif
#endif
#define AGBW_PAGE_BYTES (1u << AGBW_PAGE_SHIFT)
#if AGBW_ROM_CACHE_BYTES && AGBW_RAM_BASE && AGBW_ROM_CACHE_BYTES < AGBW_RAM_BASE
#define AGBW_PAGED 1
#define AGBW_ROM_PAGES (AGBW_RAM_BASE >> AGBW_PAGE_SHIFT)
#else
#define AGBW_PAGED 0
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define AGBW_BIG_ENDIAN 1
#else
#define AGBW_BIG_ENDIAN 0
#endif

/* libogc/newlib has no signals: wasm-rt.h's sigsetjmp/siglongjmp (used outside
 * Windows) are plain setjmp/longjmp there */
#if defined(GEKKO) || defined(__3DS__)
#include <setjmp.h>
#define sigsetjmp(b, s) setjmp(b)
#define siglongjmp(b, v) longjmp(b, v)
#endif

#endif /* AGBW_H */
