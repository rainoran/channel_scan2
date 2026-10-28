// ================================
// EitEventUpdateUnit.hpp (NEW)
// 初回取得＋更新取得（差分/差し替え）を統一処理するストア
// C++20
// ================================
#pragma once
#ifndef EIT_EVENT_UPDATE_UNIT_HPP
#define EIT_EVENT_UPDATE_UNIT_HPP

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <optional>
#include <string>
#include <string_view>

#include "EITFilter.hpp"          // ParsedEITPacket
#include "EITEventSortUnit.hpp"   // SortOptions (任意で利用)

class EitEventUpdateUnit
{
public:
    struct UpdateOptions
    {
        // まずは入力を SortUnit で “1イベント化” してから入れる前提が安全
        // ここで raw入力を渡すなら enablePreSort=true で内部ソートする
        bool enablePreSort = false;

        // 差し替え判定：同一 (onid,tsid,sid,eid) で start_time がズレた時に置換する閾値
        std::uint32_t replaceStartTimeToleranceSec = 10 * 60; // 10分
        bool requireDurationNearOnReplace = true;
        std::uint32_t replaceDurationToleranceSec = 60;

        // 更新判定：テキスト量が増えたら採用（or 欠損埋め）
        bool allowUpgradeIfTextScoreImproves = true;
        std::int64_t upgradeMinScoreGain = 32;

        // 長文/拡張itemsのマージ
        bool mergeExtendedItems = true;
        std::size_t maxExtendedItems = 32;

        // “start_time不明” (0 / 0xFFFFFFFFFF) の扱い
        // true: 0扱いでキーを寄せる（ただし更新は荒れるので注意）
        bool relaxKeyWhenInvalidStartTime = false;

        // EventID が変わる未定枠更新対策。
// 例:
//   旧: セレクション 15:10-16:00 eid=9510
//   新: 確定番組     15:10-17:50 eid=16946
//
// 同じ service の時間帯重複で、既存が汎用未定枠、
// 新規が具体番組なら既存未定枠を削除して新規を採用する。
        bool enableServiceTimeConflictResolve = true;

        // 新規側が汎用未定枠で、既存側に具体番組がある場合は新規未定枠を捨てる。
        bool dropIncomingGenericPlaceholderWhenRealExists = true;

        // 既存側が汎用未定枠で、新規側が具体番組の場合は既存未定枠を置換する。
        bool replaceGenericPlaceholderByRealEvent = true;

        // 時間帯重複を更新扱いにする最小重複秒。
        std::uint32_t serviceTimeOverlapMinSec = 60;

        // 確定番組同士でも、同一 service・同一開始時刻に EventID の異なる
        // 番組をversionの大小だけで比較する旧方式。
        // versionはsection世代であり異なるEventID同士の新旧番号ではないため、
        // 既定では無効。互換用に項目だけ残す。
        bool replaceConcreteConflictByNewerVersion = false;

        // 確定番組同士の同時刻競合を比較するときの番組長許容差。
        // 同一開始時刻でも番組枠そのものが大幅に変わった場合は、
        // 誤置換を避けるため自動解決しない。
        std::uint32_t concreteConflictDurationToleranceSec = 60;

        // Apply()へ渡された各sectionを、その取得回の編成スナップショットとして扱う。
        //
        // section_numberは日付が進むと再利用されるため、同じsection_numberを
        // 持つ保存済みイベントを無条件には削除しない。受信sectionが実際に覆う
        // 時間帯と重なる既存イベントだけを差し替え対象にする。
        // 互換用の逐次更新経路。section内version管理を通らないため既定無効。
        // 編成確定にはApplySectionSnapshots()を使用する。
        bool replaceSectionSnapshotByIncoming = false;

        // present/following (0x4E/0x4F)は完全な編成表ではないため除外する。
        bool sectionSnapshotScheduleOnly = true;

        // 受信section内の最小開始～最大終了へ加える更新境界の余白。
        std::uint32_t sectionSnapshotWindowMarginSec = 5 * 60;

        // 壊れたsectionや異なる日付の混在入力による大量削除を防ぐ上限。
        std::uint32_t sectionSnapshotMaxWindowSec = 12 * 60 * 60;

        // 内部で SortUnit を使う場合のオプション（必要なら外から指定）
        EITEventSortUnit::SortOptions preSortOpt{};
    };

    struct UpdateStats
    {
        std::uint64_t inputEvents = 0;

