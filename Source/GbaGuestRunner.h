/**
 * @file GbaGuestRunner.h
 * @brief Runs a GBA game built as a wasm2c guest (AgbGuestApi.h) in-process, on a thread
 *        of its own, for GbaPlayer: frames, input, audio, saves and the game data file.
 */
#pragma once

#include "AgbGuestApi.h"
#include "System/SystemTypes.h"

#include <atomic>
#include <stdint.h>
#include <string>
#include <vector>


class GbaGuestRunner
{
public:
    GbaGuestRunner();
    ~GbaGuestRunner();

    // Starts the game; dataPath = the guest's data file, savePath = its save file.
    bool Start(const AgbGuestDesc* guest, const std::string& dataPath, const std::string& savePath);
    // Asks the game to stop at its next frame and waits for its thread.
    void Stop();
    bool IsRunning() const { return mThread != nullptr; }
    bool HasStopped() const { return mDone.load(); }
    int ExitCode() const { return mExitCode; }

    // Main thread, once per engine tick.
    void Advance(float deltaTime);              // releases the frames due at 59.73 Hz
    void SetKeys(uint32_t keys) { mKeys.store(keys); }
    bool TakeFrame(uint8_t* rgba240x160);        // the newest frame, if there is one since the last call
    int TakeAudio(int16_t* stereo, int maxFrames);

private:
    static void ThreadMain(void* self);
    void Run();

    // AgbHostApi callbacks (game thread)
    static void HostPresent(void* user, const uint16_t* bgr555);
    static int HostWaitFrame(void* user);
    static uint32_t HostKeys(void* user);
    static void HostAudio(void* user, const int16_t* stereo, int frames);
    static int HostLoadSave(void* user, uint8_t* data, int size);
    static void HostStoreSave(void* user, const uint8_t* data, int size);
    static int HostReadData(void* user, uint32_t offset, void* dst, uint32_t size);
    static void HostLog(void* user, const char* line);

    const AgbGuestDesc* mGuest = nullptr;
    std::string mDataPath;
    std::string mSavePath;
    void* mDataFile = nullptr;  // FILE*

    void* mThread = nullptr;    // platform thread handle
    std::atomic<bool> mQuit{false};
    std::atomic<bool> mDone{false};
    std::atomic<int> mFrameTokens{0};
    std::atomic<uint32_t> mKeys{0};
    int mExitCode = 0;
    double mFrameClock = 0.0;

    MutexObject* mLock = nullptr;
    std::vector<uint8_t> mFrame;      // RGBA, guarded by mLock
    bool mFrameNew = false;
    std::vector<int16_t> mAudio;      // interleaved stereo ring, guarded by mLock
    size_t mAudioHead = 0;
    size_t mAudioCount = 0;           // stereo frames queued
};
