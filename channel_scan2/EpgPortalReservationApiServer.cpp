#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "EpgPortalReservationApiServer.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "Crypt32.lib")

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>
#include <condition_variable>
#include <chrono>
#include <limits>
#include <cwctype>
#include <exception>
#include <string_view>
#include <type_traits>


#include "DebugManager.h"
#include "TaskKinds.hpp"
#include "TaskSystemConnector.hpp"
#include "AutoReserveRuleStore.hpp"


namespace
{
    constexpr const char* kHttpOk = "OK";
    constexpr const char* kHttpBadRequest = "Bad Request";
    constexpr const char* kHttpNotFound = "Not Found";
    constexpr const char* kHttpMethodNotAllowed = "Method Not Allowed";
    constexpr const char* kHttpServerError = "Server Error";
    constexpr const char* kHttpNoContent = "No Content";
    constexpr const char* kHttpSeeOther = "See Other";
    constexpr const char* kHttpUnauthorized = "Unauthorized";
    constexpr std::uintptr_t kInvalidSocketHandle_ = UINTPTR_MAX;

    const char* StatusTextFromCode_(int statusCode) noexcept
    {
        switch (statusCode) {
        case 200: return kHttpOk;
        case 204: return kHttpNoContent;
        case 303: return kHttpSeeOther;
        case 400: return kHttpBadRequest;
        case 401: return kHttpUnauthorized;
        case 404: return kHttpNotFound;
        case 405: return kHttpMethodNotAllowed;
        default:
            return statusCode >= 500 ? kHttpServerError : kHttpOk;
        }
    }

    std::string BuildHttpResponse(
        int statusCode,
        const char* statusText,
        const std::string& body,
        const char* contentType,
        const std::string& extraHeaders = {})
    {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << statusCode << ' ' << statusText << "\r\n";
        oss << "Content-Type: " << (contentType ? contentType : "application/octet-stream") << "\r\n";
        oss << "Access-Control-Allow-Origin: *\r\n";
        oss << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
        oss << "Access-Control-Allow-Headers: Content-Type\r\n";
        oss << "X-Content-Type-Options: nosniff\r\n";
        oss << "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n";
        oss << "Pragma: no-cache\r\n";
        oss << "Expires: 0\r\n";
        if (!extraHeaders.empty()) {
            oss << extraHeaders;
            if (extraHeaders.size() < 2 ||
                extraHeaders.substr(extraHeaders.size() - 2) != "\r\n") {
                oss << "\r\n";
            }
        }
        oss << "Content-Length: " << body.size() << "\r\n";
        oss << "Connection: close\r\n\r\n";
        oss << body;
        return oss.str();
    }

    bool SendAllSocket_(std::uintptr_t sock, const std::string& data) noexcept
    {
        const char* p = data.data();
        std::size_t remain = data.size();

        while (remain > 0) {
            constexpr std::size_t kMaxSendChunkBytes =
                std::size_t{ 64 } *std::size_t{ 1024 };
            const int chunk = static_cast<int>(
                (std::min)(remain, kMaxSendChunkBytes));
            const int sent = ::send(static_cast<SOCKET>(sock), p, chunk, 0);
            if (sent <= 0) {
                return false;
            }
            p += sent;
            remain -= static_cast<std::size_t>(sent);
        }
        return true;
    }

    void ApplyClientSocketTimeout_(SOCKET sock) noexcept
    {
        // config.ini の [Network] RequestTimeoutMs=15000 と揃える。
        // 大きめの番組表HTMLを返す時に 3000ms だと送信途中で切れて
        // ERR_CONTENT_LENGTH_MISMATCH の原因になりやすい。
        DWORD timeoutMs = 15000;

        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
            reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    }

    void CloseClientSocket_(SOCKET sock) noexcept
    {
        if (sock != INVALID_SOCKET) {
            ::shutdown(sock, SD_BOTH);
            ::closesocket(sock);
        }
    }

