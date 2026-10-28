#include "EpgHtmlLite_PageKeywordSearch.hpp"
#include "AutoReserveRuleStore.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <Windows.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <cwchar>
#include <ctime>
#include <array>

namespace
{
    using namespace EpgHtmlLite;

    constexpr const wchar_t* kKeywordFilePrefix_ =
        L"epg_lite_keyword_search_space";

    std::wstring TrimConfigText_(
        std::wstring_view s);

    std::wstring HtmlEscape_(
        std::wstring_view s)
    {
        std::wstring out;
        out.reserve(s.size() + 16);

        for (wchar_t ch : s) {
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

    bool WriteUtf8BomTextFile_(
        const std::filesystem::path& path,
        const std::wstring& content)
    {
        const std::filesystem::path parent =
            path.parent_path();

        if (!parent.empty()) {
            std::error_code ec;
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

        const unsigned char bom[3] = {
            0xEF,
            0xBB,
            0xBF
        };

        ofs.write(
            reinterpret_cast<const char*>(bom),
            static_cast<std::streamsize>(sizeof(bom)));

#ifdef _WIN32
        if (!content.empty()) {
            if (content.size() >
                static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
                return false;
            }

            const int srcLen =
                static_cast<int>(content.size());

            const int required =
                ::WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    content.data(),
                    srcLen,
                    nullptr,
                    0,
                    nullptr,
                    nullptr);

            if (required <= 0) {
                return false;
            }

            std::string utf8(
                static_cast<std::size_t>(required),
                '\0');

            const int written =
                ::WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    content.data(),
                    srcLen,
                    utf8.data(),
                    required,
                    nullptr,
                    nullptr);

            if (written != required) {
                return false;
            }

            ofs.write(
                utf8.data(),
                static_cast<std::streamsize>(utf8.size()));
        }
#else
        for (wchar_t wc : content) {
            ofs.put(static_cast<char>(wc & 0xFF));
        }
#endif

        ofs.flush();
        return ofs.good();
    }

    std::wstring MakeSpaceLabel_(
        int space)
    {
        switch (space) {
        case 0:
            return L"地上波";
        case 1:
            return L"BS";
        case 2:
            return L"CS";
        default:
            return L"SPACE " + std::to_wstring(space);
        }
    }

    std::wstring FileNameFor_(
        int space)
    {
        return std::wstring(kKeywordFilePrefix_) +
            std::to_wstring(space) +
            L".html";
    }

    std::wstring DataFileNameFor_(
        int space)
    {
        return L"epg_lite_keyword_search_data_space" +
            std::to_wstring(space) +
            L".js";
    }

    std::wstring JsEscape_(
        std::wstring_view s)
    {
        std::wstring out;
        out.reserve(s.size() + 16);

        for (const wchar_t ch : s) {
            switch (ch) {
            case L'\\':
                out += L"\\\\";
                break;
            case L'\'':
                out += L"\\'";
                break;
            case L'\"':
                out += L"\\\"";
                break;
            case L'\r':
            case L'\n':
            case L'\t':
                out += L" ";
                break;
            case L'<':
                out += L"\\u003C";
                break;
            case L'>':
                out += L"\\u003E";
                break;
            case L'&':
                out += L"\\u0026";
                break;
            default:
                out.push_back(ch);
                break;
            }
        }

        return out;
    }

    std::wstring ShortText_(
        std::wstring_view s,
        std::size_t maxLen)
    {
        std::wstring out;
        out.reserve((std::min)(s.size(), maxLen));

        bool prevSpace = false;

        for (const wchar_t ch : s) {
            wchar_t c = ch;

            if (c == L'\r' ||
                c == L'\n' ||
                c == L'\t' ||
                c == L'　') {
                c = L' ';
            }

            if (c == L' ') {
                if (prevSpace) {
                    continue;
                }

                prevSpace = true;
            }
            else {
                prevSpace = false;
            }

            out.push_back(c);

            if (out.size() >= maxLen) {
                out += L"…";
                break;
            }
        }

        return out;
    }

    std::wstring PickDateText_(
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

        return e.broadcastDateKey;
    }

    std::wstring PickTimeText_(
        const ProgramEntry& e)
    {
        if (!e.startText.empty() ||
            !e.endText.empty()) {
            return e.startText +
                L" - " +
                e.endText;
        }

        if (!e.startTime.empty() ||
            !e.endTime.empty()) {
            return e.startTime +
                L" - " +
                e.endTime;
        }

        return {};
    }

    int ParseClockMinute_(
        std::wstring_view s) noexcept
    {
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] < L'0' || s[i] > L'9') {
                continue;
            }

            int hour = 0;
            std::size_t pos = i;

            while (pos < s.size() &&
                s[pos] >= L'0' &&
                s[pos] <= L'9') {
                hour =
                    hour * 10 +
                    static_cast<int>(s[pos] - L'0');
                ++pos;

                if (hour > 99) {
                    return -1;
                }
            }

            if (pos >= s.size() ||
                s[pos] != L':') {
                continue;
            }

            ++pos;

            if (pos + 1 >= s.size() ||
                s[pos] < L'0' ||
                s[pos] > L'9' ||
                s[pos + 1] < L'0' ||
                s[pos + 1] > L'9') {
                continue;
            }

            const int minute =
                static_cast<int>(s[pos] - L'0') * 10 +
                static_cast<int>(s[pos + 1] - L'0');

            if (hour < 0 ||
                hour > 47 ||
                minute < 0 ||
                minute > 59) {
                continue;
            }

