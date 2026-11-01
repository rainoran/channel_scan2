#include "EpgHtmlLite_SplitGuide.hpp"
#include "EpgHtmlLite_DateSplit.hpp"
#include "EpgHtmlLite_PageNowList.hpp"
#include "EpgHtmlLite_PageNowTimetable.hpp"
#include "EpgHtmlLite_PageReservationList.hpp"
#include "EpgHtmlLite_PageKeywordSearch.hpp"
#include "EpgHtmlLite_PageAutoKeywordReservationList.hpp"
#include "EpgHtmlLite_log.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <climits>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <system_error>
#include <utility>
#include <cwchar>

#include "DebugManager.h"

namespace
{
    using EpgHtmlLite::ProgramEntry;

    constexpr int kBlockCount = 4;

    std::wstring HtmlEscape_(std::wstring_view s)
    {
        std::wstring out;
        out.reserve(s.size() + 16);

        for (const wchar_t ch : s) {
            switch (ch) {
            case L'&':  out += L"&amp;";  break;
            case L'<':  out += L"&lt;";   break;
            case L'>':  out += L"&gt;";   break;
            case L'"':  out += L"&quot;"; break;
            default:
                if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
                    out.push_back(L' ');
                }
                else {
                    out.push_back(ch);
                }
                break;
            }
        }

        return out;
    }

    std::wstring JsEscape_(std::wstring_view s)
    {
        std::wstring out;
        out.reserve(s.size() + 16);

        for (const wchar_t ch : s) {
            switch (ch) {
            case L'\\': out += L"\\\\"; break;
            case L'"':  out += L"\\\""; break;
            case L'\r': out += L"\\r";  break;
            case L'\n': out += L"\\n";  break;
            case L'\t': out += L"\\t";  break;
            default:
                if (ch < 0x20) {
                    out.push_back(L' ');
                }
                else {
                    out.push_back(ch);
                }
                break;
            }
        }

        return out;
    }

    void AppendUtf8CodePoint_(std::string& out, std::uint32_t cp)
    {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else {
            out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    std::string WideToUtf8_(std::wstring_view s)
    {
        std::string out;
        out.reserve(s.size() * 3 + 8);

        for (std::size_t i = 0; i < s.size(); ++i) {
            std::uint32_t cp = static_cast<std::uint32_t>(s[i]);

#if WCHAR_MAX <= 0xFFFF
            if (cp >= 0xD800 && cp <= 0xDBFF && (i + 1) < s.size()) {
                const std::uint32_t lo = static_cast<std::uint32_t>(s[i + 1]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                    ++i;
                }
            }
#endif

            AppendUtf8CodePoint_(out, cp);
        }

        return out;
    }

    bool WriteUtf8File_(const std::filesystem::path& path, const std::wstring& text)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return false;
        }

        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return false;
        }

        const std::string bytes = WideToUtf8_(text);
        if (!bytes.empty()) {
            ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }

        ofs.flush();
        return ofs.good();
    }

    std::wstring SpaceDirName_(int space)
    {
        return L"space" + std::to_wstring(space);
    }

    std::wstring SpaceLabel_(int space)
    {
        switch (space) {
        case 0: return L"地上波";
        case 1: return L"BS";
        case 2: return L"CS";
        default: return L"Space " + std::to_wstring(space);
        }
    }

    std::wstring SidStem_(std::uint16_t sid)
    {
        return L"sid" + std::to_wstring(static_cast<unsigned int>(sid));
    }

    std::wstring SidHtmlFileName_(std::uint16_t sid)
    {
        return SidStem_(sid) + L".html";
    }

    std::wstring SidJsFileName_(std::uint16_t sid)
    {
        return SidStem_(sid) + L".js";
    }

    std::wstring MakeDetailKey_(const ProgramEntry& e)
    {
        if (!e.reserveKey.empty()) {
            return e.reserveKey;
        }
        return std::to_wstring(static_cast<unsigned int>(e.serviceId)) +
            L"_" +
            std::to_wstring(static_cast<unsigned int>(e.eventId));
    }

    std::wstring MakeLiteReserveKey_(const ProgramEntry& e)
    {
        if (!e.reserveKey.empty()) {
            return e.reserveKey;
        }

        std::wostringstream oss;

        oss << e.broadcastDateKey
            << L"|sp" << e.space
            << L"|sid" << static_cast<unsigned int>(e.serviceId)
            << L"|eid" << static_cast<unsigned int>(e.eventId)
            << L"|st" << e.startMinutesOfDay
            << L"|ed" << e.endMinutesOfDay;

        return oss.str();
    }

    std::wstring BlockSuffix_(int block)
    {
        switch (block) {
        case 0: return L"b04";
        case 1: return L"b10";
        case 2: return L"b16";
        case 3: return L"b22";
        default: return L"ball";
        }
    }

    constexpr int kLiteBroadcastStartMinutes_ = 4 * 60;
    constexpr int kLiteMinutesPerDay_ = 24 * 60;

    struct LiteBlockMinuteRange_
    {
        int begin = -1;
        int end = -1;
    };

    LiteBlockMinuteRange_ GetLiteBlockMinuteRange_(int block) noexcept
    {
        switch (block) {
        case 0:
            // 04:00 - 10:00
            return { 4 * 60, 10 * 60 };

        case 1:
            // 10:00 - 16:00
            return { 10 * 60, 16 * 60 };

        case 2:
            // 16:00 - 22:00
            return { 16 * 60, 22 * 60 };

        case 3:
            // 22:00 - 翌04:00
            return { 22 * 60, 28 * 60 };

        default:
            return {};
        }
    }

    int NormalizeToLiteBroadcastMinute_(int minute) noexcept
    {
        if (minute < 0) {
            return -1;
        }

        // 00:00 - 03:59 は放送日上では 24:00 - 27:59 扱い。
        if (minute < kLiteBroadcastStartMinutes_) {
            return minute + kLiteMinutesPerDay_;
        }

        return minute;
    }

    bool GetProgramLiteMinuteRange_(
        const ProgramEntry& e,
        int& outBegin,
        int& outEnd) noexcept
    {
        outBegin = NormalizeToLiteBroadcastMinute_(e.startMinutesOfDay);
        outEnd = NormalizeToLiteBroadcastMinute_(e.endMinutesOfDay);

        if (outBegin < 0) {
            return false;
        }

        // 終了時刻が取れない場合は、最低限「開始時刻が属するブロック」には出す。
        if (outEnd < 0) {
            outEnd = outBegin + 1;
            return true;
        }

        // 23:30 - 00:30 のような日跨ぎを補正。
        // 00:30 は Normalize で 1470 になるので通常はここに入らないが、
        // 03:30 - 04:30 のような境界ケースも保護する。
        if (outEnd <= outBegin) {
            outEnd += kLiteMinutesPerDay_;
        }

        return true;
    }

    bool ProgramOverlapsLiteBlock_(
        const ProgramEntry& e,
        int block) noexcept
    {
        const LiteBlockMinuteRange_ blockRange = GetLiteBlockMinuteRange_(block);
        if (blockRange.begin < 0 || blockRange.end <= blockRange.begin) {
            return false;
        }

        int programBegin = -1;
        int programEnd = -1;

        if (!GetProgramLiteMinuteRange_(e, programBegin, programEnd)) {
            // 時刻情報が壊れている場合だけ、旧仕様の timeBlock 判定へ退避。
            return e.timeBlock == block;
        }

        // 半開区間 [begin, end) の交差判定。
        return programBegin < blockRange.end && blockRange.begin < programEnd;
    }

    std::wstring GetBlockOverlapStateText_(
        const ProgramEntry& e,
        int block)
    {
        const LiteBlockMinuteRange_ blockRange = GetLiteBlockMinuteRange_(block);
        if (blockRange.begin < 0 || blockRange.end <= blockRange.begin) {
            return {};
        }

        int programBegin = -1;
        int programEnd = -1;

        if (!GetProgramLiteMinuteRange_(e, programBegin, programEnd)) {
            return {};
        }

        const bool beginsBeforeBlock = programBegin < blockRange.begin;
        const bool endsAfterBlock = programEnd > blockRange.end;

        if (beginsBeforeBlock && endsAfterBlock) {
            return L"ブロック跨ぎ";
        }
        if (beginsBeforeBlock) {
            return L"継続中";
        }
        if (endsAfterBlock) {
            return L"次ブロック継続";
        }

        return {};
    }

    std::wstring FormatMinuteOfDayText_(int minute);

    // TvRock の rowspan 的な考え方を div レイアウトで再現する。
// 基本は 1分 = 8px だが、短時間番組に最低表示高さが必要な場合は、
// その時間帯の「分行」自体を拡張して、後続番組と重ならないようにする。
    constexpr int kLiteTimelineBasePxPerMinute_ = 8;

    constexpr int kLiteTimelineCompactMaxMinutes_ = 10;

    // 1～2分番組は普通の分数比例ではタイトルが潰れやすいので、かなり厚めに取る。
    constexpr int kLiteTimelineVeryShortMaxMinutes_ = 2;
    constexpr int kLiteTimelineShortMaxMinutes_ = 5;

    constexpr int kLiteTimelineVeryShortMinItemHeightPx_ = 220;
    constexpr int kLiteTimelineShortMinItemHeightPx_ = 170;
    constexpr int kLiteTimelineCompactMinItemHeightPx_ = 132;
    constexpr int kLiteTimelineStandardMinItemHeightPx_ = 82;
    constexpr int kLiteTimelineSummaryMinHeightPx_ = 220;

    constexpr int kLiteTimelineContentSafetyPaddingPx_ = 48;
    constexpr int kLiteTimelineSpanSafetyPaddingPx_ = 32;

    // 終了時刻が取れない番組を 1分枠にすると極端に潰れるので、
    // レイアウト上だけ暫定30分として扱う。
    constexpr int kLiteTimelineMissingEndAssumedMinutes_ = 30;

    // table時間割で、前番組終了～次番組開始に微小な隙間がある場合は、
