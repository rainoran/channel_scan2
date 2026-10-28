#pragma once
#ifndef EITFILTER_HPP
#define EITFILTER_HPP

#include <vector>
#include <cstdint>
#include <array>
#include <cstddef>
#include <atomic>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <string>      // std::wstring
#include <filesystem>
#include <fstream>
#include <utility>

#include "TSBufferConstants.hpp"
#include "EITEventTextAssembler.hpp"
#include "AribDecoder.hpp"
#include "DebugManager.h"
#include "EITExtendedItemDebugDump.hpp"

// 拡張イベントの items（項目）1つ分
struct EitExtendedItem
{
    std::vector<uint8_t> item_description_bytes;
    std::vector<uint8_t> item_text_bytes;
};

// EIT の 1 イベント分の解析結果
struct ParsedEITPacket
{
    uint8_t  table_id = 0;
    uint16_t service_id = 0;
    uint16_t event_id = 0;
    uint8_t  version = 0;
    bool     current_next = false;

    uint8_t  section_number = 0;
    uint8_t  last_section_number = 0;

    uint16_t transport_stream_id = 0;
    uint16_t original_network_id = 0;

    uint64_t start_time = 0;
    uint32_t duration_hhmmss = 0;

    uint8_t  running_status = 0;
    bool     free_ca_mode = false;

    std::vector<uint8_t> descriptors;

    std::vector<uint8_t> short_event_name_bytes;
    std::vector<uint8_t> short_text_bytes;
    std::vector<uint8_t> extended_text_bytes;
    uint32_t duration = 0;

    std::vector<EitExtendedItem> extended_items;

    bool    has_genre = false;
    uint8_t content_nibble_level1 = 0;
    uint8_t content_nibble_level2 = 0;
    uint8_t user_nibble1 = 0;
    uint8_t user_nibble2 = 0;

    bool     has_series = false;
    uint16_t series_id = 0;
    uint8_t  repeat_label = 0;
    uint8_t  program_pattern = 0;
    bool     series_expire_valid = false;
    uint16_t series_expire_mjd = 0;
    uint16_t episode_number = 0;
    uint16_t last_episode_number = 0;
    std::vector<uint8_t> series_name_bytes;

    bool     has_parental_rating = false;
    std::wstring parental_country;
    uint8_t  parental_rating = 0;

    std::wstring title_utf16;
    std::wstring short_text_utf16;
    std::wstring extended_text_utf16;
};

struct SegmentKey
{
    uint16_t original_network_id = 0;
    uint16_t transport_stream_id = 0;
    uint16_t service_id = 0;
    uint8_t  table_id = 0;

    bool operator<(const SegmentKey& other) const noexcept
    {
        if (original_network_id != other.original_network_id)
            return original_network_id < other.original_network_id;
        if (transport_stream_id != other.transport_stream_id)
            return transport_stream_id < other.transport_stream_id;
        if (service_id != other.service_id)
            return service_id < other.service_id;
        return table_id < other.table_id;
    }
};

struct SegmentState
{
    bool    version_valid = false;
    uint8_t version_number = 0xFF;

    uint8_t last_section_number = 0;
    uint8_t segment_last_section_number = 0;
    uint8_t last_table_id = 0;

    std::array<bool, 256> received_sections{};
    std::array<uint32_t, 256> section_crc{};

    std::size_t received_unique = 0;
    std::size_t duplicate_same_crc = 0;
    std::size_t version_changes = 0;
};

class EITFilter
{
public:
    EITFilter() = default;
    ~EITFilter();

    void InputPacket(const std::array<uint8_t, TS_PACKET_SIZE>& pkt);
    void InputSection(const uint8_t* section, std::size_t length);

    // ここは「イベント受領」入口（ParseEITSection から呼ばれる）
    bool ParseAndCheckPacket(const ParsedEITPacket& packet);

    void Reset();
    void ResetService(uint16_t original_network_id,
        uint16_t transport_stream_id,
        uint16_t service_id);

    // ★ GetEvents() は “H + L を合成した Merged” を返す
    bool HasValidData() const noexcept { return data_valid && !m_eventsMerged.empty(); }
    const std::vector<ParsedEITPacket>& GetEvents() const noexcept { return m_eventsMerged; }

    // ★ 追加：内部格納を直接見たい場合用
    const std::vector<ParsedEITPacket>& GetEvents_H() const noexcept { return m_eventsH; }
    const std::vector<ParsedEITPacket>& GetEvents_L() const noexcept { return m_eventsL; }

    std::size_t GetIntegrityDropCount() const noexcept { return m_integrityDropCount; }
    std::size_t GetErrorDropCount() const noexcept { return m_errorDropCount; }

