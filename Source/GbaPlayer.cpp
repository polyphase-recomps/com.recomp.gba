/**
 * @file GbaPlayer.cpp
 * @brief Runs a com.recomp.gba game (in-process wasm2c guest, or the native executable as
 *        a child process) and streams its frames to a fullscreen Quad.
 */

#include "GbaPlayer.h"

#include "AgbGuestApi.h"
#include "GbaGuestRunner.h"

#include "AssetManager.h"
#include "Engine.h"
#include "Input/Input.h"
#include "Input/InputTypes.h"
#include "Nodes/Widgets/Quad.h"
#include "Plugins/PolyphaseEngineAPI.h"

#include "../Runtime/include/port_shm.h"

#include <fstream>
#include <sstream>
#include <vector>

#if PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

FORCE_LINK_DEF(GbaPlayer);
DEFINE_NODE(GbaPlayer, Node3D);

PolyphaseEngineAPI* GbaPlayer::sAPI = nullptr;

static std::vector<GbaPlayer*> sLivePlayers;

// GBA KEYINPUT bits (pressed = 1)
enum : unsigned int
{
    KEY_A = 1u << 0,
    KEY_B = 1u << 1,
    KEY_SELECT = 1u << 2,
    KEY_START = 1u << 3,
    KEY_RIGHT = 1u << 4,
    KEY_LEFT = 1u << 5,
    KEY_UP = 1u << 6,
    KEY_DOWN = 1u << 7,
    KEY_R = 1u << 8,
    KEY_L = 1u << 9,
};

GbaPlayer::GbaPlayer()
{
    // Paths left empty come from the game package's game.json; explicit relative
    // paths are resolved against the project directory.
    mGame = "com.recomp.khcom";
}

GbaPlayer::~GbaPlayer()
{
    StopGame();
}

void GbaPlayer::SetEngineAPI(PolyphaseEngineAPI* api)
{
    sAPI = api;
}

void GbaPlayer::ShutdownAll()
{
    std::vector<GbaPlayer*> players = sLivePlayers;
    for (GbaPlayer* player : players)
    {
        player->StopGame();
    }
}

void GbaPlayer::Create()
{
    Node3D::Create();
    SetName("GbaPlayer");
    EnsureDisplayQuad();
    sLivePlayers.push_back(this);
}

void GbaPlayer::Destroy()
{
    StopGame();
    for (size_t i = 0; i < sLivePlayers.size(); ++i)
    {
        if (sLivePlayers[i] == this)
        {
            sLivePlayers.erase(sLivePlayers.begin() + i);
            break;
        }
    }

    // Only touch the quad through the WeakPtr: an auto-created child is already gone here.
    if (Quad* quad = mBoundQuad.Get())
    {
        quad->SetVisible(false);
        quad->SetTexture(nullptr);
    }
    mDisplayQuad = nullptr;
    mBoundQuad = WeakPtr<Quad>();

    mFrameTexture = nullptr;
    Node3D::Destroy();
}

void GbaPlayer::GatherProperties(std::vector<Property>& outProps)
{
    Node3D::GatherProperties(outProps);
    outProps.push_back(Property(DatumType::String, "Game", this, &mGame));
    outProps.push_back(Property(DatumType::String, "Game Executable", this, &mExePath));
    outProps.push_back(Property(DatumType::String, "Save Folder", this, &mSaveDir));
}

void GbaPlayer::SaveStream(Stream& stream, Platform platform)
{
    Node3D::SaveStream(stream, platform);
    stream.WriteString(mGame);
    stream.WriteString(mExePath);
    stream.WriteString(mSaveDir);
}

void GbaPlayer::LoadStream(Stream& stream, Platform platform, uint32_t version)
{
    Node3D::LoadStream(stream, platform, version);
    stream.ReadString(mGame);
    stream.ReadString(mExePath);
    stream.ReadString(mSaveDir);
}

