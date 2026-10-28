// TaskSystemConnector.cpp
#include "TaskSystemConnector.hpp"
#include "IBonDriver2.h"

#include "DebugManager.h"
#include "SystemInfoManager.hpp"
#include "EpgPortalReservationApiServer.hpp"
#include "EpgHtmlPortal.hpp"
#include "TaskReservationCache.hpp"
#include "Reservation_ProgramTimeUtil.hpp"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <algorithm>
#include <unordered_set>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <iterator>
#include <cwctype>

namespace
{
    int CompareDateTime_(int ymd1, int hhmm1, int ymd2, int hhmm2);

    bool ProgramAdjustRequestMatchesUpdatedChannel_(
        const TaskSystemConnector::ProgramReservationAdjustRequest& p,
        const TaskSystemConnector::EpgUpdateTaskRefreshRequest& req) noexcept;

    std::vector<std::filesystem::path> BuildReservationChannelListCandidates_(
        int space);

    bool IsEpgTaskKind_(std::uint32_t kind) noexcept
    {
        return kind == TaskKinds::EpgFull || kind == TaskKinds::EpgLight;
    }

    constexpr std::uint32_t kEpgDailyTemplateInstance_ = 0;
    constexpr std::uint32_t kEpgOneShotInstance_ = 1;
    constexpr int kDefaultEpgWindowMinutes_ = 30;

    bool IsEpgAutoReservationRequest_(const TimeKeeper::TaskRequest& r) noexcept
    {
        return IsEpgTaskKind_(r.kind) &&
            (r.instance == kEpgDailyTemplateInstance_ ||
                r.instance == kEpgOneShotInstance_);
    }

    bool IsRecordTaskKind_(std::uint32_t kind) noexcept
    {
        return kind == TaskKinds::RecPrepare
            || kind == TaskKinds::RecStart
            || kind == TaskKinds::RecStop;
    }

    const wchar_t* GetTaskKindName_(std::uint32_t kind) noexcept
    {
        switch (kind) {
        case TaskKinds::EpgFull:     return L"EpgFull";
        case TaskKinds::EpgLight:    return L"EpgLight";
        case TaskKinds::RecPrepare:  return L"RecPrepare";
        case TaskKinds::RecStart:    return L"RecStart";
        case TaskKinds::RecStop:     return L"RecStop";
        case TaskKinds::CleanupLogs: return L"CleanupLogs";
        default:                     return L"Unknown";
        }
    }

    const wchar_t* GetReservationText_(std::uint32_t kind) noexcept
    {
        if (IsEpgTaskKind_(kind)) {
            return L"EPG予約";
        }
        if (IsRecordTaskKind_(kind)) {
            return L"番組予約";
        }
        return L"予約";
    }

    std::wstring GetTaskCategoryText_(std::uint32_t kind)
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

    std::wstring MakeTaskLabel_(const TaskListManager::TaskEntry& t)
    {
        if (!t.display.label.empty()) {
            return t.display.label;
        }
        if (!t.name.empty()) {
            return t.name;
        }
        return std::wstring(GetTaskKindName_(t.request.kind));
    }

    std::wstring MakeScheduleText_(const TaskListManager::TaskEntry& t)
    {
        wchar_t buf[160]{};

        switch (t.schedule.type) {
        case TaskListManager::ScheduleType::Daily:
            swprintf_s(buf, L"毎日 %02d:%02d", t.schedule.dailyHour, t.schedule.dailyMinute);
            return std::wstring(buf);

        case TaskListManager::ScheduleType::Hourly:
            swprintf_s(buf, L"毎時 %02d分", t.schedule.hourlyAtMinute);
            return std::wstring(buf);

        case TaskListManager::ScheduleType::Interval:
            swprintf_s(buf, L"%d分ごと", t.schedule.intervalMinutes);
            return std::wstring(buf);

        case TaskListManager::ScheduleType::OneShot:
            if (t.schedule.dueYmd > 0) {
                swprintf_s(
                    buf,
                    L"%08d %02d:%02d",
                    t.schedule.dueYmd,
                    t.schedule.dueHHMM / 100,
                    t.schedule.dueHHMM % 100);
            }
            else {
                swprintf_s(
                    buf,
                    L"%02d:%02d",
                    t.schedule.dueHHMM / 100,
                    t.schedule.dueHHMM % 100);
            }
            return std::wstring(buf);

        default:
            return L"-";
        }
    }

    bool HasEnabledEpgTask_(const TaskListManager& tl)
    {
        const auto snap = tl.GetSnapshot();
        for (const auto& t : snap) {
            if (t.enabled && IsEpgTaskKind_(t.request.kind)) {
                return true;
            }
        }
        return false;
    }

    bool IsTaskNameChanged_(const TaskListManager::TaskEntry& oldTask,
        const TaskListManager::TaskEntry& newTask)
    {
        return oldTask.name != newTask.name
            || oldTask.display.label != newTask.display.label;
    }

    void WriteTaskRegisteredLog_(const TaskListManager::TaskEntry& t, bool updated)
    {
        const std::wstring label = MakeTaskLabel_(t);
        const std::wstring schedule = MakeScheduleText_(t);

        SystemInfoLogInfo(
            L"%lsを%lsしました。name=%ls schedule=%ls kind=%u instance=%u enabled=%d reqTuners=%u tunerIndex=%d",
            GetReservationText_(t.request.kind),
            updated ? L"更新" : L"登録",
            label.c_str(),
            schedule.c_str(),
            static_cast<unsigned>(t.request.kind),
            static_cast<unsigned>(t.request.instance),
            t.enabled ? 1 : 0,
            static_cast<unsigned>((t.request.requiredTuners == 0) ? 1 : t.request.requiredTuners),
            t.request.preferredTunerIndex);
    }

    void WriteTaskRenamedLog_(const TaskListManager::TaskEntry& oldTask,
        const TaskListManager::TaskEntry& newTask)
    {
        SystemInfoLogInfo(
            L"%ls名を変更しました。kind=%u instance=%u old=%ls new=%ls",
            GetReservationText_(newTask.request.kind),
            static_cast<unsigned>(newTask.request.kind),
            static_cast<unsigned>(newTask.request.instance),
            MakeTaskLabel_(oldTask).c_str(),
            MakeTaskLabel_(newTask).c_str());
    }

    void WriteTaskDeletedLog_(std::uint32_t kind, std::uint32_t instance, bool removed)
    {
        if (removed) {
            SystemInfoLogInfo(
                L"%lsを削除しました。kind=%u instance=%u",
                GetReservationText_(kind),
                static_cast<unsigned>(kind),
                static_cast<unsigned>(instance));
        }
        else {
            SystemInfoLogWarn(
                L"%lsの削除に失敗しました。kind=%u instance=%u は未登録です。",
                GetReservationText_(kind),
                static_cast<unsigned>(kind),
                static_cast<unsigned>(instance));
        }
    }

    void WriteTaskExecutionStartLog_(const TimeKeeper::TaskRequest& r, int tunerIndex)
    {
        SystemInfoLogInfo(
            L"%lsを実行しました。kind=%u instance=%u tuner=%d requiredTuners=%u preferredTunerIndex=%d",
            GetReservationText_(r.kind),
            static_cast<unsigned>(r.kind),
            static_cast<unsigned>(r.instance),
            tunerIndex,
            static_cast<unsigned>((r.requiredTuners == 0) ? 1 : r.requiredTuners),
            r.preferredTunerIndex);
    }

    void WriteTaskExecutionFailLog_(const TimeKeeper::TaskRequest& r, const wchar_t* reason)
    {
        SystemInfoLogWarn(
            L"%lsの起動に失敗しました。kind=%u instance=%u reason=%ls",
            GetReservationText_(r.kind),
            static_cast<unsigned>(r.kind),
            static_cast<unsigned>(r.instance),
            reason ? reason : L"(unknown)");
    }

    bool GetLocalNowYmdHHMM_(int& outYmd, int& outHHMM)
    {
        SYSTEMTIME st{};
        ::GetLocalTime(&st);

        outYmd =
            static_cast<int>(st.wYear) * 10000 +
            static_cast<int>(st.wMonth) * 100 +
            static_cast<int>(st.wDay);

        outHHMM =
            static_cast<int>(st.wHour) * 100 +
            static_cast<int>(st.wMinute);

        return outYmd > 0 && outHHMM >= 0;
    }

    std::wstring FormatYmdHHMMForLog_(int ymd, int hhmm)
    {
        if (ymd <= 0 || hhmm < 0) {
            return L"(invalid)";
        }

        wchar_t buf[64]{};
        swprintf_s(
            buf,
            L"%08d %02d:%02d",
            ymd,
            hhmm / 100,
            hhmm % 100);

        return std::wstring(buf);
    }

    bool IsDateTimeInRangeHalfOpen_(
        int targetYmd,
        int targetHHMM,
        int startYmd,
        int startHHMM,
        int endYmd,
        int endHHMM)
    {
        return CompareDateTime_(targetYmd, targetHHMM, startYmd, startHHMM) >= 0 &&
            CompareDateTime_(targetYmd, targetHHMM, endYmd, endHHMM) < 0;
    }

    bool IsOneShotRecordPrepareOrStart_(std::uint32_t kind) noexcept
    {
        return kind == TaskKinds::RecPrepare ||
            kind == TaskKinds::RecStart;
    }

    bool ShiftLocalYmdHHMM_(int ymd, int hhmm, int deltaMinutes, int& outYmd, int& outHHMM)
    {
        SYSTEMTIME local{};
        local.wYear = static_cast<WORD>(ymd / 10000);
        local.wMonth = static_cast<WORD>((ymd / 100) % 100);
        local.wDay = static_cast<WORD>(ymd % 100);
        local.wHour = static_cast<WORD>(hhmm / 100);
        local.wMinute = static_cast<WORD>(hhmm % 100);
        local.wSecond = 0;
        local.wMilliseconds = 0;

        SYSTEMTIME utc{};
        if (!::TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) {
            return false;
        }

        FILETIME ft{};
        if (!::SystemTimeToFileTime(&utc, &ft)) {
            return false;
        }

        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;

        constexpr LONGLONG kMinute100ns = 60LL * 1000LL * 1000LL * 10LL;
        LONGLONG v = static_cast<LONGLONG>(u.QuadPart);
        v += static_cast<LONGLONG>(deltaMinutes) * kMinute100ns;
        if (v < 0) {
            v = 0;
        }

        u.QuadPart = static_cast<ULONGLONG>(v);
        ft.dwLowDateTime = u.LowPart;
        ft.dwHighDateTime = u.HighPart;

        if (!::FileTimeToSystemTime(&ft, &utc)) {
            return false;
        }

        SYSTEMTIME shiftedLocal{};
        if (!::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &shiftedLocal)) {
            return false;
        }

        outYmd = static_cast<int>(shiftedLocal.wYear) * 10000
            + static_cast<int>(shiftedLocal.wMonth) * 100
            + static_cast<int>(shiftedLocal.wDay);

        outHHMM = static_cast<int>(shiftedLocal.wHour) * 100
            + static_cast<int>(shiftedLocal.wMinute);

