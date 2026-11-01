// ================================
// EITEventSortUnit.cpp (REPLACE)
// H = authoritative / L = patch
// ================================
#include "EITEventSortUnit.hpp"

#include <vector>
#include <map>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <iterator>

#include "EitEventUpdateUnit.hpp"

#if __has_include("DebugManager.h")
#include "DebugManager.h"
#define EITSORT_HAVE_DEBUG 1
#else
#define EITSORT_HAVE_DEBUG 0
#endif

#if EITSORT_HAVE_DEBUG
#define EITSORT_LOGFMT(...) DebugLogFormat(__VA_ARGS__)
#else
#define EITSORT_LOGFMT(...) do {} while(0)
#endif

namespace {

    std::atomic<bool> g_EitSortDebugLogEnabled{ false };
    EITEventSortUnit::DebugCounters g_dbg{};
    std::mutex g_dbgMutex;

    constexpr bool IsScheduleTableId(uint8_t table_id) noexcept { return (table_id >= 0x50 && table_id <= 0x5F); }
    constexpr bool IsScheduleOtherTableId(uint8_t table_id) noexcept { return (table_id >= 0x60 && table_id <= 0x6F); }
    constexpr bool IsPfActual(uint8_t table_id) noexcept { return table_id == 0x4E; }
    constexpr bool IsPfOther(uint8_t table_id) noexcept { return table_id == 0x4F; }
    constexpr bool IsAnySchedule(uint8_t table_id) noexcept { return IsScheduleTableId(table_id) || IsScheduleOtherTableId(table_id); }

    inline int TableRankForBase(uint8_t table_id) noexcept
    {
        if (IsScheduleTableId(table_id)) return 4;
        if (IsScheduleOtherTableId(table_id)) return 3;
        if (IsPfActual(table_id)) return 2;
        if (IsPfOther(table_id)) return 1;
        return 0;
    }

    inline bool HasNameBytes(const ParsedEITPacket& ev) noexcept { return !ev.short_event_name_bytes.empty(); }
    inline bool HasTitleUtf16(const ParsedEITPacket& ev) noexcept { return !ev.title_utf16.empty(); }

    inline bool HasAnyText(const ParsedEITPacket& ev) noexcept
    {
        // ★タイトル相当は「short_event_name_bytes」を最優先（出力に使うのがこちらの想定）
        return HasNameBytes(ev)
            || HasTitleUtf16(ev)
            || !ev.short_text_bytes.empty()
            || !ev.extended_text_bytes.empty()
            || !ev.extended_items.empty();
    }

    inline long long CalcTextScore(const ParsedEITPacket& ev) noexcept
    {
        long long score = 0;
        if (!ev.short_event_name_bytes.empty()) { score += 1000; score += (long long)ev.short_event_name_bytes.size(); }
        if (!ev.short_text_bytes.empty()) { score += 400;  score += (long long)ev.short_text_bytes.size(); }
        if (!ev.extended_text_bytes.empty()) { score += 800;  score += (long long)ev.extended_text_bytes.size(); }
        if (!ev.extended_items.empty()) {
            score += (long long)ev.extended_items.size() * 500;
            for (const auto& item : ev.extended_items) {
                score += (long long)item.item_description_bytes.size();
                score += (long long)item.item_text_bytes.size() * 2;
            }
        }
        score += (long long)ev.descriptors.size();
        return score;
    }

    inline bool BetterPickTextOnly(const ParsedEITPacket& a, const ParsedEITPacket& b) noexcept
    {
        // ★ raw name bytes を優先（短形式タイトル相当）
        const bool aName = HasNameBytes(a);
        const bool bName = HasNameBytes(b);
        if (aName != bName) return aName > bName;

        const bool aTitle = HasTitleUtf16(a);
        const bool bTitle = HasTitleUtf16(b);
        if (aTitle != bTitle) return aTitle > bTitle;

        const long long sa = CalcTextScore(a);
        const long long sb = CalcTextScore(b);
        if (sa != sb) return sa > sb;

        return a.table_id < b.table_id;
    }

    inline bool BetterPickBase(const ParsedEITPacket& a, const ParsedEITPacket& b, bool preferSchedule) noexcept
    {
        if (preferSchedule) {
            const int ra = TableRankForBase(a.table_id);
            const int rb = TableRankForBase(b.table_id);
            if (ra != rb) return ra > rb;
        }
        return BetterPickTextOnly(a, b);
    }

    inline bool IsValidStartTimeRaw(uint64_t raw40) noexcept { return raw40 != 0 && raw40 != 0xFFFFFFFFFFull; }
    inline bool IsValidDurationSec(uint32_t sec) noexcept { return (sec > 0 && sec <= 24u * 3600u); }

    inline uint32_t DurationSecOf(const ParsedEITPacket& e) noexcept
    {
        if (IsValidDurationSec((uint32_t)e.duration)) return (uint32_t)e.duration;

        const uint32_t hhmmss = e.duration_hhmmss;
        const uint32_t h = (hhmmss / 10000u) % 100u;
        const uint32_t m = (hhmmss / 100u) % 100u;
        const uint32_t s = (hhmmss / 1u) % 100u;
        if (h > 23 || m > 59 || s > 59) return 0;
        const uint32_t sec = h * 3600u + m * 60u + s;
        return IsValidDurationSec(sec) ? sec : 0;
    }

    inline bool DurationNear(const ParsedEITPacket& a, const ParsedEITPacket& b, uint32_t maxDiffSec) noexcept
    {
        const uint32_t sa = DurationSecOf(a);
        const uint32_t sb = DurationSecOf(b);
        if (sa == 0 || sb == 0) return false;
        const uint32_t diff = (sa > sb) ? (sa - sb) : (sb - sa);
        return diff <= maxDiffSec;
    }

