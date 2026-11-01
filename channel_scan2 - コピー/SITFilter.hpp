#pragma once
// SITFilter.hpp
#pragma once
#ifndef SIT_FILTER_HPP
#define SIT_FILTER_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <atomic>

class SITFilter
{
public:
    struct Snapshot {
        bool     valid = false;
        uint8_t  table_id = 0xFF;     // 0x7F expected
        uint16_t table_id_ext = 0;    // table_id_extension
        uint8_t  version = 0;
        bool     current_next = false;
        uint8_t  section_number = 0;
        uint8_t  last_section_number = 0;
        uint16_t section_length = 0;
    };

    void Reset() noexcept;
    void InputPacket(const uint8_t* data, std::size_t size);

    bool IsRecovered() const noexcept { return recovered.load(std::memory_order_acquire); }

    uint64_t GetPacketCount()   const noexcept { return packetCount.load(); }
    uint64_t GetPusiCount()     const noexcept { return pusiCount.load(); }
    uint64_t GetSectionCount()  const noexcept { return sectionCount.load(); }
    uint64_t GetInvalidCount()  const noexcept { return invalidCount.load(); }
    uint64_t GetCrcErrorCount() const noexcept { return crcErrorCount.load(); }
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
    std::atomic<uint64_t> crcErrorCount{ 0 };
    std::atomic<uint64_t> ccErrorCount{ 0 };

    Snapshot lastSnapshot{};
};

#endif // SIT_FILTER_HPP
