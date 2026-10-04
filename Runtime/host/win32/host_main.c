/*
 * Windows host for natively built GBA games (com.recomp.gba runtime): implements agb_host.h.
 *
 *   game.exe [--scale N]                                 windowed, keyboard / XInput
 *   game.exe --headless --frames N [--dump dir] [--every N] [--script F:HEX[:DUR],...] [--wav f]
 *   game.exe --shm NAME                                  embedded: frames / input via shared memory (Polyphase addon)
 *
 * The game runs on its own thread; the main thread owns the window, input and pacing.
 * --script holds KEYINPUT bits (A=1 B=2 Select=4 Start=8 Right=10 Left=20 Up=40 Down=80 R=100 L=200).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <xinput.h>
#include <dbghelp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agb.h"
#include "agb_host.h"
#include "port_shm.h"

#ifndef AGB_GAME_TITLE
#define AGB_GAME_TITLE "GBA game"
#endif
#ifndef AGB_GAME_NAME
#define AGB_GAME_NAME "game"
#endif

void agb_game_entry(void); /* game glue (native) or agbw_guest.c (wasm); never returns normally */
static void profile_report(void);

/* 32-bit x86 = the native guest: game code faults on NULL reads and divisions by zero
 * and the host fixes them up. The wasm guest (any word size) cannot fault that way. */
#if defined(_M_IX86) || defined(__i386__)
#define HOST_X86 1
int port_x86_emulate_access(CONTEXT *ctx, unsigned char *mirror, int *was_skipped);
int port_x86_emulate_divide(CONTEXT *ctx, int overflow);
#else
#define HOST_X86 0
#endif

#define FPS_NUM 16777216ull /* GBA: 16.78 MHz / 280896 cycles per frame = 59.7275 Hz */
#define FPS_DEN 280896ull
#define W 240
#define H 160

/* ---- options / state ------------------------------------------------------------ */
static char sSavePath[MAX_PATH];
static char sDataPath[MAX_PATH];
static FILE *sDataFile;
static int sHeadless;
static int sFramesLimit = -1;
static const char *sDumpDir;
static int sDumpEvery = 30;
static int sDumpFrom;
static int sScale = 4;
static const char *sShmName;
#define MAX_SCRIPT 512
static struct { int frame, dur; unsigned keys; } sScript[MAX_SCRIPT];
static int sScriptCount;
static unsigned sFuzzSeed;

static volatile LONG sTick;
static HANDLE sTickEvent;
static volatile LONG sKeys;
static unsigned sFrameNo;

static CRITICAL_SECTION sFrameLock;
static unsigned char sFrameBgra[W * H * 4];
static volatile LONG sFrameSerial;

static HWND sWindow;
static FILE *sLogFile;
static PortShm *sShm;
static DWORD sGameThreadId;
static FILE *sWavFile;
static unsigned long sWavFrames;

static void parse_script(const char *text)
{
    while (*text && sScriptCount < MAX_SCRIPT)
    {
        int f, d = 4;
        unsigned b;
        char *end;

        f = (int)strtol(text, &end, 10);
        if (*end != ':') break;
        b = (unsigned)strtoul(end + 1, &end, 16);
        if (*end == ':') d = (int)strtol(end + 1, &end, 10);
        sScript[sScriptCount].frame = f;
        sScript[sScriptCount].keys = b;
        sScript[sScriptCount].dur = d;
        sScriptCount++;
        text = end;
        while (*text == ',' || *text == ' ') text++;
    }
}

/* ---- logging ---------------------------------------------------------------------- */
void agb_host_log(const char *line)
{
    fprintf(stderr, "[gba] %s\n", line);
    fflush(stderr);
    if (sLogFile)
    {
        fprintf(sLogFile, "%s\n", line);
        fflush(sLogFile);
    }
}

static void host_log(const char *fmt, ...)
{
    char buf[1024];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    agb_host_log(buf);
}

void agb_host_fatal(const char *line)
{
    host_log("FATAL: %s", line);
    RaiseException(0xE0440001, EXCEPTION_NONCONTINUABLE, 0, NULL);
    ExitProcess(1);
}

