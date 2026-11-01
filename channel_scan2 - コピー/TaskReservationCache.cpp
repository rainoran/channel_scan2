#include "TaskReservationCache.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <ctime>
#include <set>

namespace
{
    struct SortLine
    {
        std::int64_t sortKey = 0;
        std::wstring line;
    };

    std::filesystem::path GetExeDirectory_()
    {
        wchar_t path[MAX_PATH]{};
        const DWORD len = ::GetModuleFileNameW(nullptr, path, static_cast<DWORD>(_countof(path)));

        if (len == 0 || len >= _countof(path)) {
            return std::filesystem::current_path();
        }

        return std::filesystem::path(path).parent_path();
    }

    std::wstring BoolText_(bool v)
    {
        return v ? L"1" : L"0";
    }

    std::wstring EscapeField_(std::wstring_view src)
    {
        std::wstring out;
        out.reserve(src.size());

        for (const wchar_t ch : src) {
            switch (ch) {
            case L'\\':
                out += L"\\\\";
                break;
            case L'\t':
                out += L"\\t";
                break;
            case L'\r':
                out += L"\\r";
                break;
            case L'\n':
                out += L"\\n";
                break;
            default:
                out.push_back(ch);
                break;
            }
        }

        return out;
    }

    void AppendField_(std::wstring& line, std::wstring_view value)
    {
        if (!line.empty()) {
            line.push_back(L'\t');
        }
        line += EscapeField_(value);
    }

    void AppendIntField_(std::wstring& line, int value)
    {
        AppendField_(line, std::to_wstring(value));
    }

    void AppendUIntField_(std::wstring& line, std::uint32_t value)
    {
        AppendField_(line, std::to_wstring(value));
    }

    void AppendBoolField_(std::wstring& line, bool value)
    {
        AppendField_(line, BoolText_(value));
    }

    std::wstring JoinTab_(std::initializer_list<std::wstring_view> fields)
    {
        std::wstring line;

        for (const auto field : fields) {
            AppendField_(line, field);
        }

        return line;
    }

    bool WideToUtf8_(std::wstring_view src, std::string& out) noexcept
    {
        out.clear();

        if (src.empty()) {
            return true;
        }

        const int needed = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            src.data(),
            static_cast<int>(src.size()),
            nullptr,
            0,
            nullptr,
            nullptr);

        if (needed <= 0) {
            return false;
        }

        out.resize(static_cast<std::size_t>(needed));