    // ---- key normalize（start_time_raw が無効なときキーを寄せる）----
    inline EITEventKey MakeKeyNormalized(const ParsedEITPacket& ev, const EITEventSortUnit::SortOptions& opt) noexcept
    {
        EITEventKey k(ev);
        if (opt.relaxKeyWhenInvalidStartTime && !IsValidStartTimeRaw(k.start_time_raw)) {
            k.start_time_raw = 0;
        }
        return k;
    }

    // ---- extended_items merge (patch用) ----
    static uint64_t Fnv1a64(const uint8_t* p, std::size_t n) noexcept
    {
        uint64_t h = 1469598103934665603ull;
        for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    }

    static uint64_t HashItem(const EitExtendedItem& it) noexcept
    {
        uint64_t h = 0;
        if (!it.item_description_bytes.empty())
            h ^= Fnv1a64(it.item_description_bytes.data(), it.item_description_bytes.size());
        h ^= 0x9E3779B97F4A7C15ull;
        if (!it.item_text_bytes.empty())
            h ^= Fnv1a64(it.item_text_bytes.data(), it.item_text_bytes.size());
        return h;
    }

    static std::size_t MergeExtendedItemsPatch(std::vector<EitExtendedItem>& dst, const std::vector<EitExtendedItem>& src)
    {
        if (src.empty()) return 0;

        constexpr std::size_t kMaxItems = 32;
        if (dst.size() >= kMaxItems) return 0;

        std::unordered_set<uint64_t> seen;
        seen.reserve(dst.size() + src.size());
        for (const auto& d : dst) seen.insert(HashItem(d));

        std::size_t added = 0;
        for (const auto& s : src) {
            if (dst.size() >= kMaxItems) break;
            const uint64_t hs = HashItem(s);
            if (seen.insert(hs).second) {
                dst.push_back(s);
                ++added;
            }
        }
        return added;
    }

    static std::uint64_t ApplyPatchToEvent(
        ParsedEITPacket& dst,
        const ParsedEITPacket& src,
        const EITEventSortUnit::SortOptions& opt,
        std::uint64_t& out_itemsMerged)
    {
        std::uint64_t filled = 0;
        out_itemsMerged = 0;

        if (!IsValidStartTimeRaw(dst.start_time) && IsValidStartTimeRaw(src.start_time)) { dst.start_time = src.start_time; ++filled; }
        if (!IsValidDurationSec((uint32_t)dst.duration) && IsValidDurationSec((uint32_t)src.duration)) { dst.duration = src.duration; ++filled; }
        if (dst.duration_hhmmss == 0 && src.duration_hhmmss != 0) { dst.duration_hhmmss = src.duration_hhmmss; ++filled; }

        if (dst.title_utf16.empty() && !src.title_utf16.empty()) { dst.title_utf16 = src.title_utf16; ++filled; }
        if (dst.short_event_name_bytes.empty() && !src.short_event_name_bytes.empty()) { dst.short_event_name_bytes = src.short_event_name_bytes; ++filled; }
        if (dst.short_text_bytes.empty() && !src.short_text_bytes.empty()) { dst.short_text_bytes = src.short_text_bytes; ++filled; }

        if (dst.extended_text_bytes.empty()) {
            if (!src.extended_text_bytes.empty()) { dst.extended_text_bytes = src.extended_text_bytes; ++filled; }
        }
        else if (opt.patchAllowUpgradeLongerText) {
            if (src.extended_text_bytes.size() > dst.extended_text_bytes.size() + opt.patchUpgradeMinGrowBytes) {
                dst.extended_text_bytes = src.extended_text_bytes;
                ++filled;
            }
        }

        if (dst.extended_items.empty()) {
            if (!src.extended_items.empty()) { dst.extended_items = src.extended_items; ++filled; }
        }
        else if (opt.patchMergeExtendedItems && !src.extended_items.empty()) {
            out_itemsMerged = MergeExtendedItemsPatch(dst.extended_items, src.extended_items);
        }

        if (dst.descriptors.empty() && !src.descriptors.empty()) { dst.descriptors = src.descriptors; ++filled; }

        if (!dst.has_genre && src.has_genre) {
            dst.has_genre = true;
            dst.content_nibble_level1 = src.content_nibble_level1;
            dst.content_nibble_level2 = src.content_nibble_level2;
            dst.user_nibble1 = src.user_nibble1;
            dst.user_nibble2 = src.user_nibble2;
            ++filled;
        }
        if (!dst.has_series && src.has_series) {
            dst.has_series = true;
            dst.series_id = src.series_id;
            dst.repeat_label = src.repeat_label;
            dst.program_pattern = src.program_pattern;
            dst.series_expire_valid = src.series_expire_valid;
            dst.series_expire_mjd = src.series_expire_mjd;
            dst.episode_number = src.episode_number;
            dst.last_episode_number = src.last_episode_number;
            dst.series_name_bytes = src.series_name_bytes;
            ++filled;
        }
        if (!dst.has_parental_rating && src.has_parental_rating) {
            dst.has_parental_rating = true;
            dst.parental_country = src.parental_country;
            dst.parental_rating = src.parental_rating;
            ++filled;
        }

        return filled;
    }

    template<class MemberPtr>
    static const ParsedEITPacket* PickBestNonEmptyPreferVer(
        const std::vector<const ParsedEITPacket*>& vec,
        MemberPtr member,
        uint8_t preferVer) noexcept
    {
        const ParsedEITPacket* best = nullptr;
        for (auto* p : vec) {
            if (!p) continue;
            const auto& v = p->*member;
            if (v.empty()) continue;

            if (!best) { best = p; continue; }

            const bool pPref = (p->version == preferVer);
            const bool bPref = (best->version == preferVer);
            if (pPref != bPref) { if (pPref) best = p; continue; }

            const auto& b = best->*member;
            if (v.size() != b.size()) best = (v.size() > b.size()) ? p : best;
            else if (BetterPickTextOnly(*p, *best)) best = p;
        }
        return best;
    }

