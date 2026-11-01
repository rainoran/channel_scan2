#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "SIMDData.hpp"

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <intrin.h>
#include <limits>
#include <string>

#include "ConfigManager.h"

namespace
{
    struct EpgEventWork_
    {
        std::uint32_t tableId = 0;
        std::uint32_t serviceId = 0;
        std::uint32_t eventId = 0;
        std::uint32_t tsId = 0;
        std::uint32_t onId = 0;
        std::uint64_t startRaw40 = 0;
        std::uint32_t durationSec = 0;
        std::uint32_t genreMajor = 0xFF;
        std::uint32_t genreMinor = 0xFF;

        std::wstring title;
        std::wstring description;

        bool hasHeader = false;
        bool hasStart = false;
        bool hasDuration = false;
    };

    std::wstring UpperAscii_(std::wstring_view s)
    {
        std::wstring out(s);

        for (wchar_t& ch : out) {
            if (ch >= L'a' && ch <= L'z') {
                ch = static_cast<wchar_t>(ch - L'a' + L'A');
            }
        }

        return out;
    }

    std::wstring TrimCopy_(std::wstring_view s)
    {
        while (!s.empty() &&
            (s.front() == L' ' ||
                s.front() == L'\t' ||
                s.front() == L'\r' ||
                s.front() == L'\n')) {
            s.remove_prefix(1);
        }

        while (!s.empty() &&
            (s.back() == L' ' ||
                s.back() == L'\t' ||
                s.back() == L'\r' ||
                s.back() == L'\n')) {
            s.remove_suffix(1);
        }

        return std::wstring(s);
    }

    bool StartsWith_(std::wstring_view s, std::wstring_view prefix) noexcept
    {
        return s.size() >= prefix.size() &&
            s.substr(0, prefix.size()) == prefix;
    }

    bool ParseUnsignedDecAt_(
        std::wstring_view s,
        std::size_t pos,
        std::uint64_t& out) noexcept
    {
        out = 0;

        if (pos >= s.size()) {
            return false;
        }

        bool any = false;

        for (std::size_t i = pos; i < s.size(); ++i) {
            const wchar_t ch = s[i];

            if (ch < L'0' || ch > L'9') {
                break;
            }

            any = true;

            const std::uint64_t digit =
                static_cast<std::uint64_t>(ch - L'0');

            if (out >
                (std::numeric_limits<std::uint64_t>::max() - digit) / 10ull) {
                return false;
            }

            out = out * 10ull + digit;
        }

        return any;
    }

    bool ParseUnsignedHexAt_(
        std::wstring_view s,
        std::size_t pos,
        std::uint64_t& out) noexcept
    {
        out = 0;

        if (pos >= s.size()) {
            return false;
        }

        bool any = false;

        for (std::size_t i = pos; i < s.size(); ++i) {
            const wchar_t ch = s[i];

            std::uint64_t digit = 0;

            if (ch >= L'0' && ch <= L'9') {
                digit = static_cast<std::uint64_t>(ch - L'0');
            }
            else if (ch >= L'a' && ch <= L'f') {
                digit = static_cast<std::uint64_t>(ch - L'a' + 10);
            }
            else if (ch >= L'A' && ch <= L'F') {
                digit = static_cast<std::uint64_t>(ch - L'A' + 10);
            }
            else {
                break;
            }

            any = true;

            if (out >
                (std::numeric_limits<std::uint64_t>::max() - digit) / 16ull) {
                return false;
            }

            out = out * 16ull + digit;
        }

        return any;
    }

    bool ParseDecAfter_(
        std::wstring_view line,
        std::wstring_view key,
        std::uint32_t& out) noexcept
    {
        const std::size_t p = line.find(key);

        if (p == std::wstring_view::npos) {
            return false;
        }

        std::size_t pos = p + key.size();

        while (pos < line.size() &&
            (line[pos] == L' ' || line[pos] == L'\t')) {
            ++pos;
        }

        std::uint64_t v = 0;

        if (!ParseUnsignedDecAt_(line, pos, v)) {
            return false;
        }

        if (v > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }

        out = static_cast<std::uint32_t>(v);
        return true;
    }

