/*
 * Script bridge: what a game publishes to Polyphase (com.recomp.mod.base mod settings,
 * Lua Recomp.* / Gba.*, the editor's Mods windows). Same shape as the PS1 and N64
 * runtimes' port_bridge.h:
 *
 *   static const AgbBridgeVar kVars[] = {
 *       { "hp", &gGameState.hp, AGB_VAR_S16, 1, 0, "current HP" },
 *   };
 *   static int heal(const int *args, int nargs) { ...; return 0; }
 *   static const AgbBridgeRequest kRequests[] = { { "heal", heal, "restore HP" } };
 *   ... agb_bridge_add(kVars, 1, kRequests, 1);   (once, e.g. from the game's glue)
 *
 * The runtime pumps the bridge once per frame (agb_frame.c), on the game's thread:
 * requests from the host run there, between two frames, including the built-in
 * "set <name>" value[, index] that writes a variable. Variables go to the host as a copy
 * of their bytes each frame (little-endian, as the game sees them), so the host never
 * touches game memory itself - the same in the editor (the game is a child process) and
 * in packaged builds (wasm2c guest in-process).
 *
 * Types: element size 1/2/4 bytes; AGB_VAR_STR is `count` strings of `stride` bytes
 * (stride 0: one string of `count` bytes). Arrays: `count` elements `stride` bytes apart
 * (0 = packed).
 */
#ifndef AGB_BRIDGE_H
#define AGB_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    AGB_VAR_U8 = 1,
    AGB_VAR_S8,
    AGB_VAR_U16,
    AGB_VAR_S16,
    AGB_VAR_U32,
    AGB_VAR_S32,
    AGB_VAR_STR
};

#define AGB_BRIDGE_RESULT_UNKNOWN (-1000)  /* no such request */
#define AGB_BRIDGE_RESULT_BAD_ARGS (-1001)
#define AGB_BRIDGE_MAX_ARGS 8

typedef struct AgbBridgeVar
{
    const char *name;
    void *addr;
    int type;
    int count;
    int stride;
    const char *help;
} AgbBridgeVar;

typedef int (*AgbBridgeFn)(const int *args, int nargs);

typedef struct AgbBridgeRequest
{
    const char *name;
    AgbBridgeFn fn;
    const char *help;
} AgbBridgeRequest;

/* Adds a table of variables and requests (up to 16 tables; mods add their own). */
void agb_bridge_add(const AgbBridgeVar *vars, int nvars, const AgbBridgeRequest *requests, int nrequests);
/* Once per frame, on the game's thread (the runtime calls it). */
void agb_bridge_pump(void);

#ifdef __cplusplus
}
#endif

#endif /* AGB_BRIDGE_H */
