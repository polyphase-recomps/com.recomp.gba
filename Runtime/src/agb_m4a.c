/*
 * The m4a (MusicPlayer2000 / "Sappy") engine's assembly half (m4a_1.s) in C: the
 * Direct Sound mixer (SoundMain/SoundMainRAM), the sequencer tick (MPlayMain), note
 * allocation (ply_note) and the ply_* track commands.
 *
 * Written against the standard pret m4a.h struct and field names and compiled with the
 * game's own flags and headers, so any pret GBA decomp using m4a can link it in place of
 * m4a_1.s. Mixing is bit-exact with the original (8-bit wrapping adds, 23-bit position
 * fraction, the same envelope steps and reverb), and the mixed samples are handed to the
 * host through agb_audio_submit_s8 instead of being streamed by FIFO DMA.
 */
#include "m4a.h"
#include "agb.h"

/* m4a_1.s data symbols that m4a2.c refers to (SoundMainRAM is copied to IWRAM there). */
char SoundMainRAM[0x400];

#define CHAN_ON (SOUND_CHANNEL_SF_START | SOUND_CHANNEL_SF_STOP | SOUND_CHANNEL_SF_IEC | SOUND_CHANNEL_SF_ENV)

u32 umul3232H32(u32 a, u32 b)
{
    return (u32)(((unsigned long long)a * b) >> 32);
}

void SoundMainBTM(void *work)
{
    u32 *p = (u32 *)work;
    int i;

    for (i = 0; i < 16; i++) p[i] = 0;
}

void RealClearChain(void *work)
{
    SoundChannel *chan = (SoundChannel *)work;
    MusicPlayerTrack *track = chan->track;
    SoundChannel *next, *prev;

    if (!track) return;
    next = (SoundChannel *)chan->nextChannelPointer;
    prev = (SoundChannel *)chan->prevChannelPointer;
    if (prev) prev->nextChannelPointer = next;
    else track->chan = next;
    if (next) next->prevChannelPointer = prev;
    chan->track = 0;
}

/* ---- mixer ---------------------------------------------------------------------------------- */