    static const ParsedEITPacket* PickBestGenrePreferVer(const std::vector<const ParsedEITPacket*>& vec, uint8_t preferVer) noexcept
    {
        const ParsedEITPacket* best = nullptr;
        for (auto* p : vec) {
            if (!p || !p->has_genre) continue;
            if (!best) { best = p; continue; }

            const bool pPref = (p->version == preferVer);
            const bool bPref = (best->version == preferVer);
            if (pPref != bPref) { if (pPref) best = p; continue; }

            if (BetterPickTextOnly(*p, *best)) best = p;
        }
        return best;
    }

    static const ParsedEITPacket* PickBestItemsPreferVer(const std::vector<const ParsedEITPacket*>& vec, uint8_t preferVer) noexcept
    {
        const ParsedEITPacket* best = nullptr;
        for (auto* p : vec) {
            if (!p || p->extended_items.empty()) continue;
            if (!best) { best = p; continue; }

            const bool pPref = (p->version == preferVer);
            const bool bPref = (best->version == preferVer);
            if (pPref != bPref) { if (pPref) best = p; continue; }

            if (p->extended_items.size() != best->extended_items.size())
                best = (p->extended_items.size() > best->extended_items.size()) ? p : best;
            else if (p->extended_text_bytes.size() != best->extended_text_bytes.size())
                best = (p->extended_text_bytes.size() > best->extended_text_bytes.size()) ? p : best;
            else if (BetterPickTextOnly(*p, *best))
                best = p;
        }
        return best;
    }

    // ---- rescue result (per field) ----
    struct GroupRescueStats
    {
        std::uint64_t total = 0;
        std::uint64_t title = 0;
        std::uint64_t name = 0;
        std::uint64_t shortText = 0;
        std::uint64_t genre = 0;
        std::uint64_t extText = 0;
        std::uint64_t extItems = 0;
    };

    static GroupRescueStats RescueFieldsFromGroup(
        ParsedEITPacket& merged,
        const std::vector<const ParsedEITPacket*>& vec,
        uint8_t preferVer) noexcept
    {
        GroupRescueStats st{};

        // title_utf16
        if (merged.title_utf16.empty()) {
            if (const auto* t = PickBestNonEmptyPreferVer(vec, &ParsedEITPacket::title_utf16, preferVer)) {
                merged.title_utf16 = t->title_utf16;
                ++st.total; ++st.title;
            }
        }
        // short_event_name_bytes（短形式タイトル相当）
        if (merged.short_event_name_bytes.empty()) {
            if (const auto* n = PickBestNonEmptyPreferVer(vec, &ParsedEITPacket::short_event_name_bytes, preferVer)) {
                merged.short_event_name_bytes = n->short_event_name_bytes;
                ++st.total; ++st.name;
            }
        }
        // short_text_bytes（短形式概要）
        if (merged.short_text_bytes.empty()) {
            if (const auto* s = PickBestNonEmptyPreferVer(vec, &ParsedEITPacket::short_text_bytes, preferVer)) {
                merged.short_text_bytes = s->short_text_bytes;
                ++st.total; ++st.shortText;
            }
        }
        // genre
        if (!merged.has_genre) {
            if (const auto* g = PickBestGenrePreferVer(vec, preferVer)) {
                merged.has_genre = true;
                merged.content_nibble_level1 = g->content_nibble_level1;
                merged.content_nibble_level2 = g->content_nibble_level2;
                merged.user_nibble1 = g->user_nibble1;
                merged.user_nibble2 = g->user_nibble2;
                ++st.total; ++st.genre;
            }
        }
        // extended_text_bytes
        if (merged.extended_text_bytes.empty()) {
            if (const auto* e = PickBestNonEmptyPreferVer(vec, &ParsedEITPacket::extended_text_bytes, preferVer)) {
                merged.extended_text_bytes = e->extended_text_bytes;
                ++st.total; ++st.extText;
            }
        }
        // extended_items
        if (merged.extended_items.empty()) {
            if (const auto* it = PickBestItemsPreferVer(vec, preferVer)) {
                merged.extended_items = it->extended_items;
                ++st.total; ++st.extItems;
            }
        }
        return st;
    }

    template<class Container>
    inline bool UpgradeIfLonger(Container& dst, const Container& src, std::size_t minGrow) noexcept
    {
        if (src.empty()) return false;
        if (dst.empty()) { dst = src; return true; }
        if (src.size() > dst.size() + minGrow) { dst = src; return true; }
        return false;
    }

    // ---- cross-service helper keys ----
    struct ServiceKey
    {
        uint16_t onid = 0, tsid = 0, sid = 0;
        bool operator==(const ServiceKey& o) const noexcept { return onid == o.onid && tsid == o.tsid && sid == o.sid; }
    };
    struct ServiceKeyHash
    {
        std::size_t operator()(const ServiceKey& k) const noexcept
        {
            uint64_t x = 0;
            x ^= (uint64_t)k.onid << 32;
            x ^= (uint64_t)k.tsid << 16;
            x ^= (uint64_t)k.sid;
            x ^= (x >> 33);
            x *= 0xc4ceb9fe1a85ec53ULL;
            x ^= (x >> 33);
            return (std::size_t)x;
        }
    };

    struct ProgramKey
    {
        uint16_t onid = 0, tsid = 0;
        uint16_t event_id = 0;
        uint64_t start_time_raw = 0;
        uint32_t duration_sec = 0;

        bool operator==(const ProgramKey& o) const noexcept
        {
            return onid == o.onid
                && tsid == o.tsid
                && event_id == o.event_id
                && start_time_raw == o.start_time_raw
                && duration_sec == o.duration_sec;
        }
    };
    struct ProgramKeyHash
    {
        std::size_t operator()(const ProgramKey& k) const noexcept
        {
            uint64_t x = 0;
            x ^= (uint64_t)k.onid << 48;
            x ^= (uint64_t)k.tsid << 32;
            x ^= (uint64_t)k.event_id << 16;
            x ^= (uint64_t)(k.start_time_raw * 0x9e3779b97f4a7c15ULL);
            x ^= (uint64_t)k.duration_sec;
            x ^= (x >> 33);
            x *= 0xff51afd7ed558ccdULL;
            x ^= (x >> 33);
            return (std::size_t)x;
        }
    };