        return true;
    }

    int CalcTomorrowYmdForTask_()
    {
        SYSTEMTIME st{};
        ::GetLocalTime(&st);

        FILETIME ft{};
        if (!::SystemTimeToFileTime(&st, &ft)) {
            return static_cast<int>(st.wYear) * 10000
                + static_cast<int>(st.wMonth) * 100
                + static_cast<int>(st.wDay);
        }

        ULARGE_INTEGER ui{};
        ui.LowPart = ft.dwLowDateTime;
        ui.HighPart = ft.dwHighDateTime;

        constexpr ULONGLONG OneDay100ns = 24ULL * 60ULL * 60ULL * 10ULL * 1000ULL * 1000ULL;
        ui.QuadPart += OneDay100ns;

        FILETIME ft2{};
        ft2.dwLowDateTime = ui.LowPart;
        ft2.dwHighDateTime = ui.HighPart;

        SYSTEMTIME st2{};
        if (!::FileTimeToSystemTime(&ft2, &st2)) {
            return static_cast<int>(st.wYear) * 10000
                + static_cast<int>(st.wMonth) * 100
                + static_cast<int>(st.wDay);
        }

        return static_cast<int>(st2.wYear) * 10000
            + static_cast<int>(st2.wMonth) * 100
            + static_cast<int>(st2.wDay);
    }

    int ResolveNextDueYmdForTask_(int hhmm)
    {
        SYSTEMTIME st{};
        ::GetLocalTime(&st);

        const int nowHHMM = static_cast<int>(st.wHour) * 100 + static_cast<int>(st.wMinute);
        const int todayYmd = static_cast<int>(st.wYear) * 10000
            + static_cast<int>(st.wMonth) * 100
            + static_cast<int>(st.wDay);

        return (nowHHMM <= hhmm) ? todayYmd : CalcTomorrowYmdForTask_();
    }

    std::wstring BuildEpgWindowNote_(int windowMinutes)
    {
        wchar_t buf[96]{};
        swprintf_s(buf, L"開始時刻から%d分以内に起動", windowMinutes);
        return std::wstring(buf);
    }

    int ParseIsoDateToYmd_(const std::wstring& s) noexcept
    {
        unsigned y = 0, m = 0, d = 0;
        if (swscanf_s(s.c_str(), L"%u-%u-%u", &y, &m, &d) == 3) {
            return static_cast<int>(y * 10000 + m * 100 + d);
        }
        return 0;
    }

    int ParseIsoTimeToHHMM_(const std::wstring& s) noexcept
    {
        unsigned hh = 0;
        unsigned mm = 0;
        unsigned ss = 0;

        const int n = swscanf_s(s.c_str(), L"%u:%u:%u", &hh, &mm, &ss);
        if (n >= 2) {
            if (hh <= 23 && mm <= 59) {
                return static_cast<int>(hh * 100 + mm);
            }
        }

        return -1;
    }

    std::uint32_t HashFNV1a32_(const std::wstring& s) noexcept
    {
        std::uint32_t h = 2166136261u;
        for (wchar_t ch : s) {
            h ^= static_cast<std::uint32_t>(ch);
            h *= 16777619u;
        }
        return (h == 0) ? 1u : h;
    }

    std::wstring BuildReserveDedupeKey_(const TaskSystemConnector::EpgReserveCommandEx& cmd)
    {
        if (!cmd.dedupeKey.empty()) {
            return cmd.dedupeKey;
        }

        wchar_t buf[256]{};
        swprintf_s(
            buf,
            L"%d-%u-%u-%u-%u-%ls-%ls",
            cmd.space,
            static_cast<unsigned>(cmd.networkId),
            static_cast<unsigned>(cmd.tsId),
            static_cast<unsigned>(cmd.serviceId),
            static_cast<unsigned>(cmd.eventId),
            cmd.startDate.c_str(),
            cmd.startTime.c_str());


        return std::wstring(buf);
    }

    std::wstring BuildReserveDedupeKeyCanonical_(
        const TaskSystemConnector::EpgReserveCommandEx& cmd,
        const Reservation_ProgramTimeUtil::ProgramTimePlan& plan)
    {
        wchar_t buf[320]{};
        swprintf_s(
            buf,
            L"%d-%u-%u-%u-%u-%ls-%ls",
            cmd.space,
            static_cast<unsigned>(cmd.networkId),
            static_cast<unsigned>(cmd.tsId),
            static_cast<unsigned>(cmd.serviceId),
            static_cast<unsigned>(cmd.eventId),
            plan.startDate.c_str(),
            plan.startTime.c_str());

        return std::wstring(buf);
    }


    std::filesystem::path GetConfigIniPath_()
    {
        wchar_t modulePath[MAX_PATH]{};
        const DWORD n = ::GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            return {};
        }
        return std::filesystem::path(modulePath).parent_path() / L"config.ini";
    }


    enum class ReservationTunerPurpose_
    {
        Record,
        Epg,
        Any
    };

    enum class TunerUseRole_
    {
        Generic = 0,
        Record = 1,
        Epg = 2,
        Scan = 3,
        Watch = 4,
        TimeShift = 5
    };

    std::wstring TrimConfigText_(std::wstring s)
    {
        auto isWs = [](wchar_t ch) noexcept {
            return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
            };

        while (!s.empty() && isWs(s.front())) {
            s.erase(s.begin());
        }

        while (!s.empty() && isWs(s.back())) {
            s.pop_back();
        }

        return s;
    }

    std::filesystem::path ResolveConfigRelativePath_(
        const std::filesystem::path& configPath,
        const std::wstring& rawPath)
    {
        const std::wstring text = TrimConfigText_(rawPath);
        if (text.empty()) {
            return {};
        }

        std::filesystem::path p(text);
        if (p.is_absolute()) {
            return p.lexically_normal();
        }

        if (!configPath.empty()) {
            return (configPath.parent_path() / p).lexically_normal();
        }

        return p.lexically_normal();
    }

    bool IsTunerUseRoleAllowed_(
        int useRole,
        ReservationTunerPurpose_ purpose) noexcept
    {
        const auto role = static_cast<TunerUseRole_>(useRole);

        switch (purpose) {
        case ReservationTunerPurpose_::Record:
            return role == TunerUseRole_::Generic ||
                role == TunerUseRole_::Record;

        case ReservationTunerPurpose_::Epg:
            return role == TunerUseRole_::Generic ||
                role == TunerUseRole_::Epg;

        case ReservationTunerPurpose_::Any:
        default:
            return role == TunerUseRole_::Generic ||
                role == TunerUseRole_::Record ||
                role == TunerUseRole_::Epg ||
                role == TunerUseRole_::Scan ||
                role == TunerUseRole_::Watch;
        }
    }

    std::vector<int> LoadEnabledTunerIndicesFromConfig_(
        ReservationTunerPurpose_ purpose)
    {
        std::vector<int> out;

        const auto configPath = GetConfigIniPath_();
        if (configPath.empty()) {
            DebugLogFormat(
                L"[TaskSystemConnector][TUNER-CANDIDATE] config.ini path is empty");
            return out;
        }

        std::error_code configEc;
        if (!std::filesystem::exists(configPath, configEc) || configEc) {
            DebugLogFormat(
                L"[TaskSystemConnector][TUNER-CANDIDATE] config.ini not found path=%ls ec=%d",
                configPath.c_str(),
                static_cast<int>(configEc.value()));
            return out;
        }

        const int count = static_cast<int>(
            ::GetPrivateProfileIntW(
                L"Tuners",
                L"Count",
                0,
                configPath.c_str()));

        if (count <= 0) {
            DebugLogFormat(
                L"[TaskSystemConnector][TUNER-CANDIDATE] Tuners.Count is invalid count=%d",
                count);
            return out;
        }

        out.reserve(static_cast<std::size_t>(count));

        for (int i = 0; i < count; ++i) {
            const std::wstring tunerSection =
                L"Tuner" + std::to_wstring(i);

            const std::wstring keyEnable =
                L"Enable" + std::to_wstring(i);

            const std::wstring keyAllowParallel =
                L"AllowParallel" + std::to_wstring(i);

            const std::wstring keyUseRole =
                L"UseRole" + std::to_wstring(i);

            const std::wstring keyInternalBon =
                L"InternalBonDriverPath" + std::to_wstring(i);

            const std::wstring keyLegacyBon =
                L"BonDriverPath" + std::to_wstring(i);

            const bool enabled =
                (::GetPrivateProfileIntW(
                    tunerSection.c_str(),
                    keyEnable.c_str(),
                    1,
                    configPath.c_str()) != 0);

            if (!enabled) {
                DebugLogFormat(
                    L"[TaskSystemConnector][TUNER-CANDIDATE-SKIP] tuner=%d disabled",
                    i);
                continue;
            }

            const bool allowParallel =
                (::GetPrivateProfileIntW(
                    tunerSection.c_str(),
                    keyAllowParallel.c_str(),
                    1,
                    configPath.c_str()) != 0);

            if (!allowParallel) {
                DebugLogFormat(
                    L"[TaskSystemConnector][TUNER-CANDIDATE-SKIP] tuner=%d AllowParallel=0",
                    i);
                continue;
            }

            const int useRole =
                static_cast<int>(::GetPrivateProfileIntW(
                    tunerSection.c_str(),
                    keyUseRole.c_str(),
                    0,
                    configPath.c_str()));

            if (!IsTunerUseRoleAllowed_(useRole, purpose)) {
                DebugLogFormat(
                    L"[TaskSystemConnector][TUNER-CANDIDATE-SKIP] tuner=%d role=%d purpose=%d",
                    i,
                    useRole,
                    static_cast<int>(purpose));
                continue;
            }

            wchar_t internalBonPathBuf[1024]{};
            ::GetPrivateProfileStringW(
                tunerSection.c_str(),
                keyInternalBon.c_str(),
                L"",
                internalBonPathBuf,
                static_cast<DWORD>(_countof(internalBonPathBuf)),
                configPath.c_str());

            if (internalBonPathBuf[0] == L'\0') {
                ::GetPrivateProfileStringW(
                    tunerSection.c_str(),
                    keyLegacyBon.c_str(),
                    L"",
                    internalBonPathBuf,
                    static_cast<DWORD>(_countof(internalBonPathBuf)),
                    configPath.c_str());
            }

            const std::filesystem::path internalBonPath =
                ResolveConfigRelativePath_(configPath, internalBonPathBuf);

            if (internalBonPath.empty()) {
                DebugLogFormat(
                    L"[TaskSystemConnector][TUNER-CANDIDATE-SKIP] tuner=%d internalBon is empty",
                    i);
                continue;
            }

            std::error_code bonEc;
            const bool bonExists =
                std::filesystem::exists(internalBonPath, bonEc);

            if (!bonExists || bonEc) {
                DebugLogFormat(
                    L"[TaskSystemConnector][TUNER-CANDIDATE-SKIP] tuner=%d internalBon not found path=%ls ec=%d",
                    i,
                    internalBonPath.c_str(),
                    static_cast<int>(bonEc.value()));
                continue;
            }

            out.push_back(i);

            DebugLogFormat(
                L"[TaskSystemConnector][TUNER-CANDIDATE-OK] tuner=%d role=%d purpose=%d internalBon=%ls",
                i,
                useRole,
                static_cast<int>(purpose),
                internalBonPath.c_str());
        }

        return out;
    }

    int ReadReservationPreferredTunerFromConfig_()
    {
        // [Reservation] PreferredTuner は廃止。
        // TaskSystemConnector 側は固定で auto にする。
        // 明示指定が必要な場合は req.preferredTunerIndex / cmd.preferredTunerIndex で渡す。
        return -1;
    }

    int ReadReservationStartAdvanceSecFromConfig_()
    {
        const auto configPath = GetConfigIniPath_();
        if (configPath.empty() || !std::filesystem::exists(configPath)) {
            return 30;
        }

        const int v = static_cast<int>(
            ::GetPrivateProfileIntW(
                L"ReservationExecution",
                L"PrepareSeconds",
                30,
                configPath.c_str()));

        return (std::clamp)(v, 0, 3600);
    }

    int ReadReservationStartAdjustSecFromConfig_()
    {
        // [Reservation] StartAdjustSec は廃止。
        // 現段階では予約時刻の微調整は行わない。
        return 0;
    }

    int ReadReservationEndAdjustMinFromConfig_()
    {
        // [Reservation] EndAdjustMin は廃止。
        // 現段階では終了時刻の延長・短縮は行わない。
        return 0;
    }

    int ReadReservationConflictPolicyFromConfig_()
    {
        const auto configPath = GetConfigIniPath_();
        if (configPath.empty() || !std::filesystem::exists(configPath)) {
            return 0;
        }

        const int v = static_cast<int>(
            ::GetPrivateProfileIntW(
                L"TunerGlobal",
                L"ConflictPolicy",
                0,
                configPath.c_str()));

        return (std::clamp)(v, 0, 2);
    }

    bool ReadNetworkEnableServerFromConfig_()
    {
        const auto configPath = GetConfigIniPath_();
        if (configPath.empty() || !std::filesystem::exists(configPath)) {
            return false;
        }

        return ::GetPrivateProfileIntW(
            L"Network",
            L"EnableServer",
            0,
            configPath.c_str()) != 0;
    }

    bool ReadNetworkAutoStartServerFromConfig_()
    {
        const auto configPath = GetConfigIniPath_();
        if (configPath.empty() || !std::filesystem::exists(configPath)) {
            return false;
        }

        return ::GetPrivateProfileIntW(
            L"Network",
            L"AutoStartServer",
            0,
            configPath.c_str()) != 0;
    }

    int ReadNetworkHttpPortFromConfig_()
    {
        const auto configPath = GetConfigIniPath_();
        if (configPath.empty() || !std::filesystem::exists(configPath)) {
            return 37781;
        }

        const int v = static_cast<int>(
            ::GetPrivateProfileIntW(
                L"Network",
                L"HttpPort",
                37781,
                configPath.c_str()));

        return (std::clamp)(v, 1, 65535);
    }

    std::filesystem::path GetExeDirectory_()
    {
        const auto configPath = GetConfigIniPath_();
        if (!configPath.empty()) {
            return configPath.parent_path();
        }

        wchar_t modulePath[MAX_PATH]{};
        const DWORD n = ::GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            return std::filesystem::current_path();
        }
        return std::filesystem::path(modulePath).parent_path();
    }

    std::wstring TrimReservationChannelField_(std::wstring_view s)
    {
        std::size_t first = 0;
        while (first < s.size()) {
            const wchar_t ch = s[first];
            if (ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n') {
                break;
            }
            ++first;
        }

        std::size_t last = s.size();
        while (last > first) {
            const wchar_t ch = s[last - 1];
            if (ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n') {
                break;
            }
            --last;
        }

        return std::wstring(s.substr(first, last - first));
    }

    std::vector<std::wstring> SplitReservationChannelCsvLine_(std::wstring_view line)
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
                fields.push_back(TrimReservationChannelField_(current));
                current.clear();
                continue;
            }

            current.push_back(ch);
        }

        fields.push_back(TrimReservationChannelField_(current));
        return fields;
    }

    bool TryParseReservationUInt_(const std::wstring& s, std::uint32_t& out) noexcept
    {
        out = 0;

        const std::wstring v = TrimReservationChannelField_(s);
        if (v.empty()) {
            return false;
        }

        wchar_t* end = nullptr;
        const unsigned long parsed = std::wcstoul(v.c_str(), &end, 0);
        if (end == v.c_str() || *end != L'\0') {
            return false;
        }

        out = static_cast<std::uint32_t>(parsed);
        return true;
    }

    bool MultiByteToWideReservation_(
        UINT codePage,
        const char* data,
        int size,
        std::wstring& out,
        DWORD flags)
    {
        out.clear();

        if (data == nullptr || size <= 0) {
            return true;
        }

        const int needed = ::MultiByteToWideChar(
            codePage,
            flags,
            data,
            size,
            nullptr,
            0);

        if (needed <= 0) {
            return false;
        }

        out.resize(static_cast<std::size_t>(needed));

        const int written = ::MultiByteToWideChar(
            codePage,
            flags,
            data,
            size,
            out.data(),
            needed);

        return written == needed;
    }

    bool BytesToUtf16LeReservation_(
        const std::string& bytes,
        std::size_t offset,
        std::wstring& out)
    {
        out.clear();

        if (offset > bytes.size()) {
            return false;
        }

        const std::size_t remain = bytes.size() - offset;
        if ((remain % 2) != 0) {
            return false;
        }

        out.reserve(remain / 2);

        for (std::size_t i = offset; i + 1 < bytes.size(); i += 2) {
            const auto lo = static_cast<unsigned char>(bytes[i]);
            const auto hi = static_cast<unsigned char>(bytes[i + 1]);
            const wchar_t ch = static_cast<wchar_t>(
                static_cast<unsigned>(lo) |
                (static_cast<unsigned>(hi) << 8));
            out.push_back(ch);
        }

        return true;
    }

    bool ReadReservationTextFileWide_(
        const std::filesystem::path& path,
        std::wstring& out)
    {
        out.clear();

        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) {
            return false;
        }

        std::string bytes(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());

        if (bytes.empty()) {
            return true;
        }

        const auto b0 = static_cast<unsigned char>(bytes[0]);
        const auto b1 = bytes.size() >= 2 ? static_cast<unsigned char>(bytes[1]) : 0;
        const auto b2 = bytes.size() >= 3 ? static_cast<unsigned char>(bytes[2]) : 0;

        if (bytes.size() >= 3 && b0 == 0xEF && b1 == 0xBB && b2 == 0xBF) {
            return MultiByteToWideReservation_(
                CP_UTF8,
                bytes.data() + 3,
                static_cast<int>(bytes.size() - 3),
                out,
                MB_ERR_INVALID_CHARS);
        }

        if (bytes.size() >= 2 && b0 == 0xFF && b1 == 0xFE) {
            return BytesToUtf16LeReservation_(bytes, 2, out);
        }

        if (MultiByteToWideReservation_(
            CP_UTF8,
            bytes.data(),
            static_cast<int>(bytes.size()),
            out,
            MB_ERR_INVALID_CHARS)) {
            return true;
        }

        return MultiByteToWideReservation_(
            932,
            bytes.data(),
            static_cast<int>(bytes.size()),
            out,
            0);
    }

    bool ReservationIdsMatch_(
        std::uint32_t idA,
        std::uint32_t idB,
        const TaskSystemConnector::EpgReserveCommandEx& cmd) noexcept
    {
        const std::uint32_t nid = static_cast<std::uint32_t>(cmd.networkId);
        const std::uint32_t tsid = static_cast<std::uint32_t>(cmd.tsId);

        if (nid == 0 && tsid == 0) {
            return true;
        }

        if (nid != 0 && tsid != 0) {
            return (idA == nid && idB == tsid) ||
                (idA == tsid && idB == nid);
        }

        if (nid != 0) {
            return idA == nid || idB == nid;
        }

        return idA == tsid || idB == tsid;
    }

    bool TryMatchReservationChannelLayout_(
        const std::vector<std::wstring>& fields,
        const TaskSystemConnector::EpgReserveCommandEx& cmd,
        std::size_t serviceIdIndex,
        std::size_t idAIndex,
        std::size_t idBIndex)
    {
        std::size_t maxIndex = serviceIdIndex;
        if (idAIndex > maxIndex) maxIndex = idAIndex;
        if (idBIndex > maxIndex) maxIndex = idBIndex;

        if (fields.size() <= maxIndex) {
            return false;
        }

        // channellist の第2列が space の形式を優先する。
        // space列が無い形式でも壊さないため、読めた場合だけ照合する。
        if (fields.size() > 1) {
            std::uint32_t parsedSpace = 0;
            if (TryParseReservationUInt_(fields[1], parsedSpace)) {
                if (static_cast<int>(parsedSpace) != cmd.space) {
                    return false;
                }
            }
        }

        std::uint32_t sid = 0;
        if (!TryParseReservationUInt_(fields[serviceIdIndex], sid)) {
            return false;
        }

        if (sid != static_cast<std::uint32_t>(cmd.serviceId)) {
            return false;
        }

        std::uint32_t idA = 0;
        std::uint32_t idB = 0;
        if (!TryParseReservationUInt_(fields[idAIndex], idA) ||
            !TryParseReservationUInt_(fields[idBIndex], idB)) {
            return false;
        }

        return ReservationIdsMatch_(idA, idB, cmd);
    }

    bool TryResolveServiceNameFromChannelLine_(
        const std::wstring& line,
        const TaskSystemConnector::EpgReserveCommandEx& cmd,
        std::wstring& outServiceName)
    {
        outServiceName.clear();

        const std::wstring trimmed = TrimReservationChannelField_(line);
        if (trimmed.empty()) {
            return false;
        }

        const wchar_t first = trimmed.front();
        if (first == L'#' || first == L';') {
            return false;
        }

        const auto fields = SplitReservationChannelCsvLine_(trimmed);
        if (fields.empty()) {
            return false;
        }

        // 現行想定:
        // serviceName, space, physicalCh, remote, ..., serviceId, networkId, tsId, ...
        if (TryMatchReservationChannelLayout_(fields, cmd, 5, 6, 7)) {
            outServiceName = fields[0];
            return !outServiceName.empty();
        }

        // 旧/別形式想定:
        // serviceName, space, physicalCh, ..., serviceId, networkId, tsId, ...
        if (TryMatchReservationChannelLayout_(fields, cmd, 4, 5, 6)) {
            outServiceName = fields[0];
            return !outServiceName.empty();
        }

        return false;
    }

    static bool IsReservationAsciiOrFullWidthDigit_(
        wchar_t ch) noexcept
    {
        return
            (ch >= L'0' && ch <= L'9') ||
            (ch >= L'０' && ch <= L'９');
    }

    static bool IsSidOnlyReservationServiceName_(
        const std::wstring& serviceName)
    {
        const std::wstring text =
            TrimReservationChannelField_(
                serviceName);

        if (text.size() < 3) {
            return false;
        }

        const bool hasSidPrefix =
            (text[0] == L'S' || text[0] == L's') &&
            (text[1] == L'I' || text[1] == L'i') &&
            (text[2] == L'D' || text[2] == L'd');

        if (!hasSidPrefix) {
            return false;
        }

        for (std::size_t i = 3; i < text.size(); ++i) {
            const wchar_t ch = text[i];

            if (ch == L' ' ||
                ch == L'\t' ||
                ch == L'　' ||
                IsReservationAsciiOrFullWidthDigit_(ch)) {
                continue;
            }

            return false;
        }

        return true;
    }

    struct ReservationChannelIdentity_
    {
        std::wstring serviceName;

        int space = 0;
        std::uint32_t serviceId = 0;
        std::uint32_t networkId = 0;
        std::uint32_t tsId = 0;
    };

    static bool TryReadReservationChannelIdentityLayout_(
        const std::vector<std::wstring>& fields,
        const TaskSystemConnector::EpgReserveCommandEx& cmd,
        std::size_t serviceIdIndex,
        std::size_t networkIdIndex,
        std::size_t tsIdIndex,
        ReservationChannelIdentity_& out)
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

        int parsedSpace = cmd.space;

        if (fields.size() > 1) {
            std::uint32_t tmpSpace = 0;

            if (TryParseReservationUInt_(fields[1], tmpSpace)) {
                parsedSpace = static_cast<int>(tmpSpace);

                if (cmd.space >= 0 &&
                    parsedSpace != cmd.space) {
                    return false;
                }
            }
        }

        std::uint32_t parsedServiceId = 0;
        std::uint32_t parsedNetworkId = 0;
        std::uint32_t parsedTsId = 0;

        if (!TryParseReservationUInt_(fields[serviceIdIndex], parsedServiceId) ||
            !TryParseReservationUInt_(fields[networkIdIndex], parsedNetworkId) ||
            !TryParseReservationUInt_(fields[tsIdIndex], parsedTsId)) {
            return false;
        }

        if (cmd.serviceId != 0 &&
            parsedServiceId != static_cast<std::uint32_t>(cmd.serviceId)) {
            return false;
        }

        if (cmd.networkId != 0 &&
            parsedNetworkId != static_cast<std::uint32_t>(cmd.networkId)) {
            return false;
        }

        if (cmd.tsId != 0 &&
            parsedTsId != static_cast<std::uint32_t>(cmd.tsId)) {
            return false;
        }

        out.serviceName =
            fields.empty()
            ? std::wstring{}
        : fields[0];

        out.space = parsedSpace;
        out.serviceId = parsedServiceId;
        out.networkId = parsedNetworkId;
        out.tsId = parsedTsId;

        return
            !out.serviceName.empty() &&
            out.serviceId != 0 &&
            out.networkId != 0 &&
            out.tsId != 0;
    }

    static bool TryResolveReservationChannelIdentityFromLine_(
        const std::wstring& line,
        const TaskSystemConnector::EpgReserveCommandEx& cmd,
        ReservationChannelIdentity_& out)
    {
        out = {};

        const std::wstring trimmed =
            TrimReservationChannelField_(line);

        if (trimmed.empty()) {
            return false;
        }

        const wchar_t first = trimmed.front();

        if (first == L'#' || first == L';') {
            return false;
        }

        const auto fields =
            SplitReservationChannelCsvLine_(trimmed);

        if (fields.empty()) {
            return false;
        }

        // 現行想定:
        // serviceName, space, physicalCh, remote, ..., serviceId, networkId, tsId, ...
        if (TryReadReservationChannelIdentityLayout_(
            fields,
            cmd,
            5,
            6,
            7,
            out)) {
            return true;
        }

        // 旧/別形式想定:
        // serviceName, space, physicalCh, ..., serviceId, networkId, tsId, ...
        if (TryReadReservationChannelIdentityLayout_(
            fields,
            cmd,
            4,
            5,
            6,
            out)) {
            return true;
        }

        return false;
    }

    static bool ResolveReservationChannelIdentityFromChannelList_(
        const TaskSystemConnector::EpgReserveCommandEx& cmd,
        ReservationChannelIdentity_& out)
    {
        out = {};

        if (cmd.serviceId == 0) {
            return false;
        }

        const auto candidates =
            BuildReservationChannelListCandidates_(
                cmd.space);

        for (const auto& path : candidates) {
            std::error_code ec;

            if (!std::filesystem::exists(path, ec) || ec) {
                continue;
            }

            std::wstring text;

            if (!ReadReservationTextFileWide_(path, text)) {
                continue;
            }

            std::wistringstream iss(text);
            std::wstring line;
            std::size_t lineNo = 0;

            while (std::getline(iss, line)) {
                ++lineNo;

                ReservationChannelIdentity_ identity{};

                if (!TryResolveReservationChannelIdentityFromLine_(
                    line,
                    cmd,
                    identity)) {
                    continue;
                }

                DebugLogFormat(
                    L"[TaskSystemConnector][RESV-IDENTITY][MATCH] title=%ls service=%ls resolved=%ls space=%d sid=%u nid=%u tsid=%u path=%ls line=%zu",
                    cmd.title.c_str(),
                    cmd.serviceName.c_str(),
                    identity.serviceName.c_str(),
                    identity.space,
                    static_cast<unsigned>(identity.serviceId),
                    static_cast<unsigned>(identity.networkId),
                    static_cast<unsigned>(identity.tsId),
                    path.c_str(),
                    lineNo);

                out = std::move(identity);
                return true;
            }
        }

        DebugLogFormat(
            L"[TaskSystemConnector][RESV-IDENTITY][NO-MATCH] title=%ls service=%ls space=%d sid=%u nid=%u tsid=%u",
            cmd.title.c_str(),
            cmd.serviceName.c_str(),
            cmd.space,
            static_cast<unsigned>(cmd.serviceId),
            static_cast<unsigned>(cmd.networkId),
            static_cast<unsigned>(cmd.tsId));

        return false;
    }

    static bool CompleteReservationChannelIdentityFromChannelList_(
        TaskSystemConnector::EpgReserveCommandEx& cmd)
    {
        if (cmd.serviceId == 0) {
            return false;
        }

        if (cmd.networkId != 0 &&
            cmd.tsId != 0 &&
            !cmd.serviceName.empty()) {
            return false;
        }

        ReservationChannelIdentity_ identity{};

        if (!ResolveReservationChannelIdentityFromChannelList_(
            cmd,
            identity)) {
            return false;
        }

        const std::uint16_t oldNetworkId = cmd.networkId;
        const std::uint16_t oldTsId = cmd.tsId;
        const std::wstring oldServiceName = cmd.serviceName;

        if (cmd.networkId == 0) {
            cmd.networkId =
                static_cast<std::uint16_t>(
                    identity.networkId);
        }

        if (cmd.tsId == 0) {
            cmd.tsId =
                static_cast<std::uint16_t>(
                    identity.tsId);
        }

        if (cmd.serviceName.empty() ||
            IsSidOnlyReservationServiceName_(cmd.serviceName)) {
            cmd.serviceName = identity.serviceName;
        }

        DebugLogFormat(
            L"[TaskSystemConnector][RESV-IDENTITY][COMPLETE] title=%ls service=%ls oldService=%ls sid=%u oldNid=%u oldTsid=%u newNid=%u newTsid=%u",
            cmd.title.c_str(),
            cmd.serviceName.c_str(),
            oldServiceName.c_str(),
            static_cast<unsigned>(cmd.serviceId),
            static_cast<unsigned>(oldNetworkId),
            static_cast<unsigned>(oldTsId),
            static_cast<unsigned>(cmd.networkId),
            static_cast<unsigned>(cmd.tsId));

        return true;
    }

    std::vector<std::filesystem::path> BuildReservationChannelListCandidates_(int space)
    {
        const auto exeDir = GetExeDirectory_();

        std::vector<std::filesystem::path> paths;
        paths.reserve(6);

        paths.push_back(
            exeDir / (L"channellist_space" + std::to_wstring(space) + L".ch"));
        paths.push_back(
            exeDir / (L"channellist_space" + std::to_wstring(space) + L".txt"));
        paths.push_back(
            exeDir / L"channels.txt");
        paths.push_back(
            exeDir / L"channels.ini");

        return paths;
    }

    std::wstring ResolveServiceNameFromChannelList_(
        const TaskSystemConnector::EpgReserveCommandEx& cmd)
    {
        DebugLogFormat(
            L"[TaskSystemConnector][RESV-SERVICE][IN] title=%ls incomingService=%ls space=%d nid=%u tsid=%u sid=%u event=%u",
            cmd.title.c_str(),
            cmd.serviceName.c_str(),
            cmd.space,
            static_cast<unsigned>(cmd.networkId),
            static_cast<unsigned>(cmd.tsId),
            static_cast<unsigned>(cmd.serviceId),
            static_cast<unsigned>(cmd.eventId));

        if (cmd.serviceId == 0) {
            DebugLogFormat(
                L"[TaskSystemConnector][RESV-SERVICE][NO-ID] serviceId is zero. title=%ls incomingService=%ls space=%d nid=%u tsid=%u event=%u",
                cmd.title.c_str(),
                cmd.serviceName.c_str(),
                cmd.space,
                static_cast<unsigned>(cmd.networkId),
                static_cast<unsigned>(cmd.tsId),
                static_cast<unsigned>(cmd.eventId));

            SystemInfoLogWarn(
                L"番組予約の放送局補正に失敗しました。serviceId が 0 です。title=%ls service=%ls space=%d nid=%u tsid=%u event=%u",
                cmd.title.c_str(),
                cmd.serviceName.c_str(),
                cmd.space,
                static_cast<unsigned>(cmd.networkId),
                static_cast<unsigned>(cmd.tsId),
                static_cast<unsigned>(cmd.eventId));

            return L"";
        }

        const auto candidates = BuildReservationChannelListCandidates_(cmd.space);

        bool foundReadableFile = false;

        for (const auto& path : candidates) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec) || ec) {
                DebugLogFormat(
                    L"[TaskSystemConnector][RESV-SERVICE][SKIP-FILE] not exists path=%ls",
                    path.c_str());
                continue;
            }

            std::wstring text;
            if (!ReadReservationTextFileWide_(path, text)) {
                DebugLogFormat(
                    L"[TaskSystemConnector][RESV-SERVICE][READ-FAIL] path=%ls",
                    path.c_str());
                continue;
            }

            foundReadableFile = true;

            std::wistringstream iss(text);
            std::wstring line;
            std::size_t lineNo = 0;

            while (std::getline(iss, line)) {
                ++lineNo;

                std::wstring serviceName;
                if (TryResolveServiceNameFromChannelLine_(line, cmd, serviceName)) {
                    DebugLogFormat(
                        L"[TaskSystemConnector][RESV-SERVICE][MATCH] title=%ls incoming=%ls resolved=%ls path=%ls line=%zu space=%d nid=%u tsid=%u sid=%u event=%u",
                        cmd.title.c_str(),
                        cmd.serviceName.c_str(),
                        serviceName.c_str(),
                        path.c_str(),
                        lineNo,
                        cmd.space,
                        static_cast<unsigned>(cmd.networkId),
                        static_cast<unsigned>(cmd.tsId),
                        static_cast<unsigned>(cmd.serviceId),
                        static_cast<unsigned>(cmd.eventId));

                    return serviceName;
                }
            }

            DebugLogFormat(
                L"[TaskSystemConnector][RESV-SERVICE][NO-MATCH-IN-FILE] path=%ls title=%ls incoming=%ls space=%d nid=%u tsid=%u sid=%u event=%u",
                path.c_str(),
                cmd.title.c_str(),
                cmd.serviceName.c_str(),
                cmd.space,
                static_cast<unsigned>(cmd.networkId),
                static_cast<unsigned>(cmd.tsId),
                static_cast<unsigned>(cmd.serviceId),
                static_cast<unsigned>(cmd.eventId));
        }

        if (!foundReadableFile) {
            SystemInfoLogWarn(
                L"番組予約の放送局補正に失敗しました。channellist が見つからない、または読み込めません。title=%ls service=%ls space=%d sid=%u",
                cmd.title.c_str(),
                cmd.serviceName.c_str(),
                cmd.space,
                static_cast<unsigned>(cmd.serviceId));
        }
        else {
            SystemInfoLogWarn(
                L"番組予約の放送局補正に失敗しました。channellist に一致する serviceId がありません。title=%ls service=%ls space=%d nid=%u tsid=%u sid=%u event=%u",
                cmd.title.c_str(),
                cmd.serviceName.c_str(),
                cmd.space,
                static_cast<unsigned>(cmd.networkId),
                static_cast<unsigned>(cmd.tsId),
                static_cast<unsigned>(cmd.serviceId),
                static_cast<unsigned>(cmd.eventId));
        }

        return L"";
    }

    std::wstring BuildCanonicalReservationServiceName_(
        const TaskSystemConnector::EpgReserveCommandEx& cmd)
    {
        const std::wstring resolved = ResolveServiceNameFromChannelList_(cmd);

        if (!resolved.empty()) {
            if (!cmd.serviceName.empty() && resolved != cmd.serviceName) {
                SystemInfoLogWarn(
                    L"番組予約の放送局名を補正しました。title=%ls from=%ls to=%ls space=%d nid=%u tsid=%u sid=%u event=%u",
                    cmd.title.c_str(),
                    cmd.serviceName.c_str(),
                    resolved.c_str(),
                    cmd.space,
                    static_cast<unsigned>(cmd.networkId),
                    static_cast<unsigned>(cmd.tsId),
                    static_cast<unsigned>(cmd.serviceId),
                    static_cast<unsigned>(cmd.eventId));

                DebugLogFormat(
                    L"[TaskSystemConnector][RESV-SERVICE][CORRECT] title=%ls from=%ls to=%ls space=%d nid=%u tsid=%u sid=%u event=%u",
                    cmd.title.c_str(),
                    cmd.serviceName.c_str(),
                    resolved.c_str(),
                    cmd.space,
                    static_cast<unsigned>(cmd.networkId),
                    static_cast<unsigned>(cmd.tsId),
                    static_cast<unsigned>(cmd.serviceId),
                    static_cast<unsigned>(cmd.eventId));
            }
            else {
                DebugLogFormat(
                    L"[TaskSystemConnector][RESV-SERVICE][OK] title=%ls service=%ls space=%d nid=%u tsid=%u sid=%u event=%u",
                    cmd.title.c_str(),
                    resolved.c_str(),
                    cmd.space,
                    static_cast<unsigned>(cmd.networkId),
                    static_cast<unsigned>(cmd.tsId),
                    static_cast<unsigned>(cmd.serviceId),
                    static_cast<unsigned>(cmd.eventId));
            }

            return resolved;
        }

        DebugLogFormat(
            L"[TaskSystemConnector][RESV-SERVICE][FALLBACK] title=%ls fallbackService=%ls space=%d nid=%u tsid=%u sid=%u event=%u",
            cmd.title.c_str(),
            cmd.serviceName.c_str(),
            cmd.space,
            static_cast<unsigned>(cmd.networkId),
            static_cast<unsigned>(cmd.tsId),
            static_cast<unsigned>(cmd.serviceId),
            static_cast<unsigned>(cmd.eventId));

        if (!cmd.serviceName.empty()) {
            return cmd.serviceName;
        }

        return L"(放送局不明)";
    }

    EpgHtmlPortal::WriteOptions BuildDefaultReservationWriteOptions_(
        const std::wstring& tasksIniPath)
    {
        EpgHtmlPortal::WriteOptions opt{};

        const auto exeDir = GetExeDirectory_();
        opt.outputDir = exeDir / L"epg_out";
        opt.reservationDataJsFileName = L"reservation_data.js";
        opt.taskIniPath =
            tasksIniPath.empty()
            ? (exeDir / L"tasks.ini").wstring()
            : tasksIniPath;

        return opt;
    }

    int CalcStartAdvanceMinutesForScheduler_(int startAdvanceSec)
    {
        const int sec = (std::clamp)(startAdvanceSec, 0, 600);
        if (sec <= 0) {
            return 0;
        }

        // TimeKeeper は HHMM 粒度なので、秒は切り上げて分へ反映
        return (sec + 59) / 60;
    }

    void ApplyMinuteShiftSafe_(int baseYmd, int baseHHMM, int deltaMinutes, int& outYmd, int& outHHMM)
    {
        outYmd = baseYmd;
        outHHMM = baseHHMM;

        if (deltaMinutes == 0) {
            return;
        }

        int y = baseYmd;
        int hm = baseHHMM;
        if (ShiftLocalYmdHHMM_(baseYmd, baseHHMM, deltaMinutes, y, hm)) {
            outYmd = y;
            outHHMM = hm;
        }
    }

    bool LocalYmdHHMMSSToUtcFileTime_(int ymd, int hhmm, int sec, FILETIME& outUtcFt)
    {
        SYSTEMTIME local{};
        local.wYear = static_cast<WORD>(ymd / 10000);
        local.wMonth = static_cast<WORD>((ymd / 100) % 100);
        local.wDay = static_cast<WORD>(ymd % 100);
        local.wHour = static_cast<WORD>(hhmm / 100);
        local.wMinute = static_cast<WORD>(hhmm % 100);
        local.wSecond = static_cast<WORD>((std::clamp)(sec, 0, 59));
        local.wMilliseconds = 0;

        SYSTEMTIME utc{};
        if (!::TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) {
            return false;
        }
        return ::SystemTimeToFileTime(&utc, &outUtcFt) != 0;
    }

    bool ShiftLocalYmdHHMMSS_(int ymd, int hhmm, int sec, int deltaSeconds,
        int& outYmd, int& outHHMM, int& outSec)
    {
        FILETIME ft{};
        if (!LocalYmdHHMMSSToUtcFileTime_(ymd, hhmm, sec, ft)) {
            return false;
        }

        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;

        constexpr LONGLONG kSecond100ns = 1000LL * 1000LL * 10LL;
        LONGLONG v = static_cast<LONGLONG>(u.QuadPart);
        v += static_cast<LONGLONG>(deltaSeconds) * kSecond100ns;
        if (v < 0) {
            v = 0;
        }

        u.QuadPart = static_cast<ULONGLONG>(v);
        ft.dwLowDateTime = u.LowPart;
        ft.dwHighDateTime = u.HighPart;

        SYSTEMTIME utc{};
        if (!::FileTimeToSystemTime(&ft, &utc)) {
            return false;
        }

        SYSTEMTIME shiftedLocal{};
        if (!::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &shiftedLocal)) {
            return false;
        }

        outYmd = static_cast<int>(shiftedLocal.wYear) * 10000
            + static_cast<int>(shiftedLocal.wMonth) * 100
            + static_cast<int>(shiftedLocal.wDay);

        outHHMM = static_cast<int>(shiftedLocal.wHour) * 100
            + static_cast<int>(shiftedLocal.wMinute);

        outSec = static_cast<int>(shiftedLocal.wSecond);
        return true;
    }

    void SetDispatchTime_(TimeKeeper::TaskRequest& req, int ymd, int hhmm, int sec)
    {
        req.dispatchYmd = ymd;
        req.dispatchHHMM = hhmm;
        req.dispatchSecond = (std::clamp)(sec, 0, 59);
    }

    void WaitUntilDispatchTime_(const TimeKeeper::TaskRequest& req)
    {
        // dispatchHHMM=0000 は 00:00 として有効。
        if (req.dispatchYmd <= 0 || req.dispatchHHMM < 0) {
            return;
        }

        int sec = req.dispatchSecond;
        if (sec < 0) {
            sec = 0;
        }
        if (sec > 59) {
            sec = 59;
        }

        FILETIME targetFt{};
        if (!LocalYmdHHMMSSToUtcFileTime_(
            req.dispatchYmd,
            req.dispatchHHMM,
            sec,
            targetFt)) {
            return;
        }

        ULARGE_INTEGER target{};
        target.LowPart = targetFt.dwLowDateTime;
        target.HighPart = targetFt.dwHighDateTime;

        for (;;) {
            FILETIME nowFt{};
            ::GetSystemTimeAsFileTime(&nowFt);

            ULARGE_INTEGER now{};
            now.LowPart = nowFt.dwLowDateTime;
            now.HighPart = nowFt.dwHighDateTime;

            if (now.QuadPart >= target.QuadPart) {
                return;
            }

            const ULONGLONG remain100ns = target.QuadPart - now.QuadPart;
            const ULONGLONG remainMs = remain100ns / 10000ULL;

            if (remainMs > 1000ULL) {
                ::Sleep(250);
            }
            else if (remainMs > 50ULL) {
                ::Sleep(10);
            }
            else {
                ::Sleep(1);
            }
        }
    }

    std::wstring BuildTunerDisplayText_(int preferredTunerIndex)
    {
        if (preferredTunerIndex < 0) {
            return L"チューナー:auto";
        }
        return L"チューナー:" + std::to_wstring(preferredTunerIndex);
    }

    int NormalizePreferredTunerIndex_(int preferredTunerIndex,
        const std::vector<int>& enabledTuners)
    {
        if (preferredTunerIndex < 0) {
            return -1;
        }

        const auto it = std::find(
            enabledTuners.begin(),
            enabledTuners.end(),
            preferredTunerIndex);

        return (it != enabledTuners.end()) ? preferredTunerIndex : -1;
    }

    int ResolveRequestedPreferredTunerIndex_(
        int requestedPreferredTunerIndex,
        const std::vector<int>& enabledTuners)
    {
        if (requestedPreferredTunerIndex >= 0) {
            return NormalizePreferredTunerIndex_(
                requestedPreferredTunerIndex,
                enabledTuners);
        }

        const int configPreferred =
            ReadReservationPreferredTunerFromConfig_();

        return NormalizePreferredTunerIndex_(
            configPreferred,
            enabledTuners);
    }

    std::vector<int> BuildEnabledTunerIndices_(
        int fallbackTunerCount,
        ReservationTunerPurpose_ purpose)
    {
        auto tuners = LoadEnabledTunerIndicesFromConfig_(purpose);
        if (!tuners.empty()) {
            return tuners;
        }

        const int n = (fallbackTunerCount <= 0) ? 1 : fallbackTunerCount;
        tuners.reserve(static_cast<std::size_t>(n));

        for (int i = 0; i < n; ++i) {
            tuners.push_back(i);
        }

        DebugLogFormat(
            L"[TaskSystemConnector][TUNER-CANDIDATE-FALLBACK] count=%d purpose=%d",
            n,
            static_cast<int>(purpose));

        return tuners;
    }

    std::uint8_t ClampRequiredTuners_(std::uint8_t requested, const std::vector<int>& enabledTuners)
    {
        const std::size_t enabledCount = enabledTuners.empty() ? 1 : enabledTuners.size();
        const std::size_t value = (requested == 0) ? 1 : requested;
        return static_cast<std::uint8_t>(std::min<std::size_t>(value, enabledCount));
    }

    int CompareDateTime_(int ymd1, int hhmm1, int ymd2, int hhmm2)
    {
        if (ymd1 != ymd2) return (ymd1 < ymd2) ? -1 : 1;
        if (hhmm1 != hhmm2) return (hhmm1 < hhmm2) ? -1 : 1;
        return 0;
    }

    constexpr int kBroadcastDayStartHour_ = 4;

    bool IsDigitChar_(wchar_t ch) noexcept
    {
        return ch >= L'0' && ch <= L'9';
    }

    int ParseLooseDateToYmd_(const std::wstring& s) noexcept
    {
        if (s.empty()) {
            return 0;
        }

        const int iso = ParseIsoDateToYmd_(s);
        if (iso > 0) {
            return iso;
        }

        std::wstring digits;
        digits.reserve(8);

        for (wchar_t ch : s) {
            if (IsDigitChar_(ch)) {
                digits.push_back(ch);
                if (digits.size() >= 8) {
                    break;
                }
            }
        }

        if (digits.size() < 8) {
            return 0;
        }

        try {
            const int y = std::stoi(digits.substr(0, 4));
            const int m = std::stoi(digits.substr(4, 2));
            const int d = std::stoi(digits.substr(6, 2));

            if (y < 1900 || m < 1 || m > 12 || d < 1 || d > 31) {
                return 0;
            }

            return y * 10000 + m * 100 + d;
        }
        catch (...) {
            return 0;
        }
    }

    bool ReadNumberAt_(const std::wstring& s, std::size_t& pos, int& value, int& digits) noexcept
    {
        value = 0;
        digits = 0;

        while (pos < s.size() && IsDigitChar_(s[pos])) {
            value = value * 10 + static_cast<int>(s[pos] - L'0');
            ++pos;
            ++digits;

            if (digits > 3) {
                return false;
            }
        }

        return digits > 0;
    }

    bool FindNextTimeHm_(const std::wstring& s, std::size_t& pos, int& outHour, int& outMinute) noexcept
    {
        while (pos < s.size()) {
            if (!IsDigitChar_(s[pos])) {
                ++pos;
                continue;
            }

            std::size_t p = pos;

            int h = 0;
            int hd = 0;
            if (!ReadNumberAt_(s, p, h, hd)) {
                ++pos;
                continue;
            }

            if (p >= s.size() || (s[p] != L':' && s[p] != L'：')) {
                ++pos;
                continue;
            }
            ++p;

            int m = 0;
            int md = 0;
            if (!ReadNumberAt_(s, p, m, md)) {
                ++pos;
                continue;
            }

            if (hd < 1 || hd > 2 || md != 2) {
                ++pos;
                continue;
            }

            if (h < 0 || h > 48 || m < 0 || m > 59) {
                ++pos;
                continue;
            }

            outHour = h;
            outMinute = m;
            pos = p;
            return true;
        }

        return false;
    }

    bool ShiftYmdByDays_(int baseYmd, int dayOffset, int& outYmd)
    {
        outYmd = baseYmd;

        if (baseYmd <= 0) {
            return false;
        }

        if (dayOffset == 0) {
            return true;
        }

        int shiftedYmd = baseYmd;
        int dummyHHMM = 1200;

        // 12:00基準で日付だけずらす。DST境界でも0時基準より安全。
        if (!ShiftLocalYmdHHMM_(baseYmd, 1200, dayOffset * 24 * 60, shiftedYmd, dummyHHMM)) {
            return false;
        }

        outYmd = shiftedYmd;
        return true;
    }

    bool NormalizeTotalMinutesFromBroadcastDate_(
        int broadcastYmd,
        int totalMinutes,
        int& outYmd,
        int& outHHMM)
    {
        if (broadcastYmd <= 0) {
            return false;
        }

        int dayOffset = totalMinutes / (24 * 60);
        int minuteInDay = totalMinutes % (24 * 60);

        if (minuteInDay < 0) {
            minuteInDay += 24 * 60;
            --dayOffset;
        }

        if (!ShiftYmdByDays_(broadcastYmd, dayOffset, outYmd)) {
            return false;
        }

        const int hh = minuteInDay / 60;
        const int mm = minuteInDay % 60;

        outHHMM = hh * 100 + mm;
        return true;
    }

    int BroadcastTotalMinutesFromRawHm_(int rawHour, int rawMinute)
    {
        int total = rawHour * 60 + rawMinute;

        // EPGの放送日は 04:00 開始として扱う。
        // 00:00～03:59 は「表示上の日付の翌日」とみなす。
        if (rawHour >= 0 && rawHour < kBroadcastDayStartHour_) {
            total += 24 * 60;
        }

        // 24:00 以上はそのまま翌日以降として正規化される。
        return total;
    }

    struct BroadcastWindowParts_
    {
        bool hasStart = false;
        bool hasEnd = false;

        int startRawHour = 0;
        int startRawMinute = 0;
        int endRawHour = 0;
        int endRawMinute = 0;

        int startYmd = 0;
        int startHHMM = -1;
        int endYmd = 0;
        int endHHMM = -1;

        std::uint32_t durationSec = 0;
    };

    bool ParseBroadcastWindowFromText_(
        const std::wstring& dateText,
        const std::wstring& timeText,
        BroadcastWindowParts_& out)
    {
        out = {};

        const int broadcastYmd = ParseLooseDateToYmd_(dateText);
        if (broadcastYmd <= 0 || timeText.empty()) {
            return false;
        }

        std::size_t pos = 0;

        int sh = 0;
        int sm = 0;
        if (!FindNextTimeHm_(timeText, pos, sh, sm)) {
            return false;
        }

        out.hasStart = true;
        out.startRawHour = sh;
        out.startRawMinute = sm;

        const int startTotal = BroadcastTotalMinutesFromRawHm_(sh, sm);

        if (!NormalizeTotalMinutesFromBroadcastDate_(
            broadcastYmd,
            startTotal,
            out.startYmd,
            out.startHHMM)) {
            return false;
        }

        int eh = 0;
        int em = 0;
        if (FindNextTimeHm_(timeText, pos, eh, em)) {
            out.hasEnd = true;
            out.endRawHour = eh;
            out.endRawMinute = em;

            int endTotal = BroadcastTotalMinutesFromRawHm_(eh, em);

            if (endTotal < startTotal) {
                endTotal += 24 * 60;
            }

            if (NormalizeTotalMinutesFromBroadcastDate_(
                broadcastYmd,
                endTotal,
                out.endYmd,
                out.endHHMM)) {
                const int durationMin = endTotal - startTotal;
                if (durationMin > 0) {
                    out.durationSec = static_cast<std::uint32_t>(durationMin * 60);
                }
            }
            else {
                out.hasEnd = false;
            }
        }

        return true;
    }

    void ApplyDurationFallbackFromStart_(
        int startYmd,
        int startHHMM,
        std::uint32_t durationSec,
        int& endYmd,
        int& endHHMM)
    {
        if (durationSec == 0 || startYmd <= 0 || startHHMM < 0) {
            return;
        }

        int outSec = 0;

        if (ShiftLocalYmdHHMMSS_(
            startYmd,
            startHHMM,
            0,
            static_cast<int>(durationSec),
            endYmd,
            endHHMM,
            outSec)) {
            return;
        }
    }

    struct OccupancyWindow_
    {
        bool valid = false;
        int startYmd = 0;
        int startHHMM = 0;
        int endYmd = 0;
        int endHHMM = 0;
    };

    bool IsExpiredProgramReservationTask_(
        const TaskListManager::TaskEntry& t,
        int nowYmd,
        int nowHHMM,
        int nowSecond)
    {
        // 番組予約グループの終了判定は RecStop だけで行う。
        //
        // RecPrepare の display.end は番組開始時刻、
        // RecStart の display.end は番組終了時刻を持つことがある。
        // それらを使ってグループ削除すると、録画中に Stop まで消す危険がある。
        if (t.request.kind != TaskKinds::RecStop) {
            return false;
        }

        int limitYmd = t.schedule.dueYmd;
        int limitHHMM = t.schedule.dueHHMM;

        if (limitYmd <= 0 || limitHHMM < 0) {
            if (t.display.endYmd > 0 && t.display.endHHMM >= 0) {
                limitYmd = t.display.endYmd;
                limitHHMM = t.display.endHHMM;
            }
        }

        if (limitYmd <= 0 || limitHHMM < 0) {
            return false;
        }

        // RecStop の発火判定を先に行った後、この時刻以上でグループを削除する。
        // lateToleranceMinutes は「未実行として警告するまでの猶予」であり、
        // EPGタブ上の予約表示を保持する時間には使わない。
        const int dateTimeCompare =
            CompareDateTime_(nowYmd, nowHHMM, limitYmd, limitHHMM);

        if (dateTimeCompare != 0) {
            return dateTimeCompare > 0;
        }

        const int limitSecond =
            (std::clamp)(t.request.dispatchSecond, 0, 59);

        return nowSecond >= limitSecond;
    }

    bool IsProgramReservationStopAlreadyExpired_(
        int stopYmd,
        int stopHHMM,
        int stopLateToleranceMinutes,
        int nowYmd,
        int nowHHMM)
    {
        if (stopYmd <= 0 || stopHHMM < 0) {
            return false;
        }

        int limitYmd = stopYmd;
        int limitHHMM = stopHHMM;

        const int toleranceMinutes =
            (stopLateToleranceMinutes >= 0)
            ? stopLateToleranceMinutes
            : 5;

        if (toleranceMinutes > 0) {
            int shiftedYmd = limitYmd;
            int shiftedHHMM = limitHHMM;

            if (ShiftLocalYmdHHMM_(
                limitYmd,
                limitHHMM,
                toleranceMinutes,
                shiftedYmd,
                shiftedHHMM)) {
                limitYmd = shiftedYmd;
                limitHHMM = shiftedHHMM;
            }
        }

        return CompareDateTime_(nowYmd, nowHHMM, limitYmd, limitHHMM) > 0;
    }

    bool RejectExpiredProgramReservationRegistration_(
        const std::wstring& title,
        int startYmd,
        int startHHMM,
        int stopYmd,
        int stopHHMM,
        int stopLateToleranceMinutes)
    {
        int nowYmd = 0;
        int nowHHMM = -1;

        if (!GetLocalNowYmdHHMM_(nowYmd, nowHHMM)) {
            return false;
        }

        if (!IsProgramReservationStopAlreadyExpired_(
            stopYmd,
            stopHHMM,
            stopLateToleranceMinutes,
            nowYmd,
            nowHHMM)) {
            return false;
        }

        DebugLogFormat(
            L"[TaskSystemConnector][RESV-REJECT-EXPIRED] title=%ls now=%08d %04d start=%08d %04d stop=%08d %04d tol=%d",
            title.empty() ? L"(no title)" : title.c_str(),
            nowYmd,
            nowHHMM,
            startYmd,
            startHHMM,
            stopYmd,
            stopHHMM,
            stopLateToleranceMinutes);

        SystemInfoLogWarn(
            L"終了済み番組のため予約登録を拒否しました。title=%ls now=%08d %02d:%02d start=%08d %02d:%02d stop=%08d %02d:%02d",
            title.empty() ? L"(no title)" : title.c_str(),
            nowYmd,
            nowHHMM / 100,
            nowHHMM % 100,
            startYmd,
            startHHMM / 100,
            startHHMM % 100,
            stopYmd,
            stopHHMM / 100,
            stopHHMM % 100);

        return true;
    }

    bool PurgeExpiredProgramReservationGroups_(
        TaskListManager& taskList)
    {
        SYSTEMTIME st{};
        ::GetLocalTime(&st);

        const int nowYmd =
            static_cast<int>(st.wYear) * 10000 +
            static_cast<int>(st.wMonth) * 100 +
            static_cast<int>(st.wDay);

        const int nowHHMM =
            static_cast<int>(st.wHour) * 100 +
            static_cast<int>(st.wMinute);

        const int nowSecond = static_cast<int>(st.wSecond);

        if (nowYmd <= 0 || nowHHMM < 0) {
            return false;
        }

        const auto snap = taskList.GetSnapshot();

        std::unordered_set<std::uint32_t> expiredInstances;

        for (const auto& t : snap) {
            if (!IsExpiredProgramReservationTask_(
                t,
                nowYmd,
                nowHHMM,
                nowSecond)) {
                continue;
            }

            if (t.request.instance != 0) {
                expiredInstances.insert(t.request.instance);
            }
        }

        if (expiredInstances.empty()) {
            return false;
        }

        bool removedAny = false;

        for (const std::uint32_t inst : expiredInstances) {
            const bool removedPrepare =
                taskList.RemoveTask(TaskKinds::RecPrepare, inst);

            const bool removedStart =
                taskList.RemoveTask(TaskKinds::RecStart, inst);

            const bool removedStop =
                taskList.RemoveTask(TaskKinds::RecStop, inst);

            removedAny =
                removedAny ||
                removedPrepare ||
                removedStart ||
                removedStop;

            DebugLogFormat(
                L"[TaskSystemConnector][PURGE-EXPIRED-RECORD] inst=%u prepare=%d start=%d stop=%d now=%08d %04d",
                static_cast<unsigned>(inst),
                removedPrepare ? 1 : 0,
                removedStart ? 1 : 0,
                removedStop ? 1 : 0,
                nowYmd,
                nowHHMM);

            SystemInfoLogWarn(
                L"終了済み番組予約を起動対象から削除しました。instance=%u now=%08d %02d:%02d",
                static_cast<unsigned>(inst),
                nowYmd,
                nowHHMM / 100,
                nowHHMM % 100);
        }

        return removedAny;
    }

    OccupancyWindow_ MakeWindow_(int startYmd, int startHHMM, int endYmd, int endHHMM)
    {
        OccupancyWindow_ w{};

        if (startYmd <= 0 || endYmd <= 0) {
            return w;
        }

        if (startHHMM < 0 || endHHMM < 0) {
            return w;
        }

        w.valid = true;
        w.startYmd = startYmd;
        w.startHHMM = startHHMM;
        w.endYmd = endYmd;
        w.endHHMM = endHHMM;

        if (CompareDateTime_(w.endYmd, w.endHHMM, w.startYmd, w.startHHMM) < 0) {
            w.endYmd = w.startYmd;
            w.endHHMM = w.startHHMM;
        }

        return w;
    }

    bool IsTunerOccupancyTaskKind_(std::uint32_t kind) noexcept
    {
        if (IsEpgTaskKind_(kind)) {
            return true;
        }

        // 番組予約では RecStart の本体時間だけをチューナー占有扱いにする。
        // RecPrepare は実行着火点だが、登録時の重複判定には使わない。
        // RecStop は終了処理なので、占有窓としては扱わない。
        return kind == TaskKinds::RecStart;
    }

    OccupancyWindow_ BuildOccupancyWindowFromTask_(const TaskListManager::TaskEntry& t)
    {
        if (!IsTunerOccupancyTaskKind_(t.request.kind)) {
            return {};
        }

        if (!t.enabled) {
            return {};
        }
        if (t.schedule.type != TaskListManager::ScheduleType::OneShot) {
            return {};
        }

        if (t.schedule.useTimeWindow) {
            const int startYmd =
                (t.schedule.windowStartYmd > 0)
                ? t.schedule.windowStartYmd
                : t.schedule.dueYmd;

            const int startHHMM =
                (t.schedule.windowStartHHMM >= 0)
                ? t.schedule.windowStartHHMM
                : t.schedule.dueHHMM;

            int endYmd =
                (t.schedule.windowEndYmd > 0)
                ? t.schedule.windowEndYmd
                : t.display.endYmd;

            int endHHMM =
                (t.schedule.windowEndHHMM >= 0)
                ? t.schedule.windowEndHHMM
                : t.display.endHHMM;

            if (endYmd <= 0) {
                endYmd = startYmd;
            }

            if (endHHMM < 0) {
                endHHMM = startHHMM;
            }

            return MakeWindow_(startYmd, startHHMM, endYmd, endHHMM);
        }

        int endYmd =
            (t.display.endYmd > 0)
            ? t.display.endYmd
            : t.schedule.dueYmd;

        int endHHMM =
            (t.display.endHHMM >= 0)
            ? t.display.endHHMM
            : t.schedule.dueHHMM;

        return MakeWindow_(t.schedule.dueYmd, t.schedule.dueHHMM, endYmd, endHHMM);
    }

    bool Overlaps_(const OccupancyWindow_& a, const OccupancyWindow_& b)
    {
        if (!a.valid || !b.valid) {
            return false;
        }

        // 半開区間 [start, end)。
        // 05:00-06:00 と 06:00-06:30 は重複しない。
        if (CompareDateTime_(a.endYmd, a.endHHMM, b.startYmd, b.startHHMM) <= 0) {
            return false;
        }

        if (CompareDateTime_(b.endYmd, b.endHHMM, a.startYmd, a.startHHMM) <= 0) {
            return false;
        }

        return true;
    }

    bool IsCandidateBusy_(const std::vector<TaskListManager::TaskEntry>& snapshot,
        int candidateTunerIndex,
        const OccupancyWindow_& newWindow)
    {
        if (!newWindow.valid) {
            return false;
        }

        for (const auto& t : snapshot) {
            if (!t.enabled) continue;
            if (t.request.preferredTunerIndex != candidateTunerIndex) continue;

            const auto existing = BuildOccupancyWindowFromTask_(t);
            if (!existing.valid) continue;

            if (Overlaps_(existing, newWindow)) {
                return true;
            }
        }
        return false;
    }

    bool ResolvePreferredTunerIndex_(
        const std::vector<int>& enabledTuners,
        const std::vector<TaskListManager::TaskEntry>& snapshot,
        int requestedTunerIndex,
        int conflictPolicy,
        const OccupancyWindow_& newWindow,
        int& resolvedTunerIndex)
    {
        resolvedTunerIndex = -1;
        if (enabledTuners.empty()) {
            return false;
        }

        const int policy = (std::clamp)(conflictPolicy, 0, 2);

        auto isEnabledIndex = [&](int idx) {
            return std::find(enabledTuners.begin(), enabledTuners.end(), idx) != enabledTuners.end();
            };

        const bool requestedValid =
            (requestedTunerIndex >= 0) && isEnabledIndex(requestedTunerIndex);

        // 2 = 競合したら失敗
        if (policy == 2) {
            if (requestedValid) {
                if (!IsCandidateBusy_(snapshot, requestedTunerIndex, newWindow)) {
                    resolvedTunerIndex = requestedTunerIndex;
                    return true;
                }
                return false;
            }

            for (int idx : enabledTuners) {
                if (!IsCandidateBusy_(snapshot, idx, newWindow)) {
                    resolvedTunerIndex = idx;
                    return true;
                }
            }
            return false;
        }

        std::vector<int> candidates;
        candidates.reserve(enabledTuners.size());

        if (policy == 1) {
            // 1 = 優先チューナー最優先、無ければ次へ
            if (requestedValid) {
                candidates.push_back(requestedTunerIndex);
            }
            for (int idx : enabledTuners) {
                if (idx == requestedTunerIndex) continue;
                candidates.push_back(idx);
            }
        }
        else {
            // 0 = 自動で空きへ
            for (int idx : enabledTuners) {
                candidates.push_back(idx);
            }
        }

        for (int idx : candidates) {
            if (!IsCandidateBusy_(snapshot, idx, newWindow)) {
                resolvedTunerIndex = idx;
                return true;
            }
        }

        return false;
    }

    bool IsSameProgramReservationGroupTask_(const TaskListManager::TaskEntry& t, std::uint32_t instance)
    {
        if (t.request.instance != instance) {
            return false;
        }

        switch (t.request.kind) {
        case TaskKinds::RecPrepare:
        case TaskKinds::RecStart:
        case TaskKinds::RecStop:
            return true;
        default:
            return false;
        }
    }

    std::vector<TaskListManager::TaskEntry>
        BuildSnapshotWithoutProgramReservationGroup_(const TaskListManager& taskList, std::uint32_t instance)
    {
        auto snap = taskList.GetSnapshot();
        snap.erase(
            std::remove_if(
                snap.begin(),
                snap.end(),
                [instance](const TaskListManager::TaskEntry& t) {
                    return IsSameProgramReservationGroupTask_(t, instance);
                }),
            snap.end());
        return snap;
    }

    std::vector<TaskListManager::TaskEntry>
        BuildSnapshotWithoutEpgReservationGroup_(const TaskListManager& taskList,
            std::uint32_t kind,
            std::uint32_t dailyInstance,
            std::uint32_t oneShotInstance)
    {
        auto snap = taskList.GetSnapshot();
        snap.erase(
            std::remove_if(
                snap.begin(),
                snap.end(),
                [kind, dailyInstance, oneShotInstance](const TaskListManager::TaskEntry& t) {
                    if (t.request.kind != kind) {
                        return false;
                    }
                    return (t.request.instance == dailyInstance) || (t.request.instance == oneShotInstance);
                }),
            snap.end());
        return snap;
    }

    void LogTunerAutoReroute_(const wchar_t* reservationText,
        int requestedTunerIndex,
        int resolvedTunerIndex,
        const wchar_t* nameOrTitle)
    {
        if (requestedTunerIndex < 0) {
            return;
        }
        if (requestedTunerIndex == resolvedTunerIndex) {
            return;
        }

        SystemInfoLogInfo(
            L"%lsのチューナー番号が重複したため自動振替しました。name=%ls requested=%d resolved=%d",
            reservationText ? reservationText : L"予約",
            nameOrTitle ? nameOrTitle : L"(unknown)",
            requestedTunerIndex,
            resolvedTunerIndex);
    }

    static bool ParseIsoYmdLoose_(
        const std::wstring& s,
        int& outYmd)
    {
        outYmd = 0;

        if (s.size() < 10) {
            return false;
        }

        if (!iswdigit(s[0]) || !iswdigit(s[1]) ||
            !iswdigit(s[2]) || !iswdigit(s[3])) {
            return false;
        }

        const wchar_t sep1 = s[4];
        const wchar_t sep2 = s[7];

        if (!((sep1 == L'-' || sep1 == L'/') &&
            (sep2 == L'-' || sep2 == L'/'))) {
            return false;
        }

        const int y =
            (s[0] - L'0') * 1000 +
            (s[1] - L'0') * 100 +
            (s[2] - L'0') * 10 +
            (s[3] - L'0');

        const int m =
            (s[5] - L'0') * 10 +
            (s[6] - L'0');

        const int d =
            (s[8] - L'0') * 10 +
            (s[9] - L'0');

        if (y < 1900 || m < 1 || m > 12 || d < 1 || d > 31) {
            return false;
        }

        outYmd = y * 10000 + m * 100 + d;
        return true;
    }

    static bool ParseHHMMSSLoose_(
        const std::wstring& s,
        int& outHHMM,
        int& outSecond)
    {
        outHHMM = 0;
        outSecond = 0;

        if (s.size() < 5) {
            return false;
        }

        std::size_t pos = 0;

        // "2026-05-28 08:15:00" のような文字列でも時刻部分を拾う
        for (; pos + 4 < s.size(); ++pos) {
            if (iswdigit(s[pos]) &&
                iswdigit(s[pos + 1]) &&
                s[pos + 2] == L':' &&
                iswdigit(s[pos + 3]) &&
                iswdigit(s[pos + 4])) {
                break;
            }
        }

        if (pos + 4 >= s.size()) {
            return false;
        }

        const int h =
            (s[pos] - L'0') * 10 +
            (s[pos + 1] - L'0');

        const int m =
            (s[pos + 3] - L'0') * 10 +
            (s[pos + 4] - L'0');

        int sec = 0;

        if (pos + 7 < s.size() &&
            s[pos + 5] == L':' &&
            iswdigit(s[pos + 6]) &&
            iswdigit(s[pos + 7])) {
            sec =
                (s[pos + 6] - L'0') * 10 +
                (s[pos + 7] - L'0');
        }

        if (h < 0 || h > 47 || m < 0 || m > 59 || sec < 0 || sec > 59) {
            return false;
        }

        outHHMM = (h % 24) * 100 + m;
        outSecond = sec;
        return true;
    }

}//namespace