    std::string ToUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        const int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
            static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        if (need <= 0) return {};
        std::string out(static_cast<std::size_t>(need), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
            static_cast<int>(w.size()), out.data(), need, nullptr, nullptr);
        return out;
    }

    std::wstring FromUtf8(const std::string& s)
    {
        if (s.empty()) return {};
        const int need = MultiByteToWideChar(CP_UTF8, 0, s.data(),
            static_cast<int>(s.size()), nullptr, 0);
        if (need <= 0) return {};
        std::wstring out(static_cast<std::size_t>(need), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(),
            static_cast<int>(s.size()), out.data(), need);
        return out;
    }

    std::wstring MakeReservationUniqueKey_(const TaskSystemConnector::TaskViewRow& row)
    {
        if (!row.dedupeKey.empty()) {
            return row.dedupeKey + L"|" + row.reserveMode;
        }

        return L"Task_" + std::to_wstring(row.kind) + L"_" + std::to_wstring(row.instance);
    }

    std::string JsonEscapeUtf8Local_(const std::wstring& s)
    {
        const std::string in = ToUtf8(s);
        std::string out;
        out.reserve(in.size() + 8);

        for (unsigned char c : in) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else {
                    out.push_back(static_cast<char>(c));
                }
                break;
            }
        }

        return out;
    }

    std::wstring MakeTaskId_(const TaskSystemConnector::TaskViewRow& row)
    {
        return L"Task_" + std::to_wstring(row.kind) + L"_" + std::to_wstring(row.instance);
    }

    std::string BuildTaskViewRowJsonLocal_(const TaskSystemConnector::TaskViewRow& row)
    {
        const std::wstring id = MakeTaskId_(row);

        std::ostringstream oss;
        oss << "{";
        oss << "\"id\":\"" << JsonEscapeUtf8Local_(id) << "\",";
        oss << "\"label\":\"" << JsonEscapeUtf8Local_(row.label) << "\",";
        oss << "\"category\":\"" << JsonEscapeUtf8Local_(row.category) << "\",";
        oss << "\"mode\":\"" << JsonEscapeUtf8Local_(row.reserveMode) << "\",";
        oss << "\"enabled\":" << (row.enabled ? "true" : "false") << ",";
        oss << "\"disabledByUser\":" << (row.disabledByUser ? "true" : "false") << ",";
        oss << "\"excludeFromAuto\":" << (row.excludeFromAuto ? "true" : "false") << ",";
        oss << "\"autoReserved\":" << (row.autoReserved ? "true" : "false") << ",";
        oss << "\"date\":\"\",";
        oss << "\"weekday\":\"\",";
        oss << "\"time\":\"" << JsonEscapeUtf8Local_(row.note) << "\",";
        oss << "\"service\":\"" << JsonEscapeUtf8Local_(row.target) << "\",";
        oss << "\"target\":\"" << JsonEscapeUtf8Local_(row.target) << "\",";
        oss << "\"note\":\"" << JsonEscapeUtf8Local_(row.note) << "\",";
        oss << "\"reserveSource\":\"" << JsonEscapeUtf8Local_(row.reserveSource) << "\",";
        oss << "\"dedupeKey\":\"" << JsonEscapeUtf8Local_(row.dedupeKey) << "\",";
        oss << "\"excludeKey\":\"" << JsonEscapeUtf8Local_(row.excludeKey) << "\",";
        oss << "\"instance\":" << row.instance << ",";
        oss << "\"kind\":" << row.kind << ",";
        oss << "\"preferredTunerIndex\":" << row.preferredTunerIndex << ",";
        oss << "\"requiredTuners\":" << static_cast<unsigned>(row.requiredTuners);
        oss << "}";
        return oss.str();
    }

    bool IsRecordTaskKindForApi_(std::uint32_t kind) noexcept
    {
        return kind == TaskKinds::RecPrepare
            || kind == TaskKinds::RecStart
            || kind == TaskKinds::RecStop;
    }

    bool ParseTaskIdForApi_(
        const std::wstring& id,
        std::uint32_t& kind,
        std::uint32_t& instance) noexcept
    {
        kind = 0;
        instance = 0;

        unsigned k = 0;
        unsigned i = 0;
        if (swscanf_s(id.c_str(), L"Task_%u_%u", &k, &i) == 2) {
            kind = static_cast<std::uint32_t>(k);
            instance = static_cast<std::uint32_t>(i);
            return true;
        }

        return false;
    }

    std::uint32_t HashFNV1a32ForApi_(const std::wstring& s) noexcept
    {
        std::uint32_t h = 2166136261u;
        for (wchar_t ch : s) {
            h ^= static_cast<std::uint32_t>(ch);
            h *= 16777619u;
        }
        return (h == 0u) ? 1u : h;
    }

    bool IsProgramModeForApi_(const std::wstring& mode) noexcept
    {
        return mode == L"watch" || mode == L"record";
    }

    std::uint32_t ParseUInt32LooseForApi_(const std::wstring& s, std::uint32_t defValue = 0) noexcept
    {
        const wchar_t* p = s.c_str();

        while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') {
            ++p;
        }

        if (*p == L'+') {
            ++p;
        }
        else if (*p == L'-') {
            return defValue;
        }

        if (*p < L'0' || *p > L'9') {
            return defValue;
        }

        std::uint64_t value = 0;
        while (*p >= L'0' && *p <= L'9') {
            value = value * 10u + static_cast<std::uint64_t>(*p - L'0');
            if (value > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
                return defValue;
            }
            ++p;
        }

        while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') {
            ++p;
        }

        if (*p != L'\0') {
            return defValue;
        }

        return static_cast<std::uint32_t>(value);
    }

    std::wstring LowerPathString_(std::filesystem::path p)
    {
        std::wstring s = p.wstring();
        std::replace(s.begin(), s.end(), L'/', L'\\');
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
            return static_cast<wchar_t>(std::towlower(c));
            });
        return s;
    }

    bool IsPathUnderRoot_(
        const std::filesystem::path& root,
        const std::filesystem::path& target) noexcept
    {
        try {
            std::error_code ec;
            auto rootAbs = std::filesystem::weakly_canonical(root, ec);
            if (ec) {
                ec.clear();
                rootAbs = std::filesystem::absolute(root, ec);
                if (ec) return false;
            }

            auto targetAbs = std::filesystem::weakly_canonical(target, ec);
            if (ec) {
                ec.clear();
                targetAbs = std::filesystem::absolute(target, ec);
                if (ec) return false;
            }

            std::wstring r = LowerPathString_(rootAbs);
            std::wstring t = LowerPathString_(targetAbs);

            while (!r.empty() && (r.back() == L'\\' || r.back() == L'/')) {
                r.pop_back();
            }
            while (!t.empty() && (t.back() == L'\\' || t.back() == L'/')) {
                t.pop_back();
            }

            return t == r || (t.size() > r.size() && t.rfind(r + L"\\", 0) == 0);
        }
        catch (...) {
            return false;
        }
    }

    std::filesystem::path GetExeDirectoryForPortalServer_()
    {
        wchar_t modulePath[MAX_PATH]{};
        const DWORD n = ::GetModuleFileNameW(nullptr, modulePath, MAX_PATH);

        if (n > 0 && n < MAX_PATH) {
            return std::filesystem::path(modulePath).parent_path();
        }

        std::error_code ec;
        auto cur = std::filesystem::current_path(ec);
        if (!ec && !cur.empty()) {
            return cur;
        }

        return std::filesystem::path(L".");
    }

    std::filesystem::path GetNetworkIniPath_()
    {
        return GetExeDirectoryForPortalServer_() / L"config.ini";
    }

    std::wstring ReadNetworkIniString_(
        const wchar_t* key,
        const wchar_t* defaultValue = L"")
    {
        wchar_t value[4096]{};
        const std::filesystem::path iniPath = GetNetworkIniPath_();
        ::GetPrivateProfileStringW(
            L"Network",
            key,
            defaultValue ? defaultValue : L"",
            value,
            static_cast<DWORD>(std::size(value)),
            iniPath.c_str());
        return std::wstring(value);
    }

    bool ReadNetworkIniBool_(const wchar_t* key, bool defaultValue)
    {
        const std::filesystem::path iniPath = GetNetworkIniPath_();
        return ::GetPrivateProfileIntW(
            L"Network",
            key,
            defaultValue ? 1 : 0,
            iniPath.c_str()) != 0;
    }

    int ReadNetworkIniInt_(const wchar_t* key, int defaultValue)
    {
        const std::filesystem::path iniPath = GetNetworkIniPath_();
        return static_cast<int>(::GetPrivateProfileIntW(
            L"Network",
            key,
            defaultValue,
            iniPath.c_str()));
    }

    int HexValue_(wchar_t ch) noexcept
    {
        if (ch >= L'0' && ch <= L'9') return ch - L'0';
        if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
        if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
        return -1;
    }

    std::wstring UnprotectNetworkSecret_(const std::wstring& encoded)
    {
        if (encoded.empty() || (encoded.size() % 2U) != 0U) return {};

        std::vector<BYTE> encrypted(encoded.size() / 2U);
        for (std::size_t i = 0; i < encrypted.size(); ++i) {
            const int hi = HexValue_(encoded[i * 2U]);
            const int lo = HexValue_(encoded[i * 2U + 1U]);
            if (hi < 0 || lo < 0) return {};
            encrypted[i] = static_cast<BYTE>((hi << 4) | lo);
        }

        DATA_BLOB input{};
        input.cbData = static_cast<DWORD>(encrypted.size());
        input.pbData = encrypted.data();

        DATA_BLOB output{};
        if (!::CryptUnprotectData(
            &input,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &output)) {
            ::SecureZeroMemory(encrypted.data(), encrypted.size());
            return {};
        }

        std::wstring plainText;
        if ((output.cbData % sizeof(wchar_t)) == 0U) {
            const auto* text = reinterpret_cast<const wchar_t*>(output.pbData);
            plainText.assign(text, text + output.cbData / sizeof(wchar_t));
        }

        ::SecureZeroMemory(encrypted.data(), encrypted.size());
        ::SecureZeroMemory(output.pbData, output.cbData);
        ::LocalFree(output.pbData);
        return plainText;
    }

    bool ConstantTimeEqual_(const std::wstring& a, const std::wstring& b) noexcept
    {
        const std::size_t count = (std::max)(a.size(), b.size());
        std::size_t difference = a.size() ^ b.size();
        for (std::size_t i = 0; i < count; ++i) {
            const wchar_t av = i < a.size() ? a[i] : L'\0';
            const wchar_t bv = i < b.size() ? b[i] : L'\0';
            difference |= static_cast<std::size_t>(av ^ bv);
        }
        return difference == 0U;
    }

    std::int64_t CurrentUnixSeconds_() noexcept
    {
        const auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();
    }

    struct ListenAddressConfig_
    {
        unsigned long address = htonl(INADDR_LOOPBACK);
        const wchar_t* displayAddress = L"127.0.0.1";
    };

    ListenAddressConfig_ ReadListenAddressConfig_()
    {
        // NetworkSettingsTab と同じ config.ini / [Network] / BindAddress を読む。
        // 未設定値や不正値は外部公開しないよう、必ずループバックへ戻す。
        const std::filesystem::path iniPath = GetNetworkIniPath_();

        wchar_t value[64]{};
        ::GetPrivateProfileStringW(
            L"Network",
            L"BindAddress",
            L"127.0.0.1",
            value,
            static_cast<DWORD>(std::size(value)),
            iniPath.c_str());

        std::wstring bindAddress(value);

        const auto first = bindAddress.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) {
            bindAddress.clear();
        }
        else {
            const auto last = bindAddress.find_last_not_of(L" \t\r\n");
            bindAddress = bindAddress.substr(first, last - first + 1);
        }

        if (bindAddress == L"0.0.0.0") {
            return { htonl(INADDR_ANY), L"0.0.0.0" };
        }

        if (!bindAddress.empty() && bindAddress != L"127.0.0.1") {
            DebugLogFormat(
                L"[EPG-PORTAL][API] unsupported BindAddress='%s'; fallback=127.0.0.1",
                bindAddress.c_str());
        }

        return { htonl(INADDR_LOOPBACK), L"127.0.0.1" };
    }

    bool ParseUnsignedQueryValue_(
        const std::string& requestPath,
        const std::string_view key,
        std::uint64_t& outValue) noexcept
    {
        outValue = 0;

        const std::size_t queryPos = requestPath.find('?');
        if (queryPos == std::string::npos) {
            return false;
        }

        std::size_t pos = queryPos + 1;
        while (pos < requestPath.size()) {
            const std::size_t end = requestPath.find('&', pos);
            const std::size_t itemEnd =
                (end == std::string::npos) ? requestPath.size() : end;
            const std::size_t equal = requestPath.find('=', pos);

            if (equal != std::string::npos && equal < itemEnd &&
                requestPath.compare(pos, equal - pos, key) == 0) {
                if (equal + 1 >= itemEnd) {
                    return false;
                }

                std::uint64_t value = 0;
                for (std::size_t i = equal + 1; i < itemEnd; ++i) {
                    const unsigned char ch =
                        static_cast<unsigned char>(requestPath[i]);
                    if (ch < '0' || ch > '9') {
                        return false;
                    }

                    const std::uint64_t digit =
                        static_cast<std::uint64_t>(ch - '0');
                    if (value >
                        (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
                        return false;
                    }
                    value = value * 10u + digit;
                }

                outValue = value;
                return true;
            }

            if (end == std::string::npos) {
                break;
            }
            pos = end + 1;
        }

        return false;
    }

    bool ReadFileRangeShared_(
        HANDLE file,
        const std::uint64_t offset,
        const std::size_t byteCount,
        std::string& outData) noexcept
    {
        outData.clear();
        if (byteCount == 0) {
            return true;
        }

        LARGE_INTEGER fileOffset{};
        fileOffset.QuadPart = static_cast<LONGLONG>(offset);
        if (!::SetFilePointerEx(file, fileOffset, nullptr, FILE_BEGIN)) {
            return false;
        }

        try {
            outData.resize(byteCount);
        }
        catch (...) {
            return false;
        }

        std::size_t totalRead = 0;
        while (totalRead < byteCount) {
            const DWORD requestBytes = static_cast<DWORD>((std::min<std::size_t>)(
                byteCount - totalRead,
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));

            DWORD readBytes = 0;
            if (!::ReadFile(
                file,
                outData.data() + totalRead,
                requestBytes,
                &readBytes,
                nullptr)) {
                outData.clear();
                return false;
            }

            if (readBytes == 0) {
                break;
            }
            totalRead += static_cast<std::size_t>(readBytes);
        }

        outData.resize(totalRead);
        return true;
    }

    void RemoveUtf8Bom_(std::string& text) noexcept
    {
        if (text.size() >= 3 &&
            static_cast<unsigned char>(text[0]) == 0xEFu &&
            static_cast<unsigned char>(text[1]) == 0xBBu &&
            static_cast<unsigned char>(text[2]) == 0xBFu) {
            text.erase(0, 3);
        }
    }

    std::string JsonEscapeUtf8Bytes_(const std::string_view text)
    {
        std::string out;
        out.reserve(text.size() + 16);

        constexpr char kHex[] = "0123456789abcdef";
        for (const unsigned char ch : text) {
            switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20u) {
                    out += "\\u00";
                    out.push_back(kHex[(ch >> 4u) & 0x0Fu]);
                    out.push_back(kHex[ch & 0x0Fu]);
                }
                else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
            }
        }

        return out;
    }

    std::vector<std::string> SplitSystemLogLines_(std::string text)
    {
        RemoveUtf8Bom_(text);

        std::vector<std::string> lines;
        std::size_t pos = 0;
        while (pos < text.size()) {
            const std::size_t newline = text.find('\n', pos);
            const std::size_t end =
                (newline == std::string::npos) ? text.size() : newline;

            std::size_t lineEnd = end;
            if (lineEnd > pos && text[lineEnd - 1] == '\r') {
                --lineEnd;
            }
            lines.emplace_back(text.substr(pos, lineEnd - pos));

            if (newline == std::string::npos) {
                break;
            }
            pos = newline + 1;
        }

        return lines;
    }

    std::string BuildAutoReserveSaveResponseJson_(
        const AutoReserve::SaveResult& save,
        const AutoReserve::CurrentSearchRegisterResult& registration)
    {
        std::string json;
        json.reserve(1024);

        json += "{";

        json += "\"ok\":";
        json += save.ok ? "true" : "false";

        json += ",\"savedCount\":";
        json += std::to_string(save.savedCount);

        json += ",\"message\":\"";
        json += JsonEscapeUtf8Local_(save.message);
        json += "\"";

        if (registration.searchAttempted) {
            json += ",\"autoSearch\":{";

            json += "\"ok\":";
            json += registration.searchOk ? "true" : "false";

            json += ",\"rules\":";
            json += std::to_string(registration.matchResult.ruleCount);

            json += ",\"programs\":";
            json += std::to_string(registration.matchResult.programCount);

            json += ",\"checked\":";
            json += std::to_string(registration.matchResult.checkedCount);

            json += ",\"matched\":";
            json += std::to_string(registration.matchedCount);

            json += ",\"candidate\":";
            json += std::to_string(registration.candidateCount);

            json += ",\"auto\":";
            json += std::to_string(registration.autoRegisterCount);

            json += ",\"message\":\"";
            json += JsonEscapeUtf8Local_(registration.matchResult.message);
            json += "\"";

            json += "}";
        }

        if (registration.searchAttempted ||
            registration.registrationAttempted) {
            json += ",\"autoRegister\":{";

            json += "\"ok\":";
            json += registration.ok ? "true" : "false";

            json += ",\"attempted\":";
            json += registration.registrationAttempted ? "true" : "false";

            json += ",\"reservationJsOk\":";
            json += registration.reservationJsOk ? "true" : "false";

            json += ",\"input\":";
            json += std::to_string(registration.registerInput);

            json += ",\"target\":";
            json += std::to_string(registration.registerTarget);

            json += ",\"registered\":";
            json += std::to_string(registration.registered);

            json += ",\"dryRunSkipped\":";
            json += std::to_string(registration.dryRunSkipped);

            json += ",\"duplicateSkipped\":";
            json += std::to_string(registration.duplicateSkipped);

            json += ",\"userBlockedSkipped\":";
            json += std::to_string(registration.userBlockedSkipped);

            json += ",\"invalidSkipped\":";
            json += std::to_string(registration.invalidSkipped);

            json += ",\"limitSkipped\":";
            json += std::to_string(registration.limitSkipped);

            json += ",\"actionSkipped\":";
            json += std::to_string(registration.actionSkipped);

            json += ",\"failed\":";
            json += std::to_string(registration.failed);

            json += ",\"message\":\"";
            json += JsonEscapeUtf8Local_(
                registration.registerMessage.empty()
                ? registration.message
                : registration.registerMessage);
            json += "\"";

            json += "}";
        }

        json += "}";

        return json;
    }



}// namespace