        std::uint64_t added = 0;
        std::uint64_t updated = 0;     // 同一イベントの改善反映
        std::uint64_t replaced = 0;    // 差し替え（旧削除→新採用）
        std::uint64_t unchanged = 0;

        std::uint64_t filledFields = 0;
        std::uint64_t mergedExtItems = 0;

        // EventID違いの service-time 競合解決。
        std::uint64_t serviceTimeReplaced = 0;
        std::uint64_t serviceTimeDroppedIncoming = 0;

        // 確定番組同士の同一開始時刻競合解決。
        std::uint64_t concreteTimeReplaced = 0;
        std::uint64_t concreteTimeDroppedIncoming = 0;

        // 受信済みsectionの新スナップショットから消えた旧イベント。
        std::uint64_t sectionSnapshotRemoved = 0;
        std::uint64_t sectionSnapshotOutOfWindowKept = 0;
        std::uint64_t sectionSnapshotStaleVersionKept = 0;
        std::uint64_t sectionSnapshotAmbiguousSkipped = 0;
        std::uint64_t sectionVersionCandidatesSkipped = 0;
        std::uint64_t sectionVersionWrapDetected = 0;

        std::uint64_t storeSize = 0;
    };

    enum class RetireReason : std::uint8_t
    {
        Superseded, // 同じ放送時間帯が別イベントへ差し替え
        Cancelled   // 完全受信sectionから消え、代替イベントも存在しない
    };

    struct RetiredEvent
    {
        ParsedEITPacket event{};
        RetireReason reason = RetireReason::Superseded;
    };

    // 空sectionも表現できる編成更新入力。
    // windowBeginRaw40 / windowEndRaw40 は、このsectionが責任を持つ
    // 時間帯を呼び出し側で設定する。endは範囲外境界とする。
    struct ReceivedSectionSnapshot
    {
        std::uint16_t originalNetworkId = 0;
        std::uint16_t transportStreamId = 0;
        std::uint16_t serviceId = 0;
        std::uint8_t tableId = 0;
        std::uint8_t sectionNumber = 0;
        std::uint8_t version = 0;
        bool currentNext = false;
        bool complete = false;
        // 受信順。0の場合はApplySectionSnapshots()内の入力順を使用する。
        std::uint64_t receptionSequence = 0;
        std::uint64_t windowBeginRaw40 = 0;
        std::uint64_t windowEndRaw40 = 0;
        std::vector<ParsedEITPacket> events;
    };

public:
    void Reset();

    // 初回/更新 共通：新規取得イベントを適用
    UpdateStats Apply(const std::vector<ParsedEITPacket>& newEvents, const UpdateOptions& opt);

    // 編成変更用。complete=trueならeventsが空でも放送中止を検出できる。
    UpdateStats ApplySectionSnapshots(
        const std::vector<ReceivedSectionSnapshot>& snapshots,
        const UpdateOptions& opt);

    // oldデータ書込側が回収する。回収後は内部キューを手動クリアする。
    std::vector<RetiredEvent> TakeRetiredEvents();

    // 現在のスナップショット（必要なら呼び出し側で SortUnit を通す）
    std::vector<ParsedEITPacket> ExportSnapshotSorted() const;

    std::size_t Size() const noexcept;

    // SortUnit用の公開窓口。version判定本体は内部ヘルパーへ集約する。
    static std::vector<ParsedEITPacket>
        SelectLatestSubtableEventsForSort(
            const std::vector<ParsedEITPacket>& events,
            std::uint64_t* droppedCount = nullptr);

private:
    // ---- key types ----
    struct LogicalKey
    {
        std::uint16_t onid = 0;
        std::uint16_t tsid = 0;
        std::uint16_t sid = 0;
        std::uint16_t eid = 0;

        bool operator==(const LogicalKey& o) const noexcept
        {
            return onid == o.onid && tsid == o.tsid && sid == o.sid && eid == o.eid;
        }
    };
    struct LogicalKeyHash
    {
        std::size_t operator()(const LogicalKey& k) const noexcept
        {
            std::uint64_t x = 0;
            x ^= (std::uint64_t)k.onid << 48;
            x ^= (std::uint64_t)k.tsid << 32;
            x ^= (std::uint64_t)k.sid << 16;
            x ^= (std::uint64_t)k.eid;
            x ^= (x >> 33);
            x *= 0xff51afd7ed558ccdULL;
            x ^= (x >> 33);
            return (std::size_t)x;
        }
    };