    bool ParseHexAfter_(
        std::wstring_view line,
        std::wstring_view key,
        std::uint64_t& out) noexcept
    {
        const std::size_t p = line.find(key);

        if (p == std::wstring_view::npos) {
            return false;
        }

        std::size_t pos = p + key.size();

        while (pos < line.size() &&
            (line[pos] == L' ' || line[pos] == L'\t')) {
            ++pos;
        }

        if (pos + 2 <= line.size() &&
            line[pos] == L'0' &&
            (line[pos + 1] == L'x' || line[pos + 1] == L'X')) {
            pos += 2;
        }

        return ParseUnsignedHexAt_(line, pos, out);
    }

    bool ParseDurationLine_(
        std::wstring_view line,
        std::uint32_t& outSec) noexcept
    {
        outSec = 0;

        std::size_t valueStart = std::wstring_view::npos;

        const std::size_t labelEnd = line.find(L"):");

        if (labelEnd != std::wstring_view::npos) {
            valueStart = labelEnd + 2;
        }
        else {
            const std::size_t keyPos = line.find(L"Duration");

            if (keyPos != std::wstring_view::npos) {
                const std::size_t colonPos =
                    line.find(L':', keyPos + 8);

                if (colonPos != std::wstring_view::npos) {
                    valueStart = colonPos + 1;
                }
            }
        }

        if (valueStart == std::wstring_view::npos ||
            valueStart >= line.size()) {
            return false;
        }

        while (valueStart < line.size() &&
            (line[valueStart] == L' ' ||
                line[valueStart] == L'\t')) {
            ++valueStart;
        }

        if (valueStart + 8 > line.size()) {
            return false;
        }

        auto dec2 =
            [](wchar_t a, wchar_t b, int& out) noexcept -> bool
            {
                if (a < L'0' || a > L'9' ||
                    b < L'0' || b > L'9') {
                    return false;
                }

                out =
                    static_cast<int>(a - L'0') * 10 +
                    static_cast<int>(b - L'0');

                return true;
            };

        int h = 0;
        int m = 0;
        int s = 0;

        if (!dec2(line[valueStart], line[valueStart + 1], h) ||
            line[valueStart + 2] != L':' ||
            !dec2(line[valueStart + 3], line[valueStart + 4], m) ||
            line[valueStart + 5] != L':' ||
            !dec2(line[valueStart + 6], line[valueStart + 7], s)) {
            return false;
        }

        if (m > 59 || s > 59) {
            return false;
        }

        outSec =
            static_cast<std::uint32_t>(
                h * 3600 + m * 60 + s);

        return outSec > 0;
    }

    bool ParseHeaderLine_(
        std::wstring_view line,
        EpgEventWork_& ev) noexcept
    {
        if (!StartsWith_(line, L"TableID:")) {
            return false;
        }

        std::uint64_t tableId = 0;

        if (!ParseHexAfter_(line, L"TableID:", tableId)) {
            return false;
        }

        std::uint32_t serviceId = 0;
        std::uint32_t eventId = 0;
        std::uint32_t tsId = 0;
        std::uint32_t onId = 0;

        if (!ParseDecAfter_(line, L"ServiceID:", serviceId) ||
            !ParseDecAfter_(line, L"EventID:", eventId) ||
            !ParseDecAfter_(line, L"TSID:", tsId) ||
            !ParseDecAfter_(line, L"ONID:", onId)) {
            return false;
        }

        ev = EpgEventWork_{};
        ev.tableId = static_cast<std::uint32_t>(tableId);
        ev.serviceId = serviceId;
        ev.eventId = eventId;
        ev.tsId = tsId;
        ev.onId = onId;
        ev.hasHeader = true;

        return true;
    }

    bool ParseStartRawLine_(
        std::wstring_view line,
        std::uint64_t& outRaw) noexcept
    {
        return ParseHexAfter_(
            line,
            L"StartTime(raw MJD+BCD40bit):",
            outRaw);
    }

