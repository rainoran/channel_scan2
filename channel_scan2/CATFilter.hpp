#pragma once
#pragma once
#ifndef CAT_FILTER_HPP
#define CAT_FILTER_HPP

#include <cstdint>
#include <vector>
#include <atomic>
#include <array>

#include "TSBufferConstants.hpp" // TS_PACKET_SIZE 想定

class CATFilter
{
public:
    struct CADescriptor
    {
        uint16_t ca_system_id = 0;
        uint16_t ca_pid = 0;                 // 13bit PID
        std::vector<uint8_t> private_data;   // descriptor 残り
    };

    enum class Status : uint8_t
    {
        NotSeen = 0,
        Collecting,
        Recovered,
        CrcError
    };

public:
    CATFilter() = default;

    void Reset();

    // FilterManager から呼ぶ想定（PID 0x0001 の TSパケット）
    void InputPacket(const uint8_t* data, size_t size);

    bool   IsRecovered() const noexcept { return recovered.load(); }
    Status GetStatus()  const noexcept { return status.load(); }

    // 最新の結果（最後にCRC OKだったCAT）
    uint8_t GetVersion() const noexcept { return last_version.load(); }
    bool    IsCurrent()  const noexcept { return last_current.load(); }

    const std::vector<CADescriptor>& GetCADescriptors() const noexcept { return ca_desc; }

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
    // セクション組み立て状態
    std::vector<uint8_t> sec_buf;
    size_t expected_len = 0;
    int last_cc = -1;

    // 結果
    std::vector<CADescriptor> ca_desc;

    std::atomic<bool> recovered{ false };
    std::atomic<Status> status{ Status::NotSeen };
    std::atomic<uint8_t> last_version{ 0 };
    std::atomic<bool> last_current{ false };

    // 統計
    std::atomic<size_t> pid_packets{ 0 };
    std::atomic<size_t> pusi_count{ 0 };
    std::atomic<size_t> section_ok{ 0 };
    std::atomic<size_t> crc_fail{ 0 };
    std::atomic<size_t> discontinuity{ 0 };
};

#endif // CAT_FILTER_HPP