    void DumpDebugCounters() const;

    double GetSectionCoverageRatio(uint16_t original_network_id,
        uint16_t transport_stream_id,
        uint16_t service_id,
        uint8_t  table_id) const;

    bool IsEpgSegmentComplete(uint16_t original_network_id,
        uint16_t transport_stream_id,
        uint16_t service_id,
        uint8_t  table_id,
        double   threshold = 0.8) const;

    void DumpIncompleteSegments(double threshold = 1.0) const;

    bool AppendCountersTsv_AllChannels(const std::filesystem::path& tsvPath,
        int space,
        int channel) const;

    // ★ ヘッダ内で定義しない（cpp側に実装を寄せる）
    static void WriteTsvHeaderU16(std::ofstream& ofs);

private:
    struct EitStats
    {
        std::size_t section_total = 0;
        std::size_t section_eit = 0;
        std::size_t section_current_next_drop = 0;

        std::size_t section_len_range_drop = 0;
        std::size_t section_len_mismatch_drop = 0;
        std::size_t section_crc_fail = 0;

        std::size_t section_duplicate_same_crc = 0;

        std::size_t desc_len0_skip = 0;
        std::size_t desc_overrun_break = 0;

        std::size_t desc_4d_seen = 0;
        std::size_t desc_4d_ok = 0;
        std::size_t desc_4d_drop_len = 0;
        std::size_t desc_4d_drop_bounds = 0;

        std::size_t desc_4e_seen = 0;
        std::size_t desc_4e_ok = 0;
        std::size_t desc_4e_drop_items_bounds = 0;
        std::size_t desc_4e_drop_text_bounds = 0;
        std::size_t desc_4e_frag_reject = 0;

        std::size_t extitem_merged_continuation = 0;
        std::size_t extitem_merged_same_label = 0;
        std::size_t extitem_orphan_continuation = 0;

        std::size_t asm_try = 0;
        std::size_t asm_complete_true = 0;
        std::size_t asm_incomplete = 0;
        std::size_t asm_success = 0;
        std::size_t asm_fail = 0;

        // ★ H/L 分離の診断
        std::size_t h_add = 0;
        std::size_t h_update = 0;
        std::size_t l_add = 0;
        std::size_t l_update = 0;
        std::size_t merged_rebuild = 0;
        std::size_t patch_skipped_version_mismatch = 0;

        // section 単位の世代差し替え診断
        std::size_t section_snapshot_add = 0;
        std::size_t section_snapshot_replace = 0;
        std::size_t section_snapshot_stale_replay_drop = 0;
        std::size_t section_snapshot_events_removed = 0;
    };

    EitStats m_stats{};

    struct EventKey
    {
        uint16_t original_network_id = 0;
        uint16_t transport_stream_id = 0;
        uint16_t service_id = 0;
        uint8_t  table_id = 0;
        uint16_t event_id = 0;
        uint64_t start_time_raw = 0;

        bool operator==(const EventKey& o) const noexcept
        {
            return original_network_id == o.original_network_id &&
                transport_stream_id == o.transport_stream_id &&
                service_id == o.service_id &&
                table_id == o.table_id &&
                event_id == o.event_id &&
                start_time_raw == o.start_time_raw;
        }
    };

    struct EventKeyHash
    {
        std::size_t operator()(const EventKey& k) const noexcept
        {
            uint64_t x = 0;
            x ^= (uint64_t)k.original_network_id << 48;
            x ^= (uint64_t)k.transport_stream_id << 32;
            x ^= (uint64_t)k.service_id << 16;
            x ^= (uint64_t)k.table_id;
            x ^= (uint64_t)k.event_id << 24;
            x ^= k.start_time_raw * 0x9E3779B97F4A7C15ull;
            return (std::size_t)(x ^ (x >> 33));
        }
    };

    // EIT の version_number はイベント単体ではなく section の世代情報。
    // EventID が変更された番組差し替えも検出できるように、
    // section の識別情報を独立したキーとして保持する。
    struct SectionKey
    {
        uint16_t original_network_id = 0;
        uint16_t transport_stream_id = 0;
        uint16_t service_id = 0;
        uint8_t  table_id = 0;
        uint8_t  section_number = 0;

        bool operator==(const SectionKey& o) const noexcept
        {
            return original_network_id == o.original_network_id &&
                transport_stream_id == o.transport_stream_id &&
                service_id == o.service_id &&
                table_id == o.table_id &&
                section_number == o.section_number;
        }
    };