    bool ParseGenreLine_(
        std::wstring_view line,
        std::uint32_t& major,
        std::uint32_t& minor) noexcept
    {
        std::uint64_t p = 0;
        std::uint64_t s = 0;

        if (!ParseHexAfter_(line, L"primary=", p) ||
            !ParseHexAfter_(line, L"secondary=", s)) {
            return false;
        }

        major = static_cast<std::uint32_t>(p & 0xFFu);
        minor = static_cast<std::uint32_t>(s & 0xFFu);
        return true;
    }

    void AppendTextPool_(
        const std::wstring& text,
        std::wstring& pool,
        std::vector<std::uint32_t>& offsets,
        std::vector<std::uint32_t>& lengths)
    {
        const std::size_t offset = pool.size();
        const std::size_t length = text.size();

        offsets.push_back(
            offset <= std::numeric_limits<std::uint32_t>::max()
            ? static_cast<std::uint32_t>(offset)
            : 0u);

        lengths.push_back(
            length <= std::numeric_limits<std::uint32_t>::max()
            ? static_cast<std::uint32_t>(length)
            : 0u);

        pool += text;
        pool.push_back(L'\0');
    }

    bool CommitEvent_(
        const EpgEventWork_& ev,
        const SIMDData::EpgConvertOptions& options,
        SIMDData::EpgColumnStore& outStore,
        SIMDData::EpgConvertResult& result)
    {
        if (!ev.hasHeader ||
            !ev.hasStart ||
            !ev.hasDuration) {
            ++result.skippedEventCount;
            return false;
        }

        if (ev.serviceId > 0xFFFFu ||
            ev.eventId > 0xFFFFu ||
            ev.tsId > 0xFFFFu ||
            ev.onId > 0xFFFFu) {
            ++result.skippedEventCount;
            return false;
        }

        outStore.tableIds.push_back(
            static_cast<std::uint16_t>(ev.tableId & 0xFFFFu));

        outStore.serviceIds.push_back(
            static_cast<std::uint16_t>(ev.serviceId));

        outStore.eventIds.push_back(
            static_cast<std::uint16_t>(ev.eventId));

        outStore.tsIds.push_back(
            static_cast<std::uint16_t>(ev.tsId));

        outStore.onIds.push_back(
            static_cast<std::uint16_t>(ev.onId));

        outStore.startRaw40.push_back(ev.startRaw40);
        outStore.durationSec.push_back(ev.durationSec);

        if (options.keepGenre) {
            outStore.genreMajor.push_back(
                static_cast<std::uint8_t>(ev.genreMajor & 0xFFu));

            outStore.genreMinor.push_back(
                static_cast<std::uint8_t>(ev.genreMinor & 0xFFu));
        }
        else {
            outStore.genreMajor.push_back(0xFFu);
            outStore.genreMinor.push_back(0xFFu);
        }

        if (options.keepTitleText) {
            AppendTextPool_(
                ev.title,
                outStore.titleTextPool,
                outStore.titleOffset,
                outStore.titleLength);
        }
        else {
            outStore.titleOffset.push_back(0);
            outStore.titleLength.push_back(0);
        }

        if (options.keepDescriptionText) {
            AppendTextPool_(
                ev.description,
                outStore.descriptionTextPool,
                outStore.descriptionOffset,
                outStore.descriptionLength);
        }
        else {
            outStore.descriptionOffset.push_back(0);
            outStore.descriptionLength.push_back(0);
        }

        ++result.acceptedEventCount;
        return true;
    }

