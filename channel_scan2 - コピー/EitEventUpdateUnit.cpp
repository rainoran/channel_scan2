// ================================
// EitEventUpdateUnit.cpp (NEW)
// ================================
#include "EitEventUpdateUnit.hpp"

#include <algorithm>
#include <unordered_set>
#include <cmath>
#include <iterator>
#include <limits>
#include "SystemInfoManager.hpp"

namespace
{
    static std::wstring SanitizeWideTextForSystemLog(const std::wstring& src)
    {
        std::wstring out;
        out.reserve(src.size());

        for (const wchar_t wc : src) {
            if (wc >= 0xD800 && wc <= 0xDFFF) {
                out.push_back(L'?');
            }
            else if (wc == L'\r' || wc == L'\n' || wc == L'\t') {
                out.push_back(L' ');
            }
            else {
                out.push_back(wc);
            }
        }

        return out;
    }

    static std::wstring GetDecodedEventTitleForSystemLog(const ParsedEITPacket& e)
    {
        if (e.title_utf16.empty()) {
            return {};
        }
        return SanitizeWideTextForSystemLog(e.title_utf16);
    }

    static void WriteSingleLineEventNameChangeLog(
        const ParsedEITPacket& before,
        const ParsedEITPacket& after) noexcept
    {
        const std::wstring beforeName = GetDecodedEventTitleForSystemLog(before);
        const std::wstring afterName = GetDecodedEventTitleForSystemLog(after);

        // デコード済みイベント名が取れない場合は何も出さない
        if (afterName.empty()) {
            return;
        }

        if (!beforeName.empty() && beforeName != afterName) {
            SystemInfoLogInfo(
                L"\"%ls\"から\"%ls\"へ番組名の変更を行いました。",
                beforeName.c_str(),
                afterName.c_str());
        }
        else {
            SystemInfoLogInfo(
                L"\"%ls\"に変更を行いました。",
                afterName.c_str());
        }
    }
}

bool EitEventUpdateUnit::SectionVersionHelper::Scope::operator==(
    const Scope& other) const noexcept
{
    return originalNetworkId == other.originalNetworkId &&
        transportStreamId == other.transportStreamId &&
        serviceId == other.serviceId &&
        tableId == other.tableId;
}

std::size_t
EitEventUpdateUnit::SectionVersionHelper::ScopeHash::operator()(
    const Scope& scope) const noexcept
{
    std::uint64_t x =
        (static_cast<std::uint64_t>(scope.originalNetworkId) << 48) ^
        (static_cast<std::uint64_t>(scope.transportStreamId) << 32) ^
        (static_cast<std::uint64_t>(scope.serviceId) << 16) ^
        static_cast<std::uint64_t>(scope.tableId);
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33;
    return static_cast<std::size_t>(x);
}

void EitEventUpdateUnit::SectionVersionHelper::Reset() noexcept
{
    states_.clear();
}

EitEventUpdateUnit::SectionVersionHelper::Selection
EitEventUpdateUnit::SectionVersionHelper::SelectLatest(
    const Scope& scope,
    const std::vector<Candidate>& candidates)
{
    Selection result{};
    if (candidates.empty()) {
        return result;
    }

    bool has31 = false;
    bool has0 = false;
    for (const Candidate& candidate : candidates) {
        const std::uint8_t version =
            static_cast<std::uint8_t>(candidate.version & 0x1Fu);
        has31 = has31 || version == 31u;
        has0 = has0 || version == 0u;
    }

    // 同一sub-table内に31と0が共存した場合は、0側を次周として扱う。
    const bool batchWrap = has31 && has0;
    State& state = states_[scope];

    if (!state.initialized) {
        const auto generationOf =
            [batchWrap](std::uint8_t version) noexcept -> std::uint16_t {
            const std::uint16_t normalized =
                static_cast<std::uint16_t>(version & 0x1Fu);
            if (batchWrap && normalized < 16u) {
                return static_cast<std::uint16_t>(normalized + 32u);
            }
            return normalized;
            };

        const Candidate* best = &candidates.front();
        for (const Candidate& candidate : candidates) {
            const std::uint16_t candidateGeneration =
                generationOf(candidate.version);
            const std::uint16_t bestGeneration =
                generationOf(best->version);

            if (candidateGeneration > bestGeneration ||
                (candidateGeneration == bestGeneration &&
                    candidate.receptionSequence >
                    best->receptionSequence)) {
                best = &candidate;
            }
        }

        const std::uint8_t selectedVersion =
            static_cast<std::uint8_t>(best->version & 0x1Fu);

        state.initialized = true;
        state.latestVersion = selectedVersion;
        state.latestReceptionSequence = best->receptionSequence;
        state.completedWraps =
            batchWrap && selectedVersion < 31u ? 1u : 0u;

        result.sourceIndex = best->sourceIndex;
        result.version = selectedVersion;
        result.wrapDetected = state.completedWraps != 0;
        return result;
    }

    const Candidate* best = nullptr;
    std::uint8_t bestVersion = state.latestVersion;
    std::uint8_t bestAdvance = 0;
    bool bestWrap = false;

    for (const Candidate& candidate : candidates) {
        const std::uint8_t version =
            static_cast<std::uint8_t>(candidate.version & 0x1Fu);

        std::uint8_t advance = 0;
        bool wraps = false;

        if (version == state.latestVersion) {
            if (candidate.receptionSequence <
                state.latestReceptionSequence) {
                continue;
            }
        }
        else if (batchWrap &&
            version < 16u &&
            state.latestVersion >= 16u) {
            // 例: 保存済み30、今回31と0を完全受信。
            // 31を経由して0へ進んだものとして扱う。
            advance = static_cast<std::uint8_t>(
                (32u - state.latestVersion) + version);
            wraps = true;
        }
        else {
            advance = static_cast<std::uint8_t>(
                (version - state.latestVersion) & 0x1Fu);

            // 半周以上離れた値は、新版ではなく遅着した旧版とみなす。
            if (advance == 0u || advance >= 16u) {
                continue;
            }

            if (version < state.latestVersion) {
                // 30→0のように31を確認していない飛び越しは確定しない。
                // 31→0、または同じ入力内に31と0がある場合だけ周回する。
                if (version != 0u ||
                    (state.latestVersion != 31u && !batchWrap)) {
                    continue;
                }
                wraps = true;
            }
        }

        if (best == nullptr ||
            advance > bestAdvance ||
            (advance == bestAdvance &&
                candidate.receptionSequence >
                best->receptionSequence)) {
            best = &candidate;
            bestVersion = version;
            bestAdvance = advance;
            bestWrap = wraps;
        }
    }

    if (best == nullptr) {
        result.version = state.latestVersion;
        result.stale = true;
        return result;
    }

    if (bestWrap) {
        ++state.completedWraps;
    }
    state.latestVersion = bestVersion;
    state.latestReceptionSequence = best->receptionSequence;

    result.sourceIndex = best->sourceIndex;
    result.version = bestVersion;
    result.wrapDetected = bestWrap;
    return result;
}