EpgPortalReservationApiServer::~EpgPortalReservationApiServer()
{
    Stop();
}

bool EpgPortalReservationApiServer::Start(
    std::uint16_t port,
    TaskSystemConnector* connector,
    const EpgHtmlPortal::WriteOptions& writeOptions)
{
    Stop();

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        port_ = port;
        connector_ = connector;
        writeOptions_ = writeOptions;
    }

    if (!LoadAuthenticationConfig_()) {
        DebugLog(L"[EPG-PORTAL][AUTH] invalid login configuration");
        return false;
    }

    if (!InitWinsock_()) {
        ResetAuthenticationState_();
        DebugLog(L"[EPG-PORTAL][API] WSAStartup failed");
        return false;
    }

    if (!OpenListenSocket_()) {
        CleanupWinsock_();
        ResetAuthenticationState_();
        DebugLog(L"[EPG-PORTAL][API] listen socket open failed");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(clientQueueMutex_);
        clientQueue_.clear();
    }

    servedRequests_.store(0, std::memory_order_relaxed);
    rejectedRequests_.store(0, std::memory_order_relaxed);

    running_.store(true, std::memory_order_release);

    const unsigned hc = std::thread::hardware_concurrency();
    workerCount_ = static_cast<std::uint32_t>((std::min)(8u, (std::max)(2u, hc == 0 ? 4u : hc)));
    maxQueuedClients_ = 256;

    try {
        clientWorkers_.clear();
        clientWorkers_.reserve(workerCount_);
        for (std::uint32_t i = 0; i < workerCount_; ++i) {
            clientWorkers_.emplace_back([this]() { ClientWorkerMain_(); });
        }

        worker_ = std::thread([this]() { WorkerMain_(); });
    }
    catch (...) {
        running_.store(false, std::memory_order_release);
        CloseListenSocket_();
        clientQueueCv_.notify_all();

        for (auto& th : clientWorkers_) {
            if (th.joinable()) {
                try { th.join(); }
                catch (...) {}
            }
        }
        clientWorkers_.clear();

        CleanupWinsock_();
        ResetAuthenticationState_();
        return false;
    }

    DebugLogFormat(
        L"[EPG-PORTAL][API] started on port=%u workers=%u queue=%u",
        static_cast<unsigned>(port_),
        static_cast<unsigned>(workerCount_),
        static_cast<unsigned>(maxQueuedClients_));
    return true;
}

void EpgPortalReservationApiServer::Stop()
{
    const bool wasRunning = running_.exchange(false, std::memory_order_acq_rel);

    CloseListenSocket_();

    {
        std::lock_guard<std::mutex> lock(clientQueueMutex_);
        CloseQueuedClients_();
    }
    clientQueueCv_.notify_all();

    if (worker_.joinable()) {
        try {
            worker_.join();
        }
        catch (...) {
        }
    }

    for (auto& th : clientWorkers_) {
        if (th.joinable()) {
            try {
                th.join();
            }
            catch (...) {
            }
        }
    }
    clientWorkers_.clear();

    if (wasRunning || winsockReady_) {
        CleanupWinsock_();
    }

    ResetAuthenticationState_();
}

void EpgPortalReservationApiServer::SetConnector(TaskSystemConnector* connector)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    connector_ = connector;
}

void EpgPortalReservationApiServer::SetWriteOptions(
    const EpgHtmlPortal::WriteOptions& writeOptions)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    writeOptions_ = writeOptions;
}

bool EpgPortalReservationApiServer::LoadAuthenticationConfig_()
{
    forceLoopbackForInvalidAuth_ = false;
    ResetAuthenticationState_();

    const bool authRequested =
        ReadNetworkIniBool_(L"EnablePasswordAuth", false);

    if (!authRequested) {
        DebugLog(L"[EPG-PORTAL][AUTH] password login disabled by configuration");
        return true;
    }

    // 未登録と暗号化データの破損・復号失敗を区別する。
    const std::wstring protectedPassword =
        ReadNetworkIniString_(L"LoginPasswordProtected", L"");

    // パスワードが未登録なら、従来仕様どおりログインなしで動作する。
    // LANでの公開範囲は BindAddress / AllowRemoteAccess 側が決める。
    if (protectedPassword.empty()) {
        DebugLog(L"[EPG-PORTAL][AUTH] no password registered; guest access enabled");
        return true;
    }

    loginUser_ = ReadNetworkIniString_(L"LoginUser", L"");
    loginPassword_ = UnprotectNetworkSecret_(protectedPassword);

    // 暗号化済みパスワードが存在するのにユーザー名が空、または
    // パスワードが復号できなかった場合はLANへの無認証公開を防ぐ。
    if (loginUser_.empty() || loginPassword_.empty()) {
        ResetAuthenticationState_();
        forceLoopbackForInvalidAuth_ = true;
        DebugLog(
            L"[EPG-PORTAL][AUTH][WARN] incomplete or unreadable credentials; "
            L"LAN access restricted to 127.0.0.1");
        return true;
    }

    const int timeoutMinutes =
        ReadNetworkIniInt_(L"SessionTimeoutMinutes", 720);
    const int clampedMinutes =
        (std::min)(1440, (std::max)(5, timeoutMinutes));
    sessionLifetimeSeconds_ = static_cast<std::int64_t>(clampedMinutes) * 60;

    passwordAuthEnabled_ = true;
    DebugLogFormat(
        L"[EPG-PORTAL][AUTH] password login enabled sessionMinutes=%d",
        clampedMinutes);
    return true;
}

void EpgPortalReservationApiServer::ResetAuthenticationState_() noexcept
{
    {
        std::lock_guard<std::mutex> lock(authSessionMutex_);
        authSessions_.clear();
    }

    if (!loginPassword_.empty()) {
        ::SecureZeroMemory(
            loginPassword_.data(),
            loginPassword_.size() * sizeof(wchar_t));
    }
    loginPassword_.clear();
    loginUser_.clear();
    passwordAuthEnabled_ = false;
    sessionLifetimeSeconds_ = std::int64_t{ 12 } *60 * 60;
}

std::string EpgPortalReservationApiServer::GetCookieValue_(
    const RequestContext& req,
    const char* name)
{
    if (!name || !*name) return {};

    const auto headerIt = req.headers.find("cookie");
    if (headerIt == req.headers.end()) return {};

    const std::string target(name);
    const std::string& cookies = headerIt->second;
    std::size_t pos = 0;

    while (pos < cookies.size()) {
        while (pos < cookies.size() &&
            (cookies[pos] == ' ' || cookies[pos] == ';')) {
            ++pos;
        }

        const std::size_t end = cookies.find(';', pos);
        const std::size_t itemEnd =
            end == std::string::npos ? cookies.size() : end;
        const std::size_t equal = cookies.find('=', pos);

        if (equal != std::string::npos && equal < itemEnd) {
            std::string key = cookies.substr(pos, equal - pos);
            while (!key.empty() && key.back() == ' ') key.pop_back();
            if (key == target) {
                return cookies.substr(equal + 1, itemEnd - equal - 1);
            }
        }

        if (end == std::string::npos) break;
        pos = end + 1;
    }

    return {};
}

bool EpgPortalReservationApiServer::IsRequestAuthenticated_(
    const RequestContext& req)
{
    if (!passwordAuthEnabled_) return true;

    const std::string token = GetCookieValue_(req, "EpgPortalSession");
    if (token.empty()) return false;

    const std::int64_t now = CurrentUnixSeconds_();
    std::lock_guard<std::mutex> lock(authSessionMutex_);

    for (auto it = authSessions_.begin(); it != authSessions_.end();) {
        if (it->second <= now) {
            it = authSessions_.erase(it);
        }
        else {
            ++it;
        }
    }

    const auto found = authSessions_.find(token);
    if (found == authSessions_.end()) return false;

    found->second = now + sessionLifetimeSeconds_;
    return true;
}

bool EpgPortalReservationApiServer::ValidateLoginForm_(
    const std::string& formBody) const
{
    auto form = ParseFormUrlEncoded_(formBody);
    const std::wstring user = GetFormValue_(form, L"user");
    std::wstring password = GetFormValue_(form, L"password");

    const bool userMatches = ConstantTimeEqual_(user, loginUser_);
    const bool passwordMatches = ConstantTimeEqual_(password, loginPassword_);
    if (!password.empty()) {
        ::SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
    }
    const auto passwordIt = form.find(L"password");
    if (passwordIt != form.end() && !passwordIt->second.empty()) {
        ::SecureZeroMemory(
            passwordIt->second.data(),
            passwordIt->second.size() * sizeof(wchar_t));
    }
    // 両方の定時間比較は上で実行済みなので、ここでは論理積でよい。
    return userMatches && passwordMatches;
}

