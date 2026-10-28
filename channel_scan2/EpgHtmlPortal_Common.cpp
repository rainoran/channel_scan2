#include "EpgHtmlPortal_Common.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <Windows.h>
#endif

#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include "DebugManager.h"

namespace EpgHtmlPortal_Common
{
    namespace
    {
        constexpr wchar_t kBomUtf16Le = static_cast<wchar_t>(0xFEFF);

        void LogOpenFailed_(const std::filesystem::path& path)
        {
            DebugLogFormat(
                L"[EPG-PORTAL][ERROR] file open failed: %ls",
                path.wstring().c_str());
        }

        void LogWriteFailed_(const std::filesystem::path& path)
        {
            DebugLogFormat(
                L"[EPG-PORTAL][ERROR] file write failed: %ls",
                path.wstring().c_str());
        }

        bool IsAllowedControl_(wchar_t ch) noexcept
        {
            return ch == L'\r' || ch == L'\n' || ch == L'\t';
        }

        bool IsJapaneseLikelyChar_(wchar_t ch) noexcept
        {
            return
                (ch >= 0x3040 && ch <= 0x309F) || // Hiragana
                (ch >= 0x30A0 && ch <= 0x30FF) || // Katakana
                (ch >= 0x3400 && ch <= 0x9FFF) || // CJK
                (ch >= 0xFF00 && ch <= 0xFFEF) || // Fullwidth / Halfwidth
                ch == 0x3000 ||
                ch == 0x3001 ||
                ch == 0x3002 ||
                ch == 0x30FB ||
                ch == 0x30FC;
        }

        bool IsMojibakeMarker_(wchar_t ch) noexcept
        {
            return ch == 0xFFFD ||
                ch == L'□' ||
                ch == L'�' ||
                ch == L'Ã' ||
                ch == L'ã' ||
                ch == L'Â' ||
                ch == L'â';
        }

        bool ContainsMojibakeWord_(const std::wstring& s) noexcept
        {
            static constexpr const wchar_t* kWords[] = {
                L"繧", L"繝", L"譁", L"縺", L"荳",
                L"莨", L"邱", L"髯", L"蟆", L"蜷", L"驛"
            };

            for (const wchar_t* word : kWords) {
                if (s.find(word) != std::wstring::npos) {
                    return true;
                }
            }
            return false;
        }

        int ScoreDecodedText_(const std::wstring& s) noexcept
        {
            if (s.empty()) {
                return -1000000;
            }

            int score = 0;
            int japanese = 0;
            int ascii = 0;
            int bad = 0;
            int controls = 0;
            int nul = 0;

            for (wchar_t ch : s) {
                if (ch == 0) {
                    ++nul;
                    continue;
                }

                if (IsMojibakeMarker_(ch)) {
                    ++bad;
                    continue;
                }

                if (ch < 0x20 && !IsAllowedControl_(ch)) {
                    ++controls;
                    continue;
                }

                if (ch >= 0x0080 && ch <= 0x009F) {
                    ++controls;
                    continue;
                }

                if (ch >= 0x20 && ch <= 0x7E) {
                    ++ascii;
                }

                if (IsJapaneseLikelyChar_(ch)) {
                    ++japanese;
                }
            }

            score += japanese * 10;
            score += ascii;
            score -= bad * 700;
            score -= controls * 900;
            score -= nul * 2000;

            if (ContainsMojibakeWord_(s)) {
                score -= 4000;
            }

            if (s.find(L"NHK") != std::wstring::npos ||
                s.find(L"ＮＨＫ") != std::wstring::npos ||
                s.find(L"TOKYO") != std::wstring::npos ||
                s.find(L"ＴＯＫＹＯ") != std::wstring::npos ||
                s.find(L"テレ") != std::wstring::npos ||
                s.find(L"テレビ") != std::wstring::npos ||
                s.find(L"放送") != std::wstring::npos ||
                s.find(L"チャンネル") != std::wstring::npos ||
                s.find(L"ＢＳ") != std::wstring::npos ||
                s.find(L"ＣＳ") != std::wstring::npos ||
                s.find(L"BS") != std::wstring::npos ||
                s.find(L"CS") != std::wstring::npos ||
                s.find(L"WOWOW") != std::wstring::npos ||
                s.find(L"ＷＯＷＯＷ") != std::wstring::npos ||
                s.find(L"J SPORTS") != std::wstring::npos ||
                s.find(L"Ｊ　ＳＰＯＲＴＳ") != std::wstring::npos ||
                s.find(L"スターチャンネル") != std::wstring::npos ||
                s.find(L"スカチャン") != std::wstring::npos) {
                score += 200;
            }

            return score;
        }