std::vector<ParsedEITPacket>
EitEventUpdateUnit::SelectLatestSubtableEventsForSort(
    const std::vector<ParsedEITPacket>& events,
    std::uint64_t* droppedCount)
{
    struct SubtableCandidates
    {
        SectionVersionHelper::Scope scope{};
        std::vector<SectionVersionHelper::Candidate> candidates;
        std::vector<std::size_t> eventIndexes;
    };

    if (droppedCount != nullptr) {
        *droppedCount = 0;
    }

    std::vector<SubtableCandidates> subtables;
    subtables.reserve(events.size() / 8u + 1u);

    for (std::size_t index = 0;
        index < events.size();
        ++index) {
        const ParsedEITPacket& event = events[index];
        if (!event.current_next) {
            continue;
        }

        SectionVersionHelper::Scope scope{};
        scope.originalNetworkId = event.original_network_id;
        scope.transportStreamId = event.transport_stream_id;
        scope.serviceId = event.service_id;
        scope.tableId = event.table_id;

        auto groupIt = std::find_if(
            subtables.begin(),
            subtables.end(),
            [&](const SubtableCandidates& group) {
                return group.scope == scope;
            });

        if (groupIt == subtables.end()) {
            subtables.push_back(
                SubtableCandidates{ scope, {}, {} });
            groupIt = std::prev(subtables.end());
        }

        groupIt->candidates.push_back(
            SectionVersionHelper::Candidate{
                static_cast<std::uint8_t>(
                    event.version & 0x1Fu),
                static_cast<std::uint64_t>(index + 1u),
                index
            });
        groupIt->eventIndexes.push_back(index);
    }

    SectionVersionHelper versionHelper;
    std::vector<ParsedEITPacket> selectedEvents;
    selectedEvents.reserve(events.size());

    for (const SubtableCandidates& group : subtables) {
        const auto selection =
            versionHelper.SelectLatest(
                group.scope,
                group.candidates);
        if (!selection.sourceIndex) {
            continue;
        }

        for (const std::size_t eventIndex :
        group.eventIndexes) {
            const ParsedEITPacket& event = events[eventIndex];
            if (static_cast<std::uint8_t>(
                event.version & 0x1Fu) ==
                selection.version) {
                selectedEvents.push_back(event);
            }
        }
    }

    if (droppedCount != nullptr) {
        *droppedCount =
            static_cast<std::uint64_t>(
                events.size() - selectedEvents.size());
    }
    return selectedEvents;
}

void EitEventUpdateUnit::Reset()
{
    m_store.clear();
    m_index.clear();
    m_serviceIndex.clear();
    m_retiredEvents.clear();
    m_sectionVersionHelper.Reset();
}

std::size_t EitEventUpdateUnit::Size() const noexcept
{
    return m_store.size();
}

std::vector<EitEventUpdateUnit::RetiredEvent>
EitEventUpdateUnit::TakeRetiredEvents()
{
    std::vector<RetiredEvent> out;
    out.swap(m_retiredEvents);
    return out;
}

bool EitEventUpdateUnit::IsValidStartRaw40(std::uint64_t raw40) noexcept
{
    // 40bit: 0 / all-ones を無効扱い（あなたの SortUnit と同系）
    const std::uint64_t normalized =
        raw40 & 0xFFFFFFFFFFull;
    return normalized != 0 &&
        normalized != 0xFFFFFFFFFFull;
}

std::uint64_t EitEventUpdateUnit::NormalizeStartRaw40(std::uint64_t raw40, const UpdateOptions& opt) noexcept
{
    if (opt.relaxKeyWhenInvalidStartTime && !IsValidStartRaw40(raw40)) return 0;
    return raw40 & 0xFFFFFFFFFFull;
}

bool EitEventUpdateUnit::IsVersionNewer5Bit(
    std::uint8_t candidate,
    std::uint8_t current) noexcept
{
    // EIT version_number は 5bit で 31 -> 0 と循環する。
    // 差が 1..15 の場合だけ candidate を新しい版とみなす。
    const std::uint8_t diff =
        static_cast<std::uint8_t>(
            (candidate - current) & 0x1Fu);

    return diff != 0 && diff < 16;
}

std::uint32_t EitEventUpdateUnit::DurationSecOf(const ParsedEITPacket& e) noexcept
{
    // duration が sec で入ってるケースがあるならそれを優先（SortUnitのロジックに寄せる）
    if (e.duration > 0 && e.duration <= 24u * 3600u) return (std::uint32_t)e.duration;

    const std::uint32_t hhmmss = e.duration_hhmmss;
    const std::uint32_t h = (hhmmss / 10000u) % 100u;
    const std::uint32_t m = (hhmmss / 100u) % 100u;
    const std::uint32_t s = (hhmmss / 1u) % 100u;
    if (h > 23 || m > 59 || s > 59) return 0;
    const std::uint32_t sec = h * 3600u + m * 60u + s;
    if (sec == 0 || sec > 24u * 3600u) return 0;
    return sec;
}

bool EitEventUpdateUnit::DurationNear(const ParsedEITPacket& a, const ParsedEITPacket& b, std::uint32_t tolSec) noexcept
{
    const auto da = DurationSecOf(a);
    const auto db = DurationSecOf(b);
    if (da == 0 || db == 0) return false;
    const auto diff = (da > db) ? (da - db) : (db - da);
    return diff <= tolSec;
}

std::optional<std::int64_t> EitEventUpdateUnit::StartRaw40ToSeconds(
    std::uint64_t raw40) noexcept
{
    raw40 &= 0xFFFFFFFFFFull;

    if (!IsValidStartRaw40(raw40)) {
        return std::nullopt;
    }

    auto bcdToInt = [](std::uint8_t x) noexcept -> int {
        return ((x >> 4) & 0x0F) * 10 + (x & 0x0F);
        };

    const std::uint16_t mjd =
        static_cast<std::uint16_t>((raw40 >> 24) & 0xFFFFu);

    const int h =
        bcdToInt(static_cast<std::uint8_t>((raw40 >> 16) & 0xFFu));
    const int m =
        bcdToInt(static_cast<std::uint8_t>((raw40 >> 8) & 0xFFu));
    const int s =
        bcdToInt(static_cast<std::uint8_t>(raw40 & 0xFFu));

    if (h < 0 || h > 23 ||
        m < 0 || m > 59 ||
        s < 0 || s > 59) {
        return std::nullopt;
    }

    return static_cast<std::int64_t>(mjd) * 86400LL +
        static_cast<std::int64_t>(h) * 3600LL +
        static_cast<std::int64_t>(m) * 60LL +
        static_cast<std::int64_t>(s);
}

std::optional<EitEventUpdateUnit::TimeRange>
EitEventUpdateUnit::GetTimeRange(
    const ParsedEITPacket& e) noexcept
{
    const auto begin =
        StartRaw40ToSeconds(
            e.start_time);

    if (!begin) {
        return std::nullopt;
    }

    const std::uint32_t durationSec =
        DurationSecOf(e);

    if (durationSec == 0 ||
        durationSec > 24u * 3600u) {
        return std::nullopt;
    }

    TimeRange range{};
    range.beginSec = *begin;
    range.endSec =
        *begin +
        static_cast<std::int64_t>(durationSec);

    if (range.endSec <= range.beginSec) {
        return std::nullopt;
    }

    return range;
}

bool EitEventUpdateUnit::EventsOverlapEnough(
    const ParsedEITPacket& a,
    const ParsedEITPacket& b,
    std::uint32_t minOverlapSec) noexcept
{
    if (a.original_network_id != b.original_network_id ||
        a.transport_stream_id != b.transport_stream_id ||
        a.service_id != b.service_id) {
        return false;
    }

    const auto ar =
        GetTimeRange(a);

    const auto br =
        GetTimeRange(b);

    if (!ar || !br) {
        return false;
    }

    const std::int64_t begin =
        (std::max)(ar->beginSec, br->beginSec);

    const std::int64_t end =
        (std::min)(ar->endSec, br->endSec);

    if (end <= begin) {
        return false;
    }

    return static_cast<std::uint32_t>(end - begin) >= minOverlapSec;
}