std::string GbaPlayer::ResolvePath(const std::string& path) const
{
    const bool isAbsolute = path.size() > 1 && (path[1] == ':' || path[0] == '/' || path[0] == '\\');
    std::string full = isAbsolute ? path : GetEngineState()->mProjectDirectory + path;
#if PLATFORM_WINDOWS
    for (char& c : full)
    {
        if (c == '/')
        {
            c = '\\';
        }
    }
    // Absolute: the game process runs in its own folder, and packaged builds use a
    // relative project directory.
    char absolute[MAX_PATH];
    DWORD len = GetFullPathNameA(full.c_str(), MAX_PATH, absolute, nullptr);
    if (len > 0 && len < MAX_PATH)
    {
        full = absolute;
    }
#endif
    return full;
}

// Value of a top-level string field in a flat JSON object (game.json); false if absent.
static bool ReadJsonString(const std::string& text, const char* key, std::string& out)
{
    const std::string quoted = std::string("\"") + key + "\"";
    size_t pos = text.find(quoted);
    if (pos == std::string::npos)
    {
        return false;
    }
    pos = text.find(':', pos + quoted.size());
    if (pos == std::string::npos)
    {
        return false;
    }
    pos = text.find('"', pos + 1);
    if (pos == std::string::npos)
    {
        return false;
    }
    out.clear();
    for (++pos; pos < text.size() && text[pos] != '"'; ++pos)
    {
        char c = text[pos];
        if (c == '\\' && pos + 1 < text.size())
        {
            c = text[++pos];
        }
        out.push_back(c);
    }
    return true;
}