std::uint64_t TaskSystemConnector::MakeMonitorKey_(std::uint32_t kind, std::uint32_t instance) noexcept
{
    return (static_cast<std::uint64_t>(kind) << 32) | static_cast<std::uint64_t>(instance);
}

bool TaskSystemConnector::TryMarkTaskDispatching_(
    std::uint32_t kind,
    std::uint32_t instance)
{
    const std::uint64_t key = MakeMonitorKey_(kind, instance);

    std::lock_guard<std::mutex> lock(executionMonitorMutex_);

    if (executedTaskKeys_.find(key) != executedTaskKeys_.end()) {
        DebugLogFormat(
            L"[TaskSystemConnector][EXEC-GUARD] skip duplicate kind=%u inst=%u",
            static_cast<unsigned>(kind),
            static_cast<unsigned>(instance));
        return false;
    }

    executedTaskKeys_.insert(key);
    warnedTaskKeys_.erase(key);
    return true;
}

void TaskSystemConnector::MarkTaskExecuted_(std::uint32_t kind, std::uint32_t instance)
{
    const std::uint64_t key = MakeMonitorKey_(kind, instance);
    std::lock_guard<std::mutex> lock(executionMonitorMutex_);
    executedTaskKeys_.insert(key);
    warnedTaskKeys_.erase(key);
}

void TaskSystemConnector::ResetExecutionMonitorForTask_(std::uint32_t kind, std::uint32_t instance)
{
    const std::uint64_t key = MakeMonitorKey_(kind, instance);
    std::lock_guard<std::mutex> lock(executionMonitorMutex_);
    executedTaskKeys_.erase(key);
    warnedTaskKeys_.erase(key);
}

void TaskSystemConnector::SyncExecutionMonitorWithTaskList_()
{
    const auto snap = taskList_.GetSnapshot();

    std::unordered_set<std::uint64_t> alive;
    alive.reserve(snap.size());

    for (const auto& t : snap) {
        alive.insert(MakeMonitorKey_(t.request.kind, t.request.instance));
    }

    std::lock_guard<std::mutex> lock(executionMonitorMutex_);

    for (auto it = executedTaskKeys_.begin(); it != executedTaskKeys_.end();) {
        if (alive.find(*it) == alive.end()) {
            it = executedTaskKeys_.erase(it);
        }
        else {
            ++it;
        }
    }

    for (auto it = warnedTaskKeys_.begin(); it != warnedTaskKeys_.end();) {
        if (alive.find(*it) == alive.end()) {
            it = warnedTaskKeys_.erase(it);
        }
        else {
            ++it;
        }
    }
}

bool TaskSystemConnector::HandleMissedEpgOneShotAfterLogged_(
    const TaskListManager::TaskEntry& missed)
{
    if (!IsEpgTaskKind_(missed.request.kind)) {
        return false;
    }
    if (missed.schedule.type != TaskListManager::ScheduleType::OneShot) {
        return false;
    }

    // EPG OneShot は dailyInstance=0 / oneShotInstance=1 を基本形として扱う。
    // Daily 本体から次回 OneShot を再生成する。
    const auto dailyOpt = taskList_.Find(missed.request.kind, 0);

    const bool removed = RemoveTaskAndApply(missed.request.kind, missed.request.instance);
    if (!removed) {
        SystemInfoLogError(
            L"未実行EPG予約の削除に失敗しました。kind=%u instance=%u",
            static_cast<unsigned>(missed.request.kind),
            static_cast<unsigned>(missed.request.instance));
        return false;
    }

    if (!dailyOpt) {
        SystemInfoLogWarn(
            L"未実行EPG予約を削除しましたが、Daily予約が存在しないため次回予約は再登録しません。kind=%u instance=%u",
            static_cast<unsigned>(missed.request.kind),
            static_cast<unsigned>(missed.request.instance));
        return true;
    }

    const TaskListManager::TaskEntry daily = *dailyOpt;

    if (daily.schedule.type != TaskListManager::ScheduleType::Daily) {
        SystemInfoLogWarn(
            L"未実行EPG予約を削除しましたが、DailyテンプレートがDaily形式ではないため次回予約は再登録しません。kind=%u dailyInstance=%u",
            static_cast<unsigned>(daily.request.kind),
            static_cast<unsigned>(daily.request.instance));
        return true;
    }

    const int dailyHour = (std::clamp)(daily.schedule.dailyHour, 0, 23);
    const int dailyMinute = (std::clamp)(daily.schedule.dailyMinute, 0, 59);
    const int hhmm = dailyHour * 100 + dailyMinute;

    std::uint8_t requiredTuners = daily.request.requiredTuners;
    if (requiredTuners == 0) {
        requiredTuners = missed.request.requiredTuners;
    }
    if (requiredTuners == 0) {
        requiredTuners = (opt_.epgRequiredTuners == 0) ? 1 : opt_.epgRequiredTuners;
    }

    TaskSystemConnector::EpgReservationRequest req{};
    req.kind = missed.request.kind;
    req.hhmm = hhmm;

    req.windowMinutes =
        (missed.schedule.lateToleranceMinutes > 0)
        ? missed.schedule.lateToleranceMinutes
        : 30;

    req.enabledDaily = false;
    req.enabledOneShot = true;
    req.forceNextDayOneShot = true;

    req.requiredTuners = requiredTuners;
    req.preferredTunerIndex = daily.request.preferredTunerIndex;

    req.dailyInstance = daily.request.instance;
    req.oneShotInstance = missed.request.instance;

    req.dailyName =
        daily.name.empty()
        ? L"EPG Full Update (Daily)"
        : daily.name;

    req.oneShotName =
        missed.name.empty()
        ? L"EPG Full OneShot (From UI)"
        : missed.name;

    req.dailyLabel =
        daily.display.label.empty()
        ? L"EPG取得予約"
        : daily.display.label;

    req.oneShotLabel =
        missed.display.label.empty()
        ? L"EPG取得予約(次回)"
        : missed.display.label;

    req.target =
        !daily.display.target.empty()
        ? daily.display.target
        : (!missed.display.target.empty() ? missed.display.target : L"EPG対象サービス");

    req.dailyNote =
        daily.display.note.empty()
        ? L"毎日定時実行"
        : daily.display.note;

    const bool scheduled = ScheduleEpgReservation(req);
    if (!scheduled) {
        SystemInfoLogError(
            L"未実行EPG予約を削除しましたが、次回EPG予約の再登録に失敗しました。kind=%u oldInstance=%u hhmm=%04d",
            static_cast<unsigned>(missed.request.kind),
            static_cast<unsigned>(missed.request.instance),
            hhmm);
        return true;
    }

    SystemInfoLogInfo(
        L"未実行EPG予約を削除し、次回EPG予約を再登録しました。kind=%u oldInstance=%u dailyInstance=%u hhmm=%04d",
        static_cast<unsigned>(missed.request.kind),
        static_cast<unsigned>(missed.request.instance),
        static_cast<unsigned>(daily.request.instance),
        hhmm);

    return true;
}

