// IBonDriver2.cpp
#include <Windows.h>
#include <tchar.h>
#include <string>
#include "IBonDriver2.h"
#include "SDTFilter.h"
#include "PATFilter.h"
#include "PMTFilter.h"
#include "ChannelInfo.h"
#include "FilterManager.h"
#include <iostream>

#pragma warning(disable : 4244)  // 警告を抑制（推奨は `static_cast<uint16_t>` を使う）

// ✅ `TSParser` を定義して `SDTFilter` に渡す
/*TSParser tsParser;
SDTFilter sdtFilter(&tsParser);
uint16_t dummyPMTPID = 0x1000;
PMTFilter pmtFilter(&tsParser, dummyPMTPID);
PATFilter patFilter;*/

// ✅ `EnumChannelName()` 用のスレッドセーフな静的変数
static thread_local std::wstring lastChannelName;

// ✅ **チャンネル情報を取得する**
const BOOL IBonDriver2::GetChannelInfo(DWORD dwSpace, DWORD dwChannel, ChannelInfo& info) {
    for (const auto& service : sdtFilter.GetServiceList()) {
        if (service.serviceId == static_cast<uint16_t>(dwChannel)) {
            info.channelId = dwChannel;
            info.channelName = service.serviceName;
            info.networkId = static_cast<uint16_t>(service.networkId);
            info.tsid = service.tsid;
            return TRUE;
        }
    }

    // ✅ **PATFilter から PMT の PID を取得**
    const auto& programMap = patFilter.GetProgramMap();
    auto it = programMap.find(dwChannel);
    if (it != programMap.end()) {
        info.channelId = dwChannel;
        info.channelName = L"Service " + std::to_wstring(dwChannel);
        info.networkId = 0;
        info.tsid = it->second;

        std::wcout << L"GetChannelInfo: Found channel from PAT, Channel=" << dwChannel
            << L", PMT_PID=" << it->second << std::endl;
        return TRUE;
    }

    return FALSE;
}

// ✅ **`EnumChannelName()` をスレッドセーフに修正**
LPCTSTR IBonDriver2::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel) {
    lastChannelName = L"Unknown Channel";

    for (const auto& service : sdtFilter.GetServiceList()) {
        if (service.serviceId == static_cast<uint16_t>(dwChannel)) {
            lastChannelName = service.serviceName;
            break;
        }
    }

    if (lastChannelName == L"Unknown Channel") {
        const auto& programMap = patFilter.GetProgramMap();
        auto it = programMap.find(dwChannel);
        if (it != programMap.end()) {
            lastChannelName = L"Service " + std::to_wstring(dwChannel);
        }
    }

    if (lastChannelName == L"Unknown Channel") {
        lastChannelName = L"CH" + std::to_wstring(dwChannel);
    }

    std::wcout << L"EnumChannelName: Space=" << dwSpace
        << L", Channel=" << dwChannel
        << L", Name=" << lastChannelName << std::endl;

    return lastChannelName.c_str();
}
