#include "EpgHtmlLite_PageNowTimetable.hpp"
#include "EpgHtmlLite_Common.hpp"
#include "EpgHtmlLite_ReservationSwitch.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
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
#include <cstdlib>
#include <iterator>
#include <limits>

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

        const std::string::size_type reserveSize =
            static_cast<std::string::size_type>(s.size()) *
            static_cast<std::string::size_type>(3) +
            static_cast<std::string::size_type>(8);

        out.reserve(reserveSize);

        for (std::size_t i = 0; i < s.size(); ++i) {
            std::uint32_t cp =
                static_cast<std::uint32_t>(s[i]);

            if constexpr (sizeof(wchar_t) == 2) {
                if (cp >= 0xD800u &&
                    cp <= 0xDBFFu &&
                    (i + 1) < s.size()) {
                    const std::uint32_t lo =
                        static_cast<std::uint32_t>(s[i + 1]);

                    if (lo >= 0xDC00u &&
                        lo <= 0xDFFFu) {
                        cp =
                            0x10000u +
                            (((cp - 0xD800u) << 10) |
                                (lo - 0xDC00u));
                        ++i;
                    }
                }
            }

            AppendUtf8CodePoint_(out, cp);
        }

        return out;
    }

    bool WriteUtf8File_(
        const std::filesystem::path& path,
        const std::wstring& text)
    {
        std::error_code ec;
        std::filesystem::create_directories(
            path.parent_path(),
            ec);

        if (ec) {
            return false;
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

    std::wstring LiteBlockLabel_(
        int block)
    {
        switch (block) {
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
        switch (block) {
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
            return {};
        }

        minute %= kLiteMinutesPerDay_;

        if (minute < 0) {
            minute += kLiteMinutesPerDay_;
        }

        wchar_t buf[16]{};
        std::swprintf(
            buf,
            static_cast<std::size_t>(sizeof(buf) / sizeof(buf[0])),
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

    bool HasValidEndTime_(
        const ProgramEntry& e)
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
            return {};
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
        return EpgHtmlLite::Common::NormalizeGenreMajorName(
            genreKey);
    }

    struct LiteServiceIdentity_
    {
        std::uint32_t networkId = 0;
        std::uint32_t tsId = 0;
        std::wstring serviceName;
        bool found = false;
    };

    static std::wstring TrimLiteCsvField_(
        std::wstring_view s)
    {
        std::size_t first = 0;
        while (first < s.size()) {
            const wchar_t ch = s[first];
            if (ch != L' ' &&
                ch != L'\t' &&
                ch != L'\r' &&
                ch != L'\n' &&
                ch != L'　') {
                break;
            }
            ++first;
        }

        std::size_t last = s.size();
        while (last > first) {
            const wchar_t ch = s[last - 1];
            if (ch != L' ' &&
                ch != L'\t' &&
                ch != L'\r' &&
                ch != L'\n' &&
                ch != L'　') {
                break;
            }
            --last;
        }

        return std::wstring(s.substr(first, last - first));
    }

    static std::vector<std::wstring> SplitLiteCsvLine_(
        std::wstring_view line)
    {
        std::vector<std::wstring> fields;
        std::wstring current;
        bool inQuote = false;

        for (const wchar_t ch : line) {
            if (ch == L'"') {
                inQuote = !inQuote;
                continue;
            }

            if (ch == L',' && !inQuote) {
                fields.push_back(TrimLiteCsvField_(current));
                current.clear();
                continue;
            }

            current.push_back(ch);
        }

        fields.push_back(TrimLiteCsvField_(current));
        return fields;
    }

    static bool TryParseLiteUInt_(
        const std::wstring& s,
        std::uint32_t& out) noexcept
    {
        out = 0;

        const std::wstring v =
            TrimLiteCsvField_(s);

        if (v.empty()) {
            return false;
        }

        wchar_t* end = nullptr;

        const unsigned long parsed =
            std::wcstoul(
                v.c_str(),
                &end,
                0);

        if (end == v.c_str()) {
            return false;
        }

        while (end && *end != L'\0') {
            const wchar_t ch = *end;

            if (ch != L' ' &&
                ch != L'\t' &&
                ch != L'\r' &&
                ch != L'\n' &&
                ch != L'　') {
                return false;
            }

            ++end;
        }

        if (parsed >
            static_cast<unsigned long>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            return false;
        }

        out = static_cast<std::uint32_t>(parsed);
        return true;
    }

    static bool ReadLiteTextFileWide_(
        const std::filesystem::path& path,
        std::wstring& out)
    {
        out.clear();

        std::ifstream ifs(
            path,
            std::ios::binary);

        if (!ifs) {
            return false;
        }

        std::string bytes(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());

        if (bytes.empty()) {
            return true;
        }

        const auto b0 =
            static_cast<unsigned char>(bytes.size() > 0 ? bytes[0] : 0);

        const auto b1 =
            static_cast<unsigned char>(bytes.size() > 1 ? bytes[1] : 0);

        const auto b2 =
            static_cast<unsigned char>(bytes.size() > 2 ? bytes[2] : 0);

        if (bytes.size() >= 2 &&
            b0 == 0xFF &&
            b1 == 0xFE) {
            for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
                const auto lo =
                    static_cast<unsigned char>(bytes[i]);

                const auto hi =
                    static_cast<unsigned char>(bytes[i + 1]);

                const wchar_t ch =
                    static_cast<wchar_t>(
                        static_cast<unsigned>(lo) |
                        (static_cast<unsigned>(hi) << 8));

                out.push_back(ch);
            }

            return true;
        }

        std::size_t offset = 0;

        if (bytes.size() >= 3 &&
            b0 == 0xEF &&
            b1 == 0xBB &&
            b2 == 0xBF) {
            offset = 3;
        }

        out.reserve(bytes.size() - offset);

        for (std::size_t i = offset; i < bytes.size(); ++i) {
            const unsigned char c =
                static_cast<unsigned char>(bytes[i]);

            out.push_back(static_cast<wchar_t>(c));
        }

        return true;
    }

    static bool TryReadLiteServiceIdentityLayout_(
        const std::vector<std::wstring>& fields,
        int space,
        std::uint32_t serviceId,
        std::size_t serviceIdIndex,
        std::size_t networkIdIndex,
        std::size_t tsIdIndex,
        LiteServiceIdentity_& out)
    {
        std::size_t maxIndex = serviceIdIndex;
        if (networkIdIndex > maxIndex) {
            maxIndex = networkIdIndex;
        }
        if (tsIdIndex > maxIndex) {
            maxIndex = tsIdIndex;
        }

        if (fields.size() <= maxIndex) {
            return false;
        }

        if (fields.size() > 1) {
            std::uint32_t parsedSpace = 0;
            if (TryParseLiteUInt_(fields[1], parsedSpace) &&
                static_cast<int>(parsedSpace) != space) {
                return false;
            }
        }

        std::uint32_t sid = 0;
        if (!TryParseLiteUInt_(fields[serviceIdIndex], sid)) {
            return false;
        }

        if (sid != serviceId) {
            return false;
        }

        std::uint32_t nid = 0;
        std::uint32_t tsid = 0;

        if (!TryParseLiteUInt_(fields[networkIdIndex], nid) ||
            !TryParseLiteUInt_(fields[tsIdIndex], tsid)) {
            return false;
        }

        out.networkId = nid;
        out.tsId = tsid;
        out.serviceName =
            !fields.empty()
            ? fields[0]
            : std::wstring{};

        out.found = true;
        return true;
    }

    static bool TryReadLiteServiceIdentityFromLine_(
        const std::wstring& line,
        int space,
        std::uint32_t serviceId,
        LiteServiceIdentity_& out)
    {
        const std::wstring trimmed =
            TrimLiteCsvField_(line);

        if (trimmed.empty()) {
            return false;
        }

        const wchar_t first = trimmed.front();
        if (first == L'#' || first == L';') {
            return false;
        }

        const std::vector<std::wstring> fields =
            SplitLiteCsvLine_(trimmed);

        // 現行想定:
        // serviceName, space, physicalCh, remote, ..., serviceId, networkId, tsId, ...
        if (TryReadLiteServiceIdentityLayout_(
            fields,
            space,
            serviceId,
            5,
            6,
            7,
            out)) {
            return true;
        }

        // 旧/別形式想定:
        // serviceName, space, physicalCh, ..., serviceId, networkId, tsId, ...
        if (TryReadLiteServiceIdentityLayout_(
            fields,
            space,
            serviceId,
            4,
            5,
            6,
            out)) {
            return true;
        }

        return false;
    }

    static std::vector<std::filesystem::path> BuildLiteChannelListCandidates_(
        int space)
    {
        std::vector<std::filesystem::path> paths;

        std::error_code ec;
        const std::filesystem::path cur =
            std::filesystem::current_path(ec);

        if (!ec && !cur.empty()) {
            paths.push_back(
                cur /
                (L"channellist_space" +
                    std::to_wstring(space) +
                    L".ch"));

            paths.push_back(
                cur /
                (L"channellist_space" +
                    std::to_wstring(space) +
                    L".txt"));

            paths.push_back(cur / L"channels.txt");
            paths.push_back(cur / L"channels.ini");
        }

        return paths;
    }

    static LiteServiceIdentity_ ResolveLiteServiceIdentity_(
        int space,
        std::uint32_t serviceId)
    {
        static std::map<int, std::map<std::uint32_t, LiteServiceIdentity_>> cache;

        auto& spaceMap = cache[space];

        const auto cached =
            spaceMap.find(serviceId);

        if (cached != spaceMap.end()) {
            return cached->second;
        }

        LiteServiceIdentity_ result{};

        const std::vector<std::filesystem::path> candidates =
            BuildLiteChannelListCandidates_(space);

        for (const auto& path : candidates) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec) || ec) {
                continue;
            }

            std::wstring text;
            if (!ReadLiteTextFileWide_(path, text)) {
                continue;
            }

            std::wistringstream iss(text);
            std::wstring line;

            while (std::getline(iss, line)) {
                LiteServiceIdentity_ found{};
                if (TryReadLiteServiceIdentityFromLine_(
                    line,
                    space,
                    serviceId,
                    found)) {
                    spaceMap[serviceId] = found;
                    return found;
                }
            }
        }

        spaceMap[serviceId] = result;
        return result;
    }

    static LiteServiceIdentity_ ResolveLiteServiceIdentity_(
        const ProgramEntry& e)
    {
        return ResolveLiteServiceIdentity_(
            e.space,
            static_cast<std::uint32_t>(e.serviceId));
    }

    static std::wstring BuildLiteServiceKey_(
        const ProgramEntry& e,
        const LiteServiceIdentity_& id)
    {
        std::wstring key;

        key += L"s";
        key += std::to_wstring(e.space);
        key += L"_n";
        key += std::to_wstring(static_cast<unsigned int>(id.networkId));
        key += L"_t";
        key += std::to_wstring(static_cast<unsigned int>(id.tsId));
        key += L"_sid";
        key += std::to_wstring(static_cast<unsigned int>(e.serviceId));

        return key;
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

    static std::wstring LiteIsoDateToSlashDate_(
        const std::wstring& s)
    {
        if (s.size() >= 10 &&
            s[4] == L'-' &&
            s[7] == L'-') {
            return s.substr(0, 4) +
                L"/" +
                s.substr(5, 2) +
                L"/" +
                s.substr(8, 2);
        }

        return s;
    }

    static std::wstring LiteEnsureHms_(
        const std::wstring& s)
    {
        if (s.size() == 5 &&
            s[2] == L':') {
            return s + L":00";
        }

        return s;
    }

    static std::wstring LiteTrimSecond_(
        const std::wstring& s)
    {
        if (s.size() >= 5) {
            return s.substr(0, 5);
        }

        return s;
    }

    static std::wstring LitePickStartDate_(
        const ProgramEntry& e)
    {
        if (!e.startDate.empty()) {
            return e.startDate;
        }

        if (e.broadcastDateKey.size() == 8) {
            return e.broadcastDateKey.substr(0, 4) +
                L"-" +
                e.broadcastDateKey.substr(4, 2) +
                L"-" +
                e.broadcastDateKey.substr(6, 2);
        }

        return {};
    }

    static std::wstring LitePickEndDate_(
        const ProgramEntry& e)
    {
        if (!e.endDate.empty()) {
            return e.endDate;
        }

        return LitePickStartDate_(e);
    }

    static std::wstring LitePickStartTimeHms_(
        const ProgramEntry& e)
    {
        if (!e.startTime.empty()) {
            return LiteEnsureHms_(e.startTime);
        }

        if (e.startMinutesOfDay >= 0) {
            return FormatMinuteOfDayText_(
                e.startMinutesOfDay) +
                L":00";
        }

        return LiteEnsureHms_(e.startText);
    }

    static std::wstring LitePickEndTimeHms_(
        const ProgramEntry& e)
    {
        if (!e.endTime.empty()) {
            return LiteEnsureHms_(e.endTime);
        }

        if (e.endMinutesOfDay >= 0) {
            return FormatMinuteOfDayText_(
                e.endMinutesOfDay) +
                L":00";
        }

        return LiteEnsureHms_(e.endText);
    }

    static std::wstring LiteTimeRangeText_(
        const ProgramEntry& e)
    {
        const std::wstring start =
            LiteTrimSecond_(
                LitePickStartTimeHms_(e));

        const std::wstring end =
            LiteTrimSecond_(
                LitePickEndTimeHms_(e));

        if (!start.empty() && !end.empty()) {
            return start + L"-" + end;
        }

        if (!start.empty()) {
            return start;
        }

        return {};
    }

    static std::uint32_t LiteDurationSecOf_(
        const ProgramEntry& e) noexcept
    {
        if (e.startMinutesOfDay < 0 ||
            e.endMinutesOfDay < 0) {
            return 0;
        }

        int begin = e.startMinutesOfDay;
        int end = e.endMinutesOfDay;

        if (end <= begin) {
            end += 24 * 60;
        }

        if (end <= begin) {
            return 0;
        }

        return static_cast<std::uint32_t>(
            (end - begin) * 60);
    }

    static bool IsLiteLeapYear_(
        int year) noexcept
    {
        return ((year % 4) == 0 && (year % 100) != 0) ||
            ((year % 400) == 0);
    }

    static int GetLiteMonthDays_(
        int year,
        int month) noexcept
    {
        static constexpr int kDays[] = {
            31, 28, 31, 30, 31, 30,
            31, 31, 30, 31, 30, 31
        };

        if (month < 1 || month > 12) {
            return 0;
        }

        if (month == 2 && IsLiteLeapYear_(year)) {
            return 29;
        }

        return kDays[month - 1];
    }

    static bool TryParseLiteIsoDate_(
        const std::wstring& s,
        int& year,
        int& month,
        int& day) noexcept
    {
        year = 0;
        month = 0;
        day = 0;

        if (s.size() < 10 ||
            s[4] != L'-' ||
            s[7] != L'-') {
            return false;
        }

        auto digit = [](wchar_t ch) noexcept {
            return ch >= L'0' && ch <= L'9';
            };

        for (std::size_t i : { 0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u }) {
            if (!digit(s[i])) {
                return false;
            }
        }

        year =
            static_cast<int>((s[0] - L'0') * 1000 +
                (s[1] - L'0') * 100 +
                (s[2] - L'0') * 10 +
                (s[3] - L'0'));

        month =
            static_cast<int>((s[5] - L'0') * 10 +
                (s[6] - L'0'));

        day =
            static_cast<int>((s[8] - L'0') * 10 +
                (s[9] - L'0'));

        const int mdays =
            GetLiteMonthDays_(year, month);

        return year >= 1900 &&
            month >= 1 &&
            month <= 12 &&
            day >= 1 &&
            day <= mdays;
    }

    static std::wstring FormatLiteIsoDate_(
        int year,
        int month,
        int day)
    {
        wchar_t buf[16]{};

        std::swprintf(
            buf,
            static_cast<std::size_t>(sizeof(buf) / sizeof(buf[0])),
            L"%04d-%02d-%02d",
            year,
            month,
            day);

        return std::wstring(buf);
    }

    static std::wstring ShiftLiteIsoDateByDays_(
        const std::wstring& isoDate,
        int deltaDays)
    {
        int year = 0;
        int month = 0;
        int day = 0;

        if (!TryParseLiteIsoDate_(
            isoDate,
            year,
            month,
            day)) {
            return {};
        }

        int remain = deltaDays;

        while (remain > 0) {
            const int mdays =
                GetLiteMonthDays_(year, month);

            if (day < mdays) {
                ++day;
            }
            else {
                day = 1;
                ++month;

                if (month > 12) {
                    month = 1;
                    ++year;
                }
            }

            --remain;
        }

        while (remain < 0) {
            if (day > 1) {
                --day;
            }
            else {
                --month;

                if (month < 1) {
                    month = 12;
                    --year;
                }

                day = GetLiteMonthDays_(year, month);
            }

            ++remain;
        }

        return FormatLiteIsoDate_(
            year,
            month,
            day);
    }

    static ProgramEntry MakeReservationSwitchEntry_(
        const ProgramEntry& e,
        const LiteEndFallbackMap_& endFallbackMap)
    {
        ProgramEntry out = e;

        const int begin =
            PickLiteStartBroadcastMinute_(e);

        const int end =
            PickLiteEndBroadcastMinute_(
                e,
                endFallbackMap);

        if (begin < 0 ||
            end <= begin) {
            return out;
        }

        int endMinute =
            end % kLiteMinutesPerDay_;

        if (endMinute < 0) {
            endMinute += kLiteMinutesPerDay_;
        }

        out.endMinutesOfDay = endMinute;
        out.endTime =
            FormatMinuteOfDayText_(endMinute) +
            L":00";

        const std::wstring startDate =
            LitePickStartDate_(out);

        if (!startDate.empty()) {
            if (begin < kLiteMinutesPerDay_ &&
                end >= kLiteMinutesPerDay_) {
                const std::wstring shifted =
                    ShiftLiteIsoDateByDays_(
                        startDate,
                        1);

                out.endDate =
                    !shifted.empty()
                    ? shifted
                    : startDate;
            }
            else {
                out.endDate = startDate;
            }
        }

        return out;
    }

    static std::wstring LiteMakeProgramDedupeKey_(
        const ProgramEntry& e)
    {
        const LiteServiceIdentity_ id =
            ResolveLiteServiceIdentity_(e);

        std::wstring key;

        key += std::to_wstring(e.space);
        key += L"-";
        key += std::to_wstring(static_cast<unsigned int>(id.networkId));
        key += L"-";
        key += std::to_wstring(static_cast<unsigned int>(id.tsId));
        key += L"-";
        key += std::to_wstring(
            static_cast<unsigned int>(e.serviceId));
        key += L"-";
        key += std::to_wstring(
            static_cast<unsigned int>(e.eventId));
        key += L"-";
        key += LitePickStartDate_(e);
        key += L"-";
        key += LitePickStartTimeHms_(e);
        key += L"-record";

        return key;
    }

    static void AppendLiteReservationPayloadAttrs_(
        std::wostringstream& oss,
        const ProgramEntry& e)
    {
        const LiteServiceIdentity_ identity =
            ResolveLiteServiceIdentity_(e);

        const std::wstring startDate =
            LitePickStartDate_(e);

        const std::wstring endDate =
            LitePickEndDate_(e);

        const std::wstring startTime =
            LitePickStartTimeHms_(e);

        const std::wstring endTime =
            LitePickEndTimeHms_(e);

        const std::wstring dateText =
            LiteIsoDateToSlashDate_(startDate);

        const std::wstring timeText =
            LiteTimeRangeText_(e);

        const std::wstring serviceName =
            !e.serviceName.empty()
            ? e.serviceName
            : (!identity.serviceName.empty()
                ? identity.serviceName
                : (L"SID " +
                    std::to_wstring(
                        static_cast<unsigned int>(e.serviceId))));

        const std::wstring serviceKey =
            BuildLiteServiceKey_(e, identity);

        oss << L" data-rsv-mode=\"record\"";

        oss << L" data-rsv-space=\""
            << e.space
            << L"\"";

        oss << L" data-rsv-network-id=\""
            << static_cast<unsigned int>(identity.networkId)
            << L"\"";

        oss << L" data-rsv-ts-id=\""
            << static_cast<unsigned int>(identity.tsId)
            << L"\"";

        oss << L" data-rsv-service-id=\""
            << static_cast<unsigned int>(e.serviceId)
            << L"\"";

        oss << L" data-rsv-event-id=\""
            << static_cast<unsigned int>(e.eventId)
            << L"\"";

        oss << L" data-rsv-service-key=\""
            << HtmlEscape_(serviceKey)
            << L"\"";

        oss << L" data-rsv-service-name=\""
            << HtmlEscape_(serviceName)
            << L"\"";

        oss << L" data-rsv-title=\""
            << HtmlEscape_(e.title)
            << L"\"";

        oss << L" data-rsv-summary=\""
            << HtmlEscape_(e.description)
            << L"\"";

        oss << L" data-rsv-genre=\""
            << HtmlEscape_(
                ExtractGenreMajor_(e.genreKey))
            << L"\"";

        oss << L" data-rsv-date=\""
            << HtmlEscape_(dateText)
            << L"\"";

        oss << L" data-rsv-start-date=\""
            << HtmlEscape_(startDate)
            << L"\"";

        oss << L" data-rsv-start-time=\""
            << HtmlEscape_(startTime)
            << L"\"";

        oss << L" data-rsv-end-date=\""
            << HtmlEscape_(endDate)
            << L"\"";

        oss << L" data-rsv-end-time=\""
            << HtmlEscape_(endTime)
            << L"\"";

        oss << L" data-rsv-time-text=\""
            << HtmlEscape_(timeText)
            << L"\"";

        oss << L" data-rsv-duration-sec=\""
            << static_cast<unsigned int>(
                LiteDurationSecOf_(e))
            << L"\"";

        oss << L" data-rsv-dedupe-key=\""
            << HtmlEscape_(
                LiteMakeProgramDedupeKey_(e))
            << L"\"";
    }

    static void AppendLiteReservationSwitch_(
        std::wostringstream& oss,
        const ProgramEntry& e)
    {
        if (e.isPast) {
            return;
        }

        if (e.title.empty() ||
            e.serviceId == 0 ||
            e.eventId == 0 ||
            LitePickStartDate_(e).empty() ||
            LitePickStartTimeHms_(e).empty()) {
            return;
        }

        oss << L"<div class=\"lite-rsv-box\" data-lite-rsv-box=\"1\"";
        AppendLiteReservationPayloadAttrs_(oss, e);
        oss << L">";

        oss << L"<button type=\"button\" class=\"lite-rsv-btn rsv-add\"";
        oss << L" data-lite-rsv-action=\"upsert\">";
        oss << L"録画予約";
        oss << L"</button>";

        oss << L"<button type=\"button\" class=\"lite-rsv-btn rsv-disable\"";
        oss << L" data-lite-rsv-action=\"disable\" hidden>";
        oss << L"無効化";
        oss << L"</button>";

        oss << L"<button type=\"button\" class=\"lite-rsv-btn rsv-enable\"";
        oss << L" data-lite-rsv-action=\"enable\" hidden>";
        oss << L"有効化";
        oss << L"</button>";

        oss << L"<button type=\"button\" class=\"lite-rsv-btn rsv-remove\"";
        oss << L" data-lite-rsv-action=\"remove\" hidden>";
        oss << L"削除";
        oss << L"</button>";

        oss << L"<span class=\"lite-rsv-msg\" data-lite-rsv-msg=\"1\"></span>";

        oss << L"</div>";
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

            const long long spanWide =
                static_cast<long long>(cellEnd) -
                static_cast<long long>(cellBegin);

            int span = 1;

            if (spanWide > 1) {
                if (spanWide >
                    static_cast<long long>(
                        (std::numeric_limits<int>::max)())) {
                    span = (std::numeric_limits<int>::max)();
                }
                else {
                    span = static_cast<int>(spanWide);
                }
            }

            service.rowspanAt[
                static_cast<std::size_t>(cellBegin)] =
                span;

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

    bool LooksLikeLiteSubchannelName_(
        std::wstring_view name)
    {
        std::wstring compact;
        compact.reserve(name.size());

        for (wchar_t ch : name) {
            if (ch == L' ' ||
                ch == L'\t' ||
                ch == L'\r' ||
                ch == L'\n' ||
                ch == L'　' ||
                ch == L'-' ||
                ch == L'－' ||
                ch == L'_') {
                continue;
            }

            compact.push_back(ch);
        }

        if (compact.empty()) {
            return false;
        }

        if (compact.find(L"サブ") != std::wstring::npos ||
            compact.find(L"ワンセグ") != std::wstring::npos) {
            return true;
        }

        // BS12 などを誤判定しない保険。
        if (compact.rfind(L"BS", 0) == 0 ||
            compact.rfind(L"ＢＳ", 0) == 0) {
            return false;
        }

        const wchar_t last =
            compact.back();

        if (last == L'2' ||
            last == L'3' ||
            last == L'２' ||
            last == L'３') {
            return true;
        }

        return false;
    }

    std::wstring NormalizeLiteServiceNameForSubchannel_(
        std::wstring_view name)
    {
        std::wstring compact;
        compact.reserve(name.size());

        for (wchar_t ch : name) {
            if (ch == L' ' ||
                ch == L'\t' ||
                ch == L'\r' ||
                ch == L'\n' ||
                ch == L'　' ||
                ch == L'-' ||
                ch == L'－' ||
                ch == L'_') {
                continue;
            }

            compact.push_back(ch);
        }

        return compact;
    }

    void MarkDuplicateServiceNamesAsSubchannel_(
        std::vector<LiteTableService_>& services)
    {
        std::map<std::wstring, int> seenCount;

        for (LiteTableService_& service : services) {
            const std::wstring key =
                NormalizeLiteServiceNameForSubchannel_(
                    service.name);

            if (key.empty()) {
                continue;
            }

            const int seen =
                seenCount[key];

            if (seen > 0) {
                service.isSubchannel = true;
            }

            seenCount[key] = seen + 1;
        }
    }

    std::wstring ResolveServiceName_(
        const EpgHtmlLite::PageNowTimetable::BlockPageRequest& request,
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
        const EpgHtmlLite::PageNowTimetable::BlockPageRequest& request,
        const ProgramEntry& e)
    {
        if (request.isSubchannel &&
            request.isSubchannel(e)) {
            return true;
        }

        if (LooksLikeLiteSubchannelName_(e.serviceName)) {
            return true;
        }

        if (request.resolveServiceName) {
            const std::wstring resolvedName =
                request.resolveServiceName(e);

            if (LooksLikeLiteSubchannelName_(resolvedName)) {
                return true;
            }
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

        oss << L".lite-auto-reserve-assist{\n";
        oss << L"  display:inline-block;\n";
        oss << L"  margin-top:5px;\n";
        oss << L"  color:#9dc5ff;\n";
        oss << L"  font-size:10px;\n";
        oss << L"  font-weight:700;\n";
        oss << L"  line-height:1.3;\n";
        oss << L"  text-decoration:none;\n";
        oss << L"}\n";

        oss << L".lite-auto-reserve-assist:hover{\n";
        oss << L"  color:#d6e8ff;\n";
        oss << L"  text-decoration:underline;\n";
        oss << L"}\n";

        oss << L".lite-epg-table td.compact-cell .program-sub.summary{\n";
        oss << L"  display:none;\n";
        oss << L"}\n";

        // 時間割のサブch表示切替。
// JS が .subchannel-hidden を付けるので、table側でも明示的に消す。
        oss << L".lite-epg-table th.subchannel-hidden,\n";
        oss << L".lite-epg-table td.subchannel-hidden{\n";
        oss << L"  display:none!important;\n";
        oss << L"}\n";

        oss << L".lite-epg-table .subchannel-hidden{\n";
        oss << L"  display:none!important;\n";
        oss << L"}\n";

        oss << L".lite-page-note{\n";
        oss << L"  color:#aab6c8;\n";
        oss << L"  font-size:12px;\n";
        oss << L"  margin:0 0 8px;\n";
        oss << L"}\n";

        // table + rowspan では display:none にすると表が崩れやすい。
        // 時間割ではジャンル対象外を薄くするだけにする。
        oss << L".lite-epg-table td.genre-hidden{\n";
        oss << L"  display:table-cell!important;\n";
        oss << L"  opacity:.18!important;\n";
        oss << L"}\n";

        oss << L".lite-epg-table td.genre-hidden:hover{\n";
        oss << L"  opacity:.35!important;\n";
        oss << L"}\n";
    }

    void AppendLiteReservationSwitchCss_(
        std::wostringstream& oss)
    {
        oss << L".lite-rsv-box{\n";
        oss << L"  margin-top:6px;\n";
        oss << L"  display:flex;\n";
        oss << L"  align-items:center;\n";
        oss << L"  gap:5px;\n";
        oss << L"  flex-wrap:wrap;\n";
        oss << L"}\n";

        oss << L".lite-rsv-btn{\n";
        oss << L"  appearance:none;\n";
        oss << L"  border:1px solid #4b5f7f;\n";
        oss << L"  background:#1c2b3f;\n";
        oss << L"  color:#e8f1ff;\n";
        oss << L"  border-radius:999px;\n";
        oss << L"  padding:3px 8px;\n";
        oss << L"  font-size:11px;\n";
        oss << L"  font-weight:800;\n";
        oss << L"  line-height:1.25;\n";
        oss << L"  cursor:pointer;\n";
        oss << L"  white-space:nowrap;\n";
        oss << L"}\n";

        oss << L".lite-rsv-btn:hover{\n";
        oss << L"  border-color:#7ebaff;\n";
        oss << L"}\n";

        oss << L".lite-rsv-btn.busy{\n";
        oss << L"  opacity:0.55;\n";
        oss << L"  cursor:wait;\n";
        oss << L"}\n";

        oss << L".lite-rsv-btn.rsv-expired{\n";
        oss << L"  background:#333842;\n";
        oss << L"  border-color:#606a78;\n";
        oss << L"  color:#b9c3d0;\n";
        oss << L"  cursor:default;\n";
        oss << L"}\n";

        oss << L".lite-rsv-btn:disabled{\n";
        oss << L"  opacity:0.55;\n";
        oss << L"  cursor:default;\n";
        oss << L"}\n";

        oss << L".lite-rsv-box.expired .lite-rsv-msg{\n";
        oss << L"  color:#c0c7d2;\n";
        oss << L"}\n";

        oss << L".lite-rsv-btn.rsv-add{\n";
        oss << L"  background:#1c2b3f;\n";
        oss << L"  border-color:#4b5f7f;\n";
        oss << L"}\n";

        oss << L".lite-rsv-btn.rsv-disable{\n";
        oss << L"  background:#664526;\n";
        oss << L"  border-color:#c48b3a;\n";
        oss << L"}\n";

        oss << L".lite-rsv-btn.rsv-enable{\n";
        oss << L"  background:#124e35;\n";
        oss << L"  border-color:#28a36b;\n";
        oss << L"}\n";

        oss << L".lite-rsv-btn.rsv-remove{\n";
        oss << L"  background:#6b2525;\n";
        oss << L"  border-color:#c45b5b;\n";
        oss << L"}\n";

        oss << L".lite-rsv-box [hidden]{\n";
        oss << L"  display:none!important;\n";
        oss << L"}\n";

        oss << L".lite-rsv-msg{\n";
        oss << L"  color:#c7d2e6;\n";
        oss << L"  font-size:10px;\n";
        oss << L"  line-height:1.25;\n";
        oss << L"}\n";

        oss << L".lite-rsv-msg.error{\n";
        oss << L"  color:#ffb0b0;\n";
        oss << L"}\n";

        oss << L".lite-epg-table td.prog-cell.reserved-record{\n";
        oss << L"  background:#5a2028!important;\n";
        oss << L"  border-color:#d45d6e!important;\n";
        oss << L"  box-shadow:inset 0 0 0 2px rgba(255,95,112,0.55);\n";
        oss << L"}\n";

        oss << L".lite-epg-table td.prog-cell.reserved-record:hover{\n";
        oss << L"  background:#722b35!important;\n";
        oss << L"}\n";

        oss << L".lite-epg-table td.prog-cell.reserved-disabled{\n";
        oss << L"  background:#54401b!important;\n";
        oss << L"  border-color:#d2a538!important;\n";
        oss << L"  box-shadow:inset 0 0 0 2px rgba(210,165,56,0.55);\n";
        oss << L"}\n";

        oss << L".lite-epg-table td.prog-cell.reserved-disabled:hover{\n";
        oss << L"  background:#6a5121!important;\n";
        oss << L"}\n";

        oss << L".lite-epg-table td.prog-cell.reserved-record .program-title,\n";
        oss << L".lite-epg-table td.prog-cell.reserved-disabled .program-title{\n";
        oss << L"  color:#ffffff;\n";
        oss << L"}\n";
    }

    const std::array<std::wstring_view, 12>& LiteGenreMajorList_()
    {
        return EpgHtmlLite::Common::GetGenreMajorNames();
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
        oss << L"  background:#111;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .lite-shell{\n";
        oss << L"  height:100vh;\n";
        oss << L"  min-height:0;\n";
        oss << L"  display:flex;\n";
        oss << L"  flex-direction:column;\n";
        oss << L"}\n";

        // Commonメニューは root側が担当する。
        // iframe内では SplitGuide 旧方式のサブch/ジャンルだけ出す。
        oss << L"body.lite-frame-page .lite-shell-header{\n";
        oss << L"  display:none!important;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .lite-shell-main{\n";
        oss << L"  flex:1 1 auto;\n";
        oss << L"  min-height:0;\n";
        oss << L"  display:flex;\n";
        oss << L"  flex-direction:column;\n";
        oss << L"  overflow:hidden;\n";
        oss << L"  padding:8px 10px;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .lite-content-frame{\n";
        oss << L"  flex:1 1 auto;\n";
        oss << L"  min-height:0;\n";
        oss << L"  display:flex;\n";
        oss << L"  flex-direction:column;\n";
        oss << L"  overflow:hidden;\n";
        oss << L"  padding:0;\n";
        oss << L"  border:0;\n";
        oss << L"  border-radius:0;\n";
        oss << L"  background:#111;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .panel{\n";
        oss << L"  flex:1 1 auto;\n";
        oss << L"  min-height:0;\n";
        oss << L"  display:flex;\n";
        oss << L"  flex-direction:column;\n";
        oss << L"  overflow:hidden;\n";
        oss << L"  margin:6px 0 0;\n";
        oss << L"  border:1px solid #303846;\n";
        oss << L"  border-radius:10px;\n";
        oss << L"  background:#151b24;\n";
        oss << L"  padding:12px 16px;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .panel h2{\n";
        oss << L"  flex:0 0 auto;\n";
        oss << L"  margin:0 0 10px;\n";
        oss << L"  font-size:22px;\n";
        oss << L"  color:#f3f7ff;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .lite-page-note{\n";
        oss << L"  flex:0 0 auto;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .lite-list-wrap,\n";
        oss << L"body.lite-frame-page .lite-table-wrap{\n";
        oss << L"  flex:1 1 auto;\n";
        oss << L"  min-height:0;\n";
        oss << L"  overflow:auto;\n";
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
        oss << L"  min-width:52px;\n";
        oss << L"  font-size:12px;\n";
        oss << L"  color:#aab6c8;\n";
        oss << L"  font-weight:bold;\n";
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
        oss << L"  border-radius:0;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .genre-btn:hover,\n";
        oss << L"body.lite-frame-page .subch-btn:hover{\n";
        oss << L"  background:#2a2a2a;\n";
        oss << L"  border-color:#777;\n";
        oss << L"}\n";

        oss << L"body.lite-frame-page .genre-btn.active,\n";
        oss << L"body.lite-frame-page .subch-btn.active{\n";
        oss << L"  background:#345;\n";
        oss << L"  border-color:#69c;\n";
        oss << L"  color:#fff;\n";
        oss << L"}\n";
    }

    std::vector<LiteTableService_> BuildServices_(
        const EpgHtmlLite::PageNowTimetable::BlockPageRequest& request,
        const LiteBlockMinuteRange_& blockRange,
        const LiteEndFallbackMap_& endFallbackMap)
    {
        std::map<std::uint16_t, std::vector<const ProgramEntry*>> serviceMap;

        for (const ProgramEntry* p : request.rows) {
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

        // フジテレビ / フジテレビ / フジテレビ のように、
        // サービス名だけではサブch番号が出ない局がある。
        // 同名サービスの2個目以降はサブchとして扱う。
        MarkDuplicateServiceNamesAsSubchannel_(services);

        return services;
    }

    void AppendProgramCell_(
        std::wostringstream& oss,
        const ProgramEntry& e,
        const LiteTableService_& service,
        int rowspan,
        int block,
        const LiteEndFallbackMap_& endFallbackMap,
        const bool enableAutoReserveAssist,
        const std::wstring_view autoReserveEntryUrl,
        const int space)
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

        const ProgramEntry rsvEntry =
            MakeReservationSwitchEntry_(
                e,
                endFallbackMap);

        EpgHtmlLite::ReservationSwitch::AppendProgramSwitch(
            oss,
            rsvEntry);

        if (enableAutoReserveAssist &&
            !e.title.empty() &&
            !e.isPast) {
            const std::wstring assistUrl =
                EpgHtmlLite::Common::BuildAutoReserveEntryUrl(
                    e.title,
                    autoReserveEntryUrl,
                    space);

            oss << L"<a class=\"lite-auto-reserve-assist\" href=\""
                << HtmlEscape_(assistUrl)
                << L"\" target=\"_top\">この番組名で自動予約</a>";
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

    static std::wstring BuildLiteReservationSwitchScript_()
    {
        std::wstring js;

        constexpr std::wstring::size_type kLiteReservationSwitchScriptReserve =
            static_cast<std::wstring::size_type>(64) *
            static_cast<std::wstring::size_type>(1024);

        js.reserve(kLiteReservationSwitchScriptReserve);

        js += L"(function(){\n";
        js += L"'use strict';\n";
        js += L"const VIEW_CACHE_URL='/cache/reservation/view/tasks_view_index.tsv';\n";
        js += L"const API_BASE='/api/reservation';\n";
        js += L"let gRows=[];\n";
        js += L"let gMap=new Map();\n";

        js += L"function isDebug(){\n";
        js += L"  try{return localStorage.getItem('epgLiteRsvDebug')==='1'||location.search.indexOf('rsvDebug=1')>=0;}catch(_){return false;}\n";
        js += L"}\n";

        js += L"function log(){\n";
        js += L"  if(!isDebug())return;\n";
        js += L"  try{console.log.apply(console,['[lite-rsv]'].concat(Array.prototype.slice.call(arguments)));}catch(_){}\n";
        js += L"}\n";

        js += L"function warn(){\n";
        js += L"  if(!isDebug())return;\n";
        js += L"  try{console.warn.apply(console,['[lite-rsv]'].concat(Array.prototype.slice.call(arguments)));}catch(_){}\n";
        js += L"}\n";

        js += L"function text(v){return String(v==null?'':v);}\n";
        js += L"function intv(v,d){const n=Number(v);return Number.isFinite(n)?Math.trunc(n):d;}\n";
        js += L"function boolv(v){if(v===true)return true;if(v===false)return false;const s=text(v).toLowerCase().trim();return s==='1'||s==='true'||s==='yes'||s==='on';}\n";
        js += L"function modeOf(v){const s=text(v).toLowerCase().trim();if(s==='watch')return 'watch';return 'record';}\n";

        js += L"function splitTsvLine(line){return text(line).split('\\t');}\n";

        js += L"function parseTsv(src){\n";
        js += L"  const lines=text(src).replace(/^\\uFEFF/,'').split(/\\r?\\n/);\n";
        js += L"  let header=null;\n";
        js += L"  const rows=[];\n";
        js += L"  for(let i=0;i<lines.length;++i){\n";
        js += L"    const line=lines[i];\n";
        js += L"    if(!line||/^\\s*$/.test(line)||line.charAt(0)==='#')continue;\n";
        js += L"    const cols=splitTsvLine(line);\n";
        js += L"    if(!header){header=cols.map(v=>text(v).trim());continue;}\n";
        js += L"    const row={};\n";
        js += L"    for(let c=0;c<header.length;++c){row[header[c]]=c<cols.length?cols[c]:'';}\n";
        js += L"    rows.push(row);\n";
        js += L"  }\n";
        js += L"  return rows;\n";
        js += L"}\n";

        js += L"function makeEntry(row){\n";
        js += L"  const mode=modeOf(row.mode||row.reserveMode||row.taskType||row.category);\n";
        js += L"  return {\n";
        js += L"    raw:row,\n";
        js += L"    mode:mode,\n";
        js += L"    enabled:boolv(row.enabled),\n";
        js += L"    disabledByUser:boolv(row.disabledByUser),\n";
        js += L"    excludeFromAuto:boolv(row.excludeFromAuto),\n";
        js += L"    dedupeKey:text(row.dedupeKey),\n";
        js += L"    reserveKey:text(row.reserveKey),\n";
        js += L"    fallbackKey:text(row.fallbackKey),\n";
        js += L"    kind:intv(row.kind,0),\n";
        js += L"    instance:intv(row.instance,0),\n";
        js += L"    space:intv(row.space,0),\n";
        js += L"    networkId:intv(row.networkId,0),\n";
        js += L"    tsId:intv(row.tsId,0),\n";
        js += L"    serviceId:intv(row.serviceId,0),\n";
        js += L"    eventId:intv(row.eventId,0),\n";
        js += L"    startDate:text(row.startDate||row.date||row.dateText),\n";
        js += L"    startTime:text(row.startTime),\n";
        js += L"    endDate:text(row.endDate),\n";
        js += L"    endTime:text(row.endTime),\n";
        js += L"    title:text(row.title||row.titleLabel||row.label),\n";
        js += L"    serviceName:text(row.serviceName||row.service||row.target)\n";
        js += L"  };\n";
        js += L"}\n";

        js += L"function addKey(key,entry){if(key&&!gMap.has(key))gMap.set(key,entry);}\n";
        js += L"function eventKey(space,nid,tsid,sid,eid,mode){return 'event|'+space+'|'+nid+'|'+tsid+'|'+sid+'|'+eid+'|'+mode;}\n";
        js += L"function dedupeKey(key,mode){return 'dedupe|'+mode+'|'+key;}\n";

        js += L"function rebuildMap(){\n";
        js += L"  gMap=new Map();\n";
        js += L"  gRows.forEach(row=>{\n";
        js += L"    const e=makeEntry(row);\n";
        js += L"    addKey(dedupeKey(e.dedupeKey,e.mode),e);\n";
        js += L"    addKey(dedupeKey(e.reserveKey,e.mode),e);\n";
        js += L"    addKey(dedupeKey(e.fallbackKey,e.mode),e);\n";
        js += L"    addKey(eventKey(e.space,e.networkId,e.tsId,e.serviceId,e.eventId,e.mode),e);\n";
        js += L"    addKey(eventKey(e.space,0,0,e.serviceId,e.eventId,e.mode),e);\n";
        js += L"  });\n";
        js += L"}\n";

        js += L"function loadCacheSync(){\n";
        js += L"  try{\n";
        js += L"    const xhr=new XMLHttpRequest();\n";
        js += L"    xhr.open('GET',VIEW_CACHE_URL+'?v='+Date.now(),false);\n";
        js += L"    xhr.send(null);\n";
        js += L"    if(xhr.status<200||xhr.status>=300){\n";
        js += L"      warn('cache load failed',{status:xhr.status,url:VIEW_CACHE_URL});\n";
        js += L"      return false;\n";
        js += L"    }\n";
        js += L"    gRows=parseTsv(xhr.responseText||'');\n";
        js += L"    rebuildMap();\n";
        js += L"    log('cache loaded',{rows:gRows.length,mapKeys:gMap.size});\n";
        js += L"    return true;\n";
        js += L"  }catch(e){warn('cache load exception',String(e&&e.message?e.message:e));return false;}\n";
        js += L"}\n";

        js += L"function payloadFromBox(box){\n";
        js += L"  const d=box.dataset;\n";
        js += L"  return {\n";
        js += L"    reserveMode:modeOf(d.rsvMode),\n";
        js += L"    mode:modeOf(d.rsvMode),\n";
        js += L"    reserveSource:'epg_lite_timetable_switch',\n";
        js += L"    enabled:true,\n";
        js += L"    disabledByUser:false,\n";
        js += L"    excludeFromAuto:false,\n";
        js += L"    autoReserved:false,\n";
        js += L"    space:intv(d.rsvSpace,0),\n";
        js += L"    networkId:intv(d.rsvNetworkId,0),\n";
        js += L"    tsId:intv(d.rsvTsId,0),\n";
        js += L"    serviceId:intv(d.rsvServiceId,0),\n";
        js += L"    eventId:intv(d.rsvEventId,0),\n";
        js += L"    serviceKey:text(d.rsvServiceKey),\n";
        js += L"    serviceName:text(d.rsvServiceName),\n";
        js += L"    service:text(d.rsvServiceName),\n";
        js += L"    title:text(d.rsvTitle),\n";
        js += L"    titleLabel:text(d.rsvTitle),\n";
        js += L"    summary:text(d.rsvSummary),\n";
        js += L"    genre:text(d.rsvGenre),\n";
        js += L"    dateText:text(d.rsvDate),\n";
        js += L"    timeText:text(d.rsvTimeText),\n";
        js += L"    time:text(d.rsvTimeText),\n";
        js += L"    startDate:text(d.rsvStartDate),\n";
        js += L"    startTime:text(d.rsvStartTime),\n";
        js += L"    endDate:text(d.rsvEndDate),\n";
        js += L"    endTime:text(d.rsvEndTime),\n";
        js += L"    durationSec:intv(d.rsvDurationSec,0),\n";
        js += L"    dedupeKey:text(d.rsvDedupeKey),\n";
        js += L"    excludeKey:text(d.rsvDedupeKey),\n";
        js += L"    requiredTuners:1,\n";
        js += L"    preferredTunerIndex:0\n";
        js += L"  };\n";
        js += L"}\n";

        js += L"function findEntry(box){\n";
        js += L"  const p=payloadFromBox(box);\n";
        js += L"  const mode=modeOf(p.reserveMode);\n";
        js += L"  return gMap.get(dedupeKey(p.dedupeKey,mode))||\n";
        js += L"    gMap.get(eventKey(p.space,p.networkId,p.tsId,p.serviceId,p.eventId,mode))||\n";
        js += L"    gMap.get(eventKey(p.space,0,0,p.serviceId,p.eventId,mode))||null;\n";
        js += L"}\n";

        js += L"function encodeForm(obj){\n";
        js += L"  return Object.keys(obj||{}).filter(k=>obj[k]!=null).map(k=>encodeURIComponent(k)+'='+encodeURIComponent(String(obj[k]))).join('&');\n";
        js += L"}\n";

        js += L"function postSync(path,payload){\n";
        js += L"  log('post begin',path,payload);\n";
        js += L"  try{\n";
        js += L"    const xhr=new XMLHttpRequest();\n";
        js += L"    xhr.open('POST',API_BASE+path,false);\n";
        js += L"    xhr.setRequestHeader('Content-Type','application/x-www-form-urlencoded; charset=UTF-8');\n";
        js += L"    xhr.send(encodeForm(payload));\n";
        js += L"    let obj=null;\n";
        js += L"    try{obj=JSON.parse(xhr.responseText||'{}');}catch(e){warn('json parse failed',{path:path,status:xhr.status,response:xhr.responseText});}\n";
        js += L"    if(obj&&typeof obj==='object'){\n";
        js += L"      if(obj.ok===undefined)obj.ok=xhr.status>=200&&xhr.status<300;\n";
        js += L"      obj.status=xhr.status;\n";
        js += L"      log('post end',path,{status:xhr.status,response:obj});\n";
        js += L"      return obj;\n";
        js += L"    }\n";
        js += L"    const fallback={ok:xhr.status>=200&&xhr.status<300,status:xhr.status,reason:'invalid-json-response'};\n";
        js += L"    warn('post invalid response',path,{status:xhr.status,response:xhr.responseText});\n";
        js += L"    return fallback;\n";
        js += L"  }catch(e){warn('post exception',path,String(e&&e.message?e.message:e));return {ok:false,reason:'http-error',message:String(e&&e.message?e.message:e)};}\n";
        js += L"}\n";

        js += L"function parseDateTimeMs(dateText,timeText){\n";
        js += L"  const d=text(dateText).trim();\n";
        js += L"  const t=text(timeText).trim();\n";
        js += L"  if(!d||!t)return NaN;\n";
        js += L"  const dm=d.match(/^(\\d{4})-(\\d{1,2})-(\\d{1,2})$/);\n";
        js += L"  const tm=t.match(/^(\\d{1,2}):(\\d{1,2})(?::(\\d{1,2}))?$/);\n";
        js += L"  if(!dm||!tm)return NaN;\n";
        js += L"  const y=Number(dm[1]);\n";
        js += L"  const mo=Number(dm[2])-1;\n";
        js += L"  const da=Number(dm[3]);\n";
        js += L"  const h=Number(tm[1]);\n";
        js += L"  const mi=Number(tm[2]);\n";
        js += L"  const se=tm[3]?Number(tm[3]):0;\n";
        js += L"  if(!Number.isFinite(y)||!Number.isFinite(mo)||!Number.isFinite(da)||!Number.isFinite(h)||!Number.isFinite(mi)||!Number.isFinite(se))return NaN;\n";
        js += L"  const dt=new Date(y,mo,da,h,mi,se,0);\n";
        js += L"  const ms=dt.getTime();\n";
        js += L"  return Number.isFinite(ms)?ms:NaN;\n";
        js += L"}\n";

        js += L"function isBoxProgramExpired(box){\n";
        js += L"  if(!box)return false;\n";
        js += L"  const d=box.dataset||{};\n";
        js += L"  const endMs=parseDateTimeMs(d.rsvEndDate,d.rsvEndTime);\n";
        js += L"  if(!Number.isFinite(endMs))return false;\n";
        js += L"  const limitMs=endMs+(3*60*1000);\n";
        js += L"  return Date.now()>limitMs;\n";
        js += L"}\n";

        js += L"function isBoxProgramStarted(box){\n";
        js += L"  if(!box)return false;\n";
        js += L"  const d=box.dataset||{};\n";
        js += L"  const startMs=parseDateTimeMs(d.rsvStartDate,d.rsvStartTime);\n";
        js += L"  if(!Number.isFinite(startMs))return false;\n";
        js += L"  return Date.now()>startMs;\n";
        js += L"}\n";

        js += L"function setMsg(box,msg,isError){\n";
        js += L"  const el=box?box.querySelector('[data-lite-rsv-msg]'):null;\n";
        js += L"  if(el){el.textContent=msg||'';if(isError)el.classList.add('error');else el.classList.remove('error');}\n";
        js += L"}\n";

        js += L"function setButtonState(box,state){\n";
        js += L"  const add=box.querySelector('[data-lite-rsv-action=\"upsert\"]');\n";
        js += L"  const disable=box.querySelector('[data-lite-rsv-action=\"disable\"]');\n";
        js += L"  const enable=box.querySelector('[data-lite-rsv-action=\"enable\"]');\n";
        js += L"  const remove=box.querySelector('[data-lite-rsv-action=\"remove\"]');\n";
        js += L"  [add,disable,enable,remove].forEach(function(btn){\n";
        js += L"    if(!btn)return;\n";
        js += L"    btn.disabled=false;\n";
        js += L"    btn.classList.remove('rsv-expired');\n";
        js += L"  });\n";
        js += L"  box.classList.remove('expired');\n";
        js += L"  if(state==='expired'){\n";
        js += L"    if(add){\n";
        js += L"      add.hidden=false;\n";
        js += L"      add.disabled=true;\n";
        js += L"      add.textContent='終了済み';\n";
        js += L"      add.classList.add('rsv-expired');\n";
        js += L"    }\n";
        js += L"    if(disable)disable.hidden=true;\n";
        js += L"    if(enable)enable.hidden=true;\n";
        js += L"    if(remove)remove.hidden=true;\n";
        js += L"    box.classList.add('expired');\n";
        js += L"    return;\n";
        js += L"  }\n";
        js += L"  if(state==='started'){\n";
        js += L"    if(add){\n";
        js += L"      add.hidden=false;\n";
        js += L"      add.disabled=true;\n";
        js += L"      add.textContent='開始済み';\n";
        js += L"      add.classList.add('rsv-expired');\n";
        js += L"    }\n";
        js += L"    if(disable)disable.hidden=true;\n";
        js += L"    if(enable)enable.hidden=true;\n";
        js += L"    if(remove)remove.hidden=true;\n";
        js += L"    box.classList.add('expired');\n";
        js += L"    return;\n";
        js += L"  }\n";
        js += L"  if(add){\n";
        js += L"    add.textContent='録画予約';\n";
        js += L"    add.hidden=state!=='none';\n";
        js += L"  }\n";
        js += L"  if(disable)disable.hidden=state!=='record';\n";
        js += L"  if(enable)enable.hidden=state!=='disabled';\n";
        js += L"  if(remove)remove.hidden=state==='none';\n";
        js += L"}\n";

        js += L"function applyBox(box){\n";
        js += L"  const entry=findEntry(box);\n";
        js += L"  const cell=box.closest('td.prog-cell')||box.closest('tr[data-lite-program-row]');\n";
        js += L"  if(cell){cell.classList.remove('reserved-record','reserved-disabled');}\n";
        js += L"  box.dataset.rsvExisting='0';\n";
        js += L"  box.dataset.rsvKind='0';\n";
        js += L"  box.dataset.rsvInstance='0';\n";
        js += L"  if(!entry){\n";
        js += L"    if(isBoxProgramExpired(box)){\n";
        js += L"      setButtonState(box,'expired');\n";
        js += L"      setMsg(box,'終了済み',false);\n";
        js += L"      return;\n";
        js += L"    }\n";
        js += L"    if(isBoxProgramStarted(box)){\n";
        js += L"      setButtonState(box,'started');\n";
        js += L"      setMsg(box,'開始済み',false);\n";
        js += L"      return;\n";
        js += L"    }\n";
        js += L"    setButtonState(box,'none');\n";
        js += L"    setMsg(box,'',false);\n";
        js += L"    return;\n";
        js += L"  }\n";
        js += L"  box.dataset.rsvExisting='1';\n";
        js += L"  box.dataset.rsvKind=String(entry.kind||0);\n";
        js += L"  box.dataset.rsvInstance=String(entry.instance||0);\n";
        js += L"  if(entry.dedupeKey)box.dataset.rsvDedupeKey=entry.dedupeKey;\n";
        js += L"  if(entry.enabled){\n";
        js += L"    setButtonState(box,'record');\n";
        js += L"    if(cell)cell.classList.add('reserved-record');\n";
        js += L"    setMsg(box,'',false);\n";
        js += L"  }else{\n";
        js += L"    setButtonState(box,'disabled');\n";
        js += L"    if(cell)cell.classList.add('reserved-disabled');\n";
        js += L"    setMsg(box,'',false);\n";
        js += L"  }\n";
        js += L"}\n";

        js += L"function syncBoxes(){\n";
        js += L"  document.querySelectorAll('[data-lite-rsv-box]').forEach(box=>applyBox(box));\n";
        js += L"}\n";

        js += L"function refreshAfterMutation(){\n";
        js += L"  loadCacheSync();\n";
        js += L"  syncBoxes();\n";
        js += L"  try{\n";
        js += L"    if(window.parent&&window.parent!==window){\n";
        js += L"      window.parent.dispatchEvent(new CustomEvent('epg-reservation-list-changed',{detail:{source:'lite-timetable-switch',stamp:Date.now()}}));\n";
        js += L"    }\n";
        js += L"  }catch(_){}\n";
        js += L"}\n";

        js += L"function payloadWithExisting(box,entry){\n";
        js += L"  const p=payloadFromBox(box);\n";
        js += L"  p.kind=entry?entry.kind:intv(box.dataset.rsvKind,0);\n";
        js += L"  p.instance=entry?entry.instance:intv(box.dataset.rsvInstance,0);\n";
        js += L"  p.id=entry&&entry.dedupeKey?entry.dedupeKey:p.dedupeKey;\n";
        js += L"  if(entry&&entry.dedupeKey)p.dedupeKey=entry.dedupeKey;\n";
        js += L"  return p;\n";
        js += L"}\n";

        js += L"function doUpsert(box){\n";
        js += L"  if(isBoxProgramExpired(box)){\n";
        js += L"    return {ok:false,reason:'program-already-ended'};\n";
        js += L"  }\n";
        js += L"  if(isBoxProgramStarted(box)){\n";
        js += L"    return {ok:false,reason:'program-already-started'};\n";
        js += L"  }\n";
        js += L"  const p=payloadFromBox(box);\n";
        js += L"  return postSync('/upsert',p);\n";
        js += L"}\n";

        js += L"function doDisable(box,entry){\n";
        js += L"  const p=payloadWithExisting(box,entry);\n";
        js += L"  p.enabled=false;\n";
        js += L"  p.disabledByUser=true;\n";
        js += L"  p.excludeFromAuto=true;\n";
        js += L"  if(p.kind>0&&p.instance>0)return postSync('/set_enabled_by_id',p);\n";
        js += L"  return postSync('/upsert',p);\n";
        js += L"}\n";

        js += L"function doEnable(box,entry){\n";
        js += L"  const p=payloadWithExisting(box,entry);\n";
        js += L"  p.enabled=true;\n";
        js += L"  p.disabledByUser=false;\n";
        js += L"  p.excludeFromAuto=false;\n";
        js += L"  if(p.kind>0&&p.instance>0)return postSync('/set_enabled_by_id',p);\n";
        js += L"  return postSync('/upsert',p);\n";
        js += L"}\n";

        js += L"function doRemove(box,entry){\n";
        js += L"  const p=payloadWithExisting(box,entry);\n";
        js += L"  if(p.kind>0&&p.instance>0)return postSync('/remove_by_id',p);\n";
        js += L"  return postSync('/remove',p);\n";
        js += L"}\n";

        js += L"function onAction(btn){\n";
        js += L"  const box=btn.closest('[data-lite-rsv-box]');\n";
        js += L"  if(!box||btn.classList.contains('busy'))return;\n";
        js += L"  btn.classList.add('busy');\n";
        js += L"  setMsg(box,'処理中...',false);\n";
        js += L"  try{\n";
        js += L"    loadCacheSync();\n";
        js += L"    const entry=findEntry(box);\n";
        js += L"    const action=btn.getAttribute('data-lite-rsv-action')||'';\n";
        js += L"    log('action begin',action,{payload:payloadFromBox(box),entry:entry,started:isBoxProgramStarted(box),expired:isBoxProgramExpired(box)});\n";
        js += L"    let result=null;\n";
        js += L"    if(action==='upsert')result=doUpsert(box);\n";
        js += L"    else if(action==='disable')result=doDisable(box,entry);\n";
        js += L"    else if(action==='enable')result=doEnable(box,entry);\n";
        js += L"    else if(action==='remove')result=doRemove(box,entry);\n";
        js += L"    else result={ok:false,reason:'unknown-action'};\n";
        js += L"    log('action result',action,result);\n";
        js += L"    if(!result||result.ok===false){\n";
        js += L"      const reason=text(result&&result.reason||result&&result.message||'api-error');\n";
        js += L"      if(reason==='program-already-ended'){\n";
        js += L"        setMsg(box,'終了済みのため予約できません',true);\n";
        js += L"      }else if(reason==='program-already-started'){\n";
        js += L"        setMsg(box,'開始済みのため予約できません',true);\n";
        js += L"      }else if(reason==='schedule-program-reservation-failed'){\n";
        js += L"        setMsg(box,'登録失敗: 予約時刻またはチューナー条件を確認',true);\n";
        js += L"      }else{\n";
        js += L"        setMsg(box,'失敗: '+reason,true);\n";
        js += L"      }\n";
        js += L"      return;\n";
        js += L"    }\n";
        js += L"    setMsg(box,'更新しました',false);\n";
        js += L"    refreshAfterMutation();\n";
        js += L"  }finally{\n";
        js += L"    btn.classList.remove('busy');\n";
        js += L"  }\n";
        js += L"}\n";

        js += L"document.addEventListener('click',function(ev){\n";
        js += L"  const btn=ev.target&&ev.target.closest?ev.target.closest('[data-lite-rsv-action]'):null;\n";
        js += L"  if(!btn)return;\n";
        js += L"  ev.preventDefault();\n";
        js += L"  ev.stopPropagation();\n";
        js += L"  onAction(btn);\n";
        js += L"});\n";

        js += L"document.addEventListener('DOMContentLoaded',function(){\n";
        js += L"  loadCacheSync();\n";
        js += L"  syncBoxes();\n";
        js += L"});\n";

        js += L"window.__epgLiteReservationSwitchReload=function(){loadCacheSync();syncBoxes();return true;};\n";

        js += L"window.__epgLiteReservationSwitchDebugDump=function(target){\n";
        js += L"  const box=target&&target.closest?target.closest('[data-lite-rsv-box]'):document.querySelector('[data-lite-rsv-box]');\n";
        js += L"  if(!box){console.warn('[lite-rsv] box not found');return null;}\n";
        js += L"  loadCacheSync();\n";
        js += L"  const payload=payloadFromBox(box);\n";
        js += L"  const entry=findEntry(box);\n";
        js += L"  const out={payload:payload,entry:entry,started:isBoxProgramStarted(box),expired:isBoxProgramExpired(box),rows:gRows.length,mapKeys:gMap.size,sampleKeys:Array.from(gMap.keys()).slice(0,20)};\n";
        js += L"  console.log('[lite-rsv][dump]',out);\n";
        js += L"  return out;\n";
        js += L"};\n";

        js += L"})();\n";

        return js;
    }

    std::wstring BuildBlockPageHtml_(
        const EpgHtmlLite::PageNowTimetable::BlockPageRequest& request)
    {
        const std::wstring title =
            request.pageTitle.empty()
            ? L"EPG Lite"
            : request.pageTitle;

        const LiteBlockMinuteRange_ blockRange =
            GetLiteBlockMinuteRange_(request.block);

        const int blockMinutes =
            (std::max)(1, blockRange.end - blockRange.begin);

        const LiteEndFallbackMap_ endFallbackMap =
            BuildLiteEndFallbackMap_(
                request.rows,
                request.block);

        const std::vector<LiteTableService_> services =
            BuildServices_(
                request,
                blockRange,
                endFallbackMap);

        EpgHtmlLite::Common::HeaderContext header{};
        header.title = title;
        header.subtitle =
            DateLabel_(request.dateKey) +
            L" / 現在番組 / 時間割 / " +
            LiteBlockLabel_(request.block);

        header.mainLinks =
            EpgHtmlLite::Common::BuildMainMenuLinks(
                request.space,
                EpgHtmlLite::MenuId::ProgramGuide);

        header.broadcastLinks =
            EpgHtmlLite::Common::BuildBroadcastLinks(
                EpgHtmlLite::Common::GuidePageKind::NowTimetable,
                request.space);

        header.guideScopeLinks =
            EpgHtmlLite::Common::BuildGuideScopeLinks(
                request.space,
                EpgHtmlLite::Common::GuidePageKind::NowTimetable);

        header.guideViewLinks =
            EpgHtmlLite::Common::BuildGuideViewLinks(
                request.space,
                EpgHtmlLite::Common::GuidePageKind::NowTimetable);

        header.timeBlockLinks =
            EpgHtmlLite::Common::BuildTimeBlockLinks(
                request.space,
                EpgHtmlLite::Common::GuidePageKind::NowTimetable,
                request.block);

        header.genreLinks =
            EpgHtmlLite::Common::BuildGenreLinks(
                request.space,
                EpgHtmlLite::Common::GuidePageKind::NowTimetable,
                request.block,
                L"all");

        // root index.html 側に日付 / 表示 / 時間帯スイッチがある。
// iframe内では Common メニューを出さず、
// SplitGuide 旧方式のサブch / ジャンルスイッチだけ出す。
        header.showMainMenuRow = false;
        header.showGuideControlRow = false;
        header.showGenreRow = false;

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
        AppendLiteFrameSwitchCss_(oss);
        AppendTimetableCss_(oss);
        EpgHtmlLite::ReservationSwitch::AppendCss(oss, EpgHtmlLite::ReservationSwitch::HostKind::Timetable);
        oss << L"</style>\n";
        oss << L"<script defer src=\"../../../epg_lite_genre_filter.js\"></script>\n";
        oss << L"<script defer src=\"../../../epg_lite_subchannel_filter.js\"></script>\n";
        EpgHtmlLite::ReservationSwitch::AppendScriptTagForFramePage(oss);
        oss << L"</head>\n";
        oss << L"<body class=\"lite-frame-page\">\n";

        oss << EpgHtmlLite::Common::BuildLayoutBegin(header);

        oss << BuildSubchannelToolbar_(L"genreScope");
        oss << BuildGenreToolbar_(L"genreScope");

        oss << L"<section class=\"panel\">\n";
        oss << L"<h2>現在番組 時間割</h2>\n";
        oss << L"<div class=\"lite-page-note\">";
        oss << L"日付: "
            << HtmlEscape_(DateLabel_(request.dateKey))
            << L" / 時間帯: "
            << HtmlEscape_(LiteBlockLabel_(request.block))
            << L" / 件数: "
            << request.rows.size();
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
                oss << L" data-subchannel-item=\"1\"";
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
                            request.block,
                            endFallbackMap,
                            request.enableAutoReserveAssist,
                            request.autoReserveEntryUrl,
                            request.space);
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
        return L"epg_lite_now_timetable_space" +
            std::to_wstring(space) +
            L".html";
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

            if (e.isPast) {
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

namespace EpgHtmlLite::PageNowTimetable
{
    bool GenerateBlockPage(
        const BlockPageRequest& request,
        Result& ioResult)
    {
        try {
            if (request.outputPath.empty()) {
                return false;
            }

            const std::wstring html =
                BuildBlockPageHtml_(request);

            if (!WriteUtf8File_(
                request.outputPath,
                html)) {
                return false;
            }

            ioResult.generatedFiles.push_back(
                request.outputPath.wstring());

            std::wstring switchJsPath;

            if (!EpgHtmlLite::ReservationSwitch::WriteScriptForPageOutput(
                request.outputPath,
                &switchJsPath)) {
                return false;
            }

            if (!switchJsPath.empty()) {
                ioResult.generatedFiles.push_back(
                    switchJsPath);
            }

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
            BlockPageRequest request{};
            request.outputPath =
                ctx.outputDir / FileNameFor_(ctx.space);
            request.pageTitle =
                ctx.title.empty()
                ? L"EPG Lite"
                : ctx.title + L" Lite";
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

            return GenerateBlockPage(
                request,
                ioResult);
        }
        catch (...) {
            return false;
        }
    }
}
