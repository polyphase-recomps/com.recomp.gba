/**
 * @file GbaGuestRunner.cpp
 * @brief In-process host of a wasm2c GBA guest (see GbaGuestRunner.h).
 */
#include "GbaGuestRunner.h"

#include "Log.h"
#include "System/System.h"

#include <stdio.h>
#include <string.h>

#if PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif PLATFORM_LINUX || PLATFORM_ANDROID || PLATFORM_MAC
#include <pthread.h>
#elif PLATFORM_DOLPHIN
#include <ogc/lwp.h>
#elif PLATFORM_3DS
#include <3ds.h>
#endif

static const double kGbaFps = 16777216.0 / 280896.0; // 59.7275 Hz
static const size_t kAudioRingFrames = 44100 / 2;     // half a second

// ---- platform threads ------------------------------------------------------------------
// The engine's SYS_CreateThread uses 16 KB stacks on the consoles; the translated game keeps
// its locals on the native stack and needs far more.
namespace
{
struct ThreadStart
{
    void (*fn)(void*);
    void* arg;
};

#if PLATFORM_WINDOWS
DWORD WINAPI ThreadTrampoline(void* p)
{
    ThreadStart s = *(ThreadStart*)p;
    delete (ThreadStart*)p;
    s.fn(s.arg);
    return 0;
}
#elif PLATFORM_LINUX || PLATFORM_ANDROID || PLATFORM_MAC || PLATFORM_DOLPHIN
void* ThreadTrampoline(void* p)
{
    ThreadStart s = *(ThreadStart*)p;
    delete (ThreadStart*)p;
    s.fn(s.arg);
    return nullptr;
}
#elif PLATFORM_3DS
void ThreadTrampoline(void* p)
{
    ThreadStart s = *(ThreadStart*)p;
    delete (ThreadStart*)p;
    s.fn(s.arg);
}
#else
ThreadFuncRet ThreadTrampoline(void* p)
{
    ThreadStart s = *(ThreadStart*)p;
    delete (ThreadStart*)p;
    s.fn(s.arg);
    THREAD_RETURN();
}
#endif

void* StartThread(void (*fn)(void*), void* arg)
{
    ThreadStart* start = new ThreadStart{fn, arg};
#if PLATFORM_WINDOWS
    HANDLE h = CreateThread(nullptr, 8 << 20, ThreadTrampoline, start, 0, nullptr);
    return h;
#elif PLATFORM_LINUX || PLATFORM_ANDROID || PLATFORM_MAC
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 << 20);
    pthread_t* t = new pthread_t;
    if (pthread_create(t, &attr, ThreadTrampoline, start) != 0)
    {
        delete t;
        delete start;
        t = nullptr;
    }
    pthread_attr_destroy(&attr);
    return t;
#elif PLATFORM_DOLPHIN
    lwp_t* t = new lwp_t;
    // just below the main thread (higher numbers run first on libogc): the engine keeps
    // its frame rate and the game uses the time left
    if (LWP_CreateThread(t, ThreadTrampoline, start, nullptr, 512 * 1024, 63) != 0)
    {
        delete t;
        delete start;
        return nullptr;
    }
    return t;
#elif PLATFORM_3DS
    // just below the main thread (lower numbers run first on the 3DS)
    Thread t = threadCreate(ThreadTrampoline, start, 512 * 1024, 0x31, -2, false);
    if (t == nullptr) delete start;
    return t;
#else
    return SYS_CreateThread(ThreadTrampoline, start);
#endif
}

void JoinThread(void* thread)
{
#if PLATFORM_WINDOWS
    WaitForSingleObject((HANDLE)thread, INFINITE);
    CloseHandle((HANDLE)thread);
#elif PLATFORM_LINUX || PLATFORM_ANDROID || PLATFORM_MAC
    pthread_join(*(pthread_t*)thread, nullptr);
    delete (pthread_t*)thread;
#elif PLATFORM_DOLPHIN
    LWP_JoinThread(*(lwp_t*)thread, nullptr);
    delete (lwp_t*)thread;
#elif PLATFORM_3DS
    threadJoin((Thread)thread, U64_MAX);
    threadFree((Thread)thread);
#else
    SYS_JoinThread((ThreadObject*)thread);
    SYS_DestroyThread((ThreadObject*)thread);
#endif
}
} // namespace