// 表示上だけ前番組を次番組開始まで伸ばして穴を消す。
    constexpr int kLiteTableRepairGapMaxMinutes_ = 3;

    constexpr const wchar_t* kLiteReservationSwitchScriptFile_ =
        L"epg_lite_reservation_switch.js";

    struct LiteTimelineLayout_
    {
        int block = -1;
        LiteBlockMinuteRange_ range{};
        std::vector<int> prefixPx;
        int totalHeightPx = 1;
        bool valid = false;
    };

    bool GetProgramLiteMinuteRangeForLayout_(
        const ProgramEntry& e,
        int& outBegin,
        int& outEnd) noexcept
    {
        outBegin = NormalizeToLiteBroadcastMinute_(e.startMinutesOfDay);
        outEnd = NormalizeToLiteBroadcastMinute_(e.endMinutesOfDay);

        if (outBegin < 0) {
            return false;
        }

        if (outEnd < 0) {
            outEnd = outBegin + kLiteTimelineMissingEndAssumedMinutes_;
            return true;
        }

        if (outEnd <= outBegin) {
            outEnd += kLiteMinutesPerDay_;
        }

        return true;
    }

    bool GetClippedLiteMinuteRange_(
        const ProgramEntry& e,
        const LiteBlockMinuteRange_& range,
        int& outBeginOffset,
        int& outEndOffset) noexcept
    {
        outBeginOffset = 0;
        outEndOffset = 0;

        if (range.begin < 0 || range.end <= range.begin) {
            return false;
        }

        int programBegin = -1;
        int programEnd = -1;

        if (!GetProgramLiteMinuteRangeForLayout_(e, programBegin, programEnd)) {
            return false;
        }

        const int clippedBegin =
            (std::max)(programBegin, range.begin);

        const int clippedEnd =
            (std::min)(programEnd, range.end);

        if (clippedEnd <= clippedBegin) {
            return false;
        }

        outBeginOffset = clippedBegin - range.begin;
        outEndOffset = clippedEnd - range.begin;

        return outEndOffset > outBeginOffset;
    }

    int GetLiteProgramVisibleMinutes_(
        const ProgramEntry& e,
        int block) noexcept
    {
        const LiteBlockMinuteRange_ range =
            GetLiteBlockMinuteRange_(block);

        int beginOffset = 0;
        int endOffset = 0;

        if (!GetClippedLiteMinuteRange_(e, range, beginOffset, endOffset)) {
            return 1;
        }

        return (std::max)(1, endOffset - beginOffset);
    }

    int EstimateLiteTitleCharsPerLine_(
        int channelWidthPx,
        bool compact) noexcept
    {
        int contentWidthPx =
            channelWidthPx - 54;

        if (contentWidthPx < 64) {
            contentWidthPx = 64;
        }

        // 実ブラウザよりかなり安全側。
        // 1行文字数を少なめに見積もって、必要行数を多めに取る。
        const int charPx =
            compact ? 20 : 18;

        const int chars =
            contentWidthPx / charPx;

        return (std::max)(3, chars);
    }

    int EstimateLiteTitleLines_(
        std::wstring_view title,
        int charsPerLine) noexcept
    {
        if (charsPerLine <= 0) {
            charsPerLine = 1;
        }

        if (title.empty()) {
            return 1;
        }

        int lines = 1;
        int count = 0;

        for (const wchar_t ch : title) {
            if (ch == L'\r') {
                continue;
            }

            if (ch == L'\n') {
                ++lines;
                count = 0;
                continue;
            }

            ++count;

            if (count >= charsPerLine) {
                ++lines;
                count = 0;
            }
        }

        return (std::max)(1, lines);
    }

    int GetLiteTimetableTitleRequiredHeightPx_(
        const ProgramEntry& e,
        int visibleMinutes,
        int channelWidthPx) noexcept
    {
        const int minutes =
            (std::max)(1, visibleMinutes);

        const bool compact =
            minutes <= kLiteTimelineCompactMaxMinutes_;

        const int charsPerLine =
            EstimateLiteTitleCharsPerLine_(channelWidthPx, compact);

        const int titleLines =
            EstimateLiteTitleLines_(e.title, charsPerLine);

        if (compact) {
            const int titleLineHeight =
                minutes <= kLiteTimelineVeryShortMaxMinutes_ ? 19 : 18;

            int required =
                18 +                              // 上下余白
                24 +                              // 時刻行
                titleLines * titleLineHeight +    // タイトル全文
                kLiteTimelineContentSafetyPaddingPx_;

            if (minutes <= kLiteTimelineVeryShortMaxMinutes_) {
                required =
                    (std::max)(
                        required,
                        kLiteTimelineVeryShortMinItemHeightPx_);
            }
            else if (minutes <= kLiteTimelineShortMaxMinutes_) {
                required =
                    (std::max)(
                        required,
                        kLiteTimelineShortMinItemHeightPx_);
            }
            else {
                required =
                    (std::max)(
                        required,
                        kLiteTimelineCompactMinItemHeightPx_);
            }

            return required;
        }

        int required =
            18 +                              // 上下余白
            24 +                              // 時刻行
            titleLines * 19 +                 // タイトル全文
            24 +                              // ジャンル1行
            kLiteTimelineContentSafetyPaddingPx_;

        required =
            (std::max)(
                required,
                kLiteTimelineStandardMinItemHeightPx_);

        return required;
    }

    int GetLiteProgramMinItemHeightPx_(
        int visibleMinutes) noexcept
    {
        const int minutes =
            (std::max)(1, visibleMinutes);

        const int naturalHeight =
            (std::max)(1, minutes * kLiteTimelineBasePxPerMinute_ - 2);

        if (minutes <= kLiteTimelineCompactMaxMinutes_) {
            return (std::max)(
                kLiteTimelineCompactMinItemHeightPx_,
                naturalHeight);
        }

        return (std::max)(
            kLiteTimelineStandardMinItemHeightPx_,
            naturalHeight);
    }

    int GetLiteProgramMinItemHeightPx_(
        const ProgramEntry& e,
        int visibleMinutes,
        int channelWidthPx) noexcept
    {
        const int minutes =
            (std::max)(1, visibleMinutes);

        const int naturalHeight =
            (std::max)(1, minutes * kLiteTimelineBasePxPerMinute_ - 2);

        const int titleRequiredHeight =
            GetLiteTimetableTitleRequiredHeightPx_(
                e,
                minutes,
                channelWidthPx);

        return (std::max)(
            titleRequiredHeight,
            naturalHeight);
    }

    int SumMinuteHeight_(
        const std::vector<int>& minuteHeight,
        int beginOffset,
        int endOffset) noexcept
    {
        if (beginOffset < 0) {
            beginOffset = 0;
        }

        if (endOffset < beginOffset) {
            endOffset = beginOffset;
        }

        if (static_cast<std::size_t>(endOffset) > minuteHeight.size()) {
            endOffset = static_cast<int>(minuteHeight.size());
        }

        int sum = 0;

        for (int i = beginOffset; i < endOffset; ++i) {
            sum += minuteHeight[static_cast<std::size_t>(i)];
        }

        return sum;
    }

    void AddExtraHeightToMinuteRange_(
        std::vector<int>& minuteHeight,
        int beginOffset,
        int endOffset,
        int extraPx)
    {
        if (extraPx <= 0) {
            return;
        }

        if (beginOffset < 0) {
            beginOffset = 0;
        }

        if (endOffset < beginOffset) {
            endOffset = beginOffset;
        }

        if (static_cast<std::size_t>(endOffset) > minuteHeight.size()) {
            endOffset = static_cast<int>(minuteHeight.size());
        }

        const int count =
            (std::max)(1, endOffset - beginOffset);

        const int addEach =
            extraPx / count;

        int remain =
            extraPx % count;

        for (int i = beginOffset; i < endOffset; ++i) {
            int add = addEach;

            if (remain > 0) {
                ++add;
                --remain;
            }

            minuteHeight[static_cast<std::size_t>(i)] += add;
        }
    }

    LiteTimelineLayout_ BuildLiteTimelineLayout_(
        const std::vector<const ProgramEntry*>& rows,
        int block,
        int channelWidthPx = 220)
    {
        LiteTimelineLayout_ layout{};
        layout.block = block;
        layout.range = GetLiteBlockMinuteRange_(block);

        if (layout.range.begin < 0 ||
            layout.range.end <= layout.range.begin) {
            layout.prefixPx = { 0, 1 };
            layout.totalHeightPx = 1;
            layout.valid = false;
            return layout;
        }

        const int blockMinutes =
            layout.range.end - layout.range.begin;

        const int hourCount =
            (std::max)(1, (blockMinutes + 59) / 60);

        std::vector<int> hourPxPerMinute(
            static_cast<std::size_t>(hourCount),
            kLiteTimelineBasePxPerMinute_);

        // 1時間単位で、その時間帯に存在する番組のうち
        // 最も厳しい「必要高さ / 表示分数」を採用する。
        for (const ProgramEntry* p : rows) {
            if (!p) {
                continue;
            }

            int beginOffset = 0;
            int endOffset = 0;

            if (!GetClippedLiteMinuteRange_(
                *p,
                layout.range,
                beginOffset,
                endOffset)) {
                continue;
            }

            if (beginOffset < 0) {
                beginOffset = 0;
            }

            if (endOffset <= beginOffset) {
                endOffset = beginOffset + 1;
            }

            if (endOffset > blockMinutes) {
                endOffset = blockMinutes;
            }

            const int visibleMinutes =
                (std::max)(1, endOffset - beginOffset);

            int spanSafetyPx =
                kLiteTimelineSpanSafetyPaddingPx_;

            if (visibleMinutes <= kLiteTimelineVeryShortMaxMinutes_) {
                spanSafetyPx += 64;
            }
            else if (visibleMinutes <= kLiteTimelineShortMaxMinutes_) {
                spanSafetyPx += 42;
            }
            else if (visibleMinutes <= kLiteTimelineCompactMaxMinutes_) {
                spanSafetyPx += 24;
            }

            const int requiredSpanPx =
                GetLiteProgramMinItemHeightPx_(
                    *p,
                    visibleMinutes,
                    channelWidthPx) +
                spanSafetyPx;

            int requiredPxPerMinute =
                requiredSpanPx / visibleMinutes;

            if ((requiredSpanPx % visibleMinutes) != 0) {
                ++requiredPxPerMinute;
            }

            requiredPxPerMinute =
                (std::max)(
                    kLiteTimelineBasePxPerMinute_,
                    requiredPxPerMinute);

            int firstHour =
                beginOffset / 60;

            int lastHour =
                (endOffset - 1) / 60;

            if (firstHour < 0) {
                firstHour = 0;
            }

            if (lastHour < firstHour) {
                lastHour = firstHour;
            }

            if (lastHour >= hourCount) {
                lastHour = hourCount - 1;
            }

            for (int h = firstHour; h <= lastHour; ++h) {
                int& bucket =
                    hourPxPerMinute[static_cast<std::size_t>(h)];

                bucket =
                    (std::max)(
                        bucket,
                        requiredPxPerMinute);
            }
        }

        std::vector<int> minuteHeight(
            static_cast<std::size_t>(blockMinutes),
            kLiteTimelineBasePxPerMinute_);

        for (int i = 0; i < blockMinutes; ++i) {
            int hourIndex =
                i / 60;

            if (hourIndex < 0) {
                hourIndex = 0;
            }

            if (hourIndex >= hourCount) {
                hourIndex = hourCount - 1;
            }

            minuteHeight[static_cast<std::size_t>(i)] =
                hourPxPerMinute[static_cast<std::size_t>(hourIndex)];
        }

        layout.prefixPx.assign(
            static_cast<std::size_t>(blockMinutes + 1),
            0);

        for (int i = 0; i < blockMinutes; ++i) {
            layout.prefixPx[static_cast<std::size_t>(i + 1)] =
                layout.prefixPx[static_cast<std::size_t>(i)] +
                minuteHeight[static_cast<std::size_t>(i)];
        }

        layout.totalHeightPx =
            (std::max)(1, layout.prefixPx.back());

        layout.valid = true;
        return layout;
    }

    int GetLiteBlockHeightPx_(
        const LiteTimelineLayout_& layout) noexcept
    {
        return (std::max)(1, layout.totalHeightPx);
    }

    int GetLiteBlockHeightPx_(
        int block) noexcept
    {
        const LiteBlockMinuteRange_ range =
            GetLiteBlockMinuteRange_(block);

        if (range.begin < 0 ||
            range.end <= range.begin) {
            return 1;
        }

        return (range.end - range.begin) *
            kLiteTimelineBasePxPerMinute_;
    }

    int GetLiteProgramTopPx_(
        const ProgramEntry& e,
        const LiteTimelineLayout_& layout) noexcept
    {
        int beginOffset = 0;
        int endOffset = 0;

        if (!layout.valid ||
            !GetClippedLiteMinuteRange_(e, layout.range, beginOffset, endOffset)) {
            return 0;
        }

        if (beginOffset < 0) {
            beginOffset = 0;
        }

        if (static_cast<std::size_t>(beginOffset) >= layout.prefixPx.size()) {
            return 0;
        }

        return layout.prefixPx[static_cast<std::size_t>(beginOffset)];
    }

    int GetLiteProgramTopPx_(
        const ProgramEntry& e,
        int block) noexcept
    {
        const LiteBlockMinuteRange_ range =
            GetLiteBlockMinuteRange_(block);

        int beginOffset = 0;
        int endOffset = 0;

        if (!GetClippedLiteMinuteRange_(e, range, beginOffset, endOffset)) {
            return 0;
        }

        return (std::max)(
            0,
            beginOffset * kLiteTimelineBasePxPerMinute_);
    }

    int GetLiteProgramHeightPx_(
        const ProgramEntry& e,
        const LiteTimelineLayout_& layout) noexcept
    {
        int beginOffset = 0;
        int endOffset = 0;

        if (!layout.valid ||
            !GetClippedLiteMinuteRange_(e, layout.range, beginOffset, endOffset)) {
            return kLiteTimelineStandardMinItemHeightPx_;
        }

        if (beginOffset < 0) {
            beginOffset = 0;
        }

        if (endOffset < beginOffset) {
            endOffset = beginOffset;
        }

        if (static_cast<std::size_t>(endOffset) >= layout.prefixPx.size()) {
            endOffset = static_cast<int>(layout.prefixPx.size()) - 1;
        }

        if (static_cast<std::size_t>(beginOffset) >= layout.prefixPx.size()) {
            return kLiteTimelineStandardMinItemHeightPx_;
        }

        const int spanPx =
            layout.prefixPx[static_cast<std::size_t>(endOffset)] -
            layout.prefixPx[static_cast<std::size_t>(beginOffset)];

        return (std::max)(1, spanPx - 2);
    }

    int GetLiteProgramHeightPx_(
        const ProgramEntry& e,
        int block) noexcept
    {
        const int visibleMinutes =
            GetLiteProgramVisibleMinutes_(e, block);

        return GetLiteProgramMinItemHeightPx_(visibleMinutes);
    }

    int GetLiteProgramVisibleMinutes_(
        const ProgramEntry& e,
        const LiteTimelineLayout_& layout) noexcept
    {
        int beginOffset = 0;
        int endOffset = 0;

        if (!layout.valid ||
            !GetClippedLiteMinuteRange_(e, layout.range, beginOffset, endOffset)) {
            return 1;
        }

        return (std::max)(1, endOffset - beginOffset);
    }

    bool IsCompactTimetableItem_(
        const ProgramEntry& e,
        int block) noexcept
    {
        return GetLiteProgramVisibleMinutes_(e, block) <=
            kLiteTimelineCompactMaxMinutes_;
    }

    bool IsCompactTimetableItem_(
        const ProgramEntry& e,
        const LiteTimelineLayout_& layout) noexcept
    {
        return GetLiteProgramVisibleMinutes_(e, layout) <=
            kLiteTimelineCompactMaxMinutes_;
    }

    bool ShouldShowTimetableSummary_(
        const ProgramEntry& e,
        int block) noexcept
    {
        if (IsCompactTimetableItem_(e, block)) {
            return false;
        }

        const int heightPx =
            GetLiteProgramHeightPx_(e, block);

        const int minutes =
            GetLiteProgramVisibleMinutes_(e, block);

        return heightPx >= kLiteTimelineSummaryMinHeightPx_ &&
            minutes >= 20;
    }

    bool ShouldShowTimetableSummary_(
        const ProgramEntry& e,
        const LiteTimelineLayout_& layout) noexcept
    {
        if (IsCompactTimetableItem_(e, layout)) {
            return false;
        }

        const int heightPx =
            GetLiteProgramHeightPx_(e, layout);

        const int minutes =
            GetLiteProgramVisibleMinutes_(e, layout);

        return heightPx >= kLiteTimelineSummaryMinHeightPx_ &&
            minutes >= 20;
    }

    int GetLiteTimelineTopPx_(
        const LiteTimelineLayout_& layout,
        int normalizedMinute) noexcept
    {
        if (!layout.valid ||
            normalizedMinute < layout.range.begin ||
            normalizedMinute > layout.range.end) {
            return 0;
        }

        int offset =
            normalizedMinute - layout.range.begin;

        if (offset < 0) {
            offset = 0;
        }

        if (static_cast<std::size_t>(offset) >= layout.prefixPx.size()) {
            offset = static_cast<int>(layout.prefixPx.size()) - 1;
        }

        return layout.prefixPx[static_cast<std::size_t>(offset)];
    }

    int GetLiteChannelColumnWidthPx_(
        std::size_t serviceCount) noexcept
    {
        if (serviceCount <= 4) {
            return 300;
        }

        if (serviceCount <= 6) {
            return 280;
        }

        return 260;
    }

    void AppendLiteTimeRail_(
        std::wostringstream& oss,
        const LiteTimelineLayout_& layout)
    {
        oss << L"<div class=\"lite-time-rail\" style=\"height:"
            << GetLiteBlockHeightPx_(layout)
            << L"px;\">\n";

        if (layout.valid &&
            layout.range.begin >= 0 &&
            layout.range.end > layout.range.begin) {
            int tick = layout.range.begin;

            while (tick <= layout.range.end) {
                const int top =
                    GetLiteTimelineTopPx_(layout, tick);

                oss << L"<div class=\"lite-time-tick\" data-lite-offset=\""
                    << (tick - layout.range.begin)
                    << L"\" style=\"top:"
                    << top
                    << L"px;\">"
                    << HtmlEscape_(FormatMinuteOfDayText_(tick))
                    << L"</div>\n";

                tick += 60;
            }
        }

        oss << L"</div>\n";
    }

    void AppendLiteTimeRail_(
        std::wostringstream& oss,
        int block)
    {
        const LiteTimelineLayout_ layout =
            BuildLiteTimelineLayout_({}, block);

        AppendLiteTimeRail_(oss, layout);
    }

    void AppendLiteHourGuides_(
        std::wostringstream& oss,
        const LiteTimelineLayout_& layout)
    {
        if (!layout.valid ||
            layout.range.begin < 0 ||
            layout.range.end <= layout.range.begin) {
            return;
        }

        int tick = layout.range.begin;

        while (tick <= layout.range.end) {
            const int top =
                GetLiteTimelineTopPx_(layout, tick);

            oss << L"<div class=\"lite-hour-guide\" data-lite-offset=\""
                << (tick - layout.range.begin)
                << L"\" style=\"top:"
                << top
                << L"px;\"></div>\n";

            tick += 60;
        }
    }

    std::wstring DateLabel_(const std::wstring& key)
    {
        if (key.size() == 8) {
            return key.substr(0, 4) + L"/" + key.substr(4, 2) + L"/" + key.substr(6, 2);
        }
        return key;
    }

    std::wstring ClipText_(std::wstring s, std::size_t maxLen)
    {
        if (maxLen == 0) {
            return {};
        }

        if (s.size() <= maxLen) {
            return s;
        }

        s.resize(maxLen);
        s += L"…";
        return s;
    }

    std::wstring ClipTitle_(const std::wstring& s, std::size_t maxLen = 80)
    {
        return ClipText_(s, maxLen);
    }

    std::wstring ClipSummary_(const std::wstring& s, std::size_t maxLen = 80)
    {
        return ClipText_(s, maxLen);
    }

    std::wstring FormatMinuteOfDayText_(int minute)
    {
        if (minute < 0) {
            return {};
        }

        minute %= (24 * 60);
        if (minute < 0) {
            minute += (24 * 60);
        }

        wchar_t buf[16]{};
        swprintf_s(buf, L"%02d:%02d", minute / 60, minute % 60);
        return std::wstring(buf);
    }

    bool IsInvalidTimeText_(
        const std::wstring& s)
    {
        std::wstring t = s;

        auto isSpace = [](wchar_t ch) noexcept {
            return ch == L' ' ||
                ch == L'\t' ||
                ch == L'\r' ||
                ch == L'\n' ||
                ch == L'　';
            };

        while (!t.empty() && isSpace(t.front())) {
            t.erase(t.begin());
        }

        while (!t.empty() && isSpace(t.back())) {
            t.pop_back();
        }

        if (t.empty()) {
            return true;
        }

        if (t == L"--:--" ||
            t == L"--" ||
            t == L"-" ||
            t == L"??:??") {
            return true;
        }

        return false;
    }

    std::wstring PickStartTimeText_(
        const ProgramEntry& e)
    {
        if (e.startMinutesOfDay >= 0) {
            return FormatMinuteOfDayText_(e.startMinutesOfDay);
        }

        if (!IsInvalidTimeText_(e.startText)) {
            return e.startText;
        }

        if (!IsInvalidTimeText_(e.startTime)) {
            return e.startTime;
        }

        return L"--:--";
    }

    std::wstring PickEndTimeText_(
        const ProgramEntry& e)
    {
        if (e.endMinutesOfDay >= 0) {
            return FormatMinuteOfDayText_(e.endMinutesOfDay);
        }

        if (!IsInvalidTimeText_(e.endText)) {
            return e.endText;
        }

        if (!IsInvalidTimeText_(e.endTime)) {
            return e.endTime;
        }

        return L"--:--";
    }

    std::wstring TimeRangeText_(
        const ProgramEntry& e)
    {
        const std::wstring start =
            PickStartTimeText_(e);

        const std::wstring end =
            PickEndTimeText_(e);

        if (start == L"--:--" && end == L"--:--") {
            return L"--:--";
        }

        return start + L"～" + end;
    }

    using LiteEndFallbackMap_ = std::unordered_map<std::wstring, int>;

    std::wstring MakeLiteProgramTimeKey_(const ProgramEntry& e)
    {
        return e.broadcastDateKey +
            L"|" +
            std::to_wstring(static_cast<unsigned int>(e.serviceId)) +
            L"|" +
            std::to_wstring(static_cast<unsigned int>(e.eventId)) +
            L"|" +
            std::to_wstring(e.startMinutesOfDay);
    }

    bool HasValidEndTime_(const ProgramEntry& e)
    {
        if (e.endMinutesOfDay >= 0) {
            return true;
        }

        if (!IsInvalidTimeText_(e.endText)) {
            return true;
        }

        if (!IsInvalidTimeText_(e.endTime)) {
            return true;
        }

        return false;
    }

    LiteEndFallbackMap_ BuildLiteEndFallbackMap_(
        const std::vector<const ProgramEntry*>& rows,
        int block)
    {
        LiteEndFallbackMap_ out;

        std::map<std::wstring, std::vector<const ProgramEntry*>> groups;

        for (const ProgramEntry* p : rows) {
            if (!p) {
                continue;
            }

            const std::wstring groupKey =
                p->broadcastDateKey +
                L"|" +
                std::to_wstring(static_cast<unsigned int>(p->serviceId));

            groups[groupKey].push_back(p);
        }

        const LiteBlockMinuteRange_ blockRange =
            GetLiteBlockMinuteRange_(block);

        for (auto& pair : groups) {
            auto& list = pair.second;

            std::sort(list.begin(), list.end(), [](const ProgramEntry* a, const ProgramEntry* b) {
                const int aa =
                    NormalizeToLiteBroadcastMinute_(a->startMinutesOfDay);

                const int bb =
                    NormalizeToLiteBroadcastMinute_(b->startMinutesOfDay);

                if (aa != bb) {
                    return aa < bb;
                }

                if (a->endMinutesOfDay != b->endMinutesOfDay) {
                    return a->endMinutesOfDay < b->endMinutesOfDay;
                }

                return a->eventId < b->eventId;
                });

            for (std::size_t i = 0; i < list.size(); ++i) {
                const ProgramEntry& e = *list[i];

                const int startNorm =
                    NormalizeToLiteBroadcastMinute_(e.startMinutesOfDay);

                if (startNorm < 0) {
                    continue;
                }

                int explicitEnd = -1;

                if (e.endMinutesOfDay >= 0) {
                    explicitEnd =
                        NormalizeToLiteBroadcastMinute_(e.endMinutesOfDay);

                    if (explicitEnd <= startNorm) {
                        explicitEnd += kLiteMinutesPerDay_;
                    }

                    if (explicitEnd <= startNorm) {
                        explicitEnd = -1;
                    }
                }

                int nextStart = -1;

                for (std::size_t j = i + 1; j < list.size(); ++j) {
                    const int candidate =
                        NormalizeToLiteBroadcastMinute_(list[j]->startMinutesOfDay);

                    if (candidate > startNorm) {
                        nextStart = candidate;
                        break;
                    }
                }

                // 終了時刻が数値化できない場合は、次番組開始を優先する。
                if (explicitEnd < 0) {
                    int fallbackEnd = -1;

                    if (nextStart > startNorm) {
                        fallbackEnd = nextStart;
                    }
                    else if (blockRange.begin >= 0 &&
                        blockRange.end > blockRange.begin &&
                        startNorm < blockRange.end) {
                        fallbackEnd = blockRange.end;
                    }
                    else {
                        fallbackEnd =
                            startNorm + kLiteTimelineMissingEndAssumedMinutes_;
                    }

                    if (fallbackEnd > startNorm) {
                        out[MakeLiteProgramTimeKey_(e)] = fallbackEnd;
                    }

                    continue;
                }

                // 有効な終了時刻があっても、次番組まで1～3分だけ空く場合は、
                // 表示上だけ前番組を伸ばして番組表の穴を消す。
                if (nextStart > explicitEnd) {
                    const int gapMinutes =
                        nextStart - explicitEnd;

                    if (gapMinutes > 0 &&
                        gapMinutes <= kLiteTableRepairGapMaxMinutes_) {
                        out[MakeLiteProgramTimeKey_(e)] = nextStart;
                    }
                }
            }
        }

        return out;
    }

    int FindLiteFallbackEndMinute_(
        const LiteEndFallbackMap_& fallbackMap,
        const ProgramEntry& e) noexcept
    {
        const auto it = fallbackMap.find(MakeLiteProgramTimeKey_(e));
        if (it == fallbackMap.end()) {
            return -1;
        }

        return it->second;
    }

    std::wstring PickEndTimeText_(
        const ProgramEntry& e,
        const LiteEndFallbackMap_& fallbackMap)
    {
        if (e.endMinutesOfDay >= 0) {
            return FormatMinuteOfDayText_(e.endMinutesOfDay);
        }

        if (!IsInvalidTimeText_(e.endText)) {
            return e.endText;
        }

        if (!IsInvalidTimeText_(e.endTime)) {
            return e.endTime;
        }

        const int fallbackEnd =
            FindLiteFallbackEndMinute_(fallbackMap, e);

        if (fallbackEnd >= 0) {
            return FormatMinuteOfDayText_(fallbackEnd);
        }

        return L"--:--";
    }

    std::wstring TimeRangeText_(
        const ProgramEntry& e,
        const LiteEndFallbackMap_& fallbackMap)
    {
        const std::wstring start =
            PickStartTimeText_(e);

        const std::wstring end =
            PickEndTimeText_(e, fallbackMap);

        if (start == L"--:--" && end == L"--:--") {
            return L"--:--";
        }

        return start + L"～" + end;
    }

    int PickLiteStartMinuteForBlockCollect_(
        const ProgramEntry& e) noexcept
    {
        return NormalizeToLiteBroadcastMinute_(
            e.startMinutesOfDay);
    }

    int PickLiteEndMinuteForBlockCollect_(
        const ProgramEntry& e,
        const LiteEndFallbackMap_& fallbackMap,
        int block) noexcept
    {
        const int begin =
            PickLiteStartMinuteForBlockCollect_(e);

        if (begin < 0) {
            return -1;
        }

        const int mappedEnd =
            FindLiteFallbackEndMinute_(
                fallbackMap,
                e);

        if (mappedEnd > begin) {
            return mappedEnd;
        }

        int end =
            NormalizeToLiteBroadcastMinute_(
                e.endMinutesOfDay);

        if (end < 0) {
            const LiteBlockMinuteRange_ blockRange =
                GetLiteBlockMinuteRange_(block);

            if (blockRange.begin >= 0 &&
                blockRange.end > blockRange.begin &&
                begin < blockRange.end) {
                return blockRange.end;
            }

            return begin + kLiteTimelineMissingEndAssumedMinutes_;
        }

        if (end <= begin) {
            end += kLiteMinutesPerDay_;
        }

        if (end <= begin) {
            end = begin + 1;
        }

        return end;
    }

    bool ProgramOverlapsLiteBlockForCollect_(
        const ProgramEntry& e,
        int block,
        const LiteEndFallbackMap_& fallbackMap) noexcept
    {
        const LiteBlockMinuteRange_ blockRange =
            GetLiteBlockMinuteRange_(block);

        if (blockRange.begin < 0 ||
            blockRange.end <= blockRange.begin) {
            return false;
        }

        const int programBegin =
            PickLiteStartMinuteForBlockCollect_(e);

        if (programBegin < 0) {
            return e.timeBlock == block;
        }

        const int programEnd =
            PickLiteEndMinuteForBlockCollect_(
                e,
                fallbackMap,
                block);

        if (programEnd <= programBegin) {
            return e.timeBlock == block;
        }

        return programBegin < blockRange.end &&
            blockRange.begin < programEnd;
    }

    void AppendReservationCss_(std::wostringstream& oss)
    {
        oss << L".reserve-actions{display:flex;gap:3px;align-items:center;justify-content:flex-end;margin-top:4px;}\n";
        oss << L".reserve-btn{min-width:30px;min-height:22px;padding:2px 5px;border:1px solid #555;background:#222;color:#eee;font-size:11px;cursor:pointer;line-height:1.1;}\n";
        oss << L".reserve-btn:hover{background:#333;border-color:#888;}\n";
        oss << L".reserve-btn.active{background:#445b75;border-color:#8ab7e6;color:#fff;}\n";

        // 時間割は表示専用にするため、予約ボタン用の右余白を取らない。
        oss << L".lite-timetable-item .program-title{padding-right:0!important;}\n";
        oss << L".lite-timetable-item.compact .program-time{padding-right:0!important;}\n";
        oss << L".lite-timetable-item.compact .program-title{padding-right:0!important;white-space:normal;overflow:visible;}\n";

        // TVRock風に番組枠そのものの背景を変える。
        oss << L"[data-reservation-state=\"reserve\"]{background:#3a1f24!important;border-color:#8a4450!important;}\n";
        oss << L"[data-reservation-state=\"record\"]{background:#4a1d24!important;border-color:#b85665!important;}\n";
        oss << L"[data-reservation-state=\"disabled\"]{background:#303030!important;border-color:#666!important;color:#ccc!important;}\n";
        oss << L"[data-reservation-state=\"auto\"]{background:#203a27!important;border-color:#4e8a5c!important;}\n";

        // 表一覧は tr だけだと td 背景に負けるので td 側も指定する。
        oss << L"tr[data-reservation-state=\"reserve\"] td{background:#3a1f24!important;}\n";
        oss << L"tr[data-reservation-state=\"record\"] td{background:#4a1d24!important;}\n";
        oss << L"tr[data-reservation-state=\"disabled\"] td{background:#303030!important;color:#ccc!important;}\n";
        oss << L"tr[data-reservation-state=\"auto\"] td{background:#203a27!important;}\n";
    }

    void AppendLiteTitleFullDisplayCss_(
        std::wostringstream& oss)
    {
        oss << L".lite-timetable-item{\n";
        oss << L"  overflow:hidden!important;\n";
        oss << L"  contain:none!important;\n";
        oss << L"}\n";

        oss << L".lite-timetable-item .program-time{\n";
        oss << L"  white-space:nowrap!important;\n";
        oss << L"}\n";

        oss << L".lite-timetable-item .program-title{\n";
        oss << L"  display:block!important;\n";
        oss << L"  padding-right:0!important;\n";
        oss << L"  white-space:normal!important;\n";
        oss << L"  overflow:visible!important;\n";
        oss << L"  text-overflow:clip!important;\n";
        oss << L"  max-height:none!important;\n";
        oss << L"  height:auto!important;\n";
        oss << L"  overflow-wrap:anywhere!important;\n";
        oss << L"  word-break:break-word!important;\n";
        oss << L"  line-break:anywhere!important;\n";
        oss << L"}\n";

        oss << L".lite-timetable-item.compact .program-title{\n";
        oss << L"  font-size:10px!important;\n";
        oss << L"  line-height:1.34!important;\n";
        oss << L"}\n";

        oss << L".lite-timetable-item .program-sub.summary{\n";
        oss << L"  display:block;\n";
        oss << L"  color:#cfd6e0;\n";
        oss << L"  font-size:11px;\n";
        oss << L"  line-height:1.35;\n";
        oss << L"  margin-top:4px;\n";
        oss << L"  overflow-wrap:anywhere;\n";
        oss << L"  word-break:break-word;\n";
        oss << L"}\n";

        // compactは短時間なので概要は出さない。
        oss << L".lite-timetable-item.compact{\n";
        oss << L"  padding:5px 7px!important;\n";
        oss << L"  font-size:11px!important;\n";
        oss << L"}\n";

        oss << L".lite-timetable-item.compact .program-time{\n";
        oss << L"  font-size:10px!important;\n";
        oss << L"  line-height:1.35!important;\n";
        oss << L"  margin-bottom:4px!important;\n";
        oss << L"  padding-right:0!important;\n";
        oss << L"}\n";

        oss << L".lite-timetable-item.compact .program-title{\n";
        oss << L"  font-size:11px!important;\n";
        oss << L"  line-height:1.42!important;\n";
        oss << L"  padding-right:0!important;\n";
        oss << L"}\n";
    }

    void AppendLiteTableTimetableCss_(
        std::wostringstream& oss)
    {
        oss << L".lite-table-wrap{\n";
        oss << L"  flex:1 1 auto;\n";
        oss << L"  min-height:0;\n";
        oss << L"  overflow:auto;\n";
        oss << L"  border:1px solid #333;\n";
        oss << L"  background:#111;\n";
        oss << L"  scrollbar-gutter:stable both-edges;\n";
        oss << L"}\n";

        oss << L".lite-epg-table{\n";
        oss << L"  border-collapse:collapse;\n";
        oss << L"  table-layout:fixed;\n";
        oss << L"  width:max-content;\n";
        oss << L"  min-width:100%;\n";
        oss << L"  background:#111;\n";
        oss << L"}\n";

        oss << L".lite-epg-table th,\n";
        oss << L".lite-epg-table td{\n";
        oss << L"  border:1px solid #303846;\n";
        oss << L"  box-sizing:border-box;\n";
        oss << L"  vertical-align:top;\n";
        oss << L"}\n";

        oss << L".lite-epg-table thead th{\n";
        oss << L"  position:sticky;\n";
        oss << L"  top:0;\n";
        oss << L"  z-index:5;\n";
        oss << L"  background:#202020;\n";
        oss << L"  color:#fff;\n";
        oss << L"  padding:7px 8px;\n";
        oss << L"  font-size:13px;\n";
        oss << L"  white-space:nowrap;\n";
        oss << L"  overflow:hidden;\n";
        oss << L"  text-overflow:ellipsis;\n";
        oss << L"}\n";

        oss << L".lite-epg-table .time-col{\n";
        oss << L"  position:sticky;\n";
        oss << L"  left:0;\n";
        oss << L"  z-index:6;\n";
        oss << L"  width:72px;\n";
        oss << L"  min-width:72px;\n";
        oss << L"  max-width:72px;\n";
        oss << L"  background:#151515;\n";
        oss << L"  color:#dfe8ff;\n";
        oss << L"  padding:2px 4px;\n";
        oss << L"  font-size:11px;\n";
        oss << L"  font-weight:bold;\n";
        oss << L"  white-space:nowrap;\n";
        oss << L"}\n";

        oss << L".lite-epg-table thead .time-col{\n";
        oss << L"  z-index:7;\n";
        oss << L"}\n";

        oss << L".lite-epg-table .svc-col{\n";
        oss << L"  width:240px;\n";
        oss << L"  min-width:240px;\n";
        oss << L"  max-width:240px;\n";
        oss << L"}\n";

        oss << L".lite-epg-table tr.lite-minute-row{\n";
        oss << L"  height:8px;\n";
        oss << L"}\n";

        oss << L".lite-epg-table tr.hour-row th,\n";
        oss << L".lite-epg-table tr.hour-row td{\n";
        oss << L"  border-top:2px solid #536174;\n";
        oss << L"}\n";

        oss << L".lite-epg-table tr.tick-row th,\n";
        oss << L".lite-epg-table tr.tick-row td{\n";
        oss << L"  border-top:1px solid #3b4654;\n";
        oss << L"}\n";

        oss << L".lite-epg-table td.empty-cell{\n";
        oss << L"  height:8px;\n";
        oss << L"  padding:0;\n";
        oss << L"  background:#101010;\n";
        oss << L"}\n";

        oss << L".lite-epg-table td.empty-cell.gap-cell{\n";
        oss << L"  padding:0;\n";
        oss << L"  background:#101010;\n";
        oss << L"  border-top-color:#253040;\n";
        oss << L"  border-bottom-color:#253040;\n";
        oss << L"}\n";

        oss << L".lite-epg-table td.prog-cell{\n";
        oss << L"  padding:5px 7px;\n";
        oss << L"  background:#18202c;\n";
        oss << L"  color:#eef3ff;\n";
        oss << L"  overflow-wrap:anywhere;\n";
        oss << L"  word-break:break-word;\n";
        oss << L"  line-break:anywhere;\n";
        oss << L"}\n";

        oss << L".lite-epg-table td.prog-cell:hover{\n";
        oss << L"  background:#243044!important;\n";
        oss << L"}\n";

        oss << L".lite-epg-table .program-time{\n";
        oss << L"  display:block;\n";
        oss << L"  margin-bottom:3px;\n";
        oss << L"  color:#d8e6ff;\n";
        oss << L"  font-size:11px;\n";
        oss << L"  font-weight:bold;\n";
        oss << L"  line-height:1.25;\n";
        oss << L"  white-space:nowrap;\n";
        oss << L"}\n";

        oss << L".lite-epg-table .program-title{\n";
        oss << L"  display:block;\n";
        oss << L"  color:#fff;\n";
        oss << L"  font-size:12px;\n";
        oss << L"  font-weight:bold;\n";
        oss << L"  line-height:1.36;\n";
        oss << L"  white-space:normal;\n";
        oss << L"  overflow:visible;\n";
        oss << L"  text-overflow:clip;\n";
        oss << L"  max-height:none;\n";
        oss << L"  overflow-wrap:anywhere;\n";
        oss << L"  word-break:break-word;\n";
        oss << L"}\n";

        oss << L".lite-epg-table .program-sub{\n";
        oss << L"  display:block;\n";
        oss << L"  margin-top:4px;\n";
        oss << L"  color:#cfd6e0;\n";
        oss << L"  font-size:11px;\n";
        oss << L"  line-height:1.35;\n";
        oss << L"  overflow-wrap:anywhere;\n";
        oss << L"  word-break:break-word;\n";
        oss << L"}\n";

        oss << L".lite-epg-table .genre{\n";
        oss << L"  color:#8fb;\n";
        oss << L"}\n";

        oss << L".lite-epg-table td.compact-cell .program-sub.summary{\n";
        oss << L"  display:none;\n";
        oss << L"}\n";

        // genre-hidden を display:none にすると rowspan table が崩れやすいので、時間割では薄くするだけにする。
        oss << L".lite-epg-table td.genre-hidden{\n";
        oss << L"  display:table-cell!important;\n";
        oss << L"  opacity:.18!important;\n";
        oss << L"}\n";
    }

    void AppendLiteSingleScrollCss_(
        std::wostringstream& oss)
    {
        oss << L"html,body{height:100%;}\n";

        oss << L"body.root-page{height:100vh;overflow:hidden;}\n";

        oss << L"body.lite-frame-page{height:100vh;min-height:0;overflow:hidden;display:flex;flex-direction:column;background:#111;}\n";

        // root側に日付・表示切替があるので、iframe内ページの重複ヘッダーは消す。
        oss << L"body.lite-frame-page .top{display:none!important;}\n";

        // iframe内は main が画面全体を受け持つ。
        oss << L"body.lite-frame-page .main{flex:1 1 auto;min-height:0;display:flex;flex-direction:column;overflow:hidden;padding:6px 8px;}\n";

        // ジャンル / サブch は固定領域。
        oss << L"body.lite-frame-page .genre-bar,\n";
        oss << L"body.lite-frame-page .subch-bar{\n";
        oss << L"  flex:0 0 auto;\n";
        oss << L"  margin:3px 0 5px;\n";
        oss << L"  gap:4px;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .genre-label,\n";
        oss << L"body.lite-frame-page .subch-label{\n";
        oss << L"  font-size:11px;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .genre-btn,\n";
        oss << L"body.lite-frame-page .subch-btn{\n";
        oss << L"  padding:3px 6px;\n";
        oss << L"  font-size:11px;\n";
        oss << L"  line-height:1.25;\n";
        oss << L"}\n";

        // 上部横スクロールバーは重複感が強いので一旦廃止。
        oss << L"body.lite-frame-page .lite-scroll-top{display:none!important;}\n";

        // 時間割はここだけスクロール。
        oss << L"body.lite-frame-page .timetable-wrap{\n";
        oss << L"  flex:1 1 auto;\n";
        oss << L"  min-height:0;\n";
        oss << L"  max-height:none;\n";
        oss << L"  max-width:none;\n";
        oss << L"  overflow:auto;\n";
        oss << L"  scrollbar-gutter:stable both-edges;\n";
        oss << L"  padding-bottom:0;\n";
        oss << L"}\n";

        // 表一覧もここだけスクロール。
        oss << L"body.lite-frame-page .lite-list-wrap{\n";
        oss << L"  flex:1 1 auto;\n";
        oss << L"  min-height:0;\n";
        oss << L"  overflow:auto;\n";
        oss << L"  border:1px solid #333;\n";
        oss << L"  background:#111;\n";
        oss << L"  scrollbar-gutter:stable both-edges;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .lite-list-wrap .archive-table{\n";
        oss << L"  min-width:860px;\n";
        oss << L"}\n";

        // サービス別ページや過去一覧も一応スクロール可能にしておく。
        oss << L"body.lite-frame-page .program-list,\n";
        oss << L"body.lite-frame-page .svc-list{\n";
        oss << L"  flex:1 1 auto;\n";
        oss << L"  min-height:0;\n";
        oss << L"  overflow:auto;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .empty{\n";
        oss << L"  flex:0 0 auto;\n";
        oss << L"}\n";
    }

    std::wstring BuildSplitCss_()
    {
        std::wostringstream oss;

        oss << L"body{margin:0;background:#111;color:#ddd;font-family:Meiryo,'Yu Gothic UI',sans-serif;}\n";
        oss << L"a{color:#9cf;text-decoration:none;}a:hover{text-decoration:underline;}\n";
        oss << L".top{padding:8px 10px;border-bottom:1px solid #333;background:#181818;}\n";
        oss << L".top h1{font-size:17px;margin:0 0 5px;}.top .m{font-size:12px;color:#aaa;}\n";
        oss << L".main{padding:8px 10px;}\n";
        oss << L".nav{display:flex;gap:6px;flex-wrap:wrap;margin:6px 0;}.nav a,.btn{border:1px solid #444;padding:5px 8px;background:#1f1f1f;color:#ddd;}\n";
        oss << L".svc-list{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:6px;margin:8px 0;}\n";
        oss << L".svc-link{border:1px solid #333;background:#181818;padding:7px 8px;display:block;}\n";
        oss << L".program-list{display:grid;grid-template-columns:1fr;gap:5px;}\n";
        oss << L".genre-bar{display:flex;gap:6px;flex-wrap:wrap;align-items:center;margin:8px 0 10px;}\n";
        oss << L".genre-label{font-size:12px;color:#aaa;}\n";
        oss << L".genre-btn{background:#1f1f1f;color:#ddd;border:1px solid #444;padding:4px 8px;cursor:pointer;}\n";
        oss << L".genre-btn:hover{background:#2a2a2a;}\n";
        oss << L".genre-btn.active{background:#345;border-color:#69c;}\n";
        oss << L".genre-hidden{display:none!important;}\n";

        oss << L".subch-bar{display:flex;gap:6px;flex-wrap:wrap;align-items:center;margin:0 0 10px;}\n";
        oss << L".subch-label{font-size:12px;color:#aaa;}\n";
        oss << L".subch-btn{background:#1f1f1f;color:#ddd;border:1px solid #444;padding:4px 8px;cursor:pointer;}\n";
        oss << L".subch-btn:hover{background:#2a2a2a;}\n";
        oss << L".subch-btn.active{background:#345;border-color:#69c;}\n";
        oss << L".subchannel-hidden{display:none!important;}\n";

        oss << L".timetable-scroll{width:100%;overflow-x:auto;overflow-y:visible;padding-bottom:8px;}\n";
        oss << L".newspaper-grid{display:grid;grid-auto-flow:column;grid-auto-columns:minmax(210px,240px);grid-template-columns:none;gap:8px;align-items:start;min-width:max-content;}\n";
        oss << L".genre-empty-col{display:none!important;}\n";

        oss << L".lite-scroll-top{overflow-x:auto;overflow-y:hidden;height:18px;margin:6px 0;border:1px solid #333;background:#151515;}\n";
        oss << L".lite-scroll-top-inner{height:1px;}\n";

        oss << L".timetable-wrap{width:100%;overflow:auto;border:1px solid #333;background:#111;max-width:100%;}\n";

        oss << L".lite-timetable{display:flex;align-items:flex-start;width:max-content;min-width:100%;--lite-channel-width:260px;}\n";

        oss << L".lite-time-rail{position:sticky;left:0;z-index:4;flex:0 0 72px;width:72px;border-right:1px solid #333;background:#151515;box-shadow:2px 0 0 rgba(0,0,0,.35);}\n";
        oss << L".lite-time-tick{position:absolute;left:0;right:0;border-top:1px solid #333;color:#dfe8ff;font-size:11px;font-weight:bold;padding:2px 0 0 4px;background:rgba(21,21,21,.92);}\n";

        oss << L".lite-channel-column{flex:0 0 var(--lite-channel-width);width:var(--lite-channel-width);border-right:1px solid #333;min-width:0;background:#111;}\n";
        oss << L".lite-channel-head{position:sticky;top:0;z-index:3;background:#202020;color:#fff;border-bottom:1px solid #333;padding:7px 8px;font-weight:bold;text-align:center;font-size:13px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}\n";

        oss << L".lite-channel-body{position:relative;background:#141414;}\n";
        oss << L".lite-hour-guide{position:absolute;left:0;right:0;height:0;border-top:1px solid #2b2b2b;pointer-events:none;z-index:0;}\n";

        oss << L".lite-timetable-item{position:absolute;left:6px;right:6px;overflow:visible;border:1px solid #3a4557;background:#18202c;color:#eef3ff;border-radius:7px;padding:5px 7px;font-size:12px;box-sizing:border-box;}\n";
        oss << L".lite-timetable-item:hover{background:#243044!important;border-color:#5f78a0;}\n";
        oss << L".lite-timetable-item .program-time{font-size:11px;color:#d8e6ff;font-weight:bold;margin-bottom:3px;white-space:nowrap;}\n";
        oss << L".lite-timetable-item .program-title{display:block;font-size:12px;font-weight:bold;line-height:1.36;white-space:normal;overflow:visible;overflow-wrap:anywhere;word-break:break-word;line-break:anywhere;max-height:none;}\n";
        oss << L".lite-timetable-item .program-sub{font-size:11px;color:#d8dde6;line-height:1.35;margin-top:4px;word-break:break-word;}\n";
        oss << L".lite-timetable-item .genre{color:#8fb;}\n";
        oss << L".lite-timetable-item.compact{padding:2px 5px;border-radius:4px;font-size:11px;}\n";
        oss << L".lite-timetable-item.compact .program-time{display:block;color:#d8e6ff;font-size:10px;margin:0 0 2px 0;white-space:nowrap;padding-right:50px;}\n";
        oss << L".lite-timetable-item.compact .program-title{display:block;font-size:11px;line-height:1.34;white-space:normal;overflow:visible;overflow-wrap:anywhere;word-break:break-word;line-break:anywhere;max-height:none;}\n";
        oss << L".lite-timetable-item.compact .program-sub{display:none;}\n";
        oss << L".newspaper-col{border:1px solid #333;background:#151515;min-width:0;}\n";
        oss << L".newspaper-head{padding:6px 8px;border-bottom:1px solid #333;background:#1f1f1f;font-size:14px;font-weight:bold;position:sticky;top:0;z-index:1;}\n";
        oss << L".newspaper-body{display:flex;flex-direction:column;}\n";
        oss << L".newspaper-item{border:0;border-bottom:1px solid #2d2d2d;background:#151515;padding:6px;}\n";
        oss << L".newspaper-item:last-child{border-bottom:0;}\n";
        oss << L".panel{margin:8px 0 12px;border:1px solid #333;background:#151515;padding:7px;}\n";
        oss << L".service-name{font-size:14px;margin:0 0 7px;color:#ddd;border-bottom:1px solid #333;padding-bottom:4px;}\n";
        oss << L".program-card-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(230px,1fr));gap:6px;}\n";
        oss << L".program-card{min-height:64px;}\n";
        oss << L".program-time{font-size:11px;color:#aaa;margin-bottom:3px;}\n";
        oss << L".program-title{font-size:13px;font-weight:bold;line-height:1.32;}\n";
        oss << L".program-sub{font-size:11px;color:#bbb;line-height:1.35;margin-top:3px;}\n";
        oss << L".p{cursor:pointer;border:1px solid #333;background:#181818;padding:6px;}.p:hover{background:#243044!important;}\n";
        oss << L".p b{display:block;font-size:13px;line-height:1.28;}.p small{display:block;font-size:11px;color:#aaa;margin-top:2px;line-height:1.35;}\n";
        oss << L".p .sum{color:#bbb;}.p .meta{color:#aaa;}.p .genre{color:#8fb;}\n";
        oss << L".empty{color:#aaa;padding:12px;border:1px solid #333;background:#171717;}\n";
        oss << L".archive-table{border-collapse:collapse;width:100%;table-layout:fixed;}.archive-table th,.archive-table td{border:1px solid #333;padding:4px;vertical-align:top;}\n";
        oss << L".archive-table th{background:#222;}.archive-table tr:nth-child(even) td{background:#171717;}.c-time{width:7em}.c-svc{width:13em}.c-title{width:auto;}\n";
        oss << L"#epgFrame{display:block;width:100%;height:calc(100vh - 154px);border:0;background:#111;}\n";
        oss << L".root-page{height:100vh;overflow:hidden;display:flex;flex-direction:column;background:#111;}\n";
        oss << L".root-page .root-top{flex:0 0 auto;padding:6px 8px;border-bottom:1px solid #333;background:#181818;}\n";
        oss << L".root-toolbar{display:flex;gap:6px;flex-wrap:wrap;align-items:center;margin:3px 0;}\n";
        oss << L".root-row{display:flex;gap:5px;flex-wrap:wrap;align-items:center;margin:0;}\n";
        oss << L".root-main-row{flex:1 1 auto;min-width:0;}\n";
        oss << L".root-sub-row{flex:0 0 auto;}\n";
        oss << L".root-status{margin-left:auto;white-space:nowrap;}\n";
        oss << L".root-page #epgFrame{flex:1 1 auto;height:auto;min-height:0;}\n";
        oss << L"button{background:#222;color:#ddd;border:1px solid #444;padding:4px 7px;cursor:pointer;font-size:12px;line-height:1.25;}button:hover{background:#2a2a2a;}button.active{background:#345;border-color:#69c;}\n";
        oss << L".status{font-size:12px;color:#aaa;line-height:1.4;}\n";
        oss << L".modal-hidden{display:none}.modal{position:fixed;inset:0;background:rgba(0,0,0,.50);z-index:9999}.modal-box{width:min(760px,calc(100% - 24px));max-height:80vh;overflow:auto;margin:7vh auto;background:#181818;color:#ddd;border:1px solid #555;padding:12px}.modal-close{float:right;font-size:18px;background:#222;color:#ddd;border:1px solid #555}.modal h2{font-size:17px;margin:0 32px 8px 0}.modal-meta{font-size:12px;color:#aaa;margin-bottom:10px}.modal-summary{white-space:pre-wrap;line-height:1.6;font-size:13px;}\n";
        oss << L"@media(max-width:800px){.svc-list{grid-template-columns:1fr}.c-svc{width:9em}.c-time{width:5.5em}.top h1{font-size:16px}#epgFrame{height:calc(100vh - 176px)}}\n";

        AppendLiteSingleScrollCss_(oss);

        // 旧div時間割用の保険。
        // SplitGuide 管理下の root / before 互換表示用として当面残す。
        AppendLiteTitleFullDisplayCss_(oss);

        // 新しい table 時間割用。
        // PageNowTimetable 側は独自CSSを持つので、ここは旧iframe互換用。
        AppendLiteTableTimetableCss_(oss);
        return oss.str();
    }



    std::wstring BuildGenreFilterJs_()
    {
        std::wostringstream oss;

        oss << L"(function(){\n";
        oss << L"'use strict';\n";
        oss << L"var storageKey='epgLiteGenreMajor';\n";

        oss << L"function readSaved(){\n";
        oss << L" try{return localStorage.getItem(storageKey)||'all';}\n";
        oss << L" catch(e){return 'all';}\n";
        oss << L"}\n";

        oss << L"function writeSaved(value){\n";
        oss << L" try{localStorage.setItem(storageKey,value||'all');}\n";
        oss << L" catch(e){}\n";
        oss << L"}\n";

        oss << L"function hasButton(toolbar,value){\n";
        oss << L" return !!toolbar.querySelector('[data-genre-filter=\"'+CSS.escape(value)+'\"]');\n";
        oss << L"}\n";

        oss << L"function normalizeValue(toolbar,value){\n";
        oss << L" if(!value){return 'all';}\n";
        oss << L" if(value==='all'){return 'all';}\n";
        oss << L" if(hasButton(toolbar,value)){return value;}\n";
        oss << L" return 'all';\n";
        oss << L"}\n";

        oss << L"function setActive(toolbar,value){\n";
        oss << L" var buttons=toolbar.querySelectorAll('[data-genre-filter]');\n";
        oss << L" buttons.forEach(function(btn){\n";
        oss << L"  if((btn.getAttribute('data-genre-filter')||'')===value){btn.classList.add('active');}\n";
        oss << L"  else{btn.classList.remove('active');}\n";
        oss << L" });\n";
        oss << L"}\n";

        oss << L"function updateEmptyColumns(scope){\n";
        oss << L" var cols=scope.querySelectorAll('[data-lite-service-col=\"1\"]');\n";
        oss << L" cols.forEach(function(col){\n";
        oss << L"  var items=col.querySelectorAll('[data-genre-item=\"1\"]');\n";
        oss << L"  var visible=false;\n";
        oss << L"  items.forEach(function(item){if(!item.classList.contains('genre-hidden')){visible=true;}});\n";
        oss << L"  if(items.length>0&&!visible){col.classList.add('genre-empty-col');}\n";
        oss << L"  else{col.classList.remove('genre-empty-col');}\n";
        oss << L" });\n";
        oss << L"}\n";

        oss << L"function apply(toolbar,value,save){\n";
        oss << L" value=normalizeValue(toolbar,value);\n";
        oss << L" var scopeId=toolbar.getAttribute('data-genre-scope')||'';\n";
        oss << L" var scope=scopeId?document.getElementById(scopeId):document;\n";
        oss << L" if(!scope){return;}\n";
        oss << L" var items=scope.querySelectorAll('[data-genre-item=\"1\"]');\n";
        oss << L" items.forEach(function(item){\n";
        oss << L"  var g=item.getAttribute('data-genre-major')||'その他';\n";
        oss << L"  if(value==='all'||g===value){item.classList.remove('genre-hidden');}\n";
        oss << L"  else{item.classList.add('genre-hidden');}\n";
        oss << L" });\n";
        oss << L" updateEmptyColumns(scope);\n";
        oss << L" setActive(toolbar,value);\n";
        oss << L" if(window.__epgLiteSyncTimetableScroll){setTimeout(window.__epgLiteSyncTimetableScroll,0);setTimeout(window.__epgLiteSyncTimetableScroll,100);}\n";
        oss << L" if(save){writeSaved(value);}\n";
        oss << L"}\n";

        oss << L"document.addEventListener('click',function(ev){\n";
        oss << L" var btn=ev.target.closest('[data-genre-filter]');\n";
        oss << L" if(!btn){return;}\n";
        oss << L" var toolbar=btn.closest('[data-genre-toolbar=\"1\"]');\n";
        oss << L" if(!toolbar){return;}\n";
        oss << L" apply(toolbar,btn.getAttribute('data-genre-filter')||'all',true);\n";
        oss << L"});\n";

        oss << L"document.addEventListener('DOMContentLoaded',function(){\n";
        oss << L" var saved=readSaved();\n";
        oss << L" document.querySelectorAll('[data-genre-toolbar=\"1\"]').forEach(function(toolbar){apply(toolbar,saved,false);});\n";
        oss << L"});\n";

        oss << L"})();\n";
        return oss.str();
    }

    std::wstring BuildSubchannelFilterJs_()
    {
        std::wostringstream oss;

        oss << L"(function(){\n";
        oss << L"'use strict';\n";
        oss << L"var storageKey='epgLiteSubchannelMode';\n";

        oss << L"function readSaved(){\n";
        oss << L" try{return localStorage.getItem(storageKey)||'show';}\n";
        oss << L" catch(e){return 'show';}\n";
        oss << L"}\n";

        oss << L"function writeSaved(value){\n";
        oss << L" try{localStorage.setItem(storageKey,value||'show');}\n";
        oss << L" catch(e){}\n";
        oss << L"}\n";

        oss << L"function setActive(toolbar,value){\n";
        oss << L" var buttons=toolbar.querySelectorAll('[data-subch-mode]');\n";
        oss << L" buttons.forEach(function(btn){\n";
        oss << L"  if((btn.getAttribute('data-subch-mode')||'show')===value){btn.classList.add('active');}\n";
        oss << L"  else{btn.classList.remove('active');}\n";
        oss << L" });\n";
        oss << L"}\n";

        oss << L"function apply(toolbar,value,save){\n";
        oss << L" value=value==='hide'?'hide':'show';\n";
        oss << L" var scopeId=toolbar.getAttribute('data-subch-scope')||'';\n";
        oss << L" var scope=scopeId?document.getElementById(scopeId):document;\n";
        oss << L" if(!scope){return;}\n";
        oss << L" var showAll=value!=='hide';\n";

        oss << L" scope.querySelectorAll('[data-subchannel-item=\"1\"]').forEach(function(el){\n";
        oss << L"  var isSub=(el.getAttribute('data-subchannel')||'0')==='1';\n";
        oss << L"  if(showAll||!isSub){el.classList.remove('subchannel-hidden');}\n";
        oss << L"  else{el.classList.add('subchannel-hidden');}\n";
        oss << L" });\n";

        oss << L" scope.querySelectorAll('[data-lite-service-col=\"1\"]').forEach(function(col){\n";
        oss << L"  var isSub=(col.getAttribute('data-subchannel')||'0')==='1';\n";
        oss << L"  if(showAll||!isSub){col.classList.remove('subchannel-hidden');}\n";
        oss << L"  else{col.classList.add('subchannel-hidden');}\n";
        oss << L" });\n";

        oss << L" setActive(toolbar,value);\n";
        oss << L" if(window.__epgLiteSyncTimetableScroll){setTimeout(window.__epgLiteSyncTimetableScroll,0);setTimeout(window.__epgLiteSyncTimetableScroll,100);}\n";
        oss << L" if(save){writeSaved(value);}\n";
        oss << L"}\n";

        oss << L"document.addEventListener('click',function(ev){\n";
        oss << L" var btn=ev.target.closest('[data-subch-mode]');\n";
        oss << L" if(!btn){return;}\n";
        oss << L" var toolbar=btn.closest('[data-subch-toolbar=\"1\"]');\n";
        oss << L" if(!toolbar){return;}\n";
        oss << L" apply(toolbar,btn.getAttribute('data-subch-mode')||'show',true);\n";
        oss << L"});\n";

        oss << L"document.addEventListener('DOMContentLoaded',function(){\n";
        oss << L" var saved=readSaved();\n";
        oss << L" document.querySelectorAll('[data-subch-toolbar=\"1\"]').forEach(function(toolbar){apply(toolbar,saved,false);});\n";
        oss << L"});\n";

        oss << L"})();\n";

        return oss.str();
    }

    void AppendReservationStorageJs_(std::wostringstream& oss)
    {
        oss << L"var storageKey='epgLiteReservationState';\n";

        oss << L"function loadMap(){\n";
        oss << L" try{return JSON.parse(localStorage.getItem(storageKey)||'{}')||{};}\n";
        oss << L" catch(e){return {};}\n";
        oss << L"}\n";

        oss << L"function saveMap(map){\n";
        oss << L" try{localStorage.setItem(storageKey,JSON.stringify(map));}\n";
        oss << L" catch(e){}\n";
        oss << L"}\n";
    }

    void AppendReservationStateJs_(std::wostringstream& oss)
    {
        oss << L"function setButtonActive(el,state){\n";
        oss << L" var buttons=el.querySelectorAll('[data-reserve-action]');\n";
        oss << L" buttons.forEach(function(btn){\n";
        oss << L"  var action=btn.getAttribute('data-reserve-action')||'';\n";
        oss << L"  if(action===state){btn.classList.add('active');}\n";
        oss << L"  else{btn.classList.remove('active');}\n";
        oss << L" });\n";
        oss << L"}\n";

        oss << L"function applyState(el,state){\n";
        oss << L" state=state||'none';\n";
        oss << L" el.setAttribute('data-reservation-state',state);\n";
        oss << L" setButtonActive(el,state);\n";
        oss << L"}\n";

        // CSS.escape に頼らず、キー一致で全要素を更新する。
        oss << L"function applyAllByKey(key,state){\n";
        oss << L" if(!key){return;}\n";
        oss << L" document.querySelectorAll('[data-reserve-key]').forEach(function(el){\n";
        oss << L"  if((el.getAttribute('data-reserve-key')||'')===key){applyState(el,state);}\n";
        oss << L" });\n";
        oss << L"}\n";

        oss << L"function restore(){\n";
        oss << L" var map=loadMap();\n";
        oss << L" document.querySelectorAll('[data-reserve-key]').forEach(function(el){\n";
        oss << L"  var key=el.getAttribute('data-reserve-key')||'';\n";
        oss << L"  applyState(el,map[key]||'none');\n";
        oss << L" });\n";
        oss << L"}\n";
    }

    void AppendReservationClickJs_(std::wostringstream& oss)
    {
        oss << L"document.addEventListener('click',function(ev){\n";
        oss << L" var btn=ev.target.closest('[data-reserve-action]');\n";
        oss << L" if(!btn){return;}\n";

        // 詳細モーダルのクリック処理へ伝播させない。
        oss << L" ev.preventDefault();\n";
        oss << L" ev.stopPropagation();\n";

        oss << L" var card=btn.closest('[data-reserve-key]');\n";
        oss << L" if(!card){return;}\n";

        oss << L" var key=card.getAttribute('data-reserve-key')||'';\n";
        oss << L" var action=btn.getAttribute('data-reserve-action')||'none';\n";
        oss << L" var now=card.getAttribute('data-reservation-state')||'none';\n";
        oss << L" var next='none';\n";

        oss << L" if(action==='delete'){next='none';}\n";
        oss << L" else if(now===action){next='none';}\n";
        oss << L" else{next=action;}\n";

        oss << L" var map=loadMap();\n";
        oss << L" if(next==='none'){delete map[key];}\n";
        oss << L" else{map[key]=next;}\n";

        oss << L" saveMap(map);\n";
        oss << L" applyAllByKey(key,next);\n";

        // 色変更後に横スクロール幅を再計算する保険。
        oss << L" if(window.__epgLiteSyncTimetableScroll){setTimeout(window.__epgLiteSyncTimetableScroll,0);}\n";
        oss << L"});\n";

        oss << L"document.addEventListener('DOMContentLoaded',restore);\n";
    }

    std::wstring BuildReservationUiJs_()
    {
        std::wostringstream oss;

        oss << L"(function(){\n";
        oss << L"'use strict';\n";

        AppendReservationStorageJs_(oss);
        AppendReservationStateJs_(oss);
        AppendReservationClickJs_(oss);

        oss << L"})();\n";

        return oss.str();
    }

    std::wstring BuildTimetableScrollSyncJs_()
    {
        std::wostringstream oss;

        oss << L"(function(){\n";
        oss << L"'use strict';\n";
        oss << L"function id(x){return document.getElementById(x);}\n";

        oss << L"function setup(){\n";
        oss << L" var top=id('liteScrollTop');\n";
        oss << L" var inner=id('liteScrollTopInner');\n";
        oss << L" var body=id('genreScope');\n";
        oss << L" var content=id('liteScrollBody');\n";
        oss << L" if(!top||!inner||!body||!content){return;}\n";
        oss << L" var lock=false;\n";

        oss << L" function bodyMax(){return Math.max(0,body.scrollWidth-body.clientWidth);}\n";
        oss << L" function topMax(){return Math.max(0,top.scrollWidth-top.clientWidth);}\n";

        oss << L" function syncWidth(){\n";
        oss << L"  var bClient=Math.max(1,body.clientWidth);\n";
        oss << L"  var bScroll=Math.max(bClient,body.scrollWidth,content.scrollWidth);\n";
        oss << L"  if(bScroll<=bClient+1){\n";
        oss << L"   top.style.display='none';\n";
        oss << L"   inner.style.width='1px';\n";
        oss << L"   return;\n";
        oss << L"  }\n";
        oss << L"  top.style.display='block';\n";
        oss << L"  var tClient=Math.max(1,top.clientWidth);\n";
        oss << L"  var innerWidth=Math.ceil(tClient*(bScroll/bClient));\n";
        oss << L"  inner.style.width=innerWidth+'px';\n";
        oss << L"  setTopFromBody();\n";
        oss << L" }\n";

        oss << L" function setBodyFromTop(){\n";
        oss << L"  var tm=topMax();\n";
        oss << L"  var bm=bodyMax();\n";
        oss << L"  if(tm<=0||bm<=0){return;}\n";
        oss << L"  body.scrollLeft=(top.scrollLeft/tm)*bm;\n";
        oss << L" }\n";

        oss << L" function setTopFromBody(){\n";
        oss << L"  var bm=bodyMax();\n";
        oss << L"  var tm=topMax();\n";
        oss << L"  if(tm<=0||bm<=0){top.scrollLeft=0;return;}\n";
        oss << L"  top.scrollLeft=(body.scrollLeft/bm)*tm;\n";
        oss << L" }\n";

        oss << L" top.addEventListener('scroll',function(){\n";
        oss << L"  if(lock){return;}\n";
        oss << L"  lock=true;\n";
        oss << L"  setBodyFromTop();\n";
        oss << L"  lock=false;\n";
        oss << L" });\n";

        oss << L" body.addEventListener('scroll',function(){\n";
        oss << L"  if(lock){return;}\n";
        oss << L"  lock=true;\n";
        oss << L"  setTopFromBody();\n";
        oss << L"  lock=false;\n";
        oss << L" });\n";

        oss << L" function delayedSync(){\n";
        oss << L"  setTimeout(syncWidth,0);\n";
        oss << L"  setTimeout(syncWidth,80);\n";
        oss << L"  setTimeout(syncWidth,250);\n";
        oss << L" }\n";

        oss << L" window.__epgLiteSyncTimetableScroll=syncWidth;\n";
        oss << L" window.addEventListener('resize',delayedSync);\n";
        oss << L" document.addEventListener('click',delayedSync);\n";

        oss << L" if(window.ResizeObserver){\n";
        oss << L"  var ro=new ResizeObserver(delayedSync);\n";
        oss << L"  ro.observe(body);\n";
        oss << L"  ro.observe(content);\n";
        oss << L" }\n";

        oss << L" if(window.MutationObserver){\n";
        oss << L"  var mo=new MutationObserver(delayedSync);\n";
        oss << L"  mo.observe(content,{attributes:true,subtree:true,attributeFilter:['class','style']});\n";
        oss << L" }\n";

        oss << L" delayedSync();\n";
        oss << L" syncWidth();\n";
        oss << L"}\n";

        oss << L"if(document.readyState==='loading'){\n";
        oss << L" document.addEventListener('DOMContentLoaded',setup);\n";
        oss << L"}else{\n";
        oss << L" setup();\n";
        oss << L"}\n";

        oss << L"})();\n";

        return oss.str();
    }

    std::wstring BuildTimetableAutoLayoutJs_()
    {
        std::wostringstream oss;

        oss << L"(function(){\n";
        oss << L"'use strict';\n";
        oss << L"var BASE_PX_PER_MINUTE=8;\n";
        oss << L"var timer=0;\n";

        oss << L"function id(x){return document.getElementById(x);}\n";
        oss << L"function toInt(v,def){var n=parseInt(v,10);return isNaN(n)?def:n;}\n";

        oss << L"function buildPrefix(minuteHeight){\n";
        oss << L" var prefix=[0];\n";
        oss << L" for(var i=0;i<minuteHeight.length;i++){\n";
        oss << L"  prefix[i+1]=prefix[i]+minuteHeight[i];\n";
        oss << L" }\n";
        oss << L" return prefix;\n";
        oss << L"}\n";

        oss << L"function addExtra(minuteHeight,begin,end,extra){\n";
        oss << L" if(extra<=0){return;}\n";
        oss << L" if(begin<0){begin=0;}\n";
        oss << L" if(end<=begin){end=begin+1;}\n";
        oss << L" if(end>minuteHeight.length){end=minuteHeight.length;}\n";
        oss << L" var count=Math.max(1,end-begin);\n";
        oss << L" var each=Math.floor(extra/count);\n";
        oss << L" var remain=extra%count;\n";
        oss << L" for(var i=begin;i<end;i++){\n";
        oss << L"  minuteHeight[i]+=each;\n";
        oss << L"  if(remain>0){minuteHeight[i]++;remain--;}\n";
        oss << L" }\n";
        oss << L"}\n";

        oss << L"function visibleCard(card){\n";
        oss << L" if(!card){return false;}\n";
        oss << L" if(card.classList.contains('genre-hidden')){return false;}\n";
        oss << L" if(card.classList.contains('subchannel-hidden')){return false;}\n";
        oss << L" var cs=window.getComputedStyle(card);\n";
        oss << L" return cs.display!=='none' && cs.visibility!=='hidden';\n";
        oss << L"}\n";

        oss << L"function requiredHeight(card){\n";
        oss << L" var oldHeight=card.style.height;\n";
        oss << L" card.style.setProperty('height','auto','important');\n";
        oss << L" card.style.setProperty('overflow','visible','important');\n";
        oss << L" var h=Math.ceil(card.scrollHeight)+6;\n";
        oss << L" var actions=card.querySelector('.reserve-actions');\n";
        oss << L" if(actions){\n";
        oss << L"  h=Math.max(h,Math.ceil(actions.offsetTop+actions.offsetHeight)+8);\n";
        oss << L" }\n";
        oss << L" card.style.height=oldHeight;\n";
        oss << L" return Math.max(1,h);\n";
        oss << L"}\n";

        oss << L"function layout(){\n";
        oss << L" var scope=id('genreScope');\n";
        oss << L" var table=id('liteScrollBody');\n";
        oss << L" if(!scope||!table){return;}\n";

        oss << L" var cards=[];\n";
        oss << L" table.querySelectorAll('.lite-timetable-item[data-lite-begin-offset][data-lite-end-offset]').forEach(function(card){\n";
        oss << L"  if(visibleCard(card)){cards.push(card);}\n";
        oss << L" });\n";

        oss << L" var maxEnd=1;\n";
        oss << L" cards.forEach(function(card){\n";
        oss << L"  var b=toInt(card.getAttribute('data-lite-begin-offset'),0);\n";
        oss << L"  var e=toInt(card.getAttribute('data-lite-end-offset'),b+1);\n";
        oss << L"  if(e<=b){e=b+1;}\n";
        oss << L"  if(e>maxEnd){maxEnd=e;}\n";
        oss << L" });\n";

        oss << L" var minuteHeight=[];\n";
        oss << L" for(var i=0;i<maxEnd;i++){minuteHeight[i]=BASE_PX_PER_MINUTE;}\n";

        oss << L" for(var pass=0;pass<6;pass++){\n";
        oss << L"  var prefix=buildPrefix(minuteHeight);\n";
        oss << L"  var changed=false;\n";
        oss << L"  cards.forEach(function(card){\n";
        oss << L"   var b=toInt(card.getAttribute('data-lite-begin-offset'),0);\n";
        oss << L"   var e=toInt(card.getAttribute('data-lite-end-offset'),b+1);\n";
        oss << L"   if(e<=b){e=b+1;}\n";
        oss << L"   if(e>minuteHeight.length){e=minuteHeight.length;}\n";
        oss << L"   card.style.setProperty('top',prefix[b]+'px','important');\n";
        oss << L"   card.style.setProperty('height','auto','important');\n";
        oss << L"   var need=requiredHeight(card)+2;\n";
        oss << L"   var cur=Math.max(1,prefix[e]-prefix[b]-2);\n";
        oss << L"   if(cur<need){\n";
        oss << L"    addExtra(minuteHeight,b,e,need-cur);\n";
        oss << L"    changed=true;\n";
        oss << L"   }\n";
        oss << L"  });\n";
        oss << L"  if(!changed){break;}\n";
        oss << L" }\n";

        oss << L" var finalPrefix=buildPrefix(minuteHeight);\n";
        oss << L" var total=Math.max(1,finalPrefix[finalPrefix.length-1]);\n";

        oss << L" table.querySelectorAll('.lite-channel-body,.lite-time-rail').forEach(function(el){\n";
        oss << L"  el.style.setProperty('height',total+'px','important');\n";
        oss << L" });\n";

        oss << L" cards.forEach(function(card){\n";
        oss << L"  var b=toInt(card.getAttribute('data-lite-begin-offset'),0);\n";
        oss << L"  var e=toInt(card.getAttribute('data-lite-end-offset'),b+1);\n";
        oss << L"  if(e<=b){e=b+1;}\n";
        oss << L"  if(e>=finalPrefix.length){e=finalPrefix.length-1;}\n";
        oss << L"  var top=finalPrefix[b];\n";
        oss << L"  var h=Math.max(1,finalPrefix[e]-finalPrefix[b]-2);\n";
        oss << L"  card.style.setProperty('top',top+'px','important');\n";
        oss << L"  card.style.setProperty('height',h+'px','important');\n";
        oss << L"  card.style.setProperty('overflow','hidden','important');\n";
        oss << L" });\n";

        oss << L" table.querySelectorAll('[data-lite-offset]').forEach(function(el){\n";
        oss << L"  var off=toInt(el.getAttribute('data-lite-offset'),0);\n";
        oss << L"  if(off<0){off=0;}\n";
        oss << L"  if(off>=finalPrefix.length){off=finalPrefix.length-1;}\n";
        oss << L"  el.style.setProperty('top',finalPrefix[off]+'px','important');\n";
        oss << L" });\n";

        oss << L" if(window.__epgLiteSyncTimetableScroll){\n";
        oss << L"  setTimeout(window.__epgLiteSyncTimetableScroll,0);\n";
        oss << L"  setTimeout(window.__epgLiteSyncTimetableScroll,80);\n";
        oss << L" }\n";
        oss << L"}\n";

        oss << L"function schedule(){\n";
        oss << L" if(timer){cancelAnimationFrame(timer);}\n";
        oss << L" timer=requestAnimationFrame(function(){timer=0;layout();});\n";
        oss << L"}\n";

        oss << L"window.__epgLiteRelayoutTimetable=schedule;\n";

        oss << L"if(document.readyState==='loading'){\n";
        oss << L" document.addEventListener('DOMContentLoaded',schedule);\n";
        oss << L"}else{\n";
        oss << L" schedule();\n";
        oss << L"}\n";

        oss << L"window.addEventListener('load',schedule);\n";
        oss << L"window.addEventListener('resize',schedule);\n";
        oss << L"document.addEventListener('click',function(){setTimeout(schedule,0);});\n";

        oss << L"if(window.MutationObserver){\n";
        oss << L" var mo=new MutationObserver(schedule);\n";
        oss << L" if(document.body){mo.observe(document.body,{attributes:true,subtree:true,attributeFilter:['class','style']});}\n";
        oss << L"}\n";

        oss << L"})();\n";

        return oss.str();
    }

    std::wstring BuildHeader_(
        const std::wstring& title,
        const std::wstring& meta,
        const std::wstring& liteRootPrefix,
        const std::wstring& detailBase)
    {
        (void)detailBase;

        std::wostringstream oss;

        oss << L"<!doctype html>\n<html lang=\"ja\">\n<head>\n<meta charset=\"utf-8\">\n";
        oss << L"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n";
        oss << L"<title>" << HtmlEscape_(title) << L"</title>\n";
        oss << L"<link rel=\"stylesheet\" href=\"" << HtmlEscape_(liteRootPrefix) << L"epg_lite_split.css\">\n";
        oss << L"<script defer src=\"" << HtmlEscape_(liteRootPrefix) << L"epg_lite_genre_filter.js\"></script>\n";
        oss << L"<script defer src=\"" << HtmlEscape_(liteRootPrefix) << L"epg_lite_subchannel_filter.js\"></script>\n";
        oss << L"</head>\n<body class=\"lite-frame-page\">\n<header class=\"top\">\n";
        oss << L"<h1>" << HtmlEscape_(title) << L"</h1>\n";
        oss << L"<div class=\"m\">" << HtmlEscape_(meta) << L"</div>\n";

        return oss.str();
    }

    std::wstring BuildFooter_()
    {
        return L"</main>\n</body>\n</html>\n";
    }

    std::wstring BuildPageNav_(const std::wstring& serverIndexHref)
    {
        std::wostringstream oss;

        oss << L"<nav class=\"nav\">\n";
        oss << L"<a href=\"" << HtmlEscape_(serverIndexHref) << L"\" target=\"_top\">EPGトップ</a>\n";
        oss << L"</nav>\n</header>\n<main class=\"main\">\n";

        return oss.str();
    }

    bool IsKeptEntry_(const ProgramEntry& e, int space, int futureDays, int pastKeepDays)
    {
        if (e.space != space || e.broadcastDateKey.empty() || e.title.empty()) {
            return false;
        }

        if (e.isPast) {
            if (pastKeepDays > 0 && e.dayIndex < -pastKeepDays) {
                return false;
            }
            return true;
        }

        if (e.dayIndex < 0) {
            return false;
        }

        if (futureDays > 0 && e.dayIndex >= futureDays) {
            return false;
        }

        return true;
    }

    std::vector<const ProgramEntry*> CollectForDate_(
        const EpgHtmlLite::SharedData& data,
        int space,
        const std::wstring& dateKey,
        bool past,
        int futureDays,
        int pastKeepDays)
    {
        std::vector<const ProgramEntry*> rows;

        for (const auto& e : data.entries) {
            if (!IsKeptEntry_(e, space, futureDays, pastKeepDays)) {
                continue;
            }
            if (e.broadcastDateKey != dateKey) {
                continue;
            }
            if (e.isPast != past) {
                continue;
            }

            rows.push_back(&e);
        }

        std::sort(rows.begin(), rows.end(), [](const ProgramEntry* a, const ProgramEntry* b) {
            if (a->startMinutesOfDay != b->startMinutesOfDay) return a->startMinutesOfDay < b->startMinutesOfDay;
            if (a->serviceName != b->serviceName) return a->serviceName < b->serviceName;
            if (a->sortOrder != b->sortOrder) return a->sortOrder < b->sortOrder;
            return a->title < b->title;
            });

        return rows;
    }

    std::vector<const ProgramEntry*> CollectForDateBlock_(
        const EpgHtmlLite::SharedData& data,
        int space,
        const std::wstring& dateKey,
        bool past,
        int block,
        int futureDays,
        int pastKeepDays)
    {
        std::vector<const ProgramEntry*> candidates;

        for (const auto& e : data.entries) {
            if (!IsKeptEntry_(e, space, futureDays, pastKeepDays)) {
                continue;
            }

            if (e.broadcastDateKey != dateKey) {
                continue;
            }

            if (e.isPast != past) {
                continue;
            }

            if (e.title.empty()) {
                continue;
            }

            candidates.push_back(&e);
        }

        const LiteEndFallbackMap_ endFallbackMap =
            BuildLiteEndFallbackMap_(
                candidates,
                block);

        std::vector<const ProgramEntry*> rows;
        rows.reserve(candidates.size());

        for (const ProgramEntry* p : candidates) {
            if (!p) {
                continue;
            }

            if (block >= 0 &&
                !ProgramOverlapsLiteBlockForCollect_(
                    *p,
                    block,
                    endFallbackMap)) {
                continue;
            }

            rows.push_back(p);
        }

        std::sort(
            rows.begin(),
            rows.end(),
            [](const ProgramEntry* a, const ProgramEntry* b) {
                const int aStart =
                    a ? PickLiteStartMinuteForBlockCollect_(*a) : -1;

                const int bStart =
                    b ? PickLiteStartMinuteForBlockCollect_(*b) : -1;

                if (aStart != bStart) {
                    return aStart < bStart;
                }

                if (!a || !b) {
                    return a < b;
                }

                if (a->serviceName != b->serviceName) {
                    return a->serviceName < b->serviceName;
                }

                if (a->sortOrder != b->sortOrder) {
                    return a->sortOrder < b->sortOrder;
                }

                if (a->title != b->title) {
                    return a->title < b->title;
                }

                return a->eventId < b->eventId;
            });

        return rows;
    }

    std::vector<const ProgramEntry*> CollectForDateService_(
        const EpgHtmlLite::SharedData& data,
        int space,
        const std::wstring& dateKey,
        bool past,
        std::uint16_t serviceId,
        int block,
        int futureDays,
        int pastKeepDays)
    {
        std::vector<const ProgramEntry*> rows;

        for (const auto& e : data.entries) {
            if (!IsKeptEntry_(e, space, futureDays, pastKeepDays)) {
                continue;
            }
            if (e.broadcastDateKey != dateKey) {
                continue;
            }
            if (e.isPast != past) {
                continue;
            }
            if (e.serviceId != serviceId) {
                continue;
            }

            // block >= 0 のときは、開始時刻ではなく
            // 「番組の放送時間帯」と「表示ブロック」の交差で判定する。
            if (block >= 0 && !ProgramOverlapsLiteBlock_(e, block)) {
                continue;
            }

            rows.push_back(&e);
        }

        std::sort(rows.begin(), rows.end(), [](const ProgramEntry* a, const ProgramEntry* b) {
            if (a->startMinutesOfDay != b->startMinutesOfDay) return a->startMinutesOfDay < b->startMinutesOfDay;
            if (a->endMinutesOfDay != b->endMinutesOfDay) return a->endMinutesOfDay < b->endMinutesOfDay;
            if (a->sortOrder != b->sortOrder) return a->sortOrder < b->sortOrder;
            if (a->serviceName != b->serviceName) return a->serviceName < b->serviceName;
            if (a->title != b->title) return a->title < b->title;
            return a->eventId < b->eventId;
            });

        return rows;
    }

    const std::array<std::wstring_view, 12>& LiteGenreMajorList_()
    {
        static const std::array<std::wstring_view, 12> genres = {
            L"ニュース・報道",
            L"スポーツ",
            L"ドラマ",
            L"音楽",
            L"バラエティー",
            L"映画",
            L"アニメ・特撮",
            L"情報・ワイドショー",
            L"ドキュメンタリー",
            L"劇場・公演",
            L"趣味・教育",
            L"その他"
        };
        return genres;
    }

    std::wstring TrimGenreText_(std::wstring s)
    {
        auto isSpace = [](wchar_t ch) noexcept {
            return ch == L' ' ||
                ch == L'\t' ||
                ch == L'\r' ||
                ch == L'\n' ||
                ch == L'　';
            };

        while (!s.empty() && isSpace(s.front())) {
            s.erase(s.begin());
        }

        while (!s.empty() && isSpace(s.back())) {
            s.pop_back();
        }

        return s;
    }

    bool TryParseAutoBaseInt_(
        std::wstring_view text,
        int& value) noexcept
    {
        while (!text.empty() &&
            (text.front() == L' ' ||
                text.front() == L'\t' ||
                text.front() == L'=')) {
            text.remove_prefix(1);
        }

        while (!text.empty() &&
            (text.back() == L' ' ||
                text.back() == L'\t' ||
                text.back() == L',' ||
                text.back() == L')' ||
                text.back() == L']')) {
            text.remove_suffix(1);
        }

        if (text.empty()) {
            return false;
        }

        std::wstring tmp(text);
        wchar_t* endPtr = nullptr;

        const long parsed =
            std::wcstol(
                tmp.c_str(),
                &endPtr,
                0);

        if (endPtr == tmp.c_str()) {
            return false;
        }

        value = static_cast<int>(parsed);
        return true;
    }

    int NormalizeLiteGenreCode_(
        int major) noexcept
    {
        if (major >= 0x0 && major <= 0xA) {
            return major;
        }

        if (major == 0xB || major == 0xF) {
            return major;
        }

        return 0xF;
    }

    std::wstring GenreMajorNameFromCode_(
        int major)
    {
        switch (NormalizeLiteGenreCode_(major)) {
        case 0x0:
            return L"ニュース・報道";
        case 0x1:
            return L"スポーツ";
        case 0x2:
            return L"情報・ワイドショー";
        case 0x3:
            return L"ドラマ";
        case 0x4:
            return L"音楽";
        case 0x5:
            return L"バラエティー";
        case 0x6:
            return L"映画";
        case 0x7:
            return L"アニメ・特撮";
        case 0x8:
            return L"ドキュメンタリー";
        case 0x9:
            return L"劇場・公演";
        case 0xA:
            return L"趣味・教育";
        case 0xB:
        case 0xF:
        default:
            return L"その他";
        }
    }

    bool TryExtractGenreCodeFromText_(
        const std::wstring& text,
        int& outMajor) noexcept
    {
        outMajor = -1;

        if (text.empty()) {
            return false;
        }

        const std::array<std::wstring_view, 4> keys = {
            L"primary=",
            L"genre_primary=",
            L"genrePrimary=",
            L"major="
        };

        for (std::wstring_view key : keys) {
            const std::size_t pos =
                text.find(key);

            if (pos == std::wstring::npos) {
                continue;
            }

            const std::size_t valueBegin =
                pos + key.size();

            std::size_t valueEnd =
                text.find_first_of(L", )]\r\n\t", valueBegin);

            if (valueEnd == std::wstring::npos) {
                valueEnd = text.size();
            }

            int parsed = -1;

            if (TryParseAutoBaseInt_(
                std::wstring_view(text).substr(
                    valueBegin,
                    valueEnd - valueBegin),
                parsed)) {
                outMajor = NormalizeLiteGenreCode_(parsed);
                return true;
            }
        }

        int parsed = -1;

        if (TryParseAutoBaseInt_(text, parsed)) {
            outMajor = NormalizeLiteGenreCode_(parsed);
            return true;
        }

        return false;
    }

    std::wstring NormalizeLiteGenreName_(
        std::wstring genre)
    {
        genre = TrimGenreText_(std::move(genre));

        if (genre.empty()) {
            return L"その他";
        }

        const std::wstring sep = L">>";
        const std::size_t sepPos =
            genre.find(sep);

        if (sepPos != std::wstring::npos) {
            genre = genre.substr(0, sepPos);
            genre = TrimGenreText_(std::move(genre));
        }

        for (std::wstring_view g : LiteGenreMajorList_()) {
            if (genre == g) {
                return std::wstring(g);
            }
        }

        if (genre == L"news") {
            return L"ニュース・報道";
        }
        if (genre == L"sports") {
            return L"スポーツ";
        }
        if (genre == L"info") {
            return L"情報・ワイドショー";
        }
        if (genre == L"drama") {
            return L"ドラマ";
        }
        if (genre == L"music") {
            return L"音楽";
        }
        if (genre == L"variety") {
            return L"バラエティー";
        }
        if (genre == L"movie") {
            return L"映画";
        }
        if (genre == L"anime") {
            return L"アニメ・特撮";
        }
        if (genre == L"documentary") {
            return L"ドキュメンタリー";
        }
        if (genre == L"stage") {
            return L"劇場・公演";
        }
        if (genre == L"hobby") {
            return L"趣味・教育";
        }
        if (genre == L"other") {
            return L"その他";
        }

        int major = -1;

        if (TryExtractGenreCodeFromText_(genre, major)) {
            return GenreMajorNameFromCode_(major);
        }

        return L"その他";
    }

    std::wstring ExtractGenreMajor_(
        const std::wstring& genreKey)
    {
        return NormalizeLiteGenreName_(genreKey);
    }

    std::wstring BuildGenreToolbar_(const std::wstring& scopeId)
    {
        std::wostringstream oss;

        oss << L"<nav class=\"genre-bar\" data-genre-toolbar=\"1\" data-genre-scope=\""
            << HtmlEscape_(scopeId)
            << L"\">\n";

        oss << L"<span class=\"genre-label\">ジャンル:</span>\n";
        oss << L"<button type=\"button\" class=\"genre-btn active\" data-genre-filter=\"all\">すべて</button>\n";

        for (std::wstring_view genre : LiteGenreMajorList_()) {
            oss << L"<button type=\"button\" class=\"genre-btn\" data-genre-filter=\""
                << HtmlEscape_(genre)
                << L"\">";
            oss << HtmlEscape_(genre);
            oss << L"</button>\n";
        }

        oss << L"</nav>\n";
        return oss.str();
    }

    std::wstring BuildSubchannelToolbar_(const std::wstring& scopeId)
    {
        std::wostringstream oss;

        oss << L"<nav class=\"subch-bar\" data-subch-toolbar=\"1\" data-subch-scope=\""
            << HtmlEscape_(scopeId)
            << L"\">\n";

        oss << L"<span class=\"subch-label\">サブch:</span>\n";
        oss << L"<button type=\"button\" class=\"subch-btn active\" data-subch-mode=\"show\">全表示</button>\n";
        oss << L"<button type=\"button\" class=\"subch-btn\" data-subch-mode=\"hide\">サブ非表示</button>\n";

        oss << L"</nav>\n";

        return oss.str();
    }

    int GenreStatsIndexFromMajorName_(
        const std::wstring& major)
    {
        const auto& genres =
            LiteGenreMajorList_();

        for (std::size_t i = 0; i < genres.size(); ++i) {
            if (major == genres[i]) {
                return static_cast<int>(i);
            }
        }

        return 11;
    }

    struct LiteGenreDiagnostics_
    {
        std::array<std::size_t, 12> majorCount{};
        std::size_t total = 0;
        std::size_t emptySource = 0;
        std::size_t normalizedOther = 0;
    };

    void AddLiteGenreDiagnostics_(
        LiteGenreDiagnostics_& diag,
        const ProgramEntry& e)
    {
        ++diag.total;

        if (e.genreKey.empty()) {
            ++diag.emptySource;
        }

        const std::wstring major =
            ExtractGenreMajor_(e.genreKey);

        const int index =
            GenreStatsIndexFromMajorName_(major);

        if (index >= 0 &&
            index < static_cast<int>(diag.majorCount.size())) {
            ++diag.majorCount[static_cast<std::size_t>(index)];
        }

        if (major == L"その他") {
            ++diag.normalizedOther;
        }
    }

    std::wstring BuildGenreDiagnosticsText_(
        const LiteGenreDiagnostics_& diag)
    {
        std::wostringstream oss;

        oss << L"genreTotal=" << diag.total << L"\n";
        oss << L"genreEmptySource=" << diag.emptySource << L"\n";
        oss << L"genreNormalizedOther=" << diag.normalizedOther << L"\n";

        const auto& genres =
            LiteGenreMajorList_();

        for (std::size_t i = 0; i < genres.size(); ++i) {
            oss << L"genre[";
            oss << genres[i];
            oss << L"]=";
            oss << diag.majorCount[i];
            oss << L"\n";
        }

        if (diag.total > 0 &&
            diag.normalizedOther * 100 >= diag.total * 90) {
            oss << L"genreWarn=その他分類が90%以上です。ジャンル読み取り失敗の可能性があります。\n";
        }

        if (diag.total > 0 &&
            diag.emptySource == diag.total) {
            oss << L"genreWarn=全番組のgenreKeyが空です。lite用データ生成前段のジャンル設定を確認してください。\n";
        }

        return oss.str();
    }

    void AppendProgramAttrs_(std::wostringstream& oss, const ProgramEntry& e)
    {
        const std::wstring genreMajor = ExtractGenreMajor_(e.genreKey);


        oss << L" data-date=\"" << HtmlEscape_(e.broadcastDateKey) << L"\"";
        oss << L" data-sid=\"" << static_cast<unsigned int>(e.serviceId) << L"\"";
        oss << L" data-eid=\"" << static_cast<unsigned int>(e.eventId) << L"\"";
        oss << L" data-genre-item=\"1\"";
        oss << L" data-genre-major=\"" << HtmlEscape_(genreMajor) << L"\"";

        // 予約UI用。現段階では localStorage 仮状態。
        oss << L" data-reserve-key=\"" << HtmlEscape_(MakeLiteReserveKey_(e)) << L"\"";
        oss << L" data-reservation-state=\"none\"";
    }

    void AppendTimetableProgramAttrs_(
        std::wostringstream& oss,
        const ProgramEntry& e)
    {
        const std::wstring genreMajor =
            ExtractGenreMajor_(e.genreKey);

        // 時間割は表示専用。
        // data-lite-detail を付けないので詳細ポップは開かない。
        // data-reserve-key も付けないので予約UI対象にもならない。
        oss << L" data-date=\"" << HtmlEscape_(e.broadcastDateKey) << L"\"";
        oss << L" data-sid=\"" << static_cast<unsigned int>(e.serviceId) << L"\"";
        oss << L" data-eid=\"" << static_cast<unsigned int>(e.eventId) << L"\"";
        oss << L" data-genre-item=\"1\"";
        oss << L" data-genre-major=\"" << HtmlEscape_(genreMajor) << L"\"";
    }

    void AppendReserveButton_(
        std::wostringstream& oss,
        const wchar_t* action,
        const wchar_t* label,
        const wchar_t* compactLabel,
        bool compact)
    {
        oss << L"<button type=\"button\" class=\"reserve-btn\" data-reserve-action=\""
            << action
            << L"\" title=\""
            << label
            << L"\">";

        oss << (compact ? compactLabel : label);
        oss << L"</button>";
    }

    void AppendReserveButtons_(
        std::wostringstream& oss,
        bool compact)
    {
        oss << L"<div class=\"reserve-actions\" data-reserve-actions=\"1\">";

        if (compact) {
            AppendReserveButton_(oss, L"reserve", L"視聴", L"視", compact);
            AppendReserveButton_(oss, L"record", L"録画", L"録", compact);
        }
        else {
            AppendReserveButton_(oss, L"reserve", L"視聴", L"視", compact);
            AppendReserveButton_(oss, L"record", L"録画", L"録", compact);
            AppendReserveButton_(oss, L"disabled", L"無効", L"無", compact);
            AppendReserveButton_(oss, L"delete", L"削除", L"消", compact);
        }

        oss << L"</div>";
    }

    struct LiteChannelInfo_
    {
        std::wstring name;
        int space = 0;
        int channel = -1;
        int serviceType = 0;
        int remoteControlKeyId = -1;
        bool isSubchannel = false;
        bool isOneSeg = false;
    };

    using LiteChannelInfoMap_ = std::unordered_map<std::uint32_t, LiteChannelInfo_>;

    std::uint32_t MakeLiteServiceNameKey_(int space, std::uint16_t sid) noexcept
    {
        return (static_cast<std::uint32_t>(space & 0xFFFF) << 16) |
            static_cast<std::uint32_t>(sid);
    }

    std::wstring TrimLiteText_(std::wstring s)
    {
        auto isSpace = [](wchar_t ch) noexcept {
            return ch == L' ' ||
                ch == L'\t' ||
                ch == L'\r' ||
                ch == L'\n' ||
                ch == L'　';
            };

        while (!s.empty() && isSpace(s.front())) {
            s.erase(s.begin());
        }

        while (!s.empty() && isSpace(s.back())) {
            s.pop_back();
        }

        return s;
    }

    bool ReadBinaryFile_(const std::filesystem::path& path, std::string& out)
    {
        out.clear();

        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) {
            return false;
        }

        ifs.seekg(0, std::ios::end);
        const std::streamoff size = ifs.tellg();
        if (size < 0) {
            return false;
        }

        ifs.seekg(0, std::ios::beg);

        out.resize(static_cast<std::size_t>(size));

        if (!out.empty()) {
            ifs.read(out.data(), static_cast<std::streamsize>(out.size()));
        }

        return ifs.good() || ifs.eof();
    }

    bool DecodeUtf8ToWide_(std::string_view bytes, std::wstring& out)
    {
        out.clear();

        std::size_t i = 0;

        if (bytes.size() >= 3 &&
            static_cast<unsigned char>(bytes[0]) == 0xEF &&
            static_cast<unsigned char>(bytes[1]) == 0xBB &&
            static_cast<unsigned char>(bytes[2]) == 0xBF) {
            i = 3;
        }

        while (i < bytes.size()) {
            const unsigned char c0 =
                static_cast<unsigned char>(bytes[i]);

            std::uint32_t cp = 0;
            std::size_t need = 0;

            if (c0 <= 0x7F) {
                cp = c0;
                need = 1;
            }
            else if ((c0 & 0xE0) == 0xC0) {
                cp = c0 & 0x1F;
                need = 2;
            }
            else if ((c0 & 0xF0) == 0xE0) {
                cp = c0 & 0x0F;
                need = 3;
            }
            else if ((c0 & 0xF8) == 0xF0) {
                cp = c0 & 0x07;
                need = 4;
            }
            else {
                return false;
            }

            if (i + need > bytes.size()) {
                return false;
            }

            for (std::size_t k = 1; k < need; ++k) {
                const unsigned char cx =
                    static_cast<unsigned char>(bytes[i + k]);

                if ((cx & 0xC0) != 0x80) {
                    return false;
                }

                cp = (cp << 6) | (cx & 0x3F);
            }

#if WCHAR_MAX <= 0xFFFF
            if (cp <= 0xFFFF) {
                out.push_back(static_cast<wchar_t>(cp));
            }
            else {
                cp -= 0x10000;
                out.push_back(static_cast<wchar_t>(0xD800 + ((cp >> 10) & 0x3FF)));
                out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
            }
#else
            out.push_back(static_cast<wchar_t>(cp));
#endif

            i += need;
        }

        return true;
    }

    bool ReadUtf8TextFile_(const std::filesystem::path& path, std::wstring& out)
    {
        std::string bytes;

        if (!ReadBinaryFile_(path, bytes)) {
            return false;
        }

        return DecodeUtf8ToWide_(bytes, out);
    }

    std::vector<std::wstring> SplitLines_(const std::wstring& text)
    {
        std::vector<std::wstring> lines;
        std::wstring line;

        for (wchar_t ch : text) {
            if (ch == L'\r') {
                continue;
            }

            if (ch == L'\n') {
                lines.push_back(line);
                line.clear();
                continue;
            }

            line.push_back(ch);
        }

        if (!line.empty()) {
            lines.push_back(line);
        }

        return lines;
    }

    std::vector<std::wstring> SplitLiteCsvLine_(const std::wstring& line)
    {
        std::vector<std::wstring> fields;
        std::wstring cur;
        bool quoted = false;

        for (std::size_t i = 0; i < line.size(); ++i) {
            const wchar_t ch = line[i];

            if (ch == L'"') {
                if (quoted &&
                    (i + 1) < line.size() &&
                    line[i + 1] == L'"') {
                    cur.push_back(L'"');
                    ++i;
                }
                else {
                    quoted = !quoted;
                }

                continue;
            }

            if (ch == L',' && !quoted) {
                fields.push_back(TrimLiteText_(cur));
                cur.clear();
                continue;
            }

            cur.push_back(ch);
        }

        fields.push_back(TrimLiteText_(cur));
        return fields;
    }

    bool ParseLiteInt_(const std::wstring& text, int& out) noexcept
    {
        out = 0;

        std::wstring s = TrimLiteText_(text);
        if (s.empty()) {
            return false;
        }

        s.erase(
            std::remove(s.begin(), s.end(), L','),
            s.end());

        wchar_t* endPtr = nullptr;

        const long v =
            std::wcstol(s.c_str(), &endPtr, 10);

        if (endPtr == s.c_str()) {
            return false;
        }

        out = static_cast<int>(v);
        return true;
    }

    bool IsSyntheticServiceName_(const std::wstring& name)
    {
        const std::wstring s =
            TrimLiteText_(name);

        if (s.empty()) {
            return true;
        }

        if (s.rfind(L"SID ", 0) == 0 ||
            s.rfind(L"sid ", 0) == 0 ||
            s.rfind(L"Service ", 0) == 0) {
            return true;
        }

        bool allNumber = true;

        for (wchar_t ch : s) {
            if (ch < L'0' || ch > L'9') {
                allNumber = false;
                break;
            }
        }

        return allNumber;
    }

    std::vector<std::filesystem::path> BuildLiteChannelListCandidates_(
        const EpgHtmlLite::Context& ctx)
    {
        std::vector<std::filesystem::path> out;

        if (!ctx.channelListPath.empty()) {
            out.emplace_back(ctx.channelListPath);
        }

        out.emplace_back(ctx.outputDir / (L"channellist_space" + std::to_wstring(ctx.space) + L".ch"));
        out.emplace_back(ctx.outputDir.parent_path() / (L"channellist_space" + std::to_wstring(ctx.space) + L".ch"));
        out.emplace_back(std::filesystem::current_path() / (L"channellist_space" + std::to_wstring(ctx.space) + L".ch"));

        out.emplace_back(ctx.outputDir / L"channellist_space0.ch");
        out.emplace_back(ctx.outputDir.parent_path() / L"channellist_space0.ch");
        out.emplace_back(std::filesystem::current_path() / L"channellist_space0.ch");

        return out;
    }

    struct LiteChannelCsvRow_
    {
        std::wstring serviceName;
        int space = 0;
        int channel = -1;
        int serviceType = 0;
        std::uint16_t sid = 0;
        int remoteControlKeyId = -1;
    };

    LiteChannelInfoMap_ LoadLiteChannelInfoMap_(const EpgHtmlLite::Context& ctx)
    {
        LiteChannelInfoMap_ map;

        for (const auto& path : BuildLiteChannelListCandidates_(ctx)) {
            std::error_code ec;

            if (path.empty() ||
                !std::filesystem::exists(path, ec) ||
                ec) {
                continue;
            }

            std::wstring text;

            if (!ReadUtf8TextFile_(path, text)) {
                DebugLogFormat(
                    L"[EPG-LITE][SERVICE][WARN] UTF-8 channel list read failed: %ls",
                    path.c_str());
                continue;
            }

            std::vector<LiteChannelCsvRow_> rows;

            for (std::wstring line : SplitLines_(text)) {
                line = TrimLiteText_(std::move(line));

                if (line.empty()) {
                    continue;
                }

                if (line.front() == L'#' ||
                    line.front() == L';') {
                    continue;
                }

                if (line.find(L"ServiceName") != std::wstring::npos &&
                    line.find(L"ServiceID") != std::wstring::npos) {
                    continue;
                }

                const auto fields =
                    SplitLiteCsvLine_(line);

                // ServiceName,Space,Channel,ServiceType,ServiceID,NetworkID,TSID,ONID,FreeCA,Provider,NetworkName,RemoteControlKeyID
                if (fields.size() < 5) {
                    continue;
                }

                LiteChannelCsvRow_ row{};
                row.serviceName = TrimLiteText_(fields[0]);

                if (row.serviceName.empty()) {
                    continue;
                }

                int parsed = 0;

                if (fields.size() > 1 &&
                    ParseLiteInt_(fields[1], parsed)) {
                    row.space = parsed;
                }
                else {
                    row.space = ctx.space;
                }

                if (fields.size() > 2 &&
                    ParseLiteInt_(fields[2], parsed)) {
                    row.channel = parsed;
                }

                if (fields.size() > 3 &&
                    ParseLiteInt_(fields[3], parsed)) {
                    row.serviceType = parsed;
                }

                if (!ParseLiteInt_(fields[4], parsed)) {
                    continue;
                }

                if (parsed <= 0 || parsed > 0xFFFF) {
                    continue;
                }

                row.sid = static_cast<std::uint16_t>(parsed);

                if (fields.size() > 11 &&
                    ParseLiteInt_(fields[11], parsed)) {
                    row.remoteControlKeyId = parsed;
                }

                rows.push_back(std::move(row));
            }

            if (rows.empty()) {
                continue;
            }

            std::map<std::uint64_t, std::vector<LiteChannelCsvRow_>> groups;

            for (const auto& row : rows) {
                const std::uint64_t groupKey =
                    (static_cast<std::uint64_t>(row.space & 0xFFFF) << 32) |
                    static_cast<std::uint32_t>(row.channel);

                groups[groupKey].push_back(row);
            }

            for (auto& pair : groups) {
                auto& group = pair.second;

                std::sort(
                    group.begin(),
                    group.end(),
                    [](const LiteChannelCsvRow_& a, const LiteChannelCsvRow_& b) {
                        const bool aOneSeg = (a.serviceType == 192);
                        const bool bOneSeg = (b.serviceType == 192);

                        if (aOneSeg != bOneSeg) {
                            return !aOneSeg && bOneSeg;
                        }

                        if (a.sid != b.sid) {
                            return a.sid < b.sid;
                        }

                        return a.serviceName < b.serviceName;
                    });

                std::size_t mainIndex = 0;

                for (std::size_t i = 0; i < group.size(); ++i) {
                    if (group[i].serviceType != 192) {
                        mainIndex = i;
                        break;
                    }
                }

                for (std::size_t i = 0; i < group.size(); ++i) {
                    const auto& row = group[i];

                    LiteChannelInfo_ info{};
                    info.name = row.serviceName;
                    info.space = row.space;
                    info.channel = row.channel;
                    info.serviceType = row.serviceType;
                    info.remoteControlKeyId = row.remoteControlKeyId;
                    info.isOneSeg = (row.serviceType == 192);
                    info.isSubchannel = info.isOneSeg || (i != mainIndex);

                    map[MakeLiteServiceNameKey_(row.space, row.sid)] =
                        std::move(info);
                }
            }

            if (!map.empty()) {
                DebugLogFormat(
                    L"[EPG-LITE][SERVICE] loaded channel info: path=%ls count=%zu",
                    path.c_str(),
                    map.size());
                break;
            }
        }

        if (map.empty()) {
            DebugLogFormat(
                L"[EPG-LITE][SERVICE] channel info map is empty. channelListPath=%ls",
                ctx.channelListPath.c_str());
        }

        return map;
    }

    const LiteChannelInfo_* FindLiteChannelInfo_(
        int space,
        std::uint16_t sid,
        const LiteChannelInfoMap_& channelInfoMap) noexcept
    {
        const auto it =
            channelInfoMap.find(MakeLiteServiceNameKey_(space, sid));

        if (it == channelInfoMap.end()) {
            return nullptr;
        }

        return &it->second;
    }

    std::wstring ResolveLiteServiceName_(
        const ProgramEntry& e,
        const LiteChannelInfoMap_& channelInfoMap)
    {
        if (const LiteChannelInfo_* info =
            FindLiteChannelInfo_(e.space, e.serviceId, channelInfoMap)) {
            if (!info->name.empty()) {
                return info->name;
            }
        }

        if (!IsSyntheticServiceName_(e.serviceName)) {
            return e.serviceName;
        }

        return L"SID " + std::to_wstring(static_cast<unsigned int>(e.serviceId));
    }

    bool IsLiteSubchannel_(
        const ProgramEntry& e,
        const LiteChannelInfoMap_& channelInfoMap) noexcept
    {
        if (const LiteChannelInfo_* info =
            FindLiteChannelInfo_(e.space, e.serviceId, channelInfoMap)) {
            return info->isSubchannel;
        }

        return false;
    }

    std::wstring ServiceDisplayName_(
        std::uint16_t sid,
        const std::vector<const ProgramEntry*>& rows,
        const LiteChannelInfoMap_& channelInfoMap,
        int fallbackSpace)
    {
        for (const ProgramEntry* p : rows) {
            if (!p) {
                continue;
            }

            const std::wstring resolved =
                ResolveLiteServiceName_(*p, channelInfoMap);

            if (!IsSyntheticServiceName_(resolved)) {
                return resolved;
            }
        }

        if (const LiteChannelInfo_* info =
            FindLiteChannelInfo_(fallbackSpace, sid, channelInfoMap)) {
            if (!info->name.empty()) {
                return info->name;
            }
        }

        return L"SID " + std::to_wstring(static_cast<unsigned int>(sid));
    }



    std::wstring ServiceDisplayName_(std::uint16_t sid, const std::vector<const ProgramEntry*>& rows)
    {
        for (const ProgramEntry* p : rows) {
            if (p && !p->serviceName.empty()) {
                return p->serviceName;
            }
        }
        return L"SID " + std::to_wstring(static_cast<unsigned int>(sid));
    }

    bool TryParseLiteClockText_(
        const std::wstring& text,
        int& outBroadcastMinute) noexcept
    {
        outBroadcastMinute = -1;

        if (text.empty()) {
            return false;
        }

        auto isDigit = [](wchar_t ch) noexcept {
            return ch >= L'0' && ch <= L'9';
            };

        std::size_t pos = 0;

        while (pos < text.size() && !isDigit(text[pos])) {
            ++pos;
        }

        if (pos >= text.size()) {
            return false;
        }

        int hour = 0;

        while (pos < text.size() && isDigit(text[pos])) {
            hour = hour * 10 + static_cast<int>(text[pos] - L'0');
            ++pos;
        }

        while (pos < text.size() &&
            (text[pos] == L' ' ||
                text[pos] == L'\t' ||
                text[pos] == L'　')) {
            ++pos;
        }

        if (pos >= text.size() ||
            (text[pos] != L':' && text[pos] != L'：')) {
            return false;
        }

        ++pos;

        int minute = 0;
        int minuteDigits = 0;

        while (pos < text.size() && isDigit(text[pos]) && minuteDigits < 2) {
            minute = minute * 10 + static_cast<int>(text[pos] - L'0');
            ++pos;
            ++minuteDigits;
        }

        if (minuteDigits == 0) {
            return false;
        }

        if (hour < 0 || hour > 47 || minute < 0 || minute > 59) {
            return false;
        }

        const int rawMinute =
            hour * 60 + minute;

        if (hour >= 24) {
            outBroadcastMinute = rawMinute;
        }
        else {
            outBroadcastMinute =
                NormalizeToLiteBroadcastMinute_(rawMinute);
        }

        return outBroadcastMinute >= 0;
    }

    int PickLiteStartBroadcastMinuteForTable_(
        const ProgramEntry& e) noexcept
    {
        if (e.startMinutesOfDay >= 0) {
            return NormalizeToLiteBroadcastMinute_(e.startMinutesOfDay);
        }

        int parsed = -1;

        if (TryParseLiteClockText_(e.startText, parsed)) {
            return parsed;
        }

        if (TryParseLiteClockText_(e.startTime, parsed)) {
            return parsed;
        }

        return -1;
    }

    int PickLiteEndBroadcastMinuteForTable_(
        const ProgramEntry& e,
        const LiteEndFallbackMap_& fallbackMap) noexcept
    {
        // table表示では、微小ギャップ補正・終了時刻欠落補正を先に使う。
        // 表示テキスト側の TimeRangeText_ は元の時刻を優先するので、
        // ここで補正しても番組時刻表示までは勝手に書き換えない。
        const int mappedEnd =
            FindLiteFallbackEndMinute_(fallbackMap, e);

        if (mappedEnd >= 0) {
            return mappedEnd;
        }

        if (e.endMinutesOfDay >= 0) {
            return NormalizeToLiteBroadcastMinute_(e.endMinutesOfDay);
        }

        int parsed = -1;

        if (TryParseLiteClockText_(e.endText, parsed)) {
            return parsed;
        }

        if (TryParseLiteClockText_(e.endTime, parsed)) {
            return parsed;
        }

        return -1;
    }

    bool GetProgramLiteMinuteRangeForTable_(
        const ProgramEntry& e,
        const LiteEndFallbackMap_& fallbackMap,
        int& outBegin,
        int& outEnd) noexcept
    {
        outBegin =
            PickLiteStartBroadcastMinuteForTable_(e);

        if (outBegin < 0) {
            outEnd = -1;
            return false;
        }

        outEnd =
            PickLiteEndBroadcastMinuteForTable_(e, fallbackMap);

        if (outEnd < 0) {
            outEnd = outBegin + 1;
            return true;
        }

        if (outEnd <= outBegin) {
            outEnd += kLiteMinutesPerDay_;
        }

        if (outEnd <= outBegin) {
            outEnd = outBegin + 1;
        }

        return true;
    }

    bool GetClippedLiteMinuteRangeForTable_(
        const ProgramEntry& e,
        const LiteBlockMinuteRange_& range,
        const LiteEndFallbackMap_& fallbackMap,
        int& outBeginOffset,
        int& outEndOffset) noexcept
    {
        outBeginOffset = 0;
        outEndOffset = 0;

        if (range.begin < 0 || range.end <= range.begin) {
            return false;
        }

        int begin = -1;
        int end = -1;

        if (!GetProgramLiteMinuteRangeForTable_(
            e,
            fallbackMap,
            begin,
            end)) {
            return false;
        }

        const int clippedBegin =
            (std::max)(begin, range.begin);

        const int clippedEnd =
            (std::min)(end, range.end);

        if (clippedEnd <= clippedBegin) {
            return false;
        }

        outBeginOffset = clippedBegin - range.begin;
        outEndOffset = clippedEnd - range.begin;

        return outEndOffset > outBeginOffset;
    }

    struct LiteTableService_
    {
        std::uint16_t sid = 0;
        std::wstring name;
        bool isSubchannel = false;
        std::vector<const ProgramEntry*> programs;

        std::vector<const ProgramEntry*> startAt;
        std::vector<int> rowspanAt;

        // 番組セルの rowspan に吸収される分。
        std::vector<bool> covered;

        // 番組が無い空白部分も rowspan でまとめる。
        // これをしないと1分ごとの空tdが大量に出て、番組欄が穴だらけに見える。
        std::vector<int> gapRowspanAt;
        std::vector<bool> gapCovered;
    };

    void BuildLiteTableServiceCells_(
        LiteTableService_& service,
        const LiteBlockMinuteRange_& range,
        const LiteEndFallbackMap_& fallbackMap)
    {
        const int blockMinutes =
            (std::max)(1, range.end - range.begin);

        service.startAt.assign(
            static_cast<std::size_t>(blockMinutes),
            nullptr);

        service.rowspanAt.assign(
            static_cast<std::size_t>(blockMinutes),
            1);

        service.covered.assign(
            static_cast<std::size_t>(blockMinutes),
            false);

        service.gapRowspanAt.assign(
            static_cast<std::size_t>(blockMinutes),
            0);

        service.gapCovered.assign(
            static_cast<std::size_t>(blockMinutes),
            false);

        for (const ProgramEntry* p : service.programs) {
            if (!p) {
                continue;
            }

            int beginOffset = 0;
            int endOffset = 0;

            if (!GetClippedLiteMinuteRangeForTable_(
                *p,
                range,
                fallbackMap,
                beginOffset,
                endOffset)) {
                continue;
            }

            if (beginOffset < 0) {
                beginOffset = 0;
            }

            if (endOffset <= beginOffset) {
                endOffset = beginOffset + 1;
            }

            if (endOffset > blockMinutes) {
                endOffset = blockMinutes;
            }

            int cellBegin = beginOffset;

            while (cellBegin < endOffset &&
                service.covered[static_cast<std::size_t>(cellBegin)]) {
                ++cellBegin;
            }

            if (cellBegin >= endOffset) {
                continue;
            }

            int cellEnd = cellBegin;

            while (cellEnd < endOffset &&
                !service.covered[static_cast<std::size_t>(cellEnd)]) {
                ++cellEnd;
            }

            if (cellEnd <= cellBegin) {
                cellEnd = cellBegin + 1;
            }

            service.startAt[static_cast<std::size_t>(cellBegin)] = p;
            service.rowspanAt[static_cast<std::size_t>(cellBegin)] =
                (std::max)(1, cellEnd - cellBegin);

            for (int i = cellBegin; i < cellEnd; ++i) {
                service.covered[static_cast<std::size_t>(i)] = true;
            }
        }

        // 番組が存在しない連続空白も rowspan 化する。
        // 1分ごとの空セルを出さないので、見た目とDOM量の両方が改善する。
        int pos = 0;

        while (pos < blockMinutes) {
            if (service.covered[static_cast<std::size_t>(pos)]) {
                ++pos;
                continue;
            }

            const int gapBegin = pos;

            while (pos < blockMinutes &&
                !service.covered[static_cast<std::size_t>(pos)]) {
                ++pos;
            }

            const int gapEnd = pos;

            if (gapEnd > gapBegin) {
                service.gapRowspanAt[static_cast<std::size_t>(gapBegin)] =
                    gapEnd - gapBegin;

                for (int i = gapBegin; i < gapEnd; ++i) {
                    service.gapCovered[static_cast<std::size_t>(i)] = true;
                }
            }
        }
    }

    bool WriteNowServicePage_(
        const std::filesystem::path& path,
        const std::wstring& title,
        const std::wstring& dateKey,
        std::uint16_t serviceId,
        int block,
        const std::vector<const ProgramEntry*>& rows,
        const LiteChannelInfoMap_& channelInfoMap)
    {
        const bool blockPage = block >= 0;
        const std::wstring pageTitle = title.empty() ? L"EPG Lite" : title;

        const int fallbackSpace =
            rows.empty() || !rows.front() ? 0 : rows.front()->space;

        const std::wstring serviceName =
            ServiceDisplayName_(serviceId, rows, channelInfoMap, fallbackSpace);

        const LiteEndFallbackMap_ endFallbackMap =
            BuildLiteEndFallbackMap_(rows, block);

        const std::wstring litePrefix = blockPage ? L"../../../../" : L"../../../";
        const std::wstring detailBase = blockPage ? L"../../../detail/" : L"../../detail/";
        const std::wstring serverHref = blockPage ? L"../../../../../index.html" : L"../../../../index.html";

        std::wostringstream oss;

        oss << BuildHeader_(
            pageTitle,
            DateLabel_(dateKey) + L" / " + serviceName + (blockPage ? (L" / " + EpgHtmlLite::GetLiteBlockLabel(block)) : L""),
            litePrefix,
            detailBase);

        oss << BuildPageNav_(serverHref);

        if (rows.empty()) {
            oss << L"<div class=\"empty\">対象番組がありません。</div>\n";
        }
        else {
            oss << L"<div class=\"program-list\">\n";

            for (const ProgramEntry* p : rows) {
                if (!p) {
                    continue;
                }

                const ProgramEntry& e = *p;
                const std::wstring summary =
                    ClipSummary_(e.description, blockPage ? 72 : 100);

                oss << L"<article class=\"p\"";
                AppendProgramAttrs_(oss, e);
                oss << L">";

                oss << L"<b>"
                    << HtmlEscape_(ClipTitle_(e.title, blockPage ? 64 : 90))
                    << L"</b>";

                oss << L"<small class=\"meta\">"
                    << HtmlEscape_(TimeRangeText_(e, endFallbackMap));

                if (blockPage) {
                    const std::wstring overlapState =
                        GetBlockOverlapStateText_(e, block);

                    if (!overlapState.empty()) {
                        oss << L" / "
                            << HtmlEscape_(overlapState);
                    }
                }

                const std::wstring genreMajor =
                    ExtractGenreMajor_(e.genreKey);

                if (!genreMajor.empty()) {
                    oss << L" / "
                        << HtmlEscape_(genreMajor);
                }

                oss << L"</small>";

                if (!summary.empty()) {
                    oss << L"<small class=\"sum\">"
                        << HtmlEscape_(summary)
                        << L"</small>";
                }

                oss << L"</article>\n";
            }

            oss << L"</div>\n";
        }

        oss << BuildFooter_();
        return WriteUtf8File_(path, oss.str());
    }

    bool WriteNowDateTimetableBlockPage_(
        const std::filesystem::path& path,
        const std::wstring& title,
        const std::wstring& dateKey,
        int block,
        const std::vector<const ProgramEntry*>& rows,
        const LiteChannelInfoMap_& channelInfoMap)
    {
        const std::wstring pageTitle =
            title.empty() ? L"EPG Lite" : title;

        const LiteBlockMinuteRange_ blockRange =
            GetLiteBlockMinuteRange_(block);

        if (blockRange.begin < 0 ||
            blockRange.end <= blockRange.begin) {
            return false;
        }

        const int blockMinutes =
            blockRange.end - blockRange.begin;

        const LiteEndFallbackMap_ endFallbackMap =
            BuildLiteEndFallbackMap_(rows, block);

        std::map<std::uint16_t, std::vector<const ProgramEntry*>> serviceMap;

        for (const ProgramEntry* p : rows) {
            if (!p) {
                continue;
            }

            serviceMap[p->serviceId].push_back(p);
        }

        for (auto& pair : serviceMap) {
            auto& list = pair.second;

            std::sort(
                list.begin(),
                list.end(),
                [](const ProgramEntry* a, const ProgramEntry* b) {
                    const int aStart =
                        PickLiteStartBroadcastMinuteForTable_(*a);

                    const int bStart =
                        PickLiteStartBroadcastMinuteForTable_(*b);

                    if (aStart != bStart) {
                        return aStart < bStart;
                    }

                    if (a->endMinutesOfDay != b->endMinutesOfDay) {
                        return a->endMinutesOfDay < b->endMinutesOfDay;
                    }

                    if (a->sortOrder != b->sortOrder) {
                        return a->sortOrder < b->sortOrder;
                    }

                    if (a->title != b->title) {
                        return a->title < b->title;
                    }

                    return a->eventId < b->eventId;
                });
        }

        std::vector<LiteTableService_> services;
        services.reserve(serviceMap.size());

        for (const auto& pair : serviceMap) {
            LiteTableService_ service{};

            service.sid = pair.first;
            service.programs = pair.second;

            const int fallbackSpace =
                service.programs.empty() || !service.programs.front()
                ? 0
                : service.programs.front()->space;

            service.name =
                ServiceDisplayName_(
                    service.sid,
                    service.programs,
                    channelInfoMap,
                    fallbackSpace);

            if (!service.programs.empty() && service.programs.front()) {
                service.isSubchannel =
                    IsLiteSubchannel_(*service.programs.front(), channelInfoMap);
            }

            BuildLiteTableServiceCells_(
                service,
                blockRange,
                endFallbackMap);

            services.push_back(std::move(service));
        }

        std::wostringstream oss;

        oss << BuildHeader_(
            pageTitle,
            DateLabel_(dateKey) + L" / 時間割 / " + EpgHtmlLite::GetLiteBlockLabel(block),
            L"../../../",
            L"../../detail/");

        oss << BuildPageNav_(L"../../../../index.html");
        oss << BuildGenreToolbar_(L"genreScope");
        oss << BuildSubchannelToolbar_(L"genreScope");

        if (services.empty()) {
            oss << L"<div class=\"empty\">対象番組がありません。</div>\n";
        }
        else {
            oss << L"<div id=\"genreScope\" class=\"lite-table-wrap\">\n";
            oss << L"<table id=\"liteScrollBody\" class=\"lite-epg-table\">\n";

            oss << L"<thead>\n";
            oss << L"<tr>";
            oss << L"<th class=\"time-col\">時間</th>";

            for (const LiteTableService_& service : services) {
                oss << L"<th class=\"svc-col\" data-lite-service-col=\"1\" data-subchannel=\""
                    << (service.isSubchannel ? L"1" : L"0")
                    << L"\" title=\""
                    << HtmlEscape_(service.name)
                    << L"\">"
                    << HtmlEscape_(service.name)
                    << L"</th>";
            }

            oss << L"</tr>\n";
            oss << L"</thead>\n";

            oss << L"<tbody>\n";

            for (int minuteOffset = 0; minuteOffset < blockMinutes; ++minuteOffset) {
                const int normalizedMinute =
                    blockRange.begin + minuteOffset;

                const bool hourRow =
                    (minuteOffset % 60) == 0;

                const bool tickRow =
                    !hourRow && (minuteOffset % 5) == 0;

                oss << L"<tr class=\"lite-minute-row";

                if (hourRow) {
                    oss << L" hour-row";
                }
                else if (tickRow) {
                    oss << L" tick-row";
                }

                oss << L"\">";

                oss << L"<th class=\"time-col\">";

                if (hourRow) {
                    oss << HtmlEscape_(FormatMinuteOfDayText_(normalizedMinute));
                }
                else if (tickRow) {
                    oss << HtmlEscape_(FormatMinuteOfDayText_(normalizedMinute));
                }

                oss << L"</th>";

                for (const LiteTableService_& service : services) {
                    const std::size_t index =
                        static_cast<std::size_t>(minuteOffset);

                    if (index >= service.covered.size()) {
                        oss << L"<td class=\"empty-cell\"></td>";
                        continue;
                    }

                    const ProgramEntry* startProgram =
                        service.startAt[index];

                    if (startProgram) {
                        const ProgramEntry& e = *startProgram;

                        const int rowspan =
                            index < service.rowspanAt.size()
                            ? service.rowspanAt[index]
                            : 1;

                        const bool compactCell =
                            rowspan <= kLiteTimelineCompactMaxMinutes_;

                        oss << L"<td class=\"prog-cell"
                            << (compactCell ? L" compact-cell" : L"")
                            << L"\"";

                        if (rowspan > 1) {
                            oss << L" rowspan=\""
                                << rowspan
                                << L"\"";
                        }

                        oss << L" data-subchannel-item=\"1\"";
                        oss << L" data-subchannel=\""
                            << (service.isSubchannel ? L"1" : L"0")
                            << L"\"";

                        AppendTimetableProgramAttrs_(oss, e);

                        oss << L">";

                        oss << L"<div class=\"program-time\">";
                        oss << HtmlEscape_(TimeRangeText_(e, endFallbackMap));

                        const std::wstring overlapState =
                            GetBlockOverlapStateText_(e, block);

                        if (!overlapState.empty()) {
                            oss << L" / "
                                << HtmlEscape_(overlapState);
                        }

                        oss << L"</div>";

                        oss << L"<div class=\"program-title\">";
                        oss << HtmlEscape_(e.title);
                        oss << L"</div>";

                        // タイトル全文を最優先。
                        // 概要は20分以上の枠だけ軽く表示する。
                        if (rowspan >= 20 && !e.description.empty()) {
                            const std::wstring summary =
                                ClipSummary_(
                                    e.description,
                                    rowspan >= 60 ? 220 : 120);

                            if (!summary.empty()) {
                                oss << L"<div class=\"program-sub summary\">"
                                    << HtmlEscape_(summary)
                                    << L"</div>";
                            }
                        }

                        const std::wstring genreMajor =
                            ExtractGenreMajor_(e.genreKey);

                        if (!genreMajor.empty()) {
                            oss << L"<div class=\"program-sub genre\">"
                                << HtmlEscape_(genreMajor)
                                << L"</div>";
                        }

                        oss << L"</td>";
                    }
                    else {
                        const int gapRowspan =
                            index < service.gapRowspanAt.size()
                            ? service.gapRowspanAt[index]
                            : 0;

                        if (gapRowspan > 0) {
                            oss << L"<td class=\"empty-cell gap-cell\"";

                            if (gapRowspan > 1) {
                                oss << L" rowspan=\""
                                    << gapRowspan
                                    << L"\"";
                            }

                            oss << L" data-subchannel-item=\"1\" data-subchannel=\""
                                << (service.isSubchannel ? L"1" : L"0")
                                << L"\"></td>";
                        }
                        else {
                            const bool programCovered =
                                index < service.covered.size() &&
                                service.covered[index];

                            const bool gapCovered =
                                index < service.gapCovered.size() &&
                                service.gapCovered[index];

                            if (programCovered || gapCovered) {
                                // rowspanで上のセルに吸収されているので、この列は何も出さない。
                            }
                            else {
                                oss << L"<td class=\"empty-cell\" data-subchannel-item=\"1\" data-subchannel=\""
                                    << (service.isSubchannel ? L"1" : L"0")
                                    << L"\"></td>";
                            }
                        }
                    }
                }

                oss << L"</tr>\n";
            }

            oss << L"</tbody>\n";
            oss << L"</table>\n";
            oss << L"</div>\n";
        }

        oss << BuildFooter_();
        return WriteUtf8File_(path, oss.str());
    }

    bool WriteNowDateListBlockPage_(
        const std::filesystem::path& path,
        const std::wstring& title,
        const std::wstring& dateKey,
        int block,
        const std::vector<const ProgramEntry*>& rows,
        const LiteChannelInfoMap_& channelInfoMap)
    {
        const std::wstring pageTitle =
            title.empty() ? L"EPG Lite" : title;

        const LiteEndFallbackMap_ endFallbackMap =
            BuildLiteEndFallbackMap_(rows, block);

        std::wostringstream oss;

        oss << BuildHeader_(
            pageTitle,
            DateLabel_(dateKey) + L" / 表一覧 / " + EpgHtmlLite::GetLiteBlockLabel(block),
            L"../../../",
            L"../../detail/");

        oss << BuildPageNav_(L"../../../../index.html");
        oss << BuildGenreToolbar_(L"genreScope");
        oss << BuildSubchannelToolbar_(L"genreScope");

        if (rows.empty()) {
            oss << L"<div class=\"empty\">対象番組がありません。</div>\n";
        }
        else {
            oss << L"<div id=\"genreScope\" class=\"lite-list-wrap\">\n";
            oss << L"<table class=\"archive-table\">\n";
            oss << L"<thead><tr>";
            oss << L"<th class=\"c-time\">時刻</th>";
            oss << L"<th class=\"c-svc\">放送局</th>";
            oss << L"<th class=\"c-title\">番組</th>";
            oss << L"</tr></thead>\n<tbody>\n";

            for (const ProgramEntry* p : rows) {
                if (!p) {
                    continue;
                }

                const ProgramEntry& e = *p;
                const bool isSubchannel =
                    IsLiteSubchannel_(e, channelInfoMap);

                oss << L"<tr class=\"p\"";
                AppendProgramAttrs_(oss, e);
                oss << L" data-subchannel-item=\"1\"";
                oss << L" data-subchannel=\""
                    << (isSubchannel ? L"1" : L"0")
                    << L"\"";
                oss << L">";

                oss << L"<td>";
                oss << HtmlEscape_(TimeRangeText_(e, endFallbackMap));

                const std::wstring overlapState =
                    GetBlockOverlapStateText_(e, block);

                if (!overlapState.empty()) {
                    oss << L"<br><small class=\"meta\">"
                        << HtmlEscape_(overlapState)
                        << L"</small>";
                }

                oss << L"</td>";

                oss << L"<td>"
                    << HtmlEscape_(ResolveLiteServiceName_(e, channelInfoMap))
                    << L"</td>";

                oss << L"<td>";
                oss << L"<b>" << HtmlEscape_(ClipTitle_(e.title, 96)) << L"</b>";

                const std::wstring summary =
                    ClipSummary_(e.description, 90);

                if (!summary.empty()) {
                    oss << L"<small class=\"sum\">"
                        << HtmlEscape_(summary)
                        << L"</small>";
                }

                AppendReserveButtons_(oss, false);

                const std::wstring genreMajor =
                    ExtractGenreMajor_(e.genreKey);

                if (!genreMajor.empty()) {
                    oss << L"<small class=\"genre\">"
                        << HtmlEscape_(genreMajor)
                        << L"</small>";
                }

                oss << L"</td>";
                oss << L"</tr>\n";
            }

            oss << L"</tbody>\n</table>\n";
            oss << L"</div>\n";
        }

        oss << BuildFooter_();
        return WriteUtf8File_(path, oss.str());
    }

    bool WriteBeforeArchiveIndex_(
        const std::filesystem::path& path,
        const std::wstring& title,
        const std::vector<std::wstring>& beforeDates)
    {
        std::wostringstream oss;

        oss << BuildHeader_(title, L"過去アーカイブ / 日付選択", L"../../", L"../detail/");
        oss << BuildPageNav_(L"../../../index.html");

        if (beforeDates.empty()) {
            oss << L"<div class=\"empty\">過去アーカイブはありません。</div>\n";
        }
        else {
            oss << L"<div class=\"svc-list\">\n";

            for (const auto& d : beforeDates) {
                oss << L"<a class=\"svc-link\" href=\"" << HtmlEscape_(d + L".html") << L"\">";
                oss << HtmlEscape_(DateLabel_(d));
                oss << L"</a>\n";
            }

            oss << L"</div>\n";
        }

        oss << BuildFooter_();
        return WriteUtf8File_(path, oss.str());
    }

    bool WriteBeforeArchiveDatePage_(
        const std::filesystem::path& path,
        const std::wstring& title,
        const std::wstring& dateKey,
        const std::vector<const ProgramEntry*>& rows,
        const LiteChannelInfoMap_& channelInfoMap)
    {
        std::wostringstream oss;

        oss << BuildHeader_(
            title,
            DateLabel_(dateKey) + L" / 過去アーカイブ",
            L"../../",
            L"../detail/");

        oss << BuildPageNav_(L"../../../index.html");

        if (rows.empty()) {
            oss << L"<div class=\"empty\">過去番組がありません。</div>\n";
        }
        else {
            oss << L"<table class=\"archive-table\">\n";
            oss << L"<thead><tr>";
            oss << L"<th class=\"c-time\">時刻</th>";
            oss << L"<th class=\"c-svc\">放送局</th>";
            oss << L"<th class=\"c-title\">番組</th>";
            oss << L"</tr></thead>\n<tbody>\n";

            for (const ProgramEntry* p : rows) {
                if (!p) {
                    continue;
                }

                const ProgramEntry& e = *p;

                oss << L"<tr class=\"p\"";
                AppendProgramAttrs_(oss, e);
                oss << L">";

                oss << L"<td>" << HtmlEscape_(TimeRangeText_(e)) << L"</td>";
                oss << L"<td>" << HtmlEscape_(ResolveLiteServiceName_(e, channelInfoMap)) << L"</td>";
                oss << L"<td><b>" << HtmlEscape_(ClipTitle_(e.title, 100)) << L"</b></td>";

                oss << L"</tr>\n";
            }

            oss << L"</tbody>\n</table>\n";
        }

        oss << BuildFooter_();
        return WriteUtf8File_(path, oss.str());
    }



    struct ServiceManifest_
    {
        std::uint16_t sid = 0;
        std::wstring name;
        std::wstring listHref;
        std::array<std::wstring, kBlockCount> blockHref{};

    };

    struct NowDayManifest_
    {
        std::wstring key;
        std::wstring label;

        // 日付単位ページ。
        // lite/space0/now/20260706/list_b04.html
        // lite/space0/now/20260706/timetable_b04.html
        std::array<std::wstring, kBlockCount> listBlockHref{};
        std::array<std::wstring, kBlockCount> timetableBlockHref{};

        // 既存互換。サービス別ページは当面残す。
        std::vector<ServiceManifest_> services;
    };

    bool WriteManifest_(
        const std::filesystem::path& path,
        int space,
        const std::vector<NowDayManifest_>& nowDays,
        const std::vector<std::wstring>& beforeDates)
    {
        std::wostringstream oss;

        oss << L"window.EPGLITE_MANIFEST={spaces:[{";
        oss << L"space:" << space << L",";
        oss << L"label:\"" << JsEscape_(SpaceLabel_(space)) << L"\",";
        oss << L"beforeIndex:\"lite/" << JsEscape_(SpaceDirName_(space)) << L"/before/index.html\",";
        oss << L"now:[";

        for (std::size_t i = 0; i < nowDays.size(); ++i) {
            if (i) {
                oss << L",";
            }

            const NowDayManifest_& d = nowDays[i];

            oss << L"{key:\"" << JsEscape_(d.key) << L"\",";
            oss << L"label:\"" << JsEscape_(d.label) << L"\",";

            oss << L"listBlocks:{";
            bool wroteListBlock = false;
            for (int b = 0; b < kBlockCount; ++b) {
                const std::wstring& href = d.listBlockHref[static_cast<std::size_t>(b)];
                if (href.empty()) {
                    continue;
                }
                if (wroteListBlock) {
                    oss << L",";
                }
                wroteListBlock = true;
                oss << BlockSuffix_(b) << L":\"" << JsEscape_(href) << L"\"";
            }
            oss << L"},";

            oss << L"timetableBlocks:{";
            bool wroteTimetableBlock = false;
            for (int b = 0; b < kBlockCount; ++b) {
                const std::wstring& href = d.timetableBlockHref[static_cast<std::size_t>(b)];
                if (href.empty()) {
                    continue;
                }
                if (wroteTimetableBlock) {
                    oss << L",";
                }
                wroteTimetableBlock = true;
                oss << BlockSuffix_(b) << L":\"" << JsEscape_(href) << L"\"";
            }
            oss << L"},";

            oss << L"services:[";

            for (std::size_t j = 0; j < d.services.size(); ++j) {
                if (j) {
                    oss << L",";
                }

                const ServiceManifest_& s = d.services[j];

                oss << L"{sid:" << static_cast<unsigned int>(s.sid) << L",";
                oss << L"name:\"" << JsEscape_(s.name) << L"\",";
                oss << L"list:\"" << JsEscape_(s.listHref) << L"\",";

                oss << L"blocks:{";

                bool wroteBlock = false;
                for (int b = 0; b < kBlockCount; ++b) {
                    const std::wstring& href = s.blockHref[static_cast<std::size_t>(b)];
                    if (href.empty()) {
                        continue;
                    }
                    if (wroteBlock) {
                        oss << L",";
                    }
                    wroteBlock = true;
                    oss << BlockSuffix_(b) << L":\"" << JsEscape_(href) << L"\"";
                }

                oss << L"}}";
            }

            oss << L"]}";
        }

        oss << L"],before:[";

        for (std::size_t i = 0; i < beforeDates.size(); ++i) {
            if (i) {
                oss << L",";
            }

            oss << L"{key:\"" << JsEscape_(beforeDates[i]) << L"\",";
            oss << L"label:\"" << JsEscape_(DateLabel_(beforeDates[i])) << L"\",";
            oss << L"path:\"lite/" << JsEscape_(SpaceDirName_(space)) << L"/before/" << JsEscape_(beforeDates[i]) << L".html\"}";
        }

        oss << L"]}]};\n";

        return WriteUtf8File_(path, oss.str());
    }

    bool WriteServerRootIndex_(
        const std::filesystem::path& path,
        const std::wstring& title,
        int spaceId)
    {
        std::wostringstream oss;

        const std::wstring spaceSuffix =
            L"_space" + std::to_wstring(spaceId);

        const std::wstring reservationListFile =
            L"epg_lite_reservation_list" + spaceSuffix + L".html";

        const std::wstring keywordSearchFile =
            L"epg_lite_keyword_search" + spaceSuffix + L".html";

        const std::wstring autoKeywordReservationListFile =
            L"epg_lite_auto_keyword_reservation_list" + spaceSuffix + L".html";

        const std::wstring logFile =
            L"epg_lite_log" + spaceSuffix + L".html";

        oss << L"<!doctype html>\n";
        oss << L"<html lang=\"ja\">\n";
        oss << L"<head>\n";
        oss << L"<meta charset=\"utf-8\">\n";
        oss << L"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n";
        oss << L"<title>" << HtmlEscape_(title.empty() ? L"EPG Lite" : title) << L"</title>\n";
        oss << L"<link rel=\"stylesheet\" href=\"lite/epg_lite_split.css\">\n";
        oss << L"<script src=\"lite/manifest.js\"></script>\n";
        oss << L"</head>\n";
        oss << L"<body class=\"root-page\">\n";

        oss << L"<div class=\"root-top\">\n";

        oss << L"<div class=\"root-toolbar root-page-toolbar\">\n";
        oss << L"<div class=\"root-row root-page-row\" id=\"pageButtons\"></div>\n";
        oss << L"</div>\n";

        oss << L"<div class=\"root-toolbar root-guide-toolbar\" id=\"guideToolbarA\">\n";
        oss << L"<div class=\"root-row root-sub-row\" id=\"modeButtons\"></div>\n";
        oss << L"<div class=\"root-row root-main-row\" id=\"dayButtons\"></div>\n";
        oss << L"</div>\n";

        oss << L"<div class=\"root-toolbar root-guide-toolbar\" id=\"guideToolbarB\">\n";
        oss << L"<div class=\"root-row root-sub-row\" id=\"viewButtons\"></div>\n";
        oss << L"<div class=\"root-row root-main-row\" id=\"blockButtons\"></div>\n";
        oss << L"<div class=\"status root-status\" id=\"statusText\">EPG Lite</div>\n";
        oss << L"</div>\n";

        oss << L"</div>\n";

        oss << L"<iframe id=\"epgFrame\" title=\"EPG Lite\"></iframe>\n";

        oss << L"<script>\n";
        oss << L"(function(){\n";
        oss << L"'use strict';\n";

        oss << L"var manifest=window.EPGLITE_MANIFEST||{};\n";
        oss << L"var space=(manifest.spaces&&manifest.spaces[0])||null;\n";
        oss << L"var days=space?(space.now||[]):[];\n";
        oss << L"var activePage='program';\n";
        oss << L"var mode='now';\n";
        oss << L"var curDay=0;\n";
        oss << L"var curView='list';\n";
        oss << L"var curBlock='b04';\n";

        oss << L"var pageBox=document.getElementById('pageButtons');\n";
        oss << L"var guideToolbarA=document.getElementById('guideToolbarA');\n";
        oss << L"var guideToolbarB=document.getElementById('guideToolbarB');\n";
        oss << L"var modeBox=document.getElementById('modeButtons');\n";
        oss << L"var dayBox=document.getElementById('dayButtons');\n";
        oss << L"var viewBox=document.getElementById('viewButtons');\n";
        oss << L"var blockBox=document.getElementById('blockButtons');\n";
        oss << L"var frame=document.getElementById('epgFrame');\n";
        oss << L"var status=document.getElementById('statusText');\n";

        oss << L"function clear(node){while(node.firstChild){node.removeChild(node.firstChild);}}\n";
        oss << L"function button(text,active,fn){var b=document.createElement('button');b.type='button';b.textContent=text;if(active){b.className='active';}b.onclick=fn;return b;}\n";

        oss << L"var pageList=[\n";
        oss << L" {key:'program',label:'番組表',url:''},\n";
        oss << L" {key:'reservation',label:'予約リスト',url:'"
            << JsEscape_(reservationListFile)
            << L"'},\n";
        oss << L" {key:'keyword',label:'キーワード検索',url:'"
            << JsEscape_(keywordSearchFile)
            << L"'},\n";
        oss << L" {key:'autoKeyword',label:'自動検索予約リスト',url:'"
            << JsEscape_(autoKeywordReservationListFile)
            << L"'},\n";
        oss << L" {key:'log',label:'ログ',url:'"
            << JsEscape_(logFile)
            << L"'}\n";
        oss << L"];\n";

        oss << L"function findPage(key){\n";
        oss << L" for(var i=0;i<pageList.length;i++){if(pageList[i].key===key){return pageList[i];}}\n";
        oss << L" return pageList[0];\n";
        oss << L"}\n";

        oss << L"function setGuideVisible(visible){\n";
        oss << L" var v=visible?'':'none';\n";
        oss << L" if(guideToolbarA){guideToolbarA.style.display=v;}\n";
        oss << L" if(guideToolbarB){guideToolbarB.style.display=v;}\n";
        oss << L"}\n";

        oss << L"function renderPages(){\n";
        oss << L" clear(pageBox);\n";
        oss << L" pageList.forEach(function(p){\n";
        oss << L"   pageBox.appendChild(button(p.label,p.key===activePage,function(){activePage=p.key;renderAll();}));\n";
        oss << L" });\n";
        oss << L"}\n";

        oss << L"function renderMode(){\n";
        oss << L" clear(modeBox);\n";
        oss << L" modeBox.appendChild(button('当日以降',mode==='now',function(){mode='now';curDay=0;curView='list';curBlock='b04';renderAll();}));\n";
        oss << L" modeBox.appendChild(button('過去アーカイブ',mode==='before',function(){mode='before';renderAll();}));\n";
        oss << L"}\n";

        oss << L"function renderDays(){\n";
        oss << L" clear(dayBox);\n";
        oss << L" if(mode!=='now'){return;}\n";
        oss << L" days.forEach(function(d,i){dayBox.appendChild(button(d.label||d.key,i===curDay,function(){curDay=i;curBlock='b04';renderAll();}));});\n";
        oss << L"}\n";

        oss << L"function renderView(){\n";
        oss << L" clear(viewBox);\n";
        oss << L" if(mode!=='now'){return;}\n";
        oss << L" viewBox.appendChild(button('表一覧',curView==='list',function(){curView='list';renderAll();}));\n";
        oss << L" viewBox.appendChild(button('時間割',curView==='timetable',function(){curView='timetable';renderAll();}));\n";
        oss << L"}\n";

        oss << L"function renderBlocks(){\n";
        oss << L" clear(blockBox);\n";
        oss << L" if(mode!=='now'){return;}\n";
        oss << L" var blocks=[['b04','04-10'],['b10','10-16'],['b16','16-22'],['b22','22-04']];\n";
        oss << L" blocks.forEach(function(x){blockBox.appendChild(button(x[1],x[0]===curBlock,function(){curBlock=x[0];renderAll();}));});\n";
        oss << L"}\n";

        oss << L"function firstAvailableBlock(day,view){\n";
        oss << L" var map=view==='list'?(day.listBlocks||{}):(day.timetableBlocks||{});\n";
        oss << L" var keys=['b04','b10','b16','b22'];\n";
        oss << L" for(var i=0;i<keys.length;i++){if(map[keys[i]]){return keys[i];}}\n";
        oss << L" return 'b04';\n";
        oss << L"}\n";

        oss << L"function getNowUrl(day){\n";
        oss << L" if(!day){return '';}\n";
        oss << L" var map=curView==='list'?(day.listBlocks||{}):(day.timetableBlocks||{});\n";
        oss << L" if(!map[curBlock]){curBlock=firstAvailableBlock(day,curView);}\n";
        oss << L" if(map[curBlock]){return map[curBlock];}\n";

        // 旧サービス別ページへの保険。
        oss << L" var services=day.services||[];\n";
        oss << L" if(services.length>0){\n";
        oss << L"   if(curView==='list'&&services[0].list){return services[0].list;}\n";
        oss << L"   if(curView==='timetable'&&services[0].blocks&&services[0].blocks[curBlock]){return services[0].blocks[curBlock];}\n";
        oss << L" }\n";

        oss << L" return '';\n";
        oss << L"}\n";

        oss << L"function updateFrame(){\n";
        oss << L" if(activePage!=='program'){\n";
        oss << L"   setGuideVisible(false);\n";
        oss << L"   var page=findPage(activePage);\n";
        oss << L"   if(page&&page.url){frame.src=page.url;if(status){status.textContent=page.label;}}\n";
        oss << L"   else{frame.removeAttribute('src');if(status){status.textContent='ページを表示できません';}}\n";
        oss << L"   return;\n";
        oss << L" }\n";
        oss << L" setGuideVisible(true);\n";
        oss << L" if(mode==='before'){\n";
        oss << L"   clear(viewBox);\n";
        oss << L"   clear(blockBox);\n";
        oss << L"   var beforeUrl=space&&space.beforeIndex;\n";
        oss << L"   if(beforeUrl){frame.src=beforeUrl;status.textContent='過去アーカイブ';}\n";
        oss << L"   else{frame.removeAttribute('src');status.textContent='過去アーカイブはありません';}\n";
        oss << L"   return;\n";
        oss << L" }\n";

        oss << L" var day=days[curDay];\n";
        oss << L" var url=getNowUrl(day);\n";
        oss << L" if(url){\n";
        oss << L"   frame.src=url;\n";
        oss << L"   status.textContent=(day.label||day.key)+' / '+(curView==='list'?'表一覧':'時間割')+' / '+curBlock.replace('b','');\n";
        oss << L" }\n";
        oss << L" else{\n";
        oss << L"   frame.removeAttribute('src');\n";
        oss << L"   status.textContent='表示できる番組データがありません';\n";
        oss << L" }\n";
        oss << L"}\n";

        oss << L"function renderAll(){renderPages();renderMode();renderDays();renderView();renderBlocks();updateFrame();}\n";
        oss << L"renderAll();\n";

        oss << L"})();\n";
        oss << L"</script>\n";

        oss << L"</body>\n";
        oss << L"</html>\n";

        return WriteUtf8File_(path, oss.str());
    }

    void RemoveLegacyLiteFiles_(const std::filesystem::path& outputDir, int space)
    {
        std::error_code ec;

        const std::wstring suffix = L"_space" + std::to_wstring(space);

        const std::array<std::wstring, 10> names = {
            L"epg_lite_program_menu" + suffix + L".html",
            L"epg_lite_past_program_menu" + suffix + L".html",
            L"epg_lite_now_list" + suffix + L".html",
            L"epg_lite_now_timetable" + suffix + L".html",
            L"epg_lite_before_list" + suffix + L".html",
            L"epg_lite_before_timetable" + suffix + L".html",
            L"epg_lite_keyword_search" + suffix + L".html",
            L"epg_lite_reservation_list" + suffix + L".html",
            L"epg_lite_auto_keyword_reservation_list" + suffix + L".html",
            L"epg_lite_reserve_switch.js"
        };

        for (const auto& name : names) {
            std::filesystem::remove(outputDir / name, ec);
            ec.clear();
        }
    }

    std::wstring BuildDiagnostics_(
        std::size_t totalEntries,
        std::size_t keptEntries,
        std::size_t skippedEntries,
        const std::vector<NowDayManifest_>& nowDays,
        const std::vector<std::wstring>& beforeDates,
        const LiteGenreDiagnostics_& genreDiag)
    {
        std::wostringstream oss;

        oss << L"EPG Lite diagnostics\n";
        oss << L"totalEntries=" << totalEntries << L"\n";
        oss << L"keptEntries=" << keptEntries << L"\n";
        oss << L"skippedEntries=" << skippedEntries << L"\n";

        oss << BuildGenreDiagnosticsText_(genreDiag);

        oss << L"nowDays=" << nowDays.size() << L"\n";
        for (const auto& d : nowDays) {
            oss << L"  now " << d.key << L" services=" << d.services.size() << L"\n";
        }

        oss << L"beforeDates=" << beforeDates.size() << L"\n";
        for (const auto& d : beforeDates) {
            oss << L"  before " << d << L"\n";
        }

        return oss.str();
    }

}

