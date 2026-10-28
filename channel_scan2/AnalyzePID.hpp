#pragma once
#ifndef ANALYZE_PID_HPP
#define ANALYZE_PID_HPP

#include <unordered_map>
#include <vector>
#include <array>
#include <cstdint>
#include <cstddef>
#include <shared_mutex>

#include "CPUFeature.h"

class AnalyzePID {
public:
    AnalyzePID();

    void ProcessPacket(const uint8_t* tsPacket, size_t packetSize);
    void PrintPIDSummary() const;
    void Reset();
    void InitAnalyzePID();

private:
    // PID range: 0x0000..0x1FFF
    static constexpr uint16_t kPidMax = 0x1FFF;
    static constexpr size_t   kPidCountSize = 0x2000;

    static constexpr size_t   kTs188 = 188;
    static constexpr size_t   kTs204 = 204;

    std::unordered_map<uint16_t, size_t> pidCount;
    std::unordered_map<uint16_t, size_t> dropCounter; // 将来用途がなければ削除可

    mutable std::shared_mutex pidMutex;

    using PacketFunc = void (AnalyzePID::*)(const uint8_t*, size_t);
    PacketFunc optimizedProcessPacket = nullptr;

    static size_t DetectUnit(const uint8_t* data, size_t size) noexcept;

    void ProcessPacket_Scalar(const uint8_t* tsPacket, size_t packetSize);
    void ProcessPacket_SSE2(const uint8_t* tsPacket, size_t packetSize);
    void ProcessPacket_SSE4_2(const uint8_t* tsPacket, size_t packetSize);
    void ProcessPacket_AVX2(const uint8_t* tsPacket, size_t packetSize);
};

#endif // ANALYZE_PID_HPP
