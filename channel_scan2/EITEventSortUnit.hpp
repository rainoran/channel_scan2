// ================================
// EITEventSortUnit.hpp (REPLACE)
// H = authoritative / L = patch
// ================================
#pragma once
#ifndef EIT_EVENT_SORT_UNIT_HPP
#define EIT_EVENT_SORT_UNIT_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <map>
#include <algorithm>

#include "EITFilter.hpp"  // ParsedEITPacket

struct EITEventKey
{
    uint16_t original_network_id = 0;
    uint16_t transport_stream_id = 0;
    uint16_t service_id = 0;
    uint16_t event_id = 0;
    uint64_t start_time_raw = 0;

    EITEventKey() = default;

    explicit EITEventKey(const ParsedEITPacket& ev)
        : original_network_id(ev.original_network_id)
        , transport_stream_id(ev.transport_stream_id)
        , service_id(ev.service_id)
        , event_id(ev.event_id)
        , start_time_raw(ev.start_time)
    {
    }
};

struct EITEventKeyLess
{
    bool operator()(const EITEventKey& a, const EITEventKey& b) const noexcept
    {
        if (a.original_network_id != b.original_network_id) return a.original_network_id < b.original_network_id;
        if (a.transport_stream_id != b.transport_stream_id) return a.transport_stream_id < b.transport_stream_id;
        if (a.service_id != b.service_id) return a.service_id < b.service_id;
        if (a.event_id != b.event_id) return a.event_id < b.event_id;
        return a.start_time_raw < b.start_time_raw;
    }
};

class EITEventSortUnit
{
public:
    struct SortOptions
    {
        // --- H側の base 選択 ---
        bool preferScheduleAsBase = true;
        bool preferTableId50_51 = true;

        // ★H merge時の「空だけ入れ替え」方針
        // 短形式タイトル/概要は「既に入っているなら原則固定」＝破損/空のみ差し替え
        bool lockNonEmptyShortFieldsInHMerge = true;

        // lock が true の時でも、十分に増えた場合だけアップグレード許可（必要なら）
        bool allowUpgradeTitleIfLonger = false;
        std::size_t upgradeMinGrowTitleUtf16 = 2;

        bool allowUpgradeNameIfLonger = false;
        std::size_t upgradeMinGrowNameBytes = 4;

        bool allowUpgradeShortTextIfLonger = false;
        std::size_t upgradeMinGrowShortTextBytes = 16;

        // --- L側のパッチ適用 ---
        bool enablePatchApply = true;
        bool allowPatchOnlyEvents = false;
        bool patchMergeExtendedItems = true;
        bool patchAllowUpgradeLongerText = false;
        std::size_t patchUpgradeMinGrowBytes = 32;

        // --- 欠損フィールド救済（グループ内） ---
        bool enableFieldLevelRescueInGroup = true;

        // --- キー分裂対策（start_time_raw 無効時） ---
        // start_time_raw が 0 / 0xFFFF... のイベントは start_time_raw=0 扱いでキーを寄せる
        bool relaxKeyWhenInvalidStartTime = true;

        // --- cross service rescue ---
        bool enableCrossServiceTextRescue = true;
        double titleRichThreshold = 0.80;
        double titleSparseThreshold = 0.10;
        double minReplicaOverlap = 0.85;

        bool requireDurationMatch = true;
        uint32_t maxDurationDiffSec = 60;

        // version選択はEitEventUpdateUnit内のヘルパーへ委譲する。
        bool enableSubtableVersionSelection = true;

    };

    struct DebugCounters
    {
        std::uint64_t calls = 0;
        std::uint64_t rawEventsTotalH = 0;
        std::uint64_t rawEventsTotalL = 0;

        std::uint64_t groupsH = 0;
        std::uint64_t multiCandidateGroupsH = 0;
        std::uint64_t maxCandidatesInGroupH = 0;

        std::uint64_t mergesTriedH = 0;
        std::uint64_t rescuedTitleFromOlderVerH = 0;          // 互換用（従来）
        std::uint64_t fallbackBaseToOlderDueToEmptyH = 0;

        std::uint64_t patchGroupsMatched = 0;
        std::uint64_t patchEventsApplied = 0;
        std::uint64_t patchFieldsFilled = 0;
        std::uint64_t patchItemsMerged = 0;
        std::uint64_t patchOnlyAdded = 0;

        std::uint64_t crossServiceReplicaPairs = 0;
        std::uint64_t crossServiceRescuedEvents = 0;
        std::uint64_t crossServiceRescuedFields = 0;

        // --- グループ救済（項目別） ---
        std::uint64_t groupRescueTitle = 0;
        std::uint64_t groupRescueName = 0;
        std::uint64_t groupRescueShortText = 0;
        std::uint64_t groupRescueGenre = 0;
        std::uint64_t groupRescueExtText = 0;
        std::uint64_t groupRescueExtItems = 0;

        std::uint64_t groupFieldRescueFilled = 0;             // 合計

        // --- 結果欠損カウント（取りこぼし検知） ---
        std::uint64_t resultMissingTitle = 0;     // title_utf16 OR short_event_name_bytes が空
        std::uint64_t resultMissingSummary = 0;   // short_text_bytes が空
        std::uint64_t resultMissingGenre = 0;     // has_genre==false
        std::uint64_t resultMissingExtItems = 0;  // extended_items が空

        std::uint64_t resultEventsTotal = 0;
        std::uint64_t versionCandidatesDropped = 0;
    };

    static void EnableDebugLog(bool enable) noexcept;
    static void ResetDebugCounters() noexcept;
    static DebugCounters GetDebugCountersSnapshot() noexcept;
    static void DumpDebugCounters();

    static std::vector<ParsedEITPacket>
        BuildUniqueSortedEvents(const std::vector<ParsedEITPacket>& authoritativeEvents);

    static std::vector<ParsedEITPacket>
        BuildUniqueSortedEvents(const std::vector<ParsedEITPacket>& authoritativeEvents, const SortOptions& opt);

    static std::vector<ParsedEITPacket>
        BuildUniqueSortedEvents(const std::vector<ParsedEITPacket>& authoritativeEvents,
            const std::vector<ParsedEITPacket>& patchEvents);

    static std::vector<ParsedEITPacket>
        BuildUniqueSortedEvents(const std::vector<ParsedEITPacket>& authoritativeEvents,
            const std::vector<ParsedEITPacket>& patchEvents,
            const SortOptions& opt);
};

#endif // EIT_EVENT_SORT_UNIT_HPP