        const int written = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            src.data(),
            static_cast<int>(src.size()),
            out.data(),
            needed,
            nullptr,
            nullptr);

        return written == needed;
    }

    bool WriteAllBytes_(HANDLE hFile, const void* data, std::size_t size)
    {
        const auto* p = static_cast<const unsigned char*>(data);
        std::size_t remain = size;

        while (remain > 0) {
            const DWORD chunk = static_cast<DWORD>(
                (remain > 1024U * 1024U) ? (1024U * 1024U) : remain);

            DWORD written = 0;
            const BOOL ok = ::WriteFile(hFile, p, chunk, &written, nullptr);

            if (!ok || written == 0) {
                return false;
            }

            p += written;
            remain -= written;
        }

        return true;
    }

    bool WriteUtf8LineToHandle_(HANDLE hFile, std::wstring_view line)
    {
        std::wstring withNewLine(line);
        withNewLine += L"\r\n";

        std::string utf8;
        if (!WideToUtf8_(withNewLine, utf8)) {
            return false;
        }

        return WriteAllBytes_(hFile, utf8.data(), utf8.size());
    }

    std::wstring MakeCacheMutexName_(const std::wstring& path)
    {
        std::uint32_t h = 2166136261u;

        for (const wchar_t ch : path) {
            h ^= static_cast<std::uint32_t>(ch);
            h *= 16777619u;
        }

        wchar_t buf[96]{};
        swprintf_s(buf, L"Local\\TSCEPG_ReservationCache_%08X", h);
        return std::wstring(buf);
    }

    bool WaitNamedCacheMutex_(const std::wstring& cachePath, HANDLE& outMutex) noexcept
    {
        outMutex = nullptr;

        const std::wstring name = MakeCacheMutexName_(cachePath);

        HANDLE h = ::CreateMutexW(nullptr, FALSE, name.c_str());
        if (!h) {
            return false;
        }

        const DWORD wait = ::WaitForSingleObject(h, 3000);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            outMutex = h;
            return true;
        }

        ::CloseHandle(h);
        return false;
    }

    void ReleaseNamedCacheMutex_(HANDLE h) noexcept
    {
        if (h) {
            ::ReleaseMutex(h);
            ::CloseHandle(h);
        }
    }

    std::wstring MakeTempCachePath_(const std::wstring& cachePath)
    {
        // キャッシュは高速参照用の固定インデックスなので、
        // 作業中ファイルも固定名にする。
        // 同一 path には WaitNamedCacheMutex_() が掛かるため、
        // PID / Tick 付きの増殖ファイルは不要。
        return cachePath + L".writing";
    }

    void CleanupOldTempCacheFiles_(const std::wstring& cachePath) noexcept
    {
        try {
            if (cachePath.empty()) {
                return;
            }

            const std::filesystem::path target(cachePath);
            const std::filesystem::path parent = target.parent_path();

            if (parent.empty()) {
                return;
            }

            std::error_code ec;
            if (!std::filesystem::exists(parent, ec) || ec) {
                return;
            }

            const std::wstring baseName = target.filename().wstring();

            for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
                if (ec) {
                    return;
                }

                std::error_code itemEc;
                if (!entry.is_regular_file(itemEc) || itemEc) {
                    continue;
                }

                const std::wstring name = entry.path().filename().wstring();

                const bool oldDynamicTemp =
                    name.rfind(baseName + L".tmp.", 0) == 0 &&
                    name.find(L".writing") != std::wstring::npos;

                const bool fixedWriting =
                    name == baseName + L".writing";

                if (oldDynamicTemp || fixedWriting) {
                    std::error_code removeEc;
                    std::filesystem::remove(entry.path(), removeEc);
                }
            }
        }
        catch (...) {
        }
    }

    bool ReplaceCacheFileAtomic_(const std::wstring& tempPath, const std::wstring& cachePath)
    {
        constexpr DWORD flags =
            MOVEFILE_REPLACE_EXISTING |
            MOVEFILE_WRITE_THROUGH;

        for (int i = 0; i < 10; ++i) {
            if (::MoveFileExW(tempPath.c_str(), cachePath.c_str(), flags)) {
                return true;
            }

            const DWORD err = ::GetLastError();

            if (err != ERROR_SHARING_VIOLATION &&
                err != ERROR_ACCESS_DENIED &&
                err != ERROR_LOCK_VIOLATION) {
                break;
            }

            ::Sleep(10);
        }

        return false;
    }

    TaskReservationCache::WriteResult WriteLinesAtomic_(
        const std::wstring& path,
        const std::wstring& versionLine,
        const std::wstring& headerLine,
        const std::vector<std::wstring>& bodyLines)
    {
        TaskReservationCache::WriteResult result{};
        result.ok = false;

        if (path.empty()) {
            result.message = L"path is empty";
            return result;
        }

        HANDLE mutexHandle = nullptr;
        if (!WaitNamedCacheMutex_(path, mutexHandle)) {
            result.message = L"failed to lock cache mutex";
            return result;
        }

        CleanupOldTempCacheFiles_(path);

        const std::wstring tempPath = MakeTempCachePath_(path);

        std::error_code ec;
        const std::filesystem::path cacheFilePath(path);
        const auto parent = cacheFilePath.parent_path();

        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                ReleaseNamedCacheMutex_(mutexHandle);
                result.message = L"failed to create cache directory";
                return result;
            }
        }

        HANDLE hFile = ::CreateFileW(
            tempPath.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
            nullptr);

        if (hFile == INVALID_HANDLE_VALUE) {
            ReleaseNamedCacheMutex_(mutexHandle);
            result.message = L"failed to open temp cache file";
            return result;
        }

        bool ok = true;

        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        ok = ok && WriteAllBytes_(hFile, bom, sizeof(bom));
        ok = ok && WriteUtf8LineToHandle_(hFile, versionLine);
        ok = ok && WriteUtf8LineToHandle_(hFile, headerLine);

        for (const auto& line : bodyLines) {
            if (!WriteUtf8LineToHandle_(hFile, line)) {
                ok = false;
                break;
            }
        }

        if (ok) {
            ok = (::FlushFileBuffers(hFile) != 0);
        }

        ::CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;

        if (!ok) {
            ::DeleteFileW(tempPath.c_str());
            ReleaseNamedCacheMutex_(mutexHandle);
            result.message = L"failed to write temp cache file";
            return result;
        }

        if (!ReplaceCacheFileAtomic_(tempPath, path)) {
            ::DeleteFileW(tempPath.c_str());
            ReleaseNamedCacheMutex_(mutexHandle);
            result.message = L"failed to replace cache file";
            return result;
        }

        CleanupOldTempCacheFiles_(path);

        ReleaseNamedCacheMutex_(mutexHandle);

        result.ok = true;
        result.message = L"ok";
        return result;
    }

    bool IsProgramTaskKind_(std::uint32_t kind) noexcept
    {
        return kind == TaskKinds::RecPrepare ||
            kind == TaskKinds::RecStart ||
            kind == TaskKinds::RecStop;
    }

    bool IsDisplayRepresentative_(const TaskListManager::TaskEntry& t) noexcept
    {
        if (IsProgramTaskKind_(t.request.kind)) {
            return t.request.kind == TaskKinds::RecStart;
        }

        return true;
    }

    bool IsServiceViewTarget_(const TaskListManager::TaskEntry& t) noexcept
    {
        if (t.request.kind != TaskKinds::RecStart) {
            return false;
        }

        if (t.reserve.title.empty() && t.reserve.titleLabel.empty()) {
            return false;
        }

        if (t.reserve.serviceId == 0 && t.reserve.serviceName.empty()) {
            return false;
        }

        return true;
    }

    std::wstring NormalizeMode_(const TaskListManager::TaskEntry& t)
    {
        if (!t.reserve.reserveMode.empty()) {
            return t.reserve.reserveMode;
        }

        switch (t.request.kind) {
        case TaskKinds::EpgFull:
            return L"epg_full";
        case TaskKinds::EpgLight:
            return L"epg_light";
        case TaskKinds::RecPrepare:
        case TaskKinds::RecStart:
        case TaskKinds::RecStop:
            return L"program";
        default:
            return L"task";
        }
    }

    std::wstring MakeStatus_(const TaskListManager::TaskEntry& t)
    {
        if (!t.enabled || t.reserve.disabledByUser) {
            return L"disabled";
        }

        if (t.reserve.excludeFromAuto) {
            return L"excluded";
        }

        return L"reserved";
    }

    std::wstring MakeTitle_(const TaskListManager::TaskEntry& t)
    {
        if (!t.reserve.titleLabel.empty()) {
            return t.reserve.titleLabel;
        }

        if (!t.reserve.title.empty()) {
            return t.reserve.title;
        }

        if (!t.display.label.empty()) {
            return t.display.label;
        }

        return t.name;
    }

    std::wstring MakeServiceDirName_(const TaskListManager::TaskEntry& t)
    {
        std::wstring dir;
        dir.reserve(64);

        dir += L"nid";
        dir += std::to_wstring(static_cast<unsigned>(t.reserve.networkId));
        dir += L"_tsid";
        dir += std::to_wstring(static_cast<unsigned>(t.reserve.tsId));
        dir += L"_sid";
        dir += std::to_wstring(static_cast<unsigned>(t.reserve.serviceId));

        return dir;
    }

    std::wstring MakeReservationKey_(const TaskListManager::TaskEntry& t)
    {
        std::wstring key;
        key.reserve(128);

        key += NormalizeMode_(t);
        key += L"|";
        key += std::to_wstring(static_cast<unsigned>(t.reserve.networkId));
        key += L"|";
        key += std::to_wstring(static_cast<unsigned>(t.reserve.tsId));
        key += L"|";
        key += std::to_wstring(static_cast<unsigned>(t.reserve.serviceId));
        key += L"|";
        key += std::to_wstring(static_cast<unsigned>(t.reserve.eventId));
        key += L"|";
        key += t.reserve.startDate;
        key += L"|";
        key += t.reserve.startTime;

        return key;
    }

    std::wstring MakeFallbackKey_(const TaskListManager::TaskEntry& t)
    {
        std::wstring key;
        key.reserve(160);

        key += NormalizeMode_(t);
        key += L"|";
        key += std::to_wstring(static_cast<unsigned>(t.reserve.serviceId));
        key += L"|";
        key += MakeTitle_(t);
        key += L"|";
        key += t.reserve.startDate;
        key += L"|";
        key += t.reserve.startTime;
        key += L"|";
        key += std::to_wstring(t.reserve.durationSec);

        return key;
    }

    std::wstring MakeRunKey_(const TaskListManager::TaskEntry& t)
    {
        std::wstring key;
        key.reserve(96);

        key += std::to_wstring(t.request.kind);
        key += L"|";
        key += std::to_wstring(t.request.instance);
        key += L"|";
        key += std::to_wstring(t.request.dispatchYmd);
        key += L"|";
        key += std::to_wstring(t.request.dispatchHHMM);
        key += L"|";
        key += std::to_wstring(t.request.dispatchSecond);

        return key;
    }

    int ParseYmd_(const std::wstring& date) noexcept
    {
        if (date.size() < 10) {
            return 0;
        }

        const int y = _wtoi(date.substr(0, 4).c_str());
        const int m = _wtoi(date.substr(5, 2).c_str());
        const int d = _wtoi(date.substr(8, 2).c_str());

        if (y <= 0 || m <= 0 || d <= 0) {
            return 0;
        }

        return y * 10000 + m * 100 + d;
    }

    int ParseHHMM_(const std::wstring& time) noexcept
    {
        if (time.size() < 5) {
            return 0;
        }

        const int h = _wtoi(time.substr(0, 2).c_str());
        const int m = _wtoi(time.substr(3, 2).c_str());

        return h * 100 + m;
    }

    int ParseSecond_(const std::wstring& time) noexcept
    {
        if (time.size() < 8) {
            return 0;
        }

        return _wtoi(time.substr(6, 2).c_str());
    }

    static std::wstring FormatIsoDateFromYmd_(int ymd)
    {
        if (ymd <= 0) {
            return {};
        }

        const int y = ymd / 10000;
        const int m = (ymd / 100) % 100;
        const int d = ymd % 100;

        if (y <= 0 || m <= 0 || d <= 0) {
            return {};
        }

        wchar_t buf[32]{};
        swprintf_s(buf, L"%04d-%02d-%02d", y, m, d);
        return std::wstring(buf);
    }

    static std::wstring FormatIsoTimeFromHHMMSS_(int hhmm, int sec)
    {
        if (hhmm < 0) {
            return {};
        }

        const int h = hhmm / 100;
        const int m = hhmm % 100;

        if (h < 0 || h > 23 || m < 0 || m > 59) {
            return {};
        }

        if (sec < 0 || sec > 59) {
            sec = 0;
        }

        wchar_t buf[32]{};
        swprintf_s(buf, L"%02d:%02d:%02d", h, m, sec);
        return std::wstring(buf);
    }

    static bool ParseIsoDateTime_(
        const std::wstring& date,
        const std::wstring& time,
        std::tm& outTm)
    {
        if (date.size() < 10 || time.size() < 5) {
            return false;
        }

        int y = 0;
        int mo = 0;
        int d = 0;
        int h = 0;
        int mi = 0;
        int s = 0;

        if (swscanf_s(date.c_str(), L"%d-%d-%d", &y, &mo, &d) != 3) {
            return false;
        }

        if (swscanf_s(time.c_str(), L"%d:%d:%d", &h, &mi, &s) < 2) {
            return false;
        }

        if (y <= 0 || mo <= 0 || d <= 0 ||
            h < 0 || h > 23 ||
            mi < 0 || mi > 59 ||
            s < 0 || s > 59) {
            return false;
        }

        outTm = {};
        outTm.tm_year = y - 1900;
        outTm.tm_mon = mo - 1;
        outTm.tm_mday = d;
        outTm.tm_hour = h;
        outTm.tm_min = mi;
        outTm.tm_sec = s;
        outTm.tm_isdst = -1;

        return true;
    }

    static std::wstring FormatIsoDateFromTm_(const std::tm& tmv)
    {
        wchar_t buf[32]{};
        swprintf_s(
            buf,
            L"%04d-%02d-%02d",
            tmv.tm_year + 1900,
            tmv.tm_mon + 1,
            tmv.tm_mday);
        return std::wstring(buf);
    }

    static std::wstring FormatIsoTimeFromTm_(const std::tm& tmv)
    {
        wchar_t buf[32]{};
        swprintf_s(
            buf,
            L"%02d:%02d:%02d",
            tmv.tm_hour,
            tmv.tm_min,
            tmv.tm_sec);
        return std::wstring(buf);
    }

    static bool AddSecondsToIsoDateTime_(
        const std::wstring& startDate,
        const std::wstring& startTime,
        std::uint32_t durationSec,
        std::wstring& outEndDate,
        std::wstring& outEndTime)
    {
        if (durationSec == 0) {
            return false;
        }

        std::tm tmv{};
        if (!ParseIsoDateTime_(startDate, startTime, tmv)) {
            return false;
        }

        std::time_t t = std::mktime(&tmv);
        if (t == static_cast<std::time_t>(-1)) {
            return false;
        }

        t += static_cast<std::time_t>(durationSec);

#ifdef _WIN32
        if (localtime_s(&tmv, &t) != 0) {
            return false;
        }
#else
        const std::tm* p = std::localtime(&t);
        if (!p) {
            return false;
        }
        tmv = *p;
#endif

        outEndDate = FormatIsoDateFromTm_(tmv);
        outEndTime = FormatIsoTimeFromTm_(tmv);
        return true;
    }

    static bool IsEndBeforeStart_(
        const std::wstring& startDate,
        const std::wstring& startTime,
        const std::wstring& endDate,
        const std::wstring& endTime)
    {
        std::tm st{};
        std::tm en{};

        if (!ParseIsoDateTime_(startDate, startTime, st) ||
            !ParseIsoDateTime_(endDate, endTime, en)) {
            return false;
        }

        const std::time_t ts = std::mktime(&st);
        const std::time_t te = std::mktime(&en);

        if (ts == static_cast<std::time_t>(-1) ||
            te == static_cast<std::time_t>(-1)) {
            return false;
        }

        return te < ts;
    }

    static bool IsProgramStartTask_(const TaskListManager::TaskEntry& t) noexcept
    {
        return t.request.kind == TaskKinds::RecStart;
    }

    static int EffectiveStartYmd_(const TaskListManager::TaskEntry& t) noexcept
    {
        if (t.request.dispatchYmd > 0) {
            return t.request.dispatchYmd;
        }

        if (t.schedule.dueYmd > 0) {
            return t.schedule.dueYmd;
        }

        return ParseYmd_(t.reserve.startDate);
    }

    static int EffectiveStartHHMM_(const TaskListManager::TaskEntry& t) noexcept
    {
        if (t.request.dispatchHHMM > 0) {
            return t.request.dispatchHHMM;
        }

        if (t.schedule.dueHHMM > 0) {
            return t.schedule.dueHHMM;
        }

        return ParseHHMM_(t.reserve.startTime);
    }

    static int EffectiveStartSecond_(const TaskListManager::TaskEntry& t) noexcept
    {
        if (t.request.dispatchSecond >= 0 && t.request.dispatchSecond <= 59) {
            return t.request.dispatchSecond;
        }

        return ParseSecond_(t.reserve.startTime);
    }

    static std::wstring MakeCanonicalProgramDedupeKey_(
        const TaskListManager::TaskEntry& t,
        const std::wstring& startDate,
        const std::wstring& startTime)
    {
        std::wstring key;
        key.reserve(160);

        key += std::to_wstring(t.reserve.space);
        key += L"-";
        key += std::to_wstring(static_cast<unsigned>(t.reserve.networkId));
        key += L"-";
        key += std::to_wstring(static_cast<unsigned>(t.reserve.tsId));
        key += L"-";
        key += std::to_wstring(static_cast<unsigned>(t.reserve.serviceId));
        key += L"-";
        key += std::to_wstring(static_cast<unsigned>(t.reserve.eventId));
        key += L"-";
        key += startDate;
        key += L"-";
        key += startTime;
        key += L"-";
        key += NormalizeMode_(t);

        return key;
    }

    static TaskListManager::TaskEntry MakeCacheEntry_(
        const TaskListManager::TaskEntry& src)
    {
        TaskListManager::TaskEntry t = src;

        if (!IsProgramTaskKind_(t.request.kind)) {
            return t;
        }

        // 表示系キャッシュは RecStart を代表行として扱う。
        // RecStart の実行日時を正として reserve の日時ズレを補正する。
        const int startYmd = EffectiveStartYmd_(t);
        const int startHHMM = EffectiveStartHHMM_(t);
        const int startSecond = EffectiveStartSecond_(t);

        const std::wstring effectiveStartDate = FormatIsoDateFromYmd_(startYmd);
        const std::wstring effectiveStartTime =
            FormatIsoTimeFromHHMMSS_(startHHMM, startSecond);

        if (!effectiveStartDate.empty()) {
            t.reserve.startDate = effectiveStartDate;
            t.reserve.dateText = effectiveStartDate;
        }

        if (!effectiveStartTime.empty()) {
            t.reserve.startTime = effectiveStartTime;
        }

        // endDate が startDate より前、または空の場合は durationSec から再計算する。
        if (t.reserve.durationSec > 0 &&
            !t.reserve.startDate.empty() &&
            !t.reserve.startTime.empty()) {
            std::wstring endDate;
            std::wstring endTime;

            if (AddSecondsToIsoDateTime_(
                t.reserve.startDate,
                t.reserve.startTime,
                t.reserve.durationSec,
                endDate,
                endTime)) {
                if (t.reserve.endDate.empty() ||
                    t.reserve.endTime.empty() ||
                    IsEndBeforeStart_(
                        t.reserve.startDate,
                        t.reserve.startTime,
                        t.reserve.endDate,
                        t.reserve.endTime)) {
                    t.reserve.endDate = endDate;
                    t.reserve.endTime = endTime;
                }

                const std::wstring shhmm =
                    t.reserve.startTime.size() >= 5
                    ? t.reserve.startTime.substr(0, 5)
                    : t.reserve.startTime;

                const std::wstring ehhmm =
                    t.reserve.endTime.size() >= 5
                    ? t.reserve.endTime.substr(0, 5)
                    : t.reserve.endTime;

                if (!shhmm.empty() && !ehhmm.empty()) {
                    t.reserve.timeText = shhmm + L"-" + ehhmm;
                    t.display.note = t.reserve.timeText +
                        L" (" +
                        std::to_wstring(t.reserve.durationSec / 60) +
                        L"m)";
                }

                t.display.endYmd = ParseYmd_(t.reserve.endDate);
                t.display.endHHMM = ParseHHMM_(t.reserve.endTime);
            }
        }

        // 表示用キャッシュでは実行日ベースの canonical key を使う。
        if (IsProgramStartTask_(t) &&
            !t.reserve.startDate.empty() &&
            !t.reserve.startTime.empty()) {
            const std::wstring key =
                MakeCanonicalProgramDedupeKey_(
                    t,
                    t.reserve.startDate,
                    t.reserve.startTime);

            t.reserve.dedupeKey = key;
            t.reserve.excludeKey = key;
        }

        return t;
    }

   

    std::int64_t MakeSortKeyFromEntry_(const TaskListManager::TaskEntry& t)
    {
        int ymd = t.request.dispatchYmd;
        int hhmm = t.request.dispatchHHMM;
        int sec = t.request.dispatchSecond;

        if (ymd <= 0) {
            ymd = t.schedule.dueYmd;
        }

        if (hhmm <= 0) {
            hhmm = t.schedule.dueHHMM;
        }

        if (ymd <= 0) {
            ymd = ParseYmd_(t.reserve.startDate);
        }

        if (hhmm <= 0) {
            hhmm = ParseHHMM_(t.reserve.startTime);
        }

        if (sec <= 0) {
            sec = ParseSecond_(t.reserve.startTime);
        }

        return static_cast<std::int64_t>(ymd) * 1000000LL +
            static_cast<std::int64_t>(hhmm) * 100LL +
            static_cast<std::int64_t>(sec);
    }

    std::wstring BuildGlobalViewLine_(const TaskListManager::TaskEntry& t)
    {
        std::wstring line;

        AppendField_(line, MakeReservationKey_(t));
        AppendField_(line, MakeFallbackKey_(t));
        AppendField_(line, t.reserve.dedupeKey);

        AppendField_(line, NormalizeMode_(t));
        AppendBoolField_(line, t.enabled);
        AppendField_(line, MakeStatus_(t));

        AppendUIntField_(line, t.request.kind);
        AppendUIntField_(line, t.request.instance);

        AppendIntField_(line, t.reserve.space);
        AppendUIntField_(line, t.reserve.networkId);
        AppendUIntField_(line, t.reserve.tsId);
        AppendUIntField_(line, t.reserve.serviceId);
        AppendUIntField_(line, t.reserve.eventId);

        AppendField_(line, t.reserve.startDate);
        AppendField_(line, t.reserve.startTime);
        AppendField_(line, t.reserve.endDate);
        AppendField_(line, t.reserve.endTime);
        AppendUIntField_(line, t.reserve.durationSec);

        AppendField_(line, MakeTitle_(t));
        AppendField_(line, t.reserve.serviceName);
        AppendField_(line, t.reserve.genre);

        AppendField_(line, t.display.category);
        AppendField_(line, t.display.target);
        AppendField_(line, t.display.note);

        AppendBoolField_(line, t.reserve.disabledByUser);
        AppendBoolField_(line, t.reserve.excludeFromAuto);
        AppendBoolField_(line, t.reserve.autoReserved);

        return line;
    }

    std::wstring BuildServiceViewLine_(const TaskListManager::TaskEntry& t)
    {
        std::wstring line;

        AppendField_(line, MakeReservationKey_(t));
        AppendField_(line, MakeFallbackKey_(t));
        AppendField_(line, t.reserve.dedupeKey);

        AppendField_(line, NormalizeMode_(t));
        AppendBoolField_(line, t.enabled);
        AppendField_(line, MakeStatus_(t));

        AppendUIntField_(line, t.request.kind);
        AppendUIntField_(line, t.request.instance);

        AppendUIntField_(line, t.reserve.networkId);
        AppendUIntField_(line, t.reserve.tsId);
        AppendUIntField_(line, t.reserve.serviceId);
        AppendUIntField_(line, t.reserve.eventId);

        AppendField_(line, t.reserve.startDate);
        AppendField_(line, t.reserve.startTime);
        AppendField_(line, t.reserve.endDate);
        AppendField_(line, t.reserve.endTime);
        AppendUIntField_(line, t.reserve.durationSec);

        AppendField_(line, MakeTitle_(t));
        AppendField_(line, t.reserve.serviceName);

        AppendBoolField_(line, t.reserve.disabledByUser);
        AppendBoolField_(line, t.reserve.excludeFromAuto);
        AppendBoolField_(line, t.reserve.autoReserved);

        return line;
    }

    std::wstring BuildRunLine_(const TaskListManager::TaskEntry& t)
    {
        std::wstring line;

        AppendField_(line, MakeRunKey_(t));
        AppendBoolField_(line, t.enabled);
        AppendField_(line, MakeStatus_(t));

        AppendField_(line, NormalizeMode_(t));
        AppendUIntField_(line, t.request.kind);
        AppendUIntField_(line, t.request.instance);
        AppendIntField_(line, static_cast<int>(t.request.priority));

        AppendIntField_(line, static_cast<int>(t.schedule.type));
        AppendIntField_(line, t.schedule.dailyHour);
        AppendIntField_(line, t.schedule.dailyMinute);
        AppendIntField_(line, t.schedule.hourlyAtMinute);
        AppendIntField_(line, t.schedule.intervalMinutes);

        AppendIntField_(line, t.schedule.dueYmd);
        AppendIntField_(line, t.schedule.dueHHMM);
        AppendIntField_(line, t.schedule.lateToleranceMinutes);

        AppendBoolField_(line, t.schedule.useTimeWindow);
        AppendIntField_(line, t.schedule.windowStartYmd);
        AppendIntField_(line, t.schedule.windowStartHHMM);
        AppendIntField_(line, t.schedule.windowEndYmd);
        AppendIntField_(line, t.schedule.windowEndHHMM);

        AppendIntField_(line, t.request.dispatchYmd);
        AppendIntField_(line, t.request.dispatchHHMM);
        AppendIntField_(line, t.request.dispatchSecond);

        AppendUIntField_(line, t.request.requiredTuners);
        AppendIntField_(line, t.request.preferredTunerIndex);

        AppendField_(line, t.reserve.startDate);
        AppendField_(line, t.reserve.startTime);
        AppendField_(line, t.reserve.endDate);
        AppendField_(line, t.reserve.endTime);

        AppendField_(line, MakeTitle_(t));
        AppendField_(line, t.reserve.serviceName);

        AppendIntField_(line, t.reserve.space);
        AppendUIntField_(line, t.reserve.networkId);
        AppendUIntField_(line, t.reserve.tsId);
        AppendUIntField_(line, t.reserve.serviceId);
        AppendUIntField_(line, t.reserve.eventId);

        AppendField_(line, t.reserve.dedupeKey);
        AppendField_(line, MakeReservationKey_(t));

        AppendBoolField_(line, t.requiresIdle);
        AppendIntField_(line, t.idleRequiredSeconds);

        return line;
    }

    std::wstring MakeGlobalViewHeader_()
    {
        return JoinTab_({
            L"reserveKey",
            L"fallbackKey",
            L"dedupeKey",
            L"mode",
            L"enabled",
            L"status",
            L"kind",
            L"instance",
            L"space",
            L"networkId",
            L"tsId",
            L"serviceId",
            L"eventId",
            L"startDate",
            L"startTime",
            L"endDate",
            L"endTime",
            L"durationSec",
            L"title",
            L"serviceName",
            L"genre",
            L"category",
            L"target",
            L"note",
            L"disabledByUser",
            L"excludeFromAuto",
            L"autoReserved"
            });
    }

    std::wstring MakeServiceViewHeader_()
    {
        return JoinTab_({
            L"reserveKey",
            L"fallbackKey",
            L"dedupeKey",
            L"mode",
            L"enabled",
            L"status",
            L"kind",
            L"instance",
            L"networkId",
            L"tsId",
            L"serviceId",
            L"eventId",
            L"startDate",
            L"startTime",
            L"endDate",
            L"endTime",
            L"durationSec",
            L"title",
            L"serviceName",
            L"disabledByUser",
            L"excludeFromAuto",
            L"autoReserved"
            });
    }

    std::wstring MakeRunHeader_()
    {
        return JoinTab_({
            L"runKey",
            L"enabled",
            L"status",
            L"mode",
            L"kind",
            L"instance",
            L"priority",
            L"scheduleType",
            L"dailyHour",
            L"dailyMinute",
            L"hourlyAtMinute",
            L"intervalMinutes",
            L"dueYmd",
            L"dueHHMM",
            L"lateToleranceMinutes",
            L"useTimeWindow",
            L"windowStartYmd",
            L"windowStartHHMM",
            L"windowEndYmd",
            L"windowEndHHMM",
            L"dispatchYmd",
            L"dispatchHHMM",
            L"dispatchSecond",
            L"requiredTuners",
            L"preferredTunerIndex",
            L"startDate",
            L"startTime",
            L"endDate",
            L"endTime",
            L"title",
            L"serviceName",
            L"space",
            L"networkId",
            L"tsId",
            L"serviceId",
            L"eventId",
            L"dedupeKey",
            L"reserveKey",
            L"requiresIdle",
            L"idleRequiredSeconds"
            });
    }

    void SortLines_(std::vector<SortLine>& lines)
    {
        std::stable_sort(
            lines.begin(),
            lines.end(),
            [](const SortLine& a, const SortLine& b) {
                return a.sortKey < b.sortKey;
            });
    }

    std::vector<std::wstring> ExtractBody_(const std::vector<SortLine>& lines)
    {
        std::vector<std::wstring> body;
        body.reserve(lines.size());

        for (const auto& item : lines) {
            body.push_back(item.line);
        }

        return body;
    }

    bool IsSpaceCacheDirectory_(const std::filesystem::path& p)
    {
        const std::wstring name = p.filename().wstring();
        return name.rfind(L"space", 0) == 0;
    }

    void CleanServiceViewCache_(const std::filesystem::path& viewRoot) noexcept
    {
        try {
            std::error_code ec;
            if (!std::filesystem::exists(viewRoot, ec)) {
                return;
            }

            for (const auto& entry : std::filesystem::directory_iterator(viewRoot, ec)) {
                if (ec) {
                    return;
                }

                if (!entry.is_directory(ec)) {
                    continue;
                }

                if (!IsSpaceCacheDirectory_(entry.path())) {
                    continue;
                }

                std::error_code removeEc;
                std::filesystem::remove_all(entry.path(), removeEc);
            }
        }
        catch (...) {
        }
    }

    std::wstring BuildExecLine_(const TaskListManager::TaskEntry& t)
    {
        std::wstring line;

        AppendField_(line, MakeRunKey_(t));
        AppendBoolField_(line, t.enabled);
        AppendField_(line, MakeStatus_(t));

        AppendUIntField_(line, t.request.kind);
        AppendUIntField_(line, t.request.instance);

        AppendIntField_(line, t.request.dispatchYmd);
        AppendIntField_(line, t.request.dispatchHHMM);
        AppendIntField_(line, t.request.dispatchSecond);

        AppendIntField_(line, t.schedule.dueYmd);
        AppendIntField_(line, t.schedule.dueHHMM);

        AppendBoolField_(line, t.schedule.useTimeWindow);
        AppendIntField_(line, t.schedule.windowStartYmd);
        AppendIntField_(line, t.schedule.windowStartHHMM);
        AppendIntField_(line, t.schedule.windowEndYmd);
        AppendIntField_(line, t.schedule.windowEndHHMM);

        AppendUIntField_(line, t.request.requiredTuners);
        AppendIntField_(line, t.request.preferredTunerIndex);

        AppendField_(line, NormalizeMode_(t));
        AppendField_(line, MakeTitle_(t));
        AppendField_(line, t.reserve.serviceName);

        AppendField_(line, t.reserve.startDate);
        AppendField_(line, t.reserve.startTime);
        AppendField_(line, t.reserve.endDate);
        AppendField_(line, t.reserve.endTime);

        AppendField_(line, t.reserve.dedupeKey);
        AppendField_(line, MakeReservationKey_(t));

        return line;
    }

    std::wstring MakeExecHeader_()
    {
        return JoinTab_({
            L"execKey",
            L"enabled",
            L"status",
            L"kind",
            L"instance",
            L"dispatchYmd",
            L"dispatchHHMM",
            L"dispatchSecond",
            L"dueYmd",
            L"dueHHMM",
            L"useTimeWindow",
            L"windowStartYmd",
            L"windowStartHHMM",
            L"windowEndYmd",
            L"windowEndHHMM",
            L"requiredTuners",
            L"preferredTunerIndex",
            L"mode",
            L"title",
            L"serviceName",
            L"startDate",
            L"startTime",
            L"endDate",
            L"endTime",
            L"dedupeKey",
            L"reserveKey"
            });
    }

}

