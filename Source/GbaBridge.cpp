/**
 * @file GbaBridge.cpp
 * @brief Host side of the GBA script bridge (see GbaBridge.h).
 */

#include "GbaBridge.h"

#include "System/System.h"

#include "../Runtime/include/agb_bridge.h"
#include "../Runtime/include/port_shm.h"

#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>

#if PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
struct Call
{
    int id;
    std::string name;
    std::vector<int> args;
};

struct State
{
    MutexObject* lock = nullptr;
    bool published = false;
    std::vector<GbaBridge::Var> vars;
    std::vector<GbaBridge::RequestInfo> requests;
    std::vector<uint8_t> values;
    std::deque<Call> queue;
    std::map<int, int> results;
    int nextId = 1;
    // shared memory: what was copied last
    unsigned shmDescSeq = 0;
    unsigned shmResRead = 0;
    PortShm* shm = nullptr;
};

State& S()
{
    static State sState;
    if (sState.lock == nullptr)
    {
        sState.lock = SYS_CreateMutex();
    }
    return sState;
}

struct Lock
{
    Lock() { SYS_LockMutex(S().lock); }
    ~Lock() { SYS_UnlockMutex(S().lock); }
};

int ElementSize(int type)
{
    switch (type)
    {
    case AGB_VAR_U8:
    case AGB_VAR_S8:
    case AGB_VAR_STR: return 1;
    case AGB_VAR_U16:
    case AGB_VAR_S16: return 2;
    default: return 4;
    }
}

// "name\ttype\tcount\tstride\thelp\n" lines
void ParseVars(const char* text, std::vector<GbaBridge::Var>& out)
{
    out.clear();
    uint32_t offset = 0;
    const char* p = text;
    while (p && *p)
    {
        const char* end = strchr(p, '\n');
        const std::string line(p, end ? end - p : strlen(p));
        p = end ? end + 1 : nullptr;
        std::vector<std::string> fields;
        size_t start = 0;
        for (int i = 0; i < 4; ++i)
        {
            const size_t tab = line.find('\t', start);
            if (tab == std::string::npos) break;
            fields.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        if (fields.size() < 4) continue;
        GbaBridge::Var v;
        v.name = fields[0];
        v.type = atoi(fields[1].c_str());
        v.count = atoi(fields[2].c_str());
        v.stride = atoi(fields[3].c_str());
        v.help = line.substr(start);
        v.offset = offset;
        v.bytes = (uint32_t)(v.type == AGB_VAR_STR ? (v.stride ? v.count * v.stride : v.count)
                                                   : v.count * ElementSize(v.type));
        offset += v.bytes;
        out.push_back(v);
    }
}

void ParseRequests(const char* text, std::vector<GbaBridge::RequestInfo>& out)
{
    out.clear();
    const char* p = text;
    while (p && *p)
    {
        const char* end = strchr(p, '\n');
        const std::string line(p, end ? end - p : strlen(p));
        p = end ? end + 1 : nullptr;
        const size_t tab = line.find('\t');
        if (line.empty()) continue;
        out.push_back({line.substr(0, tab), tab == std::string::npos ? std::string() : line.substr(tab + 1)});
    }
}

const GbaBridge::Var* Find(const std::string& name)
{
    for (const GbaBridge::Var& v : S().vars)
    {
        if (v.name == name) return &v;
    }
    return nullptr;
}
}

void GbaBridge::Reset()
{
    Lock lock;
    State& s = S();
    s.published = false;
    s.vars.clear();
    s.requests.clear();
    s.values.clear();
    s.queue.clear();
    s.results.clear();
    s.shmDescSeq = 0;
    s.shmResRead = 0;
    s.shm = nullptr;
}

void GbaBridge::Publish(const char* vars, const char* requests)
{
    Lock lock;
    ParseVars(vars, S().vars);
    ParseRequests(requests, S().requests);
    S().published = true;
}

void GbaBridge::SetValues(const void* data, uint32_t size)
{
    Lock lock;
    State& s = S();
    s.values.assign((const uint8_t*)data, (const uint8_t*)data + size);
}

int GbaBridge::Poll(char* name, uint32_t nameCap, int* args, int maxArgs, int* nargs)
{
    Lock lock;
    State& s = S();
    if (s.queue.empty())
    {
        return 0;
    }
    const Call call = s.queue.front();
    s.queue.pop_front();
    snprintf(name, nameCap, "%s", call.name.c_str());
    *nargs = (int)call.args.size() < maxArgs ? (int)call.args.size() : maxArgs;
    for (int i = 0; i < *nargs; ++i) args[i] = call.args[i];
    return call.id;
}

void GbaBridge::Done(int id, int result)
{
    Lock lock;
    State& s = S();
    s.results[id] = result;
    while (s.results.size() > 64) s.results.erase(s.results.begin());
}