    inline ProgramKey MakeProgKey(const ParsedEITPacket& e) noexcept
    {
        return ProgramKey{ e.original_network_id, e.transport_stream_id, e.event_id, e.start_time, DurationSecOf(e) };
    }

    inline std::uint64_t FillMissingTextFields(ParsedEITPacket& dst, const ParsedEITPacket& src) noexcept
    {
        std::uint64_t filled = 0;

        if (dst.title_utf16.empty() && !src.title_utf16.empty()) { dst.title_utf16 = src.title_utf16; ++filled; }
        if (dst.short_event_name_bytes.empty() && !src.short_event_name_bytes.empty()) { dst.short_event_name_bytes = src.short_event_name_bytes; ++filled; }
        if (dst.short_text_bytes.empty() && !src.short_text_bytes.empty()) { dst.short_text_bytes = src.short_text_bytes; ++filled; }
        if (dst.extended_text_bytes.empty() && !src.extended_text_bytes.empty()) { dst.extended_text_bytes = src.extended_text_bytes; ++filled; }
        if (dst.extended_items.empty() && !src.extended_items.empty()) { dst.extended_items = src.extended_items; ++filled; }
        if (dst.descriptors.empty() && !src.descriptors.empty()) { dst.descriptors = src.descriptors; ++filled; }

        return filled;
    }

} // namespace

void EITEventSortUnit::EnableDebugLog(bool enable) noexcept
{
    g_EitSortDebugLogEnabled.store(enable, std::memory_order_relaxed);
}

void EITEventSortUnit::ResetDebugCounters() noexcept
{
    std::lock_guard<std::mutex> lk(g_dbgMutex);
    g_dbg = DebugCounters{};
}

EITEventSortUnit::DebugCounters EITEventSortUnit::GetDebugCountersSnapshot() noexcept
{
    std::lock_guard<std::mutex> lk(g_dbgMutex);
    return g_dbg;
}

void EITEventSortUnit::DumpDebugCounters()
{
#if EITSORT_HAVE_DEBUG
    const auto d = GetDebugCountersSnapshot();
    DebugLogFormat(
        L"[EIT-SORT] calls=%llu rawH=%llu rawL=%llu groupsH=%llu multiH=%llu maxCandH=%llu mergesH=%llu fallbackH=%llu "
        L"patchMatch=%llu patchApplied=%llu patchFill=%llu patchItemsMerged=%llu patchOnlyAdded=%llu "
        L"xsPairs=%llu xsResEv=%llu xsResFields=%llu "
        L"grTitle=%llu grName=%llu grSum=%llu grGenre=%llu grExtTxt=%llu grExtIt=%llu grTotal=%llu "
        L"missTitle=%llu missSum=%llu missGenre=%llu missExtIt=%llu resultTotal=%llu verDrop=%llu",
        (unsigned long long)d.calls,
        (unsigned long long)d.rawEventsTotalH,
        (unsigned long long)d.rawEventsTotalL,
        (unsigned long long)d.groupsH,
        (unsigned long long)d.multiCandidateGroupsH,
        (unsigned long long)d.maxCandidatesInGroupH,
        (unsigned long long)d.mergesTriedH,
        (unsigned long long)d.fallbackBaseToOlderDueToEmptyH,
        (unsigned long long)d.patchGroupsMatched,
        (unsigned long long)d.patchEventsApplied,
        (unsigned long long)d.patchFieldsFilled,
        (unsigned long long)d.patchItemsMerged,
        (unsigned long long)d.patchOnlyAdded,
        (unsigned long long)d.crossServiceReplicaPairs,
        (unsigned long long)d.crossServiceRescuedEvents,
        (unsigned long long)d.crossServiceRescuedFields,
        (unsigned long long)d.groupRescueTitle,
        (unsigned long long)d.groupRescueName,
        (unsigned long long)d.groupRescueShortText,
        (unsigned long long)d.groupRescueGenre,
        (unsigned long long)d.groupRescueExtText,
        (unsigned long long)d.groupRescueExtItems,
        (unsigned long long)d.groupFieldRescueFilled,
        (unsigned long long)d.resultMissingTitle,
        (unsigned long long)d.resultMissingSummary,
        (unsigned long long)d.resultMissingGenre,
        (unsigned long long)d.resultMissingExtItems,
        (unsigned long long)d.resultEventsTotal,
        (unsigned long long)d.versionCandidatesDropped
    );
#endif
}

std::vector<ParsedEITPacket>
EITEventSortUnit::BuildUniqueSortedEvents(const std::vector<ParsedEITPacket>& authoritativeEvents)
{
    SortOptions opt;
    return BuildUniqueSortedEvents(authoritativeEvents, {}, opt);
}

std::vector<ParsedEITPacket>
EITEventSortUnit::BuildUniqueSortedEvents(const std::vector<ParsedEITPacket>& authoritativeEvents, const SortOptions& opt)
{
    return BuildUniqueSortedEvents(authoritativeEvents, {}, opt);
}

std::vector<ParsedEITPacket>
EITEventSortUnit::BuildUniqueSortedEvents(const std::vector<ParsedEITPacket>& authoritativeEvents,
    const std::vector<ParsedEITPacket>& patchEvents)
{
    SortOptions opt;
    return BuildUniqueSortedEvents(authoritativeEvents, patchEvents, opt);
}