void GbaPlayer::ResolveGameDefaults(std::string& exe, std::string& saves, std::string* native) const
{
    exe = mExePath;
    saves = mSaveDir;
    if (mGame.empty())
    {
        return;
    }

    // game.json lives in the package's Assets/ (the packager copies Assets/ loose, so the
    // game executable next to it ships with packaged builds) or, older layout, in the
    // package root. Its exe path is relative to the game.json, saves to the project.
    std::string packageDir = "Packages/" + mGame + "/Assets/";
    std::ifstream file(ResolvePath(packageDir + "game.json"), std::ios::binary);
    if (!file)
    {
        packageDir = "Packages/" + mGame + "/";
        file.open(ResolvePath(packageDir + "game.json"), std::ios::binary);
    }
    if (!file)
    {
        LogWarning("GbaPlayer: no game.json in %s or its Assets folder", ResolvePath(packageDir).c_str());
        return;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();

    std::string value;
    if (exe.empty() && ReadJsonString(text, "exe", value) && !value.empty())
    {
        exe = packageDir + value;
    }
    if (saves.empty() && ReadJsonString(text, "saves", value) && !value.empty())
    {
        saves = value;
    }
    if (native && ReadJsonString(text, "native", value))
    {
        *native = value;
    }
}

// Platform name as used in game.json's "native" list.
static const char* PlatformName()
{
#if PLATFORM_WINDOWS
    return "Windows";
#elif PLATFORM_LINUX
    return "Linux";
#elif PLATFORM_ANDROID
    return "Android";
#elif PLATFORM_MAC
    return "Mac";
#elif PLATFORM_DOLPHIN && defined(HW_RVL)
    return "Wii";
#elif PLATFORM_DOLPHIN
    return "GameCube";
#elif PLATFORM_3DS
    return "3DS";
#else
    return "Other";
#endif
}

static bool ListHas(const std::string& list, const char* item)
{
    size_t pos = 0;
    const std::string needle = item;
    while ((pos = list.find(needle, pos)) != std::string::npos)
    {
        const bool startOk = pos == 0 || list[pos - 1] == ',' || list[pos - 1] == ' ';
        const size_t end = pos + needle.size();
        const bool endOk = end == list.size() || list[end] == ',' || list[end] == ' ';
        if (startOk && endOk)
        {
            return true;
        }
        pos = end;
    }
    return false;
}

// ---- which way to run ------------------------------------------------------------------
bool GbaPlayer::StartGame()
{
    std::string exePath, saveDir, native;
    ResolveGameDefaults(exePath, saveDir, &native);
    if (saveDir.empty())
    {
        saveDir = "Saves/" + (mGame.empty() ? std::string("gba") : mGame);
    }
    // game.json "native": "Windows, Linux" = platforms that use the native executable
    // instead of the portable guest (opt-in).
    const AgbGuestDesc* guest = AgbFindGuest(mGame.c_str());
    if (guest && !ListHas(native, PlatformName()))
    {
        return StartGuest(guest, saveDir);
    }
    return StartProcess();
}

void GbaPlayer::StopGame()
{
    if (mRunner)
    {
        mRunner->Stop();
        mRunner.reset();
    }
    StopProcess();
    mStartAttempted = false;
}

static void CreateDirs(const std::string& path)
{
    for (size_t i = 1; i <= path.size(); ++i)
    {
        if (i == path.size() || path[i] == '/' || path[i] == '\\')
        {
            const std::string part = path.substr(0, i);
            if (!part.empty() && part.back() != ':')
            {
#if PLATFORM_WINDOWS
                CreateDirectoryA(part.c_str(), nullptr);
#else
                mkdir(part.c_str(), 0777);
#endif
            }
        }
    }
}

bool GbaPlayer::StartGuest(const AgbGuestDesc* guest, const std::string& saveDir)
{
    const std::string dataPath = ResolvePath("Packages/" + mGame + "/Assets/" + guest->data_file);
    const std::string saves = ResolvePath(saveDir);
    CreateDirs(saves);
    std::string saveName = guest->data_file;
    saveName = saveName.substr(0, saveName.find_last_of('.')) + ".sav";
    mRunner.reset(new GbaGuestRunner());
    mGuestFrame.assign(240 * 160 * 4, 0);
    mReportedExit = false;
    if (!mRunner->Start(guest, dataPath, saves + "/" + saveName))
    {
        mRunner.reset();
        return false;
    }
    return true;
}

void GbaPlayer::TickGuest(float deltaTime)
{
    if (mRunner->HasStopped())
    {
        if (!mReportedExit)
        {
            mReportedExit = true;
            LogError("GbaPlayer: %s stopped (exit code %d)", mGame.c_str(), mRunner->ExitCode());
        }
        return;
    }
    mRunner->SetKeys(ReadKeys());
    mRunner->Advance(deltaTime);
    if (mRunner->TakeFrame(mGuestFrame.data()))
    {
        UpdateDisplayTexture(mGuestFrame.data(), 240, 160);
    }

    int16_t pcm[2048 * 2];
    int frames = mRunner->TakeAudio(pcm, 2048);
    if (frames > 0 && sAPI && sAPI->Audio_OpenStream)
    {
        if (mAudioStream == 0)
        {
            // 0 means this platform has no streaming audio: the game then runs silent
            mAudioStream = sAPI->Audio_OpenStream(44100, 2, 16);
        }
        if (mAudioStream != 0)
        {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            // the stream API takes little-endian PCM on every platform (Wii: VOICE_*_16BIT_LE)
            for (int i = 0; i < frames * 2; ++i)
            {
                pcm[i] = int16_t(__builtin_bswap16(uint16_t(pcm[i])));
            }
#endif
            sAPI->Audio_SubmitStreamBuffer(mAudioStream, (const uint8_t*)pcm, uint32_t(frames) * 4);
        }
    }
}

// ---- native executable as a child process (Windows) ----------------------------------------
#if PLATFORM_WINDOWS

bool GbaPlayer::StartProcess()
{
    std::string exePath, saveDir;
    ResolveGameDefaults(exePath, saveDir);
    if (exePath.empty())
    {
        LogError("GbaPlayer: no executable (set Game to a package with a game.json, or set Game Executable)");
        return false;
    }
    if (saveDir.empty())
    {
        saveDir = "Saves/" + (mGame.empty() ? std::string("gba") : mGame);
    }

    char name[96];
    static LONG sCounter = 0;
    snprintf(name, sizeof(name), "Local\\GbaRecompPort_%lu_%ld", GetCurrentProcessId(), InterlockedIncrement(&sCounter));

    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)sizeof(PortShm), name);
    if (mapping == nullptr)
    {
        LogError("GbaPlayer: CreateFileMapping failed (%lu)", GetLastError());
        return false;
    }
    PortShm* shm = (PortShm*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PortShm));
    if (shm == nullptr)
    {
        CloseHandle(mapping);
        LogError("GbaPlayer: MapViewOfFile failed (%lu)", GetLastError());
        return false;
    }
    memset((void*)shm, 0, offsetof(PortShm, frames));

    const std::string exe = ResolvePath(exePath);
    const std::string saves = ResolvePath(saveDir);
    mLogPath = saves + "\\game.log";
    // Create every missing level of the save folder.
    for (size_t i = 3; i <= saves.size(); ++i)
    {
        if (i == saves.size() || saves[i] == '\\')
        {
            CreateDirectoryA(saves.substr(0, i).c_str(), nullptr);
        }
    }

    std::string cmd = "\"" + exe + "\" --shm " + name;
    cmd += " --saves \"" + saves + "\" --log \"" + mLogPath + "\"";
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);

    std::string workDir = exe.substr(0, exe.find_last_of('\\'));

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(exe.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        nullptr, workDir.c_str(), &si, &pi))
    {
        LogError("GbaPlayer: could not start '%s' (%lu). Build it with Packages/%s/Native/build.ps1.", exe.c_str(),
                 GetLastError(), mGame.c_str());
        UnmapViewOfFile(shm);
        CloseHandle(mapping);
        return false;
    }

    // The game process must not outlive the editor.
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    mProcess = pi.hProcess;
    mJob = job;
    mMapping = mapping;
    mShm = shm;
    mLastSerial = 0;
    mReportedExit = false;
    LogDebug("GbaPlayer: started %s", cmd.c_str());
    return true;
}