static void mix_channel(SoundInfo *si, SoundChannel *chan, s8 *buf, int spv)
{
    WaveData *wav = chan->wav;
    u32 status = chan->statusFlags;
    u32 env;
    u32 vol, envR, envL;
    s8 *loopStart = 0;
    s32 loopLen = 0;
    s32 count;
    s8 *cur;
    int i;

    if (!(status & CHAN_ON)) return;

    if (status & SOUND_CHANNEL_SF_START)
    {
        if (status & SOUND_CHANNEL_SF_STOP)
        {
            chan->statusFlags = 0;
            return;
        }
        status = SOUND_CHANNEL_SF_ENV_ATTACK;
        chan->currentPointer = wav->data;
        chan->count = wav->size;
        chan->envelopeVolume = 0;
        chan->fw = 0;
        if (wav->status & 0xC000) status |= 0x10;
        env = 0;
        goto attack;
    }

    env = chan->envelopeVolume;
    if (status & SOUND_CHANNEL_SF_IEC)
    {
        u8 len = chan->pseudoEchoLength;

        chan->pseudoEchoLength = (u8)(len - 1);
        if (len <= 1)
        {
            chan->statusFlags = 0;
            return;
        }
    }
    else if (status & SOUND_CHANNEL_SF_STOP)
    {
        env = (env * chan->release) >> 8;
        if (env <= chan->pseudoEchoVolume)
        {
        echo:
            env = chan->pseudoEchoVolume;
            if (env == 0)
            {
                chan->statusFlags = 0;
                return;
            }
            status |= SOUND_CHANNEL_SF_IEC;
        }
    }
    else if ((status & SOUND_CHANNEL_SF_ENV) == SOUND_CHANNEL_SF_ENV_DECAY)
    {
        env = (env * chan->decay) >> 8;
        if (env <= chan->sustain)
        {
            env = chan->sustain;
            if (env == 0) goto echo;
            status--;
        }
    }
    else if ((status & SOUND_CHANNEL_SF_ENV) == SOUND_CHANNEL_SF_ENV_ATTACK)
    {
    attack:
        env += chan->attack;
        if (env >= 0xFF)
        {
            env = 0xFF;
            status--;
        }
    }

    chan->statusFlags = (u8)status;
    chan->envelopeVolume = (u8)env;
    vol = ((si->masterVolume + 1) * env) >> 4;
    envR = (chan->rightVolume * vol) >> 8;
    envL = (chan->leftVolume * vol) >> 8;
    chan->envelopeVolumeRight = (u8)envR;
    chan->envelopeVolumeLeft = (u8)envL;
    if (status & 0x10)
    {
        loopStart = wav->data + wav->loopStart;
        loopLen = (s32)(wav->size - wav->loopStart);
    }

    count = (s32)chan->count;
    cur = chan->currentPointer;

#define MIX(i, v)                                                                       \
    do                                                                                  \
    {                                                                                   \
        buf[i] = (s8)(u8)((u8)buf[i] + (u8)(((s32)envR * (v)) >> 8));                   \
        buf[(i) + PCM_DMA_BUF_SIZE] =                                                   \
            (s8)(u8)((u8)buf[(i) + PCM_DMA_BUF_SIZE] + (u8)(((s32)envL * (v)) >> 8));   \
    } while (0)

    if (chan->type & TONEDATA_TYPE_FIX)
    {
        for (i = 0; i < spv; i++)
        {
            s32 v = *cur++;

            MIX(i, v);
            if (--count == 0)
            {
                if (loopLen)
                {
                    cur = loopStart;
                    count = loopLen;
                }
                else
                {
                    chan->statusFlags = 0;
                    return;
                }
            }
        }
        chan->count = (u32)count;
        chan->currentPointer = cur;
    }
    else
    {
        u32 fw = chan->fw;
        u32 step = chan->frequency * (u32)si->divFreq;
        s32 s0 = cur[0];
        s8 *p = cur + 1;
        s32 d = *p - s0;

        for (i = 0; i < spv; i++)
        {
            s32 v = s0 + (((s32)fw * d) >> 23);
            u32 adv;

            MIX(i, v);
            fw += step;
            adv = fw >> 23;
            if (adv == 0) continue;
            fw &= 0x7FFFFF;
            count -= (s32)adv;
            if (count <= 0)
            {
                s32 over;

                if (!loopLen)
                {
                    chan->statusFlags = 0;
                    return;
                }
                over = -count;
                for (;;)
                {
                    count += loopLen;
                    if (count > 0) break;
                    over -= loopLen;
                }
                p = loopStart + over;
                s0 = *p;
            }
            else if (adv - 1 == 0)
            {
                s0 += d;
            }
            else
            {
                p += adv - 1;
                s0 = *p;
            }
            p++;
            d = *p - s0;
        }
        chan->fw = fw;
        chan->count = (u32)count;
        chan->currentPointer = p - 1;
    }
#undef MIX
}

void SoundMain(void)
{
    SoundInfo *si = gSoundInfoPtr;
    s32 spv;
    s8 *buf;
    u32 counter;
    int i;

    if (!si || si->ident != ID_NUMBER) return;
    si->ident++;

    if (si->MPlayMainHead) si->MPlayMainHead(si->musicPlayerHead);
    si->CgbSound();

    spv = si->pcmSamplesPerVBlank;
    buf = si->pcmBuffer;
    counter = si->pcmDmaCounter;
    if (counter > 1) buf += (si->pcmDmaPeriod - (counter - 1)) * spv;

    if (si->reverb)
    {
        const s8 *prev = counter == 2 ? si->pcmBuffer : buf + spv;
        u32 reverb = si->reverb;

        for (i = 0; i < spv; i++)
        {
            s32 sum = buf[i + PCM_DMA_BUF_SIZE] + buf[i] + prev[i + PCM_DMA_BUF_SIZE] + prev[i];
            s32 v = (s32)(sum * (s32)reverb) >> 9;

            if (v & 0x80) v++;
            buf[i + PCM_DMA_BUF_SIZE] = (s8)v;
            buf[i] = (s8)v;
        }
    }
    else
    {
        for (i = 0; i < spv; i++)
        {
            buf[i] = 0;
            buf[i + PCM_DMA_BUF_SIZE] = 0;
        }
    }

    for (i = 0; i < si->maxChans; i++) mix_channel(si, &si->chans[i], buf, spv);

    agb_audio_m4a_buffer(si->pcmBuffer, sizeof(si->pcmBuffer));
    agb_audio_submit_s8(buf, buf + PCM_DMA_BUF_SIZE, spv, si->pcmFreq);

    si->ident = ID_NUMBER;
}

