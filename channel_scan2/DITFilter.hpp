#pragma once
// DITFilter.hpp
#pragma once
#ifndef DIT_FILTER_HPP
#define DIT_FILTER_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <atomic>

class DITFilter
{
public:
    struct Snapshot {
        bool    valid = false;
        uint8_t table_id = 0xFF;   // 0x7E expected
        bool    transition_flag = false;
        uint16_t section_length = 0;
    };

    void Reset() noexcept;

    void InputPacket(const uint8_t* data, std::size_t size);

    bool IsRecovered() const noexcept { return recovered.load(std::memory_order_acquire); }

    uint64_t GetPacketCount()   const noexcept { return packetCount.load(); }
    uint64_t GetPusiCount()     const noexcept { return pusiCount.load(); }
    uint64_t GetSectionCount()  const noexcept { return sectionCount.load(); }
    uint64_t GetInvalidCount()  const noexcept { return invalidCount.load(); }
    uint64_t GetCcErrorCount()  const noexcept { return ccErrorCount.load(); }

    Snapshot GetLastSnapshot() const noexcept { return lastSnapshot; }

private:
    void OnSection(const uint8_t* sec, std::size_t len);

private:
    std::vector<uint8_t> secBuf;
    bool     assembling = false;
    uint8_t  lastCC = 0xFF;
    std::size_t expectedTotal = 0;

    std::atomic<bool> recovered{ false };

    std::atomic<uint64_t> packetCount{ 0 };
    std::atomic<uint64_t> pusiCount{ 0 };
    std::atomic<uint64_t> sectionCount{ 0 };
    std::atomic<uint64_t> invalidCount{ 0 };
    std::atomic<uint64_t> ccErrorCount{ 0 };

    Snapshot lastSnapshot{};
};

#endif // DIT_FILTER_HPP