std::string EpgPortalReservationApiServer::CreateSessionToken_()
{
    std::array<unsigned char, 32> randomBytes{};
    const NTSTATUS status = ::BCryptGenRandom(
        nullptr,
        randomBytes.data(),
        static_cast<ULONG>(randomBytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) return {};

    static constexpr char kHex[] = "0123456789abcdef";
    std::string token;
    token.resize(randomBytes.size() * 2U);
    for (std::size_t i = 0; i < randomBytes.size(); ++i) {
        token[i * 2U] = kHex[randomBytes[i] >> 4];
        token[i * 2U + 1U] = kHex[randomBytes[i] & 0x0F];
    }
    ::SecureZeroMemory(randomBytes.data(), randomBytes.size());

    const std::int64_t expires = CurrentUnixSeconds_() + sessionLifetimeSeconds_;
    {
        std::lock_guard<std::mutex> lock(authSessionMutex_);
        authSessions_[token] = expires;
    }
    return token;
}

void EpgPortalReservationApiServer::RemoveRequestSession_(
    const RequestContext& req)
{
    const std::string token = GetCookieValue_(req, "EpgPortalSession");
    if (token.empty()) return;

    std::lock_guard<std::mutex> lock(authSessionMutex_);
    authSessions_.erase(token);
}

std::string EpgPortalReservationApiServer::BuildLoginPage_(bool loginFailed)
{
    std::string html;
    html.reserve(2048);
    html += "<!doctype html><html lang=\"ja\"><head>";
    html += "<meta charset=\"utf-8\">";
    html += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
    html += "<title>EPG Portal Login</title>";
    html += "<style>html{min-height:100%;background:#eef2f7}";
    html += "body{box-sizing:border-box;margin:0;min-height:100%;min-height:100dvh;";
    html += "padding:env(safe-area-inset-top) env(safe-area-inset-right) ";
    html += "env(safe-area-inset-bottom) env(safe-area-inset-left);overflow-x:hidden;";
    html += "overflow-y:auto;-webkit-overflow-scrolling:touch;background:#eef2f7;";
    html += "font-family:sans-serif;color:#172033}";
    html += ".box{box-sizing:border-box;width:min(calc(100% - 24px),380px);";
    html += "margin:clamp(12px,8vh,72px) auto;background:#fff;padding:28px;";
    html += "border-radius:14px;box-shadow:0 12px 35px #20304a25}";
    html += "h1{font-size:1.35rem;margin:0 0 22px}label{display:block;margin:13px 0 6px}";
    html += "input{box-sizing:border-box;width:100%;min-height:44px;padding:11px;";
    html += "border:1px solid #aeb8c7;border-radius:8px;font-size:16px}";
    html += "button{width:100%;margin-top:22px;padding:12px;border:0;border-radius:8px;";
    html += "min-height:44px;background:#2459d3;color:#fff;font-weight:700}.err{color:#b42318}";
    html += "@media(max-width:420px),(max-height:520px){.box{margin:12px auto;padding:20px}";
    html += "h1{margin-bottom:16px}button{margin-top:18px}}</style></head><body>";
    html += "<main class=\"box\"><h1>EPG Portal Login</h1>";
    if (loginFailed) {
        html += "<p class=\"err\">&#12518;&#12540;&#12470;&#12540;&#21517;&#12414;&#12383;&#12399;";
        html += "&#12497;&#12473;&#12527;&#12540;&#12489;&#12364;&#36949;&#12356;&#12414;&#12377;&#12290;</p>";
    }
    html += "<form method=\"post\" action=\"/login\">";
    html += "<label for=\"user\">User</label><input id=\"user\" name=\"user\" autocomplete=\"username\" required>";
    html += "<label for=\"password\">Password</label><input id=\"password\" name=\"password\" type=\"password\" autocomplete=\"current-password\" required>";
    html += "<button type=\"submit\">Login</button></form></main></body></html>";
    return html;
}

bool EpgPortalReservationApiServer::InitWinsock_()
{
    if (winsockReady_) return true;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return false;
    }

    winsockReady_ = true;
    return true;
}

void EpgPortalReservationApiServer::CleanupWinsock_() noexcept
{
    if (!winsockReady_) return;
    WSACleanup();
    winsockReady_ = false;
}

bool EpgPortalReservationApiServer::OpenListenSocket_()
{
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return false;
    }

    listenSock_ = static_cast<std::uintptr_t>(s);

    int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&yes), sizeof(yes));

    // ローカルHTTPの小さいAPI応答を詰まらせない。
    // 静的ファイルはSendAllでまとめて返すため、Nagleの影響を避ける。
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
        reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);

    ListenAddressConfig_ listenAddress{};
    if (forceLoopbackForInvalidAuth_) {
        listenAddress = { htonl(INADDR_LOOPBACK), L"127.0.0.1" };
        DebugLog(
            L"[EPG-PORTAL][AUTH] safety fallback active; "
            L"LAN access is blocked until login configuration is corrected");
    }
    else if (!ReadNetworkIniBool_(L"AllowRemoteAccess", true)) {
        listenAddress = { htonl(INADDR_LOOPBACK), L"127.0.0.1" };
        DebugLog(L"[EPG-PORTAL][API] AllowRemoteAccess=0; loopback only");
    }
    else {
        listenAddress = ReadListenAddressConfig_();
    }
    addr.sin_addr.s_addr = listenAddress.address;

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int errorCode = ::WSAGetLastError();
        DebugLogFormat(
            L"[EPG-PORTAL][API] bind failed address=%s port=%u error=%d",
            listenAddress.displayAddress,
            static_cast<unsigned>(port_),
            errorCode);
        CloseListenSocket_();
        return false;
    }

    // ブラウザは複数コネクションを同時に張るので、backlogは大きめにする。
    if (::listen(s, SOMAXCONN) != 0) {
        CloseListenSocket_();
        return false;
    }

    DebugLogFormat(
        L"[EPG-PORTAL][API] listening address=%s port=%u",
        listenAddress.displayAddress,
        static_cast<unsigned>(port_));

    return true;
}

void EpgPortalReservationApiServer::CloseListenSocket_() noexcept
{
    if (listenSock_ != kInvalidSocketHandle_) {
        const SOCKET s = static_cast<SOCKET>(listenSock_);
        listenSock_ = kInvalidSocketHandle_;
        ::shutdown(s, SD_BOTH);
        ::closesocket(s);
    }
}

void EpgPortalReservationApiServer::WorkerMain_()
{
    while (running_.load(std::memory_order_acquire)) {
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);

        const SOCKET listenSocket = static_cast<SOCKET>(listenSock_);
        const SOCKET client = ::accept(
            listenSocket,
            reinterpret_cast<sockaddr*>(&clientAddr),
            &clientLen);

        if (client == INVALID_SOCKET) {
            if (running_.load(std::memory_order_acquire)) {
                ::Sleep(2);
            }
            continue;
        }

        ApplyClientSocketTimeout_(client);

        int yes = 1;
        ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
            reinterpret_cast<const char*>(&yes), sizeof(yes));

        if (!QueueClient_(static_cast<std::uintptr_t>(client))) {
            rejectedRequests_.fetch_add(1, std::memory_order_relaxed);
            CloseClientSocket_(client);
        }
    }
}

bool EpgPortalReservationApiServer::QueueClient_(std::uintptr_t clientSock)
{
    if (clientSock == kInvalidSocketHandle_) {
        return false;
    }

    std::lock_guard<std::mutex> lock(clientQueueMutex_);

    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    if (clientQueue_.size() >= maxQueuedClients_) {
        return false;
    }

    clientQueue_.push_back(clientSock);
    clientQueueCv_.notify_one();
    return true;
}

bool EpgPortalReservationApiServer::PopClientSocket_(std::uintptr_t& outClientSock) noexcept
{
    outClientSock = kInvalidSocketHandle_;

    std::unique_lock<std::mutex> lock(clientQueueMutex_);

    clientQueueCv_.wait(lock, [this]() noexcept {
        return !running_.load(std::memory_order_acquire) || !clientQueue_.empty();
        });

    if (clientQueue_.empty()) {
        return false;
    }

    outClientSock = clientQueue_.front();
    clientQueue_.pop_front();

    return outClientSock != kInvalidSocketHandle_;
}

void EpgPortalReservationApiServer::ClientWorkerMain_()
{
    for (;;) {
        std::uintptr_t clientSock = kInvalidSocketHandle_;

        const bool hasClient = PopClientSocket_(clientSock);

        if (!hasClient) {
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
            continue;
        }

        if (clientSock == kInvalidSocketHandle_) {
            continue;
        }

        try {
            HandleClient_(clientSock);
            servedRequests_.fetch_add(1, std::memory_order_relaxed);
        }
        catch (...) {
            ::OutputDebugStringW(
                L"[EPG-PORTAL][API] ClientWorkerMain HandleClient exception\n");
        }

        CloseClientSocket_(static_cast<SOCKET>(clientSock));
    }
}

void EpgPortalReservationApiServer::CloseQueuedClients_() noexcept
{
    while (!clientQueue_.empty()) {
        const std::uintptr_t s = clientQueue_.front();
        clientQueue_.pop_front();
        CloseClientSocket_(static_cast<SOCKET>(s));
    }
}