/* ---- track commands -------------------------------------------------------------------------- */

static u8 read_u8(MusicPlayerTrack *track)
{
    return *track->cmdPtr++;
}

void ply_fine(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    SoundChannel *chan = track->chan;

    (void)info;
    while (chan)
    {
        SoundChannel *next;

        if (chan->statusFlags & CHAN_ON) chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
        RealClearChain(chan);
        next = (SoundChannel *)chan->nextChannelPointer;
        if (next == chan)
        {
            chan->nextChannelPointer = 0;
            next = 0;
        }
        chan = next;
    }
    track->flags = 0;
}

void MPlayJumpTableCopy(MPlayFunc *table)
{
    int i;

    for (i = 0; i < 36; i++) table[i] = gMPlayJumpTableTemplate[i];
}

static void do_goto(MusicPlayerTrack *track)
{
    u8 *p = track->cmdPtr;

    track->cmdPtr = (u8 *)(unsigned long)(p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24));
}

void ply_goto(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    (void)info;
    do_goto(track);
}

void ply_patt(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    if (track->patternLevel >= 3)
    {
        ply_fine(info, track);
        return;
    }
    track->patternStack[track->patternLevel] = track->cmdPtr + 4;
    track->patternLevel++;
    do_goto(track);
}

void ply_pend(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    (void)info;
    if (track->patternLevel == 0) return;
    track->patternLevel--;
    track->cmdPtr = track->patternStack[track->patternLevel];
}

void ply_rept(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    u8 *start = track->cmdPtr;
    u8 n;

    (void)info;
    if (*start == 0)
    {
        track->cmdPtr = start + 1;
        do_goto(track);
        return;
    }
    track->repN++;
    n = read_u8(track);
    if (track->repN < n)
    {
        do_goto(track);
        return;
    }
    track->repN = 0;
    track->cmdPtr = start + 5;
}

void ply_prio(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    (void)info;
    track->priority = read_u8(track);
}

void ply_tempo(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    u32 t = (u32)read_u8(track) << 1;

    info->tempoD = (u16)t;
    info->tempoI = (u16)((t * info->tempoU) >> 8);
}

