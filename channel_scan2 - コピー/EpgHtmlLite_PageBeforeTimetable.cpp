#include "EpgHtmlLite_PageBeforeTimetable.hpp"
#include "EpgHtmlLite_Common.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using EpgHtmlLite::ProgramEntry;

    constexpr int kLiteBroadcastStartMinutes_ = 4 * 60;
    constexpr int kLiteMinutesPerDay_ = 24 * 60;
    constexpr int kLiteTableCompactMaxMinutes_ = 10;
    constexpr int kLiteTableMissingEndAssumedMinutes_ = 30;
    constexpr int kLiteTableRepairGapMaxMinutes_ = 3;

    struct LiteBlockMinuteRange_
    {
        int begin = -1;
        int end = -1;
    };

    std::wstring HtmlEscape_(
        std::wstring_view s)
    {
        std::wstring out;
        out.reserve(s.size() + 16);

        for (const wchar_t ch : s) {
            switch (ch) {
            case L'&':
                out += L"&amp;";
                break;
            case L'<':
                out += L"&lt;";
                break;
            case L'>':
                out += L"&gt;";
                break;
            case L'"':
                out += L"&quot;";
                break;
            default:
                if (ch == L'\r' ||
                    ch == L'\n' ||
                    ch == L'\t') {
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

    void AppendUtf8CodePoint_(
        std::string& out,
        std::uint32_t cp)
    {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
            return;
        }

        if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            return;
        }

        if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            return;
        }

        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }

    std::string WideToUtf8_(
        std::wstring_view s)
    {
        std::string out;
        out.reserve(s.size() * 3 + 8);

        for (std::size_t i = 0; i < s.size(); ++i) {
            std::uint32_t cp =
                static_cast<std::uint32_t>(s[i]);

#if WCHAR_MAX <= 0xFFFF
            if (cp >= 0xD800 &&
                cp <= 0xDBFF &&
                (i + 1) < s.size()) {
                const std::uint32_t lo =
                    static_cast<std::uint32_t>(s[i + 1]);

                if (lo >= 0xDC00 &&
                    lo <= 0xDFFF) {
                    cp =
                        0x10000 +
                        (((cp - 0xD800) << 10) |
                            (lo - 0xDC00));
                    ++i;
                }
            }
#endif

            AppendUtf8CodePoint_(out, cp);
        }

        return out;
    }

    bool WriteUtf8File_(
        const std::filesystem::path& path,
        const std::wstring& text)
    {
        std::error_code ec;

        const std::filesystem::path parent =
            path.parent_path();

        if (!parent.empty()) {
            std::filesystem::create_directories(
                parent,
                ec);

            if (ec) {
                return false;
            }
        }

        std::ofstream ofs(
            path,
            std::ios::binary | std::ios::trunc);

        if (!ofs) {
            return false;
        }

        const std::string bytes =
            WideToUtf8_(text);

        if (!bytes.empty()) {
            ofs.write(
                bytes.data(),
                static_cast<std::streamsize>(bytes.size()));
        }

        ofs.flush();
        return ofs.good();
    }

    int NormalizeBlock_(
        int block) noexcept
    {
        if (block < 0 || block > 3) {
            return 0;
        }

        return block;
    }

    std::wstring LiteBlockLabel_(
        int block)
    {
        switch (NormalizeBlock_(block)) {
        case 0:
            return L"04:00-09:59";
        case 1:
            return L"10:00-15:59";
        case 2:
            return L"16:00-21:59";
        case 3:
            return L"22:00-03:59";
        default:
            return L"全時間";
        }
    }

    std::wstring DateLabel_(
        const std::wstring& key)
    {
        if (key.size() == 8) {
            return key.substr(0, 4) +
                L"/" +
                key.substr(4, 2) +
                L"/" +
                key.substr(6, 2);
        }

        return key;
    }

    std::wstring ClipText_(
        std::wstring s,
        std::size_t maxLen)
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

    std::wstring ClipSummary_(
        const std::wstring& s,
        std::size_t maxLen)
    {
        return ClipText_(s, maxLen);
    }

    LiteBlockMinuteRange_ GetLiteBlockMinuteRange_(
        int block) noexcept
    {
        switch (NormalizeBlock_(block)) {
        case 0:
            return { 4 * 60, 10 * 60 };
        case 1:
            return { 10 * 60, 16 * 60 };
        case 2:
            return { 16 * 60, 22 * 60 };
        case 3:
            return { 22 * 60, 28 * 60 };
        default:
            return {};
        }
    }

    int NormalizeToLiteBroadcastMinute_(
        int minute) noexcept
    {
        if (minute < 0) {
            return -1;
        }

        if (minute < kLiteBroadcastStartMinutes_) {
            return minute + kLiteMinutesPerDay_;
        }

        return minute;
    }

    std::wstring FormatMinuteOfDayText_(
        int minute)
    {
        if (minute < 0) {
            return L"--:--";
        }

        minute %= kLiteMinutesPerDay_;

        if (minute < 0) {
            minute += kLiteMinutesPerDay_;
        }

        wchar_t buf[16]{};

        std::swprintf(
            buf,
            static_cast<std::size_t>(16),
            L"%02d:%02d",
            minute / 60,
            minute % 60);

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

        return t == L"--:--" ||
            t == L"--" ||
            t == L"-" ||
            t == L"??:??";
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

        while (pos < text.size() &&
            !isDigit(text[pos])) {
            ++pos;
        }

        if (pos >= text.size()) {
            return false;
        }

        int hour = 0;

        while (pos < text.size() &&
            isDigit(text[pos])) {
            hour =
                hour * 10 +
                static_cast<int>(text[pos] - L'0');
            ++pos;
        }

        while (pos < text.size() &&
            (text[pos] == L' ' ||
                text[pos] == L'\t' ||
                text[pos] == L'　')) {
            ++pos;
        }

        if (pos >= text.size() ||
            (text[pos] != L':' &&
                text[pos] != L'：')) {
            return false;
        }

        ++pos;

        int minute = 0;
        int minuteDigits = 0;

        while (pos < text.size() &&
            isDigit(text[pos]) &&
            minuteDigits < 2) {
            minute =
                minute * 10 +
                static_cast<int>(text[pos] - L'0');
            ++pos;
            ++minuteDigits;
        }

        if (minuteDigits == 0) {
            return false;
        }

        if (hour < 0 ||
            hour > 47 ||
            minute < 0 ||
            minute > 59) {
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

    int PickLiteStartBroadcastMinute_(
        const ProgramEntry& e) noexcept
    {
        if (e.startMinutesOfDay >= 0) {
            return NormalizeToLiteBroadcastMinute_(
                e.startMinutesOfDay);
        }

        int parsed = -1;

        if (TryParseLiteClockText_(
            e.startText,
            parsed)) {
            return parsed;
        }

        if (TryParseLiteClockText_(
            e.startTime,
            parsed)) {
            return parsed;
        }

        return -1;
    }

    int PickRawEndBroadcastMinute_(
        const ProgramEntry& e) noexcept
    {
        if (e.endMinutesOfDay >= 0) {
            return NormalizeToLiteBroadcastMinute_(
                e.endMinutesOfDay);
        }

        int parsed = -1;

        if (TryParseLiteClockText_(
            e.endText,
            parsed)) {
            return parsed;
        }

        if (TryParseLiteClockText_(
            e.endTime,
            parsed)) {
            return parsed;
        }

        return -1;
    }

    using LiteEndFallbackMap_ =
        std::unordered_map<std::wstring, int>;

    std::wstring MakeLiteProgramTimeKey_(
        const ProgramEntry& e)
    {
        return e.broadcastDateKey +
            L"|" +
            std::to_wstring(
                static_cast<unsigned int>(e.serviceId)) +
            L"|" +
            std::to_wstring(
                static_cast<unsigned int>(e.eventId)) +
            L"|" +
            std::to_wstring(e.startMinutesOfDay);
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
                std::to_wstring(
                    static_cast<unsigned int>(p->serviceId));

            groups[groupKey].push_back(p);
        }

        const LiteBlockMinuteRange_ blockRange =
            GetLiteBlockMinuteRange_(block);

        for (auto& pair : groups) {
            auto& list = pair.second;

            std::sort(
                list.begin(),
                list.end(),
                [](const ProgramEntry* a, const ProgramEntry* b) {
                    const int aa =
                        PickLiteStartBroadcastMinute_(*a);

                    const int bb =
                        PickLiteStartBroadcastMinute_(*b);

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
                    PickLiteStartBroadcastMinute_(e);

                if (startNorm < 0) {
                    continue;
                }

                int explicitEnd =
                    PickRawEndBroadcastMinute_(e);

                if (explicitEnd > 0 &&
                    explicitEnd <= startNorm) {
                    explicitEnd += kLiteMinutesPerDay_;
                }

                if (explicitEnd <= startNorm) {
                    explicitEnd = -1;
                }

                int nextStart = -1;

                for (std::size_t j = i + 1; j < list.size(); ++j) {
                    const int candidate =
                        PickLiteStartBroadcastMinute_(*list[j]);

                    if (candidate > startNorm) {
                        nextStart = candidate;
                        break;
                    }
                }

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
                            startNorm +
                            kLiteTableMissingEndAssumedMinutes_;
                    }

                    if (fallbackEnd > startNorm) {
                        out[MakeLiteProgramTimeKey_(e)] =
                            fallbackEnd;
                    }

                    continue;
                }

                if (nextStart > explicitEnd) {
                    const int gapMinutes =
                        nextStart - explicitEnd;

                    if (gapMinutes > 0 &&
                        gapMinutes <= kLiteTableRepairGapMaxMinutes_) {
                        out[MakeLiteProgramTimeKey_(e)] =
                            nextStart;
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
        const auto it =
            fallbackMap.find(
                MakeLiteProgramTimeKey_(e));

        if (it == fallbackMap.end()) {
            return -1;
        }

        return it->second;
    }

    int PickLiteEndBroadcastMinute_(
        const ProgramEntry& e,
        const LiteEndFallbackMap_& fallbackMap) noexcept
    {
        const int mappedEnd =
            FindLiteFallbackEndMinute_(
                fallbackMap,
                e);

        if (mappedEnd >= 0) {
            return mappedEnd;
        }

        return PickRawEndBroadcastMinute_(e);
    }

    bool GetProgramLiteMinuteRange_(
        const ProgramEntry& e,
        const LiteEndFallbackMap_& fallbackMap,
        int& outBegin,
        int& outEnd) noexcept
    {
        outBegin =
            PickLiteStartBroadcastMinute_(e);

        if (outBegin < 0) {
            outEnd = -1;
            return false;
        }

        outEnd =
            PickLiteEndBroadcastMinute_(
                e,
                fallbackMap);

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

    bool ProgramOverlapsLiteBlock_(
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

        int begin = -1;
        int end = -1;

        if (!GetProgramLiteMinuteRange_(
            e,
            fallbackMap,
            begin,
            end)) {
            return e.timeBlock == block;
        }

        return begin < blockRange.end &&
            blockRange.begin < end;
    }

    bool GetClippedLiteMinuteRange_(
        const ProgramEntry& e,
        const LiteBlockMinuteRange_& range,
        const LiteEndFallbackMap_& fallbackMap,
        int& outBeginOffset,
        int& outEndOffset) noexcept
    {
        outBeginOffset = 0;
        outEndOffset = 0;

        if (range.begin < 0 ||
            range.end <= range.begin) {
            return false;
        }

        int begin = -1;
        int end = -1;

        if (!GetProgramLiteMinuteRange_(
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

        outBeginOffset =
            clippedBegin - range.begin;

        outEndOffset =
            clippedEnd - range.begin;

        return outEndOffset > outBeginOffset;
    }

    std::wstring GetBlockOverlapStateText_(
        const ProgramEntry& e,
        int block,
        const LiteEndFallbackMap_& fallbackMap)
    {
        const LiteBlockMinuteRange_ blockRange =
            GetLiteBlockMinuteRange_(block);

        if (blockRange.begin < 0 ||
            blockRange.end <= blockRange.begin) {
            return {};
        }

        int programBegin = -1;
        int programEnd = -1;

        if (!GetProgramLiteMinuteRange_(
            e,
            fallbackMap,
            programBegin,
            programEnd)) {
            return {};
        }

        const bool beginsBeforeBlock =
            programBegin < blockRange.begin;

        const bool endsAfterBlock =
            programEnd > blockRange.end;

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

    std::wstring TimeRangeTextForBlock_(
        const ProgramEntry& e,
        int block,
        const LiteEndFallbackMap_& fallbackMap)
    {
        const LiteBlockMinuteRange_ blockRange =
            GetLiteBlockMinuteRange_(block);

        if (blockRange.begin < 0 ||
            blockRange.end <= blockRange.begin) {
            return L"--:--";
        }

        int programBegin = -1;
        int programEnd = -1;

        if (!GetProgramLiteMinuteRange_(
            e,
            fallbackMap,
            programBegin,
            programEnd)) {
            return L"--:--";
        }

        const int clippedBegin =
            (std::max)(programBegin, blockRange.begin);

        const int clippedEnd =
            (std::min)(programEnd, blockRange.end);

        if (clippedEnd <= clippedBegin) {
            return L"--:--";
        }

        return FormatMinuteOfDayText_(clippedBegin) +
            L"～" +
            FormatMinuteOfDayText_(clippedEnd);
    }

    std::wstring ExtractGenreMajor_(
        const std::wstring& genreKey)
    {
        if (genreKey.empty()) {
            return L"その他";
        }

        const std::array<std::wstring_view, 12> majorNames = {
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

        for (std::wstring_view name : majorNames) {
            if (genreKey.find(name) != std::wstring::npos) {
                return std::wstring(name);
            }
        }

        return L"その他";
    }

    void AppendProgramAttrs_(
        std::wostringstream& oss,
        const ProgramEntry& e)
    {
        const std::wstring genreMajor =
            ExtractGenreMajor_(e.genreKey);

        oss << L" data-date=\""
            << HtmlEscape_(e.broadcastDateKey)
            << L"\"";

        oss << L" data-sid=\""
            << static_cast<unsigned int>(e.serviceId)
            << L"\"";

        oss << L" data-eid=\""
            << static_cast<unsigned int>(e.eventId)
            << L"\"";

        oss << L" data-genre-item=\"1\"";

        oss << L" data-genre-major=\""
            << HtmlEscape_(genreMajor)
            << L"\"";
    }

    struct LiteTableService_
    {
        std::uint16_t sid = 0;
        std::wstring name;
        bool isSubchannel = false;
        std::vector<const ProgramEntry*> programs;

        std::vector<const ProgramEntry*> startAt;
        std::vector<int> rowspanAt;
        std::vector<bool> covered;

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

            if (!GetClippedLiteMinuteRange_(
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
                service.covered[
                    static_cast<std::size_t>(cellBegin)]) {
                ++cellBegin;
            }

            if (cellBegin >= endOffset) {
                continue;
            }

            int cellEnd = cellBegin;

            while (cellEnd < endOffset &&
                !service.covered[
                    static_cast<std::size_t>(cellEnd)]) {
                ++cellEnd;
            }

            if (cellEnd <= cellBegin) {
                cellEnd = cellBegin + 1;
            }

            service.startAt[
                static_cast<std::size_t>(cellBegin)] = p;

            service.rowspanAt[
                static_cast<std::size_t>(cellBegin)] =
                (std::max)(1, cellEnd - cellBegin);

                for (int i = cellBegin; i < cellEnd; ++i) {
                    service.covered[
                        static_cast<std::size_t>(i)] = true;
                }
        }

        int pos = 0;

        while (pos < blockMinutes) {
            if (service.covered[
                static_cast<std::size_t>(pos)]) {
                ++pos;
                continue;
            }

            const int gapBegin = pos;

            while (pos < blockMinutes &&
                !service.covered[
                    static_cast<std::size_t>(pos)]) {
                ++pos;
            }

            const int gapEnd = pos;

            if (gapEnd > gapBegin) {
                service.gapRowspanAt[
                    static_cast<std::size_t>(gapBegin)] =
                    gapEnd - gapBegin;

                    for (int i = gapBegin; i < gapEnd; ++i) {
                        service.gapCovered[
                            static_cast<std::size_t>(i)] = true;
                    }
            }
        }
    }

    std::wstring ResolveServiceName_(
        const EpgHtmlLite::PageBeforeTimetable::DatePageRequest& request,
        const ProgramEntry& e)
    {
        if (request.resolveServiceName) {
            const std::wstring name =
                request.resolveServiceName(e);

            if (!name.empty()) {
                return name;
            }
        }

        if (!e.serviceName.empty()) {
            return e.serviceName;
        }

        return L"SID " +
            std::to_wstring(
                static_cast<unsigned int>(e.serviceId));
    }

    bool IsSubchannel_(
        const EpgHtmlLite::PageBeforeTimetable::DatePageRequest& request,
        const ProgramEntry& e)
    {
        if (request.isSubchannel) {
            return request.isSubchannel(e);
        }

        return false;
    }

    void AppendTimetableCss_(
        std::wostringstream& oss)
    {
        oss << L".lite-table-wrap{\n";
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
        oss << L"  background:#3a3a3a;\n";
        oss << L"  border-color:#555;\n";
        oss << L"}\n";

        oss << L".lite-epg-table td.empty-cell.gap-cell{\n";
        oss << L"  padding:0;\n";
        oss << L"  background:#4a4a4a;\n";
        oss << L"  border-top-color:#686868;\n";
        oss << L"  border-bottom-color:#686868;\n";
        oss << L"}\n";

        oss << L".lite-epg-table td.empty-cell.gap-cell:hover{\n";
        oss << L"  background:#5a5a5a;\n";
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

        oss << L".subchannel-hidden{\n";
        oss << L"  display:none!important;\n";
        oss << L"}\n";

        oss << L".genre-hidden{\n";
        oss << L"  display:none!important;\n";
        oss << L"}\n";

        oss << L".lite-page-note{\n";
        oss << L"  color:#aab6c8;\n";
        oss << L"  font-size:12px;\n";
        oss << L"  margin:0 0 8px;\n";
        oss << L"}\n";
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

    std::wstring BuildGenreToolbar_(
        const std::wstring& scopeId)
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

    std::wstring BuildSubchannelToolbar_(
        const std::wstring& scopeId)
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

    void AppendLiteFrameSwitchCss_(
        std::wostringstream& oss)
    {
        oss << L"html,body{\n";
        oss << L"  height:100%;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page{\n";
        oss << L"  height:100vh;\n";
        oss << L"  min-height:0;\n";
        oss << L"  overflow:hidden;\n";
        oss << L"  display:flex;\n";
        oss << L"  flex-direction:column;\n";
        oss << L"  background:#111;\n";
        oss << L"}\n";

        // root側に日付/表示/時間帯切替があるので、iframe内のCommonメニューは隠す。
        oss << L"body.lite-frame-page .top{\n";
        oss << L"  display:none!important;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .main{\n";
        oss << L"  flex:1 1 auto;\n";
        oss << L"  min-height:0;\n";
        oss << L"  display:flex;\n";
        oss << L"  flex-direction:column;\n";
        oss << L"  overflow:hidden;\n";
        oss << L"  padding:8px 10px;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .panel{\n";
        oss << L"  flex:1 1 auto;\n";
        oss << L"  min-height:0;\n";
        oss << L"  display:flex;\n";
        oss << L"  flex-direction:column;\n";
        oss << L"  overflow:hidden;\n";
        oss << L"  margin:8px 0 0;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .genre-bar,\n";
        oss << L"body.lite-frame-page .subch-bar{\n";
        oss << L"  flex:0 0 auto;\n";
        oss << L"  display:flex;\n";
        oss << L"  gap:6px;\n";
        oss << L"  flex-wrap:wrap;\n";
        oss << L"  align-items:center;\n";
        oss << L"  margin:0 0 8px;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .genre-label,\n";
        oss << L"body.lite-frame-page .subch-label{\n";
        oss << L"  font-size:12px;\n";
        oss << L"  color:#aab6c8;\n";
        oss << L"  margin-right:2px;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .genre-btn,\n";
        oss << L"body.lite-frame-page .subch-btn{\n";
        oss << L"  background:#1f1f1f;\n";
        oss << L"  color:#ddd;\n";
        oss << L"  border:1px solid #444;\n";
        oss << L"  padding:4px 8px;\n";
        oss << L"  cursor:pointer;\n";
        oss << L"  font-size:12px;\n";
        oss << L"  line-height:1.25;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .genre-btn:hover,\n";
        oss << L"body.lite-frame-page .subch-btn:hover{\n";
        oss << L"  background:#2a2a2a;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .genre-btn.active,\n";
        oss << L"body.lite-frame-page .subch-btn.active{\n";
        oss << L"  background:#345;\n";
        oss << L"  border-color:#69c;\n";
        oss << L"  color:#fff;\n";
        oss << L"}\n";
    }

    std::vector<const ProgramEntry*> FilterAndSortRows_(
        const std::vector<const ProgramEntry*>& rows,
        int block,
        const LiteEndFallbackMap_& fallbackMap)
    {
        std::vector<const ProgramEntry*> out;

        out.reserve(rows.size());

        for (const ProgramEntry* p : rows) {
            if (!p) {
                continue;
            }

            if (p->title.empty()) {
                continue;
            }

            if (!ProgramOverlapsLiteBlock_(
                *p,
                block,
                fallbackMap)) {
                continue;
            }

            out.push_back(p);
        }

        std::sort(
            out.begin(),
            out.end(),
            [](const ProgramEntry* a, const ProgramEntry* b) {
                const int aStart =
                    a ? PickLiteStartBroadcastMinute_(*a) : -1;

                const int bStart =
                    b ? PickLiteStartBroadcastMinute_(*b) : -1;

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

        return out;
    }

    std::vector<LiteTableService_> BuildServices_(
        const EpgHtmlLite::PageBeforeTimetable::DatePageRequest& request,
        const std::vector<const ProgramEntry*>& rows,
        const LiteBlockMinuteRange_& blockRange,
        const LiteEndFallbackMap_& endFallbackMap)
    {
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
                        PickLiteStartBroadcastMinute_(*a);

                    const int bStart =
                        PickLiteStartBroadcastMinute_(*b);

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

            for (const ProgramEntry* p : service.programs) {
                if (!p) {
                    continue;
                }

                service.name =
                    ResolveServiceName_(request, *p);

                service.isSubchannel =
                    IsSubchannel_(request, *p);

                break;
            }

            if (service.name.empty()) {
                service.name =
                    L"SID " +
                    std::to_wstring(
                        static_cast<unsigned int>(service.sid));
            }

            BuildLiteTableServiceCells_(
                service,
                blockRange,
                endFallbackMap);

            services.push_back(std::move(service));
        }

        return services;
    }

    void AppendProgramCell_(
        std::wostringstream& oss,
        const ProgramEntry& e,
        const LiteTableService_& service,
        int rowspan,
        int block,
        const LiteEndFallbackMap_& endFallbackMap)
    {
        const bool compactCell =
            rowspan <= kLiteTableCompactMaxMinutes_;

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

        AppendProgramAttrs_(oss, e);

        oss << L">";

        oss << L"<div class=\"program-time\">";
        oss << HtmlEscape_(
            TimeRangeTextForBlock_(
                e,
                block,
                endFallbackMap));

        const std::wstring overlapState =
            GetBlockOverlapStateText_(
                e,
                block,
                endFallbackMap);

        if (!overlapState.empty()) {
            oss << L" / "
                << HtmlEscape_(overlapState);
        }

        oss << L"</div>";

        oss << L"<div class=\"program-title\">";
        oss << HtmlEscape_(e.title);
        oss << L"</div>";

        if (rowspan >= 20 &&
            !e.description.empty()) {
            const std::wstring summary =
                ClipSummary_(
                    e.description,
                    rowspan >= 60 ? 220 : 120);

            if (!summary.empty()) {
                oss << L"<div class=\"program-sub summary\">";
                oss << HtmlEscape_(summary);
                oss << L"</div>";
            }
        }

        const std::wstring genreMajor =
            ExtractGenreMajor_(e.genreKey);

        if (!genreMajor.empty()) {
            oss << L"<div class=\"program-sub genre\">";
            oss << HtmlEscape_(genreMajor);
            oss << L"</div>";
        }

        oss << L"</td>";
    }

    void AppendEmptyCell_(
        std::wostringstream& oss,
        const LiteTableService_& service,
        int rowspan)
    {
        oss << L"<td class=\"empty-cell gap-cell\"";

        if (rowspan > 1) {
            oss << L" rowspan=\""
                << rowspan
                << L"\"";
        }

        oss << L" data-subchannel-item=\"1\"";
        oss << L" data-subchannel=\""
            << (service.isSubchannel ? L"1" : L"0")
            << L"\"></td>";
    }

    EpgHtmlLite::Common::HeaderContext BuildHeader_(
        const std::wstring& title,
        const std::wstring& dateKey,
        int block,
        int space)
    {
        EpgHtmlLite::Common::HeaderContext header{};

        header.title = title;
        header.subtitle =
            DateLabel_(dateKey) +
            L" / 過去番組 / 時間割 / " +
            LiteBlockLabel_(block);

        header.mainLinks =
            EpgHtmlLite::Common::BuildMainMenuLinks(
                space,
                EpgHtmlLite::MenuId::PastPrograms);

        header.broadcastLinks =
            EpgHtmlLite::Common::BuildBroadcastLinks(
                EpgHtmlLite::Common::GuidePageKind::BeforeTimetable,
                space);

        header.guideScopeLinks =
            EpgHtmlLite::Common::BuildGuideScopeLinks(
                space,
                EpgHtmlLite::Common::GuidePageKind::BeforeTimetable);

        header.guideViewLinks =
            EpgHtmlLite::Common::BuildGuideViewLinks(
                space,
                EpgHtmlLite::Common::GuidePageKind::BeforeTimetable);

        header.timeBlockLinks =
            EpgHtmlLite::Common::BuildTimeBlockLinks(
                space,
                EpgHtmlLite::Common::GuidePageKind::BeforeTimetable,
                block);

        header.genreLinks =
            EpgHtmlLite::Common::BuildGenreLinks(
                space,
                EpgHtmlLite::Common::GuidePageKind::BeforeTimetable,
                block,
                L"all");

        header.showMainMenuRow = true;
        header.showGuideControlRow = true;
        header.showGenreRow = true;

        return header;
    }

    std::wstring BuildDateHtml_(
        const EpgHtmlLite::PageBeforeTimetable::DatePageRequest& request)
    {
        const std::wstring title =
            request.pageTitle.empty()
            ? L"EPG Lite 過去番組"
            : request.pageTitle;

        const int block =
            NormalizeBlock_(request.block);

        const LiteBlockMinuteRange_ blockRange =
            GetLiteBlockMinuteRange_(block);

        const int blockMinutes =
            (std::max)(1, blockRange.end - blockRange.begin);

        const LiteEndFallbackMap_ endFallbackMap =
            BuildLiteEndFallbackMap_(
                request.rows,
                block);

        const std::vector<const ProgramEntry*> rows =
            FilterAndSortRows_(
                request.rows,
                block,
                endFallbackMap);

        const std::vector<LiteTableService_> services =
            BuildServices_(
                request,
                rows,
                blockRange,
                endFallbackMap);

        const EpgHtmlLite::Common::HeaderContext header =
            BuildHeader_(
                title,
                request.dateKey,
                block,
                request.space);

        std::wostringstream oss;

        oss << L"<!doctype html>\n";
        oss << L"<html lang=\"ja\">\n";
        oss << L"<head>\n";
        oss << L"<meta charset=\"utf-8\">\n";
        oss << L"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n";
        oss << L"<title>"
            << HtmlEscape_(title)
            << L"</title>\n";
        oss << L"<style>\n";
        oss << EpgHtmlLite::Common::BuildCommonCss();
        AppendTimetableCss_(oss);
        oss << L"</style>\n";
        oss << L"</head>\n";
        oss << L"<body>\n";

        oss << EpgHtmlLite::Common::BuildLayoutBegin(header);

        oss << L"<section class=\"panel\">\n";
        oss << L"<h2>過去番組 時間割</h2>\n";
        oss << L"<div class=\"lite-page-note\">";
        oss << L"日付: "
            << HtmlEscape_(DateLabel_(request.dateKey))
            << L" / 時間帯: "
            << HtmlEscape_(LiteBlockLabel_(block))
            << L" / 件数: "
            << rows.size();
        oss << L"</div>\n";

        if (services.empty()) {
            oss << L"<div class=\"empty-box\">";
            oss << L"対象番組がありません。";
            oss << L"</div>\n";
        }
        else {
            oss << L"<div id=\"genreScope\" class=\"lite-table-wrap\">\n";
            oss << L"<table id=\"liteScrollBody\" class=\"lite-epg-table\">\n";

            oss << L"<thead>\n";
            oss << L"<tr>";
            oss << L"<th class=\"time-col\">時間</th>";

            for (const LiteTableService_& service : services) {
                oss << L"<th class=\"svc-col\"";
                oss << L" data-lite-service-col=\"1\"";
                oss << L" data-subchannel=\""
                    << (service.isSubchannel ? L"1" : L"0")
                    << L"\"";
                oss << L" title=\""
                    << HtmlEscape_(service.name)
                    << L"\">";
                oss << HtmlEscape_(service.name);
                oss << L"</th>";
            }

            oss << L"</tr>\n";
            oss << L"</thead>\n";
            oss << L"<tbody>\n";

            for (int minuteOffset = 0;
                minuteOffset < blockMinutes;
                ++minuteOffset) {
                const int normalizedMinute =
                    blockRange.begin + minuteOffset;

                const bool hourRow =
                    (minuteOffset % 60) == 0;

                const bool tickRow =
                    !hourRow &&
                    (minuteOffset % 5) == 0;

                oss << L"<tr class=\"lite-minute-row";

                if (hourRow) {
                    oss << L" hour-row";
                }
                else if (tickRow) {
                    oss << L" tick-row";
                }

                oss << L"\">";

                oss << L"<th class=\"time-col\">";

                if (hourRow || tickRow) {
                    oss << HtmlEscape_(
                        FormatMinuteOfDayText_(
                            normalizedMinute));
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
                        const int rowspan =
                            index < service.rowspanAt.size()
                            ? service.rowspanAt[index]
                            : 1;

                        AppendProgramCell_(
                            oss,
                            *startProgram,
                            service,
                            rowspan,
                            block,
                            endFallbackMap);
                    }
                    else {
                        const int gapRowspan =
                            index < service.gapRowspanAt.size()
                            ? service.gapRowspanAt[index]
                            : 0;

                        if (gapRowspan > 0) {
                            AppendEmptyCell_(
                                oss,
                                service,
                                gapRowspan);
                        }
                        else {
                            const bool programCovered =
                                index < service.covered.size() &&
                                service.covered[index];

                            const bool gapCovered =
                                index < service.gapCovered.size() &&
                                service.gapCovered[index];

                            if (programCovered || gapCovered) {
                                // rowspan に吸収済みなので出力しない。
                            }
                            else {
                                oss << L"<td class=\"empty-cell\"";
                                oss << L" data-subchannel-item=\"1\"";
                                oss << L" data-subchannel=\""
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

        oss << L"</section>\n";
        oss << EpgHtmlLite::Common::BuildLayoutEnd();

        oss << L"</body>\n";
        oss << L"</html>\n";

        return oss.str();
    }

    std::wstring FileNameFor_(
        int space)
    {
        return EpgHtmlLite::Common::BuildGuidePageFileName(
            EpgHtmlLite::Common::GuidePageKind::BeforeTimetable,
            space);
    }

    std::vector<const ProgramEntry*> CollectRowsForContext_(
        const EpgHtmlLite::Context& ctx)
    {
        std::vector<const ProgramEntry*> rows;

        if (!ctx.sharedData) {
            return rows;
        }

        rows.reserve(ctx.sharedData->entries.size());

        for (const ProgramEntry& e : ctx.sharedData->entries) {
            if (e.space != ctx.space) {
                continue;
            }

            if (!e.isPast) {
                continue;
            }

            if (e.title.empty()) {
                continue;
            }

            rows.push_back(&e);
        }

        return rows;
    }
}

namespace EpgHtmlLite::PageBeforeTimetable
{
    bool GenerateDatePage(
        const DatePageRequest& request,
        Result& ioResult)
    {
        try {
            if (request.outputPath.empty()) {
                return false;
            }

            const std::wstring html =
                BuildDateHtml_(request);

            if (!WriteUtf8File_(
                request.outputPath,
                html)) {
                return false;
            }

            ioResult.generatedFiles.push_back(
                request.outputPath.wstring());

            return true;
        }
        catch (...) {
            return false;
        }
    }

    bool Generate(
        const Context& ctx,
        Result& ioResult)
    {
        try {
            DatePageRequest request{};
            request.outputPath =
                ctx.outputDir / FileNameFor_(ctx.space);

            request.pageTitle =
                ctx.title.empty()
                ? L"EPG Lite 過去番組"
                : ctx.title + L" Lite 過去番組";

            request.dateKey = L"";
            request.block = 0;
            request.space = ctx.space;
            request.rows =
                CollectRowsForContext_(ctx);

            request.resolveServiceName =
                [](const ProgramEntry& e) {
                if (!e.serviceName.empty()) {
                    return e.serviceName;
                }

                return L"SID " +
                    std::to_wstring(
                        static_cast<unsigned int>(e.serviceId));
                };

            request.isSubchannel =
                [](const ProgramEntry&) {
                return false;
                };

            return GenerateDatePage(
                request,
                ioResult);
        }
        catch (...) {
            return false;
        }
    }
}