void GbaPlayer::StopProcess()
{
    if (mShm)
    {
        mShm->command = PORT_SHM_CMD_QUIT;
    }
    if (mProcess)
    {
        if (WaitForSingleObject((HANDLE)mProcess, 500) != WAIT_OBJECT_0)
        {
            TerminateProcess((HANDLE)mProcess, 0);
        }
        CloseHandle((HANDLE)mProcess);
        mProcess = nullptr;
    }
    if (mJob)
    {
        CloseHandle((HANDLE)mJob);
        mJob = nullptr;
    }
    if (mShm)
    {
        UnmapViewOfFile(mShm);
        mShm = nullptr;
    }
    if (mMapping)
    {
        CloseHandle((HANDLE)mMapping);
        mMapping = nullptr;
    }
}

bool GbaPlayer::HasProcessExited() const
{
    return mProcess && WaitForSingleObject((HANDLE)mProcess, 0) == WAIT_OBJECT_0;
}

#else

// The native executable is a Windows program; elsewhere the game runs as the in-process
// guest (build the game package with Native/build.ps1 -Guest wasm).
bool GbaPlayer::StartProcess()
{
    LogWarning("GbaPlayer: %s has no in-process guest in this build (Packages/%s/Native/build.ps1 -Guest wasm)",
               mGame.c_str(), mGame.c_str());
    return false;
}

void GbaPlayer::StopProcess()
{
}

bool GbaPlayer::HasProcessExited() const
{
    return false;
}

#endif

