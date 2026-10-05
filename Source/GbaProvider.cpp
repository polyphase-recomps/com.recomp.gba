/**
 * @file GbaProvider.cpp
 * @brief The GBA runtime's RecompProvider (see GbaProvider.h).
 */

#include "GbaProvider.h"

#include "GbaBridge.h"

#include "../Runtime/include/agb_bridge.h"

#include <cmath>

namespace
{
RecompType ToRecompType(int type)
{
    switch (type)
    {
    case AGB_VAR_U8: return RecompType::U8;
    case AGB_VAR_S8: return RecompType::S8;
    case AGB_VAR_U16: return RecompType::U16;
    case AGB_VAR_S16: return RecompType::S16;
    case AGB_VAR_U32: return RecompType::U32;
    case AGB_VAR_STR: return RecompType::Str;
    default: return RecompType::S32;
    }
}
}

GbaProvider& GbaProvider::Get()
{
    static GbaProvider sProvider;
    return sProvider;
}

bool GbaProvider::IsLive() const
{
    return mRunning;
}

void GbaProvider::Variables(std::vector<RecompVarInfo>& out) const
{
    for (const GbaBridge::Var& v : GbaBridge::Variables())
    {
        RecompVarInfo info;
        info.name = v.name;
        info.help = v.help;
        info.type = ToRecompType(v.type);
        info.count = v.count;
        info.writable = v.type != AGB_VAR_STR;
        out.push_back(info);
    }
}

void GbaProvider::Requests(std::vector<RecompRequestInfo>& out) const
{
    for (const GbaBridge::RequestInfo& r : GbaBridge::Requests())
    {
        out.push_back({r.name, r.help});
    }
}

bool GbaProvider::Get(const std::string& name, int index, RecompValue& out)
{
    double number = 0;
    if (GbaBridge::GetNumber(name, index, number))
    {
        out = RecompValue::Number(number);
        return true;
    }
    std::string text;
    if (GbaBridge::GetString(name, index, text))
    {
        out = RecompValue::Text(text);
        return true;
    }
    return false;
}

bool GbaProvider::Set(const std::string& name, int index, const RecompValue& value)
{
    if (!mRunning || value.isText)
    {
        return false;
    }
    return GbaBridge::Request("set " + name, {(int)std::lround(value.number), index}) > 0;
}

int GbaProvider::Request(const std::string& name, const std::vector<int>& args)
{
    return mRunning ? GbaBridge::Request(name, args) : 0;
}

bool GbaProvider::Result(int id, int& result)
{
    return GbaBridge::Result(id, result);
}

RecompFrameInfo GbaProvider::FrameInfo() const
{
    RecompFrameInfo info;
    info.width = 240;
    info.height = 160;
    info.displayAspect = 3.0f / 2.0f;
    return info;
}