    struct StrongKey
    {
        LogicalKey lk{};
        std::uint64_t start_raw = 0; // 正規化済み

        bool operator==(const StrongKey& o) const noexcept
        {
            return lk == o.lk && start_raw == o.start_raw;
        }
    };
    struct StrongKeyHash
    {
        std::size_t operator()(const StrongKey& k) const noexcept
        {
            std::size_t h1 = LogicalKeyHash{}(k.lk);
            std::size_t h2 = std::hash<std::uint64_t>{}(k.start_raw);
            // size_t is 32-bit on Win32 and 64-bit on x64.
            // Retain the original low bits of the hash on either target.
            constexpr std::size_t kMix =
                static_cast<std::size_t>(0x9e3779b97f4a7c15ULL);
            return h1 ^ (h2 + kMix + (h1 << 6) + (h1 >> 2));
        }
    };

    struct ServiceKey
    {
        std::uint16_t onid = 0;
        std::uint16_t tsid = 0;
        std::uint16_t sid = 0;

        bool operator==(const ServiceKey& o) const noexcept
        {
            return onid == o.onid &&
                tsid == o.tsid &&
                sid == o.sid;
        }
    };

    struct ServiceKeyHash
    {
        std::size_t operator()(const ServiceKey& k) const noexcept
        {
            std::uint64_t x = 0;
            x ^= static_cast<std::uint64_t>(k.onid) << 32;
            x ^= static_cast<std::uint64_t>(k.tsid) << 16;
            x ^= static_cast<std::uint64_t>(k.sid);
            x ^= (x >> 33);
            x *= 0xff51afd7ed558ccdULL;
            x ^= (x >> 33);
            return static_cast<std::size_t>(x);
        }
    };

    struct Entry
    {
        ParsedEITPacket ev{};
    };

    struct SectionKey
    {
        std::uint16_t onid = 0;
        std::uint16_t tsid = 0;
        std::uint16_t sid = 0;
        std::uint8_t tableId = 0;
        std::uint8_t sectionNumber = 0;

        bool operator==(const SectionKey& o) const noexcept
        {
            return onid == o.onid &&
                tsid == o.tsid &&
                sid == o.sid &&
                tableId == o.tableId &&
                sectionNumber == o.sectionNumber;
        }
    };

    struct SectionKeyHash
    {
        std::size_t operator()(const SectionKey& k) const noexcept
        {
            std::uint64_t x = 0;
            x ^= static_cast<std::uint64_t>(k.onid) << 48;
            x ^= static_cast<std::uint64_t>(k.tsid) << 32;
            x ^= static_cast<std::uint64_t>(k.sid) << 16;
            x ^= static_cast<std::uint64_t>(k.tableId) << 8;
            x ^= static_cast<std::uint64_t>(k.sectionNumber);
            x ^= (x >> 33);
            x *= 0xff51afd7ed558ccdULL;
            x ^= (x >> 33);
            return static_cast<std::size_t>(x);
        }
    };

    // EIT version_number の選択だけを担当する内部ヘルパー。
    // 不具合時に更新本体から切り離して検証できるよう、状態と判定を集約する。
    class SectionVersionHelper
    {
    public:
        struct Scope
        {
            std::uint16_t originalNetworkId = 0;
            std::uint16_t transportStreamId = 0;
            std::uint16_t serviceId = 0;
            std::uint8_t tableId = 0;

            bool operator==(const Scope& other) const noexcept;
        };

        struct Candidate
        {
            std::uint8_t version = 0;
            std::uint64_t receptionSequence = 0;
            std::size_t sourceIndex = 0;
        };

        struct Selection
        {
            std::optional<std::size_t> sourceIndex;
            std::uint8_t version = 0;
            bool wrapDetected = false;
            bool stale = false;
        };

        void Reset() noexcept;

        Selection SelectLatest(
            const Scope& scope,
            const std::vector<Candidate>& candidates);

    private:
        struct ScopeHash
        {
            std::size_t operator()(const Scope& scope) const noexcept;
        };

        struct State
        {
            bool initialized = false;
            std::uint8_t latestVersion = 0;
            std::uint64_t latestReceptionSequence = 0;
            std::uint64_t completedWraps = 0;
        };