bool TaskSystemConnector::RefreshNextEpgOneShotAfterAcquired_(
    const TimeKeeper::TaskRequest& r,
    const wchar_t* reason)
{
    if (!IsEpgAutoReservationRequest_(r)) {
        return false;
    }

    const auto dailyOpt =
        taskList_.Find(r.kind, kEpgDailyTemplateInstance_);

    if (!dailyOpt) {
        SystemInfoLogWarn(
            L"EPG取得完了後の次回予約更新をスキップしました。Dailyテンプレートがありません。kind=%u instance=%u reason=%ls",
            static_cast<unsigned>(r.kind),
            static_cast<unsigned>(r.instance),
            reason ? reason : L"(unknown)");
        return false;
    }

    const TaskListManager::TaskEntry daily = *dailyOpt;

    if (daily.schedule.type != TaskListManager::ScheduleType::Daily) {
        SystemInfoLogWarn(
            L"EPG取得完了後の次回予約更新をスキップしました。DailyテンプレートがDaily形式ではありません。kind=%u instance=%u reason=%ls",
            static_cast<unsigned>(r.kind),
            static_cast<unsigned>(r.instance),
            reason ? reason : L"(unknown)");
        return false;
    }

    const auto oneShotOpt =
        taskList_.Find(r.kind, kEpgOneShotInstance_);

    const int dailyHour =
        (std::clamp)(daily.schedule.dailyHour, 0, 23);

    const int dailyMinute =
        (std::clamp)(daily.schedule.dailyMinute, 0, 59);

    const int hhmm = dailyHour * 100 + dailyMinute;

    std::uint8_t requiredTuners = daily.request.requiredTuners;
    if (requiredTuners == 0 && oneShotOpt) {
        requiredTuners = oneShotOpt->request.requiredTuners;
    }
    if (requiredTuners == 0) {
        requiredTuners = (opt_.epgRequiredTuners == 0)
            ? 1
            : opt_.epgRequiredTuners;
    }

    int preferredTunerIndex = daily.request.preferredTunerIndex;
    if (preferredTunerIndex < 0 && oneShotOpt) {
        preferredTunerIndex = oneShotOpt->request.preferredTunerIndex;
    }

    int windowMinutes = kDefaultEpgWindowMinutes_;
    if (oneShotOpt && oneShotOpt->schedule.lateToleranceMinutes > 0) {
        windowMinutes = oneShotOpt->schedule.lateToleranceMinutes;
    }

    EpgReservationRequest req{};
    req.kind = r.kind;
    req.hhmm = hhmm;
    req.windowMinutes = windowMinutes;

    // 取得完了後は Daily を実行対象に戻さない。
    // Daily は時刻テンプレート、実行は次回 OneShot のみ。
    req.enabledDaily = false;
    req.enabledOneShot = true;

    // 取得完了後は必ず翌日分へ送る。
    req.forceNextDayOneShot = true;

    req.requiredTuners = requiredTuners;
    req.preferredTunerIndex = preferredTunerIndex;

    req.dailyInstance = kEpgDailyTemplateInstance_;
    req.oneShotInstance = kEpgOneShotInstance_;

    req.dailyName =
        daily.name.empty()
        ? L"EPG Auto Update Template"
        : daily.name;

    req.oneShotName =
        (oneShotOpt && !oneShotOpt->name.empty())
        ? oneShotOpt->name
        : L"EPG Full OneShot (Auto)";

    req.dailyLabel =
        daily.display.label.empty()
        ? L"EPG自動更新設定"
        : daily.display.label;

    req.oneShotLabel =
        (oneShotOpt && !oneShotOpt->display.label.empty())
        ? oneShotOpt->display.label
        : L"EPG取得予約(次回)";

    req.target =
        !daily.display.target.empty()
        ? daily.display.target
        : ((oneShotOpt && !oneShotOpt->display.target.empty())
            ? oneShotOpt->display.target
            : L"EPG対象サービス");

    req.dailyNote =
        daily.display.note.empty()
        ? L"毎日時刻テンプレート。実行は次回OneShotのみ"
        : daily.display.note;

    const bool ok = ScheduleEpgReservation(req);
    if (!ok) {
        SystemInfoLogError(
            L"EPG取得完了後の次回予約更新に失敗しました。kind=%u instance=%u hhmm=%04d reason=%ls",
            static_cast<unsigned>(r.kind),
            static_cast<unsigned>(r.instance),
            hhmm,
            reason ? reason : L"(unknown)");
        return false;
    }

    SystemInfoLogInfo(
        L"EPG取得完了後に次回EPG予約を更新しました。kind=%u oldInstance=%u nextInstance=%u hhmm=%04d reason=%ls",
        static_cast<unsigned>(r.kind),
        static_cast<unsigned>(r.instance),
        static_cast<unsigned>(kEpgOneShotInstance_),
        hhmm,
        reason ? reason : L"(unknown)");

    return true;
}

bool TaskSystemConnector::HandleMissedTaskAfterLogged_(
    const TaskListManager::TaskEntry& t)
{
    // 番組予約は Prepare / Start / Stop の3件で1グループ。
    // Prepare や Start だけを期限切れ削除すると、EPGタブの代表行である
    // RecStart が番組開始時刻に消える。番組予約の削除は RecStop 時刻を使う
    // PurgeExpiredProgramReservationGroups_() に一本化する。
    if (IsRecordTaskKind_(t.request.kind)) {
        return true;
    }

    if (IsEpgTaskKind_(t.request.kind) &&
        t.schedule.type == TaskListManager::ScheduleType::OneShot) {
        return HandleMissedEpgOneShotAfterLogged_(t);
    }

    const bool removed = RemoveTaskAndApply(t.request.kind, t.request.instance);
    if (!removed) {
        SystemInfoLogError(
            L"未実行予約の削除に失敗しました。kind=%u instance=%u name=%ls",
            static_cast<unsigned>(t.request.kind),
            static_cast<unsigned>(t.request.instance),
            MakeTaskLabel_(t).c_str());
        return false;
    }

    SystemInfoLogInfo(
        L"未実行予約を削除しました。kind=%u instance=%u name=%ls",
        static_cast<unsigned>(t.request.kind),
        static_cast<unsigned>(t.request.instance),
        MakeTaskLabel_(t).c_str());

    return true;
}

void TaskSystemConnector::EmitOverdueTaskWarnings_()
{
    const auto snap = taskList_.GetSnapshot();

    for (const auto& t : snap) {
        if (!t.enabled) continue;
        if (t.schedule.type != TaskListManager::ScheduleType::OneShot) continue;
        if (t.schedule.dueYmd <= 0) continue;

        // 番組予約の期限切れ処理は RecStop 基準のグループ削除だけで行う。
        // 個別タスクをここで削除してはならない。
        if (IsRecordTaskKind_(t.request.kind)) continue;

        int limitYmd = t.schedule.dueYmd;
        int limitHHMM = t.schedule.dueHHMM;

        if (t.schedule.useTimeWindow &&
            t.schedule.windowEndYmd > 0 &&
            t.schedule.windowEndHHMM >= 0) {
            limitYmd = t.schedule.windowEndYmd;
            limitHHMM = t.schedule.windowEndHHMM;
        }
        else if (t.schedule.lateToleranceMinutes > 0) {
            (void)ShiftLocalYmdHHMM_(
                t.schedule.dueYmd,
                t.schedule.dueHHMM,
                t.schedule.lateToleranceMinutes,
                limitYmd,
                limitHHMM);
        }

        SYSTEMTIME st{};
        ::GetLocalTime(&st);

        const int nowYmd =
            static_cast<int>(st.wYear) * 10000 +
            static_cast<int>(st.wMonth) * 100 +
            static_cast<int>(st.wDay);

        const int nowHHMM =
            static_cast<int>(st.wHour) * 100 +
            static_cast<int>(st.wMinute);

        const bool overdue =
            (nowYmd > limitYmd) ||
            (nowYmd == limitYmd && nowHHMM > limitHHMM);

        if (!overdue) continue;

        const std::uint64_t key =
            MakeMonitorKey_(t.request.kind, t.request.instance);

        {
            std::lock_guard<std::mutex> lock(executionMonitorMutex_);

            if (executedTaskKeys_.find(key) != executedTaskKeys_.end()) {
                continue;
            }

            if (warnedTaskKeys_.find(key) != warnedTaskKeys_.end()) {
                continue;
            }

            warnedTaskKeys_.insert(key);
        }

        SystemInfoLogWarn(
            L"%lsが実行期限を過ぎても未実行です。name=%ls kind=%u instance=%u due=%08d %02d:%02d limit=%08d %02d:%02d",
            GetReservationText_(t.request.kind),
            MakeTaskLabel_(t).c_str(),
            static_cast<unsigned>(t.request.kind),
            static_cast<unsigned>(t.request.instance),
            t.schedule.dueYmd,
            t.schedule.dueHHMM / 100,
            t.schedule.dueHHMM % 100,
            limitYmd,
            limitHHMM / 100,
            limitHHMM % 100);

        (void)HandleMissedTaskAfterLogged_(t);
    }
}

void TaskSystemConnector::Log_(const wchar_t* fmt, ...) const
{
    wchar_t buf[1024]{};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    DebugLog(L"%ls", buf);
    if (cb_.Log) {
        cb_.Log(buf);
    }
}

std::wstring TaskSystemConnector::ResolveTasksIniPathForSave_() const
{
    if (!opt_.tasksIniPath.empty()) {
        return opt_.tasksIniPath;
    }

    const auto exeDir = GetExeDirectory_();
    return (exeDir / L"tasks.ini").wstring();
}

bool TaskSystemConnector::RefreshReservationCaches_(
    const std::wstring& tasksIniPath) const
{
    if (tasksIniPath.empty()) {
        return false;
    }

    const auto snapshot = taskList_.GetSnapshot();

    const auto result =
        TaskReservationCache::WriteAllFromTasks(tasksIniPath, snapshot);

    if (!result.ok) {
        DebugLog(
            L"[TaskSystemConnector] reservation cache update failed: %ls",
            result.message.c_str());
        return false;
    }

    DebugLog(
        L"[TaskSystemConnector] reservation caches updated: globalRows=%zu serviceFiles=%zu serviceRows=%zu runRows=%zu execRows=%zu",
        result.globalViewRows,
        result.serviceViewFiles,
        result.serviceViewRows,
        result.runRows,
        result.execRows);

    return true;
}

bool TaskSystemConnector::SaveTasksToConfiguredIni_() const
{
    const std::wstring path = ResolveTasksIniPathForSave_();
    if (path.empty()) {
        return false;
    }

    return SaveTasksToIni(path);
}

void TaskSystemConnector::NotifyReservationDataChanged_() const
{
    if (cb_.OnReservationDataChanged) {
        cb_.OnReservationDataChanged();
    }
}

bool TaskSystemConnector::RunReservationMaintenanceAfterEpgBuild(bool isInitialBuild)
{
    if (!initialized_) {
        DebugLog(L"[TaskSystemConnector][MAINT] skipped: not initialized");
        return false;
    }

    DebugLogFormat(
        L"[TaskSystemConnector][MAINT] begin initial=%d",
        isInitialBuild ? 1 : 0);

    // 初回EPG構築時は予約整理しない方針。
    // ここでは何も削除せず、現行タスクの再同期だけ行う。
    if (isInitialBuild) {
        SyncExecutionMonitorWithTaskList_();

        const bool applyOk = ApplyTaskList();

        DebugLogFormat(
            L"[TaskSystemConnector][MAINT] initial build sync end ok=%d",
            applyOk ? 1 : 0);

        return applyOk;
    }

    bool ok = true;

    // EPG構築後の通常メンテ。
    // tasks.ini更新監視系の関数が未実装/不安定なため、ここでは呼ばない。
    // まずは既存の期限切れ検出、監視セット整理、保存、再反映だけ行う。
    EmitOverdueTaskWarnings_();

    SyncExecutionMonitorWithTaskList_();

    if (!SaveTasksToConfiguredIni_()) {
        DebugLog(L"[TaskSystemConnector][MAINT][WARN] SaveTasksToConfiguredIni_ failed");
        ok = false;
    }

    if (!ApplyTaskList()) {
        DebugLog(L"[TaskSystemConnector][MAINT][WARN] ApplyTaskList failed");
        ok = false;
    }

    DebugLogFormat(
        L"[TaskSystemConnector][MAINT] end ok=%d",
        ok ? 1 : 0);

    return ok;
}

TaskSystemConnector::ExternalEpgUpdateSyncResult
TaskSystemConnector::ReloadTasksAfterExternalEpgUpdate(
    bool isInitialBuild)
{
    ExternalEpgUpdateSyncResult out{};

    if (!initialized_) {
        out.message = L"TaskSystemConnector is not initialized";

        DebugLogFormat(
            L"[TaskSystemConnector][EXTERNAL-EPG-SYNC] skipped: %ls",
            out.message.c_str());

        return out;
    }

    out.tasksIniPath = ResolveTasksIniPathForSave_();

    if (out.tasksIniPath.empty()) {
        out.message = L"tasks.ini path is empty";

        DebugLogFormat(
            L"[TaskSystemConnector][EXTERNAL-EPG-SYNC] failed: %ls",
            out.message.c_str());

        return out;
    }

    DebugLogFormat(
        L"[TaskSystemConnector][EXTERNAL-EPG-SYNC] begin initial=%d path=%ls",
        isInitialBuild ? 1 : 0,
        out.tasksIniPath.c_str());

    // EpgBuild_GUI / ReservationEpgRebuildAction が tasks.ini を直接更新しているため、
    // まず TaskSystemConnector 側の taskList_ を正本から読み直す。
    out.loadOk = LoadTasksFromIni(out.tasksIniPath);

    if (!out.loadOk) {
        out.message = L"LoadTasksFromIni failed";

        SystemInfoLogWarn(
            L"EPG更新後の予約タスク再読込に失敗しました。tasks.ini=%ls",
            out.tasksIniPath.c_str());

        DebugLogFormat(
            L"[TaskSystemConnector][EXTERNAL-EPG-SYNC] load failed path=%ls",
            out.tasksIniPath.c_str());

        return out;
    }

    // 読み直した内容に対してメンテナンスを行う。
// RunReservationMaintenanceAfterEpgBuild() 内で
// SaveTasksToConfiguredIni_() と ApplyTaskList() まで行うため、
// ここで追加 ApplyTaskList() は呼ばない。
    out.maintenanceOk =
        RunReservationMaintenanceAfterEpgBuild(isInitialBuild);

    // maintenanceOk は「保存 + TimeKeeper再反映」まで含む結果として扱う。
    // 二重 ApplyTaskList による OneShot cleared/added の重複ログを避ける。
    out.applyOk = out.maintenanceOk;

    out.ok = out.loadOk && out.maintenanceOk && out.applyOk;

    out.message =
        L"load=" +
        std::to_wstring(out.loadOk ? 1 : 0) +
        L" maintenance=" +
        std::to_wstring(out.maintenanceOk ? 1 : 0) +
        L" apply=" +
        std::to_wstring(out.applyOk ? 1 : 0);

    DebugLogFormat(
        L"[TaskSystemConnector][EXTERNAL-EPG-SYNC] end ok=%d %ls path=%ls",
        out.ok ? 1 : 0,
        out.message.c_str(),
        out.tasksIniPath.c_str());

    if (out.ok) {
        SystemInfoLogInfo(
            L"EPG更新後の予約タスクを再読込し、実行監視へ反映しました。tasks.ini=%ls",
            out.tasksIniPath.c_str());
    }
    else {
        SystemInfoLogWarn(
            L"EPG更新後の予約タスク再同期に失敗しました。tasks.ini=%ls result=%ls",
            out.tasksIniPath.c_str(),
            out.message.c_str());
    }

    return out;
}

TaskSystemConnector::EpgUpdateTaskRefreshResult
TaskSystemConnector::RefreshProgramReservationsAfterEpgUpdate(
    const EpgUpdateTaskRefreshRequest& req)
{
    EpgUpdateTaskRefreshResult out{};

    if (!initialized_) {
        out.message = L"TaskSystemConnector is not initialized";

        DebugLogFormat(
            L"[TaskSystemConnector][EPG-TASK-REFRESH] skipped not initialized source=%ls",
            req.source.empty() ? L"(unknown)" : req.source.c_str());

        return out;
    }

    out.candidates = req.programs.size();

    DebugLogFormat(
        L"[TaskSystemConnector][EPG-TASK-REFRESH] begin source=%ls space=%d ch=%d nid=%u tsid=%u sid=%u programs=%zu epg=%ls",
        req.source.empty() ? L"(unknown)" : req.source.c_str(),
        req.space,
        req.physicalChannel,
        static_cast<unsigned>(req.networkId),
        static_cast<unsigned>(req.tsId),
        static_cast<unsigned>(req.serviceId),
        req.programs.size(),
        req.epgPath.empty() ? L"(empty)" : req.epgPath.c_str());

    if (req.programs.empty()) {
        if (!req.refreshEvenIfNoProgram) {
            out.ok = true;
            out.message = L"no program candidates";
            return out;
        }

        // RunReservationMaintenanceAfterEpgBuild() 内で
        // SaveTasksToConfiguredIni_() と ApplyTaskList() まで行う。
        // ここで追加 Apply / Save は呼ばない。
        out.maintenanceOk =
            RunReservationMaintenanceAfterEpgBuild(req.initialBuild);

        out.applyOk = out.maintenanceOk;
        out.saveOk = out.maintenanceOk;

        out.ok = out.maintenanceOk;

        out.message =
            L"no programs maintenance=" +
            std::to_wstring(out.maintenanceOk ? 1 : 0) +
            L" apply=" +
            std::to_wstring(out.applyOk ? 1 : 0) +
            L" save=" +
            std::to_wstring(out.saveOk ? 1 : 0);

        DebugLogFormat(
            L"[TaskSystemConnector][EPG-TASK-REFRESH] end ok=%d %ls",
            out.ok ? 1 : 0,
            out.message.c_str());

        return out;
    }

    for (const auto& p : req.programs) {
        if (!ProgramAdjustRequestMatchesUpdatedChannel_(p, req)) {
            ++out.skippedByChannel;
            continue;
        }

        const bool adjusted =
            AdjustProgramReservationFromEit(p);

        if (adjusted) {
            ++out.adjusted;
        }
        else {
            ++out.failed;
        }
    }

    // AdjustProgramReservationFromEit() は、実変更がある場合は
    // その場で ApplyTaskList() と SaveTasksToConfiguredIni_() まで行う。
    //
    // ただし NO-CHANGE や channel skip だけのケースでも、
    // EPG更新後の保守・期限切れ整理・TimeKeeper同期は1回だけ行う。
    //
    // RunReservationMaintenanceAfterEpgBuild() 内で
    // SaveTasksToConfiguredIni_() と ApplyTaskList() を行うため、
    // ここでは追加 Apply / Save を呼ばない。
    out.maintenanceOk =
        RunReservationMaintenanceAfterEpgBuild(req.initialBuild);

    out.applyOk = out.maintenanceOk;
    out.saveOk = out.maintenanceOk;

    out.ok =
        out.maintenanceOk &&
        out.failed == 0;

    out.message =
        L"candidates=" +
        std::to_wstring(out.candidates) +
        L" adjusted=" +
        std::to_wstring(out.adjusted) +
        L" skippedByChannel=" +
        std::to_wstring(out.skippedByChannel) +
        L" failed=" +
        std::to_wstring(out.failed) +
        L" maintenance=" +
        std::to_wstring(out.maintenanceOk ? 1 : 0) +
        L" apply=" +
        std::to_wstring(out.applyOk ? 1 : 0) +
        L" save=" +
        std::to_wstring(out.saveOk ? 1 : 0);

    DebugLogFormat(
        L"[TaskSystemConnector][EPG-TASK-REFRESH] end ok=%d %ls",
        out.ok ? 1 : 0,
        out.message.c_str());

    return out;
}

TaskSystemConnector::EpgUpdateTaskRefreshResult
TaskSystemConnector::RefreshProgramReservationByEventId(
    const ProgramEventUpdateRequest& req)
{
    EpgUpdateTaskRefreshResult out{};

    if (req.serviceId == 0 || req.eventId == 0) {
        out.ok = false;
        out.message = L"serviceId or eventId is empty";

        DebugLogFormat(
            L"[TaskSystemConnector][EPG-EVENT-UPDATE] rejected source=%ls space=%d ch=%d nid=%u tsid=%u sid=%u eid=%u title=%ls",
            req.source.empty() ? L"(unknown)" : req.source.c_str(),
            req.space,
            req.physicalChannel,
            static_cast<unsigned>(req.networkId),
            static_cast<unsigned>(req.tsId),
            static_cast<unsigned>(req.serviceId),
            static_cast<unsigned>(req.eventId),
            req.title.empty() ? L"(empty)" : req.title.c_str());

        return out;
    }

    ProgramReservationAdjustRequest ar{};
    ar.instance = 0;

    ar.space = req.space;
    ar.networkId = req.networkId;
    ar.tsId = req.tsId;
    ar.serviceId = req.serviceId;
    ar.eventId = req.eventId;

    ar.title = req.title;
    ar.titleLabel =
        req.titleLabel.empty()
        ? req.title
        : req.titleLabel;

    ar.summary = req.summary;
    ar.genre = req.genre;

    ar.startDate = req.startDate;
    ar.startTime = req.startTime;
    ar.endDate = req.endDate;
    ar.endTime = req.endTime;
    ar.durationSec = req.durationSec;

    ar.allowTitleUpdate = req.allowTitleUpdate;
    ar.allowStartTimeUpdate = req.allowStartTimeUpdate;
    ar.allowStopTimeUpdate = req.allowStopTimeUpdate;

    EpgUpdateTaskRefreshRequest notice{};
    notice.source =
        req.source.empty()
        ? L"epg-event-update"
        : req.source;

    notice.space = req.space;
    notice.physicalChannel = req.physicalChannel;
    notice.networkId = req.networkId;
    notice.tsId = req.tsId;
    notice.serviceId = req.serviceId;
    notice.epgPath = req.epgPath;

    notice.programs.push_back(std::move(ar));
    notice.refreshEvenIfNoProgram = true;
    notice.initialBuild = req.initialBuild;

    DebugLogFormat(
        L"[TaskSystemConnector][EPG-EVENT-UPDATE] begin source=%ls space=%d ch=%d nid=%u tsid=%u sid=%u eid=%u title=%ls start=%ls %ls end=%ls %ls",
        notice.source.c_str(),
        req.space,
        req.physicalChannel,
        static_cast<unsigned>(req.networkId),
        static_cast<unsigned>(req.tsId),
        static_cast<unsigned>(req.serviceId),
        static_cast<unsigned>(req.eventId),
        req.title.empty() ? L"(empty)" : req.title.c_str(),
        req.startDate.empty() ? L"(empty)" : req.startDate.c_str(),
        req.startTime.empty() ? L"(empty)" : req.startTime.c_str(),
        req.endDate.empty() ? L"(empty)" : req.endDate.c_str(),
        req.endTime.empty() ? L"(empty)" : req.endTime.c_str());

    out = RefreshProgramReservationsAfterEpgUpdate(notice);

    DebugLogFormat(
        L"[TaskSystemConnector][EPG-EVENT-UPDATE] end ok=%d adjusted=%zu failed=%zu skipped=%zu msg=%ls",
        out.ok ? 1 : 0,
        out.adjusted,
        out.failed,
        out.skippedByChannel,
        out.message.empty() ? L"(empty)" : out.message.c_str());

    return out;
}

bool TaskSystemConnector::Initialize(HWND hwnd, UINT_PTR timerId,
    const Options& opt,
    const Callbacks& cb)
{
    hwnd_ = hwnd;
    timerId_ = timerId;
    opt_ = opt;
    cb_ = cb;

    Log_(L"[TaskSystemConnector] Initialize begin (enableAuto=%d tickSec=%d idleSec=%d)",
        opt_.enableAuto ? 1 : 0,
        opt_.tickSeconds,
        opt_.idleRequiredSeconds);

    MultiTunerTaskRunner::Settings rs{};
    rs.tunerCount = (opt_.tunerCount <= 0 ? 1 : opt_.tunerCount);
    rs.enableUtilityWorker = opt_.enableUtilityWorker;
    rs.maxQueueSize = opt_.maxQueueSize;

    MultiTunerTaskRunner::Hooks rh{};
    rh.Log = cb_.Log;
    rh.CanRun = cb_.CanRun;

    if (!runner_.Initialize(rs, rh)) {
        Log_(L"[TaskSystemConnector] runner.Initialize failed");
        SystemInfoLogError(L"予約実行系の初期化に失敗しました。runner.Initialize failed");
        initialized_ = false;
        return false;
    }

    reservationApiServer_.SetConnector(this);

    if (!reservationApiWriteOptionsReady_) {
        reservationApiWriteOptions_ =
            BuildDefaultReservationWriteOptions_(opt_.tasksIniPath);
        reservationApiWriteOptionsReady_ = true;
    }
    reservationApiServer_.SetWriteOptions(reservationApiWriteOptions_);

    runner_.RegisterHandler(
        TaskKinds::EpgFull,
        [this](int tunerIndex, IBonDriver2* bon, const TimeKeeper::TaskRequest& r) {
            Log_(L"[TaskSystemConnector] Handler(EpgFull) fired (tuner=%d kind=%u inst=%u prio=%u reqTuners=%u bon=%p)",
                tunerIndex,
                static_cast<unsigned>(r.kind),
                static_cast<unsigned>(r.instance),
                static_cast<unsigned>(r.priority),
                static_cast<unsigned>(r.requiredTuners),
                bon);

            WaitUntilDispatchTime_(r);
            WriteTaskExecutionStartLog_(r, tunerIndex);
            MarkTaskExecuted_(r.kind, r.instance);

            if (cb_.OnEpgFull) {
                cb_.OnEpgFull(tunerIndex, bon);

                // EPG予約による取得が完了したら、再起動を待たずに次回OneShotへ更新する。
                (void)RefreshNextEpgOneShotAfterAcquired_(r, L"EpgFull completed");
            }
            else {
                WriteTaskExecutionFailLog_(r, L"OnEpgFull callback is null");
                Log_(L"[TaskSystemConnector] OnEpgFull callback is null");
            }
        });

    runner_.RegisterHandler(
        TaskKinds::EpgLight,
        [this](int tunerIndex, IBonDriver2* bon, const TimeKeeper::TaskRequest& r) {
            Log_(L"[TaskSystemConnector] Handler(EpgLight) fired (tuner=%d kind=%u inst=%u prio=%u reqTuners=%u bon=%p)",
                tunerIndex,
                static_cast<unsigned>(r.kind),
                static_cast<unsigned>(r.instance),
                static_cast<unsigned>(r.priority),
                static_cast<unsigned>(r.requiredTuners),
                bon);

            WaitUntilDispatchTime_(r);
            WriteTaskExecutionStartLog_(r, tunerIndex);
            MarkTaskExecuted_(r.kind, r.instance);

            if (cb_.OnEpgLight) {
                cb_.OnEpgLight(tunerIndex, bon);

                // EPG予約による取得が完了したら、再起動を待たずに次回OneShotへ更新する。
                (void)RefreshNextEpgOneShotAfterAcquired_(r, L"EpgLight completed");
            }
            else {
                WriteTaskExecutionFailLog_(r, L"OnEpgLight callback is null");
                Log_(L"[TaskSystemConnector] OnEpgLight callback is null");
            }
        });

    runner_.RegisterHandler(
        TaskKinds::RecPrepare,
        [this](int tunerIndex, IBonDriver2* bon, const TimeKeeper::TaskRequest& r) {
            Log_(L"[TaskSystemConnector] Handler(RecPrepare) fired (tuner=%d kind=%u inst=%u bon=%p)",
                tunerIndex,
                static_cast<unsigned>(r.kind),
                static_cast<unsigned>(r.instance),
                bon);

            WaitUntilDispatchTime_(r);
            WriteTaskExecutionStartLog_(r, tunerIndex);
            MarkTaskExecuted_(r.kind, r.instance);

            if (cb_.OnRecPrepare) {
                cb_.OnRecPrepare(tunerIndex, bon, r);
            }
            else {
                WriteTaskExecutionFailLog_(r, L"OnRecPrepare callback is null");
            }
        });

    runner_.RegisterHandler(
        TaskKinds::RecStart,
        [this](int tunerIndex, IBonDriver2* bon, const TimeKeeper::TaskRequest& r) {
            Log_(L"[TaskSystemConnector] Handler(RecStart) fired (tuner=%d kind=%u inst=%u bon=%p)",
                tunerIndex,
                static_cast<unsigned>(r.kind),
                static_cast<unsigned>(r.instance),
                bon);

            WaitUntilDispatchTime_(r);
            WriteTaskExecutionStartLog_(r, tunerIndex);
            MarkTaskExecuted_(r.kind, r.instance);

            if (cb_.OnRecStart) {
                cb_.OnRecStart(tunerIndex, bon, r);
            }
            else {
                WriteTaskExecutionFailLog_(r, L"OnRecStart callback is null");
            }
        });

    runner_.RegisterHandler(
        TaskKinds::RecStop,
        [this](int tunerIndex, IBonDriver2* bon, const TimeKeeper::TaskRequest& r) {
            Log_(L"[TaskSystemConnector] Handler(RecStop) fired (tuner=%d kind=%u inst=%u bon=%p)",
                tunerIndex,
                static_cast<unsigned>(r.kind),
                static_cast<unsigned>(r.instance),
                bon);

            WaitUntilDispatchTime_(r);
            WriteTaskExecutionStartLog_(r, tunerIndex);
            MarkTaskExecuted_(r.kind, r.instance);

            if (cb_.OnRecStop) {
                cb_.OnRecStop(tunerIndex, bon, r);
            }
            else {
                WriteTaskExecutionFailLog_(r, L"OnRecStop callback is null");
            }
        });

    TimeKeeper::Hooks hk{};
    hk.Log = cb_.Log;
    hk.CanRun = cb_.CanRun;
    hk.EnqueueTask = [this](const TimeKeeper::TaskRequest& r) -> bool {
        DebugLogFormat(
            L"[TaskSystemConnector][ENQUEUE] request kind=%u inst=%u prio=%u reqTuners=%u pref=%d dispatch=%08d %02d:%02d:%02d",
            static_cast<unsigned>(r.kind),
            static_cast<unsigned>(r.instance),
            static_cast<unsigned>(r.priority),
            static_cast<unsigned>(r.requiredTuners),
            r.preferredTunerIndex,
            r.dispatchYmd,
            r.dispatchHHMM / 100,
            r.dispatchHHMM % 100,
            r.dispatchSecond);

        if (!TryMarkTaskDispatching_(r.kind, r.instance)) {
            return true;
        }

        const bool ok = runner_.Enqueue(r);

        DebugLogFormat(
            L"[TaskSystemConnector][ENQUEUE] result kind=%u inst=%u ok=%d",
            static_cast<unsigned>(r.kind),
            static_cast<unsigned>(r.instance),
            ok ? 1 : 0);

        if (!ok) {
            // enqueue に失敗した場合は再試行できるように実行済み扱いを戻す。
            ResetExecutionMonitorForTask_(r.kind, r.instance);
            WriteTaskExecutionFailLog_(r, L"runner.Enqueue failed");
        }

        return ok;
        };

    TimeKeeper::Settings base{};
    base.enableAuto = opt_.enableAuto;
    base.bootGraceSeconds = opt_.bootGraceSeconds;
    base.catchUpOnStart = opt_.catchUpOnStart;
    base.tickSeconds = opt_.tickSeconds;
    base.idleRequiredSeconds = opt_.idleRequiredSeconds;
    base.enqueueRetryBackoffSeconds = opt_.enqueueRetryBackoffSeconds;
    base.epgRequiredTuners = (opt_.epgRequiredTuners == 0 ? 1 : opt_.epgRequiredTuners);

    timeKeeper_.Configure(base, hk);

    overdueCheckIntervalSec_ = 15;
    lastOverdueCheck_ = {};

    initialized_ = true;
    running_ = false;

    Log_(L"[TaskSystemConnector] initialized ok (tasksIniPath=%ls)",
        opt_.tasksIniPath.empty() ? L"(empty)" : opt_.tasksIniPath.c_str());

    return true;
}

