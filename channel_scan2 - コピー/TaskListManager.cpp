#include "TaskListManager.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

    std::wstring MakeSectionName(std::uint32_t kind, std::uint32_t instance);
    void NormalizeTaskEntry_(TaskListManager::TaskEntry& t);

    struct TaskIniRecord
    {
        std::uint32_t kind = 0;
        std::uint32_t instance = 0;

        std::wstring sectionName;

        std::wstring name;
        TaskListManager::DisplayInfo display{};
        TaskListManager::ReserveInfo reserve{};
        bool enabled = true;

        TimeKeeper::TaskRequest request{};
        TaskListManager::Schedule schedule{};

        bool requiresIdle = false;
        int idleRequiredSeconds = 0;
    };

    struct TaskIniDocument
    {
        std::vector<TaskIniRecord> records;
        std::uint64_t revision = 0;
    };

    std::wstring SanitizeIniValue_(const std::wstring& src)
    {
        std::wstring out;
        out.reserve(src.size());

        for (wchar_t ch : src) {
            if (ch == L'\r' || ch == L'\n') {
                out.push_back(L' ');
            }
            else {
                out.push_back(ch);
            }
        }

        return out;
    }

    void AppendIniSection_(std::wstring& out, const std::wstring& section)
    {
        out += L'[';
        out += section;
        out += L"]\r\n";
    }

    void AppendIniPair_(std::wstring& out, const wchar_t* key, const std::wstring& value)
    {
        out += key;
        out += L'=';
        out += SanitizeIniValue_(value);
        out += L"\r\n";
    }

    void AppendIniPairInt_(std::wstring& out, const wchar_t* key, long long value)
    {
        out += key;
        out += L'=';
        out += std::to_wstring(value);
        out += L"\r\n";
    }

    void AppendIniPairUInt_(std::wstring& out, const wchar_t* key, unsigned long long value)
    {
        out += key;
        out += L'=';
        out += std::to_wstring(value);
        out += L"\r\n";
    }

    void AppendIniPairBool_(std::wstring& out, const wchar_t* key, bool value)
    {
        out += key;
        out += L'=';
        out += value ? L"1" : L"0";
        out += L"\r\n";
    }

    int GetScheduleSortOrder_(TaskListManager::ScheduleType type) noexcept
    {
        switch (type) {
        case TaskListManager::ScheduleType::OneShot:
            return 0;
        case TaskListManager::ScheduleType::Daily:
            return 1;
        case TaskListManager::ScheduleType::Hourly:
            return 2;
        case TaskListManager::ScheduleType::Interval:
            return 3;
        default:
            return 9;
        }
    }

    int MakeHHMM_(int hour, int minute) noexcept
    {
        return hour * 100 + minute;
    }

    TaskIniRecord MakeTaskIniRecord_(const TaskListManager::TaskEntry& src)
    {
        TaskListManager::TaskEntry t = src;
        NormalizeTaskEntry_(t);

        TaskIniRecord r{};

        r.kind = t.request.kind;
        r.instance = t.request.instance;
        r.sectionName = MakeSectionName(t.request.kind, t.request.instance);

        r.name = t.name;
        r.display = t.display;
        r.reserve = t.reserve;
        r.enabled = t.enabled;

        r.request = t.request;
        r.schedule = t.schedule;

        r.requiresIdle = t.requiresIdle;
        r.idleRequiredSeconds = t.idleRequiredSeconds;

        return r;
    }

    void SortTaskIniRecords_(std::vector<TaskIniRecord>& records)
    {
        std::stable_sort(
            records.begin(),
            records.end(),
            [](const TaskIniRecord& a, const TaskIniRecord& b) {
                if (a.enabled != b.enabled) {
                    return a.enabled > b.enabled;
                }

                const int ao = GetScheduleSortOrder_(a.schedule.type);
                const int bo = GetScheduleSortOrder_(b.schedule.type);

                if (ao != bo) {
                    return ao < bo;
                }

                if (a.schedule.type == TaskListManager::ScheduleType::OneShot) {
                    if (a.schedule.dueYmd != b.schedule.dueYmd) {
                        return a.schedule.dueYmd < b.schedule.dueYmd;
                    }
                    if (a.schedule.dueHHMM != b.schedule.dueHHMM) {
                        return a.schedule.dueHHMM < b.schedule.dueHHMM;
                    }
                }
                else if (a.schedule.type == TaskListManager::ScheduleType::Daily) {
                    const int ah = MakeHHMM_(a.schedule.dailyHour, a.schedule.dailyMinute);
                    const int bh = MakeHHMM_(b.schedule.dailyHour, b.schedule.dailyMinute);

                    if (ah != bh) {
                        return ah < bh;
                    }
                }
                else if (a.schedule.type == TaskListManager::ScheduleType::Hourly) {
                    if (a.schedule.hourlyAtMinute != b.schedule.hourlyAtMinute) {
                        return a.schedule.hourlyAtMinute < b.schedule.hourlyAtMinute;
                    }
                }
                else if (a.schedule.type == TaskListManager::ScheduleType::Interval) {
                    if (a.schedule.intervalMinutes != b.schedule.intervalMinutes) {
                        return a.schedule.intervalMinutes < b.schedule.intervalMinutes;
                    }
                }

                if (a.kind != b.kind) {
                    return a.kind < b.kind;
                }

                return a.instance < b.instance;
            });
    }

    TaskIniDocument MakeTaskIniDocument_(
        const std::vector<TaskListManager::TaskEntry>& tasks,
        std::uint64_t revision)
    {
        TaskIniDocument doc{};
        doc.revision = revision;
        doc.records.reserve(tasks.size());

        for (const auto& task : tasks) {
            doc.records.push_back(MakeTaskIniRecord_(task));
        }

        SortTaskIniRecords_(doc.records);
        return doc;
    }

    void AppendTaskIniRecord_(std::wstring& ini, const TaskIniRecord& r)
    {
        AppendIniSection_(ini, r.sectionName);

        AppendIniPair_(ini, L"Name", r.name);

        AppendIniPair_(ini, L"Label", r.display.label);
        AppendIniPair_(ini, L"Category", r.display.category);
        AppendIniPair_(ini, L"Target", r.display.target);
        AppendIniPair_(ini, L"Note", r.display.note);
        AppendIniPairInt_(ini, L"EndYmd", r.display.endYmd);
        AppendIniPairInt_(ini, L"EndHHMM", r.display.endHHMM);

        AppendIniPairInt_(ini, L"Space", r.reserve.space);
        AppendIniPairUInt_(ini, L"NetworkId", static_cast<unsigned>(r.reserve.networkId));
        AppendIniPairUInt_(ini, L"TsId", static_cast<unsigned>(r.reserve.tsId));
        AppendIniPairUInt_(ini, L"ServiceId", static_cast<unsigned>(r.reserve.serviceId));
        AppendIniPairUInt_(ini, L"EventId", static_cast<unsigned>(r.reserve.eventId));

        AppendIniPair_(ini, L"Title", r.reserve.title);
        AppendIniPair_(ini, L"TitleLabel", r.reserve.titleLabel);
        AppendIniPair_(ini, L"ServiceName", r.reserve.serviceName);
        AppendIniPair_(ini, L"Summary", r.reserve.summary);
        AppendIniPair_(ini, L"Genre", r.reserve.genre);

        AppendIniPair_(ini, L"DateText", r.reserve.dateText);
        AppendIniPair_(ini, L"WeekdayText", r.reserve.weekdayText);
        AppendIniPair_(ini, L"TimeText", r.reserve.timeText);

        AppendIniPair_(ini, L"StartDate", r.reserve.startDate);
        AppendIniPair_(ini, L"StartTime", r.reserve.startTime);
        AppendIniPair_(ini, L"EndDate", r.reserve.endDate);
        AppendIniPair_(ini, L"EndTime", r.reserve.endTime);
        AppendIniPairUInt_(ini, L"DurationSec", r.reserve.durationSec);

        AppendIniPair_(ini, L"ReserveMode", r.reserve.reserveMode);
        AppendIniPair_(ini, L"ReserveSource", r.reserve.reserveSource);
        AppendIniPair_(ini, L"DedupeKey", r.reserve.dedupeKey);

        AppendIniPairBool_(ini, L"DisabledByUser", r.reserve.disabledByUser);
        AppendIniPairBool_(ini, L"ExcludeFromAuto", r.reserve.excludeFromAuto);
        AppendIniPairBool_(ini, L"AutoReserved", r.reserve.autoReserved);
        AppendIniPair_(ini, L"ExcludeKey", r.reserve.excludeKey);

        AppendIniPairBool_(ini, L"Enabled", r.enabled);

        AppendIniPairUInt_(ini, L"Priority", static_cast<unsigned>(r.request.priority));
        AppendIniPairUInt_(ini, L"RequiredTuners", static_cast<unsigned>(r.request.requiredTuners));
        AppendIniPairInt_(ini, L"PreferredTunerIndex", r.request.preferredTunerIndex);
        AppendIniPairInt_(ini, L"TunerIndex", r.request.preferredTunerIndex);

        AppendIniPairInt_(ini, L"DispatchYmd", r.request.dispatchYmd);
        AppendIniPairInt_(ini, L"DispatchHHMM", r.request.dispatchHHMM);
        AppendIniPairInt_(ini, L"DispatchSecond", r.request.dispatchSecond);

        AppendIniPairBool_(ini, L"RequiresIdle", r.requiresIdle);
        AppendIniPairInt_(ini, L"IdleRequiredSeconds", r.idleRequiredSeconds);

        AppendIniPairInt_(ini, L"ScheduleType", static_cast<int>(r.schedule.type));
        AppendIniPairInt_(ini, L"DailyHour", r.schedule.dailyHour);
        AppendIniPairInt_(ini, L"DailyMinute", r.schedule.dailyMinute);
        AppendIniPairInt_(ini, L"HourlyAtMinute", r.schedule.hourlyAtMinute);
        AppendIniPairInt_(ini, L"IntervalMinutes", r.schedule.intervalMinutes);

        AppendIniPairInt_(ini, L"DueYmd", r.schedule.dueYmd);
        AppendIniPairInt_(ini, L"DueHHMM", r.schedule.dueHHMM);
        AppendIniPairInt_(ini, L"DueSecond", r.schedule.dueSecond);
        AppendIniPairInt_(ini, L"LateToleranceMinutes", r.schedule.lateToleranceMinutes);

        AppendIniPairBool_(ini, L"UseTimeWindow", r.schedule.useTimeWindow);
        AppendIniPairInt_(ini, L"WindowStartYmd", r.schedule.windowStartYmd);
        AppendIniPairInt_(ini, L"WindowStartHHMM", r.schedule.windowStartHHMM);
        AppendIniPairInt_(ini, L"WindowStartSecond", r.schedule.windowStartSecond);
        AppendIniPairInt_(ini, L"WindowEndYmd", r.schedule.windowEndYmd);
        AppendIniPairInt_(ini, L"WindowEndHHMM", r.schedule.windowEndHHMM);
        AppendIniPairInt_(ini, L"WindowEndSecond", r.schedule.windowEndSecond);

        ini += L"\r\n";
    }

    std::wstring BuildTaskIniText_(const TaskIniDocument& doc)
    {
        std::wstring ini;

        // 予約が増えても再確保が増えにくい程度の目安。
        ini.reserve(4096 + doc.records.size() * 4096);

        AppendIniSection_(ini, L"TaskList");
        AppendIniPairInt_(ini, L"Count", static_cast<long long>(doc.records.size()));
        AppendIniPairUInt_(ini, L"Revision", doc.revision);

        for (std::size_t i = 0; i < doc.records.size(); ++i) {
            wchar_t key[64]{};
            swprintf_s(key, L"Task_%zu", i);

            const auto& r = doc.records[i];

            std::wstring value;
            value.reserve(32);
            value += std::to_wstring(r.kind);
            value += L",";
            value += std::to_wstring(r.instance);

            AppendIniPair_(ini, key, value);
        }

        ini += L"\r\n";

        for (const auto& r : doc.records) {
            AppendTaskIniRecord_(ini, r);
        }

        return ini;
    }

    bool EnsureParentDirectoryForTaskIni_(const std::wstring& path)
    {
        try {
            const std::filesystem::path p(path);
            const auto parent = p.parent_path();

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

    std::wstring MakeTaskIniTempPath_(const std::wstring& path)
    {
        wchar_t suffix[128]{};

        swprintf_s(
            suffix,
            L".tmp.%lu.%lu.%llu",
            static_cast<unsigned long>(::GetCurrentProcessId()),
            static_cast<unsigned long>(::GetCurrentThreadId()),
            static_cast<unsigned long long>(::GetTickCount64()));

        return path + suffix;
    }

    bool WriteUtf16LeTextAtomic_(const std::wstring& path, const std::wstring& text)
    {
        if (path.empty()) {
            return false;
        }

        if (!EnsureParentDirectoryForTaskIni_(path)) {
            return false;
        }

        const std::wstring tempPath = MakeTaskIniTempPath_(path);

        HANDLE hFile = ::CreateFileW(
            tempPath.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
            nullptr);

        if (hFile == INVALID_HANDLE_VALUE) {
            return false;
        }

        bool ok = true;

        const unsigned char bom[2] = { 0xFF, 0xFE };
        DWORD written = 0;

        ok = ok && (::WriteFile(hFile, bom, sizeof(bom), &written, nullptr) != 0);
        ok = ok && (written == sizeof(bom));

        if (ok && !text.empty()) {
            const DWORD bytesToWrite =
                static_cast<DWORD>(text.size() * sizeof(wchar_t));

            written = 0;

            ok = (::WriteFile(
                hFile,
                text.data(),
                bytesToWrite,
                &written,
                nullptr) != 0);

            ok = ok && (written == bytesToWrite);
        }

        if (ok) {
            ok = (::FlushFileBuffers(hFile) != 0);
        }

        ::CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;

        if (!ok) {
            ::DeleteFileW(tempPath.c_str());
            return false;
        }

        constexpr DWORD kMoveFlags =
            MOVEFILE_REPLACE_EXISTING |
            MOVEFILE_WRITE_THROUGH;

        for (int retry = 0; retry < 10; ++retry) {
            if (::MoveFileExW(tempPath.c_str(), path.c_str(), kMoveFlags)) {
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

        ::DeleteFileW(tempPath.c_str());
        return false;
    }

    bool WriteTaskIniDocumentAtomic_(const std::wstring& path, const TaskIniDocument& doc)
    {
        const std::wstring text = BuildTaskIniText_(doc);
        return WriteUtf16LeTextAtomic_(path, text);
    }

    int CompareYmdHHMM_(int ymd1, int hhmm1, int ymd2, int hhmm2)
    {
        if (ymd1 != ymd2) return (ymd1 < ymd2) ? -1 : 1;
        if (hhmm1 != hhmm2) return (hhmm1 < hhmm2) ? -1 : 1;
        return 0;
    }

    std::wstring MakeSectionName(std::uint32_t kind, std::uint32_t instance)
    {
        wchar_t buf[64]{};
        swprintf_s(buf, L"Task_%u_%u", kind, instance);
        return std::wstring(buf);
    }

    bool WriteIniString(const std::wstring& path, const std::wstring& section,
        const std::wstring& key, const std::wstring& value)
    {
        return ::WritePrivateProfileStringW(section.c_str(), key.c_str(), value.c_str(), path.c_str()) != 0;
    }

    bool DeleteIniSection(const std::wstring& path, const std::wstring& section)
    {
        return ::WritePrivateProfileStringW(section.c_str(), nullptr, nullptr, path.c_str()) != 0;
    }

    bool EnsureParentDirectory_(const std::wstring& path)
    {
        try {
            const std::filesystem::path p(path);
            const auto parent = p.parent_path();
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

    std::wstring MakeTempIniPath_(const std::wstring& path)
    {
        const DWORD pid = ::GetCurrentProcessId();
        const DWORD tid = ::GetCurrentThreadId();
        const ULONGLONG tick = ::GetTickCount64();

        wchar_t suffix[128]{};
        swprintf_s(
            suffix,
            L".tmp.%lu.%lu.%llu.writing",
            static_cast<unsigned long>(pid),
            static_cast<unsigned long>(tid),
            static_cast<unsigned long long>(tick));

        return path + suffix;
    }

    bool CreateEmptyUtf16IniFile_(const std::wstring& path)
    {
        HANDLE h = ::CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
            nullptr);

        if (h == INVALID_HANDLE_VALUE) {
            return false;
        }

        // WritePrivateProfileStringW で日本語を安全に保存するため、
        // 空ファイルではなく UTF-16 LE BOM を先に入れておく。
        const unsigned char bom[2] = { 0xFF, 0xFE };
        DWORD written = 0;
        const BOOL ok = ::WriteFile(h, bom, sizeof(bom), &written, nullptr);

        if (ok) {
            ::FlushFileBuffers(h);
        }

        ::CloseHandle(h);
        return ok && written == sizeof(bom);
    }

    bool ReplaceIniFileAtomic_(const std::wstring& tempPath, const std::wstring& path)
    {
        constexpr DWORD flags =
            MOVEFILE_REPLACE_EXISTING |
            MOVEFILE_WRITE_THROUGH;

        for (int i = 0; i < 10; ++i) {
            if (::MoveFileExW(tempPath.c_str(), path.c_str(), flags)) {
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

    std::wstring ReadIniString(const std::wstring& path, const std::wstring& section,
        const std::wstring& key, const std::wstring& def = L"")
    {
        wchar_t buf[2048]{};
        ::GetPrivateProfileStringW(section.c_str(), key.c_str(), def.c_str(), buf, static_cast<DWORD>(_countof(buf)), path.c_str());
        return std::wstring(buf);
    }

    int ReadIniInt(const std::wstring& path, const std::wstring& section,
        const std::wstring& key, int defVal)
    {
        return static_cast<int>(::GetPrivateProfileIntW(section.c_str(), key.c_str(), defVal, path.c_str()));
    }

    bool ReadIniBool(const std::wstring& path, const std::wstring& section,
        const std::wstring& key, bool defVal)
    {
        return ReadIniInt(path, section, key, defVal ? 1 : 0) != 0;
    }

    int ClampInt_(int v, int minV, int maxV)
    {
        if (v < minV) return minV;
        if (v > maxV) return maxV;
        return v;
    }

    std::wstring FormatHHMM_(int hhmm)
    {
        const int h = ClampInt_(hhmm / 100, 0, 23);
        const int m = ClampInt_(hhmm % 100, 0, 59);
        wchar_t buf[16]{};
        swprintf_s(buf, L"%02d:%02d", h, m);
        return std::wstring(buf);
    }

    std::wstring FormatDaily_(int hour, int minute)
    {
        wchar_t buf[24]{};
        swprintf_s(buf, L"毎日 %02d:%02d", ClampInt_(hour, 0, 23), ClampInt_(minute, 0, 59));
        return std::wstring(buf);
    }

    std::wstring DefaultCategoryForKind_(std::uint32_t kind)
    {
        switch (kind) {
        case TaskKinds::EpgFull:
        case TaskKinds::EpgLight:
            return L"EPG";
        case TaskKinds::RecPrepare:
        case TaskKinds::RecStart:
        case TaskKinds::RecStop:
            return L"RECORD";
        case TaskKinds::CleanupLogs:
            return L"UTILITY";
        default:
            return L"GENERAL";
        }
    }

    std::wstring DefaultNameForEntry_(const TaskListManager::TaskEntry& t)
    {
        switch (t.request.kind) {
        case TaskKinds::EpgFull:
            return (t.schedule.type == TaskListManager::ScheduleType::OneShot)
                ? L"EPG Full OneShot"
                : L"EPG Full Update (Daily)";
        case TaskKinds::EpgLight:
            return L"EPG Light Update (Hourly)";
        case TaskKinds::RecPrepare:
            return L"Recording Prepare";
        case TaskKinds::RecStart:
            return L"Recording Start";
        case TaskKinds::RecStop:
            return L"Recording Stop";
        case TaskKinds::CleanupLogs:
            return L"Cleanup Logs";
        default:
            return L"Task";
        }
    }

    std::wstring DefaultLabelForEntry_(const TaskListManager::TaskEntry& t)
    {
        switch (t.request.kind) {
        case TaskKinds::EpgFull:
            return (t.schedule.type == TaskListManager::ScheduleType::OneShot)
                ? L"EPG取得予約(次回)"
                : L"EPG取得予約";
        case TaskKinds::EpgLight:
            return L"EPG軽量更新予約";
        case TaskKinds::RecPrepare:
            return L"録画準備予約";
        case TaskKinds::RecStart:
            return L"番組予約";
        case TaskKinds::RecStop:
            return L"録画終了予約";
        case TaskKinds::CleanupLogs:
            return L"ログ整理予約";
        default:
            return L"タスク予約";
        }
    }

    std::wstring DefaultTargetForEntry_(const TaskListManager::TaskEntry& t)
    {
        switch (t.request.kind) {
        case TaskKinds::EpgFull:
        case TaskKinds::EpgLight:
            return L"EPG対象サービス";
        case TaskKinds::CleanupLogs:
            return L"アプリ全体";
        default:
            return L"-";
        }
    }

    std::wstring DefaultNoteForEntry_(const TaskListManager::TaskEntry& t)
    {
        switch (t.schedule.type) {
        case TaskListManager::ScheduleType::Daily:
            return FormatDaily_(t.schedule.dailyHour, t.schedule.dailyMinute);

        case TaskListManager::ScheduleType::Hourly: {
            wchar_t buf[24]{};
            swprintf_s(buf, L"毎時 %02d分", ClampInt_(t.schedule.hourlyAtMinute, 0, 59));
            return std::wstring(buf);
        }

        case TaskListManager::ScheduleType::Interval:
            return (t.schedule.intervalMinutes > 0)
                ? (std::to_wstring(t.schedule.intervalMinutes) + L"分ごと")
                : L"間隔実行";

        case TaskListManager::ScheduleType::OneShot:
            if (t.schedule.dueYmd > 0) {
                wchar_t buf[40]{};
                swprintf_s(buf, L"%08d %ls", t.schedule.dueYmd, FormatHHMM_(t.schedule.dueHHMM).c_str());
                return std::wstring(buf);
            }
            return FormatHHMM_(t.schedule.dueHHMM);

        default:
            return L"";
        }
    }

    void NormalizeTaskEntry_(TaskListManager::TaskEntry& t)
    {
        if (t.request.requiredTuners == 0) {
            t.request.requiredTuners = 1;
        }
        if (t.request.preferredTunerIndex < -1) {
            t.request.preferredTunerIndex = -1;
        }

        if (t.request.dispatchSecond < 0) {
            t.request.dispatchSecond = 0;
        }
        if (t.request.dispatchSecond > 59) {
            t.request.dispatchSecond = 59;
        }

        // dispatchHHMM=0000 は 00:00 として有効。
        // ここで dispatchHHMM <= 0 を使うと、0時予約の秒待機が消える。
        if (t.request.dispatchYmd <= 0 || t.request.dispatchHHMM < 0) {
            t.request.dispatchSecond = 0;
        }

        if (t.schedule.dueSecond < 0) {
            t.schedule.dueSecond = 0;
        }
        if (t.schedule.dueSecond > 59) {
            t.schedule.dueSecond = 59;
        }

        if (t.schedule.windowStartSecond < 0) {
            t.schedule.windowStartSecond = 0;
        }
        if (t.schedule.windowStartSecond > 59) {
            t.schedule.windowStartSecond = 59;
        }

        if (t.schedule.windowEndSecond < 0) {
            t.schedule.windowEndSecond = 0;
        }
        if (t.schedule.windowEndSecond > 59) {
            t.schedule.windowEndSecond = 59;
        }

        if (t.schedule.useTimeWindow) {
            if (t.schedule.windowStartYmd <= 0) {
                t.schedule.windowStartYmd = t.schedule.dueYmd;
            }

            // 0000 は有効なので <= 0 判定にしない。
            if (t.schedule.windowStartHHMM < 0) {
                t.schedule.windowStartHHMM = t.schedule.dueHHMM;
            }

            if (t.schedule.windowEndYmd <= 0) {
                t.schedule.windowEndYmd = t.schedule.windowStartYmd;
            }

            // 0000 は有効。
            if (t.schedule.windowEndHHMM < 0) {
                t.schedule.windowEndHHMM = t.schedule.windowStartHHMM;
            }

            if (CompareYmdHHMM_(
                t.schedule.windowEndYmd, t.schedule.windowEndHHMM,
                t.schedule.windowStartYmd, t.schedule.windowStartHHMM) < 0) {
                t.schedule.windowEndYmd = t.schedule.windowStartYmd;
                t.schedule.windowEndHHMM = t.schedule.windowStartHHMM;
                t.schedule.windowEndSecond = t.schedule.windowStartSecond;
            }

            if (t.schedule.dueYmd <= 0) {
                t.schedule.dueYmd = t.schedule.windowStartYmd;
            }

            // dueHHMM=0000 は有効なので、dueHHMM <= 0 で補正しない。
            if (t.schedule.dueHHMM < 0) {
                t.schedule.dueHHMM = t.schedule.windowStartHHMM;
                t.schedule.dueSecond = t.schedule.windowStartSecond;
            }
        }

        if (t.schedule.type == TaskListManager::ScheduleType::OneShot) {
            if (t.schedule.dueYmd > 0 && t.schedule.dueHHMM >= 0) {
                if (t.request.dispatchYmd <= 0) {
                    t.request.dispatchYmd = t.schedule.dueYmd;
                }
                if (t.request.dispatchHHMM < 0) {
                    t.request.dispatchHHMM = t.schedule.dueHHMM;
                }
            }
        }
    }

    void DeleteAllTaskSections_(const std::wstring& path)
    {
        const std::wstring listSection = L"TaskList";
        const int count = ReadIniInt(path, listSection, L"Count", 0);

        for (int i = 0; i < count; ++i) {
            wchar_t key[64]{};
            swprintf_s(key, L"Task_%d", i);
            const std::wstring v = ReadIniString(path, listSection, key, L"");
            if (v.empty()) continue;

            unsigned int kind = 0, inst = 0;
            if (swscanf_s(v.c_str(), L"%u,%u", &kind, &inst) != 2) continue;

            DeleteIniSection(path, MakeSectionName(kind, inst));
        }

        DeleteIniSection(path, listSection);
    }

    std::wstring ReadIniStringOrEmpty_(const std::wstring& path, const std::wstring& sec, const std::wstring& key)
    {
        return ReadIniString(path, sec, key, L"");
    }
}

std::uint64_t TaskListManager::MakeKey_(std::uint32_t kind, std::uint32_t instance)
{
    return (static_cast<std::uint64_t>(kind) << 32) | static_cast<std::uint64_t>(instance);
}

void TaskListManager::Clear()
{
    std::lock_guard lk(mtx_);
    tasks_.clear();
}

void TaskListManager::UpsertTask(const TaskEntry& entry)
{
    TaskEntry normalized = entry;
    NormalizeTaskEntry_(normalized);

    std::lock_guard lk(mtx_);

    const auto key = MakeKey_(normalized.request.kind, normalized.request.instance);
    for (auto& t : tasks_) {
        if (MakeKey_(t.request.kind, t.request.instance) == key) {
            t = std::move(normalized);
            return;
        }
    }

    tasks_.push_back(std::move(normalized));
}

bool TaskListManager::RemoveTask(std::uint32_t kind, std::uint32_t instance)
{
    std::lock_guard lk(mtx_);
    const auto key = MakeKey_(kind, instance);

    const auto it = std::remove_if(tasks_.begin(), tasks_.end(),
        [key](const TaskEntry& t) { return MakeKey_(t.request.kind, t.request.instance) == key; });

    const bool removed = (it != tasks_.end());
    tasks_.erase(it, tasks_.end());
    return removed;
}

std::optional<TaskListManager::TaskEntry> TaskListManager::Find(std::uint32_t kind, std::uint32_t instance) const
{
    std::lock_guard lk(mtx_);
    const auto key = MakeKey_(kind, instance);

    for (const auto& t : tasks_) {
        if (MakeKey_(t.request.kind, t.request.instance) == key) {
            return t;
        }
    }
    return std::nullopt;
}

std::vector<TaskListManager::TaskEntry> TaskListManager::GetSnapshot() const
{
    std::lock_guard lk(mtx_);
    return tasks_;
}

void TaskListManager::EnsureDefaultEpgTasks(std::uint8_t requiredTuners)
{
    {
        TaskEntry full{};
        full.name = L"EPG Full Update (Daily)";
        full.display.label = L"EPG取得予約";
        full.display.category = L"EPG";
        full.display.target = L"EPG対象サービス";
        full.display.note = L"毎日 03:30";
        full.enabled = false;
        full.request.kind = TaskKinds::EpgFull;
        full.request.instance = 0;
        full.request.priority = TimeKeeper::TaskPriority::Soft;
        full.request.requiredTuners = requiredTuners;
        full.schedule.type = ScheduleType::Daily;
        full.schedule.dailyHour = 3;
        full.schedule.dailyMinute = 30;
        UpsertTask(full);
    }

    {
        TaskEntry light{};
        light.name = L"EPG Light Update (Hourly)";
        light.display.label = L"EPG軽量更新予約";
        light.display.category = L"EPG";
        light.display.target = L"EPG対象サービス";
        light.display.note = L"毎時 05分";
        light.enabled = false;
        light.request.kind = TaskKinds::EpgLight;
        light.request.instance = 0;
        light.request.priority = TimeKeeper::TaskPriority::Soft;
        light.request.requiredTuners = requiredTuners;
        light.schedule.type = ScheduleType::Hourly;
        light.schedule.hourlyAtMinute = 5;
        UpsertTask(light);
    }
}

TimeKeeper::Settings TaskListManager::BuildTimeKeeperSettingsFromTasks(const TimeKeeper::Settings& base) const
{
    TimeKeeper::Settings s = base;

    const auto fullOpt = Find(TaskKinds::EpgFull, 0);
    if (fullOpt && fullOpt->enabled && fullOpt->schedule.type == ScheduleType::Daily) {
        s.enableDailyFull = true;
        s.dailyFullAt.hour = fullOpt->schedule.dailyHour;
        s.dailyFullAt.minute = fullOpt->schedule.dailyMinute;
        s.epgRequiredTuners = fullOpt->request.requiredTuners;
    }
    else {
        s.enableDailyFull = false;
    }

    const auto lightOpt = Find(TaskKinds::EpgLight, 0);
    if (lightOpt && lightOpt->enabled) {
        s.epgRequiredTuners = lightOpt->request.requiredTuners;

        if (lightOpt->schedule.type == ScheduleType::Hourly) {
            s.enableHourly = true;
            s.hourlyAtMinute = lightOpt->schedule.hourlyAtMinute;
            s.lightIntervalMinutes = 0;
        }
        else if (lightOpt->schedule.type == ScheduleType::Interval) {
            s.enableHourly = false;
            s.lightIntervalMinutes = lightOpt->schedule.intervalMinutes;
        }
        else {
            s.enableHourly = false;
            s.lightIntervalMinutes = 0;
        }

        if (lightOpt->requiresIdle && lightOpt->idleRequiredSeconds > 0) {
            s.idleRequiredSeconds = lightOpt->idleRequiredSeconds;
        }
    }
    else {
        s.enableHourly = false;
        s.lightIntervalMinutes = 0;
    }

    return s;
}

void TaskListManager::ApplyOneShotsToTimeKeeper(TimeKeeper& tk) const
{
    const auto snapshot = GetSnapshot();
    tk.ClearOneShotTasks();

    SYSTEMTIME localNow{};
    ::GetLocalTime(&localNow);
    const int nowYmd = static_cast<int>(localNow.wYear) * 10000
        + static_cast<int>(localNow.wMonth) * 100
        + static_cast<int>(localNow.wDay);
    const int nowHHMM = static_cast<int>(localNow.wHour) * 100
        + static_cast<int>(localNow.wMinute);
    const int nowSecond = static_cast<int>(localNow.wSecond);

    // 日時の比較は YYYYMMDD / HHMM / 秒を全て使用する。
    // 00:00 を未設定とみなさず、日付をまたぐ番組にも対応する。
    const auto compareTime = [](int ay, int ah, int as,
        int by, int bh, int bs) noexcept -> int {
            if (ay != by) return ay < by ? -1 : 1;
            if (ah != bh) return ah < bh ? -1 : 1;
            if (as != bs) return as < bs ? -1 : 1;
            return 0;
        };

    enum class BroadcastState : std::uint8_t {
        BeforeStart,
        OnAir,
        Finished,
        Unknown
    };

    const auto getBroadcastState = [&](std::uint32_t instance) -> BroadcastState {
        const TaskEntry* start = nullptr;
        const TaskEntry* stop = nullptr;
        for (const auto& item : snapshot) {
            if (item.request.instance != instance) continue;
            if (item.request.kind == TaskKinds::RecStart) start = &item;
            if (item.request.kind == TaskKinds::RecStop) stop = &item;
        }
        if (start == nullptr || stop == nullptr || !start->enabled || !stop->enabled) {
            return BroadcastState::Unknown;
        }

        const int startYmd = start->schedule.dueYmd;
        const int startHHMM = start->schedule.dueHHMM;
        const int startSec = (std::clamp)(start->request.dispatchSecond, 0, 59);

        // RecStart.display.end は番組終了日時。RecStop の時刻は
        // 録画終了マージンを含む場合があるため、放送状態判定には使用しない。
        const int endYmd = start->display.endYmd;
        const int endHHMM = start->display.endHHMM;
        if (startYmd <= 0 || startHHMM < 0 || endYmd <= 0 || endHHMM < 0) {
            return BroadcastState::Unknown;
        }
        if (compareTime(startYmd, startHHMM, startSec,
            endYmd, endHHMM, 0) >= 0) {
            return BroadcastState::Unknown;
        }
        if (compareTime(nowYmd, nowHHMM, nowSecond,
            startYmd, startHHMM, startSec) < 0) {
            return BroadcastState::BeforeStart;
        }
        if (compareTime(nowYmd, nowHHMM, nowSecond,
            endYmd, endHHMM, 0) < 0) {
            return BroadcastState::OnAir;
        }
        return BroadcastState::Finished;
        };

    for (const auto& t : snapshot) {
        if (!t.enabled || t.schedule.type != ScheduleType::OneShot) continue;
        if (t.schedule.dueYmd <= 0 || t.schedule.dueHHMM < 0) continue;

        TimeKeeper::TaskRequest req = t.request;
        if (req.dispatchYmd <= 0) req.dispatchYmd = t.schedule.dueYmd;
        if (req.dispatchHHMM < 0) req.dispatchHHMM = t.schedule.dueHHMM;
        req.dispatchSecond = (std::clamp)(req.dispatchSecond, 0, 59);
        if (req.dispatchSecond == 0 && t.schedule.dueSecond > 0) {
            req.dispatchSecond = t.schedule.dueSecond;
        }

        int tolerance = t.schedule.lateToleranceMinutes;
        // 放送中に限り、過去になった準備と開始タスクを受け付ける。
        // -1 は TimeKeeper の「遅延許容を制限しない」。
        // 実行済みの再発火は Connector の実行監視キーで防ぐ。
        // 停止タスクには適用せず、終了日時は変更しない。
        if ((t.request.kind == TaskKinds::RecPrepare ||
            t.request.kind == TaskKinds::RecStart) &&
            getBroadcastState(t.request.instance) == BroadcastState::OnAir) {
            tolerance = -1;
        }

        tk.AddOneShotTask(
            t.schedule.dueYmd,
            t.schedule.dueHHMM,
            req,
            tolerance,
            t.schedule.useTimeWindow,
            t.schedule.windowStartYmd,
            t.schedule.windowStartHHMM,
            t.schedule.windowEndYmd,
            t.schedule.windowEndHHMM);
    }
}

void TaskListManager::ApplyToTimeKeeper(TimeKeeper& tk, const TimeKeeper::Settings& base, const TimeKeeper::Hooks& hooks) const
{
    const auto s = BuildTimeKeeperSettingsFromTasks(base);
    tk.Configure(s, hooks);
    ApplyOneShotsToTimeKeeper(tk);
}

bool TaskListManager::LoadFromIni(const std::wstring& path)
{
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    const std::wstring listSection = L"TaskList";
    const int count = ReadIniInt(path, listSection, L"Count", 0);
    if (count <= 0) {
        Clear();
        return true;
    }

    std::vector<TaskEntry> loaded;
    loaded.reserve(static_cast<size_t>(count));

    for (int i = 0; i < count; ++i) {
        wchar_t key[64]{};
        swprintf_s(key, L"Task_%d", i);

        const std::wstring v = ReadIniString(path, listSection, key, L"");
        if (v.empty()) continue;

        unsigned int kind = 0, inst = 0;
        if (swscanf_s(v.c_str(), L"%u,%u", &kind, &inst) != 2) continue;

        const std::wstring sec = MakeSectionName(kind, inst);

        TaskEntry t{};
        t.name = ReadIniString(path, sec, L"Name", L"");
        t.display.label = ReadIniString(path, sec, L"Label", L"");
        t.display.category = ReadIniString(path, sec, L"Category", L"");
        t.display.target = ReadIniString(path, sec, L"Target", L"");
        t.display.note = ReadIniString(path, sec, L"Note", L"");
        t.display.endYmd = ReadIniInt(path, sec, L"EndYmd", 0);
        t.display.endHHMM = ReadIniInt(path, sec, L"EndHHMM", 0);
        t.reserve.space = ReadIniInt(path, sec, L"Space", 0);
        t.reserve.networkId = static_cast<std::uint16_t>(ReadIniInt(path, sec, L"NetworkId", 0));
        t.reserve.tsId = static_cast<std::uint16_t>(ReadIniInt(path, sec, L"TsId", 0));
        t.reserve.serviceId = static_cast<std::uint16_t>(ReadIniInt(path, sec, L"ServiceId", 0));
        t.reserve.eventId = static_cast<std::uint16_t>(ReadIniInt(path, sec, L"EventId", 0));

        t.reserve.title = ReadIniStringOrEmpty_(path, sec, L"Title");
        t.reserve.titleLabel = ReadIniStringOrEmpty_(path, sec, L"TitleLabel");
        t.reserve.serviceName = ReadIniStringOrEmpty_(path, sec, L"ServiceName");
        t.reserve.summary = ReadIniStringOrEmpty_(path, sec, L"Summary");
        t.reserve.genre = ReadIniStringOrEmpty_(path, sec, L"Genre");

        t.reserve.dateText = ReadIniStringOrEmpty_(path, sec, L"DateText");
        t.reserve.weekdayText = ReadIniStringOrEmpty_(path, sec, L"WeekdayText");
        t.reserve.timeText = ReadIniStringOrEmpty_(path, sec, L"TimeText");

        t.reserve.startDate = ReadIniStringOrEmpty_(path, sec, L"StartDate");
        t.reserve.startTime = ReadIniStringOrEmpty_(path, sec, L"StartTime");
        t.reserve.endDate = ReadIniStringOrEmpty_(path, sec, L"EndDate");
        t.reserve.endTime = ReadIniStringOrEmpty_(path, sec, L"EndTime");
        t.reserve.durationSec = static_cast<std::uint32_t>(ReadIniInt(path, sec, L"DurationSec", 0));

        t.reserve.reserveMode = ReadIniStringOrEmpty_(path, sec, L"ReserveMode");
        t.reserve.reserveSource = ReadIniStringOrEmpty_(path, sec, L"ReserveSource");
        t.reserve.dedupeKey = ReadIniStringOrEmpty_(path, sec, L"DedupeKey");

        t.reserve.disabledByUser = ReadIniBool(path, sec, L"DisabledByUser", false);
        t.reserve.excludeFromAuto = ReadIniBool(path, sec, L"ExcludeFromAuto", false);
        t.reserve.autoReserved = ReadIniBool(path, sec, L"AutoReserved", false);
        t.reserve.excludeKey = ReadIniStringOrEmpty_(path, sec, L"ExcludeKey");

        t.enabled = ReadIniBool(path, sec, L"Enabled", true);

        t.request.kind = static_cast<std::uint32_t>(kind);
        t.request.instance = static_cast<std::uint32_t>(inst);
        t.request.priority = static_cast<TimeKeeper::TaskPriority>(
            ReadIniInt(path, sec, L"Priority", static_cast<int>(TimeKeeper::TaskPriority::Soft)));
        t.request.requiredTuners = static_cast<std::uint8_t>(
            ReadIniInt(path, sec, L"RequiredTuners", 1));

        int preferredTunerIndex = ReadIniInt(path, sec, L"PreferredTunerIndex", -9999);
        if (preferredTunerIndex == -9999) {
            preferredTunerIndex = ReadIniInt(path, sec, L"TunerIndex", -1);
        }
        t.request.preferredTunerIndex = preferredTunerIndex;

        t.request.dispatchYmd = ReadIniInt(path, sec, L"DispatchYmd", 0);
        t.request.dispatchHHMM = ReadIniInt(path, sec, L"DispatchHHMM", 0);
        t.request.dispatchSecond = ReadIniInt(path, sec, L"DispatchSecond", 0);

        t.requiresIdle = ReadIniBool(path, sec, L"RequiresIdle", false);
        t.idleRequiredSeconds = ReadIniInt(path, sec, L"IdleRequiredSeconds", 0);

        t.schedule.type = static_cast<ScheduleType>(
            ReadIniInt(path, sec, L"ScheduleType", static_cast<int>(ScheduleType::None)));
        t.schedule.dailyHour = ReadIniInt(path, sec, L"DailyHour", 3);
        t.schedule.dailyMinute = ReadIniInt(path, sec, L"DailyMinute", 30);
        t.schedule.hourlyAtMinute = ReadIniInt(path, sec, L"HourlyAtMinute", 5);
        t.schedule.intervalMinutes = ReadIniInt(path, sec, L"IntervalMinutes", 0);
        t.schedule.dueYmd = ReadIniInt(path, sec, L"DueYmd", 0);
        t.schedule.dueHHMM = ReadIniInt(path, sec, L"DueHHMM", 0);
        t.schedule.dueSecond = ReadIniInt(path, sec, L"DueSecond", 0);
        t.schedule.lateToleranceMinutes = ReadIniInt(path, sec, L"LateToleranceMinutes", -1);

        t.schedule.useTimeWindow = ReadIniBool(path, sec, L"UseTimeWindow", false);
        t.schedule.windowStartYmd = ReadIniInt(path, sec, L"WindowStartYmd", 0);
        t.schedule.windowStartHHMM = ReadIniInt(path, sec, L"WindowStartHHMM", 0);
        t.schedule.windowStartSecond = ReadIniInt(path, sec, L"WindowStartSecond", 0);
        t.schedule.windowEndYmd = ReadIniInt(path, sec, L"WindowEndYmd", 0);
        t.schedule.windowEndHHMM = ReadIniInt(path, sec, L"WindowEndHHMM", 0);
        t.schedule.windowEndSecond = ReadIniInt(path, sec, L"WindowEndSecond", 0);

        NormalizeTaskEntry_(t);
        loaded.push_back(std::move(t));
    }

    {
        std::lock_guard lk(mtx_);
        tasks_ = std::move(loaded);
    }
    return true;
}

bool TaskListManager::SaveToIni(const std::wstring& path) const
{
    if (path.empty()) {
        return false;
    }

    const auto snapshot = GetSnapshot();

    // revision は次段階で TaskListManager::GetRevision() を追加して差し替える。
    // 現段階では保存テンプレを有効化することを優先する。
    const TaskIniDocument doc = MakeTaskIniDocument_(snapshot, 0);

    return WriteTaskIniDocumentAtomic_(path, doc);
}