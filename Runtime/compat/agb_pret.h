/*
 * agb runtime prelude for pret-style GBA decomps, force-included (-include) before every
 * game source. It pulls in the decomp's own gba/ headers first (their include guards keep
 * later #includes from undoing anything) and then points the hardware at the runtime:
 *  - memory regions and IO registers are the agb_* arrays, not fixed addresses;
 *  - REG_VCOUNT / REG_DISPSTAT reads follow a simulated beam so polling loops end;
 *  - DmaSet goes through agb_dma_set;
 *  - EWRAM/IWRAM section attributes are dropped (ordinary zeroed globals);
 *  - struct members are aligned as agbcc does (64-bit types on 4 bytes).
 * A game can add its own prelude after this one for game headers (see com.recomp.khcom).
 */
#ifndef AGB_PRET_H
#define AGB_PRET_H

/* agbcc (old ARM ABI) aligns 64-bit members to 4 bytes; the x86 targets use 8. */
#pragma pack(4)

#include "agb.h"
#include "gba/defines.h"
#include "gba/io_reg.h"
#include "gba/macro.h"
#include "macros.h"

#undef INTR_VECTOR
#define INTR_VECTOR (*(void **)(agb_iwram + 0x7FFC))
#undef EWRAM_START
#define EWRAM_START ((unsigned int)agb_ewram)
#undef IWRAM_START
#define IWRAM_START ((unsigned int)agb_iwram)
#undef PLTT
#define PLTT ((unsigned int)agb_pltt)
#undef VRAM
#define VRAM ((unsigned int)agb_vram)
#undef OAM
#define OAM ((unsigned int)agb_oam)

#undef REG_BASE
#define REG_BASE ((unsigned int)agb_io)
#undef REG_VCOUNT
#define REG_VCOUNT (*agb_vcount_ptr())
#undef REG_DISPSTAT
#define REG_DISPSTAT (*agb_dispstat_ptr())

#undef DmaSet
#define DmaSet(dmaNum, src, dest, control) \
    agb_dma_set(dmaNum, (const void *)(unsigned long)(src), (void *)(unsigned long)(dest), (unsigned int)(control))

#undef EWRAM_COMMON
#define EWRAM_COMMON(align) __attribute__((aligned(align))) = {0}
#undef IWRAM_DATA
#define IWRAM_DATA(align) __attribute__((aligned(align))) = {0}
#undef IWRAM_COMMON
#define IWRAM_COMMON(align) __attribute__((aligned(align))) = {0}

/* Game RAM variables in their own group, so a soft reset can zero them (agb_boot.c):
 * a COFF grouped section, or on wasm a segment wasm-ld brackets with __start_/__stop_. */
#ifdef __wasm__
#pragma clang section bss = "agb_game_bss"
#else
#pragma clang section bss = ".bss$agbg"
#endif

/* Symbol prefix for __asm__("name") labels (C symbols carry one on 32-bit Windows). */
#ifdef _WIN32
#define AGB_ASM_LABEL(name) "_" name
#else
#define AGB_ASM_LABEL(name) name
#endif

#endif