void TaskSystemConnector::SetTuner(int tunerIndex, IBonDriver2* bon)
{
    runner_.SetTuner(tunerIndex, bon);
    Log_(L"[TaskSystemConnector] SetTuner(tuner=%d bon=%p)", tunerIndex, bon);
}

void TaskSystemConnector::SetTunerAvailable(
    int tunerIndex,
    bool available)
{
    runner_.SetTunerAvailable(tunerIndex, available);
    Log_(
        L"[TaskSystemConnector] SetTunerAvailable(tuner=%d available=%d)",
        tunerIndex,
        available ? 1 : 0);
}

bool TaskSystemConnector::LoadTasksFromIni(const std::wstring& path)
{
    if (!initialized_) {
        return false;
    }

    const bool ok = taskList_.LoadFromIni(path);

    Log_(
        L"[TaskSystemConnector] LoadTasksFromIni(path=%ls) => %d",
        path.c_str(),
        ok ? 1 : 0);

    if (ok) {
        if (!RefreshReservationCaches_(path)) {
            Log_(L"[TaskSystemConnector] reservation cache refresh failed on load");
            SystemInfoLogWarn(
                L"予約キャッシュの更新に失敗しました。tasks.ini の読み込みは成功しています。");
        }

        NotifyReservationDataChanged_();
    }

    return ok;
}

bool TaskSystemConnector::SaveTasksToIni(const std::wstring& path) const
{
    if (!initialized_) {
        return false;
    }

    const bool ok = taskList_.SaveToIni(path);

    DebugLog(
        L"%ls",
        ok
        ? L"[TaskSystemConnector] SaveTasksToIni => ok"
        : L"[TaskSystemConnector] SaveTasksToIni => failed");

    if (!ok) {
        if (HasEnabledEpgTask_(taskList_)) {
            SystemInfoLogError(
                L"予約の保存に失敗しました。tasks.ini への書き込みに失敗しました。");
        }
        return false;
    }

    if (!RefreshReservationCaches_(path)) {
        SystemInfoLogWarn(
            L"予約キャッシュの更新に失敗しました。tasks.ini の保存は成功しています。");
        // tasks.ini が正本なので、キャッシュ失敗では保存失敗扱いにしない
    }

    NotifyReservationDataChanged_();

    return true;
}

bool TaskSystemConnector::Start()
{
    if (!initialized_) return false;
    if (running_) {
        Log_(L"[TaskSystemConnector] Start skipped (already running)");
        return true;
    }

    Log_(L"[TaskSystemConnector] Start begin (enableAuto=%d tickSec=%d)",
        opt_.enableAuto ? 1 : 0,
        opt_.tickSeconds);

    runner_.Start();

    if (!timeKeeper_.Start(hwnd_, timerId_)) {
        Log_(L"[TaskSystemConnector] TimeKeeper.Start failed");
        if (HasEnabledEpgTask_(taskList_)) {
            SystemInfoLogError(L"予約監視の起動に失敗しました。TimeKeeper.Start failed");
        }
        runner_.Stop(false);
        running_ = false;
        return false;
    }

    running_ = true;

    if (!StartReservationApiServer()) {
        Log_(L"[TaskSystemConnector] reservation api start failed");
        SystemInfoLogWarn(L"予約APIサーバーの起動に失敗しました。番組表からの予約登録が無効になる可能性があります。");
    }

    Log_(L"[TaskSystemConnector] started");
    return true;
}

void TaskSystemConnector::Stop(bool clearQueue)
{
    if (!running_) return;

    Log_(L"[TaskSystemConnector] Stop(clearQueue=%d)", clearQueue ? 1 : 0);

    StopReservationApiServer();

    timeKeeper_.Stop(hwnd_);
    runner_.Stop(clearQueue);

    running_ = false;
    Log_(L"[TaskSystemConnector] stopped");
}

void TaskSystemConnector::OnTick()
{
    if (!running_) {
        Log_(L"[TaskSystemConnector] OnTick ignored (running_=false)");
        return;
    }

    // 軽量監視。
    // 予約の発火判定だけを高頻度で行う。
    timeKeeper_.OnTick();

    // 重い保守処理は間引く。
    // EmitOverdueTaskWarnings_() は未実行予約の削除、
    // ApplyTaskList、SaveTasksToIni、キャッシュ更新へ進む可能性があるため、
    // 1秒周期では回さない。
    const auto now = std::chrono::steady_clock::now();

    const bool first =
        lastOverdueCheck_ == std::chrono::steady_clock::time_point{};

    const bool due =
        first ||
        (now - lastOverdueCheck_) >=
        std::chrono::seconds((std::max)(1, overdueCheckIntervalSec_));

    if (due) {
        lastOverdueCheck_ = now;

        // timeKeeper_.OnTick() に RecStop を先に発火させたうえで、
        // 終了時刻に達した番組予約を3タスクまとめて削除する。
        // これにより EPGタブの代表行 (RecStart) は番組終了まで残る。
        const bool purgedProgramReservations =
            PurgeExpiredProgramReservationGroups_(taskList_);

        if (purgedProgramReservations) {
            if (!ApplyTaskList()) {
                SystemInfoLogError(
                    L"終了済み番組予約の削除後、タスクリストの再反映に失敗しました。");
            }

            if (!SaveTasksToConfiguredIni_()) {
                SystemInfoLogError(
                    L"終了済み番組予約の削除後、tasks.ini の保存に失敗しました。");
            }
        }

        EmitOverdueTaskWarnings_();
    }
}

void TaskSystemConnector::SetEnableAuto(bool enable)
{
    opt_.enableAuto = enable;
    Log_(L"[TaskSystemConnector] SetEnableAuto(%d)", enable ? 1 : 0);
}

void TaskSystemConnector::SetTickSeconds(int sec)
{
    opt_.tickSeconds = sec;
    Log_(L"[TaskSystemConnector] SetTickSeconds(%d) (effective after re-Start)", sec);
}

void TaskSystemConnector::SetIdleRequiredSeconds(int sec)
{
    opt_.idleRequiredSeconds = sec;
    Log_(L"[TaskSystemConnector] SetIdleRequiredSeconds(%d)", sec);
}

void TaskSystemConnector::SetBootGraceSeconds(int sec)
{
    opt_.bootGraceSeconds = sec;
    Log_(L"[TaskSystemConnector] SetBootGraceSeconds(%d)", sec);
}

void TaskSystemConnector::SetReservationApiWriteOptions(
    const EpgHtmlPortal::WriteOptions& opt)
{
    reservationApiWriteOptions_ = opt;
    reservationApiWriteOptionsReady_ = true;

    reservationApiServer_.SetConnector(this);
    reservationApiServer_.SetWriteOptions(reservationApiWriteOptions_);
}

bool TaskSystemConnector::StartReservationApiServer()
{
    if (!initialized_) {
        return false;
    }

    reservationApiServer_.SetConnector(this);

    if (!ReadNetworkEnableServerFromConfig_()) {
        reservationApiServer_.Stop();
        Log_(L"[TaskSystemConnector] reservation api disabled by Network.EnableServer=0");
        return true;
    }

    if (!ReadNetworkAutoStartServerFromConfig_()) {
        reservationApiServer_.Stop();
        Log_(L"[TaskSystemConnector] reservation api autostart disabled");
        return true;
    }

    if (!reservationApiWriteOptionsReady_) {
        reservationApiWriteOptions_ =
            BuildDefaultReservationWriteOptions_(opt_.tasksIniPath);
        reservationApiWriteOptionsReady_ = true;
    }

    reservationApiServer_.SetWriteOptions(reservationApiWriteOptions_);

    const std::uint16_t port =
        static_cast<std::uint16_t>(ReadNetworkHttpPortFromConfig_());

    if (reservationApiServer_.IsRunning()) {
        if (reservationApiServer_.GetPort() == port) {
            return true;
        }
        reservationApiServer_.Stop();
    }

    const bool ok =
        reservationApiServer_.Start(port, this, reservationApiWriteOptions_);

    if (ok) {
        Log_(L"[TaskSystemConnector] reservation api started port=%u",
            static_cast<unsigned>(port));
    }

    return ok;
}

void TaskSystemConnector::StopReservationApiServer()
{
    if (reservationApiServer_.IsRunning()) {
        reservationApiServer_.Stop();
        Log_(L"[TaskSystemConnector] reservation api stopped");
    }
}

bool TaskSystemConnector::ApplyTaskList()
{
    if (!initialized_) {
        return false;
    }

    const bool purgedExpiredRecords =
        PurgeExpiredProgramReservationGroups_(taskList_);

    if (purgedExpiredRecords) {
        if (!SaveTasksToConfiguredIni_()) {
            SystemInfoLogWarn(
                L"終了済み番組予約を削除しましたが、tasks.ini の保存に失敗しました。");
        }

        DebugLog(
            L"[TaskSystemConnector] expired program reservation groups purged before ApplyTaskList");
    }

    TimeKeeper::Settings base{};
    base.enableAuto = opt_.enableAuto;
    base.bootGraceSeconds = opt_.bootGraceSeconds;
    base.catchUpOnStart = opt_.catchUpOnStart;
    base.tickSeconds = opt_.tickSeconds;
    base.idleRequiredSeconds = opt_.idleRequiredSeconds;
    base.enqueueRetryBackoffSeconds = opt_.enqueueRetryBackoffSeconds;
    base.epgRequiredTuners =
        (opt_.epgRequiredTuners == 0 ? 1 : opt_.epgRequiredTuners);

    TimeKeeper::Hooks hk{};
    hk.Log = cb_.Log;
    hk.CanRun = cb_.CanRun;

    hk.EnqueueTask = [this](const TimeKeeper::TaskRequest& r) -> bool {
        DebugLogFormat(
            L"[TaskSystemConnector][ENQUEUE] request kind=%u inst=%u prio=%u reqTuners=%u pref=%d dispatch=%08d %02d:%02d:%02d",
            static_cast<unsigned>(r.kind),
            static_cast<unsigned>(r.instance),
            static_cast<unsigned>(r.priority),
            static_cast<unsigned>(r.requiredTuners),
            r.preferredTunerIndex,
            r.dispatchYmd,
            r.dispatchHHMM / 100,
            r.dispatchHHMM % 100,
            r.dispatchSecond);

        if (!TryMarkTaskDispatching_(r.kind, r.instance)) {
            return true;
        }

        const bool ok = runner_.Enqueue(r);

        DebugLogFormat(
            L"[TaskSystemConnector][ENQUEUE] result kind=%u inst=%u ok=%d",
            static_cast<unsigned>(r.kind),
            static_cast<unsigned>(r.instance),
            ok ? 1 : 0);

        if (!ok) {
            ResetExecutionMonitorForTask_(r.kind, r.instance);
            WriteTaskExecutionFailLog_(r, L"runner.Enqueue failed");
        }

        return ok;
        };

    const auto snap = taskList_.GetSnapshot();

    Log_(
        L"[TaskSystemConnector] ApplyTaskList begin (tasks=%zu enableAuto=%d tickSec=%d)",
        snap.size(),
        base.enableAuto ? 1 : 0,
        base.tickSeconds);

    taskList_.ApplyToTimeKeeper(
        timeKeeper_,
        base,
        hk);

    std::size_t oneshotEnabled = 0;

    for (const auto& t : snap) {
        if (t.enabled &&
            t.schedule.type == TaskListManager::ScheduleType::OneShot) {
            ++oneshotEnabled;

            Log_(
                L"[TaskSystemConnector] OneShot enabled: kind=%u inst=%u due=%08d %04d:%02d tol=%d end=%08d %04d",
                static_cast<unsigned>(t.request.kind),
                static_cast<unsigned>(t.request.instance),
                t.schedule.dueYmd,
                t.schedule.dueHHMM,
                t.schedule.dueSecond,
                t.schedule.lateToleranceMinutes,
                t.display.endYmd,
                t.display.endHHMM);
        }
    }

    SyncExecutionMonitorWithTaskList_();

    Log_(
        L"[TaskSystemConnector] ApplyTaskList done (oneshotEnabled=%zu)",
        oneshotEnabled);

    return true;
}
bool TaskSystemConnector::UpsertTaskAndApply(const TaskListManager::TaskEntry& entry)
{
    if (!initialized_) return false;

    const auto before = taskList_.Find(entry.request.kind, entry.request.instance);

    Log_(L"[TaskSystemConnector] UpsertTaskAndApply(kind=%u inst=%u enabled=%d type=%d)",
        static_cast<unsigned>(entry.request.kind),
        static_cast<unsigned>(entry.request.instance),
        entry.enabled ? 1 : 0,
        static_cast<int>(entry.schedule.type));

    ResetExecutionMonitorForTask_(entry.request.kind, entry.request.instance);
    taskList_.UpsertTask(entry);

    const bool ok = ApplyTaskList();
    if (!ok) {
        SystemInfoLogError(
            L"%lsの登録に失敗しました。kind=%u instance=%u ApplyTaskList failed",
            GetReservationText_(entry.request.kind),
            static_cast<unsigned>(entry.request.kind),
            static_cast<unsigned>(entry.request.instance));
        return false;
    }

    if (!SaveTasksToConfiguredIni_()) {
        SystemInfoLogError(
            L"%lsの保存に失敗しました。kind=%u instance=%u tasks.ini 書き込み失敗",
            GetReservationText_(entry.request.kind),
            static_cast<unsigned>(entry.request.kind),
            static_cast<unsigned>(entry.request.instance));
        return false;
    }

    const auto after = taskList_.Find(entry.request.kind, entry.request.instance);
    if (after) {
        if (before) {
            if (IsTaskNameChanged_(*before, *after)) {
                WriteTaskRenamedLog_(*before, *after);
            }
            WriteTaskRegisteredLog_(*after, true);
        }
        else {
            WriteTaskRegisteredLog_(*after, false);
        }
    }

    return true;
}

bool TaskSystemConnector::RemoveTaskAndApply(std::uint32_t kind, std::uint32_t instance)
{
    if (!initialized_) return false;

    const bool removed = taskList_.RemoveTask(kind, instance);
    ResetExecutionMonitorForTask_(kind, instance);

    Log_(L"[TaskSystemConnector] RemoveTaskAndApply(kind=%u inst=%u) removed=%d",
        static_cast<unsigned>(kind),
        static_cast<unsigned>(instance),
        removed ? 1 : 0);

    const bool ok = ApplyTaskList();
    if (!ok) {
        SystemInfoLogError(
            L"%ls削除後の再反映に失敗しました。kind=%u instance=%u",
            GetReservationText_(kind),
            static_cast<unsigned>(kind),
            static_cast<unsigned>(instance));
        return false;
    }

    if (!SaveTasksToConfiguredIni_()) {
        SystemInfoLogError(
            L"%ls削除後の保存に失敗しました。kind=%u instance=%u tasks.ini 書き込み失敗",
            GetReservationText_(kind),
            static_cast<unsigned>(kind),
            static_cast<unsigned>(instance));
        return false;
    }

    WriteTaskDeletedLog_(kind, instance, removed);
    return true;
}

bool TaskSystemConnector::ScheduleOneShot(std::uint32_t kind, std::uint32_t instance,
    int dueYmd, int dueHHMM, int lateToleranceMinutes, bool enabled, const std::wstring& name)
{
    if (!initialized_) return false;

    TaskListManager::TaskEntry t{};
    t.name = name.empty() ? L"Task OneShot" : name;
    t.enabled = enabled;

    t.request.kind = kind;
    t.request.instance = instance;
    t.request.priority = TimeKeeper::TaskPriority::Soft;
    t.request.requiredTuners = (opt_.epgRequiredTuners == 0 ? 1 : opt_.epgRequiredTuners);
    t.request.preferredTunerIndex = -1;

    t.schedule.type = TaskListManager::ScheduleType::OneShot;
    t.schedule.dailyHour = 0;
    t.schedule.dailyMinute = 0;
    t.schedule.hourlyAtMinute = 0;
    t.schedule.intervalMinutes = 0;
    t.schedule.dueYmd = dueYmd;
    t.schedule.dueHHMM = dueHHMM;
    t.schedule.lateToleranceMinutes = lateToleranceMinutes;

    return UpsertTaskAndApply(t);
}

bool TaskSystemConnector::ScheduleDaily(std::uint32_t kind, std::uint32_t instance,
    int hour, int minute, bool enabled, const std::wstring& name)
{
    if (!initialized_) return false;

    TaskListManager::TaskEntry t{};
    t.name = name.empty() ? L"Task Daily" : name;
    t.enabled = enabled;

    t.request.kind = kind;
    t.request.instance = instance;
    t.request.priority = TimeKeeper::TaskPriority::Soft;
    t.request.requiredTuners = (opt_.epgRequiredTuners == 0 ? 1 : opt_.epgRequiredTuners);
    t.request.preferredTunerIndex = -1;

    t.schedule.type = TaskListManager::ScheduleType::Daily;
    t.schedule.dailyHour = hour;
    t.schedule.dailyMinute = minute;
    t.schedule.hourlyAtMinute = 0;
    t.schedule.intervalMinutes = 0;
    t.schedule.dueYmd = 0;
    t.schedule.dueHHMM = 0;
    t.schedule.lateToleranceMinutes = -1;

    return UpsertTaskAndApply(t);
}

bool TaskSystemConnector::ScheduleHourly(std::uint32_t kind, std::uint32_t instance,
    int atMinute, bool enabled, const std::wstring& name)
{
    if (!initialized_) return false;

    TaskListManager::TaskEntry t{};
    t.name = name.empty() ? L"Task Hourly" : name;
    t.enabled = enabled;

    t.request.kind = kind;
    t.request.instance = instance;
    t.request.priority = TimeKeeper::TaskPriority::Soft;
    t.request.requiredTuners = (opt_.epgRequiredTuners == 0 ? 1 : opt_.epgRequiredTuners);
    t.request.preferredTunerIndex = -1;

    t.schedule.type = TaskListManager::ScheduleType::Hourly;
    t.schedule.dailyHour = 0;
    t.schedule.dailyMinute = 0;
    t.schedule.hourlyAtMinute = atMinute;
    t.schedule.intervalMinutes = 0;
    t.schedule.dueYmd = 0;
    t.schedule.dueHHMM = 0;
    t.schedule.lateToleranceMinutes = -1;

    return UpsertTaskAndApply(t);
}

bool TaskSystemConnector::ScheduleInterval(std::uint32_t kind, std::uint32_t instance,
    int intervalMinutes, bool enabled, const std::wstring& name)
{
    if (!initialized_) return false;

    TaskListManager::TaskEntry t{};
    t.name = name.empty() ? L"Task Interval" : name;
    t.enabled = enabled;

    t.request.kind = kind;
    t.request.instance = instance;
    t.request.priority = TimeKeeper::TaskPriority::Soft;
    t.request.requiredTuners = (opt_.epgRequiredTuners == 0 ? 1 : opt_.epgRequiredTuners);
    t.request.preferredTunerIndex = -1;

    t.schedule.type = TaskListManager::ScheduleType::Interval;
    t.schedule.dailyHour = 0;
    t.schedule.dailyMinute = 0;
    t.schedule.hourlyAtMinute = 0;
    t.schedule.intervalMinutes = intervalMinutes;
    t.schedule.dueYmd = 0;
    t.schedule.dueHHMM = 0;
    t.schedule.lateToleranceMinutes = -1;

    return UpsertTaskAndApply(t);
}

std::vector<TaskSystemConnector::TaskViewRow> TaskSystemConnector::GetTaskViewSnapshot() const
{
    std::vector<TaskViewRow> out;
    const auto snap = taskList_.GetSnapshot();
    out.reserve(snap.size());

    for (const auto& t : snap) {
        TaskViewRow r{};
        r.kind = t.request.kind;
        r.instance = t.request.instance;
        r.enabled = t.enabled;

        r.disabledByUser = t.reserve.disabledByUser;
        r.excludeFromAuto = t.reserve.excludeFromAuto;
        r.autoReserved = t.reserve.autoReserved;

        r.scheduleType = t.schedule.type;
        r.dueYmd = t.schedule.dueYmd;
        r.dueHHMM = t.schedule.dueHHMM;
        r.lateToleranceMinutes = t.schedule.lateToleranceMinutes;

        r.dailyHour = t.schedule.dailyHour;
        r.dailyMinute = t.schedule.dailyMinute;
        r.hourlyAtMinute = t.schedule.hourlyAtMinute;
        r.intervalMinutes = t.schedule.intervalMinutes;

        r.useTimeWindow = t.schedule.useTimeWindow;
        r.windowStartYmd = t.schedule.windowStartYmd;
        r.windowStartHHMM = t.schedule.windowStartHHMM;
        r.windowEndYmd = t.schedule.windowEndYmd;
        r.windowEndHHMM = t.schedule.windowEndHHMM;

        r.label = t.display.label.empty() ? t.name : t.display.label;
        r.category = t.display.category.empty() ? GetTaskCategoryText_(t.request.kind) : t.display.category;
        r.target = t.display.target;
        r.note = t.display.note.empty() ? MakeScheduleText_(t) : t.display.note;
        r.name = t.name;

        r.reserveMode = t.reserve.reserveMode;
        r.reserveSource = t.reserve.reserveSource;
        r.dedupeKey = t.reserve.dedupeKey;
        r.excludeKey = t.reserve.excludeKey;

        r.endYmd = t.display.endYmd;
        r.endHHMM = t.display.endHHMM;

        r.requiredTuners = (t.request.requiredTuners == 0) ? 1 : t.request.requiredTuners;
        r.preferredTunerIndex = t.request.preferredTunerIndex;

        {
            const std::wstring tunerText = BuildTunerDisplayText_(r.preferredTunerIndex);
            if (r.note.empty()) {
                r.note = tunerText;
            }
            else {
                r.note += L" / ";
                r.note += tunerText;
            }
        }

        out.push_back(std::move(r));
    }

    return out;
}

TaskSystemConnector::ManualEpgStartCheckResult
TaskSystemConnector::CanStartManualEpgAcquisition(
    int estimatedMinutes,
    int guardMinutes) const
{
    ManualEpgStartCheckResult result{};
    result.canStart = false;

    estimatedMinutes = (std::clamp)(estimatedMinutes, 1, 240);
    guardMinutes = (std::clamp)(guardMinutes, 0, 60);

    int nowYmd = 0;
    int nowHHMM = -1;
    if (!GetLocalNowYmdHHMM_(nowYmd, nowHHMM)) {
        result.reason = L"現在時刻の取得に失敗しました。";
        return result;
    }

    int epgEndYmd = nowYmd;
    int epgEndHHMM = nowHHMM;
    if (!ShiftLocalYmdHHMM_(
        nowYmd,
        nowHHMM,
        estimatedMinutes + guardMinutes,
        epgEndYmd,
        epgEndHHMM)) {
        result.reason = L"EPG取得予定時間の計算に失敗しました。";
        return result;
    }

    const auto snap = taskList_.GetSnapshot();

    for (const auto& t : snap) {
        if (!t.enabled) {
            continue;
        }

        if (t.schedule.type != TaskListManager::ScheduleType::OneShot) {
            continue;
        }

        if (!IsOneShotRecordPrepareOrStart_(t.request.kind)) {
            continue;
        }

        const int dueYmd = t.schedule.dueYmd;
        const int dueHHMM = t.schedule.dueHHMM;

        if (dueYmd <= 0 || dueHHMM < 0) {
            continue;
        }

        // 1. Prepare / Start が、今からEPG取得予定終了+猶予までに来るなら開始禁止。
        if (IsDateTimeInRangeHalfOpen_(
            dueYmd,
            dueHHMM,
            nowYmd,
            nowHHMM,
            epgEndYmd,
            epgEndHHMM)) {
            result.reason =
                L"番組予約の準備または開始時刻が近いため、手動EPG取得を開始しません。";
            result.conflictName = MakeTaskLabel_(t);
            result.conflictKind = t.request.kind;
            result.conflictInstance = t.request.instance;
            result.conflictYmd = dueYmd;
            result.conflictHHMM = dueHHMM;

            DebugLogFormat(
                L"[TaskSystemConnector][MANUAL-EPG-GUARD] blocked by due task now=%ls epgEnd=%ls conflict=%ls kind=%u inst=%u due=%ls",
                FormatYmdHHMMForLog_(nowYmd, nowHHMM).c_str(),
                FormatYmdHHMMForLog_(epgEndYmd, epgEndHHMM).c_str(),
                result.conflictName.c_str(),
                static_cast<unsigned>(result.conflictKind),
                static_cast<unsigned>(result.conflictInstance),
                FormatYmdHHMMForLog_(dueYmd, dueHHMM).c_str());

            return result;
        }

        // 2. RecStart の番組本体時間中なら開始禁止。
        if (t.request.kind == TaskKinds::RecStart &&
            t.display.endYmd > 0 &&
            t.display.endHHMM >= 0) {
            if (IsDateTimeInRangeHalfOpen_(
                nowYmd,
                nowHHMM,
                dueYmd,
                dueHHMM,
                t.display.endYmd,
                t.display.endHHMM)) {
                result.reason =
                    L"番組予約の実行中のため、手動EPG取得を開始しません。";
                result.conflictName = MakeTaskLabel_(t);
                result.conflictKind = t.request.kind;
                result.conflictInstance = t.request.instance;
                result.conflictYmd = dueYmd;
                result.conflictHHMM = dueHHMM;

                DebugLogFormat(
                    L"[TaskSystemConnector][MANUAL-EPG-GUARD] blocked by running program now=%ls conflict=%ls kind=%u inst=%u start=%ls end=%ls",
                    FormatYmdHHMMForLog_(nowYmd, nowHHMM).c_str(),
                    result.conflictName.c_str(),
                    static_cast<unsigned>(result.conflictKind),
                    static_cast<unsigned>(result.conflictInstance),
                    FormatYmdHHMMForLog_(dueYmd, dueHHMM).c_str(),
                    FormatYmdHHMMForLog_(t.display.endYmd, t.display.endHHMM).c_str());

                return result;
            }
        }
    }

    result.canStart = true;
    result.reason = L"手動EPG取得を開始できます。";
    return result;
}