bool EpgPortalReservationApiServer::ReadRequest_(std::uintptr_t clientSock, RequestContext& outReq)
{
    std::string raw;
    raw.reserve(8192);

    std::array<char, 4096> buf{};
    int headerEnd = -1;
    int contentLength = 0;

    while (true) {
        const int n = recv(static_cast<SOCKET>(clientSock), buf.data(),
            static_cast<int>(buf.size()), 0);
        if (n <= 0) {
            return false;
        }

        raw.append(buf.data(), static_cast<std::size_t>(n));

        const std::size_t pos = raw.find("\r\n\r\n");
        if (pos != std::string::npos) {
            headerEnd = static_cast<int>(pos + 4);

            std::istringstream hs(raw.substr(0, pos));
            std::string line;
            if (!std::getline(hs, line)) {
                return false;
            }

            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::istringstream ls(line);
            ls >> outReq.method >> outReq.path;

            while (std::getline(hs, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const auto colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string key = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                std::transform(key.begin(), key.end(), key.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const std::size_t valueFirst = value.find_first_not_of(" \t");
                if (valueFirst == std::string::npos) {
                    value.clear();
                }
                else {
                    const std::size_t valueLast = value.find_last_not_of(" \t");
                    value = value.substr(valueFirst, valueLast - valueFirst + 1U);
                }
                outReq.headers[key] = value;
                if (key == "content-length") {
                    contentLength = (std::max)(0, std::atoi(value.c_str()));
                    if (contentLength > (4 * 1024 * 1024)) {
                        return false;
                    }
                }
            }

            break;
        }

        if (raw.size() > (std::size_t{ 1 } << 20)) {
            return false;
        }
    }

    while (static_cast<int>(raw.size()) < headerEnd + contentLength) {
        const int n = recv(static_cast<SOCKET>(clientSock), buf.data(),
            static_cast<int>(buf.size()), 0);
        if (n <= 0) {
            return false;
        }
        raw.append(buf.data(), static_cast<std::size_t>(n));
    }

    if (contentLength > 0) {
        outReq.body = raw.substr(static_cast<std::size_t>(headerEnd),
            static_cast<std::size_t>(contentLength));
    }

    return !outReq.method.empty() && !outReq.path.empty();
}

void EpgPortalReservationApiServer::HandleClient_(std::uintptr_t clientSock)
{
    const SOCKET sock = static_cast<SOCKET>(clientSock);
    ApplyClientSocketTimeout_(sock);

    try {
        RequestContext req{};
        if (!ReadRequest_(clientSock, req)) {
            const auto resp = BuildHttpResponse(
                400,
                kHttpBadRequest,
                R"({"ok":false,"reason":"bad-request"})",
                "application/json; charset=utf-8");
            if (!SendAllSocket_(clientSock, resp)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][API][WARN] send failed path=%S bytes=%zu",
                    req.path.c_str(),
                    resp.size());
            }
            return;
        }

        const std::string pathOnly = StripQueryString_(req.path);

        if (passwordAuthEnabled_ && pathOnly == "/login") {
            if (req.method == "GET" || req.method == "HEAD") {
                if (IsRequestAuthenticated_(req)) {
                    const auto resp = BuildHttpResponse(
                        303,
                        kHttpSeeOther,
                        {},
                        "text/plain; charset=utf-8",
                        "Location: /\r\n");
                    SendAllSocket_(clientSock, resp);
                    return;
                }
                std::string body = BuildLoginPage_(false);
                if (req.method == "HEAD") body.clear();
                const auto resp = BuildHttpResponse(
                    200,
                    kHttpOk,
                    body,
                    "text/html; charset=utf-8");
                SendAllSocket_(clientSock, resp);
                return;
            }

            if (req.method == "POST") {
                const bool loginValid = ValidateLoginForm_(req.body);
                if (!req.body.empty()) {
                    ::SecureZeroMemory(req.body.data(), req.body.size());
                    req.body.clear();
                }

                if (loginValid) {
                    const std::string token = CreateSessionToken_();
                    if (token.empty()) {
                        const auto resp = BuildHttpResponse(
                            500,
                            kHttpServerError,
                            "session-token-generation-failed",
                            "text/plain; charset=utf-8");
                        SendAllSocket_(clientSock, resp);
                        return;
                    }

                    std::string headers;
                    headers += "Location: /\r\n";
                    headers += "Set-Cookie: EpgPortalSession=";
                    headers += token;
                    headers += "; Path=/; HttpOnly; SameSite=Strict; Max-Age=";
                    headers += std::to_string(sessionLifetimeSeconds_);
                    headers += "\r\n";

                    const auto resp = BuildHttpResponse(
                        303,
                        kHttpSeeOther,
                        {},
                        "text/plain; charset=utf-8",
                        headers);
                    SendAllSocket_(clientSock, resp);
                    return;
                }

                ::Sleep(250);
                const auto resp = BuildHttpResponse(
                    401,
                    kHttpUnauthorized,
                    BuildLoginPage_(true),
                    "text/html; charset=utf-8");
                SendAllSocket_(clientSock, resp);
                return;
            }

            const auto resp = BuildHttpResponse(
                405,
                kHttpMethodNotAllowed,
                "method-not-allowed",
                "text/plain; charset=utf-8");
            SendAllSocket_(clientSock, resp);
            return;
        }

        if (passwordAuthEnabled_ && pathOnly == "/logout") {
            RemoveRequestSession_(req);
            const std::string headers =
                "Location: /login\r\n"
                "Set-Cookie: EpgPortalSession=; Path=/; HttpOnly; "
                "SameSite=Strict; Max-Age=0\r\n";
            const auto resp = BuildHttpResponse(
                303,
                kHttpSeeOther,
                {},
                "text/plain; charset=utf-8",
                headers);
            SendAllSocket_(clientSock, resp);
            return;
        }

        if (passwordAuthEnabled_ && !IsRequestAuthenticated_(req)) {
            if (pathOnly.rfind("/api/", 0) == 0) {
                const auto resp = BuildHttpResponse(
                    401,
                    kHttpUnauthorized,
                    "{\"ok\":false,\"reason\":\"authentication-required\"}",
                    "application/json; charset=utf-8");
                SendAllSocket_(clientSock, resp);
            }
            else {
                const auto resp = BuildHttpResponse(
                    303,
                    kHttpSeeOther,
                    {},
                    "text/plain; charset=utf-8",
                    "Location: /login\r\n");
                SendAllSocket_(clientSock, resp);
            }
            return;
        }

        int statusCode = 200;
        std::string contentType = "application/json; charset=utf-8";
        std::string body = DispatchRequest_(req, statusCode, contentType);

        if (req.method == "HEAD") {
            body.clear();
        }

        const auto resp = BuildHttpResponse(
            statusCode,
            StatusTextFromCode_(statusCode),
            body,
            contentType.c_str());

        SendAllSocket_(clientSock, resp);
    }
    catch (const std::out_of_range& e) {
        DebugLogFormat(L"[EPG-PORTAL][API][ERROR] std::out_of_range: %S", e.what());
        const auto resp = BuildHttpResponse(
            500,
            kHttpServerError,
            R"({"ok":false,"reason":"server-out-of-range","items":[]})",
            "application/json; charset=utf-8");
        SendAllSocket_(clientSock, resp);
    }
    catch (const std::exception& e) {
        DebugLogFormat(L"[EPG-PORTAL][API][ERROR] std::exception: %S", e.what());
        const auto resp = BuildHttpResponse(
            500,
            kHttpServerError,
            R"({"ok":false,"reason":"server-exception","items":[]})",
            "application/json; charset=utf-8");
        SendAllSocket_(clientSock, resp);
    }
    catch (...) {
        DebugLog(L"[EPG-PORTAL][API][ERROR] unknown exception");
        const auto resp = BuildHttpResponse(
            500,
            kHttpServerError,
            R"({"ok":false,"reason":"server-unknown-exception","items":[]})",
            "application/json; charset=utf-8");
        SendAllSocket_(clientSock, resp);
    }
}

std::string EpgPortalReservationApiServer::StripQueryString_(const std::string& path)
{
    std::string out = path;
    const std::size_t qpos = out.find('?');
    if (qpos != std::string::npos) {
        out.resize(qpos);
    }
    return out;
}

std::string EpgPortalReservationApiServer::DispatchRequest_(
    const RequestContext& req,
    int& statusCode,
    std::string& contentType)
{
    const std::string pathOnly = StripQueryString_(req.path);
    contentType = "application/json; charset=utf-8";

    if (req.method == "OPTIONS") {
        statusCode = 200;
        return R"({"ok":true})";
    }

    if (pathOnly.rfind("/api/", 0) == 0) {
        if (req.method == "GET" &&
            (pathOnly == "/api/reservation/health" ||
                pathOnly == "/api/reservation/ping")) {
            statusCode = 200;
            return HandleHealth_();
        }

        if (req.method == "GET" && pathOnly == "/api/reservation/list") {
            return HandleList_(statusCode);
        }

        if (req.method == "GET" && pathOnly == "/api/system/log") {
            return HandleSystemLog_(req.path, statusCode);
        }

        if (req.method == "POST" && pathOnly == "/api/reservation/upsert") {
            return HandleUpsert_(req.body, statusCode);
        }

        if (req.method == "POST" && pathOnly == "/api/reservation/remove") {
            return HandleRemove_(req.body, statusCode);
        }

        if (req.method == "POST" && pathOnly == "/api/reservation/set_enabled") {
            return HandleSetEnabled_(req.body, statusCode);
        }

        if (req.method == "POST" && pathOnly == "/api/reservation/remove_by_id") {
            return HandleRemoveById_(req.body, statusCode);
        }

        if (req.method == "POST" && pathOnly == "/api/reservation/set_enabled_by_id") {
            return HandleSetEnabledById_(req.body, statusCode);
        }

        if (req.method == "POST" && pathOnly == "/api/reservation/auto_reserve_rules_save") {
            return HandleAutoReserveRulesSave_(req.body, statusCode);
        }

        statusCode = 404;
        return R"({"ok":false,"reason":"not-found"})";
    }

    if ((req.method == "GET" || req.method == "HEAD") &&
        pathOnly == "/favicon.ico") {
        statusCode = 204;
        contentType = "image/x-icon";
        return {};
    }

    if (req.method == "GET" || req.method == "HEAD") {
        return HandleStaticFile_(pathOnly, statusCode, contentType);
    }

    statusCode = 405;
    return R"({"ok":false,"reason":"method-not-allowed"})";
}

std::string EpgPortalReservationApiServer::HandleHealth_()
{
    return R"({"ok":true})";
}

std::string EpgPortalReservationApiServer::HandleSystemLog_(
    const std::string& requestPath,
    int& statusCode)
{
    constexpr std::uint64_t kDefaultLimit = 300;
    constexpr std::uint64_t kMaximumLimit = 1000;
    constexpr std::uint64_t kMaximumReadBytes = 8ull * 1024ull * 1024ull;

    std::uint64_t cursor = 0;
    if (requestPath.find("cursor=") != std::string::npos &&
        !ParseUnsignedQueryValue_(requestPath, "cursor", cursor)) {
        statusCode = 400;
        return "{\"ok\":false,\"reason\":\"invalid-cursor\",\"lines\":[]}";
    }

    std::uint64_t requestedLimit = kDefaultLimit;
    if (requestPath.find("limit=") != std::string::npos &&
        !ParseUnsignedQueryValue_(requestPath, "limit", requestedLimit)) {
        statusCode = 400;
        return "{\"ok\":false,\"reason\":\"invalid-limit\",\"lines\":[]}";
    }
    requestedLimit = (std::max<std::uint64_t>)(1, requestedLimit);
    requestedLimit = (std::min<std::uint64_t>)(kMaximumLimit, requestedLimit);
    const std::size_t lineLimit = static_cast<std::size_t>(requestedLimit);

    const std::filesystem::path logPath =
        GetExeDirectoryForPortalServer_() / L"system.log";

    const HANDLE file = ::CreateFileW(
        logPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);

    if (file == INVALID_HANDLE_VALUE) {
        const DWORD openError = ::GetLastError();
        statusCode = (openError == ERROR_FILE_NOT_FOUND ||
            openError == ERROR_PATH_NOT_FOUND) ? 404 : 500;
        return "{\"ok\":false,\"reason\":\"system-log-open-failed\",\"lines\":[]}";
    }

    LARGE_INTEGER fileSizeValue{};
    if (!::GetFileSizeEx(file, &fileSizeValue) || fileSizeValue.QuadPart < 0) {
        ::CloseHandle(file);
        statusCode = 500;
        return "{\"ok\":false,\"reason\":\"system-log-size-failed\",\"lines\":[]}";
    }

    const std::uint64_t fileSize =
        static_cast<std::uint64_t>(fileSizeValue.QuadPart);
    const bool reset = (cursor == 0 || cursor > fileSize);

    std::uint64_t readOffset = cursor;
    if (reset) {
        const std::uint64_t tailBytes =
            (std::min<std::uint64_t>)(fileSize, kMaximumReadBytes);
        readOffset = fileSize - tailBytes;
    }

    const std::uint64_t availableBytes = fileSize - readOffset;
    const std::uint64_t readBytes64 =
        (std::min<std::uint64_t>)(availableBytes, kMaximumReadBytes);
    const std::size_t readBytes = static_cast<std::size_t>(readBytes64);

    std::string raw;
    const bool readOk = ReadFileRangeShared_(file, readOffset, readBytes, raw);
    ::CloseHandle(file);

    if (!readOk) {
        statusCode = 500;
        return "{\"ok\":false,\"reason\":\"system-log-read-failed\",\"lines\":[]}";
    }

    // 書込み途中の行は次回取得へ回し、途中でカーソルを進めない。
    std::size_t completeBytes = raw.size();
    if (!raw.empty() && raw.back() != '\n') {
        const std::size_t lastNewline = raw.rfind('\n');
        completeBytes = (lastNewline == std::string::npos) ? 0 : lastNewline + 1;
        raw.resize(completeBytes);
    }

    std::uint64_t nextCursor = readOffset + completeBytes;
    std::vector<std::string> lines;

    if (reset) {
        lines = SplitSystemLogLines_(std::move(raw));
        if (lines.size() > lineLimit) {
            lines.erase(lines.begin(), lines.end() -
                static_cast<std::ptrdiff_t>(lineLimit));
        }
    }
    else {
        // 差分がlimitを超える場合は、limit行目の直後を次のカーソルにする。
        std::size_t consumedBytes = 0;
        std::size_t newlineCount = 0;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\n') {
                ++newlineCount;
                consumedBytes = i + 1;
                if (newlineCount >= lineLimit) {
                    break;
                }
            }
        }

        if (newlineCount >= lineLimit && consumedBytes < raw.size()) {
            raw.resize(consumedBytes);
            nextCursor = readOffset + consumedBytes;
        }

        lines = SplitSystemLogLines_(std::move(raw));
    }

    std::string json;
    json.reserve(128 + lines.size() * 128);
    json += "{\"ok\":true,\"reset\":";
    json += reset ? "true" : "false";
    json += ",\"cursor\":";
    json += std::to_string(nextCursor);
    json += ",\"lines\":[";

    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            json.push_back(',');
        }
        json.push_back('"');
        json += JsonEscapeUtf8Bytes_(lines[i]);
        json.push_back('"');
    }

    json += "]}";
    statusCode = 200;
    return json;
}