void ply_keysh(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    (void)info;
    track->keyShift = (s8)read_u8(track);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_voice(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    u8 idx = read_u8(track);

    track->tone = info->tone[idx];
}

void ply_vol(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    (void)info;
    track->vol = read_u8(track);
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_pan(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    (void)info;
    track->pan = (s8)(read_u8(track) - 0x40);
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_bend(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    (void)info;
    track->bend = (s8)(read_u8(track) - 0x40);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_bendr(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    (void)info;
    track->bendRange = read_u8(track);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_lfodl(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    (void)info;
    track->lfoDelay = read_u8(track);
}

void ply_modt(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    u8 v = read_u8(track);

    (void)info;
    if (track->modT == v) return;
    track->modT = v;
    track->flags |= MPT_FLG_VOLCHG | MPT_FLG_PITCHG;
}

void ply_tune(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    (void)info;
    track->tune = (s8)(read_u8(track) - 0x40);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_port(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    u8 reg = read_u8(track);
    u8 value = read_u8(track);

    (void)info;
    agb_io[0x60 + reg] = value;
}

static void clear_mod_m(MusicPlayerTrack *track)
{
    track->modM = 0;
    track->lfoSpeedC = 0;
    track->flags |= track->modT ? MPT_FLG_VOLCHG : MPT_FLG_PITCHG;
}

void ply_lfos(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    (void)info;
    track->lfoSpeed = read_u8(track);
    if (track->lfoSpeed == 0) clear_mod_m(track);
}

void ply_mod(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    (void)info;
    track->mod = read_u8(track);
    if (track->mod == 0) clear_mod_m(track);
}

void ply_endtie(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    u8 key;
    SoundChannel *chan;

    (void)info;
    if (*track->cmdPtr < 0x80)
    {
        key = *track->cmdPtr++;
        track->key = key;
    }
    else
    {
        key = track->key;
    }
    for (chan = track->chan; chan;)
    {
        SoundChannel *next;
        u8 st = chan->statusFlags;

        if ((st & 0x83) && !(st & SOUND_CHANNEL_SF_STOP) && chan->midiKey == key)
        {
            chan->statusFlags = st | SOUND_CHANNEL_SF_STOP;
            return;
        }
        next = (SoundChannel *)chan->nextChannelPointer;
        if (next == chan)
        {
            chan->nextChannelPointer = 0;
            next = 0;
        }
        chan = next;
    }
}

void TrackStop(MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    SoundChannel *chan;

    (void)info;
    if (!(track->flags & MPT_FLG_EXIST)) return;
    for (chan = track->chan; chan;)
    {
        SoundChannel *next;

        if (chan->statusFlags)
        {
            if (chan->type & 7) gSoundInfoPtr->CgbOscOff(chan->type & 7);
            chan->statusFlags = 0;
        }
        chan->track = 0;
        next = (SoundChannel *)chan->nextChannelPointer;
        if (next == chan)
        {
            chan->nextChannelPointer = 0;
            next = 0;
        }
        chan = next;
    }
    track->chan = 0;
}

static void chn_vol_set(SoundChannel *chan, MusicPlayerTrack *track)
{
    s32 pan = (s8)chan->rhythmPan;
    u32 r = ((u32)(0x80 + pan) * chan->velocity * track->volMR) >> 14;
    u32 l = ((u32)(0x7F - pan) * chan->velocity * track->volML) >> 14;

    chan->rightVolume = (u8)(r > 0xFF ? 0xFF : r);
    chan->leftVolume = (u8)(l > 0xFF ? 0xFF : l);
}

void ply_note(u32 noteCmd, MusicPlayerInfo *info, MusicPlayerTrack *track)
{
    SoundInfo *si = gSoundInfoPtr;
    ToneData *tone;
    u32 key, prio, cgbType;
    s32 rhythmPan = 0;
    SoundChannel *chan = 0;
    s32 k;

    track->gateTime = gClockTable[noteCmd];
    {
        u8 *p = track->cmdPtr;

        if (*p < 0x80)
        {
            track->key = *p++;
            if (*p < 0x80)
            {
                track->velocity = *p++;
                if (*p < 0x80)
                {
                    track->gateTime += *p;
                    p++;
                }
            }
            track->cmdPtr = p;
        }
    }

    if (track->tone.type & 0xC0)
    {
        u32 idx = track->key;

        if (track->tone.type & 0x40)
        {
            /* key-split tones keep their split table where attack..release would be */
            const u8 *split;

            __builtin_memcpy(&split, &track->tone.attack, sizeof(split));
            idx = split[track->key];
        }
        tone = (ToneData *)track->tone.wav + idx;
        if (tone->type & 0xC0) return;
        key = track->key;
        if (track->tone.type & 0x80)
        {
            if (tone->pan_sweep & 0x80) rhythmPan = ((s32)tone->pan_sweep - 0xC0) * 2;
            key = tone->key;
        }
    }
    else
    {
        tone = &track->tone;
        key = track->key;
    }

    prio = info->priority + track->priority;
    if (prio > 0xFF) prio = 0xFF;
    cgbType = tone->type & 7;

    if (cgbType)
    {
        SoundChannel *c;

        if (!si->cgbChans) return;
        c = (SoundChannel *)((u8 *)si->cgbChans + (cgbType - 1) * 0x40);
        if (!(c->statusFlags & CHAN_ON) || (c->statusFlags & SOUND_CHANNEL_SF_STOP) || c->priority < prio ||
            (c->priority == prio && (unsigned long)c->track >= (unsigned long)track))
            chan = c;
        else
            return;
    }
    else
    {
        u32 bestPrio = prio;
        unsigned long bestTrack = (unsigned long)track;
        int releasing = 0;
        SoundChannel *c = si->chans;
        int n = si->maxChans;

        for (; n > 0; n--, c++)
        {
            u8 st = c->statusFlags;

            if (!(st & CHAN_ON))
            {
                chan = c;
                break;
            }
            if (st & SOUND_CHANNEL_SF_STOP)
            {
                if (!releasing)
                {
                    releasing = 1;
                    bestPrio = c->priority;
                    bestTrack = (unsigned long)c->track;
                    chan = c;
                    continue;
                }
            }
            else if (releasing)
            {
                continue;
            }
            if (c->priority < bestPrio)
            {
                bestPrio = c->priority;
                bestTrack = (unsigned long)c->track;
                chan = c;
            }
            else if (c->priority == bestPrio)
            {
                if ((unsigned long)c->track > bestTrack)
                {
                    bestTrack = (unsigned long)c->track;
                    chan = c;
                }
                else if ((unsigned long)c->track == bestTrack)
                {
                    chan = c;
                }
            }
        }
        if (!chan) return;
    }

    ClearChain(chan);
    chan->prevChannelPointer = 0;
    chan->nextChannelPointer = track->chan;
    if (track->chan) track->chan->prevChannelPointer = chan;
    track->chan = chan;
    chan->track = track;
    track->lfoDelayC = track->lfoDelay;
    if (track->lfoDelay) clear_mod_m(track);
    TrkVolPitSet(info, track);

    chan->gateTime = track->gateTime;
    chan->midiKey = track->key;
    chan->velocity = track->velocity;
    chan->priority = (u8)prio;
    chan->key = (u8)key;
    chan->rhythmPan = (u8)rhythmPan;
    chan->type = tone->type;
    chan->wav = tone->wav;
    chan->attack = tone->attack;
    chan->decay = tone->decay;
    chan->sustain = tone->sustain;
    chan->release = tone->release;
    chan->pseudoEchoVolume = track->pseudoEchoVolume;
    chan->pseudoEchoLength = track->pseudoEchoLength;
    chn_vol_set(chan, track);

    k = (s32)chan->key + (s8)track->keyM;
    if (k < 0) k = 0;

    if (cgbType)
    {
        CgbChannel *cgb = (CgbChannel *)chan;
        u8 sweep = tone->pan_sweep;

        cgb->length = tone->length;
        if ((sweep & 0x80) || !(sweep & 0x70)) sweep = 8;
        cgb->sweep = sweep;
        chan->frequency = si->MidiKeyToCgbFreq((u8)cgbType, (u8)k, track->pitM);
    }
    else
    {
        chan->frequency = MidiKeyToFreq(chan->wav, (u8)k, track->pitM);
    }
    chan->statusFlags = SOUND_CHANNEL_SF_START;
    track->flags &= 0xF0;
}

/* ---- sequencer -------------------------------------------------------------------------------- */

static void track_tick(MusicPlayerInfo *info, MusicPlayerTrack *track, SoundInfo *si)
{
    SoundChannel *chan;

    /* gate time of sounding notes */
    for (chan = track->chan; chan;)
    {
        SoundChannel *next;

        if (chan->statusFlags & CHAN_ON)
        {
            if (chan->gateTime && --chan->gateTime == 0) chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
        }
        else
        {
            ClearChain(chan);
        }
        next = (SoundChannel *)chan->nextChannelPointer;
        if (next == chan)
        {
            chan->nextChannelPointer = 0;
            next = 0;
        }
        chan = next;
    }

    if (track->flags & MPT_FLG_START)
    {
        Clear64byte(track);
        track->flags = MPT_FLG_EXIST;
        track->bendRange = 2;
        track->volX = 0x40;
        track->lfoSpeed = 0x16;
        track->tone.type = 1;
    }

    while (track->wait == 0)
    {
        u8 *p = track->cmdPtr;
        u32 cmd = *p;

        if (cmd < 0x80)
        {
            cmd = track->runningStatus;
        }
        else
        {
            track->cmdPtr = p + 1;
            if (cmd >= 0xBD) track->runningStatus = (u8)cmd;
        }

        if (cmd >= 0xCF)
        {
            si->plynote(cmd - 0xCF, info, track);
        }
        else if (cmd > 0xB0)
        {
            info->cmd = (u8)(cmd - 0xB1);
            ((XcmdFunc)si->MPlayJumpTable[cmd - 0xB1])(info, track);
            if (track->flags == 0) return;
        }
        else
        {
            track->wait = gClockTable[cmd - 0x80];
        }
    }

    track->wait--;
    if (track->lfoSpeed == 0 || track->mod == 0) return;
    if (track->lfoDelayC)
    {
        track->lfoDelayC--;
        return;
    }
    {
        u8 c = (u8)(track->lfoSpeedC + track->lfoSpeed);
        s32 wave, m;

        track->lfoSpeedC = c;
        if ((s8)(c - 0x40) < 0) wave = (s8)c;
        else wave = 0x80 - (s32)c;
        m = (track->mod * wave) >> 6;
        if ((u8)(track->modM ^ m) == 0) return;
        track->modM = (s8)m;
        track->flags |= track->modT ? MPT_FLG_VOLCHG : MPT_FLG_PITCHG;
    }
}

void MPlayMain(MusicPlayerInfo *info)
{
    SoundInfo *si;
    u32 tempo;

    if (info->ident != ID_NUMBER) return;
    info->ident++;
    if (info->MPlayMainNext) info->MPlayMainNext(info->musicPlayerNext);

    if ((s32)info->status < 0) goto done;
    si = gSoundInfoPtr;
    FadeOutBody(info);
    if ((s32)info->status < 0) goto done;

    tempo = (u16)(info->tempoC + info->tempoI);
    for (;;)
    {
        MusicPlayerTrack *track = info->tracks;
        u32 bit = 1, active = 0;
        int n;

        info->tempoC = (u16)tempo;
        if (tempo < 150) break;
        for (n = info->trackCount; n > 0; n--, track++, bit <<= 1)
        {
            if (!(track->flags & MPT_FLG_EXIST)) continue;
            active |= bit;
            track_tick(info, track, si);
        }
        info->clock++;
        if (active == 0)
        {
            info->status = MUSICPLAYER_STATUS_PAUSE;
            goto done;
        }
        info->status = active;
        tempo = (u16)(tempo - 150);
    }

    {
        MusicPlayerTrack *track = info->tracks;
        int n;

        for (n = info->trackCount; n > 0; n--, track++)
        {
            SoundChannel *chan;

            if (!(track->flags & MPT_FLG_EXIST) || !(track->flags & 0xF)) continue;
            TrkVolPitSet(info, track);
            for (chan = track->chan; chan;)
            {
                SoundChannel *next;

                if (!(chan->statusFlags & CHAN_ON))
                {
                    ClearChain(chan);
                }
                else
                {
                    u32 cgbType = chan->type & 7;

                    if (track->flags & MPT_FLG_VOLCHG)
                    {
                        chn_vol_set(chan, track);
                        if (cgbType) ((CgbChannel *)chan)->modify |= CGB_CHANNEL_MO_VOL;
                    }
                    if (track->flags & MPT_FLG_PITCHG)
                    {
                        s32 k = (s32)chan->key + (s8)track->keyM;

                        if (k < 0) k = 0;
                        if (cgbType)
                        {
                            ((CgbChannel *)chan)->frequency = si->MidiKeyToCgbFreq((u8)cgbType, (u8)k, track->pitM);
                            ((CgbChannel *)chan)->modify |= CGB_CHANNEL_MO_PIT;
                        }
                        else
                        {
                            chan->frequency = MidiKeyToFreq(chan->wav, (u8)k, track->pitM);
                        }
                    }
                }
                next = (SoundChannel *)chan->nextChannelPointer;
                if (next == chan)
                {
                    chan->nextChannelPointer = 0;
                    next = 0;
                }
                chan = next;
            }
            track->flags &= 0xF0;
        }
    }

done:
    info->ident = ID_NUMBER;
}
