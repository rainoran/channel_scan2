#pragma once
#ifndef BAT_FILTER_HPP
#define BAT_FILTER_HPP

#include <array>
#include <cstdint>
#include <atomic>
#include "TSBufferConstants.hpp"

class BATFilter
{
public:
    BATFilter() = default;

    void Reset() noexcept {
        recovered.store(false, std::memory_order_relaxed);
        pidPackets.store(0, std::memory_order_relaxed);
        pusiPackets.store(0, std::memory_order_relaxed);
        table4aSeen.store(0, std::memory_order_relaxed);
    }

    void InputPacket(const std::array<uint8_t, TS_PACKET_SIZE>& pkt) noexcept;
    bool IsRecovered() const noexcept { return recovered.load(std::memory_order_relaxed); }

    // デバッグ用
    uint64_t GetPidPackets()  const noexcept { return pidPackets.load(std::memory_order_relaxed); }
    uint64_t GetPusiPackets() const noexcept { return pusiPackets.load(std::memory_order_relaxed); }
    uint64_t GetTable4aSeen() const noexcept { return table4aSeen.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> recovered{ false };
    std::atomic<uint64_t> pidPackets{ 0 };
    std::atomic<uint64_t> pusiPackets{ 0 };
    std::atomic<uint64_t> table4aSeen{ 0 };
};

#endif