            return (hour % 24) * 60 + minute;
        }

        return -1;
    }

    int ProgramDurationMinutes_(
        const ProgramEntry& e) noexcept
    {
        int begin = e.startMinutesOfDay;
        int end = e.endMinutesOfDay;

        if (begin < 0) {
            begin =
                !e.startTime.empty()
                ? ParseClockMinute_(e.startTime)
                : ParseClockMinute_(e.startText);
        }

        if (end < 0) {
            end =
                !e.endTime.empty()
                ? ParseClockMinute_(e.endTime)
                : ParseClockMinute_(e.endText);
        }

        if (begin < 0 ||
            end < 0) {
            return 0;
        }

        if (end <= begin) {
            end += 24 * 60;
        }

        const int minutes =
            end - begin;

        if (minutes <= 0 ||
            minutes > 24 * 60) {
            return 0;
        }

        return minutes;
    }

    std::wstring PickServiceName_(
        const ProgramEntry& e)
    {
        if (!e.serviceName.empty()) {
            return e.serviceName;
        }

        return L"SID " +
            std::to_wstring(
                static_cast<unsigned int>(e.serviceId));
    }

    const std::array<std::wstring_view, 12>& LiteGenreMajorListForSearch_() noexcept
    {
        static constexpr std::array<std::wstring_view, 12> genres = {
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

    std::wstring GenreMajorNameFromCode_(
        int major)
    {
        const auto& genres =
            LiteGenreMajorListForSearch_();

        if (major >= 0 &&
            major < static_cast<int>(genres.size())) {
            return std::wstring(
                genres[static_cast<std::size_t>(major)]);
        }

        return L"その他";
    }

    bool TryExtractGenreCodeFromText_(
        const std::wstring& text,
        int& outMajor) noexcept
    {
        outMajor = -1;

        int value = -1;
        bool hasDigit = false;

        for (const wchar_t ch : text) {
            if (ch >= L'0' &&
                ch <= L'9') {
                if (value < 0) {
                    value = 0;
                }

                value =
                    value * 10 +
                    static_cast<int>(ch - L'0');

                hasDigit = true;

                if (value > 255) {
                    return false;
                }

                continue;
            }

            if (hasDigit) {
                if (value >= 0 &&
                    value <= 15) {
                    outMajor = value;
                    return true;
                }

                value = -1;
                hasDigit = false;
            }
        }

        if (hasDigit &&
            value >= 0 &&
            value <= 15) {
            outMajor = value;
            return true;
        }

        return false;
    }

    std::wstring NormalizeGenreLabel_(
        std::wstring_view genre)
    {
        std::wstring g =
            TrimConfigText_(genre);

        if (g.empty()) {
            return L"その他";
        }

        for (std::wstring_view name : LiteGenreMajorListForSearch_()) {
            if (g == name ||
                g.find(name) != std::wstring::npos) {
                return std::wstring(name);
            }
        }

        if (g == L"news" ||
            g.find(L"ニュース") != std::wstring::npos ||
            g.find(L"報道") != std::wstring::npos) {
            return L"ニュース・報道";
        }

        if (g == L"sports" ||
            g.find(L"スポーツ") != std::wstring::npos) {
            return L"スポーツ";
        }

        if (g == L"drama" ||
            g.find(L"ドラマ") != std::wstring::npos) {
            return L"ドラマ";
        }

        if (g == L"music" ||
            g.find(L"音楽") != std::wstring::npos) {
            return L"音楽";
        }

        if (g == L"variety" ||
            g.find(L"バラエティ") != std::wstring::npos) {
            return L"バラエティー";
        }

        if (g == L"movie" ||
            g.find(L"映画") != std::wstring::npos) {
            return L"映画";
        }

        if (g == L"anime" ||
            g.find(L"アニメ") != std::wstring::npos ||
            g.find(L"特撮") != std::wstring::npos) {
            return L"アニメ・特撮";
        }

        if (g == L"info" ||
            g.find(L"情報") != std::wstring::npos ||
            g.find(L"ワイドショー") != std::wstring::npos) {
            return L"情報・ワイドショー";
        }

        if (g == L"documentary" ||
            g.find(L"ドキュメンタリー") != std::wstring::npos ||
            g.find(L"教養") != std::wstring::npos) {
            return L"ドキュメンタリー";
        }

        if (g == L"stage" ||
            g == L"theater" ||
            g.find(L"劇場") != std::wstring::npos ||
            g.find(L"公演") != std::wstring::npos) {
            return L"劇場・公演";
        }

        if (g == L"hobby" ||
            g.find(L"趣味") != std::wstring::npos ||
            g.find(L"教育") != std::wstring::npos) {
            return L"趣味・教育";
        }

        if (g == L"welfare" ||
            g == L"other" ||
            g == L"all" ||
            g.find(L"福祉") != std::wstring::npos) {
            return L"その他";
        }

        int major = -1;

        if (TryExtractGenreCodeFromText_(
            g,
            major)) {
            return GenreMajorNameFromCode_(major);
        }

        return L"その他";
    }

    std::wstring PickGenreKey_(
        const ProgramEntry& e)
    {
        return NormalizeGenreLabel_(
            e.genreKey);
    }

    std::wstring TrimConfigText_(
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

        return std::wstring(
            s.substr(
                first,
                last - first));
    }

    std::vector<std::wstring> SplitConfigCsvLine_(
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
                fields.push_back(
                    TrimConfigText_(current));

                current.clear();
                continue;
            }

            current.push_back(ch);
        }

        fields.push_back(
            TrimConfigText_(current));

        return fields;
    }

    bool TryParseConfigInt_(
        const std::wstring& text,
        int& out) noexcept
    {
        out = 0;

        std::wstring s =
            TrimConfigText_(text);

        if (s.empty()) {
            return false;
        }

        wchar_t* endPtr = nullptr;

        const long value =
            std::wcstol(
                s.c_str(),
                &endPtr,
                10);

        if (endPtr == s.c_str()) {
            return false;
        }

        while (endPtr && *endPtr != L'\0') {
            const wchar_t ch = *endPtr;

            if (ch != L' ' &&
                ch != L'\t' &&
                ch != L'\r' &&
                ch != L'\n' &&
                ch != L'　') {
                return false;
            }

            ++endPtr;
        }

        out = static_cast<int>(value);
        return true;
    }

    bool IsValidUtf8Bytes_(
        const char* data,
        std::size_t size) noexcept
    {
        if (!data && size > 0) {
            return false;
        }

        std::size_t i = 0;

        while (i < size) {
            const unsigned char c =
                static_cast<unsigned char>(data[i]);

            if (c <= 0x7F) {
                ++i;
                continue;
            }

            std::size_t need = 0;
            unsigned char minSecond = 0x80;
            unsigned char maxSecond = 0xBF;

            if (c >= 0xC2 && c <= 0xDF) {
                need = 1;
            }
            else if (c == 0xE0) {
                need = 2;
                minSecond = 0xA0;
            }
            else if (c >= 0xE1 && c <= 0xEC) {
                need = 2;
            }
            else if (c == 0xED) {
                need = 2;
                maxSecond = 0x9F;
            }
            else if (c >= 0xEE && c <= 0xEF) {
                need = 2;
            }
            else if (c == 0xF0) {
                need = 3;
                minSecond = 0x90;
            }
            else if (c >= 0xF1 && c <= 0xF3) {
                need = 3;
            }
            else if (c == 0xF4) {
                need = 3;
                maxSecond = 0x8F;
            }
            else {
                return false;
            }

            if (i + need >= size) {
                return false;
            }

            const unsigned char second =
                static_cast<unsigned char>(data[i + 1]);

            if (second < minSecond || second > maxSecond) {
                return false;
            }

            for (std::size_t k = 2; k <= need; ++k) {
                const unsigned char cc =
                    static_cast<unsigned char>(data[i + k]);

                if (cc < 0x80 || cc > 0xBF) {
                    return false;
                }
            }

            i += need + 1;
        }

        return true;
    }

#ifdef _WIN32
    bool DecodeBytesByCodePage_(
        const char* data,
        std::size_t size,
        UINT codePage,
        DWORD flags,
        std::wstring& out)
    {
        out.clear();

        if (size == 0) {
            return true;
        }

        if (!data ||
            size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return false;
        }

        const int srcLen =
            static_cast<int>(size);

        const int required =
            ::MultiByteToWideChar(
                codePage,
                flags,
                data,
                srcLen,
                nullptr,
                0);

        if (required <= 0) {
            return false;
        }

        out.assign(
            static_cast<std::size_t>(required),
            L'\0');

        const int written =
            ::MultiByteToWideChar(
                codePage,
                flags,
                data,
                srcLen,
                out.data(),
                required);

        return written == required;
    }
