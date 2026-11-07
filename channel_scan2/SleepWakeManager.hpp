#pragma once
#ifndef TSCEPG_SLEEP_WAKE_MANAGER_HPP
#define TSCEPG_SLEEP_WAKE_MANAGER_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <cstddef>
#include <ctime>
#include <string>
#include <vector>

class ConfigManager;

// UIスレッド専用。BonDriver・録画worker・タイムシフトworkerには干渉しない。
// 録画・TS受信処理の寿命はMainUI側で管理する。
class SleepWakeManager final {
public:
    struct Activity {
        bool recording = false;
        bool epg = false;
        bool timeShift = false;
        bool channelScan = false;
        bool otherTuner = false;
        bool taskMonitorRunning = true;

        [[nodiscard]] bool Busy() const noexcept {
            return recording || epg || timeShift ||
                channelScan || otherTuner;
        }
    };

    struct Status {
        bool enabled = false;
        bool taskSourceValid = false;
        bool timerRegistered = false;
        bool wakeImmediatelyRequired = false;
        bool cacheReloaded = false;
        bool usingLastGoodCache = false;
        bool sleepBlocked = false;
        bool sleepBlockApiOk = true;
        bool managedWake = false;
        bool resleepPending = false;
        bool resleepAttempted = false;

        DWORD lastError = ERROR_SUCCESS;

        std::time_t nextDispatchLocal = 0;
        std::time_t nextWakeLocal = 0;
        std::wstring sourceSection;

        std::size_t cachedTaskCount = 0;
        unsigned int unsupportedScheduleCount = 0;
    };

    SleepWakeManager() noexcept;
    ~SleepWakeManager() noexcept;

    SleepWakeManager(const SleepWakeManager&) = delete;
    SleepWakeManager& operator=(const SleepWakeManager&) = delete;

    Status Refresh(
        const ConfigManager& config,
        const Activity& activity,
        const std::wstring& tasksIniPath);

    void NotifyTasksChanged() noexcept;
    void NotifyClockChanged() noexcept;
    void OnSuspending() noexcept;
    void OnResumeAutomatic() noexcept;
    void OnResumeUserActivity() noexcept;

    // 連続受信・録画workerなどの停止完了後、UIスレッドで明示的に呼ぶ。
    void Shutdown() noexcept;

    [[nodiscard]] Status GetStatus() const {
        return status_;
    }

    static std::wstring FormatLocalTime(std::time_t value);

private:
    struct FileStamp {
        FILETIME writeTime{};
        DWORD sizeHigh = 0;
        DWORD sizeLow = 0;
    };

    // 排他的な「内部」スケジュール種別。
    // 1/2/4はtasks.iniでの外部保存値であり、ビットフラグではない。
    // C26813対策として内部値を0/1/2にする。
    enum class ScheduleKind : int {
        Daily = 0,
        Hourly = 1,
        Once = 2
    };

    struct TaskEntry {
        std::wstring section;
        ScheduleKind kind = ScheduleKind::Once;

        int ymd = 0;
        int hhmm = 0;
        int second = 0;
        int dailyHour = 0;
        int dailyMinute = 0;
        int hourlyMinute = 0;
    };

    struct NextTask {
        std::time_t dispatch = 0;
        std::wstring section;
    };

    static bool ReadStamp_(
        const std::wstring& path,
        FileStamp& stamp,
        DWORD& error) noexcept;

    static bool SameStamp_(
        const FileStamp& left,
        const FileStamp& right) noexcept;

    bool ReloadTasks_(
        const std::wstring& path,
        bool& reloaded,
        DWORD& error);

    bool ParseTasks_(
        const std::wstring& path,
        const FileStamp& before,
        std::vector<TaskEntry>& entries,
        unsigned int& unsupported,
        DWORD& error);

    static std::time_t DispatchTime_(
        const TaskEntry& task,
        std::time_t now) noexcept;

    [[nodiscard]] NextTask FindNext_(
        std::time_t now) const;

    bool ArmWakeTimer_(
        std::time_t unixTime,
        DWORD& error) noexcept;

    void CancelWakeTimer_() noexcept;
    bool SetSleepBlock_(bool enabled) noexcept;
    void ResetWakeSession_() noexcept;

    void ConsiderResleep_(
        int afterWakeAction,
        const Activity& activity,
        std::time_t now) noexcept;

    [[nodiscard]] bool IsOwnerThread_() const noexcept;

    DWORD ownerThreadId_ = 0;
    HANDLE wakeTimer_ = nullptr;
    std::time_t armedWakeTime_ = 0;

    FileStamp fileStamp_{};
    std::wstring taskFilePath_;
    std::vector<TaskEntry> taskCache_;

    bool cacheValid_ = false;
    bool forceReload_ = true;
    bool forceRearm_ = true;
    bool sleepBlockActive_ = false;
    bool shutdown_ = false;

    unsigned int unsupportedCount_ = 0;

    std::time_t armedAtSuspend_ = 0;
    bool managedWake_ = false;
    bool activitySeen_ = false;
    bool userInputAfterWake_ = false;
    bool resleepAttempted_ = false;

    ULONGLONG wakeTick_ = 0;
    ULONGLONG lastBusyTick_ = 0;
    DWORD inputTickAtWake_ = 0;

    Status status_{};
};

#endif // TSCEPG_SLEEP_WAKE_MANAGER_HPP
