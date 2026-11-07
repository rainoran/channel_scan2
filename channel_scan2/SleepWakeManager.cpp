#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "SleepWakeManager.hpp"
#include "ConfigManager.h"

#include <PowrProf.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <ctime>
#include <cwchar>
#include <limits>
#include <utility>

#pragma comment(lib, "PowrProf.lib")

namespace {

    int ReadInt_(
        const std::wstring& path,
        const std::wstring& section,
        const wchar_t* key,
        int defaultValue = 0) noexcept
    {
        return static_cast<int>(GetPrivateProfileIntW(
            section.c_str(), key, defaultValue, path.c_str()));
    }

    std::wstring ReadText_(
        const std::wstring& path,
        const std::wstring& section,
        const wchar_t* key)
    {
        std::array<wchar_t, 1024> buffer{};

        GetPrivateProfileStringW(
            section.c_str(),
            key,
            L"",
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            path.c_str());

        return std::wstring(buffer.data());
    }

    bool ValidTaskId_(const std::wstring& id) noexcept
    {
        const auto comma = id.find(L',');

        if (id.size() > 48 ||
            comma == std::wstring::npos ||
            comma == 0 ||
            comma + 1 == id.size() ||
            id.find(L',', comma + 1) != std::wstring::npos) {
            return false;
        }

        for (std::size_t i = 0; i < id.size(); ++i) {
            if (i == comma) continue;
            if (id[i] < L'0' || id[i] > L'9') {
                return false;
            }
        }
        return true;
    }

    bool IsRecordStop_(
        const std::wstring& id,
        const std::wstring& name) noexcept
    {
        if (id.starts_with(L"1003,")) return true;

        constexpr wchar_t suffix[] = L" / Stop";
        constexpr std::size_t suffixLength =
            sizeof(suffix) / sizeof(wchar_t) - 1;

        return name.size() >= suffixLength &&
            name.compare(
                name.size() - suffixLength,
                suffixLength,
                suffix) == 0;
    }

    std::time_t MakeLocalTime_(
        int ymd, int hhmm, int seconds) noexcept
    {
        if (ymd < 19700101 || ymd > 99991231 ||
            hhmm < 0 || hhmm > 2359 ||
            hhmm % 100 >= 60 ||
            seconds < 0 || seconds > 59) {
            return -1;
        }

        const int year = ymd / 10000;
        const int month = (ymd / 100) % 100;
        const int day = ymd % 100;

        if (month < 1 || month > 12 ||
            day < 1 || day > 31) {
            return -1;
        }

        std::tm t{};
        t.tm_year = year - 1900;
        t.tm_mon = month - 1;
        t.tm_mday = day;
        t.tm_hour = hhmm / 100;
        t.tm_min = hhmm % 100;
        t.tm_sec = seconds;
        t.tm_isdst = -1;

        const std::time_t result = std::mktime(&t);

        // 不正日付の自動繰り上がりを拒否する。
        if (result == static_cast<std::time_t>(-1) ||
            t.tm_year != year - 1900 ||
            t.tm_mon != month - 1 ||
            t.tm_mday != day ||
            t.tm_hour != hhmm / 100 ||
            t.tm_min != hhmm % 100 ||
            t.tm_sec != seconds) {
            return -1;
        }

        return result;
    }

    DWORD LastInputTick_() noexcept
    {
        LASTINPUTINFO info{};
        info.cbSize = sizeof(info);
        return GetLastInputInfo(&info) ? info.dwTime : 0;
    }

} // namespace

SleepWakeManager::SleepWakeManager() noexcept
    : ownerThreadId_(GetCurrentThreadId())
{
}

SleepWakeManager::~SleepWakeManager() noexcept
{
    Shutdown();
}

bool SleepWakeManager::IsOwnerThread_() const noexcept
{
    return ownerThreadId_ == GetCurrentThreadId();
}

bool SleepWakeManager::ReadStamp_(
    const std::wstring& path,
    FileStamp& stamp,
    DWORD& error) noexcept
{
    if (path.empty()) {
        error = ERROR_INVALID_PARAMETER;
        return false;
    }

    WIN32_FILE_ATTRIBUTE_DATA data{};

    if (!GetFileAttributesExW(
        path.c_str(),
        GetFileExInfoStandard,
        &data)) {
        error = GetLastError();
        return false;
    }

    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        error = ERROR_INVALID_DATA;
        return false;
    }

    stamp.writeTime = data.ftLastWriteTime;
    stamp.sizeHigh = data.nFileSizeHigh;
    stamp.sizeLow = data.nFileSizeLow;

    error = ERROR_SUCCESS;
    return true;
}