std::optional<std::int64_t> EitEventUpdateUnit::StartTimeDeltaSecRough(const ParsedEITPacket& a, const ParsedEITPacket& b) noexcept
{
    // 40bit raw: [39:24]=MJD, [23:16]=hh(BCD), [15:8]=mm(BCD), [7:0]=ss(BCD)
    auto bcdToInt = [](std::uint8_t x) noexcept -> int {
        return ((x >> 4) & 0x0F) * 10 + (x & 0x0F);
        };

    const auto ra = a.start_time & 0xFFFFFFFFFFull;
    const auto rb = b.start_time & 0xFFFFFFFFFFull;

    if (!IsValidStartRaw40(ra) || !IsValidStartRaw40(rb)) return std::nullopt;

    const std::uint16_t mjdA = (std::uint16_t)((ra >> 24) & 0xFFFFu);
    const std::uint16_t mjdB = (std::uint16_t)((rb >> 24) & 0xFFFFu);

    // MJD差も入れる（1日=86400）
    const std::int64_t dayDiff = (std::int64_t)mjdA - (std::int64_t)mjdB;

    const int hA = bcdToInt((std::uint8_t)((ra >> 16) & 0xFFu));
    const int mA = bcdToInt((std::uint8_t)((ra >> 8) & 0xFFu));
    const int sA = bcdToInt((std::uint8_t)(ra & 0xFFu));

    const int hB = bcdToInt((std::uint8_t)((rb >> 16) & 0xFFu));
    const int mB = bcdToInt((std::uint8_t)((rb >> 8) & 0xFFu));
    const int sB = bcdToInt((std::uint8_t)(rb & 0xFFu));

    if (hA > 23 || mA > 59 || sA > 59) return std::nullopt;
    if (hB > 23 || mB > 59 || sB > 59) return std::nullopt;

    const std::int64_t secA = (std::int64_t)hA * 3600 + (std::int64_t)mA * 60 + sA;
    const std::int64_t secB = (std::int64_t)hB * 3600 + (std::int64_t)mB * 60 + sB;

    return dayDiff * 86400 + (secA - secB);
}

std::int64_t EitEventUpdateUnit::CalcTextScore(const ParsedEITPacket& e) noexcept
{
    std::int64_t score = 0;

    if (!e.short_event_name_bytes.empty()) { score += 1000; score += (std::int64_t)e.short_event_name_bytes.size(); }
    if (!e.short_text_bytes.empty()) { score += 400;  score += (std::int64_t)e.short_text_bytes.size(); }
    if (!e.extended_text_bytes.empty()) { score += 800;  score += (std::int64_t)e.extended_text_bytes.size(); }

    if (!e.extended_items.empty()) {
        score += (std::int64_t)e.extended_items.size() * 500;
        for (const auto& it : e.extended_items) {
            score += (std::int64_t)it.item_description_bytes.size();
            score += (std::int64_t)it.item_text_bytes.size() * 2;
        }
    }

    if (e.has_genre) score += 120;
    if (e.has_series) score += 120;
    if (e.has_parental_rating) score += 40;

    score += (std::int64_t)e.descriptors.size();
    return score;
}

std::wstring EitEventUpdateUnit::CompactTitleForPlaceholderCheck(
    std::wstring_view src)
{
    std::wstring text(src);

    const std::wstring cutChars =
        L"[［(（";

    for (wchar_t cut : cutChars) {
        const std::size_t pos =
            text.find(cut);

        if (pos != std::wstring::npos) {
            text.resize(pos);
        }
    }

    std::wstring out;
    out.reserve(text.size());

    for (wchar_t ch : text) {
        if (ch == L' ' ||
            ch == L'\t' ||
            ch == L'\r' ||
            ch == L'\n' ||
            ch == L'　') {
            continue;
        }

        out.push_back(ch);
    }

    return out;
}

bool EitEventUpdateUnit::IsGenericPlaceholderTitle(
    std::wstring_view title)
{
    const std::wstring compact =
        CompactTitleForPlaceholderCheck(
            title);

    if (compact.empty()) {
        return false;
    }

    return compact == L"セレクション" ||
        compact == L"未定" ||
        compact == L"番組未定" ||
        compact == L"放送番組未定" ||
        compact == L"内容未定" ||
        compact == L"詳細未定" ||
        compact == L"調整中";
}

bool EitEventUpdateUnit::IsGenericPlaceholderEvent(
    const ParsedEITPacket& e)
{
    return IsGenericPlaceholderTitle(
        e.title_utf16);
}

bool EitEventUpdateUnit::HasConcreteTitle(
    const ParsedEITPacket& e)
{
    if (e.title_utf16.empty()) {
        return false;
    }

    return !IsGenericPlaceholderEvent(e);
}

static std::uint64_t HashItem64(const EitExtendedItem& it) noexcept
{
    // FNV1a簡易
    auto fnv1a = [](const std::uint8_t* p, std::size_t n) noexcept -> std::uint64_t {
        std::uint64_t h = 1469598103934665603ull;
        for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
        };

    std::uint64_t h = 0;
    if (!it.item_description_bytes.empty())
        h ^= fnv1a(it.item_description_bytes.data(), it.item_description_bytes.size());
    h ^= 0x9E3779B97F4A7C15ull;
    if (!it.item_text_bytes.empty())
        h ^= fnv1a(it.item_text_bytes.data(), it.item_text_bytes.size());
    return h;
}