namespace TaskReservationCache
{
    std::wstring MakeDefaultCacheRoot(const std::wstring& tasksIniPath)
    {
        std::filesystem::path baseDir;

        if (!tasksIniPath.empty()) {
            baseDir = std::filesystem::path(tasksIniPath).parent_path();
        }

        if (baseDir.empty()) {
            baseDir = GetExeDirectory_();
        }

        return (baseDir / L"cache" / L"reservation").wstring();
    }

    WriteResult WriteAllFromTasks(
        const Options& options,
        const std::vector<TaskListManager::TaskEntry>& tasks)
    {
        WriteResult finalResult{};
        finalResult.ok = false;

        const std::wstring cacheRoot = options.cacheRoot.empty()
            ? MakeDefaultCacheRoot(options.tasksIniPath)
            : options.cacheRoot;

        if (cacheRoot.empty()) {
            finalResult.message = L"cacheRoot is empty";
            return finalResult;
        }

        const std::filesystem::path root(cacheRoot);
        const std::filesystem::path viewRoot = root / L"view";
        const std::filesystem::path runRoot = root / L"run";
        const std::filesystem::path execRoot = root / L"exec";

        if (options.writeServiceViewKeys && options.cleanStaleServiceViewCache) {
            CleanServiceViewCache_(viewRoot);
        }

        struct ProgramStartInfo
        {
            std::wstring startDate;
            std::wstring startTime;
        };

        std::map<std::uint32_t, ProgramStartInfo> programStartByInstance;

        for (const auto& src : tasks) {
            if (src.request.kind != TaskKinds::RecStart) {
                continue;
            }

            const int startYmd = EffectiveStartYmd_(src);
            const int startHHMM = EffectiveStartHHMM_(src);
            const int startSecond = EffectiveStartSecond_(src);

            ProgramStartInfo info{};
            info.startDate = FormatIsoDateFromYmd_(startYmd);
            info.startTime = FormatIsoTimeFromHHMMSS_(startHHMM, startSecond);

            if (!info.startDate.empty() && !info.startTime.empty()) {
                programStartByInstance[src.request.instance] = std::move(info);
            }
        }

        auto makeNormalizedCacheEntry =
            [&programStartByInstance](const TaskListManager::TaskEntry& src)
            -> TaskListManager::TaskEntry
            {
                TaskListManager::TaskEntry t = src;

                if (!IsProgramTaskKind_(t.request.kind)) {
                    return t;
                }

                const auto it = programStartByInstance.find(t.request.instance);
                if (it != programStartByInstance.end()) {
                    t.reserve.startDate = it->second.startDate;
                    t.reserve.dateText = it->second.startDate;
                    t.reserve.startTime = it->second.startTime;
                }
                else {
                    const int startYmd = EffectiveStartYmd_(t);
                    const int startHHMM = EffectiveStartHHMM_(t);
                    const int startSecond = EffectiveStartSecond_(t);

                    const std::wstring startDate = FormatIsoDateFromYmd_(startYmd);
                    const std::wstring startTime =
                        FormatIsoTimeFromHHMMSS_(startHHMM, startSecond);

                    if (!startDate.empty()) {
                        t.reserve.startDate = startDate;
                        t.reserve.dateText = startDate;
                    }

                    if (!startTime.empty()) {
                        t.reserve.startTime = startTime;
                    }
                }

                if (t.reserve.durationSec > 0 &&
                    !t.reserve.startDate.empty() &&
                    !t.reserve.startTime.empty()) {
                    std::wstring endDate;
                    std::wstring endTime;

                    if (AddSecondsToIsoDateTime_(
                        t.reserve.startDate,
                        t.reserve.startTime,
                        t.reserve.durationSec,
                        endDate,
                        endTime)) {
                        t.reserve.endDate = endDate;
                        t.reserve.endTime = endTime;

                        const std::wstring shhmm =
                            t.reserve.startTime.size() >= 5
                            ? t.reserve.startTime.substr(0, 5)
                            : t.reserve.startTime;

                        const std::wstring ehhmm =
                            t.reserve.endTime.size() >= 5
                            ? t.reserve.endTime.substr(0, 5)
                            : t.reserve.endTime;

                        if (!shhmm.empty() && !ehhmm.empty()) {
                            t.reserve.timeText = shhmm + L"-" + ehhmm;
                            t.display.note =
                                t.reserve.timeText +
                                L" (" +
                                std::to_wstring(t.reserve.durationSec / 60) +
                                L"m)";
                        }

                        t.display.endYmd = ParseYmd_(t.reserve.endDate);
                        t.display.endHHMM = ParseHHMM_(t.reserve.endTime);
                    }
                }

                if (!t.reserve.startDate.empty() &&
                    !t.reserve.startTime.empty()) {
                    const std::wstring key =
                        MakeCanonicalProgramDedupeKey_(
                            t,
                            t.reserve.startDate,
                            t.reserve.startTime);

                    if (!key.empty()) {
                        t.reserve.dedupeKey = key;
                        t.reserve.excludeKey = key;
                    }
                }

                return t;
            };

        std::vector<SortLine> globalViewLines;
        std::vector<SortLine> runLines;
        std::vector<SortLine> execLines;

        std::map<std::wstring, std::vector<SortLine>> serviceGroups;
        std::map<std::wstring, std::set<std::wstring>> serviceViewKeys;

        for (const auto& src : tasks) {
            TaskListManager::TaskEntry t = makeNormalizedCacheEntry(src);

            const std::int64_t sortKey = MakeSortKeyFromEntry_(t);

            if (options.writeGlobalViewIndex && IsDisplayRepresentative_(t)) {
                SortLine item{};
                item.sortKey = sortKey;
                item.line = BuildGlobalViewLine_(t);
                globalViewLines.push_back(std::move(item));
            }

            if (options.writeServiceViewKeys && IsServiceViewTarget_(t)) {
                const std::filesystem::path servicePath =
                    viewRoot /
                    (L"space" + std::to_wstring(t.reserve.space)) /
                    MakeServiceDirName_(t) /
                    L"reserve_keys.tsv";

                const std::wstring servicePathText = servicePath.wstring();
                const std::wstring reservationKey = MakeReservationKey_(t);

                auto& usedKeys = serviceViewKeys[servicePathText];

                if (reservationKey.empty() ||
                    usedKeys.insert(reservationKey).second) {
                    SortLine item{};
                    item.sortKey = sortKey;
                    item.line = BuildServiceViewLine_(t);

                    serviceGroups[servicePathText].push_back(std::move(item));
                }
            }

            if (options.writeRunIndex) {
                SortLine item{};
                item.sortKey = sortKey;
                item.line = BuildRunLine_(t);
                runLines.push_back(std::move(item));
            }

            if (options.writeExecIndex) {
                SortLine item{};
                item.sortKey = sortKey;
                item.line = BuildExecLine_(t);
                execLines.push_back(std::move(item));
            }
        }

        if (options.writeGlobalViewIndex) {
            SortLines_(globalViewLines);

            const std::filesystem::path globalViewPath =
                viewRoot / L"tasks_view_index.tsv";

            const auto body = ExtractBody_(globalViewLines);

            const auto result = WriteLinesAtomic_(
                globalViewPath.wstring(),
                L"# TaskReservationGlobalViewCache TSV v1",
                MakeGlobalViewHeader_(),
                body);

            if (!result.ok) {
                finalResult.message = L"global view cache failed: " + result.message;
                return finalResult;
            }

            finalResult.globalViewRows = body.size();
        }

        if (options.writeServiceViewKeys) {
            for (auto& pair : serviceGroups) {
                auto& lines = pair.second;
                SortLines_(lines);

                const auto body = ExtractBody_(lines);

                const auto result = WriteLinesAtomic_(
                    pair.first,
                    L"# TaskReservationServiceViewCache TSV v1",
                    MakeServiceViewHeader_(),
                    body);

                if (!result.ok) {
                    finalResult.message = L"service view cache failed: " + result.message;
                    return finalResult;
                }

                ++finalResult.serviceViewFiles;
                finalResult.serviceViewRows += body.size();
            }
        }

        if (options.writeRunIndex) {
            SortLines_(runLines);

            const std::filesystem::path runPath =
                runRoot / L"tasks_run_index.tsv";

            const auto body = ExtractBody_(runLines);

            const auto result = WriteLinesAtomic_(
                runPath.wstring(),
                L"# TaskReservationRunCache TSV v1",
                MakeRunHeader_(),
                body);

            if (!result.ok) {
                finalResult.message = L"run cache failed: " + result.message;
                return finalResult;
            }

            finalResult.runRows = body.size();
        }

        if (options.writeExecIndex) {
            SortLines_(execLines);

            const std::filesystem::path execPath =
                execRoot / L"tasks_exec_index.tsv";

            const auto body = ExtractBody_(execLines);

            const auto result = WriteLinesAtomic_(
                execPath.wstring(),
                L"# TaskReservationExecCache TSV v1",
                MakeExecHeader_(),
                body);

            if (!result.ok) {
                finalResult.message = L"exec cache failed: " + result.message;
                return finalResult;
            }

            finalResult.execRows = body.size();
        }

        finalResult.ok = true;
        finalResult.message = L"ok";
        return finalResult;
    }

    WriteResult WriteAllFromTasks(
        const std::wstring& tasksIniPath,
        const std::vector<TaskListManager::TaskEntry>& tasks)
    {
        Options options{};
        options.tasksIniPath = tasksIniPath;
        options.cacheRoot = MakeDefaultCacheRoot(tasksIniPath);
        options.writeGlobalViewIndex = true;
        options.writeServiceViewKeys = true;
        options.writeRunIndex = true;
        options.writeExecIndex = true;
        options.cleanStaleServiceViewCache = true;

        return WriteAllFromTasks(options, tasks);
    }
}