bool SleepWakeManager::SameStamp_(
    const FileStamp& a,
    const FileStamp& b) noexcept
{
    return CompareFileTime(
        &a.writeTime, &b.writeTime) == 0 &&
        a.sizeHigh == b.sizeHigh &&
        a.sizeLow == b.sizeLow;
}

bool SleepWakeManager::ParseTasks_(
    const std::wstring& path,
    const FileStamp& before,
    std::vector<TaskEntry>& entries,
    unsigned int& unsupported,
    DWORD& error)
{
    const int count = ReadInt_(
        path, L"TaskList", L"Count", -1);

    if (count < 0 || count > 10000) {
        error = ERROR_INVALID_DATA;
        return false;
    }

    entries.clear();
    entries.reserve(static_cast<std::size_t>(count));
    unsupported = 0;

    for (int i = 0; i < count; ++i) {
        const std::wstring key =
            L"Task_" + std::to_wstring(i);

        const std::wstring id =
            ReadText_(path, L"TaskList", key.c_str());

        if (!ValidTaskId_(id)) {
            error = ERROR_INVALID_DATA;
            return false;
        }

        const auto comma = id.find(L',');

        const std::wstring section =
            L"Task_" +
            id.substr(0, comma) +
            L"_" +
            id.substr(comma + 1);

        if (ReadInt_(path, section, L"Enabled") != 1 ||
            ReadInt_(path, section, L"DisabledByUser") == 1) {
            continue;
        }

        const std::wstring category =
            ReadText_(path, section, L"Category");

        if (category != L"RECORD" &&
            category != L"EPG") {
            continue;
        }

        if (category == L"RECORD" &&
            IsRecordStop_(
                id,
                ReadText_(path, section, L"Name"))) {
            continue;
        }

        TaskEntry entry{};
        entry.section = section;

        const int scheduleType =
            ReadInt_(path, section, L"ScheduleType", -1);

        // tasks.ini の外部保存値は 1=Daily, 2=Hourly, 4=OneShot。
        // それぞれ単一のスケジュール種別を表す。
        // & に変更すると値4を誤判定するため、等値比較を維持する。
        if (scheduleType == 4) {
            entry.kind = ScheduleKind::Once;

            entry.ymd =
                ReadInt_(path, section, L"DispatchYmd");
            entry.hhmm =
                ReadInt_(path, section, L"DispatchHHMM");
            entry.second =
                ReadInt_(path, section, L"DispatchSecond");

            if (entry.ymd == 0) {
                entry.ymd =
                    ReadInt_(path, section, L"DueYmd");
                entry.hhmm =
                    ReadInt_(path, section, L"DueHHMM");
                entry.second =
                    ReadInt_(path, section, L"DueSecond");
            }

            if (MakeLocalTime_(
                entry.ymd,
                entry.hhmm,
                entry.second) <= 0) {
                error = ERROR_INVALID_DATA;
                return false;
            }
        }
        else if (scheduleType == 1) {
            entry.kind = ScheduleKind::Daily;

            entry.dailyHour =
                ReadInt_(path, section, L"DailyHour", -1);
            entry.dailyMinute =
                ReadInt_(path, section, L"DailyMinute", -1);

            if (entry.dailyHour < 0 ||
                entry.dailyHour > 23 ||
                entry.dailyMinute < 0 ||
                entry.dailyMinute > 59) {
                error = ERROR_INVALID_DATA;
                return false;
            }
        }
        else if (scheduleType == 2) {
            entry.kind = ScheduleKind::Hourly;

            entry.hourlyMinute =
                ReadInt_(
                    path, section,
                    L"HourlyAtMinute", -1);

            if (entry.hourlyMinute < 0 ||
                entry.hourlyMinute > 59) {
                error = ERROR_INVALID_DATA;
                return false;
            }
        }
        else {
            // Interval等は次回時刻の基準が未実装。
            // 誤ったタイマーを作らず検出数を記録する。
            ++unsupported;
            continue;
        }

        entries.emplace_back(std::move(entry));
    }

    // 保存途中の読み取りでないかを更新時刻とファイルサイズで確認。
    FileStamp after{};

    if (!ReadStamp_(path, after, error)) {
        return false;
    }

    if (!SameStamp_(before, after)) {
        error = ERROR_RETRY;
        return false;
    }

    error = ERROR_SUCCESS;
    return true;
}