std::vector<ParsedEITPacket>
EITEventSortUnit::BuildUniqueSortedEvents(const std::vector<ParsedEITPacket>& authoritativeEvents,
    const std::vector<ParsedEITPacket>& patchEvents,
    const SortOptions& opt)
{
    {
        std::lock_guard<std::mutex> lk(g_dbgMutex);
        ++g_dbg.calls;
        g_dbg.rawEventsTotalH += (std::uint64_t)authoritativeEvents.size();
        g_dbg.rawEventsTotalL += (std::uint64_t)patchEvents.size();
    }

    std::vector<ParsedEITPacket> versionSelectedEvents;
    const std::vector<ParsedEITPacket>* eventsForSort =
        &authoritativeEvents;

    if (opt.enableSubtableVersionSelection) {
        std::uint64_t dropped = 0;
        versionSelectedEvents =
            EitEventUpdateUnit::
            SelectLatestSubtableEventsForSort(
                authoritativeEvents,
                &dropped);
        {
            std::lock_guard<std::mutex> lk(g_dbgMutex);
            g_dbg.versionCandidatesDropped += dropped;
        }
        eventsForSort = &versionSelectedEvents;
    }

    // SortUnit自身は選択済みイベントの重複統合と時間順配置だけを担当する。
    std::map<EITEventKey, std::vector<const ParsedEITPacket*>, EITEventKeyLess> groupsH;
    for (const auto& ev : *eventsForSort) {
        groupsH[MakeKeyNormalized(ev, opt)].push_back(&ev);
    }

    std::uint64_t multiGroupsThisCall = 0;
    std::uint64_t maxCandThisCall = 0;
    for (const auto& kv : groupsH) {
        const std::uint64_t n = (std::uint64_t)kv.second.size();
        if (n >= 2) ++multiGroupsThisCall;
        if (n > maxCandThisCall) maxCandThisCall = n;
    }
    {
        std::lock_guard<std::mutex> lk(g_dbgMutex);
        g_dbg.groupsH += (std::uint64_t)groupsH.size();
        g_dbg.multiCandidateGroupsH += multiGroupsThisCall;
        if (maxCandThisCall > g_dbg.maxCandidatesInGroupH) g_dbg.maxCandidatesInGroupH = maxCandThisCall;
    }

    std::vector<ParsedEITPacket> result;
    result.reserve(groupsH.size());

    std::uint64_t mergesThisCall = 0;
    std::uint64_t fallbackBaseThisCall = 0;

    std::uint64_t groupRescueTitleThisCall = 0;
    std::uint64_t groupRescueNameThisCall = 0;
    std::uint64_t groupRescueSumThisCall = 0;
    std::uint64_t groupRescueGenreThisCall = 0;
    std::uint64_t groupRescueExtTxtThisCall = 0;
    std::uint64_t groupRescueExtItThisCall = 0;
    std::uint64_t groupRescueTotalThisCall = 0;

    const auto mergePreferH = [&](ParsedEITPacket& dst, const ParsedEITPacket& src)
        {
            {
                std::lock_guard<std::mutex> lk(g_dbgMutex);
                ++g_dbg.mergesTriedH;
            }
            ++mergesThisCall;

            if (!IsValidStartTimeRaw(dst.start_time) && IsValidStartTimeRaw(src.start_time)) dst.start_time = src.start_time;
            if (!IsValidDurationSec((uint32_t)dst.duration) && IsValidDurationSec((uint32_t)src.duration)) dst.duration = src.duration;
            if (dst.duration_hhmmss == 0 && src.duration_hhmmss != 0) dst.duration_hhmmss = src.duration_hhmmss;

            // ★短形式は「空のみ差し替え」を基本（破損/空だけ入れ替える）
            if (opt.lockNonEmptyShortFieldsInHMerge) {
                if (dst.title_utf16.empty() && !src.title_utf16.empty())
                    dst.title_utf16 = src.title_utf16;
                else if (opt.allowUpgradeTitleIfLonger)
                    UpgradeIfLonger(dst.title_utf16, src.title_utf16, opt.upgradeMinGrowTitleUtf16);

                if (dst.short_event_name_bytes.empty() && !src.short_event_name_bytes.empty())
                    dst.short_event_name_bytes = src.short_event_name_bytes;
                else if (opt.allowUpgradeNameIfLonger)
                    UpgradeIfLonger(dst.short_event_name_bytes, src.short_event_name_bytes, opt.upgradeMinGrowNameBytes);

                if (dst.short_text_bytes.empty() && !src.short_text_bytes.empty())
                    dst.short_text_bytes = src.short_text_bytes;
                else if (opt.allowUpgradeShortTextIfLonger)
                    UpgradeIfLonger(dst.short_text_bytes, src.short_text_bytes, opt.upgradeMinGrowShortTextBytes);
            }
            else {
                // 従来挙動（長い方を採用）
                if (!src.title_utf16.empty() && (dst.title_utf16.empty() || src.title_utf16.size() > dst.title_utf16.size()))
                    dst.title_utf16 = src.title_utf16;

                if (!src.short_event_name_bytes.empty() &&
                    (dst.short_event_name_bytes.empty() || src.short_event_name_bytes.size() > dst.short_event_name_bytes.size()))
                    dst.short_event_name_bytes = src.short_event_name_bytes;

                if (!src.short_text_bytes.empty() &&
                    (dst.short_text_bytes.empty() || src.short_text_bytes.size() > dst.short_text_bytes.size()))
                    dst.short_text_bytes = src.short_text_bytes;
            }

            // extended は「長い方」「多い方」を優先（ここは従来通りでOK）
            if (src.extended_text_bytes.size() > dst.extended_text_bytes.size())
                dst.extended_text_bytes = src.extended_text_bytes;

            if (!src.extended_items.empty()) {
                if (dst.extended_items.empty()) dst.extended_items = src.extended_items;
                else if (src.extended_items.size() > dst.extended_items.size()) dst.extended_items = src.extended_items;
            }

            if (dst.descriptors.empty() && !src.descriptors.empty()) dst.descriptors = src.descriptors;

            if (!dst.has_genre && src.has_genre) {
                dst.has_genre = true;
                dst.content_nibble_level1 = src.content_nibble_level1;
                dst.content_nibble_level2 = src.content_nibble_level2;
                dst.user_nibble1 = src.user_nibble1;
                dst.user_nibble2 = src.user_nibble2;
            }
            if (!dst.has_series && src.has_series) {
                dst.has_series = true;
                dst.series_id = src.series_id;
                dst.repeat_label = src.repeat_label;
                dst.program_pattern = src.program_pattern;
                dst.series_expire_valid = src.series_expire_valid;
                dst.series_expire_mjd = src.series_expire_mjd;
                dst.episode_number = src.episode_number;
                dst.last_episode_number = src.last_episode_number;
                dst.series_name_bytes = src.series_name_bytes;
            }
            if (!dst.has_parental_rating && src.has_parental_rating) {
                dst.has_parental_rating = true;
                dst.parental_country = src.parental_country;
                dst.parental_rating = src.parental_rating;
            }
        };

    for (const auto& kv : groupsH)
    {
        const auto& vec = kv.second;
        if (vec.empty()) continue;

        bool hasScheduleAny = false;

        for (const auto* p : vec) {
            if (IsAnySchedule(p->table_id)) {
                hasScheduleAny = true;
            }
        }

        // versionはsub-tableごとに選択済み。異なるtable_id間で
        // versionの数値大小を比較せず、table種別と内容でbaseを選ぶ。
        const ParsedEITPacket* baseSelected = nullptr;
        for (const auto* p : vec) {
            if (!baseSelected ||
                BetterPickBase(
                    *p,
                    *baseSelected,
                    opt.preferScheduleAsBase &&
                    hasScheduleAny)) {
                baseSelected = p;
            }
        }
        if (!baseSelected) baseSelected = vec.front();

        const ParsedEITPacket* bestAnyText = baseSelected;
        for (const auto* p : vec) {
            if (BetterPickTextOnly(*p, *bestAnyText)) bestAnyText = p;
        }

        const ParsedEITPacket* base = baseSelected;

        // 選択済みbaseが欠損し、別tableの同一イベントに文字情報がある場合。
        if (!HasAnyText(*baseSelected) &&
            HasAnyText(*bestAnyText) &&
            bestAnyText != baseSelected) {
            base = bestAnyText;
            ++fallbackBaseThisCall;
            {
                std::lock_guard<std::mutex> lk(g_dbgMutex);
                ++g_dbg.fallbackBaseToOlderDueToEmptyH;
            }
        }

        ParsedEITPacket merged = *base;
        const uint8_t chosenVer = base->version;

        if (hasScheduleAny && opt.preferScheduleAsBase) {
            for (const auto* p : vec) {
                if (IsAnySchedule(p->table_id)) {
                    mergePreferH(merged, *p);
                }
            }
            for (const auto* p : vec) {
                if (IsPfActual(p->table_id) ||
                    IsPfOther(p->table_id)) {
                    mergePreferH(merged, *p);
                }
            }
        }
        else {
            for (const auto* p : vec) {
                mergePreferH(merged, *p);
            }
        }

        // ★グループ内：項目ごとの欠損救済（空だけ埋める）
        if (opt.enableFieldLevelRescueInGroup) {
            const auto st = RescueFieldsFromGroup(merged, vec, chosenVer);
            groupRescueTitleThisCall += st.title;
            groupRescueNameThisCall += st.name;
            groupRescueSumThisCall += st.shortText;
            groupRescueGenreThisCall += st.genre;
            groupRescueExtTxtThisCall += st.extText;
            groupRescueExtItThisCall += st.extItems;
            groupRescueTotalThisCall += st.total;
        }

        if (opt.preferTableId50_51) {
            if (merged.table_id >= 0x58 && merged.table_id <= 0x5F) {
                merged.table_id = (merged.table_id & 0x01) ? 0x51 : 0x50;
            }
        }

        result.push_back(std::move(merged));
    }

    {
        std::lock_guard<std::mutex> lk(g_dbgMutex);
        g_dbg.groupRescueTitle += groupRescueTitleThisCall;
        g_dbg.groupRescueName += groupRescueNameThisCall;
        g_dbg.groupRescueShortText += groupRescueSumThisCall;
        g_dbg.groupRescueGenre += groupRescueGenreThisCall;
        g_dbg.groupRescueExtText += groupRescueExtTxtThisCall;
        g_dbg.groupRescueExtItems += groupRescueExtItThisCall;
        g_dbg.groupFieldRescueFilled += groupRescueTotalThisCall;
    }

    // ============================================================
    // 2) L: patch apply
    // ============================================================
    std::uint64_t patchGroupsMatched = 0;
    std::uint64_t patchEventsApplied = 0;
    std::uint64_t patchFieldsFilled = 0;
    std::uint64_t patchItemsMerged = 0;
    std::uint64_t patchOnlyAdded = 0;

    if (opt.enablePatchApply && !patchEvents.empty())
    {
        std::map<EITEventKey, std::vector<const ParsedEITPacket*>, EITEventKeyLess> groupsL;
        for (const auto& ev : patchEvents) {
            groupsL[MakeKeyNormalized(ev, opt)].push_back(&ev);
        }

        std::map<EITEventKey, std::size_t, EITEventKeyLess> idxH;
        for (std::size_t i = 0; i < result.size(); ++i) {
            idxH.emplace(MakeKeyNormalized(result[i], opt), i);
        }

        for (const auto& kv : groupsL) {
            const auto& key = kv.first;
            const auto& vec = kv.second;
            if (vec.empty()) continue;

            auto itH = idxH.find(key);
            if (itH == idxH.end()) continue;

            const ParsedEITPacket* bestText = vec.front();
            for (const auto* p : vec) {
                if (BetterPickTextOnly(*p, *bestText)) bestText = p;
            }

            const ParsedEITPacket* bestItems = nullptr;
            for (const auto* p : vec) {
                if (p->extended_items.empty()) continue;
                if (!bestItems || p->extended_items.size() > bestItems->extended_items.size()) bestItems = p;
            }

            const ParsedEITPacket* bestGenre = nullptr;
            for (const auto* p : vec) {
                if (!p->has_genre) continue;
                if (!bestGenre ||
                    BetterPickBase(
                        *p,
                        *bestGenre,
                        opt.preferScheduleAsBase)) {
                    bestGenre = p;
                }
            }

            const ParsedEITPacket* bestExtText = nullptr;
            for (const auto* p : vec) {
                if (p->extended_text_bytes.empty()) continue;
                if (!bestExtText || p->extended_text_bytes.size() > bestExtText->extended_text_bytes.size()) bestExtText = p;
            }

            ParsedEITPacket& dst = result[itH->second];

            auto ApplyOne = [&](const ParsedEITPacket* src) {
                if (!src) return;
                std::uint64_t mergedItemsLocal = 0;
                const std::uint64_t filled = ApplyPatchToEvent(dst, *src, opt, mergedItemsLocal);
                patchItemsMerged += mergedItemsLocal;
                patchFieldsFilled += filled;
                if (filled > 0 || mergedItemsLocal > 0) ++patchEventsApplied;
                };

            ApplyOne(bestText);
            ApplyOne(bestItems);
            ApplyOne(bestGenre);
            ApplyOne(bestExtText);

            ++patchGroupsMatched;
        }

        if (opt.allowPatchOnlyEvents) {
            for (const auto& kv : groupsL) {
                const auto& key = kv.first;
                const auto& vec = kv.second;
                if (vec.empty()) continue;

                if (idxH.find(key) != idxH.end()) continue;

                const ParsedEITPacket* best = vec.front();
                for (const auto* p : vec) if (BetterPickTextOnly(*p, *best)) best = p;

                if (!IsValidStartTimeRaw(best->start_time)) continue;
                if (DurationSecOf(*best) == 0) continue;

                ParsedEITPacket add = *best;
                if (opt.preferTableId50_51) {
                    if (add.table_id >= 0x58 && add.table_id <= 0x5F) add.table_id = (add.table_id & 0x01) ? 0x51 : 0x50;
                }

                idxH.emplace(MakeKeyNormalized(add, opt), result.size());
                result.push_back(std::move(add));
                ++patchOnlyAdded;
            }
        }
    }

    // ============================================================
    // 3) Cross-service rescue（元のまま）
    // ============================================================
    std::uint64_t xsPairsThisCall = 0;
    std::uint64_t xsRescuedEventsThisCall = 0;
    std::uint64_t xsRescuedFieldsThisCall = 0;

    if (opt.enableCrossServiceTextRescue && !result.empty())
    {
        std::unordered_map<ServiceKey, std::vector<std::size_t>, ServiceKeyHash> bySvc;
        bySvc.reserve(64);

        for (std::size_t i = 0; i < result.size(); ++i) {
            const auto& e = result[i];
            if (e.original_network_id == 0 || e.transport_stream_id == 0 || e.service_id == 0) continue;
            bySvc[ServiceKey{ e.original_network_id, e.transport_stream_id, e.service_id }].push_back(i);
        }

        struct SvcInfo { ServiceKey key{}; double titleRatio = 0.0; };
        std::vector<SvcInfo> svcInfos;
        svcInfos.reserve(bySvc.size());

        for (const auto& kv : bySvc) {
            const auto& idx = kv.second;
            std::size_t titleCnt = 0;
            for (auto i : idx) if (HasTitleUtf16(result[i]) || HasNameBytes(result[i])) ++titleCnt;
            const double r = idx.empty() ? 0.0 : (double)titleCnt / (double)idx.size();
            svcInfos.push_back(SvcInfo{ kv.first, r });
        }

        auto IsRich = [&](double r) { return r >= opt.titleRichThreshold; };
        auto IsSparse = [&](double r) { return r <= opt.titleSparseThreshold; };

        std::unordered_map<ServiceKey, std::unordered_map<ProgramKey, std::size_t, ProgramKeyHash>, ServiceKeyHash> donorMaps;

        auto BuildDonorMap = [&](const ServiceKey& sk) -> std::unordered_map<ProgramKey, std::size_t, ProgramKeyHash>&
            {
                auto it = donorMaps.find(sk);
                if (it != donorMaps.end()) return it->second;

                std::unordered_map<ProgramKey, std::size_t, ProgramKeyHash> mp;
                const auto& idx = bySvc[sk];
                mp.reserve((std::max<std::size_t>)(idx.size(), 64));

                for (auto i : idx) {
                    const auto& e = result[i];
                    if (!IsValidStartTimeRaw(e.start_time)) continue;
                    const uint32_t dur = DurationSecOf(e);
                    if (dur == 0) continue;

                    const ProgramKey k = MakeProgKey(e);
                    auto it2 = mp.find(k);
                    if (it2 == mp.end()) mp.emplace(k, i);
                    else {
                        const auto& cur = result[it2->second];
                        if (BetterPickTextOnly(e, cur)) it2->second = i;
                    }
                }

                auto [insIt, _ok] = donorMaps.emplace(sk, std::move(mp));
                return insIt->second;
            };

        for (const auto& rcv : svcInfos) {
            if (!IsSparse(rcv.titleRatio)) continue;

            const auto& rIdx = bySvc[rcv.key];
            if (rIdx.empty()) continue;

            std::unordered_set<ProgramKey, ProgramKeyHash> rSet;
            rSet.reserve((std::max<std::size_t>)(rIdx.size(), 64));

            for (auto i : rIdx) {
                const auto& e = result[i];
                if (!IsValidStartTimeRaw(e.start_time)) continue;
                const uint32_t dur = DurationSecOf(e);
                if (dur == 0) continue;
                rSet.insert(MakeProgKey(e));
            }
            if (rSet.empty()) continue;

            const ServiceKey& skR = rcv.key;

            const SvcInfo* bestDonor = nullptr;
            double bestOverlap = 0.0;

            for (const auto& dn : svcInfos) {
                if (!IsRich(dn.titleRatio)) continue;
                if (dn.key.onid != skR.onid || dn.key.tsid != skR.tsid) continue;
                if (dn.key.sid == skR.sid) continue;

                const auto& dIdx = bySvc[dn.key];
                if (dIdx.empty()) continue;

                std::size_t hit = 0;
                for (auto i : dIdx) {
                    const auto& e = result[i];
                    if (!IsValidStartTimeRaw(e.start_time)) continue;
                    if (DurationSecOf(e) == 0) continue;
                    if (rSet.find(MakeProgKey(e)) != rSet.end()) ++hit;
                }

                const double overlap = (double)hit / (double)rSet.size();
                if (overlap > bestOverlap) {
                    bestOverlap = overlap;
                    bestDonor = &dn;
                }
            }

            if (!bestDonor || bestOverlap < opt.minReplicaOverlap) continue;

            auto& donorMap = BuildDonorMap(bestDonor->key);

            std::size_t rescuedEventsLocal = 0;
            std::uint64_t rescuedFieldsLocal = 0;

            for (auto i : rIdx) {
                auto& dst = result[i];
                if (HasAnyText(dst)) continue;

                const auto k = MakeProgKey(dst);
                auto itD = donorMap.find(k);
                if (itD == donorMap.end()) continue;

                const auto& src = result[itD->second];

                if (opt.requireDurationMatch) {
                    if (!DurationNear(dst, src, opt.maxDurationDiffSec)) continue;
                }

                const std::uint64_t filled = FillMissingTextFields(dst, src);
                if (filled > 0) {
                    ++rescuedEventsLocal;
                    rescuedFieldsLocal += filled;
                }
            }

            if (rescuedEventsLocal > 0) {
                ++xsPairsThisCall;
                xsRescuedEventsThisCall += rescuedEventsLocal;
                xsRescuedFieldsThisCall += rescuedFieldsLocal;
            }
        }
    }

    // ============================================================
    // 4) sort + debug counters
    // ============================================================
    std::sort(result.begin(), result.end(),
        [](const ParsedEITPacket& a, const ParsedEITPacket& b) {
            if (a.original_network_id != b.original_network_id) return a.original_network_id < b.original_network_id;
            if (a.transport_stream_id != b.transport_stream_id) return a.transport_stream_id < b.transport_stream_id;
            if (a.service_id != b.service_id) return a.service_id < b.service_id;
            if (a.start_time != b.start_time) return a.start_time < b.start_time;
            if (a.event_id != b.event_id) return a.event_id < b.event_id;
            return a.table_id < b.table_id;
        });

    // 結果欠損の集計（取りこぼし検知）
    std::uint64_t missTitle = 0, missSum = 0, missGenre = 0, missExtIt = 0;
    for (const auto& e : result) {
        const bool titleOk = (!e.short_event_name_bytes.empty() || !e.title_utf16.empty());
        if (!titleOk) ++missTitle;
        if (e.short_text_bytes.empty()) ++missSum;
        if (!e.has_genre) ++missGenre;
        if (e.extended_items.empty()) ++missExtIt;
    }

    {
        std::lock_guard<std::mutex> lk(g_dbgMutex);
        g_dbg.patchGroupsMatched += patchGroupsMatched;
        g_dbg.patchEventsApplied += patchEventsApplied;
        g_dbg.patchFieldsFilled += patchFieldsFilled;
        g_dbg.patchItemsMerged += patchItemsMerged;
        g_dbg.patchOnlyAdded += patchOnlyAdded;

        g_dbg.crossServiceReplicaPairs += xsPairsThisCall;
        g_dbg.crossServiceRescuedEvents += xsRescuedEventsThisCall;
        g_dbg.crossServiceRescuedFields += xsRescuedFieldsThisCall;

        g_dbg.resultMissingTitle += missTitle;
        g_dbg.resultMissingSummary += missSum;
        g_dbg.resultMissingGenre += missGenre;
        g_dbg.resultMissingExtItems += missExtIt;

        g_dbg.resultEventsTotal += (std::uint64_t)result.size();
    }

#if EITSORT_HAVE_DEBUG
    if (g_EitSortDebugLogEnabled.load(std::memory_order_relaxed)) {
        EITSORT_LOGFMT(
            L"[EIT-SORT][SUM] H=%zu L=%zu groupsH=%zu multiH=%llu maxCandH=%llu mergesH=%llu fallbackH=%llu "
            L"patchMatch=%llu patchApplied=%llu patchFill=%llu patchItemsMerged=%llu patchOnlyAdded=%llu "
            L"xsPairs=%llu xsResEv=%llu xsResFields=%llu "
            L"grTitle=%llu grName=%llu grSum=%llu grGenre=%llu grExtTxt=%llu grExtIt=%llu grTotal=%llu "
            L"missTitle=%llu missSum=%llu missGenre=%llu missExtIt=%llu result=%zu",
            authoritativeEvents.size(),
            patchEvents.size(),
            groupsH.size(),
            (unsigned long long)multiGroupsThisCall,
            (unsigned long long)maxCandThisCall,
            (unsigned long long)mergesThisCall,
            (unsigned long long)fallbackBaseThisCall,
            (unsigned long long)patchGroupsMatched,
            (unsigned long long)patchEventsApplied,
            (unsigned long long)patchFieldsFilled,
            (unsigned long long)patchItemsMerged,
            (unsigned long long)patchOnlyAdded,
            (unsigned long long)xsPairsThisCall,
            (unsigned long long)xsRescuedEventsThisCall,
            (unsigned long long)xsRescuedFieldsThisCall,
            (unsigned long long)groupRescueTitleThisCall,
            (unsigned long long)groupRescueNameThisCall,
            (unsigned long long)groupRescueSumThisCall,
            (unsigned long long)groupRescueGenreThisCall,
            (unsigned long long)groupRescueExtTxtThisCall,
            (unsigned long long)groupRescueExtItThisCall,
            (unsigned long long)groupRescueTotalThisCall,
            (unsigned long long)missTitle,
            (unsigned long long)missSum,
            (unsigned long long)missGenre,
            (unsigned long long)missExtIt,
            result.size()
        );
    }
#endif

    return result;
}
