/* Runtime-internal entry points shared between the runtime's source files. */
#ifndef AGB_INTERNAL_H
#define AGB_INTERNAL_H

/* Raises an interrupt: sets IF and calls the game's dispatcher if IME/IE allow it. */
void agb_raise_irq(unsigned int bit);

/* End of frame: plays the Direct Sound FIFO streams (DMA1/DMA2 in sound-FIFO mode, paced
 * by timer 0/1), adds what the m4a mixer submitted, and hands 44.1 kHz audio to the host. */
void agb_audio_frame(void);

/* Adds `count` stereo samples (44.1 kHz) of the PSG channels to an interleaved mix. */
void agb_psg_render(int *mix, int count);

/* Save memory: stores SRAM through the host when it changed (every 30 calls, or now). */
void agb_save_poll(int force);

/* Forgets the frame loop's state (soft reset from inside an interrupt handler). */
void agb_frame_reset(void);

#endif