bool SleepWakeManager::ReloadTasks_(
    const std::wstring& path,
    bool& reloaded,
    DWORD& error)
{
    reloaded = false;

    FileStamp current{};

    if (!ReadStamp_(path, current, error)) {
        return false;
    }

    if (!forceReload_ &&
        cacheValid_ &&
        taskFilePath_ == path &&
        SameStamp_(fileStamp_, current)) {
        error = ERROR_SUCCESS;
        return true;
    }

    std::vector<TaskEntry> newEntries;
    unsigned int unsupported = 0;

    if (!ParseTasks_(
        path,
        current,
        newEntries,
        unsupported,
        error)) {
        return false;
    }

    taskCache_.swap(newEntries);
    fileStamp_ = current;
    taskFilePath_ = path;
    cacheValid_ = true;
    forceReload_ = false;
    unsupportedCount_ = unsupported;

    reloaded = true;
    error = ERROR_SUCCESS;
    return true;
}

std::time_t SleepWakeManager::DispatchTime_(
    const TaskEntry& task,
    std::time_t now) noexcept
{
    if (task.kind == ScheduleKind::Once) {
        return MakeLocalTime_(
            task.ymd, task.hhmm, task.second);
    }

    std::tm base{};

    if (localtime_s(&base, &now) != 0) {
        return -1;
    }

    if (task.kind == ScheduleKind::Daily) {
        for (int day = 0; day < 4; ++day) {
            std::tm t = base;

            t.tm_mday += day;
            t.tm_hour = task.dailyHour;
            t.tm_min = task.dailyMinute;
            t.tm_sec = 0;
            t.tm_isdst = -1;

            const auto value = std::mktime(&t);

            if (value > now) {
                return value;
            }
        }
    }
    else if (task.kind == ScheduleKind::Hourly) {
        for (int hour = 0; hour < 48; ++hour) {
            std::tm t = base;

            t.tm_hour += hour;
            t.tm_min = task.hourlyMinute;
            t.tm_sec = 0;
            t.tm_isdst = -1;

            const auto value = std::mktime(&t);

            if (value > now) {
                return value;
            }
        }
    }

    return -1;
}

SleepWakeManager::NextTask
SleepWakeManager::FindNext_(std::time_t now) const
{
    NextTask best{};

    for (const auto& task : taskCache_) {
        const auto due = DispatchTime_(task, now);

        if (due <= now) continue;

        if (best.dispatch == 0 ||
            due < best.dispatch) {
            best.dispatch = due;
            best.section = task.section;
        }
    }

    return best;
}

void SleepWakeManager::CancelWakeTimer_() noexcept
{
    if (wakeTimer_) {
        CancelWaitableTimer(wakeTimer_);
    }

    armedWakeTime_ = 0;
}

bool SleepWakeManager::ArmWakeTimer_(
    std::time_t unixTime,
    DWORD& error) noexcept
{
    constexpr std::int64_t epochOffset =
        11644473600LL;
    constexpr std::int64_t ticksPerSecond =
        10000000LL;

    const auto seconds =
        static_cast<std::int64_t>(unixTime);

    if (seconds <= 0 ||
        seconds >
        (std::numeric_limits<std::int64_t>::max)()
        / ticksPerSecond - epochOffset) {
        error = ERROR_INVALID_PARAMETER;
        return false;
    }

    if (!wakeTimer_) {
        wakeTimer_ = CreateWaitableTimerW(
            nullptr, TRUE, nullptr);

        if (!wakeTimer_) {
            error = GetLastError();
            return false;
        }
    }

    LARGE_INTEGER due{};
    due.QuadPart =
        (seconds + epochOffset) * ticksPerSecond;

    // 絶対UTC時刻。最後のTRUEはシステム復帰要求。
    if (!SetWaitableTimer(
        wakeTimer_,
        &due,
        0,
        nullptr,
        nullptr,
        TRUE)) {
        error = GetLastError();
        return false;
    }

    armedWakeTime_ = unixTime;
    error = ERROR_SUCCESS;
    return true;
}

bool SleepWakeManager::SetSleepBlock_(
    bool enabled) noexcept
{
    if (enabled == sleepBlockActive_) {
        return true;
    }

    const EXECUTION_STATE flags = enabled
        ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED
        : ES_CONTINUOUS;

    if (SetThreadExecutionState(flags) == 0) {
        return false;
    }

    sleepBlockActive_ = enabled;
    return true;
}

void SleepWakeManager::ResetWakeSession_() noexcept
{
    managedWake_ = false;
    activitySeen_ = false;
    userInputAfterWake_ = false;
    resleepAttempted_ = false;

    wakeTick_ = 0;
    lastBusyTick_ = 0;
    inputTickAtWake_ = 0;
}