    bool ConvertEpgDumpTextCommon_(
        std::wstring_view epgText,
        SIMDData::ExecPath path,
        const SIMDData::EpgConvertOptions& options,
        SIMDData::EpgColumnStore& outStore,
        SIMDData::EpgConvertResult& outResult) noexcept
    {
        outStore.Clear();
        outResult = SIMDData::EpgConvertResult{};
        outResult.path = path;

        EpgEventWork_ current{};

        std::size_t pos = 0;

        while (pos < epgText.size()) {
            std::size_t end = epgText.find(L'\n', pos);

            if (end == std::wstring_view::npos) {
                end = epgText.size();
            }

            std::wstring_view line =
                epgText.substr(pos, end - pos);

            if (!line.empty() && line.back() == L'\r') {
                line.remove_suffix(1);
            }

            ++outResult.lineCount;

            if (StartsWith_(line, L"TableID:")) {
                if (current.hasHeader) {
                    CommitEvent_(
                        current,
                        options,
                        outStore,
                        outResult);
                }

                if (ParseHeaderLine_(line, current)) {
                    ++outResult.eventHeaderCount;
                }
            }
            else if (current.hasHeader) {
                std::wstring_view trimmed = line;

                while (!trimmed.empty() &&
                    (trimmed.front() == L' ' ||
                        trimmed.front() == L'\t')) {
                    trimmed.remove_prefix(1);
                }

                if (StartsWith_(trimmed, L"StartTime(raw MJD+BCD40bit):")) {
                    std::uint64_t raw = 0;

                    if (ParseStartRawLine_(trimmed, raw)) {
                        current.startRaw40 = raw;
                        current.hasStart = true;
                    }
                }
                else if (StartsWith_(trimmed, L"Duration(")) {
                    std::uint32_t sec = 0;

                    if (ParseDurationLine_(trimmed, sec)) {
                        current.durationSec = sec;
                        current.hasDuration = true;
                    }
                }
                else if (StartsWith_(trimmed, L"[短形式タイトル]")) {
                    current.title =
                        TrimCopy_(
                            trimmed.substr(
                                std::wstring_view(L"[短形式タイトル]").size()));

                    ++outResult.titleCount;
                }
                else if (StartsWith_(trimmed, L"[短形式概要]")) {
                    current.description =
                        TrimCopy_(
                            trimmed.substr(
                                std::wstring_view(L"[短形式概要]").size()));

                    ++outResult.descriptionCount;
                }
                else if (StartsWith_(trimmed, L"[ジャンル]")) {
                    std::uint32_t major = 0xFFu;
                    std::uint32_t minor = 0xFFu;

                    if (ParseGenreLine_(trimmed, major, minor)) {
                        current.genreMajor = major;
                        current.genreMinor = minor;
                        ++outResult.genreCount;
                    }
                }
            }

            pos = end + 1;
        }

        if (current.hasHeader) {
            CommitEvent_(
                current,
                options,
                outStore,
                outResult);
        }

        outResult.ok = true;

        outResult.message = L"EPG SIMDData convert path=";
        outResult.message += SIMDData::ToString(path);
        outResult.message += L" events=";
        outResult.message += std::to_wstring(outResult.acceptedEventCount);
        outResult.message += L" lines=";
        outResult.message += std::to_wstring(outResult.lineCount);

        return true;
    }

    bool Utf8BytesToWide_(
        const std::string& bytes,
        std::wstring& out)
    {
        out.clear();

        if (bytes.empty()) {
            return true;
        }

        const char* p = bytes.data();
        int len = static_cast<int>(bytes.size());

        if (bytes.size() >= 3 &&
            static_cast<unsigned char>(bytes[0]) == 0xEF &&
            static_cast<unsigned char>(bytes[1]) == 0xBB &&
            static_cast<unsigned char>(bytes[2]) == 0xBF) {
            p += 3;
            len -= 3;
        }

        const int need =
            MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                p,
                len,
                nullptr,
                0);

        if (need <= 0) {
            return false;
        }

        out.resize(static_cast<std::size_t>(need));

        const int done =
            MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                p,
                len,
                out.data(),
                need);

        return done == need;
    }

    bool CpuidLeafAvailable_(int leaf) noexcept
    {
        int regs[4]{};
        __cpuid(regs, 0);
        return regs[0] >= leaf;
    }

    bool IsAvxOsEnabled_() noexcept
    {
        int regs[4]{};

        if (!CpuidLeafAvailable_(1)) {
            return false;
        }

        __cpuid(regs, 1);

        const int ecx = regs[2];

        const bool avx =
            (ecx & (1 << 28)) != 0;

        const bool osxsave =
            (ecx & (1 << 27)) != 0;

        if (!avx || !osxsave) {
            return false;
        }

        const unsigned long long xcr0 =
            _xgetbv(0);

        return (xcr0 & 0x06ull) == 0x06ull;
    }
}

