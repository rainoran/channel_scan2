#pragma once
#ifndef TS_STREAM_UTILS_HPP
#define TS_STREAM_UTILS_HPP

#include <windows.h>
#include <vector>
#include <deque>
#include <array>
#include <cstdint>
#include <cstddef>

#include "IBonDriver2.h"
#include "TsStreamTypes.hpp"
#include "TSBufferConstants.hpp"
#include "DebugManager.h"

constexpr size_t MAX_BUFFER_PACKETS = 4096;

namespace TSStreamUtils
{
    struct TsStreamStats
    {
        size_t callCount = 0;
        size_t successCount = 0;
        size_t failureCount = 0;
        size_t packetCount = 0;

        // 安全診断用（既存呼び出しには影響しない追加フィールド）
        size_t invalidBlockCount = 0;
        size_t invalidSyncCount = 0;
        size_t oversizedBlockCount = 0;
        size_t converted204PacketCount = 0;
        size_t captureDroppedBlocks = 0;

        double GetFailureRate() const {
            const double total = static_cast<double>(callCount);
            return (total > 0.0) ? (static_cast<double>(failureCount) / total) : 0.0;
        }
        double GetAvgPacketsPerSuccess() const {
            const double succ = static_cast<double>(successCount);
            return (succ > 0.0) ? (static_cast<double>(packetCount) / succ) : 0.0;
        }
    };

    bool WaitForSignalLock(IBonDriver2* bon, int timeout_ms = 2000);

    bool ReceiveTsToBuffer(IBonDriver2* bon,
        std::deque<std::array<uint8_t, TS_PACKET_SIZE>>& tsBuffer,
        int msDuration = 3000);

    void PrintHex(const std::vector<uint8_t>& data, size_t bytesPerLine = 16);
    void PrintHex(const uint8_t* data, size_t size, size_t bytesPerLine = 16);

    bool ReceiveTsStreamBlock(IBonDriver2* bon, std::vector<TsPacket>& packetsOut);

    size_t GetTsStreamFailureCount();
    TsStreamStats GetTsStreamStats();
    void ResetTsStreamStats();

    // キャプチャしたパケットの保存・取得（※内部は上限制限）
    // 通常運用ではメモリピーク低減のため既定OFF。必要時だけ有効化する。
    void SetCapturedPacketDumpEnabled(bool enabled) noexcept;
    bool IsCapturedPacketDumpEnabled() noexcept;
    void DumpAllCapturedPackets();
    void ClearCapturedPackets();
}

#endif // TS_STREAM_UTILS_HPP