void SleepWakeManager::NotifyTasksChanged() noexcept
{
    forceReload_ = true;
}

void SleepWakeManager::NotifyClockChanged() noexcept
{
    forceRearm_ = true;
    CancelWakeTimer_();
}

void SleepWakeManager::OnSuspending() noexcept
{
    if (!IsOwnerThread_()) return;

    armedAtSuspend_ = armedWakeTime_;
    ResetWakeSession_();

    // この段階で復帰タイマーは解除しない。
}

void SleepWakeManager::OnResumeAutomatic() noexcept
{
    if (!IsOwnerThread_()) return;

    const auto now = std::time(nullptr);

    const auto difference =
        static_cast<std::int64_t>(now) -
        static_cast<std::int64_t>(armedAtSuspend_);

    // 自身の登録時刻に近い自動復帰のみ管理対象とみなす。
    if (armedAtSuspend_ > 0 &&
        difference >= -180 &&
        difference <= 300) {
        managedWake_ = true;
        activitySeen_ = false;
        userInputAfterWake_ = false;
        resleepAttempted_ = false;

        wakeTick_ = GetTickCount64();
        lastBusyTick_ = 0;
        inputTickAtWake_ = LastInputTick_();
    }

    armedAtSuspend_ = 0;
    NotifyClockChanged();
}

void SleepWakeManager::OnResumeUserActivity() noexcept
{
    if (!IsOwnerThread_()) return;

    // RESUMESUSPENDは自動復帰後にも届く場合がある。
    if (managedWake_ &&
        inputTickAtWake_ != 0 &&
        LastInputTick_() != inputTickAtWake_) {
        userInputAfterWake_ = true;
    }

    NotifyClockChanged();
}

void SleepWakeManager::ConsiderResleep_(
    int action,
    const Activity& activity,
    std::time_t now) noexcept
{
    if (!managedWake_ ||
        action != 1 ||
        resleepAttempted_ ||
        userInputAfterWake_ ||
        !activitySeen_ ||
        activity.Busy() ||
        !activity.taskMonitorRunning ||
        !status_.taskSourceValid ||
        !status_.sleepBlockApiOk) {
        return;
    }

    const ULONGLONG tick = GetTickCount64();

    // 最後に実行中の処理を確認してから5分間は待つ。
    if (lastBusyTick_ == 0 ||
        tick - lastBusyTick_ < 300000ULL ||
        tick - wakeTick_ < 300000ULL) {
        return;
    }

    // 次回予約が近いときは再スリープしない。
    if (status_.nextDispatchLocal > now) {
        if (!status_.timerRegistered ||
            status_.wakeImmediatelyRequired ||
            status_.nextWakeLocal <= now + 1200) {
            return;
        }
    }

    const DWORD currentInput = LastInputTick_();

    if (inputTickAtWake_ == 0 || currentInput == 0) {
        return;
    }

    if (currentInput != inputTickAtWake_) {
        userInputAfterWake_ = true;
        return;
    }

    if (!SetSleepBlock_(false)) {
        status_.sleepBlockApiOk = false;
        return;
    }

    resleepAttempted_ = true;
    status_.resleepAttempted = true;
    status_.resleepPending = false;

    // 失敗しても連続スリープ要求しない。
    if (!SetSuspendState(FALSE, FALSE, FALSE)) {
        const DWORD error = GetLastError();
        status_.lastError =
            error != ERROR_SUCCESS
            ? error : ERROR_GEN_FAILURE;
    }
}