std::string EpgPortalReservationApiServer::HandleUpsert_(
    const std::string& formBody, int& statusCode)
{
    TaskSystemConnector* connector = nullptr;
    EpgHtmlPortal::WriteOptions opt{};

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        connector = connector_;
        opt = writeOptions_;
    }

    if (connector == nullptr) {
        statusCode = 500;
        return R"({"ok":false,"reason":"connector-null"})";
    }

    std::lock_guard<std::mutex> apiLock(reservationApiMutex_);

    const auto form = ParseFormUrlEncoded_(formBody);

    EpgHtmlPortal_ReservationData::ProgramReservationMutationCommand cmd{};
    cmd.action = EpgHtmlPortal_ReservationData::ProgramReservationMutationAction::Upsert;
    cmd.payload = BuildPayload_(form);
    cmd.regenerateReservationJs = true;

    const auto r = EpgHtmlPortal_ReservationData::ApplyProgramReservationMutation(*connector, opt, cmd);

    statusCode = r.ok ? 200 : 400;
    return BuildJsonResult_(r, connector);
}

std::string EpgPortalReservationApiServer::HandleRemove_(
    const std::string& formBody, int& statusCode)
{
    TaskSystemConnector* connector = nullptr;
    EpgHtmlPortal::WriteOptions opt{};

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        connector = connector_;
        opt = writeOptions_;
    }

    if (connector == nullptr) {
        statusCode = 500;
        return R"({"ok":false,"reason":"connector-null"})";
    }

    std::lock_guard<std::mutex> apiLock(reservationApiMutex_);

    const auto form = ParseFormUrlEncoded_(formBody);

    EpgHtmlPortal_ReservationData::ProgramReservationMutationCommand cmd{};
    cmd.action = EpgHtmlPortal_ReservationData::ProgramReservationMutationAction::Remove;
    cmd.payload = BuildPayload_(form);
    cmd.regenerateReservationJs = true;

    const auto r = EpgHtmlPortal_ReservationData::ApplyProgramReservationMutation(*connector, opt, cmd);

    statusCode = r.ok ? 200 : 400;
    return BuildJsonResult_(r, connector);
}

std::string EpgPortalReservationApiServer::HandleSetEnabled_(
    const std::string& formBody, int& statusCode)
{
    TaskSystemConnector* connector = nullptr;
    EpgHtmlPortal::WriteOptions opt{};

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        connector = connector_;
        opt = writeOptions_;
    }

    if (connector == nullptr) {
        statusCode = 500;
        return R"({"ok":false,"reason":"connector-null"})";
    }

    std::lock_guard<std::mutex> apiLock(reservationApiMutex_);

    const auto form = ParseFormUrlEncoded_(formBody);

    EpgHtmlPortal_ReservationData::ProgramReservationMutationCommand cmd{};
    cmd.action = EpgHtmlPortal_ReservationData::ProgramReservationMutationAction::SetEnabled;
    cmd.payload = BuildPayload_(form);
    cmd.enabled = ParseBoolLoose_(GetFormValue_(form, L"enabled"), true);
    cmd.disabledByUser = ParseBoolLoose_(GetFormValue_(form, L"disabledByUser"), false);
    cmd.excludeFromAuto = ParseBoolLoose_(GetFormValue_(form, L"excludeFromAuto"), false);
    cmd.regenerateReservationJs = true;

    const auto r = EpgHtmlPortal_ReservationData::ApplyProgramReservationMutation(*connector, opt, cmd);

    statusCode = r.ok ? 200 : 400;
    return BuildJsonResult_(r, connector);
}

std::string EpgPortalReservationApiServer::HandleAutoReserveRulesSave_(
    const std::string& formBody,
    int& statusCode)
{
    AutoReserveRuleSaveFlowResult_ flow{};
    std::string earlyResponseJson;

    if (!SaveAutoReserveRulesFromForm_(
        formBody,
        flow,
        statusCode,
        earlyResponseJson)) {
        return earlyResponseJson;
    }

    if (flow.save.ok) {
        ApplyAutoReserveAfterRuleSave_(flow);
    }

    DebugLogFormat(
        L"[EPG-PORTAL][API][AUTO-RESERVE] save ok=%d count=%zu message=%ls searchAttempted=%d searchOk=%d registrationAttempted=%d matched=%zu candidate=%zu auto=%zu registered=%zu dup=%zu failed=%zu reservationJs=%d",
        flow.save.ok ? 1 : 0,
        flow.save.savedCount,
        flow.save.message.c_str(),
        flow.registration.searchAttempted ? 1 : 0,
        flow.registration.searchOk ? 1 : 0,
        flow.registration.registrationAttempted ? 1 : 0,
        flow.registration.matchedCount,
        flow.registration.candidateCount,
        flow.registration.autoRegisterCount,
        flow.registration.registered,
        flow.registration.duplicateSkipped,
        flow.registration.failed,
        flow.registration.reservationJsOk ? 1 : 0);

    return BuildAutoReserveRuleSaveResponseJson_(flow);
}

EpgPortalReservationApiServer::ApiStateSnapshot_
EpgPortalReservationApiServer::GetApiStateSnapshot_() const
{
    ApiStateSnapshot_ state{};

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        state.connector = connector_;
        state.writeOptions = writeOptions_;
    }

    return state;
}

bool EpgPortalReservationApiServer::SaveAutoReserveRulesFromForm_(
    const std::string& formBody,
    AutoReserveRuleSaveFlowResult_& flow,
    int& statusCode,
    std::string& earlyResponseJson)
{
    earlyResponseJson.clear();

    {
        std::lock_guard<std::mutex> apiLock(reservationApiMutex_);

        flow.save =
            AutoReserve::SaveIniFormUtf8ToDefaultRulesIni(
                formBody);
    }

    if (flow.save.ok) {
        statusCode = 200;
        return true;
    }

    statusCode = 400;
    earlyResponseJson =
        AutoReserve::MakeSaveResultJsonUtf8(
            flow.save);

    DebugLogFormat(
        L"[EPG-PORTAL][API][AUTO-RESERVE][SAVE][WARN] ok=0 message=%ls",
        flow.save.message.c_str());

    return false;
}

void EpgPortalReservationApiServer::ApplyAutoReserveAfterRuleSave_(
    AutoReserveRuleSaveFlowResult_& flow)
{
    const ApiStateSnapshot_ state =
        GetApiStateSnapshot_();

    AutoReserve::CurrentSearchRegisterOptions opt =
        AutoReserve::MakeCurrentSearchRegisterOptions(
            AutoReserve::ApplyTrigger::RuleSave,
            1000);

    opt.dryRun = false;
    opt.allowRegister = true;

    // candidate / auto の区別なし。
    // 条件一致した番組は録画予約登録対象にする。
    opt.includeCandidateRules = true;
    opt.includeAutoRules = true;

    opt.refreshReservationDataJs = true;

    {
        std::lock_guard<std::mutex> apiLock(reservationApiMutex_);

        flow.registration =
            AutoReserve::RunCurrentSearchAndRegisterRecordingTasks(
                state.connector,
                state.writeOptions,
                opt);
    }

    DebugLogFormat(
        L"[EPG-PORTAL][API][AUTO-RESERVE][REGISTER] reason=%ls ok=%d searchAttempted=%d searchOk=%d registrationAttempted=%d matched=%zu candidate=%zu auto=%zu registered=%zu dup=%zu userBlocked=%zu invalid=%zu limit=%zu action=%zu failed=%zu reservationJs=%d message=%ls",
        flow.registration.reason.c_str(),
        flow.registration.ok ? 1 : 0,
        flow.registration.searchAttempted ? 1 : 0,
        flow.registration.searchOk ? 1 : 0,
        flow.registration.registrationAttempted ? 1 : 0,
        flow.registration.matchedCount,
        flow.registration.candidateCount,
        flow.registration.autoRegisterCount,
        flow.registration.registered,
        flow.registration.duplicateSkipped,
        flow.registration.userBlockedSkipped,
        flow.registration.invalidSkipped,
        flow.registration.limitSkipped,
        flow.registration.actionSkipped,
        flow.registration.failed,
        flow.registration.reservationJsOk ? 1 : 0,
        flow.registration.message.c_str());
}


std::string EpgPortalReservationApiServer::BuildAutoReserveRuleSaveResponseJson_(
    const AutoReserveRuleSaveFlowResult_& flow) const
{
    return BuildAutoReserveSaveResponseJson_(
        flow.save,
        flow.registration);
}

std::string EpgPortalReservationApiServer::HandleRemoveById_(
    const std::string& formBody, int& statusCode)
{
    TaskSystemConnector* connector = nullptr;
    EpgHtmlPortal::WriteOptions opt{};

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        connector = connector_;
        opt = writeOptions_;
    }

    if (connector == nullptr) {
        statusCode = 500;
        return R"({"ok":false,"reason":"connector-null"})";
    }

    std::lock_guard<std::mutex> apiLock(reservationApiMutex_);

    const auto form = ParseFormUrlEncoded_(formBody);

    const std::wstring id = GetFormValue_(form, L"id");
    const std::wstring mode = GetFormValue_(form, L"mode");
    const std::wstring dedupeKey = GetFormValue_(form, L"dedupeKey");
    const std::wstring excludeKey = GetFormValue_(form, L"excludeKey");

    std::uint32_t kind = ParseUInt32LooseForApi_(GetFormValue_(form, L"kind"), 0);
    std::uint32_t instance = ParseUInt32LooseForApi_(GetFormValue_(form, L"instance"), 0);

    if ((kind == 0 || instance == 0) && !id.empty()) {
        std::uint32_t parsedKind = 0;
        std::uint32_t parsedInstance = 0;
        if (ParseTaskIdForApi_(id, parsedKind, parsedInstance)) {
            if (kind == 0) kind = parsedKind;
            if (instance == 0) instance = parsedInstance;
        }
    }

    if (instance == 0) {
        const std::wstring key = !dedupeKey.empty() ? dedupeKey : excludeKey;
        if (!key.empty()) {
            instance = HashFNV1a32ForApi_(key);
        }
    }

    if (instance == 0) {
        statusCode = 400;
        return R"({"ok":false,"reason":"invalid-id-or-instance","items":[]})";
    }

    const bool treatAsProgramReservation =
        IsRecordTaskKindForApi_(kind) ||
        IsProgramModeForApi_(mode) ||
        !dedupeKey.empty() ||
        !excludeKey.empty();

    bool changed = false;
    bool allOk = true;

    if (treatAsProgramReservation) {
        changed = connector->RemoveProgramReservationGroupAndApply(instance);
        allOk = changed;
    }
    else {
        changed = connector->RemoveTaskAndApply(kind, instance);
        allOk = changed;
    }

    if (!changed) {
        statusCode = 404;

        std::ostringstream oss;
        oss << "{";
        oss << "\"ok\":false,";
        oss << "\"changed\":false,";
        oss << "\"source\":\"remove_by_id\",";
        oss << "\"instance\":" << instance << ",";
        oss << "\"reason\":\"not-found\",";
        oss << "\"items\":" << BuildReservationListJson_(*connector);
        oss << "}";

        return oss.str();
    }

    if (!allOk) {
        statusCode = 500;

        std::ostringstream oss;
        oss << "{";
        oss << "\"ok\":false,";
        oss << "\"changed\":false,";
        oss << "\"source\":\"remove_by_id\",";
        oss << "\"instance\":" << instance << ",";
        oss << "\"reason\":\"remove-task-and-apply-failed\",";
        oss << "\"items\":" << BuildReservationListJson_(*connector);
        oss << "}";

        return oss.str();
    }

    bool jsOk = true;
    if (!opt.outputDir.empty()) {
        jsOk = EpgHtmlPortal_ReservationData::WriteReservationDataJs(opt, *connector);
    }

    statusCode = jsOk ? 200 : 500;

    std::ostringstream oss;
    oss << "{";
    oss << "\"ok\":" << (jsOk ? "true" : "false") << ",";
    oss << "\"changed\":true,";
    oss << "\"source\":\"remove_by_id\",";
    oss << "\"instance\":" << instance << ",";
    oss << "\"reason\":\"" << (jsOk ? "" : "reservation-js-regenerate-failed") << "\",";
    oss << "\"items\":" << BuildReservationListJson_(*connector);
    oss << "}";
    return oss.str();
}

