#include "AutoReserveRuleStore.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <cstdio>
#include <limits>
#include <string_view>
#include <chrono>
#include <exception>
#include <new>


#include "DebugManager.h"
#include "TaskSystemConnector.hpp"
#include "EpgHtmlPortal.hpp"
#include "EpgHtmlPortal_SearchData.hpp"
#include "EpgHtmlPortal_ReservationData.hpp"

namespace AutoReserve
{
    namespace
    {
        constexpr const wchar_t* kRootSection =
            L"AutoReserveRules";

        constexpr const wchar_t* kRuleSectionPrefix =
            L"AutoReserveRule_";



        static std::wstring MakeRuleSectionName_(
            std::uint32_t index)
        {
            return std::wstring(kRuleSectionPrefix) +
                std::to_wstring(index);
        }

        static std::wstring Trim_(
            std::wstring s)
        {
            const auto isWs = [](wchar_t ch) noexcept {
                return ch == L' ' ||
                    ch == L'\t' ||
                    ch == L'\r' ||
                    ch == L'\n' ||
                    ch == L'\u3000';
                };

            while (!s.empty() && isWs(s.front())) {
                s.erase(s.begin());
            }

            while (!s.empty() && isWs(s.back())) {
                s.pop_back();
            }

            return s;
        }

        static std::wstring ToUpperAscii_(
            std::wstring s)
        {
            for (wchar_t& ch : s) {
                if (ch >= L'a' && ch <= L'z') {
                    ch = static_cast<wchar_t>(ch - L'a' + L'A');
                }
            }

            return s;
        }

        static std::wstring SanitizeIniValue_(
            std::wstring s)
        {
            for (wchar_t& ch : s) {
                if (ch == L'\r' || ch == L'\n') {
                    ch = L' ';
                }
            }

            return Trim_(std::move(s));
        }

        static std::wstring ReadIniString_(
            const std::wstring& iniPath,
            const std::wstring& section,
            const std::wstring& key,
            const std::wstring& defValue)
        {
            wchar_t buf[4096]{};

            ::GetPrivateProfileStringW(
                section.c_str(),
                key.c_str(),
                defValue.c_str(),
                buf,
                static_cast<DWORD>(_countof(buf)),
                iniPath.c_str());

            return std::wstring(buf);
        }

        static int ReadIniInt_(
            const std::wstring& iniPath,
            const std::wstring& section,
            const std::wstring& key,
            int defValue)
        {
            return static_cast<int>(
                ::GetPrivateProfileIntW(
                    section.c_str(),
                    key.c_str(),
                    defValue,
                    iniPath.c_str()));
        }

        static bool ReadIniBool_(
            const std::wstring& iniPath,
            const std::wstring& section,
            const std::wstring& key,
            bool defValue)
        {
            const int v =
                ReadIniInt_(
                    iniPath,
                    section,
                    key,
                    defValue ? 1 : 0);

            return v != 0;
        }

        static bool WriteIniString_(
            const std::wstring& iniPath,
            const std::wstring& section,
            const std::wstring& key,
            const std::wstring& value)
        {
            return ::WritePrivateProfileStringW(
                section.c_str(),
                key.c_str(),
                value.c_str(),
                iniPath.c_str()) != FALSE;
        }

        static bool WriteIniInt_(
            const std::wstring& iniPath,
            const std::wstring& section,
            const std::wstring& key,
            int value)
        {
            return WriteIniString_(
                iniPath,
                section,
                key,
                std::to_wstring(value));
        }

        static bool WriteIniUInt_(
            const std::wstring& iniPath,
            const std::wstring& section,
            const std::wstring& key,
            std::uint32_t value)
        {
            return WriteIniString_(
                iniPath,
                section,
                key,
                std::to_wstring(value));
        }

        static bool WriteIniBool_(
            const std::wstring& iniPath,
            const std::wstring& section,
            const std::wstring& key,
            bool value)
        {
            return WriteIniInt_(
                iniPath,
                section,
                key,
                value ? 1 : 0);
        }

        static void DeleteIniSection_(
            const std::wstring& iniPath,
            const std::wstring& section)
        {
            ::WritePrivateProfileStringW(
                section.c_str(),
                nullptr,
                nullptr,
                iniPath.c_str());
        }

        static std::wstring GetExeDir_()
        {
            std::wstring path;
            path.resize(32768);

            const DWORD len =
                ::GetModuleFileNameW(
                    nullptr,
                    path.data(),
                    static_cast<DWORD>(path.size()));

            if (len == 0 || len >= path.size()) {
                return L".";
            }

            path.resize(static_cast<std::size_t>(len));

            return std::filesystem::path(path).parent_path().wstring();
        }

        static bool EnsureParentDirectory_(
            const std::wstring& iniPath)
        {
            try {
                const std::filesystem::path p(iniPath);
                const std::filesystem::path parent =
                    p.parent_path();

                if (parent.empty()) {
                    return true;
                }

                std::error_code ec;
                std::filesystem::create_directories(parent, ec);
                return !ec;
            }
            catch (...) {
                return false;
            }
        }

        static bool FileExistsNoThrow_(
            const std::wstring& path)
        {
            std::error_code ec;
            return std::filesystem::exists(std::filesystem::path(path), ec) && !ec;
        }