// ---- runner ------------------------------------------------------------------------------
GbaGuestRunner::GbaGuestRunner()
{
    mLock = SYS_CreateMutex();
    mFrame.resize(240 * 160 * 4, 0);
    mAudio.resize(kAudioRingFrames * 2, 0);
}

GbaGuestRunner::~GbaGuestRunner()
{
    Stop();
    SYS_DestroyMutex(mLock);
}

bool GbaGuestRunner::Start(const AgbGuestDesc* guest, const std::string& dataPath, const std::string& savePath)
{
    Stop();
    mGuest = guest;
    mDataPath = dataPath;
    mSavePath = savePath;
    mDataFile = fopen(dataPath.c_str(), "rb");
    if (mDataFile == nullptr)
    {
        LogError("GbaPlayer: cannot open the game data %s", dataPath.c_str());
        return false;
    }
    mQuit = false;
    mDone = false;
    mFrameTokens = 1;
    mFrameClock = 0.0;
    mExitCode = 0;
    mAudioHead = mAudioCount = 0;
    mThread = StartThread(ThreadMain, this);
    if (mThread == nullptr)
    {
        LogError("GbaPlayer: cannot start the game thread");
        fclose((FILE*)mDataFile);
        mDataFile = nullptr;
        return false;
    }
    LogDebug("GbaPlayer: running %s in-process (%u MB guest memory)", guest->package,
             unsigned(guest->memory_bytes >> 20));
    return true;
}

void GbaGuestRunner::Stop()
{
    if (mThread == nullptr)
    {
        return;
    }
    mQuit = true;
    JoinThread(mThread);
    mThread = nullptr;
    if (mDataFile)
    {
        fclose((FILE*)mDataFile);
        mDataFile = nullptr;
    }
}

void GbaGuestRunner::ThreadMain(void* self)
{
    ((GbaGuestRunner*)self)->Run();
}

void GbaGuestRunner::Run()
{
    AgbHostApi host = {};
    host.user = this;
    host.present = HostPresent;
    host.wait_frame = HostWaitFrame;
    host.keys = HostKeys;
    host.audio = HostAudio;
    host.load_save = HostLoadSave;
    host.store_save = HostStoreSave;
    host.read_data = HostReadData;
    host.log = HostLog;
    mExitCode = mGuest->run(&host);
    if (mExitCode != 0)
    {
        LogError("GbaPlayer: %s stopped after a failure (see the log)", mGuest->package);
    }
    mDone = true;
}

void GbaGuestRunner::Advance(float deltaTime)
{
    mFrameClock += double(deltaTime) * kGbaFps;
    int due = int(mFrameClock);
    mFrameClock -= due;
    if (due > 0)
    {
        // a stall does not turn into a burst of catch-up frames
        int tokens = mFrameTokens.load() + due;
        mFrameTokens.store(tokens > 2 ? 2 : tokens);
    }
}

bool GbaGuestRunner::TakeFrame(uint8_t* rgba)
{
    SYS_LockMutex(mLock);
    bool fresh = mFrameNew;
    if (fresh)
    {
        memcpy(rgba, mFrame.data(), mFrame.size());
        mFrameNew = false;
    }
    SYS_UnlockMutex(mLock);
    return fresh;
}

int GbaGuestRunner::TakeAudio(int16_t* stereo, int maxFrames)
{
    SYS_LockMutex(mLock);
    int n = int(mAudioCount < size_t(maxFrames) ? mAudioCount : size_t(maxFrames));
    for (int i = 0; i < n; ++i)
    {
        size_t at = (mAudioHead + i) % kAudioRingFrames;
        stereo[i * 2] = mAudio[at * 2];
        stereo[i * 2 + 1] = mAudio[at * 2 + 1];
    }
    mAudioHead = (mAudioHead + n) % kAudioRingFrames;
    mAudioCount -= n;
    SYS_UnlockMutex(mLock);
    return n;
}

