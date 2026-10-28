#include "EpgHtmlLite_ReservationSwitch.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace EpgHtmlLite::ReservationSwitch
{
    namespace
    {
        constexpr const wchar_t* kLiteReservationSwitchScriptFile_ =
            L"epg_lite_reservation_switch.js";

        struct LiteServiceIdentity_
        {
            std::uint32_t networkId = 0;
            std::uint32_t tsId = 0;
            std::wstring serviceName;
            bool found = false;
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
                case L'\"':
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
            if (cp <= 0x7Fu) {
                out.push_back(static_cast<char>(cp));
                return;
            }

            if (cp <= 0x7FFu) {
                out.push_back(
                    static_cast<char>(0xC0u | ((cp >> 6) & 0x1Fu)));
                out.push_back(
                    static_cast<char>(0x80u | (cp & 0x3Fu)));
                return;
            }

            if (cp <= 0xFFFFu) {
                out.push_back(
                    static_cast<char>(0xE0u | ((cp >> 12) & 0x0Fu)));
                out.push_back(
                    static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                out.push_back(
                    static_cast<char>(0x80u | (cp & 0x3Fu)));
                return;
            }

            out.push_back(
                static_cast<char>(0xF0u | ((cp >> 18) & 0x07u)));
            out.push_back(
                static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(
                static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(
                static_cast<char>(0x80u | (cp & 0x3Fu)));
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

        std::wstring TrimLiteCsvField_(
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

        std::vector<std::wstring> SplitLiteCsvLine_(
            std::wstring_view line)
        {
            std::vector<std::wstring> fields;
            std::wstring current;
            bool inQuote = false;

            for (const wchar_t ch : line) {
                if (ch == L'\"') {
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

        bool TryParseLiteUInt_(
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

        bool ReadLiteTextFileWide_(
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

        bool TryReadLiteServiceIdentityLayout_(
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

        bool TryReadLiteServiceIdentityFromLine_(
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

        std::vector<std::filesystem::path> BuildLiteChannelListCandidates_(
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

        LiteServiceIdentity_ ResolveLiteServiceIdentity_(
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

        LiteServiceIdentity_ ResolveLiteServiceIdentity_(
            const ProgramEntry& e)
        {
            return ResolveLiteServiceIdentity_(
                e.space,
                static_cast<std::uint32_t>(e.serviceId));
        }

        std::wstring BuildLiteServiceKey_(
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

        std::wstring FormatMinuteOfDayText_(
            int minute)
        {
            if (minute < 0) {
                return {};
            }

            minute %= 24 * 60;

            if (minute < 0) {
                minute += 24 * 60;
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

        std::wstring LiteIsoDateToSlashDate_(
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

        static bool IsLiteInvalidClockText_(
            const std::wstring& s) noexcept
        {
            if (s.empty()) {
                return true;
            }

            std::wstring t;
            t.reserve(s.size());

            for (wchar_t ch : s) {
                if (ch == L' ' ||
                    ch == L'\t' ||
                    ch == L'\r' ||
                    ch == L'\n' ||
                    ch == L'　') {
                    continue;
                }

                t.push_back(ch);
            }

            if (t.empty()) {
                return true;
            }

            return t == L"--" ||
                t == L"-" ||
                t == L"--:--" ||
                t == L"--:--:00" ||
                t == L"---:--" ||
                t == L"---:--:00" ||
                t == L"??:??" ||
                t == L"??:??:00";
        }

        static bool IsLiteValidHmsText_(
            const std::wstring& s) noexcept
        {
            if (IsLiteInvalidClockText_(s)) {
                return false;
            }

            if (s.size() != 5 &&
                s.size() != 8) {
                return false;
            }

            auto isDigit = [](wchar_t ch) noexcept {
                return ch >= L'0' && ch <= L'9';
                };

            if (!isDigit(s[0]) ||
                !isDigit(s[1]) ||
                s[2] != L':' ||
                !isDigit(s[3]) ||
                !isDigit(s[4])) {
                return false;
            }

            const int hour =
                static_cast<int>((s[0] - L'0') * 10 +
                    (s[1] - L'0'));

            const int minute =
                static_cast<int>((s[3] - L'0') * 10 +
                    (s[4] - L'0'));

            if (hour < 0 ||
                hour > 47 ||
                minute < 0 ||
                minute > 59) {
                return false;
            }

            if (s.size() == 8) {
                if (s[5] != L':' ||
                    !isDigit(s[6]) ||
                    !isDigit(s[7])) {
                    return false;
                }

                const int sec =
                    static_cast<int>((s[6] - L'0') * 10 +
                        (s[7] - L'0'));

                if (sec < 0 ||
                    sec > 59) {
                    return false;
                }
            }

            return true;
        }

        static std::wstring LiteEnsureHms_(
            const std::wstring& s)
        {
            const std::wstring v =
                TrimLiteCsvField_(s);

            if (IsLiteInvalidClockText_(v)) {
                return {};
            }

            if (v.size() == 5 &&
                v[2] == L':') {
                const std::wstring hms = v + L":00";
                return IsLiteValidHmsText_(hms)
                    ? hms
                    : std::wstring{};
            }

            if (v.size() == 8 &&
                v[2] == L':' &&
                v[5] == L':') {
                return IsLiteValidHmsText_(v)
                    ? v
                    : std::wstring{};
            }

            return {};
        }

        std::wstring LiteTrimSecond_(
            const std::wstring& s)
        {
            if (s.size() >= 5) {
                return s.substr(0, 5);
            }

            return s;
        }

        std::wstring LitePickStartDate_(
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

        std::wstring LitePickEndDate_(
            const ProgramEntry& e)
        {
            if (!e.endDate.empty()) {
                return e.endDate;
            }

            return LitePickStartDate_(e);
        }

        std::wstring LitePickStartTimeHms_(
            const ProgramEntry& e)
        {
            if (!e.startTime.empty()) {
                return LiteEnsureHms_(e.startTime);
            }

            if (e.startMinutesOfDay >= 0) {
                return FormatMinuteOfDayText_(e.startMinutesOfDay) +
                    L":00";
            }

            return LiteEnsureHms_(e.startText);
        }

        std::wstring LitePickEndTimeHms_(
            const ProgramEntry& e)
        {
            if (!e.endTime.empty()) {
                return LiteEnsureHms_(e.endTime);
            }

            if (e.endMinutesOfDay >= 0) {
                return FormatMinuteOfDayText_(e.endMinutesOfDay) +
                    L":00";
            }

            return LiteEnsureHms_(e.endText);
        }

        std::wstring LiteTimeRangeText_(
            const ProgramEntry& e)
        {
            const std::wstring start =
                LiteTrimSecond_(LitePickStartTimeHms_(e));

            const std::wstring end =
                LiteTrimSecond_(LitePickEndTimeHms_(e));

            if (!start.empty() && !end.empty()) {
                return start + L"-" + end;
            }

            if (!start.empty()) {
                return start;
            }

            return {};
        }

        std::uint32_t LiteDurationSecOf_(
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

            const long long minutesWide =
                static_cast<long long>(end) -
                static_cast<long long>(begin);

            if (minutesWide <= 0) {
                return 0;
            }

            const long long secWide =
                minutesWide * 60LL;

            if (secWide >
                static_cast<long long>(
                    (std::numeric_limits<std::uint32_t>::max)())) {
                return (std::numeric_limits<std::uint32_t>::max)();
            }

            return static_cast<std::uint32_t>(secWide);
        }

        std::wstring LiteMakeProgramDedupeKey_(
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
            key += std::to_wstring(static_cast<unsigned int>(e.serviceId));
            key += L"-";
            key += std::to_wstring(static_cast<unsigned int>(e.eventId));
            key += L"-";
            key += LitePickStartDate_(e);
            key += L"-";
            key += LitePickStartTimeHms_(e);
            key += L"-record";

            return key;
        }

        void AppendProgramPayloadAttrs_(
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
                    EpgHtmlLite::Common::NormalizeGenreMajorName(
                        e.genreKey))
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
                << static_cast<unsigned int>(LiteDurationSecOf_(e))
                << L"\"";

            oss << L" data-rsv-dedupe-key=\""
                << HtmlEscape_(LiteMakeProgramDedupeKey_(e))
                << L"\"";
        }

        void AddJsLine_(
            std::wstring& js,
            const wchar_t* line)
        {
            js += line;
            js += L"\n";
        }
    }

    const wchar_t* ScriptFileName() noexcept
    {
        return kLiteReservationSwitchScriptFile_;
    }

    const wchar_t* ScriptSrcFromFramePage() noexcept
    {
        return L"../../../epg_lite_reservation_switch.js";
    }

    void AppendScriptTagForFramePage(
        std::wostringstream& oss)
    {
        oss << L"<script defer src=\""
            << ScriptSrcFromFramePage()
            << L"\"></script>\n";
    }

    void AppendCss(
        std::wostringstream& oss,
        HostKind hostKind)
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

        oss << L".lite-rsv-btn:hover{border-color:#7ebaff;}\n";
        oss << L".lite-rsv-btn.busy{opacity:.55;cursor:wait;}\n";
        oss << L".lite-rsv-btn:disabled{opacity:.55;cursor:default;}\n";
        oss << L".lite-rsv-btn.rsv-expired{background:#333842;border-color:#606a78;color:#b9c3d0;cursor:default;}\n";
        oss << L".lite-rsv-btn.rsv-disable{background:#664526;border-color:#c48b3a;}\n";
        oss << L".lite-rsv-btn.rsv-enable{background:#124e35;border-color:#28a36b;}\n";
        oss << L".lite-rsv-btn.rsv-remove{background:#6b2525;border-color:#c45b5b;}\n";
        oss << L".lite-rsv-box [hidden]{display:none!important;}\n";

        oss << L".lite-rsv-msg{\n";
        oss << L"  color:#c7d2e6;\n";
        oss << L"  font-size:10px;\n";
        oss << L"  line-height:1.25;\n";
        oss << L"}\n";
        oss << L".lite-rsv-msg.error{color:#ffb0b0;}\n";
        oss << L".lite-rsv-box.expired .lite-rsv-msg{color:#c0c7d2;}\n";

        if (hostKind == HostKind::Timetable) {
            oss << L".lite-epg-table td.prog-cell.reserved-record{\n";
            oss << L"  background:#5a2028!important;\n";
            oss << L"  border-color:#d45d6e!important;\n";
            oss << L"  box-shadow:inset 0 0 0 2px rgba(255,95,112,.55);\n";
            oss << L"}\n";
            oss << L".lite-epg-table td.prog-cell.reserved-disabled{\n";
            oss << L"  background:#54401b!important;\n";
            oss << L"  border-color:#d2a538!important;\n";
            oss << L"  box-shadow:inset 0 0 0 2px rgba(210,165,56,.55);\n";
            oss << L"}\n";
            oss << L".lite-epg-table td.prog-cell.reserved-record .program-title,.lite-epg-table td.prog-cell.reserved-disabled .program-title{color:#fff;}\n";
        }
        else {
            oss << L".lite-list-table tr.reserved-record td{\n";
            oss << L"  background:#5a2028!important;\n";
            oss << L"  border-color:#d45d6e!important;\n";
            oss << L"  box-shadow:inset 3px 0 0 rgba(255,95,112,.70);\n";
            oss << L"}\n";
            oss << L".lite-list-table tr.reserved-disabled td{\n";
            oss << L"  background:#54401b!important;\n";
            oss << L"  border-color:#d2a538!important;\n";
            oss << L"  box-shadow:inset 3px 0 0 rgba(210,165,56,.70);\n";
            oss << L"}\n";
            oss << L".lite-list-table tr.reserved-record .program-title,.lite-list-table tr.reserved-disabled .program-title{color:#fff;}\n";
        }
    }

    void AppendProgramSwitch(
        std::wostringstream& oss,
        const ProgramEntry& entry)
    {
        const std::wstring startDate =
            LitePickStartDate_(entry);

        const std::wstring startTime =
            LitePickStartTimeHms_(entry);

        if (entry.title.empty() ||
            entry.serviceId == 0 ||
            entry.eventId == 0 ||
            startDate.empty() ||
            !IsLiteValidHmsText_(startTime)) {
            return;
        }

        // ここでは終了時刻不明・開始済み・終了済みを理由に消さない。
        // 表示自体は行い、予約可否は JS 側で判定する。
        oss << L"<div class=\"lite-rsv-box\" data-lite-rsv-box=\"1\"";

        if (entry.isPast) {
            oss << L" data-rsv-entry-past=\"1\"";
        }

        AppendProgramPayloadAttrs_(oss, entry);
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

    std::wstring BuildScript()
    {
        std::wstring js;

        constexpr std::wstring::size_type kReserveSize =
            static_cast<std::wstring::size_type>(96) *
            static_cast<std::wstring::size_type>(1024);

        js.reserve(kReserveSize);

        AddJsLine_(js, L"(function(){");
        AddJsLine_(js, L"'use strict';");
        AddJsLine_(js, L"const VIEW_CACHE_URL='/cache/reservation/view/tasks_view_index.tsv';");
        AddJsLine_(js, L"const API_BASE='/api/reservation';");
        AddJsLine_(js, L"let gRows=[];");
        AddJsLine_(js, L"let gMap=new Map();");
        AddJsLine_(js, L"function text(v){return String(v==null?'':v);}");
        AddJsLine_(js, L"function intv(v,d){const n=Number(v);return Number.isFinite(n)?Math.trunc(n):d;}");
        AddJsLine_(js, L"function boolv(v){if(v===true)return true;if(v===false)return false;const s=text(v).toLowerCase().trim();return s==='1'||s==='true'||s==='yes'||s==='on';}");
        AddJsLine_(js, L"function modeOf(v){const s=text(v).toLowerCase().trim();return s==='watch'?'watch':'record';}");
        AddJsLine_(js, L"function isDebug(){try{return localStorage.getItem('epgLiteRsvDebug')==='1'||location.search.indexOf('rsvDebug=1')>=0;}catch(_){return false;}}");
        AddJsLine_(js, L"function log(){if(!isDebug())return;try{console.log.apply(console,['[lite-rsv]'].concat(Array.prototype.slice.call(arguments)));}catch(_){}}");
        AddJsLine_(js, L"function warn(){if(!isDebug())return;try{console.warn.apply(console,['[lite-rsv]'].concat(Array.prototype.slice.call(arguments)));}catch(_){}}");
        AddJsLine_(js, L"function normalizeText(s){return text(s).replace(/\\u3000/g,' ').replace(/\\s+/g,' ').trim();}");
        AddJsLine_(js, L"function normalizeTimeTextForReservation(s){return text(s).replace(/\\u301C/g,'〜').replace(/\\uFF5E/g,'〜').replace(/\\u223C/g,'〜').replace(/[‐-‒–—―ー－]/g,'-').trim();}");
        AddJsLine_(js, L"function normalizeTimeRangeForDisplay(s){let v=normalizeTimeTextForReservation(s).replace(/？/g,'?').replace(/～/g,'〜').replace(/\\s+/g,' ').trim();return v.replace(/(\\d{1,2}:\\d{2})\\s*[?~〜\\-]\\s*(\\d{1,2}:\\d{2})/g,'$1～$2');}");
        AddJsLine_(js, L"function normalizeTimeRangeForStorage(s){let v=normalizeTimeTextForReservation(s).replace(/？/g,'?').replace(/[～〜?]/g,'-').replace(/\\s+/g,' ').trim();return v.replace(/(\\d{1,2}:\\d{2})\\s*-\\s*(\\d{1,2}:\\d{2})/g,'$1-$2');}");
        AddJsLine_(js, L"function formatIsoTime(h,m,sec){return String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')+':'+String(sec||0).padStart(2,'0');}");
        AddJsLine_(js, L"function formatIsoDate(y,m,d){return String(y).padStart(4,'0')+'-'+String(m).padStart(2,'0')+'-'+String(d).padStart(2,'0');}");
        AddJsLine_(js, L"function shiftIsoDate(iso,delta){const m=/^(\\d{4})-(\\d{2})-(\\d{2})$/.exec(text(iso));if(!m)return '';const dt=new Date(Number(m[1]),Number(m[2])-1,Number(m[3]));if(!Number.isFinite(dt.getTime()))return '';dt.setDate(dt.getDate()+delta);return formatIsoDate(dt.getFullYear(),dt.getMonth()+1,dt.getDate());}");
        AddJsLine_(js, L"function parseDateTextToIso(s){const m=/^(\\d{4})[\\/-](\\d{1,2})[\\/-](\\d{1,2})/.exec(text(s).trim());if(!m)return '';return formatIsoDate(Number(m[1]),Number(m[2]),Number(m[3]));}");
        AddJsLine_(js, L"function parseTimeTextParts(s){const v=normalizeTimeTextForReservation(s);const m=/^(\\d{1,2}):(\\d{2})(?:\\s*[～〜\\-]\\s*(\\d{1,2}):(\\d{2}))?/.exec(v);if(!m)return {startTime:'',endTime:'',durationSec:0,crossesDay:false};const sh=Number(m[1]);const sm=Number(m[2]);const eh=m[3]!=null?Number(m[3]):sh;const em=m[4]!=null?Number(m[4]):sm;let dur=0;let cross=false;if(m[3]!=null){dur=(eh*60+em)-(sh*60+sm);if(dur<0){dur+=24*60;cross=true;}}return {startTime:formatIsoTime(sh,sm,0),endTime:formatIsoTime(eh,em,0),durationSec:dur>0?dur*60:0,crossesDay:cross};}");
        AddJsLine_(js, L"function makeEventKey(space,nid,tsid,sid,eid,mode){const m=modeOf(mode);const sp=intv(space,0);const n=intv(nid,0);const t=intv(tsid,0);const s=intv(sid,0);const e=intv(eid,0);if(s<=0||e<=0)return '';return 'event|'+sp+'|'+n+'|'+t+'|'+s+'|'+e+'|'+m;}");
        AddJsLine_(js, L"function makeLooseEventKey(space,sid,eid,mode){const m=modeOf(mode);const sp=intv(space,0);const s=intv(sid,0);const e=intv(eid,0);if(s<=0||e<=0)return '';return 'event-loose|'+sp+'|'+s+'|'+e+'|'+m;}");
        AddJsLine_(js, L"function makeDedupeKey(key,mode){const k=text(key);if(!k)return '';return 'dedupe|'+modeOf(mode)+'|'+k;}");
        AddJsLine_(js, L"function makeTimeKey(serviceKey,startDate,startTime,title,mode){const sk=text(serviceKey);const sd=text(startDate);const st=text(startTime);const tt=normalizeText(title);const m=modeOf(mode);if(!sk||!sd||!st||!tt)return '';return 'time|'+sk+'|'+sd+'|'+st+'|'+tt+'|'+m;}");
        AddJsLine_(js, L"function addKey(key,entry){if(key&&!gMap.has(key))gMap.set(key,entry);}");
        AddJsLine_(js, L"function parseTsv(src){const lines=text(src).replace(/^\\uFEFF/,'').split(/\\r?\\n/);let header=null;const rows=[];for(let i=0;i<lines.length;++i){const line=lines[i];if(!line||/^\\s*$/.test(line)||line.charAt(0)==='#')continue;const cols=line.split('\\t');if(!header){header=cols.map(v=>text(v).trim());continue;}const row={};for(let c=0;c<header.length;++c)row[header[c]]=c<cols.length?cols[c]:'';rows.push(row);}return rows;}");
        AddJsLine_(js, L"function makeEntry(row){const mode=modeOf(row.mode||row.reserveMode||row.taskType||row.category);return {raw:row,mode:mode,enabled:boolv(row.enabled),disabledByUser:boolv(row.disabledByUser),excludeFromAuto:boolv(row.excludeFromAuto),dedupeKey:text(row.dedupeKey),reserveKey:text(row.reserveKey),fallbackKey:text(row.fallbackKey),kind:intv(row.kind,0),instance:intv(row.instance,0),space:intv(row.space,0),networkId:intv(row.networkId,0),tsId:intv(row.tsId,0),serviceId:intv(row.serviceId,0),eventId:intv(row.eventId,0),serviceKey:text(row.serviceKey),serviceName:text(row.serviceName||row.service||row.target),title:text(row.title||row.titleLabel||row.label),startDate:text(row.startDate||row.date||row.dateText),startTime:text(row.startTime),endDate:text(row.endDate),endTime:text(row.endTime),preferredTunerIndex:intv(row.preferredTunerIndex,-1)};}");
        AddJsLine_(js, L"function rebuildMap(){gMap=new Map();gRows.forEach(row=>{const e=makeEntry(row);addKey(makeDedupeKey(e.dedupeKey,e.mode),e);addKey(makeDedupeKey(e.reserveKey,e.mode),e);addKey(makeDedupeKey(e.fallbackKey,e.mode),e);addKey(makeEventKey(e.space,e.networkId,e.tsId,e.serviceId,e.eventId,e.mode),e);addKey(makeLooseEventKey(e.space,e.serviceId,e.eventId,e.mode),e);addKey(makeTimeKey(e.serviceKey,e.startDate,e.startTime,e.title,e.mode),e);});}");
        AddJsLine_(js, L"function loadCacheSync(){try{const xhr=new XMLHttpRequest();xhr.open('GET',VIEW_CACHE_URL+'?v='+Date.now(),false);xhr.send(null);if(xhr.status<200||xhr.status>=300){warn('cache load failed',{status:xhr.status,url:VIEW_CACHE_URL});return false;}gRows=parseTsv(xhr.responseText||'');rebuildMap();log('cache loaded',{rows:gRows.length,mapKeys:gMap.size});return true;}catch(e){warn('cache load exception',String(e&&e.message?e.message:e));return false;}}");
        AddJsLine_(js, L"function rawPayloadFromBox(box){const d=box.dataset||{};return {reserveMode:modeOf(d.rsvMode),mode:modeOf(d.rsvMode),reserveSource:'epg_lite_reservation_switch',space:intv(d.rsvSpace,0),networkId:intv(d.rsvNetworkId,0),tsId:intv(d.rsvTsId,0),serviceId:intv(d.rsvServiceId,0),eventId:intv(d.rsvEventId,0),serviceKey:text(d.rsvServiceKey),serviceName:text(d.rsvServiceName),service:text(d.rsvServiceName),title:text(d.rsvTitle),titleLabel:text(d.rsvTitle),summary:text(d.rsvSummary),genre:text(d.rsvGenre),dateText:text(d.rsvDate),timeText:text(d.rsvTimeText),time:text(d.rsvTimeText),displayTimeText:text(d.rsvTimeText),startDate:text(d.rsvStartDate),startTime:text(d.rsvStartTime),endDate:text(d.rsvEndDate),endTime:text(d.rsvEndTime),durationSec:intv(d.rsvDurationSec,0),dedupeKey:text(d.rsvDedupeKey),excludeKey:text(d.rsvDedupeKey),requiredTuners:1,preferredTunerIndex:-1};}");
        AddJsLine_(js, L"function buildProgramReservationPayload(payload,mode,preferredTunerIndex,stateOverride){const fixedMode=modeOf(mode);const displayTimeText=normalizeTimeRangeForDisplay(payload.displayTimeText||payload.time||payload.timeText||'');const timeText=normalizeTimeRangeForStorage(displayTimeText||payload.timeText||payload.time||'');const parsedTime=parseTimeTextParts(displayTimeText||timeText);const parsedDate=parseDateTextToIso(payload.dateText||payload.date||'');const startDate=text(payload.startDate)||parsedDate;const startTime=text(payload.startTime)||parsedTime.startTime;let endDate=text(payload.endDate)||parsedDate||startDate;if(!payload.endDate&&parsedTime.crossesDay&&startDate)endDate=shiftIsoDate(startDate,1)||startDate;const endTime=text(payload.endTime)||parsedTime.endTime||startTime;const p={space:intv(payload.space,0),networkId:intv(payload.networkId,0),tsId:intv(payload.tsId,0),serviceId:intv(payload.serviceId,0),eventId:intv(payload.eventId,0),serviceKey:text(payload.serviceKey),physicalChannel:intv(payload.physicalChannel,-1),remoteControlKeyId:text(payload.remoteControlKeyId),displayTimeText:displayTimeText,title:text(payload.title),titleLabel:text(payload.titleLabel||payload.title),serviceName:text(payload.serviceName||payload.service),service:text(payload.serviceName||payload.service),summary:text(payload.summary),genre:text(payload.genre),dateText:text(payload.dateText||payload.date),weekdayText:text(payload.weekdayText),timeText:timeText,time:displayTimeText||timeText,startDate:startDate,startTime:startTime,endDate:endDate,endTime:endTime,durationSec:intv(payload.durationSec!=null?payload.durationSec:parsedTime.durationSec,parsedTime.durationSec),reserveMode:fixedMode,reserveSource:text(payload.reserveSource||'epg_lite_reservation_switch'),dedupeKey:'',excludeKey:'',enabled:!(stateOverride&&stateOverride.enabled===false),disabledByUser:!!(stateOverride&&stateOverride.disabledByUser),excludeFromAuto:!!(stateOverride&&stateOverride.excludeFromAuto),autoReserved:!!(stateOverride&&stateOverride.autoReserved),requiredTuners:intv(payload.requiredTuners!=null?payload.requiredTuners:1,1),preferredTunerIndex:Number.isFinite(Number(preferredTunerIndex))?Number(preferredTunerIndex):intv(payload.preferredTunerIndex,-1)};p.dedupeKey=String(p.space)+'-'+String(p.networkId)+'-'+String(p.tsId)+'-'+String(p.serviceId)+'-'+String(p.eventId)+'-'+String(p.startDate||'')+'-'+String(p.startTime||'')+'-'+fixedMode;p.excludeKey=p.dedupeKey;return p;}");
        AddJsLine_(js, L"function payloadFromBox(box){return buildProgramReservationPayload(rawPayloadFromBox(box),'record',-1,{enabled:true,disabledByUser:false,excludeFromAuto:false,autoReserved:false});}");
        AddJsLine_(js, L"function findEntry(box){const p=payloadFromBox(box);const mode=modeOf(p.reserveMode);return gMap.get(makeDedupeKey(p.dedupeKey,mode))||gMap.get(makeEventKey(p.space,p.networkId,p.tsId,p.serviceId,p.eventId,mode))||gMap.get(makeLooseEventKey(p.space,p.serviceId,p.eventId,mode))||gMap.get(makeTimeKey(p.serviceKey,p.startDate,p.startTime,p.title,mode))||null;}");
        AddJsLine_(js, L"function encodeForm(obj){return Object.keys(obj||{}).filter(k=>obj[k]!=null).map(k=>encodeURIComponent(k)+'='+encodeURIComponent(String(obj[k]))).join('&');}");
        AddJsLine_(js, L"function postSync(path,payload){log('post begin',path,payload);try{const xhr=new XMLHttpRequest();xhr.open('POST',API_BASE+path,false);xhr.setRequestHeader('Content-Type','application/x-www-form-urlencoded; charset=UTF-8');xhr.send(encodeForm(payload));let obj=null;try{obj=JSON.parse(xhr.responseText||'{}');}catch(e){warn('json parse failed',{path:path,status:xhr.status,response:xhr.responseText});}if(obj&&typeof obj==='object'){if(obj.ok===undefined)obj.ok=xhr.status>=200&&xhr.status<300;obj.status=xhr.status;obj.path=path;log('post end',path,obj);return obj;}return {ok:xhr.status>=200&&xhr.status<300,status:xhr.status,path:path,reason:'invalid-json-response',responseText:xhr.responseText||''};}catch(e){return {ok:false,path:path,reason:'http-error',message:String(e&&e.message?e.message:e)};}}");
        AddJsLine_(js, L"function isValidReservationClockText(v){const s=text(v).trim();return /^\\d{1,2}:\\d{2}:\\d{2}$/.test(s);}");
        AddJsLine_(js, L"function hasValidReservationTime(p){if(!p||!p.startDate||!isValidReservationClockText(p.startTime))return false;if(p.endDate&&isValidReservationClockText(p.endTime))return true;return intv(p.durationSec,0)>0;}");
        AddJsLine_(js, L"function parseDateTimeMs(dateText,timeText){const d=text(dateText).trim();const t=text(timeText).trim();const dm=/^(\\d{4})-(\\d{1,2})-(\\d{1,2})$/.exec(d);const tm=/^(\\d{1,2}):(\\d{1,2})(?::(\\d{1,2}))?$/.exec(t);if(!dm||!tm)return NaN;const dt=new Date(Number(dm[1]),Number(dm[2])-1,Number(dm[3]),Number(tm[1]),Number(tm[2]),tm[3]?Number(tm[3]):0,0);const ms=dt.getTime();return Number.isFinite(ms)?ms:NaN;}");
        AddJsLine_(js, L"function getBoxBroadcastState(box){const p=payloadFromBox(box);const start=parseDateTimeMs(p.startDate,p.startTime);let end=parseDateTimeMs(p.endDate,p.endTime);if(!Number.isFinite(end)&&Number.isFinite(start)&&intv(p.durationSec,0)>0)end=start+intv(p.durationSec,0)*1000;const now=Date.now();if(!Number.isFinite(start)||!Number.isFinite(end)||end<=start)return 'invalid';if(now<start)return 'before';if(now<end)return 'onair';return 'expired';}");
        AddJsLine_(js, L"function isBoxProgramExpired(box){return getBoxBroadcastState(box)==='expired';}");
        AddJsLine_(js, L"function isBoxProgramStarted(box){return getBoxBroadcastState(box)==='onair';}");
        AddJsLine_(js, L"function setMsg(box,msg,isError){const el=box?box.querySelector('[data-lite-rsv-msg]'):null;if(el){el.textContent=msg||'';if(isError)el.classList.add('error');else el.classList.remove('error');}}");
        AddJsLine_(js, L"function setButtonState(box,state){const add=box.querySelector('[data-lite-rsv-action=upsert]');const disable=box.querySelector('[data-lite-rsv-action=disable]');const enable=box.querySelector('[data-lite-rsv-action=enable]');const remove=box.querySelector('[data-lite-rsv-action=remove]');[add,disable,enable,remove].forEach(btn=>{if(!btn)return;btn.disabled=false;btn.classList.remove('rsv-expired');});box.classList.remove('expired');if(state==='expired'||state==='invalid'){if(add){add.hidden=false;add.disabled=true;add.textContent=state==='expired'?'終了済み':'時刻不明';add.classList.add('rsv-expired');}if(disable)disable.hidden=true;if(enable)enable.hidden=true;if(remove)remove.hidden=true;box.classList.add('expired');return;}if(add){add.textContent=state==='onair'?'途中から録画':'録画予約';add.hidden=state!=='none'&&state!=='onair';}if(disable)disable.hidden=state!=='record';if(enable)enable.hidden=state!=='disabled';if(remove)remove.hidden=state==='none'||state==='onair';}");
        AddJsLine_(js, L"function applyBox(box){const entry=findEntry(box);const cell=box.closest('td.prog-cell')||box.closest('tr[data-lite-program-row]');if(cell)cell.classList.remove('reserved-record','reserved-disabled');box.dataset.rsvExisting='0';box.dataset.rsvKind='0';box.dataset.rsvInstance='0';if(!entry){const broadcastState=getBoxBroadcastState(box);if(broadcastState==='expired'||broadcastState==='invalid'){setButtonState(box,broadcastState);setMsg(box,broadcastState==='expired'?'終了済み':'時刻情報が不正です',false);return;}setButtonState(box,broadcastState==='onair'?'onair':'none');setMsg(box,broadcastState==='onair'?'放送中・途中から録画可能':'',false);return;}box.dataset.rsvExisting='1';box.dataset.rsvKind=String(entry.kind||0);box.dataset.rsvInstance=String(entry.instance||0);if(entry.dedupeKey)box.dataset.rsvDedupeKey=entry.dedupeKey;if(entry.enabled){setButtonState(box,'record');if(cell)cell.classList.add('reserved-record');setMsg(box,'',false);}else{setButtonState(box,'disabled');if(cell)cell.classList.add('reserved-disabled');setMsg(box,'',false);}}");
        AddJsLine_(js, L"function syncBoxes(){document.querySelectorAll('[data-lite-rsv-box]').forEach(box=>applyBox(box));}");
        AddJsLine_(js, L"function refreshAfterMutation(){loadCacheSync();syncBoxes();try{if(window.parent&&window.parent!==window){window.parent.dispatchEvent(new CustomEvent('epg-reservation-list-changed',{detail:{source:'lite-reservation-switch',stamp:Date.now()}}));}}catch(_){}}");
        AddJsLine_(js, L"function payloadWithExisting(box,entry,stateOverride){const raw=rawPayloadFromBox(box);const preferred=entry&&Number.isFinite(Number(entry.preferredTunerIndex))?Number(entry.preferredTunerIndex):-1;const p=buildProgramReservationPayload(raw,'record',preferred,stateOverride||null);p.kind=entry?entry.kind:intv(box.dataset.rsvKind,0);p.instance=entry?entry.instance:intv(box.dataset.rsvInstance,0);p.id=entry&&entry.dedupeKey?entry.dedupeKey:p.dedupeKey;if(entry&&entry.dedupeKey)p.dedupeKey=entry.dedupeKey;return p;}");
        AddJsLine_(js, L"function doUpsert(box){const p=payloadFromBox(box);if(!hasValidReservationTime(p))return {ok:false,reason:'invalid-program-time'};const state=getBoxBroadcastState(box);if(state==='expired')return {ok:false,reason:'program-already-ended'};if(state==='invalid')return {ok:false,reason:'invalid-program-time'};return postSync('/upsert',p);}");
        AddJsLine_(js, L"function doDisable(box,entry){const p=payloadWithExisting(box,entry,{enabled:false,disabledByUser:true,excludeFromAuto:true,autoReserved:!!(entry&&entry.autoReserved)});if(p.kind>0&&p.instance>0)return postSync('/set_enabled_by_id',p);return postSync('/set_enabled',p);}");
        AddJsLine_(js, L"function doEnable(box,entry){const p=payloadWithExisting(box,entry,{enabled:true,disabledByUser:false,excludeFromAuto:false,autoReserved:!!(entry&&entry.autoReserved)});if(p.kind>0&&p.instance>0)return postSync('/set_enabled_by_id',p);return postSync('/set_enabled',p);}");
        AddJsLine_(js, L"function doRemove(box,entry){const p=payloadWithExisting(box,entry,{enabled:true,disabledByUser:false,excludeFromAuto:false,autoReserved:!!(entry&&entry.autoReserved)});if(p.kind>0&&p.instance>0)return postSync('/remove_by_id',p);return postSync('/remove',p);}");
        AddJsLine_(js, L"function onAction(btn){const box=btn.closest('[data-lite-rsv-box]');if(!box||btn.classList.contains('busy'))return;btn.classList.add('busy');setMsg(box,'処理中...',false);try{loadCacheSync();const entry=findEntry(box);const action=btn.getAttribute('data-lite-rsv-action')||'';log('action begin',action,{payload:payloadFromBox(box),entry:entry,started:isBoxProgramStarted(box),expired:isBoxProgramExpired(box)});let result=null;if(action==='upsert')result=doUpsert(box);else if(action==='disable')result=doDisable(box,entry);else if(action==='enable')result=doEnable(box,entry);else if(action==='remove')result=doRemove(box,entry);else result={ok:false,reason:'unknown-action'};log('action result',action,result);if(!result||result.ok===false){const reason=text(result&&result.reason||result&&result.message||'api-error');if(reason==='program-already-ended')setMsg(box,'終了済みのため予約できません',true);else if(reason==='program-already-started')setMsg(box,'開始済みのため予約できません',true);else if(reason==='schedule-program-reservation-failed')setMsg(box,'登録失敗: 予約時刻またはチューナー条件を確認',true);else setMsg(box,'失敗: '+reason,true);return;}setMsg(box,'更新しました',false);refreshAfterMutation();}finally{btn.classList.remove('busy');}}");
        AddJsLine_(js, L"document.addEventListener('click',function(ev){const btn=ev.target&&ev.target.closest?ev.target.closest('[data-lite-rsv-action]'):null;if(!btn)return;ev.preventDefault();ev.stopPropagation();onAction(btn);});");
        AddJsLine_(js, L"document.addEventListener('DOMContentLoaded',function(){loadCacheSync();syncBoxes();});");
        AddJsLine_(js, L"setInterval(syncBoxes,15000);");
        AddJsLine_(js, L"window.__epgLiteReservationSwitchReload=function(){loadCacheSync();syncBoxes();return true;};");
        AddJsLine_(js, L"window.__epgLiteReservationSwitchDebugDump=function(target){const box=target&&target.closest?target.closest('[data-lite-rsv-box]'):document.querySelector('[data-lite-rsv-box]');if(!box){console.warn('[lite-rsv] box not found');return null;}loadCacheSync();const raw=rawPayloadFromBox(box);const payload=payloadFromBox(box);const entry=findEntry(box);const out={rawPayload:raw,payload:payload,entry:entry,started:isBoxProgramStarted(box),expired:isBoxProgramExpired(box),rows:gRows.length,mapKeys:gMap.size,sampleKeys:Array.from(gMap.keys()).slice(0,20)};console.log('[lite-rsv][dump]',out);return out;};");
        AddJsLine_(js, L"})();");

        return js;
    }

    bool WriteScriptFile(
        const std::filesystem::path& liteRootOutputDir,
        std::wstring* outPath)
    {
        if (outPath) {
            outPath->clear();
        }

        if (liteRootOutputDir.empty()) {
            return false;
        }

        const std::filesystem::path path =
            liteRootOutputDir / kLiteReservationSwitchScriptFile_;

        const std::filesystem::path normalized =
            path.lexically_normal();

        if (!WriteUtf8File_(normalized, BuildScript())) {
            return false;
        }

        if (outPath) {
            *outPath = normalized.wstring();
        }

        return true;
    }

    bool WriteScriptForPageOutput(
        const std::filesystem::path& pageOutputPath,
        std::wstring* outPath)
    {
        if (outPath) {
            outPath->clear();
        }

        if (pageOutputPath.empty()) {
            return false;
        }

        const std::filesystem::path root =
            pageOutputPath.parent_path() /
            std::filesystem::path(L"..") /
            std::filesystem::path(L"..") /
            std::filesystem::path(L"..");

        return WriteScriptFile(
            root.lexically_normal(),
            outPath);
    }
}