        std::unordered_map<Scope, State, ScopeHash> states_;
    };

private:
    // strong store: (onid,tsid,sid,eid,start) -> event
    std::unordered_map<StrongKey, Entry, StrongKeyHash> m_store;

    // service-time conflict index:
// (onid,tsid,sid) -> strong keys
// EventID が変わる未定枠更新を検出するために使う。
    std::unordered_map<ServiceKey, std::vector<StrongKey>, ServiceKeyHash> m_serviceIndex;

    // logical index: (onid,tsid,sid,eid) -> list of start_raw
    std::unordered_map<LogicalKey, std::vector<std::uint64_t>, LogicalKeyHash> m_index;

    std::vector<RetiredEvent> m_retiredEvents;
    SectionVersionHelper m_sectionVersionHelper;

private:
    // helpers
    static bool IsValidStartRaw40(std::uint64_t raw40) noexcept;
    static std::uint64_t NormalizeStartRaw40(std::uint64_t raw40, const UpdateOptions& opt) noexcept;
    static bool IsVersionNewer5Bit(std::uint8_t candidate, std::uint8_t current) noexcept;

    static std::uint32_t DurationSecOf(const ParsedEITPacket& e) noexcept;
    static bool DurationNear(const ParsedEITPacket& a, const ParsedEITPacket& b, std::uint32_t tolSec) noexcept;

    // start_time_raw40 の差（秒）: 精密変換はしない。Bcd hhmmss の差分だけ使う簡易。
    static std::optional<std::int64_t> StartTimeDeltaSecRough(const ParsedEITPacket& a, const ParsedEITPacket& b) noexcept;

    static std::int64_t CalcTextScore(const ParsedEITPacket& e) noexcept;

    struct TimeRange
    {
        std::int64_t beginSec = 0;
        std::int64_t endSec = 0;
    };

    static std::wstring CompactTitleForPlaceholderCheck(std::wstring_view src);
    static bool IsGenericPlaceholderTitle(std::wstring_view title);
    static bool IsGenericPlaceholderEvent(const ParsedEITPacket& e);
    static bool HasConcreteTitle(const ParsedEITPacket& e);

    static std::optional<std::int64_t> StartRaw40ToSeconds(std::uint64_t raw40) noexcept;
    static std::optional<TimeRange> GetTimeRange(const ParsedEITPacket& e) noexcept;
    static bool EventsOverlapEnough(
        const ParsedEITPacket& a,
        const ParsedEITPacket& b,
        std::uint32_t minOverlapSec) noexcept;

    static ServiceKey MakeServiceKey(const LogicalKey& lk) noexcept;
    static ServiceKey MakeServiceKey(const ParsedEITPacket& e) noexcept;
    static SectionKey MakeSectionKey(const ParsedEITPacket& e) noexcept;

    static std::uint64_t MergePreferBetter(ParsedEITPacket& dst, const ParsedEITPacket& src, const UpdateOptions& opt, std::uint64_t& outMergedItems);

    bool IndexContainsStart(const LogicalKey& lk, std::uint64_t start) const;
    void IndexAdd(const LogicalKey& lk, std::uint64_t start);
    void IndexRemoveOne(const LogicalKey& lk, std::uint64_t start);

    void ServiceIndexAdd(const ServiceKey& skey, const StrongKey& sk);
    void ServiceIndexRemoveOne(const ServiceKey& skey, const StrongKey& sk);

    void AddStoreEntry(const StrongKey& sk, const ParsedEITPacket& e);
    bool RemoveStoreEntry(const StrongKey& sk);

    bool ShouldDropIncomingByServiceTime(
        const ParsedEITPacket& incoming,
        const UpdateOptions& opt) const;

    std::vector<StrongKey> FindStaleGenericPlaceholderKeysByServiceTime(
        const ParsedEITPacket& incoming,
        const UpdateOptions& opt) const;

    std::vector<StrongKey> FindOlderConcreteConflictKeysAtSameStart(
        const ParsedEITPacket& incoming,
        const UpdateOptions& opt,
        bool& outDropIncoming) const;

    // 差し替え候補探索（同一 logical の別 start を検索）
    std::optional<std::uint64_t> FindReplaceCandidateStart(const LogicalKey& lk, const ParsedEITPacket& incoming, const UpdateOptions& opt) const;

    static LogicalKey MakeLogicalKey(const ParsedEITPacket& e) noexcept;
};

#endif // EIT_EVENT_UPDATE_UNIT_HPP
