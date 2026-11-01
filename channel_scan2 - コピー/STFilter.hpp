#pragma once
#pragma once
#ifndef ST_FILTER_HPP
#define ST_FILTER_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include <atomic>

// ST (Stuffing Table)
// table_id = 0x72 / PID は固定ではない
class STFilter
{
public:
    struct Snapshot {
        bool     valid = false;
        uint8_t  table_id = 0xFF;      // 0x72 expected
        uint16_t pid = 0xFFFF;         // 最後に復元した ST の PID
        bool     section_syntax_indicator = false;
        uint16_t section_length = 0;   // 0..1021
    };

    void Reset() noexcept;
    void InputPacket(const uint8_t* data, std::size_t size);

    // ST は必須テーブルではないので「見えたら true」程度の意味
    bool IsRecovered() const noexcept { return recovered.load(std::memory_order_acquire); }

    uint64_t GetPacketCount()  const noexcept { return packetCount.load(); }
    uint64_t GetPusiCount()    const noexcept { return pusiCount.load(); }
    uint64_t GetSectionCount() const noexcept { return sectionCount.load(); }
    uint64_t GetInvalidCount() const noexcept { return invalidCount.load(); }
    uint64_t GetCcErrorCount() const noexcept { return ccErrorCount.load(); }

    Snapshot GetLastSnapshot() const noexcept { return lastSnapshot; }

private:
    struct AsmState {
        std::vector<uint8_t> buf;
        bool        assembling = false;
        uint8_t     lastCC = 0xFF;
        std::size_t expectedTotal = 0;
    };

    void OnSection(const uint8_t* sec, std::size_t len, uint16_t pid);

private:
    // PID 固定でないので PID ごとに状態を持つ
    std::unordered_map<uint16_t, AsmState> states;
    static constexpr std::size_t kMaxStatePids = 64; // 暴走保護

    std::atomic<bool> recovered{ false };

    std::atomic<uint64_t> packetCount{ 0 };
    std::atomic<uint64_t> pusiCount{ 0 };
    std::atomic<uint64_t> sectionCount{ 0 };
    std::atomic<uint64_t> invalidCount{ 0 };
    std::atomic<uint64_t> ccErrorCount{ 0 };

    Snapshot lastSnapshot{};
};

#endif // ST_FILTER_HPP
