/*
 * What the agb runtime needs from the platform. One implementation per target
 * (host/win32 for Windows and the Polyphase child process).
 */
#ifndef AGB_HOST_H
#define AGB_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

/* Shows a finished 240x160 frame (BGR555, as GBA palette entries). */
void agb_host_present(const unsigned short *bgr555);
/* Blocks until the next 59.73 Hz frame (returns at once in headless runs). */
void agb_host_wait_frame(void);
/* GBA KEYINPUT layout, pressed = 1 (A=0x1 B=0x2 Select=0x4 Start=0x8 Right Left Up Down R L). */
unsigned int agb_host_keys(void);
/* Interleaved stereo 16-bit at 44100 Hz. */
void agb_host_audio(const short *stereo, int frames);
/* Save memory: loaded at boot, written back when the game has changed it. */
int agb_host_load_save(unsigned char *data, int size);
void agb_host_store_save(const unsigned char *data, int size);
/* Game data file of the wasm guest build (AGB_GUEST=wasm, <game>.agbdata): reads `size`
 * bytes at `offset`, returns the number read. Not used by the native guest. */
int agb_host_read_data(unsigned offset, void *dst, unsigned size);
void agb_host_log(const char *line);
/* Debugging: the frame whose hardware state should be dumped (-1: none), and the dump of
 * one memory region of it (the host writes it to a file). */
int agb_host_debug_frame(void);
void agb_host_debug_dump(const char *name, const void *data, unsigned size);
/* Debugging: logs the calling thread's stack, if the host can. */
void agb_host_debug_backtrace(void);
void agb_host_fatal(const char *line);

#ifdef __cplusplus
}
#endif

#endif /* AGB_HOST_H */
