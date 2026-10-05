/**
 * @file GbaBridge.h
 * @brief Host side of the GBA script bridge (Runtime/include/agb_bridge.h): what the
 *        running game published, the latest copy of its variables, and the requests
 *        queued for it. Fed by the game, whichever way it runs:
 *          in-process (packaged builds): GbaGuestRunner's AgbHostApi bridge callbacks
 *          child process (editor):       GbaPlayer::SyncBridge from the shared memory
 *        Read by GbaProvider (com.recomp.mod.base) and Lua.
 *
 * Thread-safe: the game thread publishes and polls while the main thread reads.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct PortShm;

namespace GbaBridge
{
struct Var
{
    std::string name;
    std::string help;
    int type = 0;   // AGB_VAR_*
    int count = 1;
    int stride = 0;
    uint32_t offset = 0; // in the values block
    uint32_t bytes = 0;
};

struct RequestInfo
{
    std::string name;
    std::string help;
};

// A new game (or none): forgets everything.
void Reset();

// ---- game side -----------------------------------------------------------------------
void Publish(const char* vars, const char* requests);
void SetValues(const void* data, uint32_t size);
// The next queued request: id > 0 with its name and arguments, or 0.
int Poll(char* name, uint32_t nameCap, int* args, int maxArgs, int* nargs);
void Done(int id, int result);

// Child-process mode: once per tick, exchanges all of the above through the shared memory.
void SyncShm(PortShm* shm);

// ---- host side -----------------------------------------------------------------------
bool IsPublished();
std::vector<Var> Variables();
std::vector<RequestInfo> Requests();
// A variable's element as a number (false for strings / unknown / out of range).
bool GetNumber(const std::string& name, int index, double& value);
bool GetString(const std::string& name, int index, std::string& value);
// Queues a request (built-in "set <name>" value[, index] writes a variable): id or 0.
int Request(const std::string& name, const std::vector<int>& args);
bool Result(int id, int& result);
}
