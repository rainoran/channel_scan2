#pragma once
#ifndef EIT_EVENT_TEXT_ASSEMBLER_HPP
#define EIT_EVENT_TEXT_ASSEMBLER_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <map>

// 1番組イベントを一意に識別するキー
// - ONID / TSID / ServiceID / table_id / EventID / start_time_raw / 言語コード の組
struct EitEventTextKey {
    uint16_t original_network_id = 0;
    uint16_t transport_stream_id = 0;
    uint16_t service_id = 0;
    uint16_t event_id = 0;
    uint32_t language_code = 0;

    uint8_t  table_id = 0;
    uint64_t start_time_raw = 0;

    bool operator<(const EitEventTextKey& other) const noexcept {
        if (original_network_id != other.original_network_id) return original_network_id < other.original_network_id;
        if (transport_stream_id != other.transport_stream_id) return transport_stream_id < other.transport_stream_id;
        if (service_id != other.service_id) return service_id < other.service_id;
        if (event_id != other.event_id) return event_id < other.event_id;
        if (language_code != other.language_code) return language_code < other.language_code;
        if (table_id != other.table_id) return table_id < other.table_id;
        return start_time_raw < other.start_time_raw;
    }
};

// descriptor_number ごとのテキスト断片
struct EitTextFragment
{
    uint8_t descriptor_number = 0;
    std::vector<uint8_t> bytes;
};

// 1イベント分に対応する断片の集合
struct EitEventFragments
{
    bool has_last = false;
    uint8_t last_descriptor_number = 0;
    std::vector<EitTextFragment> fragments;
};

// ExtendedEventDescriptor(0x4E) text を descriptor_number / last_descriptor_number で並べ替え・結合
class EitEventTextAssembler
{
public:
    EitEventTextAssembler();
    ~EitEventTextAssembler();

    void Reset();

    void AddFragment(
        const EitEventTextKey& key,
        uint8_t descriptor_number,
        uint8_t last_descriptor_number,
        bool    lastDescValid,
        const uint8_t* textBytes,
        std::size_t textLength
    );

    bool IsComplete(const EitEventTextKey& key) const;
    bool Assemble(const EitEventTextKey& key, std::vector<uint8_t>& out) const;
    bool AssembleAndErase(const EitEventTextKey& key, std::vector<uint8_t>& out);

    // =========================================================
    // FIX: cpp(外部ヘルパ等)から参照するため public にする
    // =========================================================
    static bool   IsValidStartTimeRaw(uint64_t raw40) noexcept;
    static uint8_t TableIdForKey(uint8_t table_id) noexcept;

private:
    static constexpr std::size_t MAX_EVENTS = 4096;
    static constexpr std::size_t MAX_FRAGMENTS_PER_EVENT = 16;

    static EitEventTextKey NormalizeKey(const EitEventTextKey& in) noexcept;

    std::map<EitEventTextKey, EitEventFragments> m_events;
};

#endif // EIT_EVENT_TEXT_ASSEMBLER_HPP
