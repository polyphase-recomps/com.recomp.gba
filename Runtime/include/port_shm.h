/*
 * Shared memory between the game process (32-bit native exe) and the Polyphase addon
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

/* script bridge (agb_bridge.h) in child-process mode */
#define PORT_SHM_BRIDGE_VARS 16384
#define PORT_SHM_BRIDGE_REQS 4096
#define PORT_SHM_BRIDGE_VALUES 32768
#define PORT_SHM_BRIDGE_QUEUE 16
#define PORT_SHM_BRIDGE_RESULTS 64
#define PORT_SHM_BRIDGE_NAME 56

typedef struct PortShmBridgeCall
{
    unsigned int id;
    int nargs;
    int args[8];
    char name[PORT_SHM_BRIDGE_NAME];
} PortShmBridgeCall;

typedef struct PortShmBridgeResult
{
    unsigned int id;
    int result;
} PortShmBridgeResult;

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

    /* Script bridge. Texts and values are written by the game under a sequence number
     * that is odd while it writes (read, copy, re-read the number, retry if it moved).
     * Requests: a ring the addon writes (req_write) and the game consumes (req_read);
     * results: a ring the game writes, the addon keeps its own read position. */
    volatile unsigned int bridge_desc_seq;
    volatile unsigned int bridge_values_seq;
    volatile unsigned int bridge_values_size;
    volatile unsigned int bridge_req_write;
    volatile unsigned int bridge_req_read;
    volatile unsigned int bridge_res_write;
    char bridge_vars[PORT_SHM_BRIDGE_VARS];
    char bridge_reqs[PORT_SHM_BRIDGE_REQS];
    unsigned char bridge_values[PORT_SHM_BRIDGE_VALUES];
    PortShmBridgeCall bridge_req[PORT_SHM_BRIDGE_QUEUE];
    PortShmBridgeResult bridge_res[PORT_SHM_BRIDGE_RESULTS];
} PortShm;

#endif /* PORT_SHM_H */
