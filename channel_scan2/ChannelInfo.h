#pragma once
#ifndef CHANNEL_INFO_H
#define CHANNEL_INFO_H

#include <string>
#include <cstdint>

struct ChannelInfo {  // ✅ `class` ではなく `struct` に統一
    uint32_t channelId;
    std::wstring channelName;
    uint32_t networkId;
    uint32_t tsid;
    std::wstring description;

    ChannelInfo()
        : channelId(0), networkId(0), tsid(0), description(L"") {
    }

    ChannelInfo(uint32_t id, std::wstring name, uint32_t netId, uint32_t ts, std::wstring desc)
        : channelId(id), channelName(std::move(name)), networkId(netId), tsid(ts), description(std::move(desc)) {
    }
};

#endif // CHANNEL_INFO_H