SleepWakeManager::Status SleepWakeManager::Refresh(
    const ConfigManager& config,
    const Activity& activity,
    const std::wstring& tasksIniPath)
{
    if (!IsOwnerThread_()) {
        Status result = status_;
        result.lastError = ERROR_INVALID_FUNCTION;
        result.sleepBlockApiOk = false;
        return result;
    }

    if (shutdown_) {
        Status result = status_;
        result.lastError = ERROR_INVALID_HANDLE;
        return result;
    }

    const bool enabled = config.GetSleepEnabled();

    const int leadMinutes = (std::clamp)(
        config.GetSleepWakeLeadMinutes(), 1, 120);

    const int action =
        static_cast<int>(config.GetSleepAfterWakeAction());

    const std::time_t now = std::time(nullptr);
    const ULONGLONG tick = GetTickCount64();

    Status next{};
    next.enabled = enabled;

    if (!enabled) {
        ResetWakeSession_();
    }

    if (enabled && activity.taskMonitorRunning) {
        bool reloaded = false;
        DWORD error = ERROR_SUCCESS;

        next.taskSourceValid =
            ReloadTasks_(
                tasksIniPath, reloaded, error);

        next.cacheReloaded = reloaded;
        next.usingLastGoodCache =
            !next.taskSourceValid && cacheValid_;

        next.cachedTaskCount = taskCache_.size();
        next.unsupportedScheduleCount = unsupportedCount_;
        next.lastError = error;

        if (next.taskSourceValid) {
            const NextTask candidate = FindNext_(now);

            next.nextDispatchLocal = candidate.dispatch;
            next.sourceSection = candidate.section;

            if (candidate.dispatch > now) {
                next.nextWakeLocal =
                    candidate.dispatch -
                    static_cast<std::time_t>(leadMinutes) * 60;

                if (next.nextWakeLocal <= now) {
                    next.wakeImmediatelyRequired = true;
                    CancelWakeTimer_();
                }
                else if (!forceRearm_ &&
                    armedWakeTime_ == next.nextWakeLocal) {
                    next.timerRegistered = true;
                }
                else {
                    DWORD armError = ERROR_SUCCESS;

                    if (ArmWakeTimer_(
                        next.nextWakeLocal, armError)) {
                        next.timerRegistered = true;
                    }
                    else {
                        CancelWakeTimer_();
                        next.lastError = armError;
                    }
                }
            }
            else {
                CancelWakeTimer_();
            }
        }
        else {
            // 読み込み失敗時に古い復帰タイマーを使い続けない。
            CancelWakeTimer_();
        }
    }
    else {
        CancelWakeTimer_();

        if (enabled && !activity.taskMonitorRunning) {
            next.lastError = ERROR_SERVICE_NOT_ACTIVE;
        }
    }

    if (next.timerRegistered ||
        next.wakeImmediatelyRequired ||
        !enabled) {
        forceRearm_ = false;
    }

    if (managedWake_) {
        if (activity.Busy()) {
            activitySeen_ = true;
            lastBusyTick_ = tick;
        }

        if (inputTickAtWake_ != 0 &&
            LastInputTick_() != inputTickAtWake_) {
            userInputAfterWake_ = true;
        }
    }

    // 予約処理開始を待つ自動復帰後の猶予時間：最大10分。
    const bool startupGrace =
        managedWake_ &&
        !activitySeen_ &&
        wakeTick_ > 0 &&
        tick - wakeTick_ < 600000ULL;

    const bool holdManaged =
        enabled && managedWake_ &&
        (action == 2 ||
            startupGrace ||
            (action == 1 &&
                activitySeen_ &&
                !resleepAttempted_ &&
                !userInputAfterWake_));

    const bool shouldBlock =
        activity.Busy() ||
        (enabled && next.wakeImmediatelyRequired) ||
        holdManaged;

    next.sleepBlockApiOk =
        SetSleepBlock_(shouldBlock);

    next.sleepBlocked = sleepBlockActive_;
    next.managedWake = managedWake_;
    next.resleepAttempted = resleepAttempted_;

    next.resleepPending =
        enabled && managedWake_ &&
        action == 1 &&
        activitySeen_ &&
        !resleepAttempted_ &&
        !userInputAfterWake_;

    status_ = next;

    if (enabled && action == 1) {
        ConsiderResleep_(action, activity, now);
        status_.sleepBlocked = sleepBlockActive_;
    }

    return status_;
}

std::wstring SleepWakeManager::FormatLocalTime(
    std::time_t value)
{
    if (value <= 0) return L"なし";

    std::tm t{};

    if (localtime_s(&t, &value) != 0) {
        return L"時刻変換失敗";
    }

    wchar_t buffer[64]{};

    _snwprintf_s(
        buffer,
        _countof(buffer),
        _TRUNCATE,
        L"%04d/%02d/%02d %02d:%02d:%02d",
        t.tm_year + 1900,
        t.tm_mon + 1,
        t.tm_mday,
        t.tm_hour,
        t.tm_min,
        t.tm_sec);

    return buffer;
}

void SleepWakeManager::Shutdown() noexcept
{
    if (!IsOwnerThread_() || shutdown_) return;

    (void)SetSleepBlock_(false);
    CancelWakeTimer_();

    if (wakeTimer_) {
        CloseHandle(wakeTimer_);
        wakeTimer_ = nullptr;
    }

    taskCache_.clear();
    taskFilePath_.clear();

    cacheValid_ = false;
    forceReload_ = true;
    forceRearm_ = true;

    ResetWakeSession_();
    status_ = {};
    shutdown_ = true;
}