std::uint64_t EitEventUpdateUnit::MergePreferBetter(
    ParsedEITPacket& dst,
    const ParsedEITPacket& src,
    const UpdateOptions& opt,
    std::uint64_t& outMergedItems)
{
    outMergedItems = 0;
    std::uint64_t filled = 0;

    const bool dstPlaceholder =
        IsGenericPlaceholderEvent(dst);

    const bool srcPlaceholder =
        IsGenericPlaceholderEvent(src);

    const bool srcConcrete =
        HasConcreteTitle(src);

    const bool sourceVersionNewer =
        IsVersionNewer5Bit(
            static_cast<std::uint8_t>(src.version),
            static_cast<std::uint8_t>(dst.version));

    const std::int64_t dstScoreBefore =
        CalcTextScore(dst);

    const std::int64_t srcScore =
        CalcTextScore(src);

    const bool sourceScoreImproved =
        opt.allowUpgradeIfTextScoreImproves &&
        srcScore >= dstScoreBefore + opt.upgradeMinScoreGain;

    const bool preferSrcText =
        srcConcrete &&
        (dstPlaceholder ||
            sourceVersionNewer ||
            sourceScoreImproved);

    // 基本：欠損を埋める
    if (!IsValidStartRaw40(dst.start_time) &&
        IsValidStartRaw40(src.start_time)) {
        dst.start_time = src.start_time;
        ++filled;
    }

    const std::uint32_t dstDurationSec =
        DurationSecOf(dst);
    const std::uint32_t srcDurationSec =
        DurationSecOf(src);

    // スポーツ延長・短縮ではEventIDと開始時刻を維持したままdurationだけが
    // 更新される。新しいsection世代、または現在受信した有効なEITなら
    // 既存durationが有効でも新しい値へ更新する。
    if (srcDurationSec != 0 &&
        (dstDurationSec == 0 ||
            sourceVersionNewer ||
            (src.current_next &&
                dstDurationSec != srcDurationSec))) {
        if (dst.duration != src.duration ||
            dst.duration_hhmmss != src.duration_hhmmss) {
            dst.duration = src.duration;
            dst.duration_hhmmss = src.duration_hhmmss;
            ++filled;
        }
    }

    // タイトル・本文系。
    // 旧実装の「src の方が長い時だけ置換」だと、
    // セレクション → 確定番組 のような短いタイトル更新を取り逃す。
    if (!src.short_event_name_bytes.empty()) {
        if (dst.short_event_name_bytes.empty() ||
            preferSrcText) {
            if (dst.short_event_name_bytes != src.short_event_name_bytes) {
                dst.short_event_name_bytes = src.short_event_name_bytes;
                ++filled;
            }
        }
    }

    if (!src.short_text_bytes.empty()) {
        if (dst.short_text_bytes.empty() ||
            preferSrcText) {
            if (dst.short_text_bytes != src.short_text_bytes) {
                dst.short_text_bytes = src.short_text_bytes;
                ++filled;
            }
        }
    }

    if (!src.extended_text_bytes.empty()) {
        if (dst.extended_text_bytes.empty() ||
            preferSrcText ||
            src.extended_text_bytes.size() > dst.extended_text_bytes.size()) {
            if (dst.extended_text_bytes != src.extended_text_bytes) {
                dst.extended_text_bytes = src.extended_text_bytes;
                ++filled;
            }
        }
    }

    if (!src.title_utf16.empty()) {
        if (dst.title_utf16.empty() ||
            preferSrcText ||
            sourceVersionNewer) {
            if (dst.title_utf16 != src.title_utf16) {
                dst.title_utf16 = src.title_utf16;
                ++filled;
            }
        }
    }

    // 汎用未定枠でない具体番組の情報を優先して補完。
    if ((!dst.has_genre || preferSrcText) && src.has_genre) {
        if (!dst.has_genre ||
            dst.content_nibble_level1 != src.content_nibble_level1 ||
            dst.content_nibble_level2 != src.content_nibble_level2 ||
            dst.user_nibble1 != src.user_nibble1 ||
            dst.user_nibble2 != src.user_nibble2) {
            dst.has_genre = true;
            dst.content_nibble_level1 = src.content_nibble_level1;
            dst.content_nibble_level2 = src.content_nibble_level2;
            dst.user_nibble1 = src.user_nibble1;
            dst.user_nibble2 = src.user_nibble2;
            ++filled;
        }
    }

    if ((!dst.has_series || preferSrcText) && src.has_series) {
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

    if ((!dst.has_parental_rating || preferSrcText) &&
        src.has_parental_rating) {
        dst.has_parental_rating = true;
        dst.parental_country = src.parental_country;
        dst.parental_rating = src.parental_rating;
        ++filled;
    }

    // extended_items はユニオン。
    if (opt.mergeExtendedItems && !src.extended_items.empty()) {
        if (dst.extended_items.empty() || preferSrcText) {
            dst.extended_items = src.extended_items;
            ++filled;
        }
        else {
            std::unordered_set<std::uint64_t> seen;
            seen.reserve(dst.extended_items.size() + src.extended_items.size());

            for (const auto& d : dst.extended_items) {
                seen.insert(HashItem64(d));
            }

            for (const auto& it : src.extended_items) {
                if (dst.extended_items.size() >= opt.maxExtendedItems) {
                    break;
                }

                const auto h = HashItem64(it);
                if (seen.insert(h).second) {
                    dst.extended_items.push_back(it);
                    ++outMergedItems;
                }
            }
        }
    }

    if ((dst.descriptors.empty() || preferSrcText) &&
        !src.descriptors.empty()) {
        dst.descriptors = src.descriptors;
        ++filled;
    }

    // table_id / version は新しいものを優先。
    if (sourceVersionNewer) {
        dst.version = src.version;
        ++filled;
    }

    if (src.table_id != 0 &&
        (dst.table_id == 0 || preferSrcText)) {
        dst.table_id = src.table_id;
        ++filled;
    }

    // 放送中判定と次番組判定に使う運用情報も最新受信値へ追従させる。
    if (src.current_next) {
        if (!dst.current_next) {
            dst.current_next = true;
            ++filled;
        }

        if (dst.running_status != src.running_status) {
            dst.running_status = src.running_status;
            ++filled;
        }

        if (dst.free_ca_mode != src.free_ca_mode) {
            dst.free_ca_mode = src.free_ca_mode;
            ++filled;
        }

        if (dst.section_number != src.section_number ||
            dst.last_section_number != src.last_section_number) {
            dst.section_number = src.section_number;
            dst.last_section_number = src.last_section_number;
            ++filled;
        }
    }

    // src が未定枠、dst が具体番組の場合は、ここまでの処理で
    // タイトル/本文を未定枠へ戻さない。
    (void)srcPlaceholder;

    return filled;
}

EitEventUpdateUnit::LogicalKey EitEventUpdateUnit::MakeLogicalKey(const ParsedEITPacket& e) noexcept
{
    return LogicalKey{ e.original_network_id, e.transport_stream_id, e.service_id, e.event_id };
}

EitEventUpdateUnit::ServiceKey
EitEventUpdateUnit::MakeServiceKey(
    const LogicalKey& lk) noexcept
{
    return ServiceKey{
        lk.onid,
        lk.tsid,
        lk.sid
    };
}

EitEventUpdateUnit::ServiceKey
EitEventUpdateUnit::MakeServiceKey(
    const ParsedEITPacket& e) noexcept
{
    return ServiceKey{
        e.original_network_id,
        e.transport_stream_id,
        e.service_id
    };
}

EitEventUpdateUnit::SectionKey
EitEventUpdateUnit::MakeSectionKey(
    const ParsedEITPacket& e) noexcept
{
    return SectionKey{
        e.original_network_id,
        e.transport_stream_id,
        e.service_id,
        e.table_id,
        e.section_number
    };
}

bool EitEventUpdateUnit::IndexContainsStart(const LogicalKey& lk, std::uint64_t start) const
{
    auto it = m_index.find(lk);
    if (it == m_index.end()) return false;
    const auto& v = it->second;
    return std::find(v.begin(), v.end(), start) != v.end();
}

void EitEventUpdateUnit::IndexAdd(const LogicalKey& lk, std::uint64_t start)
{
    auto& v = m_index[lk];
    if (std::find(v.begin(), v.end(), start) == v.end()) v.push_back(start);
}

void EitEventUpdateUnit::IndexRemoveOne(const LogicalKey& lk, std::uint64_t start)
{
    auto it = m_index.find(lk);
    if (it == m_index.end()) return;
    auto& v = it->second;
    v.erase(std::remove(v.begin(), v.end(), start), v.end());
    if (v.empty()) m_index.erase(it);
}

void EitEventUpdateUnit::ServiceIndexAdd(
    const ServiceKey& skey,
    const StrongKey& sk)
{
    auto& v =
        m_serviceIndex[skey];

    if (std::find(v.begin(), v.end(), sk) == v.end()) {
        v.push_back(sk);
    }
}

void EitEventUpdateUnit::ServiceIndexRemoveOne(
    const ServiceKey& skey,
    const StrongKey& sk)
{
    auto it =
        m_serviceIndex.find(skey);

    if (it == m_serviceIndex.end()) {
        return;
    }

    auto& v =
        it->second;

    v.erase(
        std::remove(
            v.begin(),
            v.end(),
            sk),
        v.end());

    if (v.empty()) {
        m_serviceIndex.erase(it);
    }
}

void EitEventUpdateUnit::AddStoreEntry(
    const StrongKey& sk,
    const ParsedEITPacket& e)
{
    m_store.emplace(
        sk,
        Entry{ e });

    IndexAdd(
        sk.lk,
        sk.start_raw);

    ServiceIndexAdd(
        MakeServiceKey(sk.lk),
        sk);
}

bool EitEventUpdateUnit::RemoveStoreEntry(
    const StrongKey& sk)
{
    auto it =
        m_store.find(sk);

    if (it == m_store.end()) {
        return false;
    }

    m_store.erase(it);

    IndexRemoveOne(
        sk.lk,
        sk.start_raw);

    ServiceIndexRemoveOne(
        MakeServiceKey(sk.lk),
        sk);

    return true;
}

bool EitEventUpdateUnit::ShouldDropIncomingByServiceTime(
    const ParsedEITPacket& incoming,
    const UpdateOptions& opt) const
{
    if (!opt.enableServiceTimeConflictResolve ||
        !opt.dropIncomingGenericPlaceholderWhenRealExists) {
        return false;
    }

    if (!IsGenericPlaceholderEvent(incoming)) {
        return false;
    }

    const ServiceKey skey =
        MakeServiceKey(incoming);

    auto it =
        m_serviceIndex.find(skey);

    if (it == m_serviceIndex.end()) {
        return false;
    }

    for (const StrongKey& oldKey : it->second) {
        auto itOld =
            m_store.find(oldKey);

        if (itOld == m_store.end()) {
            continue;
        }

        const ParsedEITPacket& oldEvent =
            itOld->second.ev;

        if (!HasConcreteTitle(oldEvent)) {
            continue;
        }

        if (EventsOverlapEnough(
            oldEvent,
            incoming,
            opt.serviceTimeOverlapMinSec)) {
            return true;
        }
    }

    return false;
}

std::vector<EitEventUpdateUnit::StrongKey>
EitEventUpdateUnit::FindStaleGenericPlaceholderKeysByServiceTime(
    const ParsedEITPacket& incoming,
    const UpdateOptions& opt) const
{
    std::vector<StrongKey> out;

    if (!opt.enableServiceTimeConflictResolve ||
        !opt.replaceGenericPlaceholderByRealEvent) {
        return out;
    }

    if (!HasConcreteTitle(incoming)) {
        return out;
    }

    const ServiceKey skey =
        MakeServiceKey(incoming);

    auto it =
        m_serviceIndex.find(skey);

    if (it == m_serviceIndex.end()) {
        return out;
    }

    for (const StrongKey& oldKey : it->second) {
        auto itOld =
            m_store.find(oldKey);

        if (itOld == m_store.end()) {
            continue;
        }

        const ParsedEITPacket& oldEvent =
            itOld->second.ev;

        if (!IsGenericPlaceholderEvent(oldEvent)) {
            continue;
        }

        if (!EventsOverlapEnough(
            oldEvent,
            incoming,
            opt.serviceTimeOverlapMinSec)) {
            continue;
        }

        out.push_back(oldKey);
    }

    std::sort(
        out.begin(),
        out.end(),
        [](const StrongKey& a, const StrongKey& b) {
            if (a.lk.onid != b.lk.onid) return a.lk.onid < b.lk.onid;
            if (a.lk.tsid != b.lk.tsid) return a.lk.tsid < b.lk.tsid;
            if (a.lk.sid != b.lk.sid) return a.lk.sid < b.lk.sid;
            if (a.lk.eid != b.lk.eid) return a.lk.eid < b.lk.eid;
            return a.start_raw < b.start_raw;
        });

    out.erase(
        std::unique(
            out.begin(),
            out.end()),
        out.end());

    return out;
}

std::vector<EitEventUpdateUnit::StrongKey>
EitEventUpdateUnit::FindOlderConcreteConflictKeysAtSameStart(
    const ParsedEITPacket& incoming,
    const UpdateOptions& opt,
    bool& outDropIncoming) const
{
    std::vector<StrongKey> out;
    outDropIncoming = false;

    if (!opt.enableServiceTimeConflictResolve ||
        !opt.replaceConcreteConflictByNewerVersion ||
        !HasConcreteTitle(incoming) ||
        !IsValidStartRaw40(incoming.start_time)) {
        return out;
    }

    const ServiceKey serviceKey =
        MakeServiceKey(incoming);

    const auto serviceIt =
        m_serviceIndex.find(serviceKey);

    if (serviceIt == m_serviceIndex.end()) {
        return out;
    }

    const std::uint64_t incomingStart =
        incoming.start_time & 0xFFFFFFFFFFull;

    for (const StrongKey& oldKey : serviceIt->second) {
        if (oldKey.start_raw != incomingStart ||
            oldKey.lk.eid == incoming.event_id) {
            continue;
        }

        const auto oldIt =
            m_store.find(oldKey);

        if (oldIt == m_store.end()) {
            continue;
        }

        const ParsedEITPacket& oldEvent =
            oldIt->second.ev;

        if (!HasConcreteTitle(oldEvent) ||
            !DurationNear(
                incoming,
                oldEvent,
                opt.concreteConflictDurationToleranceSec)) {
            continue;
        }

        const bool incomingIsNewer =
            IsVersionNewer5Bit(
                static_cast<std::uint8_t>(incoming.version),
                static_cast<std::uint8_t>(oldEvent.version));

        const bool oldIsNewer =
            IsVersionNewer5Bit(
                static_cast<std::uint8_t>(oldEvent.version),
                static_cast<std::uint8_t>(incoming.version));

        if (oldIsNewer) {
            // 古い断片を後から再受信しても番組表を巻き戻さない。
            out.clear();
            outDropIncoming = true;
            return out;
        }

        if (incomingIsNewer) {
            out.push_back(oldKey);
        }
        // 同一 version は取得順だけでは新旧を確定できないため触らない。
    }

    std::sort(
        out.begin(),
        out.end(),
        [](const StrongKey& a, const StrongKey& b) {
            if (a.lk.onid != b.lk.onid) return a.lk.onid < b.lk.onid;
            if (a.lk.tsid != b.lk.tsid) return a.lk.tsid < b.lk.tsid;
            if (a.lk.sid != b.lk.sid) return a.lk.sid < b.lk.sid;
            if (a.lk.eid != b.lk.eid) return a.lk.eid < b.lk.eid;
            return a.start_raw < b.start_raw;
        });

    out.erase(
        std::unique(
            out.begin(),
            out.end()),
        out.end());

    return out;
}

std::optional<std::uint64_t>
EitEventUpdateUnit::FindReplaceCandidateStart(const LogicalKey& lk, const ParsedEITPacket& incoming, const UpdateOptions& opt) const
{
    auto it = m_index.find(lk);
    if (it == m_index.end()) return std::nullopt;

    // incoming の start_time が有効でないと差し替え精度が落ちるので、原則しない
    if (!IsValidStartRaw40(incoming.start_time)) return std::nullopt;

    const auto& starts = it->second;

    std::optional<std::uint64_t> best;
    std::int64_t bestAbs = (std::numeric_limits<std::int64_t>::max)();

    for (auto st : starts) {
        StrongKey sk{ lk, st };
        auto itS = m_store.find(sk);
        if (itS == m_store.end()) continue;

        const auto& cur = itS->second.ev;

        // start差（rough）
        auto d = StartTimeDeltaSecRough(incoming, cur);
        if (!d) continue;

        const auto absd = (std::int64_t)std::llabs(*d);
        if ((std::uint32_t)absd > opt.replaceStartTimeToleranceSec) continue;

        if (opt.requireDurationNearOnReplace) {
            if (!DurationNear(incoming, cur, opt.replaceDurationToleranceSec)) continue;
        }

        if (absd < bestAbs) {
            bestAbs = absd;
            best = st;
        }
    }

    return best;
}

EitEventUpdateUnit::UpdateStats
EitEventUpdateUnit::Apply(
    const std::vector<ParsedEITPacket>& newEvents,
    const UpdateOptions& opt)
{
    UpdateStats st{};
    st.inputEvents =
        static_cast<std::uint64_t>(newEvents.size());

    std::vector<ParsedEITPacket> input =
        newEvents;

    if (opt.enablePreSort) {
        input =
            EITEventSortUnit::BuildUniqueSortedEvents(
                input,
                opt.preSortOpt);
    }

    // 初回Applyは既存EPGのseedとして使われるため、その入力内に
    // 複数世代が混在していてもversion値から新旧を推測して削除しない。
    // 2回目以降のApplyでは、受信した各sectionのイベント集合を
    // authoritative snapshotとして扱い、そこから消えたEventIDを削除する。
    const bool hadStoreAtApplyStart =
        !m_store.empty();

    // 保存側より古いsection入力は、削除だけでなく後段の追加・更新も拒否する。
    std::unordered_set<SectionKey, SectionKeyHash>
        rejectedIncomingSections;

    if (opt.replaceSectionSnapshotByIncoming &&
        hadStoreAtApplyStart &&
        !input.empty()) {
        struct IncomingSectionGroup
        {
            bool versionSet = false;
            bool mixedVersions = false;
            bool timeRangeValid = true;
            std::uint8_t version = 0;
            std::int64_t beginSec =
                (std::numeric_limits<std::int64_t>::max)();
            std::int64_t endSec =
                (std::numeric_limits<std::int64_t>::min)();
            std::unordered_set<StrongKey, StrongKeyHash> eventKeys;
        };

        std::unordered_map<
            SectionKey,
            IncomingSectionGroup,
            SectionKeyHash> groups;
        groups.reserve(input.size() / 4u + 1u);

        for (const auto& event : input) {
            if (!event.current_next ||
                event.section_number > event.last_section_number) {
                continue;
            }

            if (opt.sectionSnapshotScheduleOnly &&
                (event.table_id < 0x50u ||
                    event.table_id > 0x6Fu)) {
                continue;
            }

            IncomingSectionGroup& group =
                groups[MakeSectionKey(event)];

            const std::uint8_t eventVersion =
                static_cast<std::uint8_t>(event.version & 0x1Fu);

            if (!group.versionSet) {
                group.versionSet = true;
                group.version = eventVersion;
            }
            else if (group.version != eventVersion) {
                group.mixedVersions = true;
            }

            const LogicalKey logicalKey =
                MakeLogicalKey(event);
            const std::uint64_t start =
                NormalizeStartRaw40(
                    event.start_time,
                    opt);
            group.eventKeys.insert(
                StrongKey{ logicalKey, start });

            const auto eventRange =
                GetTimeRange(event);

            if (!eventRange) {
                group.timeRangeValid = false;
            }
            else {
                group.beginSec =
                    (std::min)(
                        group.beginSec,
                        eventRange->beginSec);
                group.endSec =
                    (std::max)(
                        group.endSec,
                        eventRange->endSec);
            }
        }

        std::vector<StrongKey> staleSectionKeys;

        for (const auto& groupPair : groups) {
            const SectionKey& sectionKey =
                groupPair.first;
            const IncomingSectionGroup& group =
                groupPair.second;

            // 同じ取得入力内に複数versionが混在するsectionは曖昧。
            // versionの数値大小では決めず、その取得回では削除を保留する。
            if (!group.versionSet ||
                group.mixedVersions ||
                !group.timeRangeValid ||
                group.eventKeys.empty() ||
                group.beginSec >= group.endSec) {
                ++st.sectionSnapshotAmbiguousSkipped;
                continue;
            }

            const std::int64_t groupSpanSec =
                group.endSec -
                group.beginSec;

            if (groupSpanSec >
                static_cast<std::int64_t>(
                    opt.sectionSnapshotMaxWindowSec)) {
                ++st.sectionSnapshotAmbiguousSkipped;
                continue;
            }

            const std::int64_t marginSec =
                static_cast<std::int64_t>(
                    opt.sectionSnapshotWindowMarginSec);
            const std::int64_t authoritativeBegin =
                group.beginSec -
                marginSec;
            const std::int64_t authoritativeEnd =
                group.endSec +
                marginSec;

            std::vector<StrongKey> groupStaleKeys;
            bool rejectIncomingGroup = false;

            for (const auto& storePair : m_store) {
                const ParsedEITPacket& storedEvent =
                    storePair.second.ev;

                if (!(MakeSectionKey(storedEvent) == sectionKey)) {
                    continue;
                }

                if (group.eventKeys.find(storePair.first) ==
                    group.eventKeys.end()) {
                    const auto storedRange =
                        GetTimeRange(storedEvent);

                    if (!storedRange ||
                        storedRange->endSec <= authoritativeBegin ||
                        storedRange->beginSec >= authoritativeEnd) {
                        ++st.sectionSnapshotOutOfWindowKept;
                        continue;
                    }

                    // 保存側が明確に新しい場合は、遅れて届いた旧sectionで
                    // 現役編成を巻き戻さない。
                    if (IsVersionNewer5Bit(
                        static_cast<std::uint8_t>(
                            storedEvent.version & 0x1Fu),
                        group.version)) {
                        ++st.sectionSnapshotStaleVersionKept;
                        rejectIncomingGroup = true;
                        break;
                    }

                    groupStaleKeys.push_back(
                        storePair.first);
                }
            }

            if (rejectIncomingGroup) {
                rejectedIncomingSections.insert(
                    sectionKey);
                continue;
            }

            staleSectionKeys.insert(
                staleSectionKeys.end(),
                groupStaleKeys.begin(),
                groupStaleKeys.end());
        }

        std::sort(
            staleSectionKeys.begin(),
            staleSectionKeys.end(),
            [](const StrongKey& a, const StrongKey& b) {
                if (a.lk.onid != b.lk.onid) return a.lk.onid < b.lk.onid;
                if (a.lk.tsid != b.lk.tsid) return a.lk.tsid < b.lk.tsid;
                if (a.lk.sid != b.lk.sid) return a.lk.sid < b.lk.sid;
                if (a.lk.eid != b.lk.eid) return a.lk.eid < b.lk.eid;
                return a.start_raw < b.start_raw;
            });
        staleSectionKeys.erase(
            std::unique(
                staleSectionKeys.begin(),
                staleSectionKeys.end()),
            staleSectionKeys.end());

        for (const StrongKey& staleKey : staleSectionKeys) {
            const auto oldIt =
                m_store.find(staleKey);
            if (oldIt == m_store.end()) {
                continue;
            }

            const ParsedEITPacket oldEvent =
                oldIt->second.ev;
            bool replacementExists = false;

            for (const ParsedEITPacket& incoming :
                input) {
                if (MakeSectionKey(incoming) ==
                    MakeSectionKey(oldEvent) &&
                    EventsOverlapEnough(
                        oldEvent,
                        incoming,
                        opt.serviceTimeOverlapMinSec)) {
                    replacementExists = true;
                    break;
                }
            }

            if (RemoveStoreEntry(staleKey)) {
                m_retiredEvents.push_back(
                    RetiredEvent{
                        oldEvent,
                        replacementExists
                            ? RetireReason::Superseded
                            : RetireReason::Cancelled
                    });
                ++st.sectionSnapshotRemoved;
            }
        }
    }

    for (const auto& e : input) {
        if (rejectedIncomingSections.find(
            MakeSectionKey(e)) !=
            rejectedIncomingSections.end()) {
            ++st.unchanged;
            continue;
        }

        const LogicalKey lk =
            MakeLogicalKey(e);

        const std::uint64_t startN =
            NormalizeStartRaw40(
                e.start_time,
                opt);

        const StrongKey sk{
            lk,
            startN
        };

        // 1) strong一致：同一イベント → 改善反映
        if (auto it = m_store.find(sk); it != m_store.end()) {
            const ParsedEITPacket before =
                it->second.ev;

            std::uint64_t mergedItems = 0;

            const auto filled =
                MergePreferBetter(
                    it->second.ev,
                    e,
                    opt,
                    mergedItems);

            st.filledFields += filled;
            st.mergedExtItems += mergedItems;

            if (filled > 0 || mergedItems > 0) {
                ++st.updated;
                WriteSingleLineEventNameChangeLog(
                    before,
                    it->second.ev);
            }
            else {
                ++st.unchanged;
            }

            continue;
        }

        // 2) EventID違いの service-time 競合解決。
        //
        // 既存が具体番組で、新規が汎用未定枠なら新規を捨てる。
        if (ShouldDropIncomingByServiceTime(
            e,
            opt)) {
            ++st.serviceTimeDroppedIncoming;
            ++st.unchanged;
            continue;
        }

        // 既存が汎用未定枠で、新規が具体番組なら既存未定枠を削除する。
        const std::vector<StrongKey> staleKeys =
            FindStaleGenericPlaceholderKeysByServiceTime(
                e,
                opt);

        if (!staleKeys.empty()) {
            for (const StrongKey& staleKey : staleKeys) {
                auto itOld =
                    m_store.find(staleKey);

                if (itOld != m_store.end()) {
                    const ParsedEITPacket before =
                        itOld->second.ev;

                    if (RemoveStoreEntry(staleKey)) {
                        ++st.serviceTimeReplaced;
                        WriteSingleLineEventNameChangeLog(
                            before,
                            e);
                    }
                }
            }
        }

        // 3) 確定番組同士の同一開始時刻競合を解決。
        //
        // 例:
        //   旧 version 23: マッサン（149） 12:30-12:45
        //   新 version 24: マッサン（150） 12:30-12:45
        //
        // EventID が異なっていても、同じ service の同一枠には
        // 1番組しか成立しないため、新しい EIT version を採用する。
        bool dropIncomingConcreteConflict = false;

        const std::vector<StrongKey> olderConcreteKeys =
            FindOlderConcreteConflictKeysAtSameStart(
                e,
                opt,
                dropIncomingConcreteConflict);

        if (dropIncomingConcreteConflict) {
            ++st.concreteTimeDroppedIncoming;
            ++st.unchanged;
            continue;
        }

        for (const StrongKey& oldKey : olderConcreteKeys) {
            const auto oldIt =
                m_store.find(oldKey);

            if (oldIt == m_store.end()) {
                continue;
            }

            const ParsedEITPacket before =
                oldIt->second.ev;

            if (RemoveStoreEntry(oldKey)) {
                ++st.concreteTimeReplaced;
                WriteSingleLineEventNameChangeLog(
                    before,
                    e);
            }
        }

        // 4) strong不一致：差し替え候補探索。
        // これは同一 EventID の start_time ずれ更新用。
        if (auto repStart = FindReplaceCandidateStart(lk, e, opt)) {
            const StrongKey old{
                lk,
                *repStart
            };

            auto itOld =
                m_store.find(old);

            if (itOld != m_store.end()) {
                const ParsedEITPacket before =
                    itOld->second.ev;

                RemoveStoreEntry(old);
                AddStoreEntry(sk, e);

                ++st.replaced;
                WriteSingleLineEventNameChangeLog(
                    before,
                    e);
                continue;
            }
        }

        // 5) 新規追加
        AddStoreEntry(sk, e);
        ++st.added;
    }

    st.storeSize =
        static_cast<std::uint64_t>(m_store.size());

    return st;
}

EitEventUpdateUnit::UpdateStats
EitEventUpdateUnit::ApplySectionSnapshots(
    const std::vector<ReceivedSectionSnapshot>& snapshots,
    const UpdateOptions& opt)
{
    UpdateStats result{};
    std::vector<ParsedEITPacket> acceptedEvents;

    struct VersionCandidateGroup
    {
        SectionVersionHelper::Scope scope{};
        std::vector<SectionVersionHelper::Candidate> candidates;
        std::vector<std::size_t> incomingIndexes;
    };

    std::vector<VersionCandidateGroup> versionGroups;
    std::unordered_set<std::size_t> versionManagedIndexes;
    std::unordered_set<std::size_t> selectedIndexes;

    const auto isVersionCandidateValid =
        [&](const ReceivedSectionSnapshot& snapshot) {
        const bool scheduleTable =
            snapshot.tableId >= 0x50u &&
            snapshot.tableId <= 0x6Fu;

        if (!snapshot.complete ||
            !snapshot.currentNext ||
            (opt.sectionSnapshotScheduleOnly &&
                !scheduleTable)) {
            return false;
        }

        const auto windowBegin =
            StartRaw40ToSeconds(
                snapshot.windowBeginRaw40);
        const auto windowEnd =
            StartRaw40ToSeconds(
                snapshot.windowEndRaw40);

        if (!windowBegin ||
            !windowEnd ||
            *windowBegin >= *windowEnd ||
            (*windowEnd - *windowBegin) >
            static_cast<std::int64_t>(
                opt.sectionSnapshotMaxWindowSec)) {
            return false;
        }

        const SectionKey sectionKey{
            snapshot.originalNetworkId,
            snapshot.transportStreamId,
            snapshot.serviceId,
            snapshot.tableId,
            snapshot.sectionNumber
        };

        return std::all_of(
            snapshot.events.begin(),
            snapshot.events.end(),
            [&](const ParsedEITPacket& event) {
                return MakeSectionKey(event) == sectionKey &&
                    event.current_next &&
                    static_cast<std::uint8_t>(
                        event.version & 0x1Fu) ==
                    static_cast<std::uint8_t>(
                        snapshot.version & 0x1Fu);
            });
        };

    // 先に全sectionを責任時間範囲へ仮配置し、同じ範囲内の最新versionだけを選ぶ。
    for (std::size_t index = 0;
        index < snapshots.size();
        ++index) {
        const ReceivedSectionSnapshot& snapshot =
            snapshots[index];

        result.inputEvents +=
            static_cast<std::uint64_t>(
                snapshot.events.size());

        if (!isVersionCandidateValid(snapshot)) {
            continue;
        }

        SectionVersionHelper::Scope scope{};
        scope.originalNetworkId =
            snapshot.originalNetworkId;
        scope.transportStreamId =
            snapshot.transportStreamId;
        scope.serviceId =
            snapshot.serviceId;
        scope.tableId =
            snapshot.tableId;

        auto groupIt = std::find_if(
            versionGroups.begin(),
            versionGroups.end(),
            [&](const VersionCandidateGroup& group) {
                return group.scope == scope;
            });

        if (groupIt == versionGroups.end()) {
            versionGroups.push_back(
                VersionCandidateGroup{ scope, {}, {} });
            groupIt = std::prev(versionGroups.end());
        }

        const std::uint64_t sequence =
            snapshot.receptionSequence != 0
            ? snapshot.receptionSequence
            : static_cast<std::uint64_t>(index + 1u);

        groupIt->candidates.push_back(
            SectionVersionHelper::Candidate{
                static_cast<std::uint8_t>(
                    snapshot.version & 0x1Fu),
                sequence,
                index
            });
        groupIt->incomingIndexes.push_back(index);
        versionManagedIndexes.insert(index);
    }

    // 再起動直後などヘルパー状態が空でも、保存済みsectionのversionを
    // 仮配置候補へ加える。31を確認せず30から0へ飛ぶ誤周回を防ぐ。
    constexpr std::size_t kStoredCandidateIndex =
        (std::numeric_limits<std::size_t>::max)();

    for (VersionCandidateGroup& group :
        versionGroups) {
        for (const auto& storePair : m_store) {
            const ParsedEITPacket& storedEvent =
                storePair.second.ev;
            if (storedEvent.original_network_id !=
                group.scope.originalNetworkId ||
                storedEvent.transport_stream_id !=
                group.scope.transportStreamId ||
                storedEvent.service_id !=
                group.scope.serviceId ||
                storedEvent.table_id !=
                group.scope.tableId) {
                continue;
            }

            const auto storedRange =
                GetTimeRange(storedEvent);
            if (!storedRange) {
                continue;
            }

            bool overlapsManagedWindow = false;
            for (const std::size_t incomingIndex :
            group.incomingIndexes) {
                const auto windowBegin =
                    StartRaw40ToSeconds(
                        snapshots[incomingIndex]
                        .windowBeginRaw40);
                const auto windowEnd =
                    StartRaw40ToSeconds(
                        snapshots[incomingIndex]
                        .windowEndRaw40);
                if (windowBegin &&
                    windowEnd &&
                    storedRange->endSec > *windowBegin &&
                    storedRange->beginSec < *windowEnd) {
                    overlapsManagedWindow = true;
                    break;
                }
            }

            if (!overlapsManagedWindow) {
                continue;
            }

            group.candidates.push_back(
                SectionVersionHelper::Candidate{
                    static_cast<std::uint8_t>(
                        storedEvent.version & 0x1Fu),
                    0,
                    kStoredCandidateIndex
                });
        }
    }

    for (const VersionCandidateGroup& group :
        versionGroups) {
        const auto selection =
            m_sectionVersionHelper.SelectLatest(
                group.scope,
                group.candidates);

        if (!selection.sourceIndex) {
            ++result.sectionSnapshotStaleVersionKept;
            result.sectionVersionCandidatesSkipped +=
                static_cast<std::uint64_t>(
                    group.incomingIndexes.size());
            continue;
        }

        // versionはsub-table全体で選び、そのversionに属する各sectionを
        // section_number + 責任時間範囲ごとに一つずつ確定する。
        std::vector<std::size_t> matchingIndexes;
        for (const std::size_t incomingIndex :
        group.incomingIndexes) {
            if (static_cast<std::uint8_t>(
                snapshots[incomingIndex].version & 0x1Fu) ==
                selection.version) {
                matchingIndexes.push_back(incomingIndex);
            }
        }
        if (matchingIndexes.empty()) {
            ++result.sectionSnapshotStaleVersionKept;
        }

        for (const std::size_t candidateIndex :
        matchingIndexes) {
            bool isLatestForSection = true;
            const ReceivedSectionSnapshot& candidate =
                snapshots[candidateIndex];
            const std::uint64_t candidateSequence =
                candidate.receptionSequence != 0
                ? candidate.receptionSequence
                : static_cast<std::uint64_t>(
                    candidateIndex + 1u);

            for (const std::size_t otherIndex :
            matchingIndexes) {
                const ReceivedSectionSnapshot& other =
                    snapshots[otherIndex];
                if (other.sectionNumber !=
                    candidate.sectionNumber ||
                    other.windowBeginRaw40 !=
                    candidate.windowBeginRaw40 ||
                    other.windowEndRaw40 !=
                    candidate.windowEndRaw40) {
                    continue;
                }

                const std::uint64_t otherSequence =
                    other.receptionSequence != 0
                    ? other.receptionSequence
                    : static_cast<std::uint64_t>(
                        otherIndex + 1u);
                if (otherSequence >
                    candidateSequence) {
                    isLatestForSection = false;
                    break;
                }
            }

            if (isLatestForSection) {
                selectedIndexes.insert(candidateIndex);
            }
        }

        std::uint64_t selectedInThisGroup = 0;
        for (const std::size_t incomingIndex :
        group.incomingIndexes) {
            if (selectedIndexes.find(incomingIndex) !=
                selectedIndexes.end()) {
                ++selectedInThisGroup;
            }
        }

        result.sectionVersionCandidatesSkipped +=
            static_cast<std::uint64_t>(
                group.incomingIndexes.size()) -
            selectedInThisGroup;
        if (selection.wrapDetected) {
            ++result.sectionVersionWrapDetected;
        }
    }

    for (std::size_t snapshotIndex = 0;
        snapshotIndex < snapshots.size();
        ++snapshotIndex) {
        const ReceivedSectionSnapshot& snapshot =
            snapshots[snapshotIndex];
        const bool scheduleTable =
            snapshot.tableId >= 0x50u &&
            snapshot.tableId <= 0x6Fu;

        // 不完全sectionは退役判定に使わず、欠損補完だけに利用する。
        if (!snapshot.complete ||
            !snapshot.currentNext ||
            (opt.sectionSnapshotScheduleOnly &&
                !scheduleTable)) {
            acceptedEvents.insert(
                acceptedEvents.end(),
                snapshot.events.begin(),
                snapshot.events.end());
            continue;
        }

        // 完全section候補は、時間範囲内で選ばれた最新versionだけを確定する。
        if (versionManagedIndexes.find(snapshotIndex) !=
            versionManagedIndexes.end() &&
            selectedIndexes.find(snapshotIndex) ==
            selectedIndexes.end()) {
            continue;
        }

        const auto windowBegin =
            StartRaw40ToSeconds(
                snapshot.windowBeginRaw40);
        const auto windowEnd =
            StartRaw40ToSeconds(
                snapshot.windowEndRaw40);

        if (!windowBegin ||
            !windowEnd ||
            *windowBegin >= *windowEnd ||
            (*windowEnd - *windowBegin) >
            static_cast<std::int64_t>(
                opt.sectionSnapshotMaxWindowSec)) {
            ++result.sectionSnapshotAmbiguousSkipped;
            continue;
        }

        const SectionKey sectionKey{
            snapshot.originalNetworkId,
            snapshot.transportStreamId,
            snapshot.serviceId,
            snapshot.tableId,
            snapshot.sectionNumber
        };

        bool metadataMismatch = false;
        std::unordered_set<StrongKey, StrongKeyHash>
            incomingKeys;
        incomingKeys.reserve(
            snapshot.events.size());

        for (const ParsedEITPacket& event : snapshot.events) {
            if (!(MakeSectionKey(event) == sectionKey) ||
                !event.current_next ||
                static_cast<std::uint8_t>(
                    event.version & 0x1Fu) !=
                static_cast<std::uint8_t>(
                    snapshot.version & 0x1Fu)) {
                metadataMismatch = true;
                break;
            }

            incomingKeys.insert(
                StrongKey{
                    MakeLogicalKey(event),
                    NormalizeStartRaw40(
                        event.start_time,
                        opt)
                });
        }

        if (metadataMismatch) {
            ++result.sectionSnapshotAmbiguousSkipped;
            continue;
        }

        std::vector<StrongKey> removeKeys;
        for (const auto& storePair : m_store) {
            const ParsedEITPacket& storedEvent =
                storePair.second.ev;

            if (!(MakeSectionKey(storedEvent) == sectionKey) ||
                incomingKeys.find(storePair.first) !=
                incomingKeys.end()) {
                continue;
            }

            const auto storedRange =
                GetTimeRange(storedEvent);
            if (!storedRange ||
                storedRange->endSec <= *windowBegin ||
                storedRange->beginSec >= *windowEnd) {
                ++result.sectionSnapshotOutOfWindowKept;
                continue;
            }

            removeKeys.push_back(
                storePair.first);
        }

        for (const StrongKey& removeKey : removeKeys) {
            const auto oldIt =
                m_store.find(removeKey);
            if (oldIt == m_store.end()) {
                continue;
            }

            const ParsedEITPacket oldEvent =
                oldIt->second.ev;

            bool replacementExists = false;
            for (const ParsedEITPacket& incoming :
                snapshot.events) {
                if (EventsOverlapEnough(
                    oldEvent,
                    incoming,
                    opt.serviceTimeOverlapMinSec)) {
                    replacementExists = true;
                    break;
                }
            }

            if (RemoveStoreEntry(removeKey)) {
                m_retiredEvents.push_back(
                    RetiredEvent{
                        oldEvent,
                        replacementExists
                            ? RetireReason::Superseded
                            : RetireReason::Cancelled
                    });
                ++result.sectionSnapshotRemoved;
            }
        }

        acceptedEvents.insert(
            acceptedEvents.end(),
            snapshot.events.begin(),
            snapshot.events.end());
    }

    UpdateOptions mergeOptions =
        opt;
    mergeOptions.replaceSectionSnapshotByIncoming =
        false;

    const UpdateStats merged =
        Apply(
            acceptedEvents,
            mergeOptions);

    result.added += merged.added;
    result.updated += merged.updated;
    result.replaced += merged.replaced;
    result.unchanged += merged.unchanged;
    result.filledFields += merged.filledFields;
    result.mergedExtItems += merged.mergedExtItems;
    result.serviceTimeReplaced +=
        merged.serviceTimeReplaced;
    result.serviceTimeDroppedIncoming +=
        merged.serviceTimeDroppedIncoming;
    result.concreteTimeReplaced +=
        merged.concreteTimeReplaced;
    result.concreteTimeDroppedIncoming +=
        merged.concreteTimeDroppedIncoming;
    result.sectionSnapshotRemoved +=
        merged.sectionSnapshotRemoved;
    result.sectionSnapshotOutOfWindowKept +=
        merged.sectionSnapshotOutOfWindowKept;
    result.sectionSnapshotStaleVersionKept +=
        merged.sectionSnapshotStaleVersionKept;
    result.sectionSnapshotAmbiguousSkipped +=
        merged.sectionSnapshotAmbiguousSkipped;
    result.sectionVersionCandidatesSkipped +=
        merged.sectionVersionCandidatesSkipped;
    result.sectionVersionWrapDetected +=
        merged.sectionVersionWrapDetected;
    result.storeSize =
        static_cast<std::uint64_t>(
            m_store.size());

    return result;
}

std::vector<ParsedEITPacket> EitEventUpdateUnit::ExportSnapshotSorted() const
{
    std::vector<ParsedEITPacket> out;
    out.reserve(m_store.size());
    for (const auto& kv : m_store) out.push_back(kv.second.ev);

    std::sort(out.begin(), out.end(),
        [](const ParsedEITPacket& a, const ParsedEITPacket& b) {
            if (a.original_network_id != b.original_network_id) return a.original_network_id < b.original_network_id;
            if (a.transport_stream_id != b.transport_stream_id) return a.transport_stream_id < b.transport_stream_id;
            if (a.service_id != b.service_id) return a.service_id < b.service_id;
            if (a.start_time != b.start_time) return a.start_time < b.start_time;
            if (a.event_id != b.event_id) return a.event_id < b.event_id;
            return a.table_id < b.table_id;
        });

    return out;
}