        void AppendHex4_(std::wstring& out, unsigned int v)
        {
            static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
            out.push_back(kHex[(v >> 12) & 0x0F]);
            out.push_back(kHex[(v >> 8) & 0x0F]);
            out.push_back(kHex[(v >> 4) & 0x0F]);
            out.push_back(kHex[v & 0x0F]);
        }
	}//namespace

    std::wstring TrimW(std::wstring s)
    {
        const auto isWs = [](wchar_t ch) noexcept {
            return ch == L' ' ||
                ch == L'\t' ||
                ch == L'\r' ||
                ch == L'\n' ||
                ch == 0x3000;
            };

        while (!s.empty() && isWs(s.front())) {
            s.erase(s.begin());
        }
        while (!s.empty() && isWs(s.back())) {
            s.pop_back();
        }
        return s;
    }

    std::wstring ToLowerAsciiW(std::wstring s)
    {
        for (wchar_t& ch : s) {
            if (ch >= L'A' && ch <= L'Z') {
                ch = static_cast<wchar_t>(ch - L'A' + L'a');
            }
        }
        return s;
    }

    std::string WideToUtf8(std::wstring_view ws)
    {
#ifdef _WIN32
        if (ws.empty()) {
            return {};
        }

        const int need = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            ws.data(),
            static_cast<int>(ws.size()),
            nullptr,
            0,
            nullptr,
            nullptr);

        if (need <= 0) {
            return {};
        }

        std::string out(static_cast<std::size_t>(need), '\0');

        const int written = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            ws.data(),
            static_cast<int>(ws.size()),
            out.data(),
            need,
            nullptr,
            nullptr);

        if (written <= 0) {
            return {};
        }

        if (written != need) {
            out.resize(static_cast<std::size_t>(written));
        }

        return out;
#else
        return std::string(ws.begin(), ws.end());
#endif
    }

    std::wstring EscapeHtmlW(std::wstring_view s)
    {
        std::wstring out;
        out.reserve(s.size() + 32);

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
                out.push_back(ch);
                break;
            }
        }

        return out;
    }

    std::wstring ToJsBool(bool v)
    {
        return v ? L"true" : L"false";
    }

    std::wstring RepairUtf8ByteWidenedMojibake(std::wstring_view s)
    {
        std::wstring original(s);

#ifdef _WIN32
        if (s.empty()) {
            return {};
        }

        bool hasMarker = false;
        std::string bytes;
        bytes.reserve(s.size());

        for (const wchar_t ch : s) {
            if (ch > 0x00FF) {
                return original;
            }

            if (ch == L'Ã' ||
                ch == L'ã' ||
                ch == L'Â' ||
                ch == L'â' ||
                (ch >= 0x0080 && ch <= 0x009F)) {
                hasMarker = true;
            }

            bytes.push_back(
                static_cast<char>(
                    static_cast<unsigned char>(ch & 0x00FF)));
        }

        if (!hasMarker) {
            return original;
        }

        const int need = ::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            bytes.data(),
            static_cast<int>(bytes.size()),
            nullptr,
            0);

        if (need <= 0) {
            return original;
        }

        std::wstring decoded(static_cast<std::size_t>(need), L'\0');

        const int written = ::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            bytes.data(),
            static_cast<int>(bytes.size()),
            decoded.data(),
            need);

        if (written <= 0) {
            return original;
        }

        if (written != need) {
            decoded.resize(static_cast<std::size_t>(written));
        }

        const int beforeScore = ScoreDecodedText_(original);
        const int afterScore = ScoreDecodedText_(decoded);

        if (afterScore > beforeScore + 100) {
            return decoded;
        }