/* ---- crash reporting / safety net ----------------------------------------------------- */
static void print_stack(CONTEXT *ctx)
{
    HANDLE process = GetCurrentProcess();
    STACKFRAME64 frame;
    char buffer[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *symbol = (SYMBOL_INFO *)buffer;
    int depth;

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, NULL, TRUE);
    memset(&frame, 0, sizeof(frame));
#if HOST_X86
    frame.AddrPC.Offset = ctx->Eip; frame.AddrFrame.Offset = ctx->Ebp; frame.AddrStack.Offset = ctx->Esp;
#define HOST_MACHINE IMAGE_FILE_MACHINE_I386
#else
    frame.AddrPC.Offset = ctx->Rip; frame.AddrFrame.Offset = ctx->Rbp; frame.AddrStack.Offset = ctx->Rsp;
#define HOST_MACHINE IMAGE_FILE_MACHINE_AMD64
#endif
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    for (depth = 0; depth < 32; depth++)
    {
        DWORD64 displacement = 0;
        DWORD line_displacement = 0;
        IMAGEHLP_LINE64 line;

        if (!StackWalk64(HOST_MACHINE, process, GetCurrentThread(), &frame, ctx, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL) || frame.AddrPC.Offset == 0)
            break;
        memset(buffer, 0, sizeof(buffer));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        line.SizeOfStruct = sizeof(line);
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
        {
            if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_displacement, &line))
            {
                const char *file = strrchr(line.FileName, '\\');
                host_log("  #%d %s (%s:%lu)", depth, symbol->Name, file ? file + 1 : line.FileName, line.LineNumber);
            }
            else
            {
                host_log("  #%d %s+0x%llX", depth, symbol->Name, displacement);
            }
        }
        else
        {
            host_log("  #%d %llX", depth, (unsigned long long)frame.AddrPC.Offset);
        }
    }
}

/*
 * On the GBA a NULL (or small) pointer reads the BIOS area instead of crashing, and
 * games do it. A faulting access below 64 KB is retried with its base register moved
 * into a zeroed stand-in block, single-stepped, and the register put back afterwards
 * (unless the instruction overwrote it); an absolute operand is emulated on the block.
 * Integer division by zero returns 0 as with the game's libgcc.
 */
