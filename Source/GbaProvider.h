/**
 * @file GbaProvider.h
 * @brief The GBA runtime's RecompProvider (com.recomp.mod.base): mod settings, the
 *        Recomp/Mods Lua tables, Recomp* widgets and the editor's Mods windows work on
 *        the running game through it, in both of GbaPlayer's modes (in-process guest,
 *        or the native executable as a child process in the editor).
 *
 * Variables and requests are what the game publishes on its script bridge
 * (Runtime/include/agb_bridge.h, GbaBridge.h); writes go through the bridge's built-in
 * "set <name>" between two frames.
 */
#pragma once

#include "ModBaseProvider.h"

class GbaProvider : public RecompProvider
{
public:
    static GbaProvider& Get();

    // Set by GbaPlayer.
    void SetGame(const std::string& package) { mGame = package; }
    void SetRunning(bool running) { mRunning = running; }

    const char* RuntimeId() const override { return "gba"; }
    std::string GamePackage() const override { return mGame; }
    bool IsLive() const override;
    void Variables(std::vector<RecompVarInfo>& out) const override;
    void Requests(std::vector<RecompRequestInfo>& out) const override;
    bool Get(const std::string& name, int index, RecompValue& out) override;
    bool Set(const std::string& name, int index, const RecompValue& value) override;
    int Request(const std::string& name, const std::vector<int>& args) override;
    bool Result(int id, int& result) override;
    RecompFrameInfo FrameInfo() const override;

private:
    std::string mGame;
    bool mRunning = false;
};