unsigned int GbaPlayer::ReadKeys() const
{
    unsigned int bits = 0;

    if (sAPI && sAPI->IsKeyDown)
    {
        auto down = [](int32_t key) { return sAPI->IsKeyDown(key); };

        if (down(POLYPHASE_KEY_UP))        bits |= KEY_UP;
        if (down(POLYPHASE_KEY_DOWN))      bits |= KEY_DOWN;
        if (down(POLYPHASE_KEY_LEFT))      bits |= KEY_LEFT;
        if (down(POLYPHASE_KEY_RIGHT))     bits |= KEY_RIGHT;
        if (down(POLYPHASE_KEY_X))         bits |= KEY_A;
        if (down(POLYPHASE_KEY_Z))         bits |= KEY_B;
        if (down(POLYPHASE_KEY_A))         bits |= KEY_L;
        if (down(POLYPHASE_KEY_S))         bits |= KEY_R;
        if (down(POLYPHASE_KEY_Q))         bits |= KEY_L;
        if (down(POLYPHASE_KEY_W))         bits |= KEY_R;
        if (down(POLYPHASE_KEY_ENTER))     bits |= KEY_START;
        if (down(POLYPHASE_KEY_BACKSPACE)) bits |= KEY_SELECT;
    }

    const int32_t gamepad = 0;
    if (INP_IsGamepadConnected(gamepad) && INP_GetGamepadType(gamepad) == GamepadType::Wiimote)
    {
        // The engine reports a Wiimote as if held upright (A, B, 1 = X, 2 = Y, d-pad as
        // printed). A bare Wiimote plays a GBA game sideways (d-pad on the left), like the
        // Virtual Console: 2 = A, 1 = B, + = Start, - = Select, B trigger = L, A = R.
        // Once a Nunchuk is used: stick or d-pad to move, A = A, B = B, Z = L, C = R.
        auto pdown = [gamepad](int32_t button) { return INP_IsGamepadButtonDown(button, gamepad); };
        const float lx = INP_GetGamepadAxisValue(GAMEPAD_AXIS_LTHUMB_X, gamepad);
        const float ly = INP_GetGamepadAxisValue(GAMEPAD_AXIS_LTHUMB_Y, gamepad);

        if (pdown(GAMEPAD_Z) || pdown(GAMEPAD_C) || lx > 0.5f || lx < -0.5f || ly > 0.5f || ly < -0.5f)
        {
            mNunchukSeen = true;
        }

        if (mNunchukSeen)
        {
            if (pdown(GAMEPAD_UP) || ly > 0.5f)    bits |= KEY_UP;
            if (pdown(GAMEPAD_DOWN) || ly < -0.5f) bits |= KEY_DOWN;
            if (pdown(GAMEPAD_LEFT) || lx < -0.5f) bits |= KEY_LEFT;
            if (pdown(GAMEPAD_RIGHT) || lx > 0.5f) bits |= KEY_RIGHT;
            if (pdown(GAMEPAD_A)) bits |= KEY_A;
            if (pdown(GAMEPAD_B)) bits |= KEY_B;
            if (pdown(GAMEPAD_Z)) bits |= KEY_L;
            if (pdown(GAMEPAD_C)) bits |= KEY_R;
            if (pdown(GAMEPAD_X)) bits |= KEY_L; /* 1 */
            if (pdown(GAMEPAD_Y)) bits |= KEY_R; /* 2 */
        }
        else
        {
            if (pdown(GAMEPAD_RIGHT)) bits |= KEY_UP;
            if (pdown(GAMEPAD_LEFT))  bits |= KEY_DOWN;
            if (pdown(GAMEPAD_UP))    bits |= KEY_LEFT;
            if (pdown(GAMEPAD_DOWN))  bits |= KEY_RIGHT;
            if (pdown(GAMEPAD_Y)) bits |= KEY_A; /* 2 */
            if (pdown(GAMEPAD_X)) bits |= KEY_B; /* 1 */
            if (pdown(GAMEPAD_B)) bits |= KEY_L;
            if (pdown(GAMEPAD_A)) bits |= KEY_R;
        }
        if (pdown(GAMEPAD_START))  bits |= KEY_START;
        if (pdown(GAMEPAD_SELECT)) bits |= KEY_SELECT;
    }
    else if (INP_IsGamepadConnected(gamepad))
    {
        auto pdown = [gamepad](int32_t button) { return INP_IsGamepadButtonDown(button, gamepad); };
        const float lx = INP_GetGamepadAxisValue(GAMEPAD_AXIS_LTHUMB_X, gamepad);
        const float ly = INP_GetGamepadAxisValue(GAMEPAD_AXIS_LTHUMB_Y, gamepad);

        if (pdown(GAMEPAD_UP) || ly > 0.5f)    bits |= KEY_UP;
        if (pdown(GAMEPAD_DOWN) || ly < -0.5f) bits |= KEY_DOWN;
        if (pdown(GAMEPAD_LEFT) || lx < -0.5f) bits |= KEY_LEFT;
        if (pdown(GAMEPAD_RIGHT) || lx > 0.5f) bits |= KEY_RIGHT;
        if (pdown(GAMEPAD_A))      bits |= KEY_A;
        if (pdown(GAMEPAD_B))      bits |= KEY_B;
        if (pdown(GAMEPAD_X))      bits |= KEY_B;
        if (pdown(GAMEPAD_L1) || INP_GetGamepadAxisValue(GAMEPAD_AXIS_LTRIGGER, gamepad) > 0.3f) bits |= KEY_L;
        if (pdown(GAMEPAD_R1) || INP_GetGamepadAxisValue(GAMEPAD_AXIS_RTRIGGER, gamepad) > 0.3f) bits |= KEY_R;
        if (pdown(GAMEPAD_START))  bits |= KEY_START;
        if (pdown(GAMEPAD_SELECT)) bits |= KEY_SELECT;
    }
    return bits;
}