        static long long ElapsedMs_(
            const std::chrono::steady_clock::time_point& begin)
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin).count();
        }

        static bool GetFileInfoNoThrow_(
            const std::wstring& path,
            std::uintmax_t& outSize)
        {
            outSize = 0;

            if (path.empty()) {
                return false;
            }

            std::error_code ec;
            const std::filesystem::path p(path);

            if (!std::filesystem::exists(p, ec) || ec) {
                return false;
            }

            ec.clear();
            outSize = std::filesystem::file_size(p, ec);
            if (ec) {
                outSize = 0;
            }

            return true;
        }

        struct FileStamp_
        {
            bool exists = false;
            std::uintmax_t size = 0;
            ULONGLONG writeTime = 0;
        };

        static FileStamp_ GetFileStampNoThrow_(
            const std::wstring& path)
        {
            FileStamp_ stamp{};

            if (path.empty()) {
                return stamp;
            }

            WIN32_FILE_ATTRIBUTE_DATA data{};

            if (::GetFileAttributesExW(
                path.c_str(),
                GetFileExInfoStandard,
                &data) == FALSE) {
                return stamp;
            }

            ULARGE_INTEGER size{};
            size.HighPart = data.nFileSizeHigh;
            size.LowPart = data.nFileSizeLow;

            ULARGE_INTEGER writeTime{};
            writeTime.HighPart = data.ftLastWriteTime.dwHighDateTime;
            writeTime.LowPart = data.ftLastWriteTime.dwLowDateTime;

            stamp.exists = true;
            stamp.size = static_cast<std::uintmax_t>(size.QuadPart);
            stamp.writeTime = writeTime.QuadPart;
            return stamp;
        }

        static bool EnsureIniFileUnicode_(
            const std::wstring& iniPath)
        {
            if (iniPath.empty()) {
                return false;
            }

            if (FileExistsNoThrow_(iniPath)) {
                return true;
            }

            HANDLE h =
                ::CreateFileW(
                    iniPath.c_str(),
                    GENERIC_WRITE,
                    0,
                    nullptr,
                    CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);

            if (h == INVALID_HANDLE_VALUE) {
                return ::GetLastError() == ERROR_FILE_EXISTS;
            }

            const unsigned char bom[2] = { 0xFF, 0xFE };
            DWORD written = 0;
            const BOOL ok =
                ::WriteFile(
                    h,
                    bom,
                    static_cast<DWORD>(sizeof(bom)),
                    &written,
                    nullptr);

            ::CloseHandle(h);

            return ok != FALSE && written == sizeof(bom);
        }

        static std::wstring StripUtf16Bom_(
            std::wstring text)
        {
            if (!text.empty() && text.front() == L'\ufeff') {
                text.erase(text.begin());
            }

            return text;
        }

        static bool ContainsIniSection_(
            const std::wstring& text,
            const std::wstring& targetSection)
        {
            const std::wstring target =
                ToUpperAscii_(Trim_(targetSection));

            std::size_t pos = 0;

            while (pos < text.size()) {
                std::size_t end = text.find(L'\n', pos);
                if (end == std::wstring::npos) {
                    end = text.size();
                }

                std::wstring line =
                    text.substr(pos, end - pos);

                if (!line.empty() && line.back() == L'\r') {
                    line.pop_back();
                }

                line = Trim_(std::move(line));

                if (line.size() >= 2 &&
                    line.front() == L'[' &&
                    line.back() == L']') {
                    std::wstring section =
                        Trim_(line.substr(1, line.size() - 2));

                    section = ToUpperAscii_(std::move(section));

                    if (section == target) {
                        return true;
                    }
                }

                pos = (end < text.size()) ? (end + 1) : text.size();
            }

            return false;
        }

        static bool WriteUtf16LeTextFile_(
            const std::wstring& path,
            const std::wstring& text)
        {
            if (path.empty()) {
                return false;
            }

            if (!EnsureParentDirectory_(path)) {
                return false;
            }

            HANDLE h =
                ::CreateFileW(
                    path.c_str(),
                    GENERIC_WRITE,
                    0,
                    nullptr,
                    CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);

            if (h == INVALID_HANDLE_VALUE) {
                return false;
            }

            bool ok = true;

            const unsigned char bom[2] = { 0xFF, 0xFE };
            DWORD written = 0;

            if (::WriteFile(
                h,
                bom,
                static_cast<DWORD>(sizeof(bom)),
                &written,
                nullptr) == FALSE ||
                written != sizeof(bom)) {
                ok = false;
            }

            if (ok && !text.empty()) {
                const std::size_t bytes =
                    text.size() * sizeof(wchar_t);

                if (bytes >
                    static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())) {
                    ok = false;
                }
                else {
                    written = 0;

                    if (::WriteFile(
                        h,
                        text.data(),
                        static_cast<DWORD>(bytes),
                        &written,
                        nullptr) == FALSE ||
                        written != static_cast<DWORD>(bytes)) {
                        ok = false;
                    }
                }
            }

            ::CloseHandle(h);

            return ok;
        }

        static void DeleteFileNoThrow_(
            const std::wstring& path)
        {
            if (!path.empty()) {
                ::DeleteFileW(path.c_str());
            }
        }

        static std::wstring Utf8ToWide_(
            const std::string& text)
        {
            if (text.empty()) {
                return {};
            }

            if (text.size() >
                static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
                return {};
            }

            const int srcLen =
                static_cast<int>(text.size());

            int wideLen =
                ::MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    text.data(),
                    srcLen,
                    nullptr,
                    0);

            if (wideLen <= 0) {
                wideLen =
                    ::MultiByteToWideChar(
                        CP_UTF8,
                        0,
                        text.data(),
                        srcLen,
                        nullptr,
                        0);
            }

            if (wideLen <= 0) {
                return {};
            }

            std::wstring out;
            out.resize(static_cast<std::size_t>(wideLen));

            const int written =
                ::MultiByteToWideChar(
                    CP_UTF8,
                    0,
                    text.data(),
                    srcLen,
                    out.data(),
                    wideLen);

            if (written <= 0) {
                return {};
            }

            out.resize(static_cast<std::size_t>(written));
            return out;
        }

        static std::string WideToUtf8_(
            const std::wstring& text)
        {
            if (text.empty()) {
                return {};
            }

            if (text.size() >
                static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
                return {};
            }

            const int srcLen =
                static_cast<int>(text.size());

            const int utf8Len =
                ::WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    text.data(),
                    srcLen,
                    nullptr,
                    0,
                    nullptr,
                    nullptr);

            if (utf8Len <= 0) {
                return {};
            }

            std::string out;
            out.resize(static_cast<std::size_t>(utf8Len));

            const int written =
                ::WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    text.data(),
                    srcLen,
                    out.data(),
                    utf8Len,
                    nullptr,
                    nullptr);

            if (written <= 0) {
                return {};
            }

            out.resize(static_cast<std::size_t>(written));
            return out;
        }

        static int ClampDurationMinutes_(int value) noexcept;
        static int ClampSpace_(int value) noexcept;
        static int ClampSeconds_(int value) noexcept;
        static int ClampPriority_(int value) noexcept;
        static std::wstring NormalizeWeekdays_(
            std::wstring value);

        static bool WriteAllBytes_(
            HANDLE file,
            const void* data,
            std::size_t size)
        {
            const auto* p =
                static_cast<const unsigned char*>(data);

            while (size > 0) {
                const DWORD chunk =
                    static_cast<DWORD>(
                        (std::min)(
                            size,
                            static_cast<std::size_t>(
                                (std::numeric_limits<DWORD>::max)())));

                DWORD written = 0;

                if (::WriteFile(
                    file,
                    p,
                    chunk,
                    &written,
                    nullptr) == FALSE ||
                    written == 0) {
                    return false;
                }

                p += written;
                size -= written;
            }

            return true;
        }

        static bool ReadUtf8CacheFile_(
            const std::wstring& path,
            std::string& outText,
            std::wstring& outMessage)
        {
            outText.clear();
            outMessage.clear();

            const FileStamp_ stamp =
                GetFileStampNoThrow_(path);

            if (!stamp.exists) {
                outMessage = L"cache file not found";
                return false;
            }

            constexpr std::uintmax_t kMaxCacheBytes =
                16ULL * 1024ULL * 1024ULL;

            if (stamp.size < 3 || stamp.size > kMaxCacheBytes) {
                outMessage = L"cache file size is invalid";
                return false;
            }

            HANDLE file =
                ::CreateFileW(
                    path.c_str(),
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);

            if (file == INVALID_HANDLE_VALUE) {
                outMessage = L"cache file open failed";
                return false;
            }

            std::string bytes;
            bytes.resize(static_cast<std::size_t>(stamp.size));

            std::size_t offset = 0;
            bool ok = true;

            while (offset < bytes.size()) {
                const DWORD chunk =
                    static_cast<DWORD>(
                        (std::min)(
                            bytes.size() - offset,
                            static_cast<std::size_t>(
                                (std::numeric_limits<DWORD>::max)())));

                DWORD read = 0;

                if (::ReadFile(
                    file,
                    bytes.data() + offset,
                    chunk,
                    &read,
                    nullptr) == FALSE ||
                    read == 0) {
                    ok = false;
                    break;
                }

                offset += read;
            }

            ::CloseHandle(file);

            if (!ok) {
                outMessage = L"cache file read failed";
                return false;
            }

            bytes.resize(offset);

            if (bytes.size() >= 3 &&
                static_cast<unsigned char>(bytes[0]) == 0xEF &&
                static_cast<unsigned char>(bytes[1]) == 0xBB &&
                static_cast<unsigned char>(bytes[2]) == 0xBF) {
                bytes.erase(0, 3);
            }

            const std::wstring wide =
                Utf8ToWide_(bytes);

            if (wide.empty() ||
                !ContainsIniSection_(
                    wide,
                    kRootSection)) {
                outMessage = L"cache content is invalid";
                return false;
            }

            outText = std::move(bytes);
            outMessage = L"loaded";
            return true;
        }

        static std::size_t ParseRuleCountFromCacheText_(
            const std::string& cacheText)
        {
            try {
                const std::wstring text =
                    Utf8ToWide_(cacheText);

                const std::wstring root =
                    L"[AutoReserveRules]";

                const std::size_t section =
                    text.find(root);

                if (section == std::wstring::npos) {
                    return 0;
                }

                std::size_t line =
                    text.find(L'\n', section + root.size());

                while (line != std::wstring::npos &&
                    line + 1 < text.size()) {
                    const std::size_t begin =
                        line + 1;

                    std::size_t end =
                        text.find(L'\n', begin);

                    if (end == std::wstring::npos) {
                        end = text.size();
                    }

                    std::wstring value =
                        Trim_(
                            text.substr(
                                begin,
                                end - begin));

                    if (!value.empty() &&
                        value.front() == L'[') {
                        break;
                    }

                    constexpr const wchar_t* kCountPrefix =
                        L"Count=";

                    if (value.rfind(kCountPrefix, 0) == 0) {
                        const unsigned long long count =
                            std::stoull(
                                value.substr(
                                    std::wcslen(
                                        kCountPrefix)));

                        return static_cast<std::size_t>(
                            (std::min)(
                                count,
                                static_cast<unsigned long long>(
                                    (std::numeric_limits<std::size_t>::max)())));
                    }

                    line =
                        end < text.size()
                        ? end
                        : std::wstring::npos;
                }
            }
            catch (...) {
            }

            return 0;
        }

        static std::wstring BuildRulesCacheText_(
            const RuleStore& store)
        {
            std::wstring text;
            text.reserve(
                256 +
                store.GetRules().size() * 1024);

            const auto append =
                [&text](
                    const wchar_t* key,
                    const std::wstring& value)
                {
                    text += key;
                    text += L'=';
                    text += value;
                    text += L'\n';
                };

            const auto appendInt =
                [&append](
                    const wchar_t* key,
                    long long value)
                {
                    append(
                        key,
                        std::to_wstring(value));
                };

            const auto appendBool =
                [&appendInt](
                    const wchar_t* key,
                    bool value)
                {
                    appendInt(
                        key,
                        value ? 1 : 0);
                };

            text += L"; AutoReserveRulesCache v1\n";
            text += L"; Generated from auto_reserve_rules.ini. Do not edit.\n\n";
            text += L"[AutoReserveRules]\n";
            appendInt(
                L"Count",
                static_cast<long long>(
                    store.GetRules().size()));
            appendInt(
                L"NextId",
                static_cast<long long>(
                    store.GetNextId()));
            text += L'\n';

            const auto& rules =
                store.GetRules();

            for (std::size_t i = 0; i < rules.size(); ++i) {
                const Rule& r = rules[i];
                const bool useTimeRange =
                    r.useSearchTimeRange;

                text += L"[AutoReserveRule_";
                text += std::to_wstring(i);
                text += L"]\n";

                appendInt(L"Id", r.id);
                appendBool(L"Enabled", r.enabled);
                append(L"Name", SanitizeIniValue_(r.name));
                appendBool(L"UseKeyword", r.useKeyword);
                append(L"Keyword", SanitizeIniValue_(r.keyword));
                append(
                    L"KeywordJoinMode",
                    TokenJoinModeToString(r.keywordJoinMode));
                appendBool(
                    L"UseExcludeKeyword",
                    r.useExcludeKeyword);
                append(
                    L"ExcludeKeyword",
                    SanitizeIniValue_(r.excludeKeyword));
                append(
                    L"ExcludeKeywordJoinMode",
                    TokenJoinModeToString(
                        r.excludeKeywordJoinMode));
                appendInt(
                    L"TitleMatchMode",
                    static_cast<int>(r.titleMatchMode));
                appendBool(L"MatchTitle", r.matchTitle);
                appendBool(L"MatchSummary", r.matchSummary);
                appendBool(L"MatchExtended", r.matchExtended);
                appendInt(
                    L"TargetSpace",
                    ClampSpace_(r.targetSpace));
                appendBool(L"UseService", r.useService);
                append(
                    L"ServiceName",
                    SanitizeIniValue_(r.serviceName));
                append(
                    L"ServiceMatchMode",
                    TokenJoinModeToString(r.serviceMatchMode));
                appendBool(L"UseGenre", r.useGenre);
                append(
                    L"GenreName",
                    SanitizeIniValue_(r.genreName));
                append(
                    L"GenreMatchMode",
                    TokenJoinModeToString(r.genreMatchMode));
                appendBool(
                    L"UseMinDuration",
                    r.useMinDuration);
                appendInt(
                    L"MinDurationMinutes",
                    ClampDurationMinutes_(
                        r.minDurationMinutes));
                appendBool(
                    L"UseSearchTimeRange",
                    useTimeRange);
                appendInt(
                    L"SearchStartMinutes",
                    ClampDurationMinutes_(
                        r.searchStartMinutes));
                appendInt(
                    L"SearchEndMinutes",
                    ClampDurationMinutes_(
                        r.searchEndMinutes));
                appendBool(
                    L"SearchTimeWrap",
                    r.searchTimeWrap);
                appendInt(
                    L"StartMinutes",
                    useTimeRange
                    ? ClampDurationMinutes_(
                        r.searchStartMinutes)
                    : -1);
                appendInt(
                    L"EndMinutes",
                    useTimeRange
                    ? ClampDurationMinutes_(
                        r.searchEndMinutes)
                    : -1);
                appendBool(L"UseWeekday", r.useWeekday);
                append(
                    L"Weekdays",
                    NormalizeWeekdays_(r.weekdays));
                appendInt(
                    L"Priority",
                    ClampPriority_(r.priority));
                appendInt(
                    L"PreMarginSeconds",
                    ClampSeconds_(r.preMarginSeconds));
                appendInt(
                    L"PostMarginSeconds",
                    ClampSeconds_(r.postMarginSeconds));
                appendBool(
                    L"CreateReservationEnabled",
                    r.createReservationEnabled);
                appendInt(
                    L"DuplicatePolicy",
                    static_cast<int>(r.duplicatePolicy));
                appendBool(
                    L"SkipDuplicate",
                    r.skipDuplicate);
                appendBool(
                    L"ExcludeRerunLikeProgram",
                    r.excludeRerunLikeProgram);
                append(
                    L"ActionMode",
                    ReserveActionModeToString(r.actionMode));
                text += L'\n';
            }

            return text;
        }

        static bool WriteRulesCacheAtomic_(
            const std::wstring& cachePath,
            const std::string& cacheTextUtf8,
            std::wstring& outMessage)
        {
            outMessage.clear();

            if (cachePath.empty()) {
                outMessage = L"cache path is empty";
                return false;
            }

            if (!EnsureParentDirectory_(cachePath)) {
                outMessage = L"cache directory create failed";
                return false;
            }

            HANDLE mutexHandle =
                ::CreateMutexW(
                    nullptr,
                    FALSE,
                    L"Local\\TSCEPG_AutoReserveRulesCache");

            if (!mutexHandle) {
                outMessage = L"cache mutex create failed";
                return false;
            }

            const DWORD wait =
                ::WaitForSingleObject(
                    mutexHandle,
                    3000);

            if (wait != WAIT_OBJECT_0 &&
                wait != WAIT_ABANDONED) {
                ::CloseHandle(mutexHandle);
                outMessage = L"cache mutex wait failed";
                return false;
            }

            const std::wstring writingPath =
                cachePath + L".writing";

            ::DeleteFileW(writingPath.c_str());

            HANDLE file =
                ::CreateFileW(
                    writingPath.c_str(),
                    GENERIC_WRITE,
                    0,
                    nullptr,
                    CREATE_ALWAYS,
                    FILE_ATTRIBUTE_TEMPORARY |
                    FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
                    nullptr);

            bool ok =
                file != INVALID_HANDLE_VALUE;

            if (ok) {
                const unsigned char bom[3] = {
                    0xEF,
                    0xBB,
                    0xBF
                };

                ok =
                    WriteAllBytes_(
                        file,
                        bom,
                        sizeof(bom)) &&
                    WriteAllBytes_(
                        file,
                        cacheTextUtf8.data(),
                        cacheTextUtf8.size()) &&
                    ::FlushFileBuffers(file) != FALSE;

                ::CloseHandle(file);
                file = INVALID_HANDLE_VALUE;
            }

            if (ok) {
                constexpr DWORD flags =
                    MOVEFILE_REPLACE_EXISTING |
                    MOVEFILE_WRITE_THROUGH;

                ok = false;

                for (int retry = 0; retry < 10; ++retry) {
                    if (::MoveFileExW(
                        writingPath.c_str(),
                        cachePath.c_str(),
                        flags) != FALSE) {
                        ok = true;
                        break;
                    }

                    const DWORD error =
                        ::GetLastError();

                    if (error != ERROR_SHARING_VIOLATION &&
                        error != ERROR_ACCESS_DENIED &&
                        error != ERROR_LOCK_VIOLATION) {
                        break;
                    }

                    ::Sleep(10);
                }
            }

            if (!ok) {
                ::DeleteFileW(writingPath.c_str());
            }

            ::ReleaseMutex(mutexHandle);
            ::CloseHandle(mutexHandle);

            outMessage =
                ok
                ? L"cache written"
                : L"cache atomic write failed";

            return ok;
        }

        static bool PathsReferToSameFileNoThrow_(
            const std::wstring& lhs,
            const std::wstring& rhs)
        {
            if (lhs.empty() || rhs.empty()) {
                return false;
            }

            try {
                std::error_code ec;

                if (std::filesystem::equivalent(
                    std::filesystem::path(lhs),
                    std::filesystem::path(rhs),
                    ec) &&
                    !ec) {
                    return true;
                }

                ec.clear();

                const std::filesystem::path left =
                    std::filesystem::absolute(
                        std::filesystem::path(lhs),
                        ec).lexically_normal();

                if (ec) {
                    return false;
                }

                ec.clear();

                const std::filesystem::path right =
                    std::filesystem::absolute(
                        std::filesystem::path(rhs),
                        ec).lexically_normal();

                if (ec) {
                    return false;
                }

                return ::_wcsicmp(
                    left.c_str(),
                    right.c_str()) == 0;
            }
            catch (...) {
                return false;
            }
        }

        static std::string EscapeJsonUtf8_(
            const std::string& text)
        {
            std::string out;
            out.reserve(text.size() + 16);

            for (unsigned char ch : text) {
                switch (ch) {
                case '\\':
                    out += "\\\\";
                    break;
                case '"':
                    out += "\\\"";
                    break;
                case '\b':
                    out += "\\b";
                    break;
                case '\f':
                    out += "\\f";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (ch < 0x20) {
                        char buf[8]{};
                        sprintf_s(
                            buf,
                            "\\u%04X",
                            static_cast<unsigned int>(ch));
                        out += buf;
                    }
                    else {
                        out.push_back(static_cast<char>(ch));
                    }
                    break;
                }
            }

            return out;
        }

        static constexpr std::size_t kMaxIncomingAutoReserveFormBytes_ =
            4u * 1024u * 1024u;

        static int HexDigit_(
            char ch) noexcept
        {
            if (ch >= '0' && ch <= '9') {
                return ch - '0';
            }

            if (ch >= 'a' && ch <= 'f') {
                return ch - 'a' + 10;
            }

            if (ch >= 'A' && ch <= 'F') {
                return ch - 'A' + 10;
            }

            return -1;
        }

        static std::string UrlDecodeFormComponentUtf8_(
            std::string_view text)
        {
            std::string out;
            out.reserve(text.size());

            for (std::size_t i = 0; i < text.size(); ++i) {
                const char ch = text[i];

                if (ch == '+') {
                    out.push_back(' ');
                    continue;
                }

                if (ch == '%' && i + 2 < text.size()) {
                    const int hi = HexDigit_(text[i + 1]);
                    const int lo = HexDigit_(text[i + 2]);

                    if (hi >= 0 && lo >= 0) {
                        out.push_back(
                            static_cast<char>((hi << 4) | lo));
                        i += 2;
                        continue;
                    }
                }

                out.push_back(ch);
            }

            return out;
        }

        static bool ExtractFormValueUtf8_(
            const std::string& formUtf8,
            const char* keyName,
            std::string& outValue)
        {
            outValue.clear();

            if (keyName == nullptr || *keyName == '\0') {
                return false;
            }

            std::size_t pos = 0;

            while (pos <= formUtf8.size()) {
                std::size_t amp = formUtf8.find('&', pos);
                if (amp == std::string::npos) {
                    amp = formUtf8.size();
                }

                const std::string_view pair(
                    formUtf8.data() + pos,
                    amp - pos);

                const std::size_t eq = pair.find('=');

                std::string rawKey;
                std::string rawValue;

                if (eq == std::string_view::npos) {
                    rawKey.assign(pair.data(), pair.size());
                }
                else {
                    rawKey.assign(pair.data(), eq);
                    rawValue.assign(pair.data() + eq + 1, pair.size() - eq - 1);
                }

                const std::string decodedKey =
                    UrlDecodeFormComponentUtf8_(rawKey);

                if (decodedKey == keyName) {
                    outValue =
                        UrlDecodeFormComponentUtf8_(rawValue);
                    return true;
                }

                if (amp >= formUtf8.size()) {
                    break;
                }

                pos = amp + 1;
            }

            return false;
        }

        static std::uint32_t ClampId_(
            int value) noexcept
        {
            if (value <= 0) {
                return 0;
            }

            return static_cast<std::uint32_t>(value);
        }

        static int ClampDurationMinutes_(
            int value) noexcept
        {
            if (value < 0) {
                return 0;
            }

            if (value > 24 * 60) {
                return 24 * 60;
            }

            return value;
        }

        static int ClampMinutesOrDisabled_(
            int value) noexcept
        {
            if (value < 0) {
                return -1;
            }

            return ClampDurationMinutes_(value);
        }

        static int ClampSpace_(
            int value) noexcept
        {
            if (value < -1) {
                return -1;
            }

            if (value > 2) {
                return 2;
            }

            return value;
        }

        static int ClampSeconds_(
            int value) noexcept
        {
            if (value < 0) {
                return 0;
            }

            if (value > 24 * 60 * 60) {
                return 24 * 60 * 60;
            }

            return value;
        }

        static int ClampPriority_(
            int value) noexcept
        {
            if (value < 0) {
                return 0;
            }

            if (value > 100) {
                return 100;
            }

            return value;
        }

        static std::wstring NormalizeWeekdays_(
            std::wstring value)
        {
            value = Trim_(std::move(value));

            std::wstring out;
            out.reserve(7);

            for (wchar_t ch : value) {
                if (ch == L'0' || ch == L'1') {
                    out.push_back(ch);
                    if (out.size() >= 7) {
                        break;
                    }
                }
            }

            while (out.size() < 7) {
                out.push_back(L'1');
            }

            return out;
        }

        static bool HasAnyAutoReserveCondition_(
            const Rule& r)
        {
            if (r.useKeyword && !Trim_(r.keyword).empty()) {
                return true;
            }

            if (r.useService && !Trim_(r.serviceName).empty()) {
                return true;
            }

            if (r.useGenre && !Trim_(r.genreName).empty()) {
                return true;
            }

            return false;
        }

        static std::wstring MakeDefaultRuleName_(
            const Rule& r)
        {
            if (!Trim_(r.name).empty()) {
                return r.name;
            }

            if (r.useKeyword && !Trim_(r.keyword).empty()) {
                return r.keyword;
            }

            if (r.useGenre && !Trim_(r.genreName).empty()) {
                return L"\u30B8\u30E3\u30F3\u30EB: " + r.genreName;
            }

            if (r.useService && !Trim_(r.serviceName).empty()) {
                return L"\u653E\u9001\u5C40: " + r.serviceName;
            }

            return L"\u81EA\u52D5\u4E88\u7D04\u8A2D\u5B9A " +
                std::to_wstring(r.id);
        }

        static Rule ReadRule_(
            const std::wstring& iniPath,
            const std::wstring& section)
        {
            Rule r{};

            r.id =
                ClampId_(
                    ReadIniInt_(
                        iniPath,
                        section,
                        L"Id",
                        0));

            r.enabled =
                ReadIniBool_(
                    iniPath,
                    section,
                    L"Enabled",
                    true);

            r.name =
                SanitizeIniValue_(
                    ReadIniString_(
                        iniPath,
                        section,
                        L"Name",
                        L""));

            r.keyword =
                SanitizeIniValue_(
                    ReadIniString_(
                        iniPath,
                        section,
                        L"Keyword",
                        L""));

            r.excludeKeyword =
                SanitizeIniValue_(
                    ReadIniString_(
                        iniPath,
                        section,
                        L"ExcludeKeyword",
                        L""));

            r.titleMatchMode =
                MatchModeFromInt(
                    ReadIniInt_(
                        iniPath,
                        section,
                        L"TitleMatchMode",
                        static_cast<int>(MatchMode::Contains)));

            r.matchTitle =
                ReadIniBool_(
                    iniPath,
                    section,
                    L"MatchTitle",
                    true);

            r.matchSummary =
                ReadIniBool_(
                    iniPath,
                    section,
                    L"MatchSummary",
                    false);

            r.matchExtended =
                ReadIniBool_(
                    iniPath,
                    section,
                    L"MatchExtended",
                    false);

            r.targetSpace =
                ClampSpace_(
                    ReadIniInt_(
                        iniPath,
                        section,
                        L"TargetSpace",
                        0));

            r.serviceName =
                SanitizeIniValue_(
                    ReadIniString_(
                        iniPath,
                        section,
                        L"ServiceName",
                        L""));

            r.genreName =
                SanitizeIniValue_(
                    ReadIniString_(
                        iniPath,
                        section,
                        L"GenreName",
                        L""));

            r.startMinutes =
                ClampMinutesOrDisabled_(
                    ReadIniInt_(
                        iniPath,
                        section,
                        L"StartMinutes",
                        -1));

            r.endMinutes =
                ClampMinutesOrDisabled_(
                    ReadIniInt_(
                        iniPath,
                        section,
                        L"EndMinutes",
                        -1));

            r.priority =
                ClampPriority_(
                    ReadIniInt_(
                        iniPath,
                        section,
                        L"Priority",
                        50));

            r.preMarginSeconds =
                ClampSeconds_(
                    ReadIniInt_(
                        iniPath,
                        section,
                        L"PreMarginSeconds",
                        0));

            r.postMarginSeconds =
                ClampSeconds_(
                    ReadIniInt_(
                        iniPath,
                        section,
                        L"PostMarginSeconds",
                        0));

            r.createReservationEnabled =
                ReadIniBool_(
                    iniPath,
                    section,
                    L"CreateReservationEnabled",
                    true);

            r.duplicatePolicy =
                DuplicatePolicyFromInt(
                    ReadIniInt_(
                        iniPath,
                        section,
                        L"DuplicatePolicy",
                        static_cast<int>(
                            DuplicatePolicy::SkipSameEvent)));

            r.useKeyword =
                ReadIniBool_(
                    iniPath,
                    section,
                    L"UseKeyword",
                    !r.keyword.empty());

            r.keywordJoinMode =
                TokenJoinModeFromString(
                    ReadIniString_(
                        iniPath,
                        section,
                        L"KeywordJoinMode",
                        L"OR"));

            r.useExcludeKeyword =
                ReadIniBool_(
                    iniPath,
                    section,
                    L"UseExcludeKeyword",
                    !r.excludeKeyword.empty());

            r.excludeKeywordJoinMode =
                TokenJoinModeFromString(
                    ReadIniString_(
                        iniPath,
                        section,
                        L"ExcludeKeywordJoinMode",
                        L"OR"));

            r.useService =
                ReadIniBool_(
                    iniPath,
                    section,
                    L"UseService",
                    !r.serviceName.empty());

            r.serviceMatchMode =
                TokenJoinModeFromString(
                    ReadIniString_(
                        iniPath,
                        section,
                        L"ServiceMatchMode",
                        L"OR"));

            r.useGenre =
                ReadIniBool_(
                    iniPath,
                    section,
                    L"UseGenre",
                    !r.genreName.empty());

            r.genreMatchMode =
                TokenJoinModeFromString(
                    ReadIniString_(
                        iniPath,
                        section,
                        L"GenreMatchMode",
                        L"OR"));

            r.useMinDuration =
                ReadIniBool_(
                    iniPath,
                    section,
                    L"UseMinDuration",
                    false);

            r.minDurationMinutes =
                ClampDurationMinutes_(
                    ReadIniInt_(
                        iniPath,
                        section,
                        L"MinDurationMinutes",
                        0));

            const bool oldHasTimeRange =
                r.startMinutes >= 0 &&
                r.endMinutes >= 0;

            r.useSearchTimeRange =
                ReadIniBool_(
                    iniPath,
                    section,
                    L"UseSearchTimeRange",
                    oldHasTimeRange);

            r.searchStartMinutes =
                ClampDurationMinutes_(
                    ReadIniInt_(
                        iniPath,
                        section,
                        L"SearchStartMinutes",
                        oldHasTimeRange ? r.startMinutes : 0));

            r.searchEndMinutes =
                ClampDurationMinutes_(
                    ReadIniInt_(
                        iniPath,
                        section,
                        L"SearchEndMinutes",
                        oldHasTimeRange ? r.endMinutes : 1440));

            r.searchTimeWrap =
                ReadIniBool_(
                    iniPath,
                    section,
                    L"SearchTimeWrap",
                    false);

            r.startMinutes = r.useSearchTimeRange ? r.searchStartMinutes : -1;
            r.endMinutes = r.useSearchTimeRange ? r.searchEndMinutes : -1;

            r.useWeekday =
                ReadIniBool_(
                    iniPath,
                    section,
                    L"UseWeekday",
                    false);

            r.weekdays =
                NormalizeWeekdays_(
                    ReadIniString_(
                        iniPath,
                        section,
                        L"Weekdays",
                        L"1111111"));

            r.skipDuplicate =
                ReadIniBool_(
                    iniPath,
                    section,
                    L"SkipDuplicate",
                    true);

            r.excludeRerunLikeProgram =
                ReadIniBool_(
                    iniPath,
                    section,
                    L"ExcludeRerunLikeProgram",
                    false);

            // 旧INIの ActionMode=candidate は録画自動予約へ移行します。
            // 現行仕様では自動予約ルールの動作は録画予約のみです。
            r.actionMode = ReserveActionMode::Auto;

            if (r.name.empty()) {
                r.name = MakeDefaultRuleName_(r);
            }

            return r;
        }

        static bool WriteRule_(
            const std::wstring& iniPath,
            const std::wstring& section,
            const Rule& r)
        {
            bool ok = true;

            const bool useTimeRange = r.useSearchTimeRange;
            const int startMinutes =
                useTimeRange ? ClampDurationMinutes_(r.searchStartMinutes) : -1;
            const int endMinutes =
                useTimeRange ? ClampDurationMinutes_(r.searchEndMinutes) : -1;

            ok = WriteIniUInt_(iniPath, section, L"Id", r.id) && ok;
            ok = WriteIniBool_(iniPath, section, L"Enabled", r.enabled) && ok;

            ok = WriteIniString_(
                iniPath,
                section,
                L"Name",
                SanitizeIniValue_(r.name)) && ok;

            ok = WriteIniBool_(
                iniPath,
                section,
                L"UseKeyword",
                r.useKeyword) && ok;

            ok = WriteIniString_(
                iniPath,
                section,
                L"Keyword",
                SanitizeIniValue_(r.keyword)) && ok;

            ok = WriteIniString_(
                iniPath,
                section,
                L"KeywordJoinMode",
                TokenJoinModeToString(r.keywordJoinMode)) && ok;

            ok = WriteIniBool_(
                iniPath,
                section,
                L"UseExcludeKeyword",
                r.useExcludeKeyword) && ok;

            ok = WriteIniString_(
                iniPath,
                section,
                L"ExcludeKeyword",
                SanitizeIniValue_(r.excludeKeyword)) && ok;

            ok = WriteIniString_(
                iniPath,
                section,
                L"ExcludeKeywordJoinMode",
                TokenJoinModeToString(r.excludeKeywordJoinMode)) && ok;

            ok = WriteIniInt_(
                iniPath,
                section,
                L"TitleMatchMode",
                static_cast<int>(r.titleMatchMode)) && ok;

            ok = WriteIniBool_(iniPath, section, L"MatchTitle", r.matchTitle) && ok;
            ok = WriteIniBool_(iniPath, section, L"MatchSummary", r.matchSummary) && ok;
            ok = WriteIniBool_(iniPath, section, L"MatchExtended", r.matchExtended) && ok;

            ok = WriteIniInt_(iniPath, section, L"TargetSpace", ClampSpace_(r.targetSpace)) && ok;

            ok = WriteIniBool_(iniPath, section, L"UseService", r.useService) && ok;

            ok = WriteIniString_(
                iniPath,
                section,
                L"ServiceName",
                SanitizeIniValue_(r.serviceName)) && ok;

            ok = WriteIniString_(
                iniPath,
                section,
                L"ServiceMatchMode",
                TokenJoinModeToString(r.serviceMatchMode)) && ok;

            ok = WriteIniBool_(iniPath, section, L"UseGenre", r.useGenre) && ok;

            ok = WriteIniString_(
                iniPath,
                section,
                L"GenreName",
                SanitizeIniValue_(r.genreName)) && ok;

            ok = WriteIniString_(
                iniPath,
                section,
                L"GenreMatchMode",
                TokenJoinModeToString(r.genreMatchMode)) && ok;

            ok = WriteIniBool_(iniPath, section, L"UseMinDuration", r.useMinDuration) && ok;
            ok = WriteIniInt_(iniPath, section, L"MinDurationMinutes", ClampDurationMinutes_(r.minDurationMinutes)) && ok;

            ok = WriteIniBool_(iniPath, section, L"UseSearchTimeRange", useTimeRange) && ok;
            ok = WriteIniInt_(iniPath, section, L"SearchStartMinutes", ClampDurationMinutes_(r.searchStartMinutes)) && ok;
            ok = WriteIniInt_(iniPath, section, L"SearchEndMinutes", ClampDurationMinutes_(r.searchEndMinutes)) && ok;
            ok = WriteIniBool_(iniPath, section, L"SearchTimeWrap", r.searchTimeWrap) && ok;

            ok = WriteIniInt_(iniPath, section, L"StartMinutes", startMinutes) && ok;
            ok = WriteIniInt_(iniPath, section, L"EndMinutes", endMinutes) && ok;

            ok = WriteIniBool_(iniPath, section, L"UseWeekday", r.useWeekday) && ok;

            ok = WriteIniString_(
                iniPath,
                section,
                L"Weekdays",
                NormalizeWeekdays_(r.weekdays)) && ok;

            ok = WriteIniInt_(iniPath, section, L"Priority", ClampPriority_(r.priority)) && ok;
            ok = WriteIniInt_(iniPath, section, L"PreMarginSeconds", ClampSeconds_(r.preMarginSeconds)) && ok;
            ok = WriteIniInt_(iniPath, section, L"PostMarginSeconds", ClampSeconds_(r.postMarginSeconds)) && ok;
            ok = WriteIniBool_(iniPath, section, L"CreateReservationEnabled", r.createReservationEnabled) && ok;

            ok = WriteIniInt_(
                iniPath,
                section,
                L"DuplicatePolicy",
                static_cast<int>(r.duplicatePolicy)) && ok;

            ok = WriteIniBool_(iniPath, section, L"SkipDuplicate", r.skipDuplicate) && ok;
            ok = WriteIniBool_(iniPath, section, L"ExcludeRerunLikeProgram", r.excludeRerunLikeProgram) && ok;

            ok = WriteIniString_(
                iniPath,
                section,
                L"ActionMode",
                ReserveActionModeToString(r.actionMode)) && ok;

            return ok;
        }
    }

    LoadResult RuleStore::LoadFromIni(
        const std::wstring& iniPath)
    {
        const auto t0 =
            std::chrono::steady_clock::now();

        LoadResult result{};

        Clear();

        if (iniPath.empty()) {
            result.ok = false;
            result.message = L"iniPath is empty";

            DebugLogFormat(
                L"[AutoReserve][RuleStore] load failed empty-path ms=%lld",
                ElapsedMs_(t0));

            return result;
        }

        std::uintmax_t fileSize = 0;
        const bool exists =
            GetFileInfoNoThrow_(
                iniPath,
                fileSize);

        DebugLogFormat(
            L"[AutoReserve][RuleStore] load begin exists=%d size=%llu path=%ls",
            exists ? 1 : 0,
            static_cast<unsigned long long>(fileSize),
            iniPath.c_str());

        if (!exists) {
            result.ok = false;
            result.loadedCount = 0;
            result.message = L"ini file not found";

            DebugLogFormat(
                L"[AutoReserve][RuleStore] load skipped file-not-found ms=%lld path=%ls",
                ElapsedMs_(t0),
                iniPath.c_str());

            return result;
        }

        const int count =
            ReadIniInt_(
                iniPath,
                kRootSection,
                L"Count",
                0);

        const int nextId =
            ReadIniInt_(
                iniPath,
                kRootSection,
                L"NextId",
                1);

        nextId_ =
            static_cast<std::uint32_t>(
                (std::max)(nextId, 1));

        std::uint32_t maxId = 0;

        const int safeCount =
            (std::max)(0, count);

        for (int i = 0; i < safeCount; ++i) {
            const std::wstring section =
                MakeRuleSectionName_(
                    static_cast<std::uint32_t>(i));

            Rule r =
                ReadRule_(
                    iniPath,
                    section);

            if (r.id == 0) {
                DebugLogFormat(
                    L"[AutoReserve][RuleStore] skip rule index=%d reason=id-zero section=%ls",
                    i,
                    section.c_str());
                continue;
            }

            if (r.name.empty() && !HasAnyAutoReserveCondition_(r)) {
                DebugLogFormat(
                    L"[AutoReserve][RuleStore] skip rule id=%u index=%d reason=no-condition section=%ls",
                    r.id,
                    i,
                    section.c_str());
                continue;
            }

            maxId = (std::max)(maxId, r.id);
            rules_.push_back(std::move(r));
        }

        if (nextId_ <= maxId) {
            nextId_ = maxId + 1;
        }

        result.ok = true;
        result.loadedCount = rules_.size();
        result.message = L"loaded";

        DebugLogFormat(
            L"[AutoReserve][RuleStore] load done ok=1 countKey=%d loaded=%zu nextId=%u size=%llu ms=%lld path=%ls",
            count,
            rules_.size(),
            nextId_,
            static_cast<unsigned long long>(fileSize),
            ElapsedMs_(t0),
            iniPath.c_str());

        return result;
    }

    SaveResult RuleStore::SaveToIni(
        const std::wstring& iniPath) const
    {
        const auto t0 =
            std::chrono::steady_clock::now();

        SaveResult result{};

        if (iniPath.empty()) {
            result.ok = false;
            result.message = L"iniPath is empty";
            return result;
        }

        if (!EnsureParentDirectory_(iniPath)) {
            result.ok = false;
            result.message = L"parent directory create failed";
            return result;
        }

        if (!EnsureIniFileUnicode_(iniPath)) {
            result.ok = false;
            result.message = L"ini file create failed";
            return result;
        }

        const int oldCount =
            ReadIniInt_(
                iniPath,
                kRootSection,
                L"Count",
                0);

        for (int i = 0; i < oldCount; ++i) {
            DeleteIniSection_(
                iniPath,
                MakeRuleSectionName_(
                    static_cast<std::uint32_t>(i)));
        }

        bool ok = true;

        ok = WriteIniInt_(
            iniPath,
            kRootSection,
            L"Count",
            static_cast<int>(rules_.size())) && ok;

        ok = WriteIniUInt_(
            iniPath,
            kRootSection,
            L"NextId",
            nextId_) && ok;

        for (std::size_t i = 0; i < rules_.size(); ++i) {
            const std::wstring section =
                MakeRuleSectionName_(
                    static_cast<std::uint32_t>(i));

            ok = WriteRule_(
                iniPath,
                section,
                rules_[i]) && ok;
        }

        result.ok = ok;
        result.savedCount = ok ? rules_.size() : 0;
        result.message = ok ? L"saved" : L"save failed";

        if (ok &&
            PathsReferToSameFileNoThrow_(
                iniPath,
                GetDefaultRulesIniPath())) {
            const CacheResult cache =
                WriteRulesCache(*this);

            if (!cache.ok) {
                result.message +=
                    L"; cache update failed: " +
                    cache.message;
            }
        }

        std::uintmax_t savedSize = 0;
        const bool savedExists =
            GetFileInfoNoThrow_(
                iniPath,
                savedSize);

        DebugLogFormat(
            L"[AutoReserve][RuleStore] save done ok=%d rules=%zu nextId=%u exists=%d size=%llu ms=%lld path=%ls",
            ok ? 1 : 0,
            rules_.size(),
            nextId_,
            savedExists ? 1 : 0,
            static_cast<unsigned long long>(savedSize),
            ElapsedMs_(t0),
            iniPath.c_str());

        return result;
    }

    const std::vector<Rule>& RuleStore::GetRules() const noexcept
    {
        return rules_;
    }

    void RuleStore::Clear()
    {
        rules_.clear();
        nextId_ = 1;
    }

    std::uint32_t RuleStore::GenerateId_()
    {
        if (nextId_ == 0) {
            nextId_ = 1;
        }

        const std::uint32_t id = nextId_;
        ++nextId_;
        return id;
    }

    std::uint32_t RuleStore::AddRule(
        const Rule& rule)
    {
        Rule r = rule;
        r.actionMode = ReserveActionMode::Auto;

        if (r.id == 0 || FindRule(r.id) != nullptr) {
            r.id = GenerateId_();
        }

        if (r.useSearchTimeRange) {
            r.startMinutes = ClampDurationMinutes_(r.searchStartMinutes);
            r.endMinutes = ClampDurationMinutes_(r.searchEndMinutes);
        }
        else {
            r.startMinutes = -1;
            r.endMinutes = -1;
        }

        if (r.name.empty()) {
            r.name = MakeDefaultRuleName_(r);
        }

        rules_.push_back(std::move(r));
        return rules_.back().id;
    }

    bool RuleStore::UpdateRule(
        const Rule& rule)
    {
        if (rule.id == 0) {
            return false;
        }

        Rule* p =
            FindRule(rule.id);

        if (p == nullptr) {
            return false;
        }

        Rule r = rule;
        r.actionMode = ReserveActionMode::Auto;
        if (r.useSearchTimeRange) {
            r.startMinutes = ClampDurationMinutes_(r.searchStartMinutes);
            r.endMinutes = ClampDurationMinutes_(r.searchEndMinutes);
        }
        else {
            r.startMinutes = -1;
            r.endMinutes = -1;
        }

        if (r.name.empty()) {
            r.name = MakeDefaultRuleName_(r);
        }

        *p = std::move(r);
        return true;
    }

    bool RuleStore::RemoveRule(
        std::uint32_t id)
    {
        const auto oldSize =
            rules_.size();

        rules_.erase(
            std::remove_if(
                rules_.begin(),
                rules_.end(),
                [id](const Rule& r) {
                    return r.id == id;
                }),
            rules_.end());

        return rules_.size() != oldSize;
    }

    Rule* RuleStore::FindRule(
        std::uint32_t id) noexcept
    {
        for (Rule& r : rules_) {
            if (r.id == id) {
                return &r;
            }
        }

        return nullptr;
    }

    const Rule* RuleStore::FindRule(
        std::uint32_t id) const noexcept
    {
        for (const Rule& r : rules_) {
            if (r.id == id) {
                return &r;
            }
        }

        return nullptr;
    }

    std::uint32_t RuleStore::GetNextId() const noexcept
    {
        return nextId_;
    }

    void RuleStore::SetNextId(
        std::uint32_t nextId) noexcept
    {
        nextId_ = nextId == 0 ? 1 : nextId;
    }

    std::wstring GetDefaultRulesIniPath()
    {
        return (std::filesystem::path(GetExeDir_()) /
            std::filesystem::path(L"auto_reserve_rules.ini")).wstring();
    }

    std::wstring GetDefaultRulesCacheDirectory()
    {
        return (
            std::filesystem::path(GetExeDir_()) /
            std::filesystem::path(L"cache") /
            std::filesystem::path(L"auto_reserve")).wstring();
    }

    std::wstring GetDefaultRulesCachePath()
    {
        return (
            std::filesystem::path(
                GetDefaultRulesCacheDirectory()) /
            std::filesystem::path(
                L"auto_reserve_rules.cache")).wstring();
    }

    CacheResult WriteRulesCache(
        const RuleStore& store)
    {
        const auto t0 =
            std::chrono::steady_clock::now();

        CacheResult result{};
        result.cachePath =
            GetDefaultRulesCachePath();
        result.ruleCount =
            store.GetRules().size();

        const std::wstring cacheText =
            BuildRulesCacheText_(store);

        const std::string cacheTextUtf8 =
            WideToUtf8_(cacheText);

        if (cacheText.empty() ||
            cacheTextUtf8.empty()) {
            result.message =
                L"cache text conversion failed";

            DebugLogFormat(
                L"[AutoReserve][RuleStore] cache write failed reason=convert path=%ls ms=%lld",
                result.cachePath.c_str(),
                ElapsedMs_(t0));

            return result;
        }

        std::wstring writeMessage;

        result.ok =
            WriteRulesCacheAtomic_(
                result.cachePath,
                cacheTextUtf8,
                writeMessage);
        result.updated = result.ok;
        result.message =
            std::move(writeMessage);

        DebugLogFormat(
            L"[AutoReserve][RuleStore] cache write done ok=%d updated=%d rules=%zu path=%ls message=%ls ms=%lld",
            result.ok ? 1 : 0,
            result.updated ? 1 : 0,
            result.ruleCount,
            result.cachePath.c_str(),
            result.message.c_str(),
            ElapsedMs_(t0));

        return result;
    }

    CacheResult RebuildRulesCache()
    {
        CacheResult result{};
        result.cachePath =
            GetDefaultRulesCachePath();

        const std::wstring iniPath =
            GetDefaultRulesIniPath();

        const FileStamp_ iniStamp =
            GetFileStampNoThrow_(iniPath);

        RuleStore store;

        // ルール未登録時も、一覧ページで正常に「0件」を表示できるようにします。
        if (!iniStamp.exists) {
            result =
                WriteRulesCache(store);

            if (result.ok) {
                result.message =
                    L"empty rules cache written because ini file does not exist";
            }

            DebugLogFormat(
                L"[AutoReserve][RuleStore] empty cache rebuild ok=%d rules=%zu path=%ls message=%ls",
                result.ok ? 1 : 0,
                result.ruleCount,
                result.cachePath.c_str(),
                result.message.c_str());

            return result;
        }

        const LoadResult load =
            store.LoadFromIni(iniPath);

        if (!load.ok) {
            result.ok = false;
            result.updated = false;
            result.ruleCount = 0;
            result.message =
                L"rules ini load failed: " +
                load.message;

            DebugLogFormat(
                L"[AutoReserve][RuleStore] cache rebuild failed path=%ls message=%ls",
                result.cachePath.c_str(),
                result.message.c_str());

            return result;
        }

        result =
            WriteRulesCache(store);

        DebugLogFormat(
            L"[AutoReserve][RuleStore] cache rebuild done ok=%d updated=%d rules=%zu path=%ls message=%ls",
            result.ok ? 1 : 0,
            result.updated ? 1 : 0,
            result.ruleCount,
            result.cachePath.c_str(),
            result.message.c_str());

        return result;
    }

    CacheResult EnsureRulesCacheCurrent()
    {
        CacheResult result{};
        result.cachePath =
            GetDefaultRulesCachePath();

        const std::wstring iniPath =
            GetDefaultRulesIniPath();

        const FileStamp_ iniStamp =
            GetFileStampNoThrow_(iniPath);

        // INI未作成はエラーではなく、ルール0件として扱います。
        // すでに正常な0件キャッシュがあれば再利用します。
        if (!iniStamp.exists) {
            const FileStamp_ cacheStamp =
                GetFileStampNoThrow_(
                    result.cachePath);

            if (cacheStamp.exists &&
                cacheStamp.size >= 3) {
                std::string cacheText;
                std::wstring loadMessage;

                if (ReadUtf8CacheFile_(
                    result.cachePath,
                    cacheText,
                    loadMessage) &&
                    cacheText.find(
                        "\nCount=0\n") != std::string::npos) {
                    result.ok = true;
                    result.updated = false;
                    result.ruleCount = 0;
                    result.message =
                        L"empty cache is current";

                    return result;
                }
            }

            return RebuildRulesCache();
        }

        const FileStamp_ cacheStamp =
            GetFileStampNoThrow_(
                result.cachePath);

        if (cacheStamp.exists &&
            cacheStamp.size >= 3 &&
            cacheStamp.writeTime >= iniStamp.writeTime) {
            std::string cacheText;
            std::wstring loadMessage;

            if (ReadUtf8CacheFile_(
                result.cachePath,
                cacheText,
                loadMessage)) {
                result.ok = true;
                result.updated = false;
                result.ruleCount =
                    ParseRuleCountFromCacheText_(
                        cacheText);
                result.message =
                    L"cache is current";

                return result;
            }
        }

        return RebuildRulesCache();
    }

    CacheResult RefreshRulesCache(
        RulesCacheRefreshMode mode)
    {
        switch (mode) {
        case RulesCacheRefreshMode::EnsureCurrent:
            return EnsureRulesCacheCurrent();

        case RulesCacheRefreshMode::ForceRebuild:
            return RebuildRulesCache();

        default:
            break;
        }

        CacheResult result{};
        result.cachePath =
            GetDefaultRulesCachePath();
        result.message =
            L"invalid rules cache refresh mode";

        return result;
    }

    bool LoadRulesCacheUtf8(
        std::string& outCacheText,
        std::wstring* outMessage)
    {
        outCacheText.clear();

        const CacheResult ensured =
            EnsureRulesCacheCurrent();

        if (!ensured.ok) {
            if (outMessage) {
                *outMessage =
                    ensured.message;
            }

            return false;
        }

        std::wstring loadMessage;

        const bool ok =
            ReadUtf8CacheFile_(
                ensured.cachePath,
                outCacheText,
                loadMessage);

        if (outMessage) {
            *outMessage =
                std::move(loadMessage);
        }

        return ok;
    }

    std::wstring MatchModeToString(
        MatchMode mode)
    {
        switch (mode) {
        case MatchMode::Exact:
            return L"Exact";
        case MatchMode::StartsWith:
            return L"StartsWith";
        case MatchMode::EndsWith:
            return L"EndsWith";
        case MatchMode::Contains:
        default:
            return L"Contains";
        }
    }

    MatchMode MatchModeFromInt(
        int value) noexcept
    {
        switch (value) {
        case static_cast<int>(MatchMode::Exact):
            return MatchMode::Exact;
        case static_cast<int>(MatchMode::StartsWith):
            return MatchMode::StartsWith;
        case static_cast<int>(MatchMode::EndsWith):
            return MatchMode::EndsWith;
        case static_cast<int>(MatchMode::Contains):
        default:
            return MatchMode::Contains;
        }
    }

    std::wstring DuplicatePolicyToString(
        DuplicatePolicy policy)
    {
        switch (policy) {
        case DuplicatePolicy::SkipSameTitleAndTime:
            return L"SkipSameTitleAndTime";
        case DuplicatePolicy::Allow:
            return L"Allow";
        case DuplicatePolicy::SkipSameEvent:
        default:
            return L"SkipSameEvent";
        }
    }

    DuplicatePolicy DuplicatePolicyFromInt(
        int value) noexcept
    {
        switch (value) {
        case static_cast<int>(DuplicatePolicy::SkipSameTitleAndTime):
            return DuplicatePolicy::SkipSameTitleAndTime;
        case static_cast<int>(DuplicatePolicy::Allow):
            return DuplicatePolicy::Allow;
        case static_cast<int>(DuplicatePolicy::SkipSameEvent):
        default:
            return DuplicatePolicy::SkipSameEvent;
        }
    }

    std::wstring TokenJoinModeToString(
        TokenJoinMode mode)
    {
        return mode == TokenJoinMode::And
            ? L"AND"
            : L"OR";
    }

    TokenJoinMode TokenJoinModeFromString(
        const std::wstring& value) noexcept
    {
        const std::wstring upper = ToUpperAscii_(Trim_(value));

        if (upper == L"AND") {
            return TokenJoinMode::And;
        }

        return TokenJoinMode::Or;
    }

    std::wstring ReserveActionModeToString(
        ReserveActionMode mode)
    {
        (void)mode;
        return L"auto";
    }

    ReserveActionMode ReserveActionModeFromString(
        const std::wstring& value) noexcept
    {
        // candidate を含む旧設定値は、すべて録画自動予約へ正規化します。
        (void)value;
        return ReserveActionMode::Auto;
    }

    SaveResult SaveIniTextToDefaultRulesIni(
        const std::wstring& iniText)
    {
        SaveResult result{};

        std::wstring text =
            StripUtf16Bom_(iniText);

        if (Trim_(text).empty()) {
            result.ok = false;
            result.message = L"ini text is empty";
            return result;
        }

        if (!ContainsIniSection_(text, kRootSection)) {
            result.ok = false;
            result.message = L"AutoReserveRules section not found";
            return result;
        }

        const std::wstring dstPath =
            GetDefaultRulesIniPath();

        if (dstPath.empty()) {
            result.ok = false;
            result.message = L"default ini path is empty";
            return result;
        }

        const std::wstring tmpPath =
            dstPath + L".incoming.tmp";

        DeleteFileNoThrow_(tmpPath);

        if (!WriteUtf16LeTextFile_(tmpPath, text)) {
            result.ok = false;
            result.message = L"temporary ini write failed";

            DebugLogFormat(
                L"[AutoReserve][RuleStore] incoming ini temp write failed path=%ls",
                tmpPath.c_str());

            return result;
        }

        RuleStore store;
        const LoadResult load =
            store.LoadFromIni(tmpPath);

        DeleteFileNoThrow_(tmpPath);

        if (!load.ok) {
            result.ok = false;
            result.message = L"incoming ini load failed: " + load.message;

            DebugLogFormat(
                L"[AutoReserve][RuleStore] incoming ini load failed message=%ls",
                result.message.c_str());

            return result;
        }

        const SaveResult save =
            store.SaveToIni(dstPath);

        DebugLogFormat(
            L"[AutoReserve][RuleStore] incoming ini save ok=%d rules=%zu path=%ls",
            save.ok ? 1 : 0,
            save.savedCount,
            dstPath.c_str());

        return save;
    }

    SaveResult SaveIniUtf8ToDefaultRulesIni(
        const std::string& iniUtf8)
    {
        SaveResult result{};

        if (iniUtf8.empty()) {
            result.ok = false;
            result.message = L"ini utf8 text is empty";
            return result;
        }

        std::wstring text =
            Utf8ToWide_(iniUtf8);

        if (text.empty()) {
            result.ok = false;
            result.message = L"utf8 decode failed";
            return result;
        }

        return SaveIniTextToDefaultRulesIni(text);
    }

    SaveResult SaveIniFormUtf8ToDefaultRulesIni(
        const std::string& formUtf8)
    {
        SaveResult result{};

        if (formUtf8.empty()) {
            result.ok = false;
            result.message = L"form body is empty";
            return result;
        }

        if (formUtf8.size() > kMaxIncomingAutoReserveFormBytes_) {
            result.ok = false;
            result.message = L"form body is too large";
            return result;
        }

        std::string iniUtf8;

        if (!ExtractFormValueUtf8_(
            formUtf8,
            "iniText",
            iniUtf8)) {
            result.ok = false;
            result.message = L"iniText field not found";
            return result;
        }

        if (iniUtf8.empty()) {
            result.ok = false;
            result.message = L"iniText is empty";
            return result;
        }

        return SaveIniUtf8ToDefaultRulesIni(iniUtf8);
    }

    std::string MakeSaveResultJsonUtf8(
        const SaveResult& result)
    {
        std::string json;

        json += "{";
        json += "\"ok\":";
        json += result.ok ? "true" : "false";
        json += ",\"savedCount\":";
        json += std::to_string(result.savedCount);
        json += ",\"message\":\"";
        json += EscapeJsonUtf8_(WideToUtf8_(result.message));
        json += "\"";
        json += "}";

        return json;
    }

    std::wstring ApplyTriggerToReason(
        ApplyTrigger trigger)
    {
        switch (trigger) {
        case ApplyTrigger::RuleSave:
            return L"rule-save";

        case ApplyTrigger::EpgUpdate:
            return L"epg-update";

        case ApplyTrigger::Manual:
            return L"manual";

        case ApplyTrigger::Unknown:
        default:
            return L"auto-reserve";
        }
    }

    CurrentSearchMatchRequest MakeCurrentSearchMatchRequest(
        ApplyTrigger trigger,
        std::size_t maxMatches)
    {
        CurrentSearchMatchRequest request{};

        request.trigger = trigger;
        request.reason = ApplyTriggerToReason(trigger);
        request.maxMatches = maxMatches;

        // 自動予約ルールは録画予約専用です。
        request.includeCandidateRules = false;
        request.includeAutoRules = true;

        return request;
    }

    CurrentSearchMatchSummary SummarizeRuleMatchResultForRecording(
        const RuleMatchResult& matchResult,
        const CurrentSearchMatchRequest& request) noexcept
    {
        CurrentSearchMatchSummary summary{};

        summary.reason =
            request.reason.empty()
            ? ApplyTriggerToReason(request.trigger)
            : request.reason;

        summary.ok = matchResult.ok;
        summary.ruleCount = matchResult.ruleCount;
        summary.programCount = matchResult.programCount;
        summary.checkedCount = matchResult.checkedCount;
        summary.matchedCount = matchResult.matchedCount;
        summary.candidateCount = 0;
        summary.autoRegisterCount = matchResult.autoRegisterCount;

        if (!matchResult.ok) {
            summary.hasMatchedPrograms = false;
            summary.shouldApplyToRecording = false;
            summary.message = L"match result is not ok: " + matchResult.message;
            return summary;
        }

        summary.hasMatchedPrograms =
            !matchResult.matches.empty();

        if (!summary.hasMatchedPrograms) {
            summary.shouldApplyToRecording = false;
            summary.message = L"no matched programs";
            return summary;
        }

        if (!request.includeAutoRules &&
            matchResult.autoRegisterCount != 0) {
            summary.shouldApplyToRecording = false;
            summary.message = L"auto rules are excluded";
            return summary;
        }

        summary.shouldApplyToRecording = true;
        summary.message = L"matched";
        return summary;
    }

    namespace
    {
        static std::wstring MakeRegisterReserveSourceFromTrigger_(
            ApplyTrigger trigger)
        {
            switch (trigger) {
            case ApplyTrigger::RuleSave:
                return L"auto_reserve";

            case ApplyTrigger::EpgUpdate:
                return L"auto_reserve_epg_update";

            case ApplyTrigger::Manual:
                return L"auto_reserve_manual";

            case ApplyTrigger::Unknown:
            default:
                return L"auto_reserve";
            }
        }

        static void CopyAutoReserveRegisterResult_(
            CurrentSearchRegisterResult& dst,
            const TaskSystemConnector::AutoReserveRecordingApplyResult& src)
        {
            dst.registerInput = src.inputMatches;
            dst.registerTarget = src.targetPrograms;
            dst.registered = src.registered;
            dst.dryRunSkipped = src.dryRunSkipped;
            dst.duplicateSkipped = src.duplicateSkipped;
            dst.userBlockedSkipped = src.userBlockedSkipped;
            dst.invalidSkipped = src.invalidSkipped;
            dst.limitSkipped = src.limitSkipped;
            dst.actionSkipped = src.actionSkipped;
            dst.failed = src.failed;
            dst.registerMessage = src.message;
        }
    }

    CurrentSearchRegisterOptions MakeCurrentSearchRegisterOptions(
        ApplyTrigger trigger,
        std::size_t maxMatches)
    {
        CurrentSearchRegisterOptions opt{};

        opt.trigger = trigger;
        opt.reason = ApplyTriggerToReason(trigger);
        opt.maxMatches = maxMatches;

        // 自動予約ルールは録画予約専用です。
        opt.includeCandidateRules = false;
        opt.includeAutoRules = true;

        opt.dryRun = false;
        opt.allowRegister = true;

        opt.skipDuplicate = true;
        opt.protectUserBlockedReservation = true;
        opt.maxRegisterCount = 0;

        opt.reserveMode = L"record";
        opt.reserveSource =
            MakeRegisterReserveSourceFromTrigger_(trigger);

        opt.forceCreatedReservationEnabled = true;
        opt.requiredTuners = 1;
        opt.preferredTunerIndex = -1;

        opt.refreshReservationDataJs = true;

        return opt;
    }

    CurrentSearchRegisterResult RunCurrentSearchAndRegisterRecordingTasks(
        TaskSystemConnector* connector,
        const EpgHtmlPortal::WriteOptions& portalOpt,
        const CurrentSearchRegisterOptions& options) noexcept
    {
        CurrentSearchRegisterResult result{};

        result.reason =
            options.reason.empty()
            ? ApplyTriggerToReason(options.trigger)
            : options.reason;

        try {
            result.searchRequest =
                MakeCurrentSearchMatchRequest(
                    options.trigger,
                    options.maxMatches);

            result.searchRequest.reason =
                result.reason;

            result.searchRequest.includeCandidateRules = false;

            result.searchRequest.includeAutoRules =
                options.includeAutoRules;

            if (portalOpt.outputDir.empty()) {
                result.ok = false;
                result.searchAttempted = false;
                result.searchOk = false;
                result.message = L"writeOptions.outputDir is empty";

                DebugLogFormat(
                    L"[AutoReserve][CURRENT-SEARCH-REGISTER][SKIP] reason=%ls message=%ls",
                    result.reason.c_str(),
                    result.message.c_str());

                return result;
            }

            result.searchAttempted = true;

            DebugLogFormat(
                L"[AutoReserve][CURRENT-SEARCH-REGISTER] search begin reason=%ls maxMatches=%zu",
                result.reason.c_str(),
                result.searchRequest.maxMatches);

            result.matchResult =
                EpgHtmlPortal_SearchData::RunAutoReserveMatchDryRunFromCurrentSearchData(
                    portalOpt,
                    result.searchRequest.maxMatches);

            result.searchOk =
                result.matchResult.ok;

            result.matchedCount =
                result.matchResult.matchedCount;

            result.candidateCount = 0;

            result.autoRegisterCount =
                result.matchResult.autoRegisterCount;

            result.searchSummary =
                SummarizeRuleMatchResultForRecording(
                    result.matchResult,
                    result.searchRequest);

            DebugLogFormat(
                L"[AutoReserve][CURRENT-SEARCH-REGISTER] search done reason=%ls ok=%d rules=%zu programs=%zu checked=%zu matched=%zu candidate=%zu auto=%zu shouldRegister=%d message=%ls",
                result.reason.c_str(),
                result.matchResult.ok ? 1 : 0,
                result.matchResult.ruleCount,
                result.matchResult.programCount,
                result.matchResult.checkedCount,
                result.matchResult.matchedCount,
                result.matchResult.candidateCount,
                result.matchResult.autoRegisterCount,
                result.searchSummary.shouldApplyToRecording ? 1 : 0,
                result.searchSummary.message.c_str());

            if (!result.matchResult.ok) {
                result.ok = false;
                result.message =
                    L"auto reserve search failed: " +
                    result.matchResult.message;

                return result;
            }

            if (!result.searchSummary.shouldApplyToRecording) {
                result.ok = true;
                result.message = result.searchSummary.message;

                DebugLogFormat(
                    L"[AutoReserve][CURRENT-SEARCH-REGISTER][SKIP] reason=%ls matched=%zu candidate=%zu auto=%zu message=%ls",
                    result.reason.c_str(),
                    result.matchedCount,
                    result.candidateCount,
                    result.autoRegisterCount,
                    result.message.c_str());

                return result;
            }

            if (connector == nullptr) {
                result.ok = false;
                result.message = L"TaskSystemConnector is null";

                DebugLogFormat(
                    L"[AutoReserve][CURRENT-SEARCH-REGISTER][SKIP] connector null reason=%ls matched=%zu",
                    result.reason.c_str(),
                    result.matchedCount);

                return result;
            }

            if (!connector->IsInitialized()) {
                result.ok = false;
                result.message = L"TaskSystemConnector is not initialized";

                DebugLogFormat(
                    L"[AutoReserve][CURRENT-SEARCH-REGISTER][SKIP] connector not initialized reason=%ls matched=%zu",
                    result.reason.c_str(),
                    result.matchedCount);

                return result;
            }

            TaskSystemConnector::AutoReserveRecordingApplyOptions recOpt{};

            recOpt.dryRun =
                options.dryRun;

            recOpt.allowRegister =
                options.allowRegister;

            recOpt.includeCandidateRules = false;

            recOpt.includeAutoRules =
                options.includeAutoRules;

            recOpt.skipDuplicate =
                options.skipDuplicate;

            recOpt.protectUserBlockedReservation =
                options.protectUserBlockedReservation;

            recOpt.maxRegisterCount =
                options.maxRegisterCount;

            recOpt.reserveMode =
                options.reserveMode.empty()
                ? std::wstring(L"record")
                : options.reserveMode;

            recOpt.reserveSource =
                options.reserveSource.empty()
                ? MakeRegisterReserveSourceFromTrigger_(options.trigger)
                : options.reserveSource;

            recOpt.forceCreatedReservationEnabled =
                options.forceCreatedReservationEnabled;

            recOpt.requiredTuners =
                options.requiredTuners == 0
                ? 1
                : options.requiredTuners;

            recOpt.preferredTunerIndex =
                options.preferredTunerIndex;

            result.registrationAttempted = true;

            const TaskSystemConnector::AutoReserveRecordingApplyResult reg =
                connector->RegisterAutoReserveMatchesAsRecording(
                    result.matchResult,
                    recOpt);

            CopyAutoReserveRegisterResult_(
                result,
                reg);

            result.reservationJsOk = true;

            if (options.refreshReservationDataJs &&
                reg.registered != 0 &&
                !portalOpt.outputDir.empty()) {
                result.reservationJsOk =
                    EpgHtmlPortal_ReservationData::WriteReservationDataJs(
                        portalOpt,
                        *connector);
            }

            result.ok =
                reg.ok &&
                result.reservationJsOk;

            result.message =
                result.ok
                ? std::wstring(L"registered")
                : std::wstring(L"register failed: ") + reg.message;

            DebugLogFormat(
                L"[AutoReserve][CURRENT-SEARCH-REGISTER] register done reason=%ls ok=%d searchOk=%d input=%zu target=%zu registered=%zu dryRun=%zu dup=%zu userBlocked=%zu invalid=%zu limit=%zu action=%zu failed=%zu reservationJs=%d message=%ls",
                result.reason.c_str(),
                result.ok ? 1 : 0,
                result.searchOk ? 1 : 0,
                result.registerInput,
                result.registerTarget,
                result.registered,
                result.dryRunSkipped,
                result.duplicateSkipped,
                result.userBlockedSkipped,
                result.invalidSkipped,
                result.limitSkipped,
                result.actionSkipped,
                result.failed,
                result.reservationJsOk ? 1 : 0,
                result.message.c_str());

            return result;
        }
        catch (const std::bad_alloc&) {
            result.ok = false;
            result.message = L"bad_alloc";

            DebugLogFormat(
                L"[AutoReserve][CURRENT-SEARCH-REGISTER][ERROR] bad_alloc reason=%ls",
                result.reason.c_str());
        }
        catch (const std::exception& e) {
            result.ok = false;
            result.message = L"exception";

            DebugLogFormat(
                L"[AutoReserve][CURRENT-SEARCH-REGISTER][ERROR] exception reason=%ls what=%S",
                result.reason.c_str(),
                e.what());
        }
        catch (...) {
            result.ok = false;
            result.message = L"unknown exception";

            DebugLogFormat(
                L"[AutoReserve][CURRENT-SEARCH-REGISTER][ERROR] unknown reason=%ls",
                result.reason.c_str());
        }

        return result;
    }








    namespace
    {
        static std::wstring TrimMatchText_(
            std::wstring text)
        {
            auto isWs = [](wchar_t ch) noexcept {
                return ch == L' ' ||
                    ch == L'\t' ||
                    ch == L'\r' ||
                    ch == L'\n' ||
                    ch == L'\u3000';
                };

            while (!text.empty() && isWs(text.front())) {
                text.erase(text.begin());
            }

            while (!text.empty() && isWs(text.back())) {
                text.pop_back();
            }

            return text;
        }

        static std::wstring ToLowerAsciiForMatch_(
            std::wstring text)
        {
            for (wchar_t& ch : text) {
                if (ch >= L'A' && ch <= L'Z') {
                    ch = static_cast<wchar_t>(ch - L'A' + L'a');
                }
            }

            return text;
        }

        static std::vector<std::wstring> SplitRuleTokens_(
            const std::wstring& text)
        {
            std::vector<std::wstring> out;

            std::size_t pos = 0;
            while (pos <= text.size()) {
                const std::size_t next = text.find(L'|', pos);

                std::wstring token =
                    next == std::wstring::npos
                    ? text.substr(pos)
                    : text.substr(pos, next - pos);

                token = TrimMatchText_(std::move(token));

                if (!token.empty()) {
                    out.push_back(std::move(token));
                }

                if (next == std::wstring::npos) {
                    break;
                }

                pos = next + 1;
            }

            return out;
        }

        static bool ContainsTextLoose_(
            const std::wstring& text,
            const std::wstring& token)
        {
            if (token.empty()) {
                return true;
            }

            const std::wstring a =
                ToLowerAsciiForMatch_(text);

            const std::wstring b =
                ToLowerAsciiForMatch_(token);

            return a.find(b) != std::wstring::npos;
        }

        static bool MatchOneTextByMode_(
            const std::wstring& text,
            const std::wstring& token,
            MatchMode mode)
        {
            if (token.empty()) {
                return true;
            }

            const std::wstring a =
                ToLowerAsciiForMatch_(TrimMatchText_(text));

            const std::wstring b =
                ToLowerAsciiForMatch_(TrimMatchText_(token));

            if (b.empty()) {
                return true;
            }

            switch (mode) {
            case MatchMode::Exact:
                return a == b;

            case MatchMode::StartsWith:
                return a.rfind(b, 0) == 0;

            case MatchMode::EndsWith:
                if (a.size() < b.size()) {
                    return false;
                }
                return a.compare(a.size() - b.size(), b.size(), b) == 0;

            case MatchMode::Contains:
            default:
                return a.find(b) != std::wstring::npos;
            }
        }

        static bool MatchTokenList_(
            const std::wstring& text,
            const std::vector<std::wstring>& tokens,
            TokenJoinMode joinMode)
        {
            if (tokens.empty()) {
                return true;
            }

            if (joinMode == TokenJoinMode::And) {
                for (const auto& token : tokens) {
                    if (!ContainsTextLoose_(text, token)) {
                        return false;
                    }
                }

                return true;
            }

            for (const auto& token : tokens) {
                if (ContainsTextLoose_(text, token)) {
                    return true;
                }
            }

            return false;
        }

        static bool MatchKeywordToken_(
            const Rule& rule,
            const SearchProgram& program,
            const std::wstring& token)
        {
            if (rule.matchTitle &&
                MatchOneTextByMode_(program.title, token, rule.titleMatchMode)) {
                return true;
            }

            if (rule.matchSummary &&
                MatchOneTextByMode_(program.summary, token, rule.titleMatchMode)) {
                return true;
            }

            if (rule.matchExtended &&
                MatchOneTextByMode_(program.extendedText, token, rule.titleMatchMode)) {
                return true;
            }

            return false;
        }

        static bool MatchKeyword_(
            const Rule& rule,
            const SearchProgram& program)
        {
            if (!rule.useKeyword) {
                return true;
            }

            const std::vector<std::wstring> tokens =
                SplitRuleTokens_(rule.keyword);

            if (tokens.empty()) {
                return false;
            }

            if (rule.keywordJoinMode == TokenJoinMode::And) {
                for (const auto& token : tokens) {
                    if (!MatchKeywordToken_(rule, program, token)) {
                        return false;
                    }
                }

                return true;
            }

            for (const auto& token : tokens) {
                if (MatchKeywordToken_(rule, program, token)) {
                    return true;
                }
            }

            return false;
        }

        static bool MatchExcludeKeyword_(
            const Rule& rule,
            const SearchProgram& program)
        {
            if (!rule.useExcludeKeyword) {
                return true;
            }

            const std::vector<std::wstring> tokens =
                SplitRuleTokens_(rule.excludeKeyword);

            if (tokens.empty()) {
                return true;
            }

            std::wstring target;
            target.reserve(
                program.title.size() +
                program.summary.size() +
                program.extendedText.size() +
                4);

            target += program.title;
            target += L"\n";
            target += program.summary;
            target += L"\n";
            target += program.extendedText;

            const bool hit =
                MatchTokenList_(
                    target,
                    tokens,
                    rule.excludeKeywordJoinMode);

            return !hit;
        }

        static bool MatchService_(
            const Rule& rule,
            const SearchProgram& program)
        {
            if (!rule.useService) {
                return true;
            }

            const std::vector<std::wstring> tokens =
                SplitRuleTokens_(rule.serviceName);

            if (tokens.empty()) {
                return false;
            }

            return MatchTokenList_(
                program.serviceName,
                tokens,
                rule.serviceMatchMode);
        }

        static std::wstring NormalizeGenreTextForMatch_(
            std::wstring text)
        {
            text = TrimMatchText_(std::move(text));

            constexpr std::wstring_view kSeparators =
                L"\uFF0F/\u30FB\uFF65-\uFF0D\u30FC";

            for (wchar_t& ch : text) {
                if (kSeparators.find(ch) != std::wstring_view::npos) {
                    ch = L' ';
                }
            }

            return ToLowerAsciiForMatch_(std::move(text));
        }

        static bool ContainsGenreLoose_(
            const std::wstring& text,
            const std::wstring& token)
        {
            const std::wstring a =
                NormalizeGenreTextForMatch_(text);

            const std::wstring b =
                NormalizeGenreTextForMatch_(token);

            if (b.empty()) {
                return true;
            }

            return a.find(b) != std::wstring::npos;
        }

        static bool MatchGenreTokenList_(
            const std::wstring& text,
            const std::vector<std::wstring>& tokens,
            TokenJoinMode joinMode)
        {
            if (tokens.empty()) {
                return true;
            }

            if (joinMode == TokenJoinMode::And) {
                for (const auto& token : tokens) {
                    if (!ContainsGenreLoose_(text, token)) {
                        return false;
                    }
                }

                return true;
            }

            for (const auto& token : tokens) {
                if (ContainsGenreLoose_(text, token)) {
                    return true;
                }
            }

            return false;
        }

        static bool MatchGenre_(
            const Rule& rule,
            const SearchProgram& program)
        {
            if (!rule.useGenre) {
                return true;
            }

            const std::vector<std::wstring> tokens =
                SplitRuleTokens_(rule.genreName);

            if (tokens.empty()) {
                return false;
            }

            return MatchGenreTokenList_(
                program.genreName,
                tokens,
                rule.genreMatchMode);
        }

        static bool MatchDuration_(
            const Rule& rule,
            const SearchProgram& program)
        {
            if (!rule.useMinDuration) {
                return true;
            }

            if (program.durationSec == 0) {
                return false;
            }

            const int minutes =
                static_cast<int>(program.durationSec / 60u);

            return minutes >= rule.minDurationMinutes;
        }

        static bool MatchTimeRange_(
            const Rule& rule,
            const SearchProgram& program)
        {
            if (!rule.useSearchTimeRange) {
                return true;
            }

            const int m = program.startMinutesOfDay;
            if (m < 0) {
                return false;
            }

            const int begin = rule.searchStartMinutes;
            const int end = rule.searchEndMinutes;

            if (!rule.searchTimeWrap) {
                return m >= begin && m < end;
            }

            return m >= begin || m < end;
        }

        static int WeekdayIndexFromText_(
            const std::wstring& text)
        {
            if (text.find(L"\u6708") != std::wstring::npos) return 0;
            if (text.find(L"\u706B") != std::wstring::npos) return 1;
            if (text.find(L"\u6C34") != std::wstring::npos) return 2;
            if (text.find(L"\u6728") != std::wstring::npos) return 3;
            if (text.find(L"\u91D1") != std::wstring::npos) return 4;
            if (text.find(L"\u571F") != std::wstring::npos) return 5;
            if (text.find(L"\u65E5") != std::wstring::npos) return 6;

            return -1;
        }

        static bool MatchWeekday_(
            const Rule& rule,
            const SearchProgram& program)
        {
            if (!rule.useWeekday) {
                return true;
            }

            int index = program.weekdayIndex;

            if (index < 0) {
                index = WeekdayIndexFromText_(program.weekdayText);
            }

            if (index < 0 || index >= 7) {
                return false;
            }

            const std::wstring bits =
                rule.weekdays.empty()
                ? std::wstring(L"1111111")
                : rule.weekdays;

            if (static_cast<std::size_t>(index) >= bits.size()) {
                return false;
            }

            return bits[static_cast<std::size_t>(index)] != L'0';
        }

        static bool IsRerunLikeProgram_(
            const SearchProgram& program)
        {
            std::wstring text;
            text.reserve(
                program.title.size() +
                program.summary.size() +
                program.extendedText.size() +
                4);

            text += program.title;
            text += L"\n";
            text += program.summary;
            text += L"\n";
            text += program.extendedText;

            return text.find(L"[\u518D]") != std::wstring::npos ||
                text.find(L"\uFF3B\u518D\uFF3D") != std::wstring::npos ||
                text.find(L"\u3010\u518D\u3011") != std::wstring::npos ||
                text.find(L"\uFF08\u518D\uFF09") != std::wstring::npos ||
                text.find(L"(\u518D)") != std::wstring::npos ||
                text.find(L"\u518D\u653E\u9001") != std::wstring::npos;
        }
    }

    bool IsProgramMatchedByRule(
        const Rule& rule,
        const SearchProgram& program,
        std::wstring* outReason)
    {
        if (!rule.enabled) {
            if (outReason) {
                *outReason = L"rule disabled";
            }
            return false;
        }

        if (rule.targetSpace != -1 &&
            rule.targetSpace != program.space) {
            if (outReason) {
                *outReason = L"space mismatch";
            }
            return false;
        }

        if (!MatchKeyword_(rule, program)) {
            if (outReason) {
                *outReason = L"keyword mismatch";
            }
            return false;
        }

        if (!MatchExcludeKeyword_(rule, program)) {
            if (outReason) {
                *outReason = L"exclude keyword hit";
            }
            return false;
        }

        if (!MatchService_(rule, program)) {
            if (outReason) {
                *outReason = L"service mismatch";
            }
            return false;
        }

        if (!MatchGenre_(rule, program)) {
            if (outReason) {
                *outReason = L"genre mismatch";
            }
            return false;
        }

        if (!MatchDuration_(rule, program)) {
            if (outReason) {
                *outReason = L"duration mismatch";
            }
            return false;
        }

        if (!MatchTimeRange_(rule, program)) {
            if (outReason) {
                *outReason = L"time range mismatch";
            }
            return false;
        }

        if (!MatchWeekday_(rule, program)) {
            if (outReason) {
                *outReason = L"weekday mismatch";
            }
            return false;
        }

        if (rule.excludeRerunLikeProgram &&
            IsRerunLikeProgram_(program)) {
            if (outReason) {
                *outReason = L"rerun-like program";
            }
            return false;
        }

        if (outReason) {
            *outReason = L"matched";
        }

        return true;
    }

    RuleMatchResult FindMatchedPrograms(
        const std::vector<Rule>& rules,
        const std::vector<SearchProgram>& programs,
        const RuleMatchOptions& options)
    {
        RuleMatchResult result{};
        result.ok = true;
        result.ruleCount = rules.size();
        result.programCount = programs.size();

        for (const auto& rule : rules) {
            if (!rule.enabled) {
                ++result.skippedDisabledRule;
                continue;
            }

            if (!options.includeAutoRules) {
                ++result.skippedActionMode;
                continue;
            }

            for (const auto& program : programs) {
                ++result.checkedCount;

                std::wstring reason;
                if (!IsProgramMatchedByRule(rule, program, &reason)) {
                    continue;
                }

                RuleProgramMatch m{};
                m.ruleId = rule.id;
                m.ruleName = rule.name;
                m.autoRegister = true;
                m.rule = rule;
                m.program = program;
                m.reason = reason;

                result.matches.push_back(std::move(m));
                ++result.matchedCount;

                ++result.autoRegisterCount;

                if (options.maxMatches > 0 &&
                    result.matches.size() >= options.maxMatches) {
                    result.message = L"max matches reached";
                    return result;
                }
            }
        }

        result.message = L"matched";
        return result;
    }


}// namespace AutoReserve