std::string EpgPortalReservationApiServer::HandleSetEnabledById_(
    const std::string& formBody, int& statusCode)
{
    TaskSystemConnector* connector = nullptr;
    EpgHtmlPortal::WriteOptions opt{};

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        connector = connector_;
        opt = writeOptions_;
    }

    if (connector == nullptr) {
        statusCode = 500;
        return R"({"ok":false,"reason":"connector-null"})";
    }

    std::lock_guard<std::mutex> apiLock(reservationApiMutex_);

    const auto form = ParseFormUrlEncoded_(formBody);

    const std::wstring id = GetFormValue_(form, L"id");
    const std::wstring mode = GetFormValue_(form, L"mode");
    const std::wstring dedupeKey = GetFormValue_(form, L"dedupeKey");
    const std::wstring excludeKey = GetFormValue_(form, L"excludeKey");

    const bool enabled = ParseBoolLoose_(GetFormValue_(form, L"enabled"), true);
    const bool disabledByUser = ParseBoolLoose_(GetFormValue_(form, L"disabledByUser"), !enabled);
    const bool excludeFromAuto = ParseBoolLoose_(GetFormValue_(form, L"excludeFromAuto"), !enabled);

    std::uint32_t kind = ParseUInt32LooseForApi_(GetFormValue_(form, L"kind"), 0);
    std::uint32_t instance = ParseUInt32LooseForApi_(GetFormValue_(form, L"instance"), 0);

    if ((kind == 0 || instance == 0) && !id.empty()) {
        std::uint32_t parsedKind = 0;
        std::uint32_t parsedInstance = 0;
        if (ParseTaskIdForApi_(id, parsedKind, parsedInstance)) {
            if (kind == 0) kind = parsedKind;
            if (instance == 0) instance = parsedInstance;
        }
    }

    if (instance == 0) {
        const std::wstring key = !dedupeKey.empty() ? dedupeKey : excludeKey;
        if (!key.empty()) {
            instance = HashFNV1a32ForApi_(key);
        }
    }

    if (instance == 0) {
        statusCode = 400;
        return R"({"ok":false,"reason":"invalid-id-or-instance","items":[]})";
    }

    const bool treatAsProgramReservation =
        IsRecordTaskKindForApi_(kind) ||
        IsProgramModeForApi_(mode) ||
        !dedupeKey.empty() ||
        !excludeKey.empty();

    bool changed = false;
    bool allOk = true;

    if (treatAsProgramReservation) {
        changed = connector->SetProgramReservationGroupEnabledAndApply(
            instance,
            enabled,
            disabledByUser,
            excludeFromAuto);

        allOk = changed;
    }
    else {
        const auto snap = connector->GetTaskListManager().GetSnapshot();

        for (const auto& t : snap) {
            if (t.request.kind != kind || t.request.instance != instance) {
                continue;
            }

            // 変更した複製をUpsertするための意図的な値コピー。
            std::remove_cvref_t<decltype(t)> updated(t);
            updated.enabled = enabled;
            updated.reserve.disabledByUser = disabledByUser;
            updated.reserve.excludeFromAuto = excludeFromAuto;

            allOk = connector->UpsertTaskAndApply(updated) && allOk;
            changed = true;
            break;
        }
    }

    if (!changed) {
        statusCode = 404;

        std::ostringstream oss;
        oss << "{";
        oss << "\"ok\":false,";
        oss << "\"changed\":false,";
        oss << "\"source\":\"remove_by_id\",";
        oss << "\"instance\":" << instance << ",";
        oss << "\"reason\":\"not-found\",";
        oss << "\"items\":" << BuildReservationListJson_(*connector);
        oss << "}";

        return oss.str();
    }

    if (!allOk) {
        statusCode = 500;

        std::ostringstream oss;
        oss << "{";
        oss << "\"ok\":false,";
        oss << "\"changed\":false,";
        oss << "\"source\":\"remove_by_id\",";
        oss << "\"instance\":" << instance << ",";
        oss << "\"reason\":\"remove-task-and-apply-failed\",";
        oss << "\"items\":" << BuildReservationListJson_(*connector);
        oss << "}";

        return oss.str();
    }

    bool jsOk = true;
    if (!opt.outputDir.empty()) {
        jsOk = EpgHtmlPortal_ReservationData::WriteReservationDataJs(opt, *connector);
    }

    statusCode = jsOk ? 200 : 500;

    std::ostringstream oss;
    oss << "{";
    oss << "\"ok\":" << (jsOk ? "true" : "false") << ",";
    oss << "\"changed\":true,";
    oss << "\"source\":\"set_enabled_by_id\",";
    oss << "\"instance\":" << instance << ",";
    oss << "\"reason\":\"" << (jsOk ? "" : "reservation-js-regenerate-failed") << "\",";
    oss << "\"items\":" << BuildReservationListJson_(*connector);
    oss << "}";
    return oss.str();
}

std::string EpgPortalReservationApiServer::HandleStaticFile_(
    const std::string& urlPath,
    int& statusCode,
    std::string& contentType)
{
    std::filesystem::path filePath;
    if (!ResolveStaticFilePath_(urlPath, filePath)) {
        statusCode = 404;
        contentType = "application/json; charset=utf-8";
        return R"({"ok":false,"reason":"static-file-not-found"})";
    }

    std::error_code sec;
    const auto fileSize = std::filesystem::file_size(filePath, sec);
    if (sec || fileSize > (128ull * 1024ull * 1024ull)) {
        statusCode = 500;
        contentType = "application/json; charset=utf-8";
        return R"({"ok":false,"reason":"static-file-size-invalid"})";
    }

    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs) {
        statusCode = 404;
        contentType = "application/json; charset=utf-8";
        return R"({"ok":false,"reason":"static-file-open-failed"})";
    }

    std::string body;

    try {
        body.assign(
            std::istreambuf_iterator<char>(ifs),
            std::istreambuf_iterator<char>());
    }
    catch (...) {
        statusCode = 500;
        contentType = "application/json; charset=utf-8";
        return R"({"ok":false,"reason":"static-file-read-exception"})";
    }

    if (ifs.bad()) {
        statusCode = 500;
        contentType = "application/json; charset=utf-8";
        return R"({"ok":false,"reason":"static-file-read-failed"})";
    }

    if (body.size() > (128ull * 1024ull * 1024ull)) {
        statusCode = 500;
        contentType = "application/json; charset=utf-8";
        return R"({"ok":false,"reason":"static-file-read-too-large"})";
    }

    statusCode = 200;
    contentType = GetMimeType_(filePath);
    return body;
}

bool EpgPortalReservationApiServer::ResolveStaticFilePath_(
    const std::string& urlPath,
    std::filesystem::path& outPath) const
{
    EpgHtmlPortal::WriteOptions opt{};
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        opt = writeOptions_;
    }

    const std::filesystem::path exeDir = GetExeDirectoryForPortalServer_();

    // HTML出力先は原則 opt.outputDir。
    // 未設定なら exeDir/epg_out にフォールバック。
    std::filesystem::path epgOutRoot;

    if (opt.outputDir.empty()) {
        epgOutRoot = exeDir / L"epg_out";
    }
    else if (opt.outputDir.is_relative()) {
        epgOutRoot = exeDir / opt.outputDir;
    }
    else {
        epgOutRoot = opt.outputDir;
    }

    std::error_code ec;
    if (!std::filesystem::exists(epgOutRoot, ec) || ec) {
        return false;
    }

    std::string pathNoQuery = StripQueryString_(urlPath);
    if (pathNoQuery.empty() || pathNoQuery == "/") {
        pathNoQuery = "/index.html";
    }

    std::wstring decoded = UrlDecodeToWide_(pathNoQuery);
    std::replace(decoded.begin(), decoded.end(), L'\\', L'/');

    while (!decoded.empty() && decoded.front() == L'/') {
        decoded.erase(decoded.begin());
    }

    if (decoded.empty()) {
        decoded = L"index.html";
    }

    // ルート選択:
    // cache は exeDir/cache。
    // それ以外は epg_out。
    std::filesystem::path staticRoot = epgOutRoot;

    {
        std::wstring probe = decoded;
        std::wstring lower = probe;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) {
            return static_cast<wchar_t>(std::towlower(c));
            });

        const std::wstring cachePrefix = L"cache/";
        if (lower.rfind(cachePrefix, 0) == 0) {
            staticRoot = exeDir / L"cache";
            decoded = probe.substr(cachePrefix.size());
        }
    }

    if (decoded.empty()) {
        decoded = L"index.html";
    }

    if (!std::filesystem::exists(staticRoot, ec) || ec) {
        return false;
    }

    std::filesystem::path rel;
    std::wstring part;

    auto flushPart = [&]() -> bool {
        if (part.empty() || part == L".") {
            part.clear();
            return true;
        }

        if (part == L"..") {
            return false;
        }

        if (part.find(L':') != std::wstring::npos) {
            return false;
        }

        rel /= part;
        part.clear();
        return true;
        };

    for (wchar_t ch : decoded) {
        if (ch == L'/') {
            if (!flushPart()) {
                return false;
            }
        }
        else {
            part.push_back(ch);
        }
    }

    if (!flushPart()) {
        return false;
    }

    if (rel.empty()) {
        rel = L"index.html";
    }

    std::filesystem::path target = staticRoot / rel;

    if (std::filesystem::is_directory(target, ec) && !ec) {
        target /= L"index.html";
    }

    if (!std::filesystem::exists(target, ec) || ec) {
        return false;
    }

    if (!std::filesystem::is_regular_file(target, ec) || ec) {
        return false;
    }

    if (!IsPathUnderRoot_(staticRoot, target)) {
        return false;
    }

    outPath = target;
    return true;
}