void GbaBridge::SyncShm(PortShm* shm)
{
    if (shm == nullptr) return;
    State& s = S();
    if (s.shm != shm)
    {
        Reset();
        S().shm = shm;
    }
    // descriptions (rare): copy when the sequence moved and isn't mid-write
    const unsigned descSeq = shm->bridge_desc_seq;
    if (descSeq != s.shmDescSeq && (descSeq & 1) == 0)
    {
        std::string vars(shm->bridge_vars, strnlen(shm->bridge_vars, sizeof(shm->bridge_vars)));
        std::string reqs(shm->bridge_reqs, strnlen(shm->bridge_reqs, sizeof(shm->bridge_reqs)));
        if (shm->bridge_desc_seq == descSeq)
        {
            s.shmDescSeq = descSeq;
            Publish(vars.c_str(), reqs.c_str());
        }
    }
    // values: a consistent copy (retry while the game writes)
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const unsigned seq = shm->bridge_values_seq;
        if (seq & 1) continue;
        const unsigned size = shm->bridge_values_size;
        if (size > sizeof(shm->bridge_values)) break;
        std::vector<uint8_t> copy(shm->bridge_values, shm->bridge_values + size);
#if PLATFORM_WINDOWS
        MemoryBarrier();
#endif
        if (shm->bridge_values_seq == seq)
        {
            Lock lock;
            s.values.swap(copy);
            break;
        }
    }
    // our queued requests -> the shared ring
    {
        Lock lock;
        while (!s.queue.empty() && shm->bridge_req_write - shm->bridge_req_read < PORT_SHM_BRIDGE_QUEUE)
        {
            const Call& call = s.queue.front();
            PortShmBridgeCall& slot = shm->bridge_req[shm->bridge_req_write % PORT_SHM_BRIDGE_QUEUE];
            slot.id = (unsigned)call.id;
            slot.nargs = (int)call.args.size() < 8 ? (int)call.args.size() : 8;
            for (int i = 0; i < slot.nargs; ++i) slot.args[i] = call.args[i];
            snprintf(slot.name, sizeof(slot.name), "%s", call.name.c_str());
#if PLATFORM_WINDOWS
            MemoryBarrier();
#endif
            shm->bridge_req_write = shm->bridge_req_write + 1;
            s.queue.pop_front();
        }
    }
    // results
    const unsigned resWrite = shm->bridge_res_write;
    if (resWrite - s.shmResRead > PORT_SHM_BRIDGE_RESULTS) s.shmResRead = resWrite - PORT_SHM_BRIDGE_RESULTS;
    while (s.shmResRead != resWrite)
    {
        const PortShmBridgeResult r = shm->bridge_res[s.shmResRead % PORT_SHM_BRIDGE_RESULTS];
        Done((int)r.id, r.result);
        ++s.shmResRead;
    }
}

bool GbaBridge::IsPublished()
{
    Lock lock;
    return S().published;
}

std::vector<GbaBridge::Var> GbaBridge::Variables()
{
    Lock lock;
    return S().vars;
}

std::vector<GbaBridge::RequestInfo> GbaBridge::Requests()
{
    Lock lock;
    return S().requests;
}

// Values are little-endian (the game's own byte order: x86 or the wasm guest).
bool GbaBridge::GetNumber(const std::string& name, int index, double& value)
{
    Lock lock;
    const Var* v = Find(name);
    if (v == nullptr || v->type == AGB_VAR_STR || index < 0 || index >= v->count) return false;
    const int elem = ElementSize(v->type);
    const uint32_t at = v->offset + (uint32_t)(index * elem);
    if (at + elem > S().values.size()) return false;
    const uint8_t* b = S().values.data() + at;
    uint32_t raw = 0;
    for (int i = elem - 1; i >= 0; --i) raw = (raw << 8) | b[i];
    switch (v->type)
    {
    case AGB_VAR_U8: value = (uint8_t)raw; break;
    case AGB_VAR_S8: value = (int8_t)raw; break;
    case AGB_VAR_U16: value = (uint16_t)raw; break;
    case AGB_VAR_S16: value = (int16_t)raw; break;
    case AGB_VAR_U32: value = raw; break;
    default: value = (int32_t)raw; break;
    }
    return true;
}

bool GbaBridge::GetString(const std::string& name, int index, std::string& value)
{
    Lock lock;
    const Var* v = Find(name);
    if (v == nullptr || v->type != AGB_VAR_STR) return false;
    const uint32_t len = v->stride ? (uint32_t)v->stride : (uint32_t)v->count;
    const uint32_t at = v->offset + (v->stride ? (uint32_t)index * len : 0);
    if ((v->stride && index >= v->count) || (!v->stride && index != 0) || at + len > S().values.size()) return false;
    const char* p = (const char*)S().values.data() + at;
    value.assign(p, strnlen(p, len));
    return true;
}

int GbaBridge::Request(const std::string& name, const std::vector<int>& args)
{
    Lock lock;
    State& s = S();
    if (s.queue.size() >= 64) return 0;
    const int id = s.nextId++;
    if (s.nextId <= 0) s.nextId = 1;
    s.queue.push_back({id, name, args});
    return id;
}

bool GbaBridge::Result(int id, int& result)
{
    Lock lock;
    auto it = S().results.find(id);
    if (it == S().results.end()) return false;
    result = it->second;
    return true;
}
