
#pragma once
#ifndef RSTFILTER_HPP
#define RSTFILTER_HPP

#include <cstdint>
#include <cstddef>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <functional>

class RSTFilter
{
public:
    struct EventKey {
        uint16_t tsid = 0;
        uint16_t onid = 0;
        uint16_t sid = 0;
        uint16_t eid = 0;

        friend bool operator==(const EventKey& a, const EventKey& b) noexcept
        {
            return a.tsid == b.tsid && a.onid == b.onid && a.sid == b.sid && a.eid == b.eid;
        }
    };

    struct EventInfo {
        uint8_t  running_status = 0;   // 0..7
        uint64_t update_seq = 0;       // 更新順
    };

    RSTFilter() = default;
    ~RSTFilter() = default;

    void Reset() noexcept;

    // TS パケット(188/204)を想定。FilterManager 側から (data,size) で呼べる形にしておく
    void InputPacket(const uint8_t* ts, std::size_t size);

    bool IsRecovered() const noexcept { return recovered_.load(std::memory_order_acquire); }

    // デバッグ用カウンタ
    std::size_t GetPidPacketCount() const noexcept { return pidPacketCount_.load(); }
    std::size_t GetPusiCount() const noexcept { return pusiCount_.load(); }
    std::size_t GetSectionCount() const noexcept { return sectionCount_.load(); }
    std::size_t GetEntryCount() const noexcept { return entryCount_.load(); }
    std::size_t GetInvalidSectionCount() const noexcept { return invalidSectionCount_.load(); }
    std::size_t GetContinuityErrorCount() const noexcept { return continuityErrorCount_.load(); }

    // スナップショット取得（GUI/プラグイン想定）
    std::vector<std::pair<EventKey, EventInfo>> GetSnapshot() const;

private:
    static constexpr uint16_t kPidRST = 0x0013;
    static constexpr uint8_t  kTableIdRST = 0x71;
    static constexpr std::size_t kMaxSectionBytes = 1024; // section_length<=1021 => total<=1024

    // section reassembly
    std::array<uint8_t, kMaxSectionBytes> secBuf_{};
    std::size_t secPos_ = 0;
    std::size_t secNeed_ = 0;
    bool assembling_ = false;

    bool lastCcValid_ = false;
    uint8_t lastCc_ = 0xFF;

    // counters
    std::atomic_bool recovered_{ false };
    std::atomic<std::size_t> pidPacketCount_{ 0 };
    std::atomic<std::size_t> pusiCount_{ 0 };
    std::atomic<std::size_t> sectionCount_{ 0 };
    std::atomic<std::size_t> entryCount_{ 0 };
    std::atomic<std::size_t> invalidSectionCount_{ 0 };
    std::atomic<std::size_t> continuityErrorCount_{ 0 };

    mutable std::mutex mtx_;
    std::unordered_map<uint64_t, EventInfo> latest_; // packed key -> info
    uint64_t updateSeq_ = 0;

private:
    static uint64_t PackKey(const EventKey& k) noexcept;
    void ProcessSection(const uint8_t* sec, std::size_t totalLen);
    void ResetAssembler() noexcept;
};

namespace std {
    template<> struct hash<RSTFilter::EventKey> {
        std::size_t operator()(const RSTFilter::EventKey& k) const noexcept {
            // 64bit pack の下位混ぜ
            const uint64_t v =
                (uint64_t(k.tsid) << 48) |
                (uint64_t(k.onid) << 32) |
                (uint64_t(k.sid) << 16) |
                (uint64_t(k.eid));
            return std::hash<uint64_t>{}(v);
        }
    };
}

#endif // RSTFILTER_HPP