#endif

    bool DecodeUtf16Le_(
        const std::string& bytes,
        std::size_t offset,
        std::wstring& out)
    {
        out.clear();

        if (offset >= bytes.size()) {
            return true;
        }

        for (std::size_t i = offset; i + 1 < bytes.size(); i += 2) {
            const unsigned int lo =
                static_cast<unsigned char>(bytes[i]);

            const unsigned int hi =
                static_cast<unsigned char>(bytes[i + 1]);

            out.push_back(
                static_cast<wchar_t>(
                    lo | (hi << 8)));
        }

        return true;
    }

    bool DecodeUtf16Be_(
        const std::string& bytes,
        std::size_t offset,
        std::wstring& out)
    {
        out.clear();

        if (offset >= bytes.size()) {
            return true;
        }

        for (std::size_t i = offset; i + 1 < bytes.size(); i += 2) {
            const unsigned int hi =
                static_cast<unsigned char>(bytes[i]);

            const unsigned int lo =
                static_cast<unsigned char>(bytes[i + 1]);

            out.push_back(
                static_cast<wchar_t>(
                    lo | (hi << 8)));
        }

        return true;
    }

    bool ReadConfigTextFileWide_(
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

        const unsigned char b0 =
            static_cast<unsigned char>(bytes.size() > 0 ? bytes[0] : 0);

        const unsigned char b1 =
            static_cast<unsigned char>(bytes.size() > 1 ? bytes[1] : 0);

        const unsigned char b2 =
            static_cast<unsigned char>(bytes.size() > 2 ? bytes[2] : 0);

        if (bytes.size() >= 2 &&
            b0 == 0xFF &&
            b1 == 0xFE) {
            return DecodeUtf16Le_(
                bytes,
                2,
                out);
        }

        if (bytes.size() >= 2 &&
            b0 == 0xFE &&
            b1 == 0xFF) {
            return DecodeUtf16Be_(
                bytes,
                2,
                out);
        }

#ifdef _WIN32
        if (bytes.size() >= 3 &&
            b0 == 0xEF &&
            b1 == 0xBB &&
            b2 == 0xBF) {
            return DecodeBytesByCodePage_(
                bytes.data() + 3,
                bytes.size() - 3,
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                out);
        }

        if (IsValidUtf8Bytes_(
            bytes.data(),
            bytes.size())) {
            if (DecodeBytesByCodePage_(
                bytes.data(),
                bytes.size(),
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                out)) {
                return true;
            }
        }

        // 日本語Windowsでは CP_ACP = CP932/Shift-JIS 相当。
        // UTF-8として妥当でない場合だけ ANSI 側へ落とす。
        return DecodeBytesByCodePage_(
            bytes.data(),
            bytes.size(),
            CP_ACP,
            0,
            out);
#else
        for (const char ch : bytes) {
            out.push_back(
                static_cast<wchar_t>(
                    static_cast<unsigned char>(ch)));
        }

        return true;
#endif
    }

    std::vector<std::filesystem::path> BuildConfigIniCandidates_(
        const Context& ctx)
    {
        std::vector<std::filesystem::path> paths;

        if (!ctx.outputDir.empty()) {
            paths.push_back(
                ctx.outputDir /
                L"config.ini");

            paths.push_back(
                ctx.outputDir.parent_path() /
                L"config.ini");
        }

        if (!ctx.mergedEpgPath.empty()) {
            const std::filesystem::path mergedPath(
                ctx.mergedEpgPath);

            if (!mergedPath.parent_path().empty()) {
                paths.push_back(
                    mergedPath.parent_path() /
                    L"config.ini");
            }
        }

        if (!ctx.channelListPath.empty()) {
            const std::filesystem::path chPath(
                ctx.channelListPath);

            if (!chPath.parent_path().empty()) {
                paths.push_back(
                    chPath.parent_path() /
                    L"config.ini");
            }
        }

        std::error_code ec;
        const std::filesystem::path cur =
            std::filesystem::current_path(ec);

        if (!ec && !cur.empty()) {
            paths.push_back(
                cur /
                L"config.ini");
        }

        std::vector<std::filesystem::path> unique;

        for (const auto& path : paths) {
            if (path.empty()) {
                continue;
            }

            bool exists = false;

            for (const auto& item : unique) {
                if (item == path) {
                    exists = true;
                    break;
                }
            }

            if (!exists) {
                unique.push_back(path);
            }
        }

        return unique;
    }

    bool TryLoadEnabledServiceNamesFromConfig_(
        const std::filesystem::path& configPath,
        int space,
        std::set<std::wstring>& out)
    {
        std::error_code ec;

        if (configPath.empty() ||
            !std::filesystem::exists(configPath, ec) ||
            ec) {
            return false;
        }

        std::wstring text;

        if (!ReadConfigTextFileWide_(
            configPath,
            text)) {
            return false;
        }

        const std::wstring targetSection =
            L"[EpgServiceEnable_space" +
            std::to_wstring(space) +
            L"]";

        bool inTargetSection = false;
        bool foundSection = false;

        std::wistringstream iss(text);
        std::wstring line;

        while (std::getline(iss, line)) {
            const std::wstring trimmed =
                TrimConfigText_(line);

            if (trimmed.empty()) {
                continue;
            }

            const wchar_t first =
                trimmed.front();

            if (first == L'#' ||
                first == L';') {
                continue;
            }

            if (first == L'[') {
                if (trimmed == targetSection) {
                    inTargetSection = true;
                    foundSection = true;
                    continue;
                }

                if (inTargetSection) {
                    break;
                }

                continue;
            }

            if (!inTargetSection) {
                continue;
            }

            const std::size_t eq =
                trimmed.find(L'=');

            if (eq == std::wstring::npos) {
                continue;
            }

            const std::wstring valueText =
                TrimConfigText_(
                    std::wstring_view(trimmed).substr(eq + 1));

            const std::vector<std::wstring> fields =
                SplitConfigCsvLine_(valueText);

            if (fields.size() < 2) {
                continue;
            }

            int enabled = 0;

            if (!TryParseConfigInt_(
                fields[0],
                enabled)) {
                continue;
            }

            if (enabled == 0) {
                continue;
            }

            const std::wstring serviceName =
                TrimConfigText_(fields[1]);

            if (!serviceName.empty()) {
                out.insert(serviceName);
            }
        }

        return foundSection;
    }

    std::set<std::wstring> BuildEnabledServiceNamesFromConfig_(
        const Context& ctx)
    {
        std::set<std::wstring> services;

        const std::vector<std::filesystem::path> candidates =
            BuildConfigIniCandidates_(ctx);

        for (const auto& path : candidates) {
            std::set<std::wstring> loaded;

            if (!TryLoadEnabledServiceNamesFromConfig_(
                path,
                ctx.space,
                loaded)) {
                continue;
            }

            if (!loaded.empty()) {
                return loaded;
            }
        }

        return services;
    }

    int MakeDateKey_(
        int year,
        int month,
        int day) noexcept
    {
        if (year <= 0 ||
            month <= 0 ||
            month > 12 ||
            day <= 0 ||
            day > 31) {
            return 0;
        }

        return year * 10000 + month * 100 + day;
    }

    int CurrentLocalDateKey_() noexcept
    {
        std::time_t now =
            std::time(nullptr);

        if (now == static_cast<std::time_t>(-1)) {
            return 0;
        }

        std::tm localTm{};

#ifdef _WIN32
        if (::localtime_s(
            &localTm,
            &now) != 0) {
            return 0;
        }
#else
        std::tm* p =
            std::localtime(&now);

        if (!p) {
            return 0;
        }

        localTm = *p;
#endif

        return MakeDateKey_(
            localTm.tm_year + 1900,
            localTm.tm_mon + 1,
            localTm.tm_mday);
    }

    int ParseDateKeyFromText_(
        std::wstring_view text) noexcept
    {
        int nums[3] = {
            0,
            0,
            0
        };

        int index = 0;
        int value = -1;
        bool hasDigit = false;

        for (std::size_t i = 0; i <= text.size(); ++i) {
            const wchar_t ch =
                i < text.size() ? text[i] : L'\0';

            if (ch >= L'0' && ch <= L'9') {
                if (value < 0) {
                    value = 0;
                }

                value =
                    value * 10 +
                    static_cast<int>(ch - L'0');

                hasDigit = true;
                continue;
            }

            if (hasDigit) {
                if (index >= 3) {
                    return 0;
                }

                nums[index] = value;
                ++index;
                value = -1;
                hasDigit = false;
            }
        }

        if (index == 1) {
            // YYYYMMDD
            const int raw = nums[0];

            const int year = raw / 10000;
            const int month = (raw / 100) % 100;
            const int day = raw % 100;

            return MakeDateKey_(
                year,
                month,
                day);
        }

        if (index >= 3) {
            return MakeDateKey_(
                nums[0],
                nums[1],
                nums[2]);
        }

        return 0;
    }

    int ProgramDateKey_(
        const ProgramEntry& e) noexcept
    {
        int key = 0;

        if (!e.startDate.empty()) {
            key =
                ParseDateKeyFromText_(
                    e.startDate);
        }

        if (key <= 0 &&
            !e.broadcastDateKey.empty()) {
            key =
                ParseDateKeyFromText_(
                    e.broadcastDateKey);
        }

        if (key <= 0 &&
            !e.startText.empty()) {
            key =
                ParseDateKeyFromText_(
                    e.startText);
        }

        return key;
    }

    bool IsPastProgramForSearchData_(
        const ProgramEntry& e,
        int todayKey) noexcept
    {
        if (todayKey <= 0) {
            return false;
        }

        const int programKey =
            ProgramDateKey_(e);

        if (programKey <= 0) {
            return false;
        }

        return programKey < todayKey;
    }

    std::vector<const ProgramEntry*> CollectRows_(
        const Context& ctx)
    {
        std::vector<const ProgramEntry*> rows;

        if (!ctx.sharedData) {
            return rows;
        }

        const int todayKey =
            CurrentLocalDateKey_();

        rows.reserve(ctx.sharedData->entries.size());

        for (const ProgramEntry& entry : ctx.sharedData->entries) {
            if (entry.space != ctx.space) {
                continue;
            }

            if (entry.title.empty()) {
                continue;
            }

            // キーワード検索用データは、今日より前の過去番組を除外する。
            // 日付が取得できないデータは安全側で残す。
            if (IsPastProgramForSearchData_(
                entry,
                todayKey)) {
                continue;
            }

            rows.push_back(&entry);
        }

        std::sort(
            rows.begin(),
            rows.end(),
            [](const ProgramEntry* a, const ProgramEntry* b) {
                if (!a || !b) {
                    return a < b;
                }

                const int ad =
                    ProgramDateKey_(*a);

                const int bd =
                    ProgramDateKey_(*b);

                if (ad != bd) {
                    return ad < bd;
                }

                const std::wstring at =
                    PickTimeText_(*a);

                const std::wstring bt =
                    PickTimeText_(*b);

                if (at != bt) {
                    return at < bt;
                }

                const std::wstring as =
                    PickServiceName_(*a);

                const std::wstring bs =
                    PickServiceName_(*b);

                if (as != bs) {
                    return as < bs;
                }

                if (a->title != b->title) {
                    return a->title < b->title;
                }

                return a->eventId < b->eventId;
            });

        return rows;
    }

    void AppendCssLine_(
        std::wostringstream& oss,
        const wchar_t* line)
    {
        oss << line << L"\n";
    }

    void AppendKeywordSearchCss_(
        std::wostringstream& oss)
    {
        AppendCssLine_(oss, L":root{--bg:#0f131a;--panel:#181f29;--panel2:#1d2632;--line:#2d3848;--text:#e8edf5;--sub:#aab6c8;--accent:#70a7ff;--danger:#ff9b9b;}");
        AppendCssLine_(oss, L"*{box-sizing:border-box;}");
        AppendCssLine_(oss, L"body{margin:0;font-family:\"Segoe UI\",\"Yu Gothic UI\",\"Meiryo\",sans-serif;background:var(--bg);color:var(--text);}");
        AppendCssLine_(oss, L"main{padding:12px;}");
        AppendCssLine_(oss, L"h1{margin:0 0 8px;font-size:22px;}");
        AppendCssLine_(oss, L"h2{margin:0 0 8px;font-size:17px;}");
        AppendCssLine_(oss, L".panel{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:12px 14px;margin-bottom:12px;}");
        AppendCssLine_(oss, L".note,.program-sub{color:var(--sub);font-size:12px;line-height:1.55;}");

        AppendCssLine_(oss, L".keyword-layout{margin-top:12px;}");
        AppendCssLine_(oss, L".keyword-main-row{display:block;margin-bottom:10px;}");
        AppendCssLine_(oss, L".keyword-option-row{display:grid;grid-template-columns:minmax(240px,320px) minmax(420px,1fr);gap:14px;align-items:start;}");
        AppendCssLine_(oss, L".keyword-basic-options{display:grid;grid-template-columns:1fr;gap:8px;}");
        AppendCssLine_(oss, L".keyword-filter-selects{display:grid;grid-template-columns:1fr 1fr;gap:12px;align-items:start;}");

        AppendCssLine_(oss, L".field{display:flex;flex-direction:column;gap:4px;font-size:12px;color:var(--sub);}");
        AppendCssLine_(oss, L".keyword-field{width:100%;max-width:720px;}");
        AppendCssLine_(oss, L".keyword-field input{width:100%;}");

        AppendCssLine_(oss, L"input,select{background:#101720;color:var(--text);border:1px solid #43516a;border-radius:10px;padding:8px 10px;font-size:13px;}");
        AppendCssLine_(oss, L"select[multiple]{height:132px;padding:6px 8px;overflow:auto;}");
        AppendCssLine_(oss, L"select[multiple] option{padding:3px 4px;}");
        AppendCssLine_(oss, L"input:focus,select:focus{outline:2px solid rgba(112,167,255,.35);border-color:var(--accent);}");

        AppendCssLine_(oss, L".radio-box{display:flex;gap:10px;flex-wrap:wrap;align-items:center;background:#101720;border:1px solid #43516a;border-radius:10px;padding:8px 10px;min-height:36px;}");
        AppendCssLine_(oss, L".radio-item{display:inline-flex;align-items:center;gap:4px;color:var(--text);font-size:12px;white-space:nowrap;}");
        AppendCssLine_(oss, L".radio-item input{width:auto;height:auto;margin:0;padding:0;}");

        AppendCssLine_(oss, L".check-row{display:flex;gap:10px;flex-wrap:wrap;margin-top:10px;align-items:center;}");
        AppendCssLine_(oss, L".check-field{display:inline-flex;align-items:center;gap:4px;font-size:12px;color:var(--sub);}");

        AppendCssLine_(oss, L".actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px;}");
        AppendCssLine_(oss, L"button{appearance:none;border:1px solid #4b5f7f;background:#1c2b3f;color:#e8f1ff;border-radius:999px;padding:7px 12px;font-size:12px;font-weight:800;cursor:pointer;}");
        AppendCssLine_(oss, L"button:hover{border-color:#7ebaff;}");
        AppendCssLine_(oss, L"button.danger{background:#5b2424;border-color:#b85b5b;}");

        AppendCssLine_(oss, L".status{margin-top:8px;font-size:12px;color:var(--sub);}");
        AppendCssLine_(oss, L".status.error{color:var(--danger);}");

        AppendCssLine_(oss, L".rule-table{width:100%;border-collapse:collapse;font-size:12px;}");
        AppendCssLine_(oss, L".rule-table th,.rule-table td{border:1px solid var(--line);padding:6px 7px;vertical-align:top;}");
        AppendCssLine_(oss, L".rule-table th{background:#202a38;color:#fff;position:sticky;top:0;}");
        AppendCssLine_(oss, L".rule-table td{background:#151d27;}");
        AppendCssLine_(oss, L".rule-table .disabled-rule td{opacity:.55;}");

        AppendCssLine_(oss, L".program-card{background:var(--panel2);border:1px solid var(--line);border-radius:12px;padding:10px;margin-bottom:8px;}");
        AppendCssLine_(oss, L".program-title{font-weight:800;margin-bottom:4px;font-size:14px;}");
        AppendCssLine_(oss, L".program-meta{color:#d4deee;font-size:12px;line-height:1.45;margin-bottom:4px;}");
        AppendCssLine_(oss, L".program-actions{display:flex;gap:6px;flex-wrap:wrap;margin-top:8px;}");

        AppendCssLine_(oss, L".empty-box{padding:18px;border:1px dashed #425067;border-radius:12px;color:var(--sub);font-size:13px;line-height:1.7;}");
        AppendCssLine_(oss, L".result-head{display:flex;justify-content:space-between;gap:8px;align-items:center;margin-bottom:8px;}");
        AppendCssLine_(oss, L".ini-preview{width:100%;min-height:120px;margin-top:8px;background:#0d141d;color:#dfe8ff;border:1px solid #43516a;border-radius:10px;padding:8px;font-family:Consolas,\"Yu Gothic UI\",monospace;font-size:12px;}");

        AppendCssLine_(oss, L"@media(max-width:980px){.keyword-option-row{grid-template-columns:1fr;}.keyword-filter-selects{grid-template-columns:1fr 1fr;}.keyword-field{max-width:none;}}");
        AppendCssLine_(oss, L"@media(max-width:680px){.keyword-filter-selects{grid-template-columns:1fr;}}");
    }

    void AppendJsLine_(
        std::wostringstream& oss,
        const wchar_t* line)
    {
        oss << line << L"\n";
    }

    void AppendKeywordSearchScript_(
        std::wostringstream& oss)
    {
        AppendJsLine_(oss, L"(function(){");
        AppendJsLine_(oss, L"'use strict';");
        
        AppendJsLine_(oss, L"var DATA_LOADED=false;");
        AppendJsLine_(oss, L"var DATA_LOADING=false;");
        AppendJsLine_(oss, L"var AUTO_RULES=[];");

        AppendJsLine_(oss, L"function text(v){return String(v==null?'':v);}");
        AppendJsLine_(oss, L"function def(v,d){return v===undefined||v===null?d:v;}");
        AppendJsLine_(oss, L"function norm(v){return text(v).toLowerCase().replace(/\\u3000/g,' ').replace(/\\s+/g,' ').trim();}");
        AppendJsLine_(oss, L"function esc(v){return text(v).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;');}");
        AppendJsLine_(oss, L"function q(sel){return document.querySelector(sel);}");
        AppendJsLine_(oss, L"function val(sel,defv){var el=q(sel);return el?text(el.value):text(defv||'');}");
        AppendJsLine_(oss, L"function radioVal(name,defv){var el=document.querySelector('input[name=\"'+name+'\"]:checked');return el?text(el.value):text(defv||'');}");
        AppendJsLine_(oss, L"function intVal(sel,defv){var n=parseInt(val(sel,String(defv||0)),10);return Number.isFinite(n)?n:(defv||0);}");
        AppendJsLine_(oss, L"function checked(sel,defv){var el=q(sel);return el?!!el.checked:!!defv;}");
        AppendJsLine_(oss, L"function setStatus(msg,isError){var el=q('[data-rule-status]');if(!el)return;el.textContent=msg||'';if(isError)el.classList.add('error');else el.classList.remove('error');}");

        AppendJsLine_(oss, L"function normalizeAutoRule(r){r=r||{};return {id:Number(def(r.id,0))||0,enabled:def(r.enabled,true)!==false,name:text(def(r.name,'')),keyword:text(def(r.keyword,'')),excludeKeyword:text(def(r.excludeKeyword,'')),titleMatchMode:Number(def(r.titleMatchMode,0))||0,matchTitle:def(r.matchTitle,true)!==false,matchSummary:def(r.matchSummary,true)!==false,matchExtended:!!def(r.matchExtended,false),targetSpace:Number(def(r.targetSpace,0))||0,serviceName:text(def(r.serviceName,'')),genreName:text(def(r.genreName,'')),useKeyword:!!def(r.useKeyword,text(def(r.keyword,'')).length>0),keywordJoinMode:text(def(r.keywordJoinMode,'OR')),useExclude:!!def(r.useExclude,false),excludeJoinMode:text(def(r.excludeJoinMode,'OR')),useService:!!def(r.useService,text(def(r.serviceName,'')).length>0),useGenre:!!def(r.useGenre,text(def(r.genreName,'')).length>0),genreJoinMode:text(def(r.genreJoinMode,'OR')),useMinDuration:!!def(r.useMinDuration,false),minDurationMinutes:Number(def(r.minDurationMinutes,0))||0,useSearchTimeRange:!!def(r.useSearchTimeRange,false),searchStartMinutes:Number(def(r.searchStartMinutes,0))||0,searchEndMinutes:Number(def(r.searchEndMinutes,1440))||1440,searchTimeWrap:!!def(r.searchTimeWrap,false),useWeekday:!!def(r.useWeekday,false),weekdays:text(def(r.weekdays,'1111111')),priority:Number(def(r.priority,50))||50,preMarginSeconds:Number(def(r.preMarginSeconds,0))||0,postMarginSeconds:Number(def(r.postMarginSeconds,0))||0,createReservationEnabled:def(r.createReservationEnabled,true)!==false,duplicatePolicy:Number(def(r.duplicatePolicy,0))||0,skipDuplicate:def(r.skipDuplicate,true)!==false,excludeRerun:!!def(r.excludeRerun,false),actionMode:text(def(r.actionMode,'auto'))};}");
        AppendJsLine_(oss, L"AUTO_RULES=(Array.isArray(window.EPG_LITE_AUTO_RESERVE_INITIAL_RULES)?window.EPG_LITE_AUTO_RESERVE_INITIAL_RULES:[]).map(normalizeAutoRule);");

        AppendJsLine_(oss, L"function selectedList(selector,allValue){var el=q(selector);if(!el)return [allValue];var values=[];Array.prototype.slice.call(el.options||[]).forEach(function(o){if(o.selected)values.push(text(o.value));});if(!values.length)return [allValue];if(values.length>1&&values.indexOf(allValue)>=0){values=values.filter(function(v){return v!==allValue;});}if(!values.length||values.indexOf(allValue)>=0)return [allValue];return values;}");
        AppendJsLine_(oss, L"function isAllSelected(values,allValue){return !values||!values.length||values.indexOf(allValue)>=0;}");
        AppendJsLine_(oss, L"function resetSelect(selector,allValue){var el=q(selector);if(!el)return;Array.prototype.slice.call(el.options||[]).forEach(function(o){o.selected=(o.value===allValue);});}");
        AppendJsLine_(oss, L"function selectSingle(selector,value,allValue){var el=q(selector);if(!el)return;var found=false;Array.prototype.slice.call(el.options||[]).forEach(function(o){var hit=text(o.value)===text(value);o.selected=hit;if(hit)found=true;});if(!found)resetSelect(selector,allValue);}");

        AppendJsLine_(oss, L"function pageSpace(){var root=q('[data-rule-space]');var n=parseInt(root?root.getAttribute('data-rule-space'):'0',10);return Number.isFinite(n)?n:0;}");
        AppendJsLine_(oss, L"function readSearchConfig(){var genres=selectedList('[data-search-genre-list]','all');var services=selectedList('[data-search-service-list]','all');var minDurationMin=Math.max(0,intVal('[data-rule-min-duration-min]',0));return {space:pageSpace(),keyword:val('[data-search-keyword]','').trim(),genres:genres,services:services,minDurationMin:minDurationMin,matchMode:radioVal('epgLiteMatchMode','contains'),useTitle:checked('[data-rule-title]',true),useDesc:checked('[data-rule-description]',true),useService:checked('[data-rule-service-name]',false),enabled:checked('[data-rule-enabled]',true),skipDuplicate:checked('[data-rule-skip-duplicate]',true),excludeRerunLikeProgram:checked('[data-rule-exclude-rerun]',true)};}");
        AppendJsLine_(oss, L"function hasSearchCondition(cfg){if(norm(cfg.keyword))return true;if(!isAllSelected(cfg.genres,'all'))return true;if(!isAllSelected(cfg.services,'all'))return true;if(cfg.minDurationMin>0)return true;return false;}");

        AppendJsLine_(oss, L"function matchByMode(target,keyword,mode){if(!keyword)return true;if(mode==='exact')return target===keyword;if(mode==='prefix')return target.indexOf(keyword)===0;return target.indexOf(keyword)>=0;}");
        AppendJsLine_(oss, L"function matchMulti(values,value,allValue){if(isAllSelected(values,allValue))return true;return values.indexOf(value)>=0;}");
        AppendJsLine_(oss, L"function rowTargetText(row,cfg){var parts=[];if(cfg.useTitle)parts.push(row.t||'');if(cfg.useDesc)parts.push(row.x||'');if(cfg.useService)parts.push(row.s||'');if(parts.length===0)parts=[row.t||'',row.x||'',row.s||''];return norm(parts.join(' '));}");

        AppendJsLine_(oss, L"function dataSrc(){var root=q('[data-search-data-src]');return root?text(root.getAttribute('data-search-data-src')):'';}");
        AppendJsLine_(oss, L"function exposeLoadedDataForDebug(){try{window.__EPG_LITE_KEYWORD_SEARCH_DATA_READY=Array.isArray(window.__EPG_LITE_KEYWORD_SEARCH_DATA);if(window.top&&window.top!==window){window.top.__EPG_LITE_KEYWORD_SEARCH_DATA=window.__EPG_LITE_KEYWORD_SEARCH_DATA;window.top.__EPG_LITE_KEYWORD_SEARCH_DATA_READY=window.__EPG_LITE_KEYWORD_SEARCH_DATA_READY;}}catch(_){}}");
        AppendJsLine_(oss, L"function ensureDataLoaded(done){if(DATA_LOADED){done(true);return;}if(DATA_LOADING){setTimeout(function(){ensureDataLoaded(done);},50);return;}var src=dataSrc();if(!src){setStatus('検索データのパスがありません。',true);done(false);return;}DATA_LOADING=true;setStatus('検索データ読み込み中...',false);var s=document.createElement('script');s.src=src+'?v='+(Date.now());s.async=true;s.onload=function(){DATA_LOADED=Array.isArray(window.__EPG_LITE_KEYWORD_SEARCH_DATA);DATA_LOADING=false;exposeLoadedDataForDebug();if(!DATA_LOADED){setStatus('検索データ形式が不正です。',true);done(false);return;}setStatus('検索データを読み込みました。',false);done(true);};s.onerror=function(){DATA_LOADING=false;setStatus('検索データを読み込めませんでした。',true);done(false);};document.head.appendChild(s);}");

        AppendJsLine_(oss, L"function renderResultRows(rows,total){var host=q('[data-search-results]');if(!host)return;var counter=q('[data-result-counter]');if(counter)counter.textContent=String(rows.length)+' / '+String(total)+' 件';if(!rows.length){host.innerHTML='<div class=\"empty-box\">該当する番組がありません。</div>';return;}var html='';rows.forEach(function(r){html+='<article class=\"program-card\">';html+='<div class=\"program-title\">'+esc(r.t)+'</div>';html+='<div class=\"program-meta\">'+esc(r.d)+' / '+esc(r.tm)+' / '+esc(r.s)+' / '+esc(r.g);if(r.du>0)html+=' / '+esc(r.du)+'分';html+='</div>';if(r.x)html+='<div class=\"program-sub\">'+esc(r.x)+'</div>';html+='<div class=\"program-actions\"><button type=\"button\" data-fill-keyword=\"'+esc(r.t)+'\">検索欄へ入れる</button><button type=\"button\" data-rule-from-card=\"'+esc(r.t)+'\" data-rule-service=\"'+esc(r.s)+'\">このタイトルで自動予約ルール登録</button></div>';html+='</article>';});host.innerHTML=html;}");
        AppendJsLine_(oss, L"function runSearch(){ensureDataLoaded(function(ok){if(!ok)return;var cfg=readSearchConfig();var key=norm(cfg.keyword);var active=hasSearchCondition(cfg);var data=Array.isArray(window.__EPG_LITE_KEYWORD_SEARCH_DATA)?window.__EPG_LITE_KEYWORD_SEARCH_DATA:[];if(!active){var host=q('[data-search-results]');if(host)host.innerHTML='<div class=\"empty-box\">検索条件を入力または選択してください。</div>';var counter=q('[data-result-counter]');if(counter)counter.textContent='検索条件を入力または選択してください';return;}var out=[];for(var i=0;i<data.length;i++){var r=data[i]||{};var genreOk=matchMulti(cfg.genres,text(r.g||'その他'),'all');var serviceOk=matchMulti(cfg.services,text(r.s||''),'all');var du=parseInt(r.du||'0',10);if(!Number.isFinite(du))du=0;var durationOk=cfg.minDurationMin<=0||(du>0&&du>=cfg.minDurationMin);var textOk=matchByMode(rowTargetText(r,cfg),key,cfg.matchMode);if(genreOk&&serviceOk&&durationOk&&textOk){out.push(r);if(out.length>=300)break;}}renderResultRows(out,data.length);});}");

        AppendJsLine_(oss, L"function nextAutoRuleId(){var m=0;AUTO_RULES.forEach(function(r){m=Math.max(m,Number(r.id)||0);});return m+1;}");
        AppendJsLine_(oss, L"function titleMatchModeNumber(mode){if(mode==='exact')return 1;if(mode==='prefix')return 2;if(mode==='suffix')return 3;return 0;}");
        AppendJsLine_(oss, L"function listValue(values,allValue){if(isAllSelected(values,allValue))return '';return values.filter(function(v){return v!==allValue;}).join('|');}");
        AppendJsLine_(oss, L"function iniValue(v){return text(v).replace(/[\\r\\n]+/g,' ').trim();}");
        AppendJsLine_(oss, L"function iniLine(k,v){return k+'='+iniValue(v)+'\\n';}");
        AppendJsLine_(oss, L"function encodeForm(payload){var a=[];Object.keys(payload||{}).forEach(function(k){var v=payload[k];if(v===undefined||v===null)return;a.push(encodeURIComponent(k)+'='+encodeURIComponent(String(v)));});return a.join('&');}");
        AppendJsLine_(oss, L"function apiUrl(){var base='';try{if(window.EPG_RUNTIME&&window.EPG_RUNTIME.reservationApiBase)base=String(window.EPG_RUNTIME.reservationApiBase);}catch(e){}try{if(!base&&window.top&&window.top!==window&&window.top.EPG_RUNTIME&&window.top.EPG_RUNTIME.reservationApiBase)base=String(window.top.EPG_RUNTIME.reservationApiBase);}catch(e){}return base?base+'/auto_reserve_rules_save':'/api/reservation/auto_reserve_rules_save';}");

        AppendJsLine_(oss, L"function newAutoRuleFromControls(){var cfg=readSearchConfig();if(!hasSearchCondition(cfg))return null;var serviceName=listValue(cfg.services,'all');var genreName=listValue(cfg.genres,'all');var keyword=cfg.keyword;var name=keyword||genreName||serviceName||'自動予約ルール';return normalizeAutoRule({id:nextAutoRuleId(),enabled:cfg.enabled,name:name,keyword:keyword,useKeyword:!!keyword,keywordJoinMode:'OR',titleMatchMode:titleMatchModeNumber(cfg.matchMode),matchTitle:cfg.useTitle,matchSummary:cfg.useDesc,matchExtended:false,targetSpace:cfg.space,serviceName:serviceName,useService:!!serviceName,genreName:genreName,useGenre:!!genreName,genreJoinMode:'OR',useMinDuration:cfg.minDurationMin>0,minDurationMinutes:cfg.minDurationMin,useSearchTimeRange:false,searchStartMinutes:0,searchEndMinutes:1440,searchTimeWrap:false,useWeekday:false,weekdays:'1111111',priority:50,preMarginSeconds:0,postMarginSeconds:0,createReservationEnabled:true,duplicatePolicy:cfg.skipDuplicate?1:2,skipDuplicate:cfg.skipDuplicate,excludeRerun:cfg.excludeRerunLikeProgram,actionMode:'auto'});}");

        AppendJsLine_(oss, L"function makeAutoReserveIniText(){var out='[AutoReserveRules]\\n';out+=iniLine('Count',AUTO_RULES.length);out+=iniLine('NextId',nextAutoRuleId());out+='\\n';AUTO_RULES.forEach(function(r0,i){var r=normalizeAutoRule(r0);out+='[AutoReserveRule_'+String(i)+']\\n';out+=iniLine('Id',r.id);out+=iniLine('Enabled',r.enabled?1:0);out+=iniLine('Name',r.name);out+=iniLine('UseKeyword',r.useKeyword?1:0);out+=iniLine('Keyword',r.keyword);out+=iniLine('KeywordJoinMode',r.keywordJoinMode);out+=iniLine('UseExcludeKeyword',r.useExclude?1:0);out+=iniLine('ExcludeKeyword',r.excludeKeyword);out+=iniLine('ExcludeKeywordJoinMode',r.excludeJoinMode);out+=iniLine('TitleMatchMode',r.titleMatchMode);out+=iniLine('MatchTitle',r.matchTitle?1:0);out+=iniLine('MatchSummary',r.matchSummary?1:0);out+=iniLine('MatchExtended',r.matchExtended?1:0);out+=iniLine('TargetSpace',r.targetSpace);out+=iniLine('UseService',r.useService?1:0);out+=iniLine('ServiceName',r.serviceName);out+=iniLine('ServiceMatchMode','OR');out+=iniLine('UseGenre',r.useGenre?1:0);out+=iniLine('GenreName',r.genreName);out+=iniLine('GenreMatchMode',r.genreJoinMode);out+=iniLine('UseMinDuration',r.useMinDuration?1:0);out+=iniLine('MinDurationMinutes',r.minDurationMinutes);out+=iniLine('UseSearchTimeRange',r.useSearchTimeRange?1:0);out+=iniLine('SearchStartMinutes',r.searchStartMinutes);out+=iniLine('SearchEndMinutes',r.searchEndMinutes);out+=iniLine('SearchTimeWrap',r.searchTimeWrap?1:0);out+=iniLine('StartMinutes',r.useSearchTimeRange?r.searchStartMinutes:-1);out+=iniLine('EndMinutes',r.useSearchTimeRange?r.searchEndMinutes:-1);out+=iniLine('UseWeekday',r.useWeekday?1:0);out+=iniLine('Weekdays',r.weekdays);out+=iniLine('Priority',r.priority);out+=iniLine('PreMarginSeconds',r.preMarginSeconds);out+=iniLine('PostMarginSeconds',r.postMarginSeconds);out+=iniLine('CreateReservationEnabled',r.createReservationEnabled?1:0);out+=iniLine('DuplicatePolicy',r.duplicatePolicy);out+=iniLine('SkipDuplicate',r.skipDuplicate?1:0);out+=iniLine('ExcludeRerunLikeProgram',r.excludeRerun?1:0);out+=iniLine('ActionMode',r.actionMode);out+='\\n';});return out;}");
        AppendJsLine_(oss, L"function saveAutoRules(){var iniText=makeAutoReserveIniText();setStatus('自動予約ルール保存中...',false);return fetch(apiUrl(),{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded; charset=UTF-8'},body:encodeForm({iniText:iniText}),cache:'no-store'}).then(function(res){return res.text().then(function(t){return {ok:res.ok,text:t,status:res.status,url:res.url};});}).then(function(r){var obj=null;try{obj=JSON.parse(r.text||'{}');}catch(_){}if(!r.ok||(obj&&obj.ok===false)){throw new Error((obj&&(obj.reason||obj.message))||('HTTP '+String(r.status)));}setStatus('auto_reserve_rules.ini へ自動予約ルールを登録しました。',false);return true;}).catch(function(e){setStatus('保存失敗: '+String(e&&e.message?e.message:e),true);return false;});}");
        
        AppendJsLine_(oss, L"function addAutoRuleFromControls(){var rule=newAutoRuleFromControls();if(!rule){setStatus('キーワード、ジャンル、放送局、最小放送時間のいずれかを指定してください。',true);return;}AUTO_RULES.push(rule);saveAutoRules().then(function(ok){if(!ok){AUTO_RULES=AUTO_RULES.filter(function(r){return Number(r.id)!==Number(rule.id);});}});}");
        AppendJsLine_(oss, L"function addRuleFromTitle(title,service){var el=q('[data-search-keyword]');if(el)el.value=text(title);if(service)selectSingle('[data-search-service-list]',service,'all');addAutoRuleFromControls();}");
        AppendJsLine_(oss, L"function fillKeyword(title){var el=q('[data-search-keyword]');if(el){el.value=text(title);el.focus();}}");

        AppendJsLine_(oss, L"document.addEventListener('click',function(ev){");
        AppendJsLine_(oss, L"var t=ev.target;");
        AppendJsLine_(oss, L"if(!t||!t.closest)return;");
        AppendJsLine_(oss, L"var run=t.closest('[data-run-search]');");
        AppendJsLine_(oss, L"if(run){ev.preventDefault();runSearch();return;}");
        AppendJsLine_(oss, L"var add=t.closest('[data-add-auto-rule]');");
        AppendJsLine_(oss, L"if(add){ev.preventDefault();addAutoRuleFromControls();return;}");
        AppendJsLine_(oss, L"var clear=t.closest('[data-clear-search]');");
        AppendJsLine_(oss, L"if(clear){ev.preventDefault();var el=q('[data-search-keyword]');if(el)el.value='';var md=q('[data-rule-min-duration-min]');if(md)md.value='0';resetSelect('[data-search-genre-list]','all');resetSelect('[data-search-service-list]','all');var host=q('[data-search-results]');if(host)host.innerHTML='<div class=\"empty-box\">検索ボタンを押すと検索データを読み込んで表示します。</div>';var counter=q('[data-result-counter]');if(counter)counter.textContent='検索条件を入力してください';setStatus('',false);return;}");
        AppendJsLine_(oss, L"var fill=t.closest('[data-fill-keyword]');");
        AppendJsLine_(oss, L"if(fill){ev.preventDefault();fillKeyword(fill.getAttribute('data-fill-keyword')||'');return;}");
        AppendJsLine_(oss, L"var from=t.closest('[data-rule-from-card]');");
        AppendJsLine_(oss, L"if(from){ev.preventDefault();addRuleFromTitle(from.getAttribute('data-rule-from-card')||'',from.getAttribute('data-rule-service')||'');return;}");
        AppendJsLine_(oss, L"});");

        AppendJsLine_(oss, L"document.addEventListener('DOMContentLoaded',function(){");
        AppendJsLine_(oss, L"resetSelect('[data-search-genre-list]','all');");
        AppendJsLine_(oss, L"resetSelect('[data-search-service-list]','all');");
        AppendJsLine_(oss, L"});");

        AppendJsLine_(oss, L"})();");
    }

    void AppendOption_(
        std::wostringstream& oss,
        const std::wstring& value,
        const std::wstring& label)
    {
        oss << L"<option value=\""
            << HtmlEscape_(value)
            << L"\">"
            << HtmlEscape_(label)
            << L"</option>\n";
    }

    void AppendSearchRulePanel_(
        std::wostringstream& oss,
        int space,
        std::size_t rowCount,
        const std::set<std::wstring>& genres,
        const std::set<std::wstring>& services)
    {
        (void)genres;

        oss << L"<section class=\"panel keyword-rule-panel\"";
        oss << L" data-search-data-src=\""
            << HtmlEscape_(DataFileNameFor_(space))
            << L"\"";
        oss << L" data-rule-space=\""
            << space
            << L"\">\n";

        oss << L"<h1>キーワード検索 / 自動登録</h1>\n";
        oss << L"<div class=\"note\">Lite / "
            << HtmlEscape_(MakeSpaceLabel_(space))
            << L" / 検索対象件数="
            << rowCount
            << L"</div>\n";

        oss << L"<div class=\"keyword-layout\">\n";

        oss << L"<div class=\"keyword-main-row\">\n";
        oss << L"<label class=\"field keyword-field\">";
        oss << L"<span>キーワード</span>";
        oss << L"<input type=\"text\" data-search-keyword data-rule-keyword";
        oss << L" placeholder=\"番組名・説明・放送局で検索 / 自動登録ワード\">";
        oss << L"</label>\n";
        oss << L"</div>\n";

        oss << L"<div class=\"keyword-option-row\">\n";

        oss << L"<div class=\"keyword-basic-options\">\n";

        oss << L"<div class=\"field\">";
        oss << L"<span>一致方式</span>";
        oss << L"<div class=\"radio-box\" data-rule-match-mode-group>";
        oss << L"<label class=\"radio-item\"><input type=\"radio\" name=\"epgLiteMatchMode\" value=\"contains\" data-rule-match-mode checked>含む</label>";
        oss << L"<label class=\"radio-item\"><input type=\"radio\" name=\"epgLiteMatchMode\" value=\"exact\" data-rule-match-mode>完全一致</label>";
        oss << L"<label class=\"radio-item\"><input type=\"radio\" name=\"epgLiteMatchMode\" value=\"prefix\" data-rule-match-mode>前方一致</label>";
        oss << L"</div>";
        oss << L"</div>\n";

        

        oss << L"<label class=\"field\">";
        oss << L"<span>最小放送時間</span>";
        oss << L"<input type=\"number\"";
        oss << L" min=\"0\"";
        oss << L" max=\"1440\"";
        oss << L" step=\"1\"";
        oss << L" value=\"0\"";
        oss << L" data-rule-min-duration-min";
        oss << L" data-search-min-duration-min";
        oss << L" placeholder=\"0=制限なし\">";
        oss << L"</label>\n";

        oss << L"</div>\n";

        oss << L"<div class=\"keyword-filter-selects\">\n";

        oss << L"<label class=\"field\">";
        oss << L"<span>ジャンル</span>";
        oss << L"<select multiple size=\"10\" data-search-genre-list>";
        oss << L"<option value=\"all\" selected>全ジャンル</option>\n";

        for (std::wstring_view genre : LiteGenreMajorListForSearch_()) {
            oss << L"<option value=\""
                << HtmlEscape_(genre)
                << L"\">"
                << HtmlEscape_(genre)
                << L"</option>\n";
        }

        oss << L"</select>";
        oss << L"</label>\n";

        oss << L"<label class=\"field\">";
        oss << L"<span>放送局</span>";
        oss << L"<select multiple size=\"10\" data-search-service-list>";
        oss << L"<option value=\"all\" selected>全局</option>\n";

        for (const std::wstring& service : services) {
            if (service.empty()) {
                continue;
            }

            oss << L"<option value=\""
                << HtmlEscape_(service)
                << L"\">"
                << HtmlEscape_(service)
                << L"</option>\n";
        }

        oss << L"</select>";
        oss << L"</label>\n";

        oss << L"</div>\n";
        oss << L"</div>\n";
        oss << L"</div>\n";

        oss << L"<div class=\"check-row\">\n";
        oss << L"<label class=\"check-field\"><input type=\"checkbox\" data-rule-title checked>タイトル</label>\n";
        oss << L"<label class=\"check-field\"><input type=\"checkbox\" data-rule-description checked>説明</label>\n";
        oss << L"<label class=\"check-field\"><input type=\"checkbox\" data-rule-service-name>局名</label>\n";
        oss << L"<label class=\"check-field\"><input type=\"checkbox\" data-rule-enabled checked>有効</label>\n";
        oss << L"<label class=\"check-field\"><input type=\"checkbox\" data-rule-skip-duplicate checked>重複回避</label>\n";
        oss << L"<label class=\"check-field\"><input type=\"checkbox\" data-rule-exclude-rerun checked>再放送らしき番組を除外</label>\n";
        oss << L"</div>\n";

        oss << L"<div class=\"actions\">\n";
        oss << L"<button type=\"button\" data-run-search>検索</button>\n";
        oss << L"<button type=\"button\" data-add-auto-rule>この条件を auto_reserve_rules.ini へ登録</button>\n";
        oss << L"<button type=\"button\" data-clear-search>検索クリア</button>\n";
        oss << L"</div>\n";

        oss << L"<div class=\"status\" data-rule-status></div>\n";
        oss << L"</section>\n";
    }

    

    

    bool ContainsText_(
        std::wstring_view text,
        std::wstring_view needle) noexcept
    {
        return text.find(needle) != std::wstring_view::npos;
    }

    void ReplaceAllText_(
        std::wstring& text,
        std::wstring_view from,
        std::wstring_view to)
    {
        if (from.empty()) {
            return;
        }

        std::size_t pos = 0;

        while ((pos = text.find(from, pos)) != std::wstring::npos) {
            text.replace(
                pos,
                from.size(),
                to);

            pos += to.size();
        }
    }

    std::wstring SearchServiceGroupName_(
        std::wstring_view serviceName)
    {
        std::wstring s(serviceName);

        ReplaceAllText_(s, L"ＮＨＫ", L"NHK");
        ReplaceAllText_(s, L"Ｅテレ１", L"Eテレ");
        ReplaceAllText_(s, L"Ｅテレ２", L"Eテレ");
        ReplaceAllText_(s, L"Ｅテレ３", L"Eテレ");
        ReplaceAllText_(s, L"Eテレ1", L"Eテレ");
        ReplaceAllText_(s, L"Eテレ2", L"Eテレ");
        ReplaceAllText_(s, L"Eテレ3", L"Eテレ");
        ReplaceAllText_(s, L"教育１", L"教育");
        ReplaceAllText_(s, L"教育２", L"教育");
        ReplaceAllText_(s, L"教育３", L"教育");
        ReplaceAllText_(s, L"教育1", L"教育");
        ReplaceAllText_(s, L"教育2", L"教育");
        ReplaceAllText_(s, L"教育3", L"教育");
        ReplaceAllText_(s, L"総合１", L"総合");
        ReplaceAllText_(s, L"総合２", L"総合");
        ReplaceAllText_(s, L"総合３", L"総合");
        ReplaceAllText_(s, L"総合1", L"総合");
        ReplaceAllText_(s, L"総合2", L"総合");
        ReplaceAllText_(s, L"総合3", L"総合");

        return s;
    }

    int SearchServicePreferenceScore_(
        std::wstring_view serviceName) noexcept
    {
        int score = 0;

        if (ContainsText_(serviceName, L"ワンセグ")) {
            score += 1000;
        }

        if (ContainsText_(serviceName, L"サブ") ||
            ContainsText_(serviceName, L"臨時")) {
            score += 500;
        }

        if (ContainsText_(serviceName, L"Eテレ3") ||
            ContainsText_(serviceName, L"Ｅテレ３") ||
            ContainsText_(serviceName, L"教育3") ||
            ContainsText_(serviceName, L"教育３") ||
            ContainsText_(serviceName, L"総合3") ||
            ContainsText_(serviceName, L"総合３")) {
            score += 30;
        }
        else if (ContainsText_(serviceName, L"Eテレ2") ||
            ContainsText_(serviceName, L"Ｅテレ２") ||
            ContainsText_(serviceName, L"教育2") ||
            ContainsText_(serviceName, L"教育２") ||
            ContainsText_(serviceName, L"総合2") ||
            ContainsText_(serviceName, L"総合２")) {
            score += 20;
        }
        else if (ContainsText_(serviceName, L"Eテレ1") ||
            ContainsText_(serviceName, L"Ｅテレ１") ||
            ContainsText_(serviceName, L"教育1") ||
            ContainsText_(serviceName, L"教育１") ||
            ContainsText_(serviceName, L"総合1") ||
            ContainsText_(serviceName, L"総合１")) {
            score -= 10;
        }

        return score;
    }

    std::wstring SearchDuplicateKey_(
        const ProgramEntry& e)
    {
        std::wstring key;

        key.reserve(
            e.title.size() +
            128);

        key += std::to_wstring(
            ProgramDateKey_(e));

        key += L"|";
        key += PickTimeText_(e);

        key += L"|";
        key += std::to_wstring(
            ProgramDurationMinutes_(e));

        key += L"|";
        key += SearchServiceGroupName_(
            PickServiceName_(e));

        key += L"|";
        key += PickGenreKey_(e);

        key += L"|";
        key += TrimConfigText_(e.title);

        return key;
    }

    int AutoRuleMatchModeNumber_(
        std::wstring_view mode) noexcept
    {
        if (mode == L"exact") {
            return 1;
        }

        if (mode == L"prefix") {
            return 2;
        }

        if (mode == L"suffix") {
            return 3;
        }

        return 0;
    }

    std::wstring AutoRuleActionText_(
        AutoReserve::ReserveActionMode mode)
    {
        return mode == AutoReserve::ReserveActionMode::Auto
            ? L"auto"
            : L"candidate";
    }

    std::wstring AutoRuleJoinModeText_(
        AutoReserve::TokenJoinMode mode)
    {
        return mode == AutoReserve::TokenJoinMode::And
            ? L"AND"
            : L"OR";
    }

    int AutoRuleDuplicatePolicyNumber_(
        AutoReserve::DuplicatePolicy policy) noexcept
    {
        switch (policy) {
        case AutoReserve::DuplicatePolicy::SkipSameTitleAndTime:
            return 1;
        case AutoReserve::DuplicatePolicy::Allow:
            return 2;
        case AutoReserve::DuplicatePolicy::SkipSameEvent:
        default:
            return 0;
        }
    }

    int AutoRuleTitleMatchModeNumber_(
        AutoReserve::MatchMode mode) noexcept
    {
        switch (mode) {
        case AutoReserve::MatchMode::Exact:
            return 1;
        case AutoReserve::MatchMode::StartsWith:
            return 2;
        case AutoReserve::MatchMode::EndsWith:
            return 3;
        case AutoReserve::MatchMode::Contains:
        default:
            return 0;
        }
    }

    void AppendAutoRuleJsObject_(
        std::wostringstream& oss,
        const AutoReserve::Rule& r)
    {
        oss << L"{";
        oss << L"id:" << r.id << L",";
        oss << L"enabled:" << (r.enabled ? L"true" : L"false") << L",";
        oss << L"name:'" << JsEscape_(r.name) << L"',";
        oss << L"keyword:'" << JsEscape_(r.keyword) << L"',";
        oss << L"excludeKeyword:'" << JsEscape_(r.excludeKeyword) << L"',";
        oss << L"titleMatchMode:" << AutoRuleTitleMatchModeNumber_(r.titleMatchMode) << L",";
        oss << L"matchTitle:" << (r.matchTitle ? L"true" : L"false") << L",";
        oss << L"matchSummary:" << (r.matchSummary ? L"true" : L"false") << L",";
        oss << L"matchExtended:" << (r.matchExtended ? L"true" : L"false") << L",";
        oss << L"targetSpace:" << r.targetSpace << L",";
        oss << L"serviceName:'" << JsEscape_(r.serviceName) << L"',";
        oss << L"genreName:'" << JsEscape_(r.genreName) << L"',";
        oss << L"useKeyword:" << (r.useKeyword ? L"true" : L"false") << L",";
        oss << L"keywordJoinMode:'" << JsEscape_(AutoRuleJoinModeText_(r.keywordJoinMode)) << L"',";
        oss << L"useExclude:" << (r.useExcludeKeyword ? L"true" : L"false") << L",";
        oss << L"excludeJoinMode:'" << JsEscape_(AutoRuleJoinModeText_(r.excludeKeywordJoinMode)) << L"',";
        oss << L"useService:" << (r.useService ? L"true" : L"false") << L",";
        oss << L"useGenre:" << (r.useGenre ? L"true" : L"false") << L",";
        oss << L"genreJoinMode:'" << JsEscape_(AutoRuleJoinModeText_(r.genreMatchMode)) << L"',";
        oss << L"useMinDuration:" << (r.useMinDuration ? L"true" : L"false") << L",";
        oss << L"minDurationMinutes:" << r.minDurationMinutes << L",";
        oss << L"useSearchTimeRange:" << (r.useSearchTimeRange ? L"true" : L"false") << L",";
        oss << L"searchStartMinutes:" << r.searchStartMinutes << L",";
        oss << L"searchEndMinutes:" << r.searchEndMinutes << L",";
        oss << L"searchTimeWrap:" << (r.searchTimeWrap ? L"true" : L"false") << L",";
        oss << L"useWeekday:" << (r.useWeekday ? L"true" : L"false") << L",";
        oss << L"weekdays:'" << JsEscape_(r.weekdays.empty() ? std::wstring(L"1111111") : r.weekdays) << L"',";
        oss << L"priority:" << r.priority << L",";
        oss << L"preMarginSeconds:" << r.preMarginSeconds << L",";
        oss << L"postMarginSeconds:" << r.postMarginSeconds << L",";
        oss << L"createReservationEnabled:" << (r.createReservationEnabled ? L"true" : L"false") << L",";
        oss << L"duplicatePolicy:" << AutoRuleDuplicatePolicyNumber_(r.duplicatePolicy) << L",";
        oss << L"skipDuplicate:" << (r.skipDuplicate ? L"true" : L"false") << L",";
        oss << L"excludeRerun:" << (r.excludeRerunLikeProgram ? L"true" : L"false") << L",";
        oss << L"actionMode:'" << JsEscape_(AutoRuleActionText_(r.actionMode)) << L"'";
        oss << L"}";
    }

    void AppendInitialAutoRulesScript_(
        std::wostringstream& oss,
        const std::vector<AutoReserve::Rule>& rules)
    {
        oss << L"<script>\n";
        oss << L"window.EPG_LITE_AUTO_RESERVE_INITIAL_RULES=[\n";

        for (std::size_t i = 0; i < rules.size(); ++i) {
            oss << L"  ";
            AppendAutoRuleJsObject_(
                oss,
                rules[i]);

            if (i + 1 < rules.size()) {
                oss << L",";
            }

            oss << L"\n";
        }

        oss << L"];\n";
        oss << L"</script>\n";
    }

    std::wstring BuildSearchDataScript_(
        const Context& ctx)
    {
        const std::vector<const ProgramEntry*> rows =
            CollectRows_(ctx);

        std::map<std::wstring, const ProgramEntry*> selected;

        for (const ProgramEntry* p : rows) {
            if (!p) {
                continue;
            }

            const std::wstring key =
                SearchDuplicateKey_(*p);

            const auto it =
                selected.find(key);

            if (it == selected.end()) {
                selected.emplace(
                    key,
                    p);

                continue;
            }

            const int newScore =
                SearchServicePreferenceScore_(
                    PickServiceName_(*p));

            const int oldScore =
                SearchServicePreferenceScore_(
                    PickServiceName_(*it->second));

            if (newScore < oldScore) {
                it->second = p;
            }
        }

        std::wostringstream oss;

        oss << L"window.__EPG_LITE_KEYWORD_SEARCH_DATA = [\n";

        bool first = true;

        for (const auto& item : selected) {
            const ProgramEntry* p =
                item.second;

            if (!p) {
                continue;
            }

            const ProgramEntry& e = *p;

            const std::wstring serviceName =
                PickServiceName_(e);

            const std::wstring genre =
                PickGenreKey_(e);

            const std::wstring dateText =
                PickDateText_(e);

            const std::wstring timeText =
                PickTimeText_(e);

            const int durationMinutes =
                ProgramDurationMinutes_(e);

            if (!first) {
                oss << L",\n";
            }

            first = false;

            oss << L"{";
            oss << L"t:'" << JsEscape_(e.title) << L"',";
            oss << L"s:'" << JsEscape_(serviceName) << L"',";
            oss << L"g:'" << JsEscape_(genre) << L"',";
            oss << L"d:'" << JsEscape_(dateText) << L"',";
            oss << L"tm:'" << JsEscape_(timeText) << L"',";
            oss << L"du:" << durationMinutes << L",";
            oss << L"x:'" << JsEscape_(ShortText_(e.description, 240)) << L"'";
            oss << L"}";
        }

        oss << L"\n];\n";

        return oss.str();
    }

    void AppendSearchResultsPanel_(
        std::wostringstream& oss,
        const std::vector<const ProgramEntry*>& rows)
    {
        (void)rows;

        oss << L"<section class=\"panel\">\n";
        oss << L"<div class=\"result-head\">";
        oss << L"<h2>検索結果</h2>";
        oss << L"<div class=\"note\" data-result-counter>";
        oss << L"検索条件を入力して検索してください";
        oss << L"</div>";
        oss << L"</div>\n";

        oss << L"<div data-search-results>\n";
        oss << L"<div class=\"empty-box\">";
        oss << L"検索ボタンを押すと検索データを読み込んで表示します。";
        oss << L"</div>\n";
        oss << L"</div>\n";

        oss << L"</section>\n";
    }

    std::wstring BuildHtml_(
        const Context& ctx,
        const std::vector<AutoReserve::Rule>& autoRules)
    {
        const std::vector<const ProgramEntry*> rows =
            CollectRows_(ctx);

        std::set<std::wstring> genres;

        std::set<std::wstring> services =
            BuildEnabledServiceNamesFromConfig_(ctx);

        for (const ProgramEntry* p : rows) {
            if (!p) {
                continue;
            }

            if (services.empty()) {
                services.insert(
                    PickServiceName_(*p));
            }
        }

        std::wostringstream oss;

        oss << L"<!doctype html>\n";
        oss << L"<html lang=\"ja\">\n";
        oss << L"<head>\n";
        oss << L"<meta charset=\"utf-8\">\n";
        oss << L"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n";
        oss << L"<title>キーワード検索 / 自動登録</title>\n";
        oss << L"<style>\n";
        AppendKeywordSearchCss_(oss);
        oss << L"</style>\n";
        oss << L"</head>\n";
        oss << L"<body>\n";
        oss << L"<main>\n";

        AppendSearchRulePanel_(
            oss,
            ctx.space,
            rows.size(),
            genres,
            services);

        AppendSearchResultsPanel_(
            oss,
            rows);

        oss << L"</main>\n";

        AppendInitialAutoRulesScript_(
            oss,
            autoRules);

        oss << L"<script>\n";
        AppendKeywordSearchScript_(oss);
        oss << L"</script>\n";

        oss << L"</body>\n";
        oss << L"</html>\n";

        return oss.str();
    }

}