namespace EpgHtmlLite::SplitGuide
{
    bool Generate(const Context& ctx, Result& result)
    {
        if (!ctx.sharedData) {
            return false;
        }

        const int futureDays = ctx.futureDays > 0 ? ctx.futureDays : 8;
        const int pastKeepDays = ctx.pastKeepDays;

        const std::filesystem::path liteRoot = ctx.outputDir / L"lite";
        const std::filesystem::path spaceRoot = liteRoot / SpaceDirName_(ctx.space);
        const std::filesystem::path nowRoot = spaceRoot / L"now";
        const std::filesystem::path beforeRoot = spaceRoot / L"before";


        std::error_code ec;
        std::filesystem::remove_all(spaceRoot, ec);
        ec.clear();

        std::filesystem::create_directories(nowRoot, ec);
        if (ec) {
            return false;
        }
        std::filesystem::create_directories(beforeRoot, ec);
        if (ec) {
            return false;
        }


        RemoveLegacyLiteFiles_(ctx.outputDir, ctx.space);
        const LiteChannelInfoMap_ channelInfoMap =
            LoadLiteChannelInfoMap_(ctx);


        if (!WriteUtf8File_(liteRoot / L"epg_lite_split.css", BuildSplitCss_())) {
            return false;
        }
        result.generatedFiles.push_back((liteRoot / L"epg_lite_split.css").wstring());



        if (!WriteUtf8File_(liteRoot / L"epg_lite_genre_filter.js", BuildGenreFilterJs_())) {
            return false;
        }
        result.generatedFiles.push_back((liteRoot / L"epg_lite_genre_filter.js").wstring());

        if (!WriteUtf8File_(liteRoot / L"epg_lite_subchannel_filter.js", BuildSubchannelFilterJs_())) {
            return false;
        }
        result.generatedFiles.push_back((liteRoot / L"epg_lite_subchannel_filter.js").wstring());



        std::set<std::wstring> nowSet;
        std::set<std::wstring, std::greater<std::wstring>> beforeSet;




        std::size_t keptEntries = 0;
        std::size_t skippedEntries = 0;

        LiteGenreDiagnostics_ genreDiag{};

        for (const auto& e : ctx.sharedData->entries) {
            if (!IsKeptEntry_(e, ctx.space, futureDays, pastKeepDays)) {
                ++skippedEntries;
                continue;
            }

            ++keptEntries;
            AddLiteGenreDiagnostics_(genreDiag, e);



            if (e.isPast) {
                beforeSet.insert(e.broadcastDateKey);
            }
            else {
                nowSet.insert(e.broadcastDateKey);
            }
        }

        std::vector<NowDayManifest_> nowDays;
        nowDays.reserve(nowSet.size());

        for (const auto& d : nowSet) {
            NowDayManifest_ day{};
            day.key = d;
            day.label = DateLabel_(d);

            const std::filesystem::path dayDir = nowRoot / d;

            for (int block = 0; block < kBlockCount; ++block) {
                const std::vector<const ProgramEntry*> blockRows =
                    CollectForDateBlock_(
                        *ctx.sharedData,
                        ctx.space,
                        d,
                        false,
                        block,
                        futureDays,
                        pastKeepDays);

                if (blockRows.empty()) {
                    continue;
                }

                const std::wstring blockName = BlockSuffix_(block);

                const std::filesystem::path listPath =
                    dayDir / (L"list_" + blockName + L".html");

                EpgHtmlLite::PageNowList::BlockPageRequest listReq{};
                listReq.outputPath = listPath;
                listReq.pageTitle = ctx.title + L" Lite";
                listReq.dateKey = d;
                listReq.block = block;
                listReq.rows = blockRows;
                listReq.space = ctx.space;

                listReq.resolveServiceName =
                    [&channelInfoMap](const ProgramEntry& e) {
                    return ResolveLiteServiceName_(e, channelInfoMap);
                    };

                listReq.isSubchannel =
                    [&channelInfoMap](const ProgramEntry& e) {
                    return IsLiteSubchannel_(e, channelInfoMap);
                    };

                if (!EpgHtmlLite::PageNowList::GenerateBlockPage(
                    listReq,
                    result)) {
                    return false;
                }

                day.listBlockHref[static_cast<std::size_t>(block)] =
                    L"lite/" + SpaceDirName_(ctx.space) + L"/now/" + d + L"/list_" + blockName + L".html";

                const std::filesystem::path timetablePath =
                    dayDir / (L"timetable_" + blockName + L".html");

                EpgHtmlLite::PageNowTimetable::BlockPageRequest timetableReq{};
                timetableReq.outputPath = timetablePath;
                timetableReq.pageTitle = ctx.title + L" Lite";
                timetableReq.dateKey = d;
                timetableReq.block = block;
                timetableReq.rows = blockRows;
                timetableReq.space = ctx.space;

                timetableReq.resolveServiceName =
                    [&channelInfoMap](const ProgramEntry& e) {
                    return ResolveLiteServiceName_(e, channelInfoMap);
                    };

                timetableReq.isSubchannel =
                    [&channelInfoMap](const ProgramEntry& e) {
                    return IsLiteSubchannel_(e, channelInfoMap);
                    };

                if (!EpgHtmlLite::PageNowTimetable::GenerateBlockPage(
                    timetableReq,
                    result)) {
                    return false;
                }

                day.timetableBlockHref[static_cast<std::size_t>(block)] =
                    L"lite/" + SpaceDirName_(ctx.space) + L"/now/" + d + L"/timetable_" + blockName + L".html";
            }



            nowDays.push_back(std::move(day));
        }

        std::vector<std::wstring> beforeDates(beforeSet.begin(), beforeSet.end());

        if (!WriteBeforeArchiveIndex_(
            beforeRoot / L"index.html",
            ctx.title + L" Lite 過去アーカイブ",
            beforeDates)) {
            return false;
        }
        result.generatedFiles.push_back((beforeRoot / L"index.html").wstring());

        for (const auto& d : beforeDates) {
            const auto rows = CollectForDate_(
                *ctx.sharedData,
                ctx.space,
                d,
                true,
                futureDays,
                pastKeepDays);

            const std::filesystem::path p = beforeRoot / (d + L".html");

            if (!WriteBeforeArchiveDatePage_(
                p,
                ctx.title + L" Lite 過去アーカイブ",
                d,
                rows,
                channelInfoMap)) {
                return false;
            }

            result.generatedFiles.push_back(p.wstring());
        }



        if (!WriteManifest_(liteRoot / L"manifest.js", ctx.space, nowDays, beforeDates)) {
            return false;
        }
        result.generatedFiles.push_back((liteRoot / L"manifest.js").wstring());

        if (!EpgHtmlLite::PageReservationList::Generate(
            ctx,
            result)) {
            return false;
        }

        if (!EpgHtmlLite::PageKeywordSearch::Generate(
            ctx,
            result)) {
            return false;
        }

        if (!EpgHtmlLite::PageAutoKeywordReservationList::Generate(
            ctx,
            result)) {
            return false;
        }

        if (!EpgHtmlLite::Log::Generate(
            ctx,
            result)) {
            DebugLogFormat(
                L"[EPG-LITE][LOG] page generation failed space=%d outputDir=%ls",
                ctx.space,
                ctx.outputDir.wstring().c_str());
            return false;
        }

        if (!WriteServerRootIndex_(
            ctx.outputDir / L"index.html",
            ctx.title.empty() ? L"EPG Lite" : ctx.title,
            ctx.space)) {
            return false;
        }
        result.generatedFiles.push_back((ctx.outputDir / L"index.html").wstring());

        const std::wstring diagnostics =
            BuildDiagnostics_(
                ctx.sharedData->entries.size(),
                keptEntries,
                skippedEntries,
                nowDays,
                beforeDates,
                genreDiag);

        DebugLogFormat(
            L"[EPG-LITE][GENRE] total=%zu empty=%zu other=%zu news=%zu sports=%zu info=%zu drama=%zu anime=%zu",
            genreDiag.total,
            genreDiag.emptySource,
            genreDiag.normalizedOther,
            genreDiag.majorCount[0],
            genreDiag.majorCount[1],
            genreDiag.majorCount[7],
            genreDiag.majorCount[2],
            genreDiag.majorCount[6]);

        if (!WriteUtf8File_(spaceRoot / L"diagnostics.txt", diagnostics)) {
            return false;
        }
        result.generatedFiles.push_back((spaceRoot / L"diagnostics.txt").wstring());

        result.indexPath = (ctx.outputDir / L"index.html").wstring();
        result.success = true;

        return true;
    }
}