#endif

        return original;
    }

    std::wstring ToJsStringLiteral(std::wstring_view s)
    {
        const std::wstring repaired =
            RepairUtf8ByteWidenedMojibake(s);

        std::wstring out;
        out.reserve(repaired.size() * 6 + 8);
        out.push_back(L'\'');

        for (const wchar_t ch : repaired) {
            switch (ch) {
            case L'\\':
                out += L"\\\\";
                break;
            case L'\'':
                out += L"\\\'";
                break;
            case L'\r':
                out += L"\\r";
                break;
            case L'\n':
                out += L"\\n";
                break;
            case L'\t':
                out += L"\\t";
                break;
            case 0x2028:
                out += L"\\u2028";
                break;
            case 0x2029:
                out += L"\\u2029";
                break;
            default:
                if (ch >= 0x20 && ch <= 0x7E) {
                    out.push_back(ch);
                }
                else {
                    out += L"\\u";
                    AppendHex4_(
                        out,
                        static_cast<unsigned int>(
                            static_cast<unsigned short>(ch)));
                }
                break;
            }
        }

        out.push_back(L'\'');
        return out;
    }

    bool ReadAllBytes(const std::filesystem::path& path, std::string& outBytes)
    {
        outBytes.clear();

        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) {
            LogOpenFailed_(path);
            return false;
        }

        outBytes.assign(
            std::istreambuf_iterator<char>(ifs),
            std::istreambuf_iterator<char>());

        return true;
    }

    bool ReadTextFileToWide(const std::filesystem::path& path, std::wstring& outText)
    {
        outText.clear();

        std::string bytes;
        if (!ReadAllBytes(path, bytes)) {
            return false;
        }

        outText = DecodeTextBytesToWide(bytes);
        return true;
    }

    bool EnsureParentDirectory(const std::filesystem::path& path)
    {
        const std::filesystem::path parent = path.parent_path();
        if (parent.empty()) {
            return true;
        }

        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        return !ec;
    }

    bool WriteUtf8BomTextFile(const std::filesystem::path& path, std::wstring_view body)
    {
        if (!EnsureParentDirectory(path)) {
            DebugLogFormat(
                L"[EPG-PORTAL][ERROR] create directory failed: %ls",
                path.parent_path().wstring().c_str());
            return false;
        }

        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            LogOpenFailed_(path);
            return false;
        }

        static constexpr unsigned char kUtf8Bom[3] = { 0xEF, 0xBB, 0xBF };
        ofs.write(reinterpret_cast<const char*>(kUtf8Bom), 3);

        const std::string utf8 = WideToUtf8(body);
        if (!utf8.empty()) {
            ofs.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
        }

        ofs.flush();

        if (!ofs.good()) {
            LogWriteFailed_(path);
            return false;
        }

        return true;
    }

    bool TryDecodeMultiByte(unsigned int codePage, const std::string& bytes, std::wstring& out)
    {
#ifdef _WIN32
        out.clear();

        if (bytes.empty()) {
            return true;
        }

        UINT flags = 0;
        if (codePage == CP_UTF8) {
            flags = MB_ERR_INVALID_CHARS;
        }

        const int need = ::MultiByteToWideChar(
            static_cast<UINT>(codePage),
            flags,
            bytes.data(),
            static_cast<int>(bytes.size()),
            nullptr,
            0);

        if (need <= 0) {
            return false;
        }

        out.resize(static_cast<std::size_t>(need));

        const int written = ::MultiByteToWideChar(
            static_cast<UINT>(codePage),
            flags,
            bytes.data(),
            static_cast<int>(bytes.size()),
            out.data(),
            need);

        if (written <= 0) {
            out.clear();
            return false;
        }

        if (written != need) {
            out.resize(static_cast<std::size_t>(written));
        }

        return true;
#else
        out.assign(bytes.begin(), bytes.end());
        return true;
#endif
    }

    std::wstring DecodeUtf16LeBytes(const std::string& bytes, std::size_t offset)
    {
        std::wstring out;

        if (bytes.size() <= offset) {
            return out;
        }

        const std::size_t count = (bytes.size() - offset) / 2;
        out.resize(count);

        for (std::size_t i = 0; i < count; ++i) {
            const unsigned char lo = static_cast<unsigned char>(bytes[offset + i * 2 + 0]);
            const unsigned char hi = static_cast<unsigned char>(bytes[offset + i * 2 + 1]);
            out[i] = static_cast<wchar_t>(
                static_cast<unsigned short>(lo | (hi << 8)));
        }

        return out;
    }

    std::wstring DecodeUtf16BeBytes(const std::string& bytes, std::size_t offset)
    {
        std::wstring out;

        if (bytes.size() <= offset) {
            return out;
        }

        const std::size_t count = (bytes.size() - offset) / 2;
        out.resize(count);

        for (std::size_t i = 0; i < count; ++i) {
            const unsigned char hi = static_cast<unsigned char>(bytes[offset + i * 2 + 0]);
            const unsigned char lo = static_cast<unsigned char>(bytes[offset + i * 2 + 1]);
            out[i] = static_cast<wchar_t>(
                static_cast<unsigned short>(lo | (hi << 8)));
        }

        return out;
    }

    std::wstring DecodeTextBytesToWide(const std::string& bytes)
    {
        if (bytes.empty()) {
            return {};
        }

        if (bytes.size() >= 2 &&
            static_cast<unsigned char>(bytes[0]) == 0xFF &&
            static_cast<unsigned char>(bytes[1]) == 0xFE) {
            return RepairUtf8ByteWidenedMojibake(
                DecodeUtf16LeBytes(bytes, 2));
        }

        if (bytes.size() >= 2 &&
            static_cast<unsigned char>(bytes[0]) == 0xFE &&
            static_cast<unsigned char>(bytes[1]) == 0xFF) {
            return RepairUtf8ByteWidenedMojibake(
                DecodeUtf16BeBytes(bytes, 2));
        }

        if (bytes.size() >= 3 &&
            static_cast<unsigned char>(bytes[0]) == 0xEF &&
            static_cast<unsigned char>(bytes[1]) == 0xBB &&
            static_cast<unsigned char>(bytes[2]) == 0xBF) {
            std::wstring out;
            const std::string sliced(bytes.begin() + 3, bytes.end());

            if (TryDecodeMultiByte(CP_UTF8, sliced, out)) {
                return RepairUtf8ByteWidenedMojibake(out);
            }
        }

        struct Candidate
        {
            std::wstring text;
            int score = -100000000;
            const wchar_t* name = L"unknown";
        };

        std::vector<Candidate> candidates;

        auto addCandidate = [&](std::wstring text, const wchar_t* name) {
            text = RepairUtf8ByteWidenedMojibake(text);

            Candidate c{};
            c.text = std::move(text);
            c.score = ScoreDecodedText_(c.text);
            c.name = name;
            candidates.push_back(std::move(c));
            };

#ifdef _WIN32
        {
            std::wstring out;
            if (TryDecodeMultiByte(CP_UTF8, bytes, out)) {
                addCandidate(std::move(out), L"UTF-8");
            }
        }

        {
            std::wstring out;
            if (TryDecodeMultiByte(932U, bytes, out)) {
                addCandidate(std::move(out), L"CP932");
            }
        }

        {
            std::wstring out;
            if (TryDecodeMultiByte(CP_ACP, bytes, out)) {
                addCandidate(std::move(out), L"CP_ACP");
            }
        }
#else
        {
            std::wstring out(bytes.begin(), bytes.end());
            addCandidate(std::move(out), L"byte-wide");
        }
#endif

        if (!candidates.empty()) {
            const auto best = std::max_element(
                candidates.begin(),
                candidates.end(),
                [](const Candidate& a, const Candidate& b) {
                    return a.score < b.score;
                });

            if (best != candidates.end()) {
                return best->text;
            }
        }

        return std::wstring(bytes.begin(), bytes.end());
    }

    std::filesystem::path GetExeDirectoryPath()
    {
#ifdef _WIN32
        wchar_t modulePath[MAX_PATH]{};
        const DWORD n = ::GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            return std::filesystem::path(modulePath).parent_path();
        }
#endif
        return {};
    }

    std::filesystem::path ResolveAuxiliaryPath(
        const std::filesystem::path& baseDir,
        const std::filesystem::path& filePath)
    {
        if (filePath.empty()) {
            return {};
        }

        if (filePath.is_absolute()) {
            return filePath;
        }

        const std::filesystem::path exeDir = GetExeDirectoryPath();
        if (!exeDir.empty()) {
            const std::filesystem::path exeSide = exeDir / filePath;
            if (std::filesystem::exists(exeSide)) {
                return exeSide;
            }
        }

        if (!baseDir.empty()) {
            const std::filesystem::path outputSide = baseDir / filePath;
            if (std::filesystem::exists(outputSide)) {
                return outputSide;
            }
        }

        const std::filesystem::path cwdSide = std::filesystem::current_path() / filePath;
        if (std::filesystem::exists(cwdSide)) {
            return cwdSide;
        }

        if (!exeDir.empty()) {
            return exeDir / filePath;
        }

        if (!baseDir.empty()) {
            return baseDir / filePath;
        }

        return cwdSide;
    }
}