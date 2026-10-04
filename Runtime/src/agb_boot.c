/*
 * Boot, soft reset and save memory.
 *
 * The game's RAM variables are ordinary zeroed globals natively; the runtime prelude puts
 * them in their own group (.bss$agbg on COFF, segment agb_game_bss on wasm) so that a
 * BIOS SoftReset can zero them again, as the console's RAM clear does, before restarting
 * the game's main function.
 */
#include "agb.h"
#include "agb_host.h"
#include "agb_internal.h"

#include <string.h>

#ifdef __wasm__
/* wasm-ld brackets the segment; setjmp/longjmp need wasm exception handling. */
#include <setjmp.h>
extern char __start_agb_game_bss[] __attribute__((weak));
extern char __stop_agb_game_bss[] __attribute__((weak));
#define GAME_BSS_BEGIN __start_agb_game_bss
#define GAME_BSS_END __stop_agb_game_bss
static jmp_buf sResetBuf;
#define RESET_SAVE() setjmp(sResetBuf)
#define RESET_JUMP() longjmp(sResetBuf, 1)
#else
/* Markers around the game's .bss$agbg group (the linker sorts grouped sections by name). */
__attribute__((section(".bss$agba"))) static char sGameBssBegin[4];
__attribute__((section(".bss$agbz"))) static char sGameBssEnd[4];
#define GAME_BSS_BEGIN sGameBssBegin
#define GAME_BSS_END sGameBssEnd
static void *sResetBuf[5];
#define RESET_SAVE() __builtin_setjmp(sResetBuf)
#define RESET_JUMP() __builtin_longjmp(sResetBuf, 1)
#endif

static void (*sMain)(void);
static unsigned char sSaveShadow[AGB_SRAM_SIZE];
static unsigned sSaveCheck;

void agb_boot(void (*game_main)(void))
{
    int got;

    sMain = game_main;
    memset(agb_sram, 0xFF, AGB_SRAM_SIZE); /* erased SRAM / flash reads 0xFF */
    got = agb_host_load_save(agb_sram, AGB_SRAM_SIZE);
    (void)got;
    memcpy(sSaveShadow, agb_sram, AGB_SRAM_SIZE);
    AGB_IO16(0x130) = 0x3FF; /* no keys */
    AGB_IO16(0x88) = 0x200; /* SOUNDBIAS */
    if (RESET_SAVE())
    {
        agb_log("soft reset");
    }
    sMain();
}

void agb_soft_reset(void)
{
    char *begin = GAME_BSS_BEGIN, *end = GAME_BSS_END;

    agb_save_poll(1);
    if (begin && end > begin) memset(begin, 0, (size_t)(end - begin));
    agb_frame_reset();
    agb_register_ram_reset(0xFF);
    RESET_JUMP();
}

/* Writes the save file when SRAM changed (checked every half second, or now if forced). */
void agb_save_poll(int force)
{
    if (!force && ++sSaveCheck < 30) return;
    sSaveCheck = 0;
    if (memcmp(sSaveShadow, agb_sram, AGB_SRAM_SIZE) == 0) return;
    memcpy(sSaveShadow, agb_sram, AGB_SRAM_SIZE);
    agb_host_store_save(agb_sram, AGB_SRAM_SIZE);
}