namespace SIMDData
{
    CpuCaps DetectCpuCaps() noexcept
    {
        CpuCaps out{};

        int regs[4]{};

        if (!CpuidLeafAvailable_(1)) {
            return out;
        }

        __cpuid(regs, 1);

        const int ecx = regs[2];
        const int edx = regs[3];

        out.sse2 =
            (edx & (1 << 26)) != 0;

        out.sse42 =
            (ecx & (1 << 20)) != 0;

        out.avx =
            (ecx & (1 << 28)) != 0;

        out.osAvx = IsAvxOsEnabled_();

        if (CpuidLeafAvailable_(7)) {
            int regs7[4]{};
            __cpuidex(regs7, 7, 0);

            out.avx2 =
                out.osAvx &&
                ((regs7[1] & (1 << 5)) != 0);
        }

        return out;
    }

    ExecPath ParseExecPathName(std::wstring_view name) noexcept
    {
        const std::wstring s =
            UpperAscii_(name);

        if (s.empty() || s == L"AUTO") {
            return ExecPath::Auto;
        }

        if (s == L"AVX2") {
            return ExecPath::Avx2;
        }

        if (s == L"SSE4.2" ||
            s == L"SSE42" ||
            s == L"SSE4_2") {
            return ExecPath::Sse42;
        }

        if (s == L"SSE2") {
            return ExecPath::Sse2;
        }

        if (s == L"SCALAR") {
            return ExecPath::Scalar;
        }

        // 未知値は通常運用の AUTO へ戻す。
        return ExecPath::Auto;
    }

    const wchar_t* ToString(ExecPath path) noexcept
    {
        switch (path) {
        case ExecPath::Auto:
            return L"AUTO";

        case ExecPath::Avx2:
            return L"AVX2";

        case ExecPath::Sse42:
            return L"SSE4.2";

        case ExecPath::Sse2:
            return L"SSE2";

        case ExecPath::Scalar:
        default:
            return L"SCALAR";
        }
    }

    SelectedPath SelectPathFromConfig() noexcept
    {
        SelectedPath out{};

        try {
            auto& cfg = ConfigManager::GetInstance();

            out.requested =
                ParseExecPathName(
                    cfg.GetSelectedCPUFeature());

            const CpuCaps caps =
                DetectCpuCaps();

            const bool allowSse2 =
                cfg.GetCpuUseSSE2() && caps.sse2;

            const bool allowSse42 =
                cfg.GetCpuUseSSE4_2() && caps.sse42;

            const bool allowAvx2 =
                cfg.GetCpuUseAVX2() && caps.avx2;

            const auto selectAuto =
                [&]() noexcept -> ExecPath
                {
                    if (allowAvx2) {
                        return ExecPath::Avx2;
                    }

                    if (allowSse42) {
                        return ExecPath::Sse42;
                    }

                    if (allowSse2) {
                        return ExecPath::Sse2;
                    }

                    return ExecPath::Scalar;
                };

            if (out.requested == ExecPath::Auto) {
                out.actual = selectAuto();
                out.fallback = false;
                out.reason = L"AUTO";
                return out;
            }

            switch (out.requested) {
            case ExecPath::Avx2:
                if (allowAvx2) {
                    out.actual = ExecPath::Avx2;
                    out.reason = L"requested";
                    return out;
                }

                out.actual = selectAuto();
                out.fallback = true;
                out.reason = L"AVX2 unavailable, fallback to ";
                out.reason += ToString(out.actual);
                return out;

            case ExecPath::Sse42:
                if (allowSse42) {
                    out.actual = ExecPath::Sse42;
                    out.reason = L"requested";
                    return out;
                }

                if (allowSse2) {
                    out.actual = ExecPath::Sse2;
                }
                else {
                    out.actual = ExecPath::Scalar;
                }

                out.fallback = true;
                out.reason = L"SSE4.2 unavailable, fallback to ";
                out.reason += ToString(out.actual);
                return out;

            case ExecPath::Sse2:
                if (allowSse2) {
                    out.actual = ExecPath::Sse2;
                    out.reason = L"requested";
                    return out;
                }

                out.actual = ExecPath::Scalar;
                out.fallback = true;
                out.reason = L"SSE2 unavailable, fallback to SCALAR";
                return out;

            case ExecPath::Scalar:
            default:
                out.actual = ExecPath::Scalar;
                out.reason = L"requested";
                return out;
            }
        }
        catch (...) {
            out.requested = ExecPath::Auto;
            out.actual = ExecPath::Scalar;
            out.fallback = true;
            out.reason = L"ConfigManager unavailable, fallback to SCALAR";
            return out;
        }
    }