namespace EpgHtmlLite::PageKeywordSearch
{
    bool Generate(
        const Context& ctx,
        Result& ioResult)
    {
        try {
            AutoReserve::RuleStore store;

            const std::filesystem::path rulesIniPath =
                AutoReserve::GetDefaultRulesIniPath();

            const auto load =
                store.LoadFromIni(
                    rulesIniPath.wstring());

            if (!load.ok) {
                const auto initSave =
                    store.SaveToIni(
                        rulesIniPath.wstring());

                (void)initSave;
            }

            const std::vector<AutoReserve::Rule> autoRules =
                store.GetRules();

            const std::filesystem::path htmlPath =
                ctx.outputDir /
                std::filesystem::path(FileNameFor_(ctx.space));

            if (!WriteUtf8BomTextFile_(
                htmlPath,
                BuildHtml_(
                    ctx,
                    autoRules))) {
                return false;
            }

            ioResult.generatedFiles.push_back(
                htmlPath.wstring());

            const std::filesystem::path dataPath =
                ctx.outputDir /
                std::filesystem::path(DataFileNameFor_(ctx.space));

            if (!WriteUtf8BomTextFile_(
                dataPath,
                BuildSearchDataScript_(ctx))) {
                return false;
            }

            ioResult.generatedFiles.push_back(
                dataPath.wstring());

            return true;
        }
        catch (...) {
            return false;
        }
    }
}