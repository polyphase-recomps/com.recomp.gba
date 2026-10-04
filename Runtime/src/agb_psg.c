/*
 * The four Game Boy sound channels (PSG) of the GBA, synthesised from the IO registers
 * once per frame: two square waves (the first with frequency sweep), the 4-bit wave
 * channel and the noise channel, with length counters and volume envelopes clocked by
 * the 512 Hz frame sequencer.
 *
 * The registers are plain memory, so writes cannot be seen as they happen: a channel is
 * (re)started when its NRx4 register has the restart bit (7) set, which is then cleared,
 * and frequencies/duties are re-read every frame. The channel-on bits of SOUNDCNT_X
 * (NR52) are kept up to date because sound drivers read them back (m4a's CgbSound does).
 */
#include "agb.h"
#include "agb_internal.h"

#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define IO8(o) (agb_io[o])
#define RATE 44100.0

typedef struct Psg
{
    int on;
    int length;         /* remaining length clocks (256 Hz) */
    int volume;         /* current envelope volume 0..15 */
    int envTimer;       /* envelope clocks left before the next step */
    double phase;       /* position in the waveform, in steps */
    /* square 1 sweep */
    int sweepTimer;
    int sweepFreq;
    /* noise */
    u32 lfsr;
} Psg;

static Psg sCh[4];
static double sSeqAcc;
static int sSeqStep;

static const u8 kDuty[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 1, 1, 1},
    {0, 1, 1, 1, 1, 1, 1, 0},
};

/* register offsets per channel: NRx1 (length/duty), NRx2 (envelope), NRx3 (freq lo), NRx4 */
static const int kNr1[4] = {0x62, 0x68, 0x72, 0x78};
static const int kNr2[4] = {0x63, 0x69, 0x73, 0x79};
static const int kNr3[4] = {0x64, 0x6C, 0x74, 0x7C};
static const int kNr4[4] = {0x65, 0x6D, 0x75, 0x7D};

static int reg_freq(int ch)
{
    return IO8(kNr3[ch]) | ((IO8(kNr4[ch]) & 7) << 8);
}

static int dac_on(int ch)
{
    if (ch == 2) return (IO8(0x70) & 0x80) != 0;
    return (IO8(kNr2[ch]) & 0xF8) != 0;
}

static void trigger(int ch)
{
    Psg *c = &sCh[ch];
    u8 env = IO8(kNr2[ch]);

    c->on = dac_on(ch);
    if (ch == 2)
        c->length = 256 - IO8(0x72);
    else
        c->length = 64 - (IO8(kNr1[ch]) & 63);
    c->volume = env >> 4;
    c->envTimer = env & 7;
    c->phase = 0;
    if (ch == 0)
    {
        c->sweepFreq = reg_freq(0);
        c->sweepTimer = (IO8(0x60) >> 4) & 7;
    }
    if (ch == 3) c->lfsr = 0x7FFF;
}

static void clock_length(void)
{
    int ch;

    for (ch = 0; ch < 4; ch++)
    {
        if (sCh[ch].on && (IO8(kNr4[ch]) & 0x40) && sCh[ch].length > 0 && --sCh[ch].length == 0) sCh[ch].on = 0;
    }
}

static void clock_envelope(void)
{
    int ch;

    for (ch = 0; ch < 4; ch++)
    {
        Psg *c = &sCh[ch];
        u8 env = IO8(kNr2[ch]);
        int period = env & 7;

        if (ch == 2 || !c->on || period == 0) continue;
        if (--c->envTimer > 0) continue;
        c->envTimer = period;
        if ((env & 8) && c->volume < 15) c->volume++;
        else if (!(env & 8) && c->volume > 0) c->volume--;
    }
}

static void clock_sweep(void)
{
    Psg *c = &sCh[0];
    u8 sweep = IO8(0x60);
    int period = (sweep >> 4) & 7, shift = sweep & 7;

    if (!c->on || period == 0) return;
    if (--c->sweepTimer > 0) return;
    c->sweepTimer = period;
    {
        int delta = c->sweepFreq >> shift;
        int f = (sweep & 8) ? c->sweepFreq - delta : c->sweepFreq + delta;

        if (f > 2047)
        {
            c->on = 0;
            return;
        }
        if (shift && f >= 0)
        {
            c->sweepFreq = f;
            IO8(0x64) = (u8)f;
            IO8(0x65) = (u8)((IO8(0x65) & ~7) | ((f >> 8) & 7));
        }
    }
}