bool TaskSystemConnector::ScheduleEpgReservation(const EpgReservationRequest& req)
{
    if (!initialized_) return false;
    if (!IsEpgTaskKind_(req.kind)) return false;

    int hour = req.hhmm / 100;
    int minute = req.hhmm % 100;

    if (hour < 0) hour = 0;
    if (hour > 23) hour = 23;
    if (minute < 0) minute = 0;
    if (minute > 59) minute = 59;

    const int hhmm = hour * 100 + minute;
    const int windowMinutes = (req.windowMinutes <= 0) ? 30 : req.windowMinutes;

    const auto enabledTuners = BuildEnabledTunerIndices_(
        opt_.tunerCount,
        ReservationTunerPurpose_::Epg);

    const std::uint8_t requiredTuners =
        ClampRequiredTuners_(
            (req.requiredTuners == 0)
            ? ((opt_.epgRequiredTuners == 0) ? 1 : opt_.epgRequiredTuners)
            : req.requiredTuners,
            enabledTuners);

    const int requestedPreferredTunerIndex =
        ResolveRequestedPreferredTunerIndex_(
            req.preferredTunerIndex,
            enabledTuners);

    const int conflictPolicy = ReadReservationConflictPolicyFromConfig_();
    const int startAdvanceSec = ReadReservationStartAdvanceSecFromConfig_();

    const int oneShotDueYmd = req.forceNextDayOneShot
        ? CalcTomorrowYmdForTask_()
        : ResolveNextDueYmdForTask_(hhmm);

    int oneShotRunYmd = oneShotDueYmd;
    int oneShotRunHHMM = hhmm;
    int oneShotRunSec = 0;
    (void)ShiftLocalYmdHHMMSS_(
        oneShotDueYmd,
        hhmm,
        0,
        -startAdvanceSec,
        oneShotRunYmd,
        oneShotRunHHMM,
        oneShotRunSec);

    int oneShotEndYmd = oneShotDueYmd;
    int oneShotEndHHMM = hhmm;
    if (!ShiftLocalYmdHHMM_(oneShotDueYmd, hhmm, windowMinutes, oneShotEndYmd, oneShotEndHHMM)) {
        oneShotEndYmd = oneShotDueYmd;
        oneShotEndHHMM = hhmm;
    }

    const auto snapshot =
        BuildSnapshotWithoutEpgReservationGroup_(
            taskList_,
            req.kind,
            req.dailyInstance,
            req.oneShotInstance);

    const auto candidateWindow =
        MakeWindow_(oneShotRunYmd, oneShotRunHHMM, oneShotEndYmd, oneShotEndHHMM);

    int preferredTunerIndex = -1;
    if (!ResolvePreferredTunerIndex_(
        enabledTuners,
        snapshot,
        requestedPreferredTunerIndex,
        conflictPolicy,
        candidateWindow,
        preferredTunerIndex)) {
        SystemInfoLogWarn(
            L"EPG取得予約の登録に失敗しました。空きチューナーがありません。hhmm=%04d reqTuners=%u requestedTuner=%d",
            hhmm,
            static_cast<unsigned>(requiredTuners),
            requestedPreferredTunerIndex);
        return false;
    }

    LogTunerAutoReroute_(
        L"EPG予約",
        requestedPreferredTunerIndex,
        preferredTunerIndex,
        req.oneShotName.empty() ? req.dailyName.c_str() : req.oneShotName.c_str());
    const auto beforeDaily = taskList_.Find(req.kind, req.dailyInstance);
    const auto beforeOneShot = taskList_.Find(req.kind, req.oneShotInstance);

    {
        TaskListManager::TaskEntry t{};
        t.name = req.dailyName.empty() ? L"EPG Full Update (Daily)" : req.dailyName;
        t.display.label = req.dailyLabel.empty() ? L"EPG取得予約" : req.dailyLabel;
        t.display.category = L"EPG";
        t.display.target = req.target.empty() ? L"EPG対象サービス" : req.target;
        t.display.note = req.dailyNote.empty() ? L"毎日定時実行" : req.dailyNote;
        t.display.endYmd = 0;
        t.display.endHHMM = 0;
        t.enabled = req.enabledDaily;

        t.request.kind = req.kind;
        t.request.instance = req.dailyInstance;
        t.request.priority = TimeKeeper::TaskPriority::Soft;
        t.request.requiredTuners = requiredTuners;
        t.request.preferredTunerIndex = preferredTunerIndex;

        t.schedule.type = TaskListManager::ScheduleType::Daily;
        t.schedule.dailyHour = hour;
        t.schedule.dailyMinute = minute;
        t.schedule.hourlyAtMinute = 0;
        t.schedule.intervalMinutes = 0;
        t.schedule.dueYmd = 0;
        t.schedule.dueHHMM = 0;
        t.schedule.lateToleranceMinutes = -1;
        t.schedule.useTimeWindow = false;
        t.schedule.windowStartYmd = 0;
        t.schedule.windowStartHHMM = 0;
        t.schedule.windowEndYmd = 0;
        t.schedule.windowEndHHMM = 0;

        ResetExecutionMonitorForTask_(t.request.kind, t.request.instance);
        taskList_.UpsertTask(t);
    }

    {
        TaskListManager::TaskEntry t{};
        t.name = req.oneShotName.empty() ? L"EPG Full OneShot (From UI)" : req.oneShotName;
        t.display.label = req.oneShotLabel.empty() ? L"EPG取得予約(次回)" : req.oneShotLabel;
        t.display.category = L"EPG";
        t.display.target = req.target.empty() ? L"EPG対象サービス" : req.target;
        t.display.note = BuildEpgWindowNote_(windowMinutes);
        t.display.endYmd = oneShotEndYmd;
        t.display.endHHMM = oneShotEndHHMM;
        t.enabled = req.enabledOneShot;

        t.request.kind = req.kind;
        t.request.instance = req.oneShotInstance;
        t.request.priority = TimeKeeper::TaskPriority::Soft;
        t.request.requiredTuners = requiredTuners;
        t.request.preferredTunerIndex = preferredTunerIndex;
        SetDispatchTime_(t.request, oneShotRunYmd, oneShotRunHHMM, oneShotRunSec);

        t.schedule.type = TaskListManager::ScheduleType::OneShot;
        t.schedule.dailyHour = 0;
        t.schedule.dailyMinute = 0;
        t.schedule.hourlyAtMinute = 0;
        t.schedule.intervalMinutes = 0;
        t.schedule.dueYmd = oneShotRunYmd;
        t.schedule.dueHHMM = oneShotRunHHMM;
        t.schedule.lateToleranceMinutes = windowMinutes;
        t.schedule.useTimeWindow = true;
        t.schedule.windowStartYmd = oneShotRunYmd;
        t.schedule.windowStartHHMM = oneShotRunHHMM;
        t.schedule.windowEndYmd = oneShotEndYmd;
        t.schedule.windowEndHHMM = oneShotEndHHMM;

        ResetExecutionMonitorForTask_(t.request.kind, t.request.instance);
        taskList_.UpsertTask(t);
    }

    const bool ok = ApplyTaskList();
    if (!ok) {
        SystemInfoLogError(
            L"EPG取得予約の登録に失敗しました。kind=%u dailyInst=%u oneShotInst=%u",
            static_cast<unsigned>(req.kind),
            static_cast<unsigned>(req.dailyInstance),
            static_cast<unsigned>(req.oneShotInstance));
        return false;
    }

    if (!SaveTasksToConfiguredIni_()) {
        SystemInfoLogError(
            L"EPG取得予約の保存に失敗しました。kind=%u dailyInst=%u oneShotInst=%u",
            static_cast<unsigned>(req.kind),
            static_cast<unsigned>(req.dailyInstance),
            static_cast<unsigned>(req.oneShotInstance));
        return false;
    }

    const auto afterDaily = taskList_.Find(req.kind, req.dailyInstance);
    if (afterDaily) {
        if (beforeDaily && IsTaskNameChanged_(*beforeDaily, *afterDaily)) {
            WriteTaskRenamedLog_(*beforeDaily, *afterDaily);
        }
        WriteTaskRegisteredLog_(*afterDaily, beforeDaily.has_value());
    }

    const auto afterOneShot = taskList_.Find(req.kind, req.oneShotInstance);
    if (afterOneShot) {
        if (beforeOneShot && IsTaskNameChanged_(*beforeOneShot, *afterOneShot)) {
            WriteTaskRenamedLog_(*beforeOneShot, *afterOneShot);
        }
        WriteTaskRegisteredLog_(*afterOneShot, beforeOneShot.has_value());
    }

    return true;
}

bool TaskSystemConnector::ScheduleOneShotWindow(std::uint32_t kind, std::uint32_t instance,
    int windowStartYmd, int windowStartHHMM,
    int windowEndYmd, int windowEndHHMM,
    bool enabled,
    const std::wstring& name,
    int lateToleranceMinutes)
{
    if (!initialized_) return false;

    TaskListManager::TaskEntry t{};
    t.name = name.empty() ? L"Task OneShot Window" : name;
    t.enabled = enabled;

    t.request.kind = kind;
    t.request.instance = instance;
    t.request.priority = TimeKeeper::TaskPriority::Soft;
    t.request.requiredTuners = (opt_.epgRequiredTuners == 0 ? 1 : opt_.epgRequiredTuners);
    t.request.preferredTunerIndex = -1;

    t.schedule.type = TaskListManager::ScheduleType::OneShot;
    t.schedule.dailyHour = 0;
    t.schedule.dailyMinute = 0;
    t.schedule.hourlyAtMinute = 0;
    t.schedule.intervalMinutes = 0;
    t.schedule.dueYmd = windowStartYmd;
    t.schedule.dueHHMM = windowStartHHMM;
    t.schedule.lateToleranceMinutes = lateToleranceMinutes;

    t.display.endYmd = windowEndYmd;
    t.display.endHHMM = windowEndHHMM;

    wchar_t note[128]{};
    swprintf_s(note,
        L"開始 %08d %02d:%02d / 終了 %08d %02d:%02d",
        windowStartYmd,
        windowStartHHMM / 100, windowStartHHMM % 100,
        windowEndYmd,
        windowEndHHMM / 100, windowEndHHMM % 100);
    t.display.note = note;

    return UpsertTaskAndApply(t);
}

bool TaskSystemConnector::ScheduleProgramReservation(const ProgramReservationRequest& req)
{
    if (!initialized_) return false;
    if (req.instance == 0) return false;
    if (req.startYmd <= 0 || req.endYmd <= 0) return false;

    const bool groupAlreadyExists =
        taskList_.Find(TaskKinds::RecPrepare, req.instance).has_value() ||
        taskList_.Find(TaskKinds::RecStart, req.instance).has_value() ||
        taskList_.Find(TaskKinds::RecStop, req.instance).has_value();

    if (!groupAlreadyExists) {
        ResetExecutionMonitorForTask_(TaskKinds::RecPrepare, req.instance);
        ResetExecutionMonitorForTask_(TaskKinds::RecStart, req.instance);
        ResetExecutionMonitorForTask_(TaskKinds::RecStop, req.instance);
    }



    const auto enabledTuners = BuildEnabledTunerIndices_(
        opt_.tunerCount,
        ReservationTunerPurpose_::Record);
    const std::uint8_t requiredTuners = ClampRequiredTuners_(req.requiredTuners, enabledTuners);

    const int requestedPreferredTunerIndex =
        ResolveRequestedPreferredTunerIndex_(
            req.preferredTunerIndex,
            enabledTuners);

    const int conflictPolicy = ReadReservationConflictPolicyFromConfig_();
    const int startAdvanceSec = ReadReservationStartAdvanceSecFromConfig_();
    const int startAdjustSec = ReadReservationStartAdjustSecFromConfig_();
    const int endAdjustMin = ReadReservationEndAdjustMinFromConfig_();

    int prepareExactYmd = req.startYmd;
    int prepareExactHHMM = req.startHHMM;
    int prepareExactSec = 0;
    (void)ShiftLocalYmdHHMMSS_(
        req.startYmd,
        req.startHHMM,
        0,
        -(req.prepareMinutes * 60 + startAdvanceSec),
        prepareExactYmd,
        prepareExactHHMM,
        prepareExactSec);

    int startExactYmd = req.startYmd;
    int startExactHHMM = req.startHHMM;
    int startExactSec = 0;
    (void)ShiftLocalYmdHHMMSS_(
        req.startYmd,
        req.startHHMM,
        0,
        startAdjustSec,
        startExactYmd,
        startExactHHMM,
        startExactSec);

    int stopExactYmd = req.endYmd;
    int stopExactHHMM = req.endHHMM;
    int stopExactSec = 0;
    (void)ShiftLocalYmdHHMMSS_(
        req.endYmd,
        req.endHHMM,
        0,
        (req.stopMarginMinutes + endAdjustMin) * 60,
        stopExactYmd,
        stopExactHHMM,
        stopExactSec);

    constexpr int kRecStopLateToleranceMinutes = 3;

    if (RejectExpiredProgramReservationRegistration_(
        req.title,
        startExactYmd,
        startExactHHMM,
        stopExactYmd,
        stopExactHHMM,
        kRecStopLateToleranceMinutes)) {
        return false;
    }
    const auto snapshot =
        BuildSnapshotWithoutProgramReservationGroup_(
            taskList_,
            req.instance);

    const auto candidateWindow =
        MakeWindow_(startExactYmd, startExactHHMM, stopExactYmd, stopExactHHMM);

    int preferredTunerIndex = -1;
    if (!ResolvePreferredTunerIndex_(
        enabledTuners,
        snapshot,
        requestedPreferredTunerIndex,
        conflictPolicy,
        candidateWindow,
        preferredTunerIndex)) {
        SystemInfoLogWarn(
            L"番組予約の登録に失敗しました。空きチューナーがありません。name=%ls instance=%u requestedTuner=%d",
            req.title.c_str(),
            static_cast<unsigned>(req.instance),
            requestedPreferredTunerIndex);
        return false;
    }

    LogTunerAutoReroute_(
        L"番組予約",
        requestedPreferredTunerIndex,
        preferredTunerIndex,
        req.title.c_str());

    auto makeEntry =
        [&](std::uint32_t kind,
            int dueYmd,
            int dueHHMM,
            int dueSecond,
            int endYmd,
            int endHHMM,
            TimeKeeper::TaskPriority prio,
            const wchar_t* suffix,
            int lateToleranceMinutes) -> TaskListManager::TaskEntry
        {
            TaskListManager::TaskEntry t{};
            t.name = req.title + suffix;
            t.display.label = req.title;
            t.display.category = L"RECORD";
            t.display.target = req.serviceName;
            t.display.endYmd = endYmd;
            t.display.endHHMM = endHHMM;
            t.enabled = req.enabled;

            wchar_t note[128]{};
            swprintf_s(note,
                L"開始 %08d %02d:%02d / 終了 %08d %02d:%02d",
                req.startYmd,
                req.startHHMM / 100, req.startHHMM % 100,
                req.endYmd,
                req.endHHMM / 100, req.endHHMM % 100);
            t.display.note = note;

            t.request.kind = kind;
            t.request.instance = req.instance;
            t.request.priority = prio;
            t.request.requiredTuners = requiredTuners;
            t.request.preferredTunerIndex = preferredTunerIndex;
            SetDispatchTime_(t.request, dueYmd, dueHHMM, dueSecond);

            t.schedule.type = TaskListManager::ScheduleType::OneShot;
            t.schedule.dailyHour = 0;
            t.schedule.dailyMinute = 0;
            t.schedule.hourlyAtMinute = 0;
            t.schedule.intervalMinutes = 0;
            t.schedule.dueYmd = dueYmd;
            t.schedule.dueHHMM = dueHHMM;
            t.schedule.lateToleranceMinutes = lateToleranceMinutes;
            return t;
        };

    taskList_.UpsertTask(makeEntry(
        TaskKinds::RecPrepare,
        prepareExactYmd,
        prepareExactHHMM,
        prepareExactSec,
        req.startYmd,
        req.startHHMM,
        TimeKeeper::TaskPriority::Soft,
        L" / Prepare",
        1));

    taskList_.UpsertTask(makeEntry(
        TaskKinds::RecStart,
        startExactYmd,
        startExactHHMM,
        startExactSec,
        req.endYmd,
        req.endHHMM,
        TimeKeeper::TaskPriority::Hard,
        L" / Start",
        1));

    taskList_.UpsertTask(makeEntry(
        TaskKinds::RecStop,
        stopExactYmd,
        stopExactHHMM,
        stopExactSec,
        stopExactYmd,
        stopExactHHMM,
        TimeKeeper::TaskPriority::Hard,
        L" / Stop",
        3));

    const bool ok = ApplyTaskList();

    if (!ok) {
        SystemInfoLogError(
            L"番組予約の登録に失敗しました。name=%ls instance=%u",
            req.title.c_str(),
            static_cast<unsigned>(req.instance));
        return false;
    }

    if (!SaveTasksToConfiguredIni_()) {
        SystemInfoLogError(
            L"番組予約の保存に失敗しました。name=%ls instance=%u",
            req.title.c_str(),
            static_cast<unsigned>(req.instance));
        return false;
    }

    SystemInfoLogInfo(
        L"番組予約を登録しました。name=%ls target=%ls start=%08d %02d:%02d end=%08d %02d:%02d instance=%u reqTuners=%u tunerIndex=%d",
        req.title.c_str(),
        req.serviceName.c_str(),
        req.startYmd, req.startHHMM / 100, req.startHHMM % 100,
        req.endYmd, req.endHHMM / 100, req.endHHMM % 100,
        static_cast<unsigned>(req.instance),
        static_cast<unsigned>(requiredTuners),
        preferredTunerIndex);

    return true;
}

bool TaskSystemConnector::ScheduleProgramReservation(const EpgReserveCommandEx& cmd)
{
    if (!initialized_) {
        return false;
    }

    EpgReserveCommandEx fixedCmd = cmd;

    // startDate/startTime は EPG から渡された番組の絶対開始日時。
    // 00:00〜03:59 であっても dateText（番組表の表示日）から日付を
    // 翌日へ変更しない。準備開始だけを BuildStageTimingPlan() で
    // 絶対開始日時から減算し、日付をまたぐ場合は前日へ送る。

    // Lite版の自動予約検索データでは NetworkId/TsId が 0 のまま来る場合がある。
    // serviceId を基準に channellist から補完し、録画実行時のチャンネル指定を安定させる。
    (void)CompleteReservationChannelIdentityFromChannelList_(
        fixedCmd);

    const int startAdvanceSec =
        ReadReservationStartAdvanceSecFromConfig_();

    const int startAdjustSec =
        ReadReservationStartAdjustSecFromConfig_();

    const int endAdjustMin =
        ReadReservationEndAdjustMinFromConfig_();

    const auto timePlan =
        Reservation_ProgramTimeUtil::BuildProgramTimePlan(
            fixedCmd.startDate,
            fixedCmd.startTime,
            fixedCmd.endDate,
            fixedCmd.endTime,
            fixedCmd.dateText,
            fixedCmd.timeText,
            fixedCmd.durationSec,
            kBroadcastDayStartHour_);

    if (!Reservation_ProgramTimeUtil::IsValidProgramTimePlan(
        timePlan)) {
        SystemInfoLogError(
            L"番組予約の登録に失敗しました。日時解析に失敗しました。title=%ls rawStart=%ls %ls rawEnd=%ls %ls display=%ls %ls",
            fixedCmd.title.c_str(),
            fixedCmd.startDate.c_str(),
            fixedCmd.startTime.c_str(),
            fixedCmd.endDate.c_str(),
            fixedCmd.endTime.c_str(),
            fixedCmd.dateText.c_str(),
            fixedCmd.timeText.c_str());

        return false;
    }

    const auto timingPlan =
        Reservation_ProgramTimeUtil::BuildStageTimingPlan(
            timePlan,
            startAdvanceSec,
            startAdjustSec,
            endAdjustMin * 60);

    if (!timingPlan.valid) {
        SystemInfoLogError(
            L"番組予約の登録に失敗しました。準備/開始/終了時刻の計算に失敗しました。title=%ls start=%ls %ls end=%ls %ls",
            fixedCmd.title.c_str(),
            timePlan.startDate.c_str(),
            timePlan.startTime.c_str(),
            timePlan.endDate.c_str(),
            timePlan.endTime.c_str());

        return false;
    }

    DebugLogFormat(
        L"[TaskSystemConnector][RESV-TIME] title=%ls rawStart=%ls %ls rawEnd=%ls %ls display=%ls %ls normalized=%ls %ls-%ls %ls prepare=%08d %04d:%02d start=%08d %04d:%02d stop=%08d %04d:%02d usedDisplayStart=%d usedDurationEnd=%d usedDisplayEnd=%d adjustedBroadcast=%d",
        fixedCmd.title.c_str(),
        fixedCmd.startDate.c_str(),
        fixedCmd.startTime.c_str(),
        fixedCmd.endDate.c_str(),
        fixedCmd.endTime.c_str(),
        fixedCmd.dateText.c_str(),
        fixedCmd.timeText.c_str(),
        timePlan.startDate.c_str(),
        timePlan.startTime.c_str(),
        timePlan.endDate.c_str(),
        timePlan.endTime.c_str(),
        timingPlan.prepareYmd,
        timingPlan.prepareHHMM,
        timingPlan.prepareSecond,
        timingPlan.startYmd,
        timingPlan.startHHMM,
        timingPlan.startSecond,
        timingPlan.stopYmd,
        timingPlan.stopHHMM,
        timingPlan.stopSecond,
        timePlan.usedDisplayStart ? 1 : 0,
        timePlan.usedDurationEnd ? 1 : 0,
        timePlan.usedDisplayEnd ? 1 : 0,
        timePlan.startAdjustedByBroadcastDay ? 1 : 0);

    constexpr int kRecStopLateToleranceMinutes = 3;

    if (RejectExpiredProgramReservationRegistration_(
        fixedCmd.title,
        timingPlan.startYmd,
        timingPlan.startHHMM,
        timingPlan.stopYmd,
        timingPlan.stopHHMM,
        kRecStopLateToleranceMinutes)) {
        return false;
    }

    if (timePlan.startAdjustedByBroadcastDay ||
        timePlan.usedDisplayStart ||
        timePlan.usedDurationEnd ||
        timePlan.usedDisplayEnd) {
        SystemInfoLogInfo(
            L"番組予約の日時を正規化しました。title=%ls display=%ls %ls start=%ls %ls end=%ls %ls",
            fixedCmd.title.c_str(),
            fixedCmd.dateText.c_str(),
            fixedCmd.timeText.c_str(),
            timePlan.startDate.c_str(),
            timePlan.startTime.c_str(),
            timePlan.endDate.c_str(),
            timePlan.endTime.c_str());
    }

    // raw は受信時そのままのキー。
    // fixed 側で NID/TSID 補完後、canonical key を作る。
    // これにより旧 0-0-0 キーの予約グループは削除し、新しい正規IDキーへ移行できる。
    const std::wstring rawDedupeKey =
        BuildReserveDedupeKey_(
            cmd);

    const std::wstring dedupeKey =
        BuildReserveDedupeKeyCanonical_(
            fixedCmd,
            timePlan);

    const std::uint32_t rawInstance =
        HashFNV1a32_(
            rawDedupeKey);

    const std::uint32_t instance =
        HashFNV1a32_(
            dedupeKey);

    if (rawInstance != 0 &&
        rawInstance != instance) {
        const bool oldPrepare =
            taskList_.RemoveTask(
                TaskKinds::RecPrepare,
                rawInstance);

        const bool oldStart =
            taskList_.RemoveTask(
                TaskKinds::RecStart,
                rawInstance);

        const bool oldStop =
            taskList_.RemoveTask(
                TaskKinds::RecStop,
                rawInstance);

        ResetExecutionMonitorForTask_(
            TaskKinds::RecPrepare,
            rawInstance);

        ResetExecutionMonitorForTask_(
            TaskKinds::RecStart,
            rawInstance);

        ResetExecutionMonitorForTask_(
            TaskKinds::RecStop,
            rawInstance);

        if (oldPrepare || oldStart || oldStop) {
            DebugLogFormat(
                L"[TaskSystemConnector][RESV-TIME] removed legacy raw reservation group rawInst=%u canonicalInst=%u title=%ls rawKey=%ls canonicalKey=%ls prepare=%d start=%d stop=%d",
                static_cast<unsigned>(rawInstance),
                static_cast<unsigned>(instance),
                fixedCmd.title.c_str(),
                rawDedupeKey.c_str(),
                dedupeKey.c_str(),
                oldPrepare ? 1 : 0,
                oldStart ? 1 : 0,
                oldStop ? 1 : 0);
        }
    }

    const bool groupAlreadyExists =
        taskList_.Find(
            TaskKinds::RecPrepare,
            instance).has_value() ||
        taskList_.Find(
            TaskKinds::RecStart,
            instance).has_value() ||
        taskList_.Find(
            TaskKinds::RecStop,
            instance).has_value();

    if (!groupAlreadyExists) {
        ResetExecutionMonitorForTask_(
            TaskKinds::RecPrepare,
            instance);

        ResetExecutionMonitorForTask_(
            TaskKinds::RecStart,
            instance);

        ResetExecutionMonitorForTask_(
            TaskKinds::RecStop,
            instance);
    }

    const std::wstring reserveMode =
        fixedCmd.reserveMode.empty()
        ? L"record"
        : fixedCmd.reserveMode;

    const std::wstring reserveSource =
        fixedCmd.reserveSource.empty()
        ? L"epg_portal"
        : fixedCmd.reserveSource;

    const std::wstring serviceNameForReservation =
        BuildCanonicalReservationServiceName_(
            fixedCmd);

    const auto enabledTuners =
        BuildEnabledTunerIndices_(
            opt_.tunerCount,
            ReservationTunerPurpose_::Record);

    const std::uint8_t requiredTuners =
        ClampRequiredTuners_(
            fixedCmd.requiredTuners,
            enabledTuners);

    const int requestedPreferredTunerIndex =
        ResolveRequestedPreferredTunerIndex_(
            fixedCmd.preferredTunerIndex,
            enabledTuners);

    const int conflictPolicy =
        ReadReservationConflictPolicyFromConfig_();

    const auto snapshot =
        BuildSnapshotWithoutProgramReservationGroup_(
            taskList_,
            instance);

    const auto candidateWindow =
        MakeWindow_(
            timingPlan.startYmd,
            timingPlan.startHHMM,
            timingPlan.stopYmd,
            timingPlan.stopHHMM);

    int preferredTunerIndex = -1;

    if (!ResolvePreferredTunerIndex_(
        enabledTuners,
        snapshot,
        requestedPreferredTunerIndex,
        conflictPolicy,
        candidateWindow,
        preferredTunerIndex)) {
        SystemInfoLogWarn(
            L"番組予約の登録に失敗しました。空きチューナーがありません。title=%ls requestedTuner=%d start=%ls %ls end=%ls %ls",
            fixedCmd.title.c_str(),
            requestedPreferredTunerIndex,
            timePlan.startDate.c_str(),
            timePlan.startTime.c_str(),
            timePlan.endDate.c_str(),
            timePlan.endTime.c_str());

        return false;
    }

    LogTunerAutoReroute_(
        L"番組予約",
        requestedPreferredTunerIndex,
        preferredTunerIndex,
        fixedCmd.title.c_str());

    auto makeEntry =
        [&](std::uint32_t kind,
            int dueYmd,
            int dueHHMM,
            int dueSecond,
            int displayEndYmd,
            int displayEndHHMM,
            TimeKeeper::TaskPriority prio,
            const wchar_t* suffix,
            int lateToleranceMinutes) -> TaskListManager::TaskEntry
        {
            TaskListManager::TaskEntry t{};

            const std::wstring title =
                fixedCmd.title.empty()
                ? L"番組予約"
                : fixedCmd.title;

            t.name = title + suffix;
            t.enabled = fixedCmd.enabled;

            t.display.label =
                !fixedCmd.titleLabel.empty()
                ? fixedCmd.titleLabel
                : title;

            t.display.category =
                (reserveMode == L"watch")
                ? L"WATCH"
                : L"RECORD";

            t.display.target = serviceNameForReservation;
            t.display.endYmd = displayEndYmd;
            t.display.endHHMM = displayEndHHMM;

            wchar_t note[192]{};
            swprintf_s(
                note,
                L"開始 %ls %ls / 終了 %ls %ls / 表示 %ls %ls",
                timePlan.startDate.c_str(),
                timePlan.startTime.c_str(),
                timePlan.endDate.c_str(),
                timePlan.endTime.c_str(),
                fixedCmd.dateText.c_str(),
                fixedCmd.timeText.c_str());

            t.display.note = note;

            t.reserve.space = fixedCmd.space;
            t.reserve.networkId = fixedCmd.networkId;
            t.reserve.tsId = fixedCmd.tsId;
            t.reserve.serviceId = fixedCmd.serviceId;
            t.reserve.eventId = fixedCmd.eventId;

            t.reserve.title = fixedCmd.title;
            t.reserve.titleLabel = fixedCmd.titleLabel;
            t.reserve.serviceName = serviceNameForReservation;
            t.reserve.summary = fixedCmd.summary;
            t.reserve.genre = fixedCmd.genre;

            t.reserve.dateText = fixedCmd.dateText;
            t.reserve.weekdayText = fixedCmd.weekdayText;
            t.reserve.timeText = fixedCmd.timeText;

            // 実行・表示・キャッシュのズレを防ぐため、ここは必ず正規化済み日時を保存する。
            t.reserve.startDate = timePlan.startDate;
            t.reserve.startTime = timePlan.startTime;
            t.reserve.endDate = timePlan.endDate;
            t.reserve.endTime = timePlan.endTime;

            t.reserve.durationSec =
                timePlan.durationSec > 0
                ? timePlan.durationSec
                : fixedCmd.durationSec;

            t.reserve.reserveMode = reserveMode;
            t.reserve.reserveSource = reserveSource;
            t.reserve.dedupeKey = dedupeKey;

            t.reserve.disabledByUser = fixedCmd.disabledByUser;
            t.reserve.excludeFromAuto = fixedCmd.excludeFromAuto;
            t.reserve.autoReserved = fixedCmd.autoReserved;

            t.reserve.excludeKey =
                fixedCmd.excludeKey.empty()
                ? dedupeKey
                : fixedCmd.excludeKey;

            t.request.kind = kind;
            t.request.instance = instance;
            t.request.priority = prio;
            t.request.requiredTuners = requiredTuners;
            t.request.preferredTunerIndex = preferredTunerIndex;

            SetDispatchTime_(
                t.request,
                dueYmd,
                dueHHMM,
                dueSecond);

            t.schedule.type = TaskListManager::ScheduleType::OneShot;
            t.schedule.dailyHour = 0;
            t.schedule.dailyMinute = 0;
            t.schedule.hourlyAtMinute = 0;
            t.schedule.intervalMinutes = 0;
            t.schedule.dueYmd = dueYmd;
            t.schedule.dueHHMM = dueHHMM;
            t.schedule.lateToleranceMinutes = lateToleranceMinutes;
            t.schedule.useTimeWindow = false;
            t.schedule.windowStartYmd = 0;
            t.schedule.windowStartHHMM = 0;
            t.schedule.windowEndYmd = 0;
            t.schedule.windowEndHHMM = 0;

            return t;
        };

    taskList_.UpsertTask(
        makeEntry(
            TaskKinds::RecPrepare,
            timingPlan.prepareYmd,
            timingPlan.prepareHHMM,
            timingPlan.prepareSecond,
            timePlan.startYmd,
            timePlan.startHHMM,
            TimeKeeper::TaskPriority::Soft,
            L" / Prepare",
            1));

    taskList_.UpsertTask(
        makeEntry(
            TaskKinds::RecStart,
            timingPlan.startYmd,
            timingPlan.startHHMM,
            timingPlan.startSecond,
            timePlan.endYmd,
            timePlan.endHHMM,
            TimeKeeper::TaskPriority::Hard,
            L" / Start",
            1));

    taskList_.UpsertTask(
        makeEntry(
            TaskKinds::RecStop,
            timingPlan.stopYmd,
            timingPlan.stopHHMM,
            timingPlan.stopSecond,
            timingPlan.stopYmd,
            timingPlan.stopHHMM,
            TimeKeeper::TaskPriority::Hard,
            L" / Stop",
            3));

    const bool ok =
        ApplyTaskList();

    if (!ok) {
        SystemInfoLogError(
            L"番組予約の登録に失敗しました。title=%ls service=%ls dedupe=%ls",
            fixedCmd.title.c_str(),
            serviceNameForReservation.c_str(),
            dedupeKey.c_str());

        return false;
    }

    if (!SaveTasksToConfiguredIni_()) {
        SystemInfoLogError(
            L"番組予約の保存に失敗しました。title=%ls service=%ls dedupe=%ls",
            fixedCmd.title.c_str(),
            serviceNameForReservation.c_str(),
            dedupeKey.c_str());

        return false;
    }

    SystemInfoLogInfo(
        L"番組予約を登録しました。title=%ls service=%ls start=%ls %ls end=%ls %ls mode=%ls source=%ls dedupe=%ls reqTuners=%u tunerIndex=%d",
        fixedCmd.title.c_str(),
        serviceNameForReservation.c_str(),
        timePlan.startDate.c_str(),
        timePlan.startTime.c_str(),
        timePlan.endDate.c_str(),
        timePlan.endTime.c_str(),
        reserveMode.c_str(),
        reserveSource.c_str(),
        dedupeKey.c_str(),
        static_cast<unsigned>(requiredTuners),
        preferredTunerIndex);

    return true;
}