void GbaPlayer::EnsureDisplayQuad()
{
    if (mDisplayQuad != nullptr)
    {
        return;
    }
    mDisplayQuad = CreateChild<Quad>("GBA Display");
    mDisplayQuad->SetAnchorMode(AnchorMode::FullStretch);
    mDisplayQuad->SetSize(1.0f, 1.0f);
    mDisplayQuad->SetObjectFit(ObjectFit::Fill);
    // Hidden until the first game frame is bound: a Quad without a texture falls back to the
    // engine's white texture, which console packages do not have (null read: Dolphin shrugs,
    // a real Wii/GameCube takes a DSI exception in GX_LoadTexObj).
    mDisplayQuad->SetVisible(false);
    mBoundQuad = ResolveWeakPtr<Quad>(mDisplayQuad);
}

void GbaPlayer::UpdateDisplayTexture(const uint8_t* pixels, unsigned int width, unsigned int height)
{
    EnsureDisplayQuad();

    Texture* texture = mFrameTexture.Get<Texture>();
    if (texture == nullptr || texture->GetWidth() != width || texture->GetHeight() != height)
    {
        // Must be a transient asset: Quad drops textures the AssetManager does not know.
        texture = NewTransientAsset<Texture>();
        texture->SetName("T_GbaFrame");
        texture->SetMipmapped(false);
        texture->SetFilterType(FilterType::Nearest);
        texture->SetWrapMode(WrapMode::Clamp);
        texture->Init(width, height, (uint8_t*)pixels);
        texture->Create();
        mFrameTexture = texture;
    }

    if (Quad* quad = mBoundQuad.Get())
    {
        quad->SetTexture(texture);
        quad->SetVisible(true);
    }
    texture->UpdatePixels(pixels, size_t(width) * size_t(height) * 4);
}

void GbaPlayer::Tick(float deltaTime)
{
    Node3D::Tick(deltaTime);

    if (!mStartAttempted)
    {
        mStartAttempted = true;
        StartGame();
    }
    if (mRunner)
    {
        TickGuest(deltaTime);
    }
    else if (mShm)
    {
        TickProcess();
    }
}

void GbaPlayer::TickProcess()
{
    if (mShm->status == PORT_SHM_STATUS_CRASHED || mShm->status == PORT_SHM_STATUS_EXITED || HasProcessExited())
    {
        if (!mReportedExit)
        {
            mReportedExit = true;
            LogError("GbaPlayer: the game process %s (log: %s)",
                     mShm->status == PORT_SHM_STATUS_CRASHED ? "crashed" : "exited", mLogPath.c_str());
        }
        return;
    }

    mShm->pad = ReadKeys();

    const unsigned int serial = mShm->frame_serial;
    if (serial != mLastSerial)
    {
        mLastSerial = serial;
        const unsigned int slot = mShm->frame_index & 1;
        const unsigned int width = mShm->width[slot];
        const unsigned int height = mShm->height[slot];
        if (width != 0 && height != 0 && width <= PORT_SHM_MAX_W && height <= PORT_SHM_MAX_H)
        {
            UpdateDisplayTexture(mShm->frames[slot], width, height);
        }
    }
}
