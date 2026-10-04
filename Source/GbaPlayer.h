/**
 * @file GbaPlayer.h
 * @brief Node that runs a natively compiled GBA game (com.recomp.gba runtime) and shows
 *        its frames on a Quad.
 *
 * The game is chosen by package id (Game property, e.g. "com.recomp.khcom"); that
 * package's game.json gives the executable and save folder, and the explicit properties
 * override them when set.
 *
 * Two ways to run the game:
 *  - in-process, when the game package is built as a wasm2c guest (AgbGuestApi.h): the
 *    default wherever the guest is linked in, i.e. packaged builds on every platform;
 *  - the native 32-bit Windows executable as a child process, talking through shared
 *    memory (frames back as RGBA, KEYINPUT bits out; the child plays its own audio): in
 *    the editor, and on the platforms the game's game.json lists under "native".
 */
#pragma once

#include "AssetRef.h"
#include "Nodes/3D/Node3D.h"

#include <memory>
#include <string>
#include <vector>

struct PolyphaseEngineAPI;
struct PortShm;
struct AgbGuestDesc;
class GbaGuestRunner;

class GbaPlayer : public Node3D
{
public:
    DECLARE_NODE(GbaPlayer, Node3D);

    GbaPlayer();
    virtual ~GbaPlayer();

    virtual void Create() override;
    virtual void Destroy() override;
    virtual void Tick(float deltaTime) override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;

    virtual void SaveStream(Stream& stream, Platform platform) override;
    virtual void LoadStream(Stream& stream, Platform platform, uint32_t version) override;

    static void SetEngineAPI(PolyphaseEngineAPI* api);
    // Stops every running game process; called when the addon unloads.
    static void ShutdownAll();

private:
    bool StartGame();
    void StopGame();
    bool StartProcess();
    void StopProcess();
    bool StartGuest(const AgbGuestDesc* guest, const std::string& saveDir);
    void TickProcess();
    void TickGuest(float deltaTime);
    bool HasProcessExited() const;
    unsigned int ReadKeys() const;
    void EnsureDisplayQuad();
    void UpdateDisplayTexture(const uint8_t* rgba, unsigned int width, unsigned int height);
    std::string ResolvePath(const std::string& path) const;
    // Fills the empty path properties from the game package's game.json; native = its
    // "native" platform list.
    void ResolveGameDefaults(std::string& exe, std::string& saves, std::string* native = nullptr) const;

    static PolyphaseEngineAPI* sAPI;

    class Quad* mDisplayQuad = nullptr;
    WeakPtr<class Quad> mBoundQuad;
    // A Nunchuk was used on the Wiimote (its stick or Z/C): switch from the sideways layout
    mutable bool mNunchukSeen = false;
    // Transient texture owned by the AssetManager; this ref keeps it alive.
    AssetRef mFrameTexture;

    std::string mGame;
    std::string mExePath;
    std::string mSaveDir;
    std::string mLogPath;

    void* mProcess = nullptr;   // HANDLE
    void* mJob = nullptr;       // HANDLE, kills the game with the editor
    void* mMapping = nullptr;   // HANDLE
    PortShm* mShm = nullptr;
    std::unique_ptr<GbaGuestRunner> mRunner;
    std::vector<uint8_t> mGuestFrame;
    uint32_t mAudioStream = 0;
    unsigned int mLastSerial = 0;
    bool mStartAttempted = false;
    bool mReportedExit = false;
};