namespace
{
    enum class AutoReserveExistingState_
    {
        None,
        Duplicate,
        UserBlocked
    };

    static bool IsAutoReserveSearchProgramUsable_(
        const AutoReserve::SearchProgram& p)
    {
        if (p.title.empty()) {
            return false;
        }

        if (!p.startDate.empty() &&
            !p.startTime.empty()) {
            return true;
        }

        if (!p.dateText.empty() &&
            !p.timeText.empty()) {
            return true;
        }

        return false;
    }

    static std::wstring AutoReservePickDateText_(
        const AutoReserve::SearchProgram& p)
    {
        if (!p.startDate.empty()) {
            return p.startDate;
        }

        return p.dateText;
    }

    static std::wstring AutoReservePickTimeText_(
        const AutoReserve::SearchProgram& p)
    {
        if (!p.startTime.empty()) {
            return p.startTime;
        }

        return p.timeText;
    }

    static std::wstring BuildAutoReserveStableProgramKey_(
        const AutoReserve::RuleProgramMatch& m)
    {
        const AutoReserve::SearchProgram& p = m.program;

        std::wstring key;
        key.reserve(256 + p.title.size() + p.serviceName.size());

        key += L"auto-reserve|space=";
        key += std::to_wstring(p.space);

        key += L"|nid=";
        key += std::to_wstring(static_cast<unsigned>(p.networkId));

        key += L"|tsid=";
        key += std::to_wstring(static_cast<unsigned>(p.tsId));

        key += L"|sid=";
        key += std::to_wstring(static_cast<unsigned>(p.serviceId));

        key += L"|eid=";
        key += std::to_wstring(static_cast<unsigned>(p.eventId));

        key += L"|date=";
        key += AutoReservePickDateText_(p);

        key += L"|time=";
        key += AutoReservePickTimeText_(p);

        key += L"|service=";
        key += p.serviceName;

        key += L"|title=";
        key += p.title;

        return key;
    }

    static std::wstring NormalizeAutoReserveHardDedupeText_(
        const std::wstring& text)
    {
        std::wstring out;
        out.reserve(text.size());

        bool prevSpace = false;

        for (const wchar_t ch : text) {
            const bool isSpace =
                ch == L' ' ||
                ch == L'\t' ||
                ch == L'\r' ||
                ch == L'\n' ||
                ch == L'　';

            if (isSpace) {
                if (!prevSpace && !out.empty()) {
                    out.push_back(L' ');
                }

                prevSpace = true;
                continue;
            }

            prevSpace = false;
            out.push_back(ch);
        }

        while (!out.empty() && out.back() == L' ') {
            out.pop_back();
        }

        return out;
    }

    static std::wstring BuildAutoReserveSameTransportProgramKey_(
        const AutoReserve::RuleProgramMatch& m)
    {
        const AutoReserve::SearchProgram& p = m.program;

        const std::wstring dateText =
            AutoReservePickDateText_(p);

        const std::wstring timeText =
            AutoReservePickTimeText_(p);

        // NID/TSID/時刻/タイトルが取れない場合は、
        // 誤爆防止のため従来の service 単位キーへ戻す。
        if (p.networkId == 0 ||
            p.tsId == 0 ||
            p.title.empty() ||
            dateText.empty() ||
            timeText.empty()) {
            return BuildAutoReserveStableProgramKey_(m);
        }

        std::wstring key;
        key.reserve(256 + p.title.size());

        key += L"auto-reserve-ts|space=";
        key += std::to_wstring(p.space);

        key += L"|nid=";
        key += std::to_wstring(
            static_cast<unsigned>(p.networkId));

        key += L"|tsid=";
        key += std::to_wstring(
            static_cast<unsigned>(p.tsId));

        key += L"|date=";
        key += dateText;

        key += L"|time=";
        key += timeText;

        key += L"|duration=";
        key += std::to_wstring(
            static_cast<unsigned>(p.durationSec));

        key += L"|title=";
        key += NormalizeAutoReserveHardDedupeText_(p.title);

        return key;
    }

    struct AutoReserveMainSubDedupeProbe_
    {
        int space = 0;

        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;
        std::uint16_t serviceId = 0;
        std::uint16_t eventId = 0;

        std::uint32_t durationSec = 0;

        std::wstring title;
        std::wstring serviceName;
        std::wstring serviceFamily;

        std::wstring dateText;
        std::wstring startTime;
        std::wstring endDate;
        std::wstring endTime;
    };

    static bool AutoReserveIsAsciiOrFullWidthDigit_(
        wchar_t ch) noexcept
    {
        return
            (ch >= L'0' && ch <= L'9') ||
            (ch >= L'０' && ch <= L'９');
    }

    static bool AutoReserveIsSidOnlyServiceName_(
        const std::wstring& serviceName)
    {
        const std::wstring text =
            NormalizeAutoReserveHardDedupeText_(
                serviceName);

        if (text.size() < 3) {
            return false;
        }

        const bool hasSidPrefix =
            (text[0] == L'S' || text[0] == L's') &&
            (text[1] == L'I' || text[1] == L'i') &&
            (text[2] == L'D' || text[2] == L'd');

        if (!hasSidPrefix) {
            return false;
        }

        for (std::size_t i = 3; i < text.size(); ++i) {
            const wchar_t ch = text[i];

            if (ch == L' ' ||
                ch == L'\t' ||
                ch == L'　' ||
                AutoReserveIsAsciiOrFullWidthDigit_(ch)) {
                continue;
            }

            return false;
        }

        return true;
    }

    static std::wstring BuildAutoReserveSidBlockFamily_(
        std::uint16_t serviceId)
    {
        if (serviceId == 0) {
            return {};
        }

        // 1024/1025 のような隣接メイン/サブを同じ束に入れるため、
        // SIDを16単位のブロックへ丸める。
        return L"SIDBLOCK" +
            std::to_wstring(
                static_cast<unsigned>(serviceId / 16));
    }

    static std::wstring BuildAutoReserveMainSubServiceFamily_(
        const std::wstring& serviceName,
        std::uint16_t serviceId)
    {
        const std::wstring normalized =
            NormalizeAutoReserveHardDedupeText_(
                serviceName);

        if (AutoReserveIsSidOnlyServiceName_(normalized)) {
            return BuildAutoReserveSidBlockFamily_(
                serviceId);
        }

        std::wstring out;
        out.reserve(normalized.size());

        for (const wchar_t ch : normalized) {
            // NHK総合1・東京 / NHK総合2・東京
            // TOKYO MX1 / TOKYO MX2
            // のようなサービス番号だけを落として同じ family にする。
            if (AutoReserveIsAsciiOrFullWidthDigit_(ch)) {
                continue;
            }

            out.push_back(ch);
        }

        out =
            NormalizeAutoReserveHardDedupeText_(
                out);

        if (!out.empty()) {
            return out;
        }

        return BuildAutoReserveSidBlockFamily_(
            serviceId);
    }

    static AutoReserveMainSubDedupeProbe_
        MakeAutoReserveMainSubDedupeProbe_(
            const AutoReserve::SearchProgram& p)
    {
        AutoReserveMainSubDedupeProbe_ probe{};

        probe.space = p.space;

        probe.networkId = p.networkId;
        probe.tsId = p.tsId;
        probe.serviceId = p.serviceId;
        probe.eventId = p.eventId;

        probe.durationSec = p.durationSec;

        probe.title =
            NormalizeAutoReserveHardDedupeText_(
                p.title);

        probe.serviceName =
            NormalizeAutoReserveHardDedupeText_(
                p.serviceName);

        probe.serviceFamily =
            BuildAutoReserveMainSubServiceFamily_(
                probe.serviceName,
                probe.serviceId);

        probe.dateText =
            NormalizeAutoReserveHardDedupeText_(
                AutoReservePickDateText_(p));

        probe.startTime =
            NormalizeAutoReserveHardDedupeText_(
                AutoReservePickTimeText_(p));

        probe.endDate =
            NormalizeAutoReserveHardDedupeText_(
                p.endDate);

        probe.endTime =
            NormalizeAutoReserveHardDedupeText_(
                p.endTime);

        return probe;
    }

    static AutoReserveMainSubDedupeProbe_
        MakeAutoReserveMainSubDedupeProbe_(
            const TaskListManager::TaskEntry& t)
    {
        AutoReserveMainSubDedupeProbe_ probe{};

        probe.space = t.reserve.space;

        probe.networkId = t.reserve.networkId;
        probe.tsId = t.reserve.tsId;
        probe.serviceId = t.reserve.serviceId;
        probe.eventId = t.reserve.eventId;

        probe.durationSec = t.reserve.durationSec;

        probe.title =
            NormalizeAutoReserveHardDedupeText_(
                t.reserve.title.empty()
                ? t.display.label
                : t.reserve.title);

        probe.serviceName =
            NormalizeAutoReserveHardDedupeText_(
                t.reserve.serviceName);

        probe.serviceFamily =
            BuildAutoReserveMainSubServiceFamily_(
                probe.serviceName,
                probe.serviceId);

        probe.dateText =
            NormalizeAutoReserveHardDedupeText_(
                t.reserve.startDate.empty()
                ? t.reserve.dateText
                : t.reserve.startDate);

        probe.startTime =
            NormalizeAutoReserveHardDedupeText_(
                t.reserve.startTime.empty()
                ? t.reserve.timeText
                : t.reserve.startTime);

        probe.endDate =
            NormalizeAutoReserveHardDedupeText_(
                t.reserve.endDate);

        probe.endTime =
            NormalizeAutoReserveHardDedupeText_(
                t.reserve.endTime);

        return probe;
    }

    static bool IsAutoReserveMainSubProbeUsable_(
        const AutoReserveMainSubDedupeProbe_& p) noexcept
    {
        if (p.title.empty()) {
            return false;
        }

        if (p.dateText.empty()) {
            return false;
        }

        if (p.startTime.empty()) {
            return false;
        }

        if (p.serviceFamily.empty()) {
            return false;
        }

        if (p.durationSec == 0 && p.endTime.empty()) {
            return false;
        }

        return true;
    }

    static bool ShouldUseAutoReserveMainSubFallbackKey_(
        const AutoReserveMainSubDedupeProbe_& p) noexcept
    {
        if (!IsAutoReserveMainSubProbeUsable_(p)) {
            return false;
        }

        // NID/TSIDが取れているなら既存の sameTransportProgramKey を優先する。
        // fallback は NID/TSID 欠落時専用。
        return p.networkId == 0 || p.tsId == 0;
    }

    static std::wstring BuildAutoReserveMainSubFallbackKey_(
        const AutoReserveMainSubDedupeProbe_& p)
    {
        if (!ShouldUseAutoReserveMainSubFallbackKey_(p)) {
            return {};
        }

        std::wstring key;
        key.reserve(256 + p.title.size() + p.serviceFamily.size());

        key += L"auto-reserve-main-sub";
        key += L"|space=";
        key += std::to_wstring(p.space);

        key += L"|family=";
        key += p.serviceFamily;

        key += L"|date=";
        key += p.dateText;

        key += L"|start=";
        key += p.startTime;

        if (p.durationSec != 0) {
            key += L"|duration=";
            key += std::to_wstring(
                static_cast<unsigned>(p.durationSec));
        }
        else {
            key += L"|end=";
            key += p.endTime;
        }

        key += L"|title=";
        key += p.title;

        return key;
    }

    static std::wstring BuildAutoReserveMainSubProgramKey_(
        const AutoReserve::RuleProgramMatch& m)
    {
        const AutoReserveMainSubDedupeProbe_ probe =
            MakeAutoReserveMainSubDedupeProbe_(
                m.program);

        const std::wstring fallbackKey =
            BuildAutoReserveMainSubFallbackKey_(
                probe);

        if (!fallbackKey.empty()) {
            return fallbackKey;
        }

        return BuildAutoReserveSameTransportProgramKey_(
            m);
    }

    static std::wstring BuildAutoReserveMainSubTaskKey_(
        const TaskListManager::TaskEntry& t)
    {
        if (!IsRecordTaskKind_(t.request.kind)) {
            return {};
        }

        if (!t.reserve.autoReserved) {
            return {};
        }

        if (t.reserve.reserveMode != L"record") {
            return {};
        }

        const AutoReserveMainSubDedupeProbe_ probe =
            MakeAutoReserveMainSubDedupeProbe_(
                t);

        return BuildAutoReserveMainSubFallbackKey_(
            probe);
    }

    static std::uint32_t AutoReserveMainSubRank_(
        const AutoReserveMainSubDedupeProbe_& p) noexcept
    {
        // 小さい serviceId をメイン扱い。
        // 例: NHK総合1=1024 / NHK総合2=1025
        if (p.serviceId == 0) {
            return 0xFFFFFFFFu;
        }

        return static_cast<std::uint32_t>(p.serviceId);
    }

    static std::uint32_t AutoReserveMainServiceRank_(
        const AutoReserve::RuleProgramMatch& m) noexcept
    {
        const AutoReserveMainSubDedupeProbe_ probe =
            MakeAutoReserveMainSubDedupeProbe_(
                m.program);

        const std::uint32_t mainSubRank =
            AutoReserveMainSubRank_(
                probe);

        if (mainSubRank != 0xFFFFFFFFu) {
            return mainSubRank;
        }

        const std::uint32_t sid =
            static_cast<std::uint32_t>(m.program.serviceId);

        if (sid == 0) {
            return 0xFFFFFFFFu;
        }

        return sid;
    }

    static bool AutoReserveMatchMainServiceLess_(
        const AutoReserve::RuleProgramMatch* a,
        const AutoReserve::RuleProgramMatch* b)
    {
        if (a == nullptr || b == nullptr) {
            return a != nullptr;
        }

        const std::wstring ak =
            BuildAutoReserveMainSubProgramKey_(*a);

        const std::wstring bk =
            BuildAutoReserveMainSubProgramKey_(*b);

        if (ak != bk) {
            return ak < bk;
        }

        const std::uint32_t ar =
            AutoReserveMainServiceRank_(*a);

        const std::uint32_t br =
            AutoReserveMainServiceRank_(*b);

        if (ar != br) {
            return ar < br;
        }

        if (a->program.serviceName != b->program.serviceName) {
            return a->program.serviceName < b->program.serviceName;
        }

        if (a->program.eventId != b->program.eventId) {
            return a->program.eventId < b->program.eventId;
        }

        return a->ruleId < b->ruleId;
    }

    static TaskSystemConnector::EpgReserveCommandEx
        MakeAutoReserveRecordingCommand_(
            const AutoReserve::RuleProgramMatch& m,
            const TaskSystemConnector::AutoReserveRecordingApplyOptions& options,
            const std::wstring& stableProgramKey)
    {
        const AutoReserve::SearchProgram& p = m.program;

        TaskSystemConnector::EpgReserveCommandEx cmd{};

        cmd.space = p.space;
        cmd.networkId = p.networkId;
        cmd.tsId = p.tsId;
        cmd.serviceId = p.serviceId;
        cmd.eventId = p.eventId;

        cmd.title = p.title;
        cmd.titleLabel = p.title;

        cmd.serviceName =
            p.serviceName.empty()
            ? L"(放送局不明)"
            : p.serviceName;

        cmd.summary =
            !p.summary.empty()
            ? p.summary
            : p.extendedText;

        cmd.genre = p.genreName;

        cmd.dateText = p.dateText;
        cmd.weekdayText = p.weekdayText;
        cmd.timeText = p.timeText;

        cmd.startDate = p.startDate;
        cmd.startTime = p.startTime;
        cmd.endDate = p.endDate;
        cmd.endTime = p.endTime;
        cmd.durationSec = p.durationSec;

        cmd.reserveMode =
            options.reserveMode.empty()
            ? L"record"
            : options.reserveMode;

        cmd.reserveSource =
            options.reserveSource.empty()
            ? L"auto_reserve"
            : options.reserveSource;

        // dedupeKey は空にする。
        // ScheduleProgramReservation() 側で正規化済み日時から
        // 既存の canonical dedupeKey を作らせる。
        // ここで ruleId を混ぜると、複数ルール一致時に重複予約になる。
        cmd.dedupeKey.clear();

        cmd.enabled = options.forceCreatedReservationEnabled;
        cmd.disabledByUser = false;
        cmd.excludeFromAuto = false;
        cmd.autoReserved = true;

        // ユーザーが後で自動予約除外にした時の安定キー。
        // ruleId は入れない。番組単位で除外できるようにする。
        cmd.excludeKey = stableProgramKey;

        cmd.requiredTuners =
            options.requiredTuners == 0
            ? 1
            : options.requiredTuners;

        cmd.preferredTunerIndex = options.preferredTunerIndex;

        return cmd;
    }

    static std::wstring BuildAutoReserveScheduleDedupeCandidate_(
        const TaskSystemConnector::EpgReserveCommandEx& cmd)
    {
        const auto timePlan =
            Reservation_ProgramTimeUtil::BuildProgramTimePlan(
                cmd.startDate,
                cmd.startTime,
                cmd.endDate,
                cmd.endTime,
                cmd.dateText,
                cmd.timeText,
                cmd.durationSec,
                kBroadcastDayStartHour_);

        if (Reservation_ProgramTimeUtil::IsValidProgramTimePlan(timePlan)) {
            return BuildReserveDedupeKeyCanonical_(cmd, timePlan);
        }

        return BuildReserveDedupeKey_(cmd);
    }

    static bool AutoReserveProgramIdsMatchTask_(
        const TaskListManager::TaskEntry& t,
        const AutoReserve::SearchProgram& p)
    {
        if (p.space >= 0 &&
            t.reserve.space >= 0 &&
            t.reserve.space != p.space) {
            return false;
        }

        if (p.networkId != 0 &&
            t.reserve.networkId != 0 &&
            t.reserve.networkId != p.networkId) {
            return false;
        }

        if (p.tsId != 0 &&
            t.reserve.tsId != 0 &&
            t.reserve.tsId != p.tsId) {
            return false;
        }

        if (p.serviceId != 0 &&
            t.reserve.serviceId != 0 &&
            t.reserve.serviceId != p.serviceId) {
            return false;
        }

        if (p.eventId != 0 &&
            t.reserve.eventId != 0 &&
            t.reserve.eventId == p.eventId) {
            return true;
        }

        return false;
    }

    static bool AutoReserveProgramTextTimeMatchesTask_(
        const TaskListManager::TaskEntry& t,
        const AutoReserve::SearchProgram& p)
    {
        if (p.title.empty()) {
            return false;
        }

        const std::wstring taskTitle =
            !t.reserve.title.empty()
            ? t.reserve.title
            : t.display.label;

        if (taskTitle != p.title) {
            return false;
        }

        if (!p.serviceName.empty() &&
            !t.reserve.serviceName.empty() &&
            t.reserve.serviceName != p.serviceName) {
            return false;
        }

        if (!p.startDate.empty() &&
            !t.reserve.startDate.empty() &&
            t.reserve.startDate != p.startDate) {
            return false;
        }

        if (!p.startTime.empty() &&
            !t.reserve.startTime.empty() &&
            t.reserve.startTime != p.startTime) {
            return false;
        }

        return true;
    }

    static bool AutoReserveProgramMatchesTask_(
        const TaskListManager::TaskEntry& t,
        const AutoReserve::SearchProgram& p,
        const std::wstring& stableProgramKey,
        const std::wstring& scheduleDedupeKey)
    {
        if (!IsRecordTaskKind_(t.request.kind)) {
            return false;
        }

        if (!scheduleDedupeKey.empty()) {
            if (t.reserve.dedupeKey == scheduleDedupeKey ||
                t.reserve.excludeKey == scheduleDedupeKey) {
                return true;
            }
        }

        if (!stableProgramKey.empty()) {
            if (t.reserve.dedupeKey == stableProgramKey ||
                t.reserve.excludeKey == stableProgramKey) {
                return true;
            }

            const std::wstring taskMainSubKey =
                BuildAutoReserveMainSubTaskKey_(t);

            if (!taskMainSubKey.empty() &&
                taskMainSubKey == stableProgramKey) {
                return true;
            }
        }

        if (AutoReserveProgramIdsMatchTask_(t, p)) {
            return true;
        }

        return AutoReserveProgramTextTimeMatchesTask_(t, p);
    }

    static AutoReserveExistingState_ FindAutoReserveExistingState_(
        const std::vector<TaskListManager::TaskEntry>& snapshot,
        const AutoReserve::SearchProgram& p,
        const std::wstring& stableProgramKey,
        const std::wstring& scheduleDedupeKey,
        bool protectUserBlockedReservation,
        bool skipDuplicate)
    {
        for (const auto& t : snapshot) {
            if (!AutoReserveProgramMatchesTask_(
                t,
                p,
                stableProgramKey,
                scheduleDedupeKey)) {
                continue;
            }

            if (protectUserBlockedReservation &&
                (t.reserve.disabledByUser || t.reserve.excludeFromAuto)) {
                return AutoReserveExistingState_::UserBlocked;
            }

            if (skipDuplicate) {
                return AutoReserveExistingState_::Duplicate;
            }
        }

        return AutoReserveExistingState_::None;
    }
}

