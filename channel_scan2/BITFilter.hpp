#pragma once
#pragma once
#ifndef BIT_FILTER_HPP
#define BIT_FILTER_HPP

#include <cstdint>
#include <vector>
#include <atomic>

#include "TSBufferConstants.hpp"

class BITFilter
{
public:
    struct Descriptor
    {
        uint8_t tag = 0;
        std::vector<uint8_t> data; // descriptor_body
    };

    enum class Status : uint8_t
    {
        NotSeen = 0,
        Collecting,
        Recovered,
        CrcError
    };

public:
    BITFilter() = default;

    void Reset();
    void InputPacket(const uint8_t* data, size_t size); // PID 0x0024 想定

    bool   IsRecovered() const noexcept { return recovered.load(); }
    Status GetStatus()  const noexcept { return status.load(); }

    uint16_t GetBroadcasterId() const noexcept { return broadcaster_id.load(); }
    uint8_t  GetVersion() const noexcept { return last_version.load(); }
    bool     IsCurrent()  const noexcept { return last_current.load(); }

    const std::vector<Descriptor>& GetBroadcasterDescriptors() const noexcept { return broadcaster_desc; }
    const std::vector<uint8_t>& GetBodyRest() const noexcept { return body_rest; }

    // 統計
    size_t GetPidPacketCount() const noexcept { return pid_packets.load(); }
    size_t GetPusiCount() const noexcept { return pusi_count.load(); }
    size_t GetSectionOkCount() const noexcept { return section_ok.load(); }
    size_t GetCrcFailCount() const noexcept { return crc_fail.load(); }
    size_t GetDiscontinuityCount() const noexcept { return discontinuity.load(); }

private:
    static uint32_t ComputePsiCrc32(const uint8_t* data, size_t len) noexcept;
    static bool     IsCrcOk(const uint8_t* sec, size_t len) noexcept;

    void ConsumePayloadBytes(const uint8_t* payload, size_t len, bool pusi, uint8_t cc);
    void TryConsumeByte(uint8_t b);
    void ProcessSection(const uint8_t* sec, size_t len);

private:
    std::vector<uint8_t> sec_buf;
    size_t expected_len = 0;
    int last_cc = -1;

    std::vector<Descriptor> broadcaster_desc;
    std::vector<uint8_t> body_rest;

    std::atomic<bool> recovered{ false };
    std::atomic<Status> status{ Status::NotSeen };

    std::atomic<uint16_t> broadcaster_id{ 0 };
    std::atomic<uint8_t> last_version{ 0 };
    std::atomic<bool> last_current{ false };

    std::atomic<size_t> pid_packets{ 0 };
    std::atomic<size_t> pusi_count{ 0 };
    std::atomic<size_t> section_ok{ 0 };
    std::atomic<size_t> crc_fail{ 0 };
    std::atomic<size_t> discontinuity{ 0 };
};

#endif // BIT_FILTER_HPP
