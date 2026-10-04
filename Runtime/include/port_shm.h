/*
 * Shared memory between dw.exe (32-bit game process) and the Polyphase addon
 * (64-bit, inside the editor). Fixed-size types only: both sides must agree on
 * the layout regardless of pointer size.
 */
#ifndef PORT_SHM_H
#define PORT_SHM_H

#define PORT_SHM_MAX_W 640
#define PORT_SHM_MAX_H 512
#define PORT_SHM_AUDIO_FRAMES 16384

#define PORT_SHM_STATUS_STARTING 0
#define PORT_SHM_STATUS_RUNNING 1
#define PORT_SHM_STATUS_EXITED 2
#define PORT_SHM_STATUS_CRASHED 3

#define PORT_SHM_CMD_NONE 0
#define PORT_SHM_CMD_QUIT 1

typedef struct PortShm
{
    volatile unsigned int status;       /* written by the game */
    volatile unsigned int command;      /* written by the addon */
    volatile unsigned int pad;          /* PsyQ PadRead() bits, written by the addon */
    volatile unsigned int frame_serial; /* incremented per presented frame */
    volatile unsigned int frame_index;  /* which of the two frame slots is newest */
    volatile unsigned int width[2];
    volatile unsigned int height[2];
    volatile unsigned int audio_write;  /* stereo frames written (game) */
    volatile unsigned int audio_read;   /* stereo frames consumed (addon) */
    unsigned int reserved[6];
    unsigned char frames[2][PORT_SHM_MAX_W * PORT_SHM_MAX_H * 4]; /* RGBA8 */
    short audio[PORT_SHM_AUDIO_FRAMES * 2]; /* 44100 Hz stereo ring */
} PortShm;

#endif /* PORT_SHM_H */