// ---- AgbHostApi (game thread) -------------------------------------------------------------
void GbaGuestRunner::HostPresent(void* user, const uint16_t* bgr555)
{
    GbaGuestRunner* self = (GbaGuestRunner*)user;
    uint8_t rgba[240 * 160 * 4];
    for (int i = 0; i < 240 * 160; ++i)
    {
        uint32_t c = bgr555[i];
        uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        rgba[i * 4] = uint8_t((r << 3) | (r >> 2));
        rgba[i * 4 + 1] = uint8_t((g << 3) | (g >> 2));
        rgba[i * 4 + 2] = uint8_t((b << 3) | (b >> 2));
        rgba[i * 4 + 3] = 255;
    }
    SYS_LockMutex(self->mLock);
    memcpy(self->mFrame.data(), rgba, sizeof(rgba));
    self->mFrameNew = true;
    SYS_UnlockMutex(self->mLock);
}

int GbaGuestRunner::HostWaitFrame(void* user)
{
    GbaGuestRunner* self = (GbaGuestRunner*)user;
    for (;;)
    {
        if (self->mQuit.load())
        {
            return 1;
        }
        int tokens = self->mFrameTokens.load();
        if (tokens > 0 && self->mFrameTokens.compare_exchange_weak(tokens, tokens - 1))
        {
            return 0;
        }
        SYS_Sleep(1);
    }
}

uint32_t GbaGuestRunner::HostKeys(void* user)
{
    return ((GbaGuestRunner*)user)->mKeys.load();
}

void GbaGuestRunner::HostAudio(void* user, const int16_t* stereo, int frames)
{
    GbaGuestRunner* self = (GbaGuestRunner*)user;
    SYS_LockMutex(self->mLock);
    for (int i = 0; i < frames; ++i)
    {
        if (self->mAudioCount == kAudioRingFrames)
        {
            // the engine is not draining (no streaming audio here): drop the oldest
            self->mAudioHead = (self->mAudioHead + 1) % kAudioRingFrames;
            self->mAudioCount--;
        }
        size_t at = (self->mAudioHead + self->mAudioCount) % kAudioRingFrames;
        self->mAudio[at * 2] = stereo[i * 2];
        self->mAudio[at * 2 + 1] = stereo[i * 2 + 1];
        self->mAudioCount++;
    }
    SYS_UnlockMutex(self->mLock);
}

int GbaGuestRunner::HostLoadSave(void* user, uint8_t* data, int size)
{
    GbaGuestRunner* self = (GbaGuestRunner*)user;
    FILE* f = fopen(self->mSavePath.c_str(), "rb");
    if (f == nullptr)
    {
        return 0;
    }
    int got = int(fread(data, 1, size_t(size), f));
    fclose(f);
    return got;
}

void GbaGuestRunner::HostStoreSave(void* user, const uint8_t* data, int size)
{
    GbaGuestRunner* self = (GbaGuestRunner*)user;
    std::string tmp = self->mSavePath + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (f == nullptr)
    {
        LogWarning("GbaPlayer: cannot write the save %s", self->mSavePath.c_str());
        return;
    }
    fwrite(data, 1, size_t(size), f);
    fclose(f);
    remove(self->mSavePath.c_str());
    rename(tmp.c_str(), self->mSavePath.c_str());
}

int GbaGuestRunner::HostReadData(void* user, uint32_t offset, void* dst, uint32_t size)
{
    GbaGuestRunner* self = (GbaGuestRunner*)user;
    FILE* f = (FILE*)self->mDataFile;
    if (f == nullptr || fseek(f, long(offset), SEEK_SET) != 0)
    {
        return 0;
    }
    return int(fread(dst, 1, size, f));
}

void GbaGuestRunner::HostLog(void* user, const char* line)
{
    (void)user;
    LogDebug("[gba] %s", line);
}