/* 512 Hz frame sequencer: length 256 Hz, sweep 128 Hz, envelope 64 Hz */
static void sequencer_step(void)
{
    if ((sSeqStep & 1) == 0) clock_length();
    if ((sSeqStep & 3) == 2) clock_sweep();
    if (sSeqStep == 7) clock_envelope();
    sSeqStep = (sSeqStep + 1) & 7;
}

static int sample(int ch, double *phase_step)
{
    Psg *c = &sCh[ch];
    int f = ch == 0 ? c->sweepFreq : reg_freq(ch);

    switch (ch)
    {
    case 0:
    case 1:
    {
        int duty = IO8(kNr1[ch]) >> 6;
        int bit = kDuty[duty][(int)c->phase & 7];

        *phase_step = 8.0 * 131072.0 / (2048 - f) / RATE;
        return bit ? c->volume : -c->volume;
    }
    case 2:
    {
        u8 nr32 = IO8(0x73);
        int index = (int)c->phase & 31;
        u8 b = IO8(0x90 + (index >> 1));
        int s = ((index & 1) ? (b & 15) : (b >> 4)) * 2 - 15;

        *phase_step = 2097152.0 / (2048 - f) / RATE;
        if (nr32 & 0x80) return s * 3 / 4;
        switch ((nr32 >> 5) & 3)
        {
        case 0: return 0;
        case 1: return s;
        case 2: return s / 2;
        default: return s / 4;
        }
    }
    default:
    {
        u8 nr43 = IO8(0x7C);
        int r = nr43 & 7, s = nr43 >> 4;
        double hz = 524288.0 / (r ? r : 0.5) / (double)(2 << s);

        *phase_step = hz / RATE;
        return (c->lfsr & 1) ? -c->volume : c->volume;
    }
    }
}

static void advance(int ch, double step)
{
    Psg *c = &sCh[ch];

    c->phase += step;
    if (ch == 3)
    {
        /* one LFSR shift per whole step */
        int shifts = (int)c->phase;

        c->phase -= shifts;
        if (shifts > 64) shifts = 64;
        while (shifts--)
        {
            u32 bit = (c->lfsr ^ (c->lfsr >> 1)) & 1;

            c->lfsr = (c->lfsr >> 1) | (bit << 14);
            if (IO8(0x7C) & 8) c->lfsr = (c->lfsr & ~0x40u) | (bit << 6);
        }
    }
    else if (c->phase >= 64.0)
    {
        c->phase -= 64.0;
    }
}

/* Adds `count` stereo samples of PSG output at 44.1 kHz to mix (interleaved L/R). */
void agb_psg_render(int *mix, int count)
{
    u8 nr50 = IO8(0x80), nr51 = IO8(0x81);
    int volR = (nr50 & 7) + 1, volL = ((nr50 >> 4) & 7) + 1;
    static const int kRatio[4] = {8, 16, 32, 32}; /* 25%, 50%, 100% (x32 = full) */
    int ratio = kRatio[IO8(0x82) & 3];
    int ch, i;

    if (!(IO8(0x84) & 0x80))
    {
        memset(sCh, 0, sizeof(sCh));
        IO8(0x84) &= 0x80;
        return;
    }
    for (ch = 0; ch < 4; ch++)
    {
        if (IO8(kNr4[ch]) & 0x80)
        {
            IO8(kNr4[ch]) &= 0x7F;
            trigger(ch);
        }
        if (!dac_on(ch)) sCh[ch].on = 0;
    }

    for (i = 0; i < count; i++)
    {
        int left = 0, right = 0;

        sSeqAcc += 512.0 / RATE;
        if (sSeqAcc >= 1.0)
        {
            sSeqAcc -= 1.0;
            sequencer_step();
        }
        for (ch = 0; ch < 4; ch++)
        {
            double step;
            int s;

            if (!sCh[ch].on) continue;
            s = sample(ch, &step);
            advance(ch, step);
            if (nr51 & (1 << ch)) right += s;
            if (nr51 & (0x10 << ch)) left += s;
        }
        mix[i * 2] += left * volL * ratio;
        mix[i * 2 + 1] += right * volR * ratio;
    }

    IO8(0x84) = (u8)((IO8(0x84) & 0xF0) | (sCh[0].on ? 1 : 0) | (sCh[1].on ? 2 : 0) | (sCh[2].on ? 4 : 0) |
                     (sCh[3].on ? 8 : 0));
}