    bool ConvertEpgDumpTextToColumnStore(
        std::wstring_view epgText,
        const EpgConvertOptions& options,
        EpgColumnStore& outStore,
        EpgConvertResult& outResult) noexcept
    {
        const SelectedPath selected =
            SelectPathFromConfig();

        switch (selected.actual) {
        case ExecPath::Avx2:
            return ConvertEpgDumpTextToColumnStoreAvx2(
                epgText,
                options,
                outStore,
                outResult);

        case ExecPath::Sse42:
            return ConvertEpgDumpTextToColumnStoreSse42(
                epgText,
                options,
                outStore,
                outResult);

        case ExecPath::Sse2:
            return ConvertEpgDumpTextToColumnStoreSse2(
                epgText,
                options,
                outStore,
                outResult);

        case ExecPath::Scalar:
        default:
            return ConvertEpgDumpTextCommon_(
                epgText,
                ExecPath::Scalar,
                options,
                outStore,
                outResult);
        }
    }

    bool ConvertEpgDumpFileToColumnStore(
        const std::filesystem::path& path,
        const EpgConvertOptions& options,
        EpgColumnStore& outStore,
        EpgConvertResult& outResult) noexcept
    {
        outStore.Clear();
        outResult = EpgConvertResult{};

        try {
            std::ifstream ifs(
                path,
                std::ios::binary);

            if (!ifs.is_open()) {
                outResult.message = L"EPG file open failed: ";
                outResult.message += path.wstring();
                return false;
            }

            std::string bytes(
                (std::istreambuf_iterator<char>(ifs)),
                std::istreambuf_iterator<char>());

            std::wstring text;

            if (!Utf8BytesToWide_(bytes, text)) {
                outResult.message = L"EPG UTF-8 decode failed: ";
                outResult.message += path.wstring();
                return false;
            }

            return ConvertEpgDumpTextToColumnStore(
                text,
                options,
                outStore,
                outResult);
        }
        catch (...) {
            outResult.message = L"EPG convert exception: ";
            outResult.message += path.wstring();
            return false;
        }
    }

    bool ConvertEpgDumpTextToColumnStoreSse2(
        std::wstring_view epgText,
        const EpgConvertOptions& options,
        EpgColumnStore& outStore,
        EpgConvertResult& outResult) noexcept
    {
        return ConvertEpgDumpTextCommon_(
            epgText,
            ExecPath::Sse2,
            options,
            outStore,
            outResult);
    }

    bool ConvertEpgDumpTextToColumnStoreSse42(
        std::wstring_view epgText,
        const EpgConvertOptions& options,
        EpgColumnStore& outStore,
        EpgConvertResult& outResult) noexcept
    {
        return ConvertEpgDumpTextCommon_(
            epgText,
            ExecPath::Sse42,
            options,
            outStore,
            outResult);
    }

    bool ConvertEpgDumpTextToColumnStoreAvx2(
        std::wstring_view epgText,
        const EpgConvertOptions& options,
        EpgColumnStore& outStore,
        EpgConvertResult& outResult) noexcept
    {
        return ConvertEpgDumpTextCommon_(
            epgText,
            ExecPath::Avx2,
            options,
            outStore,
            outResult);
    }
}