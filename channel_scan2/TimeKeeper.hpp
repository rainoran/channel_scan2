#pragma once
#ifndef TIME_KEEPER_HPP
#define TIME_KEEPER_HPP

#include <Windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class TimeKeeper final
{
public:
    struct DailyTime {
        int hour = 3;
        int minute = 30;
    };

    enum class TaskPriority : std::uint8_t {
        Soft = 0,
        Hard = 1,
    };

    struct TaskRequest {
        std::uint32_t kind = 0;
        std::uint32_t instance = 0;
        TaskPriority priority = TaskPriority::Soft;
        std::uint8_t requiredTuners = 1;
        int preferredTunerIndex = -1;
        // 分単位スケジューラで起動後、実際に処理を走らせる絶対時刻
        // 0 の場合は秒待機なし
        int dispatchYmd = 0;
        int dispatchHHMM = 0;
        int dispatchSecond = 0; // 0..59
    };

    struct Settings {
        bool enableAuto = false;
        int bootGraceSeconds = 60;
        bool catchUpOnStart = false;

        bool enableDailyFull = true;
        DailyTime dailyFullAt{ 3, 30 };

        bool enableHourly = true;
        int hourlyAtMinute = 5;
        int lightIntervalMinutes = 0;

        int tickSeconds = 5;
        int idleRequiredSeconds = 0;
        int enqueueRetryBackoffSeconds = 30;
        std::uint8_t epgRequiredTuners = 1;
    };

    struct Hooks {
        std::function<bool()> CanRun;
        std::function<void(const std::wstring&)> Log;
        std::function<bool(const TaskRequest&)> EnqueueTask;

        // 追加: 期限切れ / 窓切れログ用
        std::function<void(const TaskRequest&, const wchar_t* reason)> OnTaskMissed;

        std::function<void()> StartLightUpdate;
        std::function<void()> StartFullUpdate;
    };

    void Configure(const Settings& s, const Hooks& h);

    bool Start(HWND hwnd, UINT_PTR timerId);
    void Stop(HWND hwnd);

    void OnTick();

    void ClearOneShotTasks();

    void AddOneShotTask(
        int dueYmd,
        int dueHHMM,
        const TaskRequest& req,
        int lateToleranceMinutes = -1,
        bool useTimeWindow = false,
        int windowStartYmd = 0,
        int windowStartHHMM = 0,
        int windowEndYmd = 0,
        int windowEndHHMM = 0);

    bool CancelOneShotTask(std::uint32_t kind, std::uint32_t instance);

private:
    struct OneShotTask {
        int dueYmd = 0;
        int dueHHMM = 0;
        int lateToleranceMinutes = -1;

        bool useTimeWindow = false;
        int windowStartYmd = 0;
        int windowStartHHMM = 0;
        int windowEndYmd = 0;
        int windowEndHHMM = 0;

        TaskRequest req{};
        ULONGLONG lastAttemptTick = 0;
        bool firedOrMissed = false;
    };

private:
    bool IsIdleEnough_() const;
    static void GetNow_(int& outYmd, int& outHour, int& outMin, int& outHHMM);

    bool Dispatch_(const TaskRequest& req, ULONGLONG& inoutLastAttemptTick, const wchar_t* logTag);
    static bool IsWithinBackoff_(ULONGLONG nowTick, ULONGLONG lastAttemptTick, int backoffSeconds);

    static bool LocalYmdHHMMToUtcFileTime_(int ymd, int hhmm, FILETIME& outUtcFt);
    static ULONGLONG FileTimeToULL_(const FILETIME& ft);
    static int DiffMinutesUtcFileTime_(const FILETIME& a, const FILETIME& b);
    static int CompareLocalYmdHHMM_(int ymd1, int hhmm1, int ymd2, int hhmm2);

private:
    Settings settings_{};
    Hooks hooks_{};

    int lastFullYmd_ = 0;
    ULONGLONG lastFullAttemptTick_ = 0;

    ULONGLONG lastLightTick_ = 0;
    ULONGLONG lastLightAttemptTick_ = 0;

    int lastLightHourKey_ = 0;
    ULONGLONG startTick_ = 0;

    bool started_ = false;
    UINT_PTR timerId_ = 0;
    HWND hwnd_ = nullptr;

    std::vector<OneShotTask> oneShots_;
};

#endif // TIME_KEEPER_HPP