    struct SectionKeyHash
    {
        std::size_t operator()(const SectionKey& k) const noexcept
        {
            uint64_t x = 0;
            x ^= static_cast<uint64_t>(k.original_network_id) << 48;
            x ^= static_cast<uint64_t>(k.transport_stream_id) << 32;
            x ^= static_cast<uint64_t>(k.service_id) << 16;
            x ^= static_cast<uint64_t>(k.table_id) << 8;
            x ^= static_cast<uint64_t>(k.section_number);
            x ^= (x >> 33);
            x *= 0xff51afd7ed558ccdULL;
            x ^= (x >> 33);
            return static_cast<std::size_t>(x);
        }
    };

    struct SectionSignature
    {
        uint8_t  version = 0;
        uint32_t crc = 0;

        bool operator==(const SectionSignature& o) const noexcept
        {
            return version == o.version && crc == o.crc;
        }
    };

    struct SectionSnapshot
    {
        uint8_t  version = 0;
        uint32_t crc = 0;
        std::vector<EventKey> event_keys;

        // 受信バッファに遅れて残った旧sectionで A→B→A と
        // 巻き戻らないよう、直近の退役済み署名を少数だけ保持する。
        std::vector<SectionSignature> retired_signatures;
    };

    static constexpr std::size_t MAX_SECTION_SIZE = 8192;
    // EIT収集中のメモリピークを抑えるため、保持数は安全寄りに制限する。
    // 関数名・公開I/Fは維持し、内部保持量のみ調整。
    static constexpr std::size_t MAX_STORED_EVENTS = 32768;
    static constexpr std::size_t MAX_PATCH_EVENTS = 32768;

    bool data_valid = false;

    // ★ H = authoritative skeleton（骨格）
    std::vector<ParsedEITPacket> m_eventsH;
    std::unordered_map<EventKey, std::size_t, EventKeyHash> m_eventIndexH;

    // ★ L = patch（穴埋め）
    std::vector<ParsedEITPacket> m_eventsL;
    std::unordered_map<EventKey, std::size_t, EventKeyHash> m_eventIndexL;

    // ★ Merged（H+L 合成） : GetEvents() で返す
    std::vector<ParsedEITPacket> m_eventsMerged;

    std::map<SegmentKey, SegmentState> m_segmentStates;

    // section -> 現在そのsectionが所有するイベント。
    // 新sectionを完全解析してからこのスナップショットを交換する。
    std::unordered_map<SectionKey, SectionSnapshot, SectionKeyHash> m_sectionSnapshots;

    // 同じ EventKey がsection境界移動中に一時的に複数sectionから
    // 参照されても、片側の更新で誤削除しないための所有数。
    std::unordered_map<EventKey, std::size_t, EventKeyHash> m_eventOwnerCounts;

    std::vector<uint8_t> sectionBuffer;
    std::size_t expectedSectionSize = 0;

    void ParseEITSection(const uint8_t* section, std::size_t length);
    bool PerformIntegrityCheck(const ParsedEITPacket& packet);
    bool PerformErrorCheck(const ParsedEITPacket& packet);

    EitEventTextAssembler m_textAssembler;

    // ★ H/L 分離ヘルパ
    static EventKey MakeKey(const ParsedEITPacket& p) noexcept;
    static SectionKey MakeSectionKey(const ParsedEITPacket& p) noexcept;
    static ParsedEITPacket MakeSkeletonH(const ParsedEITPacket& in); // H化（骨格のみ）
    static bool HasAnyPatchL(const ParsedEITPacket& in) noexcept;    // Lとして保存価値があるか
    static ParsedEITPacket MakePatchL(const ParsedEITPacket& in);    // L化（補完成分中心）

    static void MergePatchL(ParsedEITPacket& dst, const ParsedEITPacket& src); // L内マージ
    static void ApplyPatchToMerged(ParsedEITPacket& merged, const ParsedEITPacket& patch); // 合成

    void RebuildMergedAtIndex(std::size_t hIndex); // merged[hIndex] = H + (L)
    bool StoreCheckedPacket(const ParsedEITPacket& packet);
    bool CommitSectionSnapshot(
        const SectionKey& sectionKey,
        uint8_t version,
        uint32_t crc,
        const std::vector<ParsedEITPacket>& parsedEvents);
    void RemoveEventsByKeySet(
        const std::unordered_set<EventKey, EventKeyHash>& removeKeys);
    void RebuildEventIndexesAndMerged();
    void TrimStoredEventsIfNeeded();

    std::size_t m_integrityDropCount = 0;
    std::size_t m_errorDropCount = 0;

    std::size_t m_tsPacketCount = 0;
    std::size_t m_sectionStartCount = 0;
    std::size_t m_sectionCompleteCount = 0;
    std::size_t m_sectionDropBySize = 0;
};

#endif // EITFILTER_HPP
