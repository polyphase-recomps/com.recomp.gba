/*
 * Audio: Direct Sound A/B rebuilt per frame, mixed to 44.1 kHz stereo for the host.
 *
 * Two kinds of sources:
 *  - the native m4a mixer hands over each frame's freshly mixed samples
 *    (agb_audio_submit_s8), and tells us its DMA buffer so that its FIFO DMA is not
 *    played a second time (agb_audio_m4a_buffer);
 *  - any other sound-FIFO DMA (e.g. a game's own PCM streamer) is played by following the
 *    DMA source pointer at the rate of the timer that drives that FIFO.
 * The PSG channels (1-4) are synthesised by agb_psg.c into the same mix.
 */
#include "agb.h"
#include "agb_host.h"
#include "agb_internal.h"

#include <string.h>

typedef signed char s8;
typedef unsigned short u16;
typedef unsigned int u32;

#define GBA_CLOCK 16777216.0
#define GBA_FPS 59.7275
#define OUT_RATE 44100
#define MAX_OUT 1024

static int sMix[MAX_OUT * 2];
static int sOutCount;
static double sOutAcc;

static const unsigned char *sM4aBuffer;
static u32 sM4aBufferSize;

typedef struct FifoStream
{
    u32 lastSad;
    u32 lastCnt;
    const s8 *ptr;
    double acc;
} FifoStream;

static FifoStream sFifo[2]; /* DMA1, DMA2 */

static void begin_frame(void)
{
    int n;

    if (sOutCount) return;
    sOutAcc += OUT_RATE / GBA_FPS;
    n = (int)sOutAcc;
    if (n > MAX_OUT) n = MAX_OUT;
    sOutAcc -= n;
    sOutCount = n;
    memset(sMix, 0, sizeof(int) * 2 * n);
}

/* Linear resample of `count` samples onto the frame's output, into one or both sides. */
static void add_resampled(const s8 *src, int count, int stride, int gain, int toLeft, int toRight)
{
    int i;

    if (count <= 0) return;
    begin_frame();
    for (i = 0; i < sOutCount; i++)
    {
        u32 pos16 = (u32)(((unsigned long long)i * (u32)count << 16) / (u32)sOutCount);
        int idx = pos16 >> 16, frac = pos16 & 0xFFFF;
        int a = src[idx * stride];
        int b = idx + 1 < count ? src[(idx + 1) * stride] : a;
        int v = (a * (0x10000 - frac) + b * frac) >> 16;

        v *= gain;
        if (toLeft) sMix[i * 2] += v;
        if (toRight) sMix[i * 2 + 1] += v;
    }
}

void agb_audio_submit_s8(const signed char *left, const signed char *right, int count, int rate)
{
    (void)rate;
    add_resampled(left, count, 1, 256, 1, 0);
    add_resampled(right, count, 1, 256, 0, 1);
}

void agb_audio_m4a_buffer(const void *buffer, unsigned int size)
{
    sM4aBuffer = (const unsigned char *)buffer;
    sM4aBufferSize = size;
}

static double timer_rate(int timer)
{
    static const int kPrescale[4] = {1, 64, 256, 1024};
    u16 reload = AGB_IO16(0x100 + timer * 4), ctrl = AGB_IO16(0x102 + timer * 4);

    if (!(ctrl & 0x80)) return 0;
    return GBA_CLOCK / kPrescale[ctrl & 3] / (double)(0x10000 - reload);
}

static void play_fifo(int which)
{
    FifoStream *f = &sFifo[which];
    int ch = which + 1;
    u32 sad = AGB_IO32(0xB0 + ch * 12), dad = AGB_IO32(0xB4 + ch * 12), cnt = AGB_IO32(0xB8 + ch * 12);
    u32 fifoA = (u32)(unsigned long)(agb_io + 0xA0), fifoB = (u32)(unsigned long)(agb_io + 0xA4);
    u16 soundcntH = AGB_IO16(0x82);
    int isA, enabled, timer, left, right, gain, n;
    double rate;

    enabled = (cnt & 0x80000000u) && ((cnt >> 28) & 3) == 3 && (dad == fifoA || dad == fifoB);
    if (!enabled)
    {
        f->lastCnt = cnt;
        f->ptr = 0;
        return;
    }
    if (sad != f->lastSad || !(f->lastCnt & 0x80000000u) || !f->ptr)
    {
        f->ptr = (const s8 *)(unsigned long)sad;
        f->acc = 0;
    }
    f->lastSad = sad;
    f->lastCnt = cnt;

    if (sM4aBuffer && (const unsigned char *)f->ptr >= sM4aBuffer &&
        (const unsigned char *)f->ptr < sM4aBuffer + sM4aBufferSize)
        return; /* m4a submits its mix directly */
    if (!(AGB_IO16(0x84) & 0x80)) return; /* master enable */

    isA = dad == fifoA;
    timer = (soundcntH >> (isA ? 10 : 14)) & 1;
    rate = timer_rate(timer);
    if (rate <= 0) return;
    right = (soundcntH >> (isA ? 8 : 12)) & 1;
    left = (soundcntH >> (isA ? 9 : 13)) & 1;
    gain = (soundcntH >> (isA ? 2 : 3)) & 1 ? 256 : 128;

    f->acc += rate / GBA_FPS;
    n = (int)f->acc;
    f->acc -= n;
    add_resampled(f->ptr, n, 1, gain, left, right);
    f->ptr += n;
}

void agb_audio_frame(void)
{
    static short out[MAX_OUT * 2];
    int i;

    static int sPrevIn[2], sPrevOut[2];

    play_fifo(0);
    play_fifo(1);
    begin_frame();
    agb_psg_render(sMix, sOutCount);
    for (i = 0; i < sOutCount * 2; i++)
    {
        /* DC blocker (the console's output is AC-coupled): y = x - x1 + 0.995 y1 */
        int c = i & 1;
        int v = sMix[i] - sPrevIn[c] + (sPrevOut[c] * 4076 >> 12);

        sPrevIn[c] = sMix[i];
        sPrevOut[c] = v;

        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        out[i] = (short)v;
    }
    agb_host_audio(out, sOutCount);
    sOutCount = 0;
}
