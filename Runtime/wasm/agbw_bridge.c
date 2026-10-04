/*
 * In-process host for the wasm2c guest inside the engine (GbaPlayer): implements
 * agb_host.h by forwarding to the AgbHostApi callbacks the player passes to
 * agbw_bridge_run(), on the game's thread. When the player wants the game to stop
 * (wait_frame returns nonzero) or the game fails, the bridge unwinds out of the guest
 * with longjmp; the next run re-instantiates the module from scratch.
 *
 * Published into the game package's Source/Guest with the generated guest; not part of
 * the standalone test host (host/win32 implements agb_host.h there).
 */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "AgbGuestApi.h"
#include "agb_host.h"

void agb_game_entry(void);    /* agbw_guest.c */
void agbw_guest_free(void);   /* agbw_guest.c */

static const AgbHostApi *sHost;
static jmp_buf sExit;
static int sExitCode;

int agbw_bridge_run(const AgbHostApi *host)
{
    sHost = host;
    sExitCode = 0;
    if (setjmp(sExit) == 0)
    {
        agb_game_entry();
        sHost->log(sHost->user, "the game returned from AgbMain");
    }
    agbw_guest_free();
    sHost = NULL;
    return sExitCode;
}

static void leave(int code)
{
    sExitCode = code;
    longjmp(sExit, 1);
}

void agb_host_present(const unsigned short *bgr555)
{
    sHost->present(sHost->user, bgr555);
}

void agb_host_wait_frame(void)
{
    if (sHost->wait_frame(sHost->user)) leave(0);
}

unsigned int agb_host_keys(void)
{
    return sHost->keys(sHost->user);
}

void agb_host_audio(const short *stereo, int frames)
{
    sHost->audio(sHost->user, (const int16_t *)stereo, frames);
}

int agb_host_load_save(unsigned char *data, int size)
{
    return sHost->load_save(sHost->user, data, size);
}

void agb_host_store_save(const unsigned char *data, int size)
{
    sHost->store_save(sHost->user, data, size);
}

int agb_host_read_data(unsigned offset, void *dst, unsigned size)
{
    return sHost->read_data(sHost->user, offset, dst, size);
}

void agb_host_log(const char *line)
{
    sHost->log(sHost->user, line);
}

void agb_host_fatal(const char *line)
{
    char text[300];

    snprintf(text, sizeof(text), "FATAL: %s", line);
    sHost->log(sHost->user, text);
    leave(1);
}

int agb_host_debug_frame(void)
{
    return -1;
}

void agb_host_debug_dump(const char *name, const void *data, unsigned size)
{
    (void)name; (void)data; (void)size;
}

void agb_host_debug_backtrace(void)
{
}