TaskSystemConnector::AutoReserveRecordingApplyResult
TaskSystemConnector::RegisterAutoReserveMatchesAsRecording(
    const AutoReserve::RuleMatchResult& matchResult,
    const AutoReserveRecordingApplyOptions& options)
{
    AutoReserveRecordingApplyResult out{};

    out.inputMatches = matchResult.matches.size();

    if (!initialized_) {
        out.ok = false;
        out.message = L"TaskSystemConnector is not initialized";

        DebugLogFormat(
            L"[TaskSystemConnector][AUTO-RESERVE-REC][ERROR] %ls",
            out.message.c_str());

        return out;
    }

    if (!matchResult.ok) {
        out.ok = false;
        out.message =
            L"match result is not ok: " + matchResult.message;

        DebugLogFormat(
            L"[TaskSystemConnector][AUTO-RESERVE-REC][ERROR] %ls",
            out.message.c_str());

        return out;
    }

    DebugLogFormat(
        L"[TaskSystemConnector][AUTO-RESERVE-REC] begin matches=%zu dryRun=%d allowRegister=%d includeCandidate=%d includeAuto=%d skipDuplicate=%d max=%zu",
        matchResult.matches.size(),
        options.dryRun ? 1 : 0,
        options.allowRegister ? 1 : 0,
        options.includeCandidateRules ? 1 : 0,
        options.includeAutoRules ? 1 : 0,
        options.skipDuplicate ? 1 : 0,
        options.maxRegisterCount);

    std::vector<const AutoReserve::RuleProgramMatch*> orderedMatches;
    orderedMatches.reserve(matchResult.matches.size());

    for (const AutoReserve::RuleProgramMatch& m : matchResult.matches) {
        orderedMatches.push_back(&m);
    }

    std::stable_sort(
        orderedMatches.begin(),
        orderedMatches.end(),
        AutoReserveMatchMainServiceLess_);

    std::unordered_set<std::wstring> seenProgramKeys;
    seenProgramKeys.reserve(matchResult.matches.size());

    for (const AutoReserve::RuleProgramMatch* matchPtr : orderedMatches) {
        if (matchPtr == nullptr) {
            continue;
        }

        const AutoReserve::RuleProgramMatch& m =
            *matchPtr;
        if (m.autoRegister) {
            ++out.autoInputs;
        }
        else {
            ++out.candidateInputs;
        }

        if (m.autoRegister && !options.includeAutoRules) {
            ++out.actionSkipped;
            continue;
        }

        if (!m.autoRegister && !options.includeCandidateRules) {
            ++out.actionSkipped;
            continue;
        }

        if (!IsAutoReserveSearchProgramUsable_(m.program)) {
            ++out.invalidSkipped;

            DebugLogFormat(
                L"[TaskSystemConnector][AUTO-RESERVE-REC][SKIP-INVALID] rule=%u title=%ls service=%ls date=%ls time=%ls start=%ls %ls end=%ls %ls duration=%u",
                static_cast<unsigned>(m.ruleId),
                m.program.title.c_str(),
                m.program.serviceName.c_str(),
                m.program.dateText.c_str(),
                m.program.timeText.c_str(),
                m.program.startDate.c_str(),
                m.program.startTime.c_str(),
                m.program.endDate.c_str(),
                m.program.endTime.c_str(),
                static_cast<unsigned>(m.program.durationSec));

            continue;
        }

        const std::wstring stableProgramKey =
            BuildAutoReserveStableProgramKey_(m);

        const std::wstring sameTransportProgramKey =
            BuildAutoReserveMainSubProgramKey_(m);

        if (options.skipDuplicate) {
            const auto insertResult =
                seenProgramKeys.insert(sameTransportProgramKey);

            if (!insertResult.second) {
                ++out.duplicateSkipped;

                DebugLogFormat(
                    L"[TaskSystemConnector][AUTO-RESERVE-REC][SKIP-DUP-IN-MATCH] rule=%u title=%ls service=%ls key=%ls stable=%ls",
                    static_cast<unsigned>(m.ruleId),
                    m.program.title.c_str(),
                    m.program.serviceName.c_str(),
                    sameTransportProgramKey.c_str(),
                    stableProgramKey.c_str());

                continue;
            }
        }

        const EpgReserveCommandEx cmd =
            MakeAutoReserveRecordingCommand_(
                m,
                options,
                sameTransportProgramKey);

        const std::wstring scheduleDedupeKey =
            BuildAutoReserveScheduleDedupeCandidate_(cmd);

        const auto snapshot =
            taskList_.GetSnapshot();

        const AutoReserveExistingState_ existing =
            FindAutoReserveExistingState_(
                snapshot,
                m.program,
                sameTransportProgramKey,
                scheduleDedupeKey,
                options.protectUserBlockedReservation,
                options.skipDuplicate);

        if (existing == AutoReserveExistingState_::UserBlocked) {
            ++out.userBlockedSkipped;

            DebugLogFormat(
                L"[TaskSystemConnector][AUTO-RESERVE-REC][SKIP-USER-BLOCKED] rule=%u title=%ls service=%ls key=%ls stable=%ls dedupe=%ls",
                static_cast<unsigned>(m.ruleId),
                m.program.title.c_str(),
                m.program.serviceName.c_str(),
                sameTransportProgramKey.c_str(),
                stableProgramKey.c_str(),
                scheduleDedupeKey.c_str());

            continue;
        }

        if (existing == AutoReserveExistingState_::Duplicate) {
            ++out.duplicateSkipped;

            DebugLogFormat(
                L"[TaskSystemConnector][AUTO-RESERVE-REC][SKIP-DUP-EXISTING] rule=%u title=%ls service=%ls key=%ls stable=%ls dedupe=%ls",
                static_cast<unsigned>(m.ruleId),
                m.program.title.c_str(),
                m.program.serviceName.c_str(),
                sameTransportProgramKey.c_str(),
                stableProgramKey.c_str(),
                scheduleDedupeKey.c_str());

            continue;
        }

        if (options.maxRegisterCount != 0 &&
            out.targetPrograms >= options.maxRegisterCount) {
            ++out.limitSkipped;

            DebugLogFormat(
                L"[TaskSystemConnector][AUTO-RESERVE-REC][SKIP-LIMIT] rule=%u title=%ls service=%ls max=%zu",
                static_cast<unsigned>(m.ruleId),
                m.program.title.c_str(),
                m.program.serviceName.c_str(),
                options.maxRegisterCount);

            continue;
        }

        ++out.targetPrograms;

        if (options.dryRun || !options.allowRegister) {
            ++out.dryRunSkipped;

            DebugLogFormat(
                L"[TaskSystemConnector][AUTO-RESERVE-REC][DRYRUN] rule=%u auto=%d title=%ls service=%ls date=%ls time=%ls start=%ls %ls end=%ls %ls stable=%ls dedupe=%ls reason=%ls",
                static_cast<unsigned>(m.ruleId),
                m.autoRegister ? 1 : 0,
                m.program.title.c_str(),
                m.program.serviceName.c_str(),
                m.program.dateText.c_str(),
                m.program.timeText.c_str(),
                m.program.startDate.c_str(),
                m.program.startTime.c_str(),
                m.program.endDate.c_str(),
                m.program.endTime.c_str(),
                stableProgramKey.c_str(),
                scheduleDedupeKey.c_str(),
                m.reason.c_str());

            continue;
        }

        const bool ok =
            ScheduleProgramReservation(cmd);

        if (ok) {
            ++out.registered;

            DebugLogFormat(
                L"[TaskSystemConnector][AUTO-RESERVE-REC][REGISTERED] rule=%u auto=%d title=%ls service=%ls start=%ls %ls end=%ls %ls stable=%ls dedupe=%ls",
                static_cast<unsigned>(m.ruleId),
                m.autoRegister ? 1 : 0,
                m.program.title.c_str(),
                m.program.serviceName.c_str(),
                m.program.startDate.c_str(),
                m.program.startTime.c_str(),
                m.program.endDate.c_str(),
                m.program.endTime.c_str(),
                stableProgramKey.c_str(),
                scheduleDedupeKey.c_str());
        }
        else {
            ++out.failed;

            DebugLogFormat(
                L"[TaskSystemConnector][AUTO-RESERVE-REC][FAILED] rule=%u auto=%d title=%ls service=%ls start=%ls %ls end=%ls %ls stable=%ls dedupe=%ls",
                static_cast<unsigned>(m.ruleId),
                m.autoRegister ? 1 : 0,
                m.program.title.c_str(),
                m.program.serviceName.c_str(),
                m.program.startDate.c_str(),
                m.program.startTime.c_str(),
                m.program.endDate.c_str(),
                m.program.endTime.c_str(),
                stableProgramKey.c_str(),
                scheduleDedupeKey.c_str());
        }
    }

    out.ok = out.failed == 0;

    out.message =
        L"input=" + std::to_wstring(out.inputMatches) +
        L" target=" + std::to_wstring(out.targetPrograms) +
        L" registered=" + std::to_wstring(out.registered) +
        L" dryRun=" + std::to_wstring(out.dryRunSkipped) +
        L" failed=" + std::to_wstring(out.failed) +
        L" duplicate=" + std::to_wstring(out.duplicateSkipped) +
        L" userBlocked=" + std::to_wstring(out.userBlockedSkipped) +
        L" invalid=" + std::to_wstring(out.invalidSkipped) +
        L" limit=" + std::to_wstring(out.limitSkipped) +
        L" actionSkipped=" + std::to_wstring(out.actionSkipped);

    DebugLogFormat(
        L"[TaskSystemConnector][AUTO-RESERVE-REC] end ok=%d %ls",
        out.ok ? 1 : 0,
        out.message.c_str());

    if (out.failed != 0) {
        SystemInfoLogWarn(
            L"自動予約の録画予約登録で失敗があります。%ls",
            out.message.c_str());
    }
    else if (!options.dryRun && options.allowRegister && out.registered != 0) {
        SystemInfoLogInfo(
            L"自動予約の条件一致番組を録画予約へ登録しました。%ls",
            out.message.c_str());
    }

    return out;
}

namespace
{
    bool IsProgramReservationAdjustMatch_(
        const TaskListManager::TaskEntry& t,
        const TaskSystemConnector::ProgramReservationAdjustRequest& req)
    {
        if (!IsRecordTaskKind_(t.request.kind)) {
            return false;
        }

        if (req.instance != 0 && t.request.instance == req.instance) {
            return true;
        }

        // 第1照合: チャンネル識別 + eventId。
        // space は地デジ/BS/CS の混線防止用。
        if (req.space >= 0 && t.reserve.space >= 0 && t.reserve.space != req.space) {
            return false;
        }

        if (req.networkId != 0 && t.reserve.networkId != req.networkId) {
            return false;
        }

        if (req.tsId != 0 && t.reserve.tsId != req.tsId) {
            return false;
        }

        if (req.serviceId != 0 && t.reserve.serviceId != req.serviceId) {
            return false;
        }

        if (req.eventId != 0 && t.reserve.eventId != req.eventId) {
            return false;
        }

        // eventId が取れている場合を最優先。
        // serviceIdだけ一致では誤補正しやすいので、初期段階では eventId 必須寄りにする。
        return req.eventId != 0 && req.serviceId != 0;
    }

    const wchar_t* GetRecordSuffixByKind_(std::uint32_t kind) noexcept
    {
        switch (kind) {
        case TaskKinds::RecPrepare:
            return L" / Prepare";
        case TaskKinds::RecStart:
            return L" / Start";
        case TaskKinds::RecStop:
            return L" / Stop";
        default:
            return L"";
        }
    }

    std::wstring PickReservationTitleForLog_(
        const TaskListManager::TaskEntry& t)
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
        return t.name.empty() ? L"(no title)" : t.name;
    }

    std::wstring FormatIsoDateTimeForLog_(
        const std::wstring& date,
        const std::wstring& time)
    {
        if (date.empty() && time.empty()) {
            return L"(empty)";
        }

        if (date.empty()) {
            return time;
        }

        if (time.empty()) {
            return date;
        }

        return date + L" " + time;
    }

    std::wstring FormatTaskDueForLog_(
        const TaskListManager::TaskEntry& t)
    {
        wchar_t buf[64]{};

        if (t.schedule.dueYmd <= 0 || t.schedule.dueHHMM < 0) {
            return L"(invalid)";
        }

        swprintf_s(
            buf,
            L"%08d %02d:%02d:%02d",
            t.schedule.dueYmd,
            t.schedule.dueHHMM / 100,
            t.schedule.dueHHMM % 100,
            (std::clamp)(t.request.dispatchSecond, 0, 59));

        return std::wstring(buf);
    }

    bool ProgramAdjustRequestMatchesUpdatedChannel_(
        const TaskSystemConnector::ProgramReservationAdjustRequest& p,
        const TaskSystemConnector::EpgUpdateTaskRefreshRequest& req) noexcept
    {
        if (req.space >= 0 && p.space >= 0 && p.space != req.space) {
            return false;
        }

        if (req.networkId != 0 && p.networkId != 0 && p.networkId != req.networkId) {
            return false;
        }

        if (req.tsId != 0 && p.tsId != 0 && p.tsId != req.tsId) {
            return false;
        }

        if (req.serviceId != 0 && p.serviceId != 0 && p.serviceId != req.serviceId) {
            return false;
        }

        return true;
    }
}// namespace

bool TaskSystemConnector::AdjustProgramReservationFromEit(
    const ProgramReservationAdjustRequest& req)
{
    if (!initialized_) {
        return false;
    }

    if (req.serviceId == 0 && req.eventId == 0 && req.instance == 0) {
        return false;
    }

    int newStartYmd = 0;
    int newStartHHMM = -1;
    int newStartSec = 0;

    int newEndYmd = 0;
    int newEndHHMM = -1;
    int newEndSec = 0;

    if (req.allowStartTimeUpdate) {
        newStartYmd = ParseIsoDateToYmd_(req.startDate);
        if (!ParseHHMMSSLoose_(req.startTime, newStartHHMM, newStartSec)) {
            newStartHHMM = ParseIsoTimeToHHMM_(req.startTime);
            newStartSec = 0;
        }

        if (newStartYmd <= 0 || newStartHHMM < 0) {
            return false;
        }
    }

    if (req.allowStopTimeUpdate) {
        newEndYmd = ParseIsoDateToYmd_(req.endDate);
        if (!ParseHHMMSSLoose_(req.endTime, newEndHHMM, newEndSec)) {
            newEndHHMM = ParseIsoTimeToHHMM_(req.endTime);
            newEndSec = 0;
        }

        if (newEndYmd <= 0 || newEndHHMM < 0) {
            return false;
        }
    }

    const auto snap = taskList_.GetSnapshot();

    std::uint32_t targetInstance = req.instance;

    if (targetInstance == 0) {
        for (const auto& t : snap) {
            if (IsProgramReservationAdjustMatch_(t, req)) {
                targetInstance = t.request.instance;
                break;
            }
        }
    }

    if (targetInstance == 0) {
        DebugLogFormat(
            L"[TaskSystemConnector][EIT-ADJUST][NO-TARGET] nid=%u tsid=%u sid=%u eid=%u title=%ls",
            static_cast<unsigned>(req.networkId),
            static_cast<unsigned>(req.tsId),
            static_cast<unsigned>(req.serviceId),
            static_cast<unsigned>(req.eventId),
            req.title.c_str());
        return false;
    }

    TaskListManager::TaskEntry prepare{};
    TaskListManager::TaskEntry start{};
    TaskListManager::TaskEntry stop{};

    bool hasPrepare = false;
    bool hasStart = false;
    bool hasStop = false;

    for (const auto& t : snap) {
        if (t.request.instance != targetInstance) {
            continue;
        }

        if (t.request.kind == TaskKinds::RecPrepare) {
            prepare = t;
            hasPrepare = true;
        }
        else if (t.request.kind == TaskKinds::RecStart) {
            start = t;
            hasStart = true;
        }
        else if (t.request.kind == TaskKinds::RecStop) {
            stop = t;
            hasStop = true;
        }
    }

    if (!hasStart || !hasStop) {
        DebugLogFormat(
            L"[TaskSystemConnector][EIT-ADJUST][BAD-GROUP] inst=%u prepare=%d start=%d stop=%d",
            static_cast<unsigned>(targetInstance),
            hasPrepare ? 1 : 0,
            hasStart ? 1 : 0,
            hasStop ? 1 : 0);
        return false;
    }

    const std::wstring oldTitleForLog =
        PickReservationTitleForLog_(start);

    const std::wstring oldStartForLog =
        FormatIsoDateTimeForLog_(
            start.reserve.startDate,
            start.reserve.startTime);

    const std::wstring oldEndForLog =
        FormatIsoDateTimeForLog_(
            start.reserve.endDate,
            start.reserve.endTime);

    const std::wstring oldPrepareDueForLog =
        hasPrepare ? FormatTaskDueForLog_(prepare) : L"(none)";

    const std::wstring oldStartDueForLog =
        FormatTaskDueForLog_(start);

    const std::wstring oldStopDueForLog =
        FormatTaskDueForLog_(stop);

    const std::wstring requestedTitleForLog =
        !req.titleLabel.empty()
        ? req.titleLabel
        : req.title;

    const bool willChangeTitle =
        req.allowTitleUpdate &&
        !requestedTitleForLog.empty() &&
        requestedTitleForLog != oldTitleForLog;

    const bool willChangeStart =
        req.allowStartTimeUpdate &&
        !req.startDate.empty() &&
        !req.startTime.empty() &&
        (start.reserve.startDate != req.startDate ||
            start.reserve.startTime != req.startTime);

    const bool willChangeEnd =
        req.allowStopTimeUpdate &&
        !req.endDate.empty() &&
        !req.endTime.empty() &&
        (start.reserve.endDate != req.endDate ||
            start.reserve.endTime != req.endTime);

    const bool willChangeSummary =
        !req.summary.empty() &&
        start.reserve.summary != req.summary;

    const bool willChangeGenre =
        !req.genre.empty() &&
        start.reserve.genre != req.genre;

    const bool willChangeIds =
        (req.networkId != 0 && start.reserve.networkId != req.networkId) ||
        (req.tsId != 0 && start.reserve.tsId != req.tsId) ||
        (req.serviceId != 0 && start.reserve.serviceId != req.serviceId) ||
        (req.eventId != 0 && start.reserve.eventId != req.eventId);

    if (!willChangeTitle &&
        !willChangeStart &&
        !willChangeEnd &&
        !willChangeSummary &&
        !willChangeGenre &&
        !willChangeIds) {
        DebugLogFormat(
            L"[TaskSystemConnector][EIT-ADJUST][NO-CHANGE] inst=%u space=%d nid=%u tsid=%u sid=%u eid=%u title=%ls",
            static_cast<unsigned>(targetInstance),
            req.space,
            static_cast<unsigned>(req.networkId),
            static_cast<unsigned>(req.tsId),
            static_cast<unsigned>(req.serviceId),
            static_cast<unsigned>(req.eventId),
            oldTitleForLog.c_str());

        return true;
    }

    const int startAdvanceSec = ReadReservationStartAdvanceSecFromConfig_();
    const int startAdjustSec = ReadReservationStartAdjustSecFromConfig_();
    const int endAdjustMin = ReadReservationEndAdjustMinFromConfig_();

    int prepareYmd = 0;
    int prepareHHMM = -1;
    int prepareSec = 0;

    int startYmd = 0;
    int startHHMM = -1;
    int startSec = 0;

    int stopYmd = 0;
    int stopHHMM = -1;
    int stopSec = 0;

    if (req.allowStartTimeUpdate) {
        (void)ShiftLocalYmdHHMMSS_(
            newStartYmd,
            newStartHHMM,
            newStartSec,
            -startAdvanceSec,
            prepareYmd,
            prepareHHMM,
            prepareSec);

        (void)ShiftLocalYmdHHMMSS_(
            newStartYmd,
            newStartHHMM,
            newStartSec,
            startAdjustSec,
            startYmd,
            startHHMM,
            startSec);
    }

    if (req.allowStopTimeUpdate) {
        (void)ShiftLocalYmdHHMMSS_(
            newEndYmd,
            newEndHHMM,
            newEndSec,
            endAdjustMin * 60,
            stopYmd,
            stopHHMM,
            stopSec);
    }

    auto applyCommon =
        [&](TaskListManager::TaskEntry& t)
        {
            if (req.allowTitleUpdate) {
                const std::wstring newTitle =
                    req.title.empty()
                    ? (t.reserve.title.empty() ? t.display.label : t.reserve.title)
                    : req.title;

                const std::wstring newTitleLabel =
                    req.titleLabel.empty()
                    ? newTitle
                    : req.titleLabel;

                t.name = newTitle + GetRecordSuffixByKind_(t.request.kind);
                t.display.label = newTitleLabel;

                t.reserve.title = newTitle;
                t.reserve.titleLabel = newTitleLabel;

                if (!req.summary.empty()) {
                    t.reserve.summary = req.summary;
                }

                if (!req.genre.empty()) {
                    t.reserve.genre = req.genre;
                }
            }

            if (req.allowStartTimeUpdate) {
                t.reserve.startDate = req.startDate;
                t.reserve.startTime = req.startTime;
            }

            if (req.allowStopTimeUpdate) {
                t.reserve.endDate = req.endDate;
                t.reserve.endTime = req.endTime;
            }

            if (req.durationSec > 0) {
                t.reserve.durationSec = req.durationSec;
            }

            if (req.networkId != 0) {
                t.reserve.networkId = req.networkId;
            }

            if (req.tsId != 0) {
                t.reserve.tsId = req.tsId;
            }

            if (req.serviceId != 0) {
                t.reserve.serviceId = req.serviceId;
            }

            if (req.eventId != 0) {
                t.reserve.eventId = req.eventId;
            }
        };

    if (hasPrepare) {
        applyCommon(prepare);

        if (req.allowStartTimeUpdate && prepareYmd > 0 && prepareHHMM >= 0) {
            SetDispatchTime_(prepare.request, prepareYmd, prepareHHMM, prepareSec);
            prepare.schedule.dueYmd = prepareYmd;
            prepare.schedule.dueHHMM = prepareHHMM;
            prepare.schedule.lateToleranceMinutes = 1;

            prepare.display.endYmd = newStartYmd;
            prepare.display.endHHMM = newStartHHMM;
        }

        taskList_.UpsertTask(prepare);

        if (req.allowStartTimeUpdate && prepareYmd > 0 && prepareHHMM >= 0) {
            ResetExecutionMonitorForTask_(TaskKinds::RecPrepare, targetInstance);
        }
    }

    applyCommon(start);

    if (req.allowStartTimeUpdate && startYmd > 0 && startHHMM >= 0) {
        SetDispatchTime_(start.request, startYmd, startHHMM, startSec);
        start.schedule.dueYmd = startYmd;
        start.schedule.dueHHMM = startHHMM;
        start.schedule.lateToleranceMinutes = 1;
    }

    if (req.allowStopTimeUpdate) {
        start.display.endYmd = newEndYmd;
        start.display.endHHMM = newEndHHMM;
    }

    taskList_.UpsertTask(start);

    if (req.allowStartTimeUpdate && startYmd > 0 && startHHMM >= 0) {
        ResetExecutionMonitorForTask_(TaskKinds::RecStart, targetInstance);
    }


    applyCommon(stop);

    if (req.allowStopTimeUpdate && stopYmd > 0 && stopHHMM >= 0) {
        SetDispatchTime_(stop.request, stopYmd, stopHHMM, stopSec);
        stop.schedule.dueYmd = stopYmd;
        stop.schedule.dueHHMM = stopHHMM;
        stop.schedule.lateToleranceMinutes = 3;

        stop.display.endYmd = stopYmd;
        stop.display.endHHMM = stopHHMM;
    }

    taskList_.UpsertTask(stop);

    if (req.allowStopTimeUpdate && stopYmd > 0 && stopHHMM >= 0) {
        ResetExecutionMonitorForTask_(TaskKinds::RecStop, targetInstance);
    }

    DebugLogFormat(
        L"[TaskSystemConnector][EIT-ADJUST][APPLY] inst=%u title=%ls startUpdate=%d stopUpdate=%d newStart=%ls %ls newEnd=%ls %ls",
        static_cast<unsigned>(targetInstance),
        req.title.c_str(),
        req.allowStartTimeUpdate ? 1 : 0,
        req.allowStopTimeUpdate ? 1 : 0,
        req.startDate.c_str(),
        req.startTime.c_str(),
        req.endDate.c_str(),
        req.endTime.c_str());

    const bool applyOk = ApplyTaskList();
    if (!applyOk) {
        SystemInfoLogError(
            L"EITによる番組予約補正後の再反映に失敗しました。instance=%u title=%ls",
            static_cast<unsigned>(targetInstance),
            req.title.c_str());
        return false;
    }

    if (!SaveTasksToConfiguredIni_()) {
        SystemInfoLogError(
            L"EITによる番組予約補正後の保存に失敗しました。instance=%u title=%ls",
            static_cast<unsigned>(targetInstance),
            req.title.c_str());
        return false;
    }

    const std::wstring newTitleForLog =
        PickReservationTitleForLog_(start);

    const std::wstring newStartForLog =
        FormatIsoDateTimeForLog_(
            start.reserve.startDate,
            start.reserve.startTime);

    const std::wstring newEndForLog =
        FormatIsoDateTimeForLog_(
            start.reserve.endDate,
            start.reserve.endTime);

    const std::wstring newPrepareDueForLog =
        hasPrepare ? FormatTaskDueForLog_(prepare) : L"(none)";

    const std::wstring newStartDueForLog =
        FormatTaskDueForLog_(start);

    const std::wstring newStopDueForLog =
        FormatTaskDueForLog_(stop);

    const bool titleChangedForLog =
        oldTitleForLog != newTitleForLog;

    const bool programTimeChangedForLog =
        oldStartForLog != newStartForLog ||
        oldEndForLog != newEndForLog;

    const bool taskTimeChangedForLog =
        oldPrepareDueForLog != newPrepareDueForLog ||
        oldStartDueForLog != newStartDueForLog ||
        oldStopDueForLog != newStopDueForLog;

    if (titleChangedForLog) {
        SystemInfoLogInfo(
            L"EPG更新により番組予約タイトルを変更しました。instance=%u old=\"%ls\" new=\"%ls\"",
            static_cast<unsigned>(targetInstance),
            oldTitleForLog.c_str(),
            newTitleForLog.c_str());
    }

    if (programTimeChangedForLog || taskTimeChangedForLog) {
        SystemInfoLogInfo(
            L"EPG更新により番組予約時刻を変更しました。instance=%u title=\"%ls\" 番組 old=\"%ls - %ls\" new=\"%ls - %ls\" 実行 old=\"prepare:%ls start:%ls stop:%ls\" new=\"prepare:%ls start:%ls stop:%ls\"",
            static_cast<unsigned>(targetInstance),
            newTitleForLog.c_str(),
            oldStartForLog.c_str(),
            oldEndForLog.c_str(),
            newStartForLog.c_str(),
            newEndForLog.c_str(),
            oldPrepareDueForLog.c_str(),
            oldStartDueForLog.c_str(),
            oldStopDueForLog.c_str(),
            newPrepareDueForLog.c_str(),
            newStartDueForLog.c_str(),
            newStopDueForLog.c_str());
    }

    SystemInfoLogInfo(
        L"EIT情報により番組予約を補正しました。instance=%u title=\"%ls\" titleChanged=%d programTimeChanged=%d taskTimeChanged=%d startUpdate=%d stopUpdate=%d",
        static_cast<unsigned>(targetInstance),
        newTitleForLog.c_str(),
        titleChangedForLog ? 1 : 0,
        programTimeChangedForLog ? 1 : 0,
        taskTimeChangedForLog ? 1 : 0,
        req.allowStartTimeUpdate ? 1 : 0,
        req.allowStopTimeUpdate ? 1 : 0);

    return true;
}

bool TaskSystemConnector::RemoveProgramReservationGroupAndApply(std::uint32_t instance)
{
    if (!initialized_) {
        return false;
    }

    if (instance == 0) {
        return false;
    }

    const bool removedPrepare = taskList_.RemoveTask(TaskKinds::RecPrepare, instance);
    const bool removedStart = taskList_.RemoveTask(TaskKinds::RecStart, instance);
    const bool removedStop = taskList_.RemoveTask(TaskKinds::RecStop, instance);

    const bool removedAny = removedPrepare || removedStart || removedStop;

    ResetExecutionMonitorForTask_(TaskKinds::RecPrepare, instance);
    ResetExecutionMonitorForTask_(TaskKinds::RecStart, instance);
    ResetExecutionMonitorForTask_(TaskKinds::RecStop, instance);

    Log_(
        L"[TaskSystemConnector] RemoveProgramReservationGroupAndApply(inst=%u) prepare=%d start=%d stop=%d",
        static_cast<unsigned>(instance),
        removedPrepare ? 1 : 0,
        removedStart ? 1 : 0,
        removedStop ? 1 : 0);

    if (!removedAny) {
        SystemInfoLogWarn(
            L"番組予約の削除に失敗しました。instance=%u は未登録です。",
            static_cast<unsigned>(instance));
        return false;
    }

    const bool applyOk = ApplyTaskList();
    if (!applyOk) {
        SystemInfoLogError(
            L"番組予約削除後の再反映に失敗しました。instance=%u",
            static_cast<unsigned>(instance));
        return false;
    }

    if (!SaveTasksToConfiguredIni_()) {
        SystemInfoLogError(
            L"番組予約削除後の保存に失敗しました。instance=%u tasks.ini 書き込み失敗",
            static_cast<unsigned>(instance));
        return false;
    }

    SystemInfoLogInfo(
        L"番組予約を削除しました。instance=%u prepare=%d start=%d stop=%d",
        static_cast<unsigned>(instance),
        removedPrepare ? 1 : 0,
        removedStart ? 1 : 0,
        removedStop ? 1 : 0);

    return true;
}



bool TaskSystemConnector::SetProgramReservationGroupEnabledAndApply(
    std::uint32_t instance,
    bool enabled,
    bool disabledByUser,
    bool excludeFromAuto)
{
    if (!initialized_) {
        return false;
    }

    if (instance == 0) {
        return false;
    }

    bool changed = false;
    bool foundPrepare = false;
    bool foundStart = false;
    bool foundStop = false;

    auto snap = taskList_.GetSnapshot();

    for (auto& t : snap) {
        if (t.request.instance != instance) {
            continue;
        }

        switch (t.request.kind) {
        case TaskKinds::RecPrepare:
            foundPrepare = true;
            break;

        case TaskKinds::RecStart:
            foundStart = true;
            break;

        case TaskKinds::RecStop:
            foundStop = true;
            break;

        default:
            continue;
        }

        if (t.enabled != enabled ||
            t.reserve.disabledByUser != disabledByUser ||
            t.reserve.excludeFromAuto != excludeFromAuto) {
            changed = true;
        }

        t.enabled = enabled;
        t.reserve.disabledByUser = disabledByUser;
        t.reserve.excludeFromAuto = excludeFromAuto;

        ResetExecutionMonitorForTask_(t.request.kind, t.request.instance);
        taskList_.UpsertTask(t);
    }

    const bool foundAny = foundPrepare || foundStart || foundStop;

    Log_(
        L"[TaskSystemConnector] SetProgramReservationGroupEnabledAndApply(inst=%u enabled=%d disabledByUser=%d excludeFromAuto=%d prepare=%d start=%d stop=%d changed=%d)",
        static_cast<unsigned>(instance),
        enabled ? 1 : 0,
        disabledByUser ? 1 : 0,
        excludeFromAuto ? 1 : 0,
        foundPrepare ? 1 : 0,
        foundStart ? 1 : 0,
        foundStop ? 1 : 0,
        changed ? 1 : 0);

    if (!foundAny) {
        SystemInfoLogWarn(
            L"番組予約の有効状態変更に失敗しました。instance=%u は未登録です。",
            static_cast<unsigned>(instance));
        return false;
    }

    const bool applyOk = ApplyTaskList();
    if (!applyOk) {
        SystemInfoLogError(
            L"番組予約の有効状態変更後の再反映に失敗しました。instance=%u enabled=%d",
            static_cast<unsigned>(instance),
            enabled ? 1 : 0);
        return false;
    }

    if (!SaveTasksToConfiguredIni_()) {
        SystemInfoLogError(
            L"番組予約の有効状態変更後の保存に失敗しました。instance=%u tasks.ini 書き込み失敗",
            static_cast<unsigned>(instance));
        return false;
    }

    SystemInfoLogInfo(
        L"番組予約の有効状態を変更しました。instance=%u enabled=%d disabledByUser=%d excludeFromAuto=%d",
        static_cast<unsigned>(instance),
        enabled ? 1 : 0,
        disabledByUser ? 1 : 0,
        excludeFromAuto ? 1 : 0);

    return true;
}

TaskSystemConnector::AutoReserveRecordingApplyResult
TaskSystemConnector::RegisterAutoReserveMatchesAsRecordingFixed(
    const AutoReserve::RuleMatchResult& matchResult)
{
    AutoReserveRecordingApplyOptions opt{};

    opt.dryRun = false;
    opt.allowRegister = true;

    // candidate / auto の区別なし。
    // 検索一致した番組は録画予約対象にする。
    opt.includeCandidateRules = true;
    opt.includeAutoRules = true;

    opt.skipDuplicate = true;
    opt.protectUserBlockedReservation = true;
    opt.maxRegisterCount = 0;

    // 録画予約固定
    opt.reserveMode = L"record";
    opt.reserveSource = L"auto_reserve";
    opt.forceCreatedReservationEnabled = true;

    opt.requiredTuners = 1;
    opt.preferredTunerIndex = -1;

    return RegisterAutoReserveMatchesAsRecording(
        matchResult,
        opt);
}