/* "function+offset (file:line)" for log lines */
static const char *where(DWORD64 eip)
{
    static int init;
    static char text[300];
    char buffer[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *symbol = (SYMBOL_INFO *)buffer;
    DWORD64 displacement = 0;
    DWORD line_displacement = 0;
    IMAGEHLP_LINE64 line;
    HANDLE process = GetCurrentProcess();

    if (!init)
    {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        SymInitialize(process, NULL, TRUE);
        init = 1;
    }
    memset(buffer, 0, sizeof(buffer));
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    line.SizeOfStruct = sizeof(line);
    if (!SymFromAddr(process, eip, &displacement, symbol))
    {
        snprintf(text, sizeof(text), "%llX", (unsigned long long)eip);
        return text;
    }
    if (SymGetLineFromAddr64(process, eip, &line_displacement, &line))
    {
        const char *file = strrchr(line.FileName, '\\');
        snprintf(text, sizeof(text), "%s (%s:%lu)", symbol->Name, file ? file + 1 : line.FileName, line.LineNumber);
    }
    else
    {
        snprintf(text, sizeof(text), "%s+0x%llX", symbol->Name, displacement);
    }
    return text;
}

#if HOST_X86
#define LOW_SIZE 0x10000u
static unsigned char sLowMem[LOW_SIZE + 0x1000];
static unsigned sFixCount;
static DWORD sFixEip;
static int sFixReg = -1;
static DWORD sFixOrig;
static unsigned sFixTried;

static DWORD *context_reg(CONTEXT *ctx, int i)
{
    switch (i)
    {
    case 0: return &ctx->Eax;
    case 1: return &ctx->Ecx;
    case 2: return &ctx->Edx;
    case 3: return &ctx->Ebx;
    case 4: return &ctx->Esi;
    case 5: return &ctx->Edi;
    default: return &ctx->Ebp;
    }
}

static LONG CALLBACK safety_handler(EXCEPTION_POINTERS *info)
{
    EXCEPTION_RECORD *rec = info->ExceptionRecord;
    CONTEXT *ctx = info->ContextRecord;
    DWORD delta = (DWORD)(unsigned long)sLowMem;
    int i;

    if (GetCurrentThreadId() != sGameThreadId) return EXCEPTION_CONTINUE_SEARCH;
    if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && sFixReg >= 0)
    {
        DWORD *r = context_reg(ctx, sFixReg);

        if (*r == sFixOrig + delta) *r = sFixOrig;
        sFixReg = -1;
        sFixEip = 0; /* it worked: the next fault here (a loop) starts over */
        ctx->EFlags &= ~0x100u;
        /* reads see zeroes: forget what the instruction may have written */
        memset(sLowMem, 0, sizeof(sLowMem));
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (rec->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO || rec->ExceptionCode == EXCEPTION_INT_OVERFLOW)
    {
        DWORD eip = ctx->Eip;

        if (port_x86_emulate_divide(ctx, rec->ExceptionCode == EXCEPTION_INT_OVERFLOW))
        {
            if (sFixCount++ < 64) host_log("integer division fault in %s given the GBA result", where(eip));
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    {
        DWORD addr = (DWORD)rec->ExceptionInformation[1];

        if (addr >= LOW_SIZE) return EXCEPTION_CONTINUE_SEARCH;
        if (sFixReg >= 0)
        {
            /* the previous candidate did not help: undo it before trying another */
            *context_reg(ctx, sFixReg) = sFixOrig;
            sFixReg = -1;
            ctx->EFlags &= ~0x100u;
        }
        if (ctx->Eip != sFixEip)
        {
            sFixEip = ctx->Eip;
            sFixTried = 0;
        }
        /* candidates: registers pointing just below the faulting address, zero first */
        for (i = 0; i < 2 * 7; i++)
        {
            int reg = i % 7;
            DWORD v = *context_reg(ctx, reg);

            if (sFixTried & (1u << reg)) continue;
            if (i < 7 && v != 0) continue;
            if (v >= LOW_SIZE || addr - v >= LOW_SIZE) continue;
            sFixTried |= 1u << reg;
            sFixReg = reg;
            sFixOrig = v;
            *context_reg(ctx, reg) = v + delta;
            ctx->EFlags |= 0x100u;
            if (sFixCount++ < 64)
                host_log("low-memory %s in %s (address %08lX), retried on a zeroed block",
                         rec->ExceptionInformation[0] ? "write" : "read", where(ctx->Eip), addr);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        /* absolute operand (no register to move): perform the access on the block */
        {
            int skipped;
            DWORD eip = ctx->Eip;

            memset(sLowMem, 0, sizeof(sLowMem));
            if (port_x86_emulate_access(ctx, sLowMem + addr, &skipped))
            {
                sFixEip = 0;
                if (sFixCount++ < 64 || skipped)
                    host_log("low-memory %s in %s (address %08lX), %s", rec->ExceptionInformation[0] ? "write" : "read",
                             where(eip), addr, skipped ? "instruction skipped" : "emulated");
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif /* HOST_X86 */

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *info)
{
    DWORD code = info->ExceptionRecord->ExceptionCode;
    CONTEXT ctx;

    if (code < 0xC0000000 && code != 0xE0440001) return EXCEPTION_CONTINUE_SEARCH;
    host_log("CRASH: exception 0x%08lX at %p (address %p) frame %u", code, info->ExceptionRecord->ExceptionAddress,
             (code == EXCEPTION_ACCESS_VIOLATION) ? (void *)info->ExceptionRecord->ExceptionInformation[1] : NULL, sFrameNo);
    ctx = *info->ContextRecord;
#if HOST_X86
    host_log("  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX", ctx.Eax, ctx.Ebx,
             ctx.Ecx, ctx.Edx, ctx.Esi, ctx.Edi, ctx.Ebp, ctx.Esp);
#endif
    print_stack(&ctx);
    if (sShm) sShm->status = PORT_SHM_STATUS_CRASHED;
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- input ---------------------------------------------------------------------------- */
#define K_A 0x001
#define K_B 0x002
#define K_SELECT 0x004
#define K_START 0x008
#define K_RIGHT 0x010
#define K_LEFT 0x020
#define K_UP 0x040
#define K_DOWN 0x080
#define K_R 0x100
#define K_L 0x200

static unsigned poll_keyboard(void)
{
    static const struct { int key; unsigned bit; } map[] = {
        { VK_UP, K_UP }, { VK_DOWN, K_DOWN }, { VK_LEFT, K_LEFT }, { VK_RIGHT, K_RIGHT },
        { 'Z', K_B }, { 'X', K_A }, { 'A', K_L }, { 'S', K_R }, { 'Q', K_L }, { 'W', K_R },
        { VK_RETURN, K_START }, { VK_BACK, K_SELECT }, { VK_RSHIFT, K_SELECT },
    };
    unsigned bits = 0;
    int i;

    if (GetForegroundWindow() != sWindow) return 0;
    for (i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++)
        if (GetAsyncKeyState(map[i].key) & 0x8000) bits |= map[i].bit;
    return bits;
}

static unsigned poll_xinput(void)
{
    XINPUT_STATE state;
    unsigned bits = 0;
    WORD b;

    if (XInputGetState(0, &state) != ERROR_SUCCESS) return 0;
    b = state.Gamepad.wButtons;
    if (b & XINPUT_GAMEPAD_DPAD_UP) bits |= K_UP;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN) bits |= K_DOWN;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT) bits |= K_LEFT;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT) bits |= K_RIGHT;
    if (b & XINPUT_GAMEPAD_A) bits |= K_A;
    if (b & XINPUT_GAMEPAD_B) bits |= K_B;
    if (b & XINPUT_GAMEPAD_X) bits |= K_B;
    if (b & XINPUT_GAMEPAD_LEFT_SHOULDER) bits |= K_L;
    if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) bits |= K_R;
    if (b & XINPUT_GAMEPAD_START) bits |= K_START;
    if (b & XINPUT_GAMEPAD_BACK) bits |= K_SELECT;
    if (state.Gamepad.bLeftTrigger > 64) bits |= K_L;
    if (state.Gamepad.bRightTrigger > 64) bits |= K_R;
    if (state.Gamepad.sThumbLX < -16000) bits |= K_LEFT;
    if (state.Gamepad.sThumbLX > 16000) bits |= K_RIGHT;
    if (state.Gamepad.sThumbLY > 16000) bits |= K_UP;
    if (state.Gamepad.sThumbLY < -16000) bits |= K_DOWN;
    return bits;
}

unsigned int agb_host_keys(void)
{
    unsigned bits = 0;
    int i;

    for (i = 0; i < sScriptCount; i++)
        if ((int)sFrameNo >= sScript[i].frame && (int)sFrameNo < sScript[i].frame + sScript[i].dur) bits |= sScript[i].keys;
    if (sFuzzSeed)
    {
        /* random button mashing for robustness runs; Select+Start+A+B (soft reset) never together */
        static unsigned state, held, until;

        if (!state) state = sFuzzSeed;
        if (sFrameNo >= until)
        {
            state = state * 1103515245u + 12345u;
            held = (state >> 8) & 0x3FF;
            if ((held & 0xF) == 0xF) held &= ~K_SELECT;
            until = sFrameNo + 2 + ((state >> 20) & 15);
        }
        bits |= held;
    }
    if (sHeadless) return bits;
    return bits | (unsigned)sKeys;
}

/* ---- video ---------------------------------------------------------------------------- */
static void write_ppm(const char *path, const unsigned char *bgra)
{
    FILE *f = fopen(path, "wb");
    int i;

    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (i = 0; i < W * H; i++)
    {
        unsigned char rgb[3] = { bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4] };

        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static unsigned char expand5(unsigned v)
{
    return (unsigned char)((v << 3) | (v >> 2));
}

static void wav_header(FILE *f, unsigned long frames);

void agb_host_present(const unsigned short *bgr555)
{
    static unsigned char bgra[W * H * 4];
    int i;

    for (i = 0; i < W * H; i++)
    {
        unsigned c = bgr555[i];

        bgra[i * 4] = expand5((c >> 10) & 31);
        bgra[i * 4 + 1] = expand5((c >> 5) & 31);
        bgra[i * 4 + 2] = expand5(c & 31);
        bgra[i * 4 + 3] = 255;
    }
    EnterCriticalSection(&sFrameLock);
    memcpy(sFrameBgra, bgra, sizeof(bgra));
    InterlockedIncrement(&sFrameSerial);
    LeaveCriticalSection(&sFrameLock);

    if (sShm)
    {
        int slot = (sShm->frame_index + 1) & 1;
        unsigned char *dst = sShm->frames[slot];

        for (i = 0; i < W * H; i++)
        {
            dst[i * 4] = bgra[i * 4 + 2];
            dst[i * 4 + 1] = bgra[i * 4 + 1];
            dst[i * 4 + 2] = bgra[i * 4];
            dst[i * 4 + 3] = 255;
        }
        sShm->width[slot] = W;
        sShm->height[slot] = H;
        MemoryBarrier();
        sShm->frame_index = slot;
        InterlockedIncrement((volatile LONG *)&sShm->frame_serial);
    }
    if (sDumpDir && (int)sFrameNo >= sDumpFrom && sFrameNo % sDumpEvery == 0)
    {
        char path[MAX_PATH];

        snprintf(path, sizeof(path), "%s/frame_%05u.ppm", sDumpDir, sFrameNo);
        write_ppm(path, bgra);
    }
    sFrameNo++;
    if (sHeadless && sFramesLimit >= 0 && (int)sFrameNo >= sFramesLimit)
    {
        host_log("reached %d frames", sFramesLimit);
        profile_report();
        if (sWavFile) { wav_header(sWavFile, sWavFrames); fclose(sWavFile); sWavFile = NULL; }
        ExitProcess(0);
    }
}

void agb_host_wait_frame(void)
{
    if (sHeadless) return;
    WaitForSingleObject(sTickEvent, 100);
}

/* ---- audio ---------------------------------------------------------------------------- */
#define AUDIO_RING (44100 * 2)
static short sAudioRing[AUDIO_RING * 2];
static volatile LONG sAudioWrite, sAudioRead;
static HWAVEOUT sWaveOut;
#define AUDIO_BLOCKS 4
#define AUDIO_BLOCK_FRAMES 1024
static WAVEHDR sWaveHdr[AUDIO_BLOCKS];
static short sWaveData[AUDIO_BLOCKS][AUDIO_BLOCK_FRAMES * 2];

static void wav_header(FILE *f, unsigned long frames)
{
    unsigned long data = frames * 4, v;

    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    v = 36 + data; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f);
    v = 0x00020001; fwrite(&v, 4, 1, f);
    v = 44100; fwrite(&v, 4, 1, f);
    v = 44100 * 4; fwrite(&v, 4, 1, f);
    v = 0x00100004; fwrite(&v, 4, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data, 4, 1, f);
    fseek(f, 0, SEEK_END);
}

void agb_host_audio(const short *samples, int frames)
{
    int i;

    if (sWavFile)
    {
        fwrite(samples, 4, (size_t)frames, sWavFile);
        sWavFrames += (unsigned long)frames;
        if ((sWavFrames & 0xFFFF) < (unsigned long)frames) wav_header(sWavFile, sWavFrames);
    }
    if (sShm)
    {
        for (i = 0; i < frames; i++)
        {
            unsigned w = sShm->audio_write;

            if (w - sShm->audio_read >= PORT_SHM_AUDIO_FRAMES) break;
            sShm->audio[(w % PORT_SHM_AUDIO_FRAMES) * 2] = samples[i * 2];
            sShm->audio[(w % PORT_SHM_AUDIO_FRAMES) * 2 + 1] = samples[i * 2 + 1];
            MemoryBarrier();
            sShm->audio_write = w + 1;
        }
    }
    for (i = 0; i < frames; i++)
    {
        LONG w = sAudioWrite;

        if (w - sAudioRead >= AUDIO_RING) break;
        sAudioRing[(w % AUDIO_RING) * 2] = samples[i * 2];
        sAudioRing[(w % AUDIO_RING) * 2 + 1] = samples[i * 2 + 1];
        sAudioWrite = w + 1;
    }
}

static void audio_fill_block(int index)
{
    int i;

    for (i = 0; i < AUDIO_BLOCK_FRAMES; i++)
    {
        LONG r = sAudioRead;

        if (r < sAudioWrite)
        {
            sWaveData[index][i * 2] = sAudioRing[(r % AUDIO_RING) * 2];
            sWaveData[index][i * 2 + 1] = sAudioRing[(r % AUDIO_RING) * 2 + 1];
            sAudioRead = r + 1;
        }
        else
        {
            sWaveData[index][i * 2] = sWaveData[index][i * 2 + 1] = 0;
        }
    }
}

static void audio_open(void)
{
    WAVEFORMATEX format;
    int i;

    memset(&format, 0, sizeof(format));
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 44100;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = 44100 * 4;
    if (waveOutOpen(&sWaveOut, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
    {
        sWaveOut = NULL;
        return;
    }
    for (i = 0; i < AUDIO_BLOCKS; i++)
    {
        sWaveHdr[i].lpData = (LPSTR)sWaveData[i];
        sWaveHdr[i].dwBufferLength = sizeof(sWaveData[i]);
        waveOutPrepareHeader(sWaveOut, &sWaveHdr[i], sizeof(WAVEHDR));
        audio_fill_block(i);
        waveOutWrite(sWaveOut, &sWaveHdr[i], sizeof(WAVEHDR));
    }
}

static void audio_pump(void)
{
    int i;

    if (!sWaveOut) return;
    /* keep latency bounded if the game ran ahead */
    if (sAudioWrite - sAudioRead > 44100 / 4) sAudioRead = sAudioWrite - 44100 / 8;
    for (i = 0; i < AUDIO_BLOCKS; i++)
    {
        if (sWaveHdr[i].dwFlags & WHDR_DONE)
        {
            audio_fill_block(i);
            waveOutWrite(sWaveOut, &sWaveHdr[i], sizeof(WAVEHDR));
        }
    }
}

/* ---- save data -------------------------------------------------------------------------- */
int agb_host_load_save(unsigned char *data, int size)
{
    FILE *f = fopen(sSavePath, "rb");
    int got;

    if (!f) return 0;
    got = (int)fread(data, 1, (size_t)size, f);
    fclose(f);
    host_log("save: loaded %d bytes from %s", got, sSavePath);
    return got;
}

void agb_host_store_save(const unsigned char *data, int size)
{
    char tmp[MAX_PATH];
    FILE *f;

    snprintf(tmp, sizeof(tmp), "%s.tmp", sSavePath);
    f = fopen(tmp, "wb");
    if (!f) return;
    fwrite(data, 1, (size_t)size, f);
    fclose(f);
    MoveFileExA(tmp, sSavePath, MOVEFILE_REPLACE_EXISTING);
}

/* ---- debugging ---------------------------------------------------------------------------- */
static int sDebugFrame = -1;

int agb_host_debug_frame(void)
{
    return sDebugFrame;
}

void agb_host_debug_dump(const char *name, const void *data, unsigned size)
{
    char path[MAX_PATH];
    FILE *f;

    snprintf(path, sizeof(path), "%s/state_%05d_%s.bin", sDumpDir ? sDumpDir : ".", sDebugFrame, name);
    f = fopen(path, "wb");
    if (!f) return;
    fwrite(data, 1, size, f);
    fclose(f);
}

void agb_host_debug_backtrace(void)
{
    CONTEXT ctx;

    RtlCaptureContext(&ctx);
    print_stack(&ctx);
}

/* ---- game data file (wasm guest) ---------------------------------------------------------- */
int agb_host_read_data(unsigned offset, void *dst, unsigned size)
{
    if (!sDataFile)
    {
        sDataFile = fopen(sDataPath, "rb");
        if (!sDataFile)
        {
            host_log("cannot open the game data file %s", sDataPath);
            return 0;
        }
    }
    if (fseek(sDataFile, (long)offset, SEEK_SET) != 0) return 0;
    return (int)fread(dst, 1, size, sDataFile);
}

/* ---- game thread ------------------------------------------------------------------------ */
/* ---- sampling profiler (--profile) ---------------------------------------------------------
 * Samples the game thread's instruction pointer every millisecond and prints the hottest
 * functions at exit (the wasm2c build keeps the game's function names). */
#define PROF_SLOTS 65536
static int sProfile;
static HANDLE sGameThread;
static DWORD64 sProfAddr[PROF_SLOTS];
static unsigned sProfHits[PROF_SLOTS];
static unsigned sProfTotal;
static volatile LONG sProfStop;

static DWORD WINAPI profile_thread(void *param)
{
    (void)param;
    timeBeginPeriod(1);
    for (;;)
    {
        CONTEXT ctx;

        Sleep(1);
        if (sProfStop) return 0;
        if (SuspendThread(sGameThread) == (DWORD)-1) continue;
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(sGameThread, &ctx))
        {
#if HOST_X86
            DWORD64 pc = ctx.Eip;
#else
            DWORD64 pc = ctx.Rip;
#endif
            unsigned h = (unsigned)((pc >> 2) * 2654435761u) & (PROF_SLOTS - 1);
            while (sProfAddr[h] && sProfAddr[h] != pc) h = (h + 1) & (PROF_SLOTS - 1);
            sProfAddr[h] = pc;
            sProfHits[h]++;
            sProfTotal++;
        }
        ResumeThread(sGameThread);
    }
    return 0;
}

static void profile_report(void)
{
    /* aggregate by function */
    static char names[4096][128];
    static unsigned hits[4096];
    int count = 0, i, j;
    HANDLE process = GetCurrentProcess();
    char buffer[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *symbol = (SYMBOL_INFO *)buffer;

    if (!sProfile || !sProfTotal) return;
    sProfStop = 1;
    Sleep(5); /* let the sampler see it */
    SymSetOptions(SYMOPT_UNDNAME);
    SymInitialize(process, NULL, TRUE);
    for (i = 0; i < PROF_SLOTS; i++)
    {
        DWORD64 displacement = 0;
        const char *name = "?";

        if (!sProfHits[i]) continue;
        memset(buffer, 0, sizeof(buffer));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        if (SymFromAddr(process, sProfAddr[i], &displacement, symbol)) name = symbol->Name;
        for (j = 0; j < count && strcmp(names[j], name); j++) {}
        if (j == count)
        {
            if (count == 4096) continue;
            snprintf(names[count], sizeof(names[count]), "%s", name);
            hits[count++] = 0;
        }
        hits[j] += sProfHits[i];
    }
    host_log("profile: %u samples", sProfTotal);
    for (i = 0; i < 30; i++)
    {
        int best = -1;

        for (j = 0; j < count; j++)
            if (hits[j] && (best < 0 || hits[j] > hits[best])) best = j;
        if (best < 0) break;
        host_log("  %5.1f%%  %s", 100.0 * hits[best] / sProfTotal, names[best]);
        hits[best] = 0;
    }
}

static DWORD WINAPI game_thread(void *param)
{
    (void)param;
    agb_game_entry();
    host_log("game returned from AgbMain()");
    if (sShm) sShm->status = PORT_SHM_STATUS_EXITED;
    ExitProcess(0);
    return 0;
}

/* ---- window ------------------------------------------------------------------------------ */
static void toggle_fullscreen(HWND hwnd)
{
    static WINDOWPLACEMENT saved = {sizeof(WINDOWPLACEMENT)};
    DWORD style = (DWORD)GetWindowLongA(hwnd, GWL_STYLE);

    if (style & WS_OVERLAPPEDWINDOW)
    {
        MONITORINFO mi = {sizeof(mi)};

        if (GetWindowPlacement(hwnd, &saved) && GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
        {
            SetWindowLongA(hwnd, GWL_STYLE, (LONG)(style & ~WS_OVERLAPPEDWINDOW));
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    }
    else
    {
        SetWindowLongA(hwnd, GWL_STYLE, (LONG)(style | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(hwnd, &saved);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        BITMAPINFO bmi;
        int w, h, dw, dh, dx, dy;
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT bar;

        GetClientRect(hwnd, &rc);
        w = rc.right;
        h = rc.bottom;
        /* integer-ish 3:2 fit with black bars */
        dw = w; dh = w * 2 / 3;
        if (dh > h) { dh = h; dw = h * 3 / 2; }
        dx = (w - dw) / 2; dy = (h - dh) / 2;
        SetRect(&bar, 0, 0, w, dy); FillRect(dc, &bar, black);
        SetRect(&bar, 0, dy + dh, w, h); FillRect(dc, &bar, black);
        SetRect(&bar, 0, dy, dx, dy + dh); FillRect(dc, &bar, black);
        SetRect(&bar, dx + dw, dy, w, dy + dh); FillRect(dc, &bar, black);
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = W;
        bmi.bmiHeader.biHeight = -H;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        EnterCriticalSection(&sFrameLock);
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, dx, dy, dw, dh, 0, 0, W, H, sFrameBgra, &bmi, DIB_RGB_COLORS, SRCCOPY);
        LeaveCriticalSection(&sFrameLock);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
        if ((wp == VK_RETURN && msg == WM_SYSKEYDOWN) || wp == VK_F11)
        {
            toggle_fullscreen(hwnd);
            return 0;
        }
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int main(int argc, char **argv)
{
    HANDLE thread;
    int i;
    LARGE_INTEGER freq, start, now;
    unsigned long long frame = 0;
    LONG presented = 0;
    const char *saves = NULL;

    AddVectoredExceptionHandler(1, crash_filter);
#if HOST_X86
    AddVectoredExceptionHandler(1, safety_handler);
#endif
    InitializeCriticalSection(&sFrameLock);
    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--headless")) sHeadless = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) sFramesLimit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) sDumpDir = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) sDumpEvery = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-from") && i + 1 < argc) sDumpFrom = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) parse_script(argv[++i]);
        else if (!strcmp(argv[i], "--profile")) sProfile = 1;
        else if (!strcmp(argv[i], "--fuzz") && i + 1 < argc) sFuzzSeed = (unsigned)strtoul(argv[++i], NULL, 0) | 1;
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc)
        {
            sWavFile = fopen(argv[++i], "wb");
            if (sWavFile) wav_header(sWavFile, 0);
        }
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) sScale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shm") && i + 1 < argc) sShmName = argv[++i];
        else if (!strcmp(argv[i], "--saves") && i + 1 < argc) saves = argv[++i];
        else if (!strcmp(argv[i], "--dump-state") && i + 1 < argc) sDebugFrame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) snprintf(sDataPath, sizeof(sDataPath), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) sLogFile = fopen(argv[++i], "w");
        else
        {
            fprintf(stderr, "usage: %s [--scale N] [--headless --frames N [--dump dir] [--every N] [--script F:HEX[:DUR],...] "
                            "[--fuzz seed] [--wav file]] [--shm name] [--saves dir] [--log file]\n", AGB_GAME_NAME);
            return 2;
        }
    }
    {
        char dir[MAX_PATH];

        if (saves)
        {
            snprintf(dir, sizeof(dir), "%s", saves);
        }
        else
        {
            char *slash;

            GetModuleFileNameA(NULL, dir, sizeof(dir));
            slash = strrchr(dir, '\\');
            if (slash) *slash = 0;
            strncat(dir, "\\saves", sizeof(dir) - strlen(dir) - 1);
        }
        CreateDirectoryA(dir, NULL);
        snprintf(sSavePath, sizeof(sSavePath), "%s\\%s.sav", dir, AGB_GAME_NAME);
    }
    if (!sDataPath[0])
    {
        /* wasm guest: <game>.agbdata next to the executable */
        char *slash;

        GetModuleFileNameA(NULL, sDataPath, sizeof(sDataPath));
        slash = strrchr(sDataPath, '\\');
        if (slash) slash[1] = 0;
        strncat(sDataPath, AGB_GAME_NAME ".agbdata", sizeof(sDataPath) - strlen(sDataPath) - 1);
    }
    if (sShmName)
    {
        HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, sShmName);

        if (mapping) sShm = (PortShm *)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PortShm));
        if (!sShm)
        {
            host_log("cannot open shared memory '%s'", sShmName);
            return 1;
        }
        sHeadless = 0;
    }
    sTickEvent = CreateEventA(NULL, FALSE, FALSE, NULL);

    if (!sHeadless && !sShm)
    {
        WNDCLASSA wc;
        RECT rc = { 0, 0, W * sScale, H * sScale };

        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = "AgbRecompWindow";
        RegisterClassA(&wc);
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        sWindow = CreateWindowA(wc.lpszClassName, AGB_GAME_TITLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                                CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
    }
    if (!sHeadless) audio_open();
    if (sShm) sShm->status = PORT_SHM_STATUS_RUNNING;

    thread = CreateThread(NULL, 8 << 20, game_thread, NULL, CREATE_SUSPENDED, &sGameThreadId);
    sGameThread = thread;
    if (sProfile) CreateThread(NULL, 0, profile_thread, NULL, 0, NULL);
    ResumeThread(thread);
    if (sHeadless)
    {
        WaitForSingleObject(thread, INFINITE);
        return 0;
    }

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    timeBeginPeriod(1);
    for (;;)
    {
        MSG msg;
        unsigned long long elapsed_ticks;

        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) ExitProcess(0);
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (sShm)
        {
            if (sShm->command == PORT_SHM_CMD_QUIT) ExitProcess(0);
            sKeys = (LONG)sShm->pad;
        }
        else
        {
            sKeys = (LONG)(poll_keyboard() | poll_xinput());
        }
        QueryPerformanceCounter(&now);
        /* frames elapsed at 59.7275 Hz */
        elapsed_ticks = (unsigned long long)(now.QuadPart - start.QuadPart) * FPS_NUM / FPS_DEN / (unsigned long long)freq.QuadPart;
        if (elapsed_ticks > frame)
        {
            frame = elapsed_ticks > frame + 5 ? elapsed_ticks : frame + 1; /* no catch-up burst after a stall */
            InterlockedIncrement(&sTick);
            SetEvent(sTickEvent);
        }
        if (sWindow && sFrameSerial != presented)
        {
            presented = sFrameSerial;
            InvalidateRect(sWindow, NULL, FALSE);
        }
        audio_pump();
        Sleep(1);
    }
}