std::string EpgPortalReservationApiServer::GetMimeType_(const std::filesystem::path& path)
{
    auto toAsciiLower = [](wchar_t ch) noexcept -> wchar_t {
        if (ch >= L'A' && ch <= L'Z') {
            return static_cast<wchar_t>(ch - L'A' + L'a');
        }
        return ch;
        };

    std::wstring ext = path.extension().wstring();

    std::transform(
        ext.begin(),
        ext.end(),
        ext.begin(),
        toAsciiLower);

    struct MimeEntry {
        std::wstring_view ext;
        const char* mime;
    };

    static constexpr MimeEntry kMimeTable[] = {
        { L".html", "text/html; charset=utf-8" },
        { L".htm",  "text/html; charset=utf-8" },

        { L".js",   "text/javascript; charset=utf-8" },
        { L".mjs",  "text/javascript; charset=utf-8" },
        { L".css",  "text/css; charset=utf-8" },

        { L".json", "application/json; charset=utf-8" },
        { L".map",  "application/json; charset=utf-8" },

        { L".tsv",  "text/tab-separated-values; charset=utf-8" },
        { L".csv",  "text/csv; charset=utf-8" },
        { L".txt",  "text/plain; charset=utf-8" },
        { L".log",  "text/plain; charset=utf-8" },

        { L".svg",  "image/svg+xml" },
        { L".png",  "image/png" },
        { L".jpg",  "image/jpeg" },
        { L".jpeg", "image/jpeg" },
        { L".gif",  "image/gif" },
        { L".webp", "image/webp" },
        { L".ico",  "image/x-icon" },

        { L".woff",  "font/woff" },
        { L".woff2", "font/woff2" },

        { L".wasm", "application/wasm" }
    };

    for (const auto& item : kMimeTable) {
        if (ext == item.ext) {
            return item.mime;
        }
    }

    return "application/octet-stream";
}

std::wstring EpgPortalReservationApiServer::UrlDecodeToWide_(const std::string& s)
{
    std::string bytes;
    bytes.reserve(s.size());

    for (std::size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (ch == '+') {
            bytes.push_back(' ');
        }
        else if (ch == '%' && i + 2 < s.size()) {
            auto hexv = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return -1;
                };
            const int hi = hexv(s[i + 1]);
            const int lo = hexv(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                bytes.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            }
            else {
                bytes.push_back(ch);
            }
        }
        else {
            bytes.push_back(ch);
        }
    }

    return FromUtf8(bytes);
}

std::string EpgPortalReservationApiServer::JsonEscapeUtf8_(const std::wstring& s)
{
    return JsonEscapeUtf8Local_(s);
}

std::string EpgPortalReservationApiServer::BuildJsonResult_(
    const EpgHtmlPortal_ReservationData::ReservationMutationResult& r,
    const TaskSystemConnector* connector)
{
    std::ostringstream oss;
    oss << "{";
    oss << "\"ok\":" << (r.ok ? "true" : "false") << ",";
    oss << "\"changed\":" << (r.changed ? "true" : "false") << ",";
    oss << "\"instance\":" << r.instance << ",";
    oss << "\"dedupeKey\":\"" << JsonEscapeUtf8_(r.dedupeKey) << "\",";
    oss << "\"reason\":\"" << JsonEscapeUtf8_(r.reason) << "\"";

    if (connector != nullptr) {
        oss << ",\"items\":" << BuildReservationListJson_(*connector);
    }

    oss << "}";
    return oss.str();
}

std::wstring EpgPortalReservationApiServer::GetFormValue_(
    const std::map<std::wstring, std::wstring>& form,
    const wchar_t* key)
{
    const auto it = form.find(std::wstring(key));
    if (it == form.end()) return {};
    return it->second;
}

bool EpgPortalReservationApiServer::ParseBoolLoose_(
    const std::wstring& s, bool defValue)
{
    std::wstring v = s;
    std::transform(v.begin(), v.end(), v.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });

    if (v == L"1" || v == L"true" || v == L"yes" || v == L"on") return true;
    if (v == L"0" || v == L"false" || v == L"no" || v == L"off") return false;
    return defValue;
}

int EpgPortalReservationApiServer::ParseIntLoose_(
    const std::wstring& s, int defValue)
{
    const wchar_t* p = s.c_str();

    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') {
        ++p;
    }

    bool negative = false;
    if (*p == L'+') {
        ++p;
    }
    else if (*p == L'-') {
        negative = true;
        ++p;
    }

    if (*p < L'0' || *p > L'9') {
        return defValue;
    }

    std::int64_t value = 0;
    const std::int64_t limit = negative
        ? static_cast<std::int64_t>(std::numeric_limits<int>::max()) + 1LL
        : static_cast<std::int64_t>(std::numeric_limits<int>::max());

    while (*p >= L'0' && *p <= L'9') {
        value = value * 10 + static_cast<std::int64_t>(*p - L'0');
        if (value > limit) {
            return defValue;
        }
        ++p;
    }

    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') {
        ++p;
    }

    if (*p != L'\0') {
        return defValue;
    }

    if (negative) {
        if (value == static_cast<std::int64_t>(std::numeric_limits<int>::max()) + 1LL) {
            return std::numeric_limits<int>::min();
        }
        return -static_cast<int>(value);
    }

    return static_cast<int>(value);
}

std::map<std::wstring, std::wstring> EpgPortalReservationApiServer::ParseFormUrlEncoded_(
    const std::string& body)
{
    std::map<std::wstring, std::wstring> out;

    std::size_t pos = 0;
    while (pos < body.size()) {
        const std::size_t amp = body.find('&', pos);
        const std::string pair = body.substr(
            pos,
            (amp == std::string::npos ? body.size() : amp) - pos);

        const std::size_t eq = pair.find('=');
        const std::string k = (eq == std::string::npos) ? pair : pair.substr(0, eq);
        const std::string v = (eq == std::string::npos) ? std::string() : pair.substr(eq + 1);

        out.emplace(UrlDecodeToWide_(k), UrlDecodeToWide_(v));

        if (amp == std::string::npos) break;
        pos = amp + 1;
    }

    return out;
}

EpgHtmlPortal_ReservationData::ProgramReservationPayload
EpgPortalReservationApiServer::BuildPayload_(
    const std::map<std::wstring, std::wstring>& form) const
{
    using Payload = EpgHtmlPortal_ReservationData::ProgramReservationPayload;
    Payload p{};

    p.space = ParseIntLoose_(GetFormValue_(form, L"space"), 0);
    p.networkId = static_cast<std::uint16_t>((std::max)(0, ParseIntLoose_(GetFormValue_(form, L"networkId"), 0)));
    p.tsId = static_cast<std::uint16_t>((std::max)(0, ParseIntLoose_(GetFormValue_(form, L"tsId"), 0)));
    p.serviceId = static_cast<std::uint16_t>((std::max)(0, ParseIntLoose_(GetFormValue_(form, L"serviceId"), 0)));
    p.eventId = static_cast<std::uint16_t>((std::max)(0, ParseIntLoose_(GetFormValue_(form, L"eventId"), 0)));

    p.title = GetFormValue_(form, L"title");
    p.titleLabel = GetFormValue_(form, L"titleLabel");
    p.serviceName = GetFormValue_(form, L"serviceName");
    if (p.serviceName.empty()) p.serviceName = GetFormValue_(form, L"service");
    p.summary = GetFormValue_(form, L"summary");
    p.genre = GetFormValue_(form, L"genre");

    p.dateText = GetFormValue_(form, L"dateText");
    if (p.dateText.empty()) p.dateText = GetFormValue_(form, L"date");
    p.weekdayText = GetFormValue_(form, L"weekdayText");
    p.timeText = GetFormValue_(form, L"timeText");
    if (p.timeText.empty()) p.timeText = GetFormValue_(form, L"time");

    p.startDate = GetFormValue_(form, L"startDate");
    p.startTime = GetFormValue_(form, L"startTime");
    p.endDate = GetFormValue_(form, L"endDate");
    p.endTime = GetFormValue_(form, L"endTime");
    p.durationSec = static_cast<std::uint32_t>((std::max)(0, ParseIntLoose_(GetFormValue_(form, L"durationSec"), 0)));

    p.reserveMode = GetFormValue_(form, L"reserveMode");
    p.reserveSource = GetFormValue_(form, L"reserveSource");
    p.dedupeKey = GetFormValue_(form, L"dedupeKey");
    p.excludeKey = GetFormValue_(form, L"excludeKey");

    p.enabled = ParseBoolLoose_(GetFormValue_(form, L"enabled"), true);
    p.disabledByUser = ParseBoolLoose_(GetFormValue_(form, L"disabledByUser"), false);
    p.excludeFromAuto = ParseBoolLoose_(GetFormValue_(form, L"excludeFromAuto"), false);
    p.autoReserved = ParseBoolLoose_(GetFormValue_(form, L"autoReserved"), false);

    p.requiredTuners = static_cast<std::uint8_t>((std::max)(1, ParseIntLoose_(GetFormValue_(form, L"requiredTuners"), 1)));
    p.preferredTunerIndex = ParseIntLoose_(GetFormValue_(form, L"preferredTunerIndex"), -1);

    return p;
}

std::string EpgPortalReservationApiServer::HandleList_(int& statusCode)
{
    TaskSystemConnector* connector = nullptr;

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        connector = connector_;
    }

    if (connector == nullptr) {
        statusCode = 500;
        return R"({"ok":false,"reason":"connector-null","items":[]})";
    }

    std::lock_guard<std::mutex> apiLock(reservationApiMutex_);

    statusCode = 200;
    std::ostringstream oss;
    oss << "{";
    oss << "\"ok\":true,";
    oss << "\"items\":" << BuildReservationListJson_(*connector);
    oss << "}";
    return oss.str();
}

std::string EpgPortalReservationApiServer::BuildReservationListJson_(
    const TaskSystemConnector& connector)
{
    const auto rows = connector.GetTaskViewSnapshot();

    std::ostringstream oss;
    oss << "[";

    std::unordered_set<std::wstring> seen;
    bool first = true;

    for (const auto& row : rows) {
        if (row.reserveMode.empty()) {
            // EPG取得予約も予約一覧に出したい場合はここを外す。
            // 動的番組予約APIとしては reserveMode のある行だけ返す。
            continue;
        }

        const std::wstring uniqueKey = MakeReservationUniqueKey_(row);
        if (!seen.emplace(uniqueKey).second) {
            continue;
        }

        if (!first) {
            oss << ",";
        }
        first = false;
        oss << BuildTaskViewRowJsonLocal_(row);
    }

    oss << "]";
    return oss.str();
}
