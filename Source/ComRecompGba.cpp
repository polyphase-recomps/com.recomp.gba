/**
 * @file ComRecompGba.cpp
 * @brief Native addon: com.recomp.gba
 *
 * Shared runtime for GBA games compiled natively from their decompilations (see
 * ../Runtime). Exposes the GbaPlayer node; each game package (e.g. com.recomp.khcom)
 * only carries its build config, glue, patches and game.json.
 */

#include "Plugins/PolyphasePluginAPI.h"
#include "Plugins/PolyphaseEngineAPI.h"

#include "GbaPlayer.h"

static PolyphaseEngineAPI* sEngineAPI = nullptr;

static int OnLoad(PolyphaseEngineAPI* api)
{
    sEngineAPI = api;
    GbaPlayer::SetEngineAPI(api);
    FORCE_LINK_CALL(GbaPlayer);
    if (api && api->LogDebug)
    {
        api->LogDebug("com.recomp.gba loaded!");
    }
    return 0;
}

static void OnUnload()
{
    // Game processes are driven by this module's node instances: stop them first.
    GbaPlayer::ShutdownAll();
    GbaPlayer::SetEngineAPI(nullptr);
    if (sEngineAPI && sEngineAPI->LogDebug)
    {
        sEngineAPI->LogDebug("com.recomp.gba unloaded.");
    }
    sEngineAPI = nullptr;
}

static void RegisterTypes(void* nodeFactory)
{
    // Register custom node types here
    // Example: REGISTER_NODE(MyCustomNode);
}

static void RegisterScriptFuncs(lua_State* L)
{
    // Register Lua functions here
    // Use L to interact with Lua state
    (void)L; // Suppress unused parameter warning
}

#if EDITOR
static void RegisterEditorUI(EditorUIHooks* hooks, uint64_t hookId)
{
    // Register editor UI extensions here
    // Example:
    // hooks->AddMenuItem(hookId, "Developer", "com.recomp.gba Tool",
    //     [](void*) { /* do something */ }, nullptr, nullptr);
}
#endif

static int FillDesc(PolyphasePluginDesc* desc)
{
    desc->apiVersion = OCTAVE_PLUGIN_API_VERSION;
    desc->pluginName = "com.recomp.gba";
    desc->pluginVersion = "1.0.0";
    desc->OnLoad = OnLoad;
    desc->OnUnload = OnUnload;
    desc->RegisterTypes = RegisterTypes;
    desc->RegisterScriptFuncs = RegisterScriptFuncs;
#if EDITOR
    desc->RegisterEditorUI = RegisterEditorUI;
#else
    desc->RegisterEditorUI = nullptr;
#endif
    desc->OnEditorPreInit = nullptr;
    desc->OnEditorReady = nullptr;
    return 0;
}

#if EDITOR
extern "C" OCTAVE_PLUGIN_API int PolyphasePlugin_GetDesc(PolyphasePluginDesc* desc)
{
    return FillDesc(desc);
}
#else
extern "C" int PolyphasePlugin_GetDesc_com_recomp_gba(PolyphasePluginDesc* desc)
{
    return FillDesc(desc);
}
#endif
