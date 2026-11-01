#pragma once
#ifndef RESERVATION_EXECUTION_MANAGER_HPP
#define RESERVATION_EXECUTION_MANAGER_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <Windows.h>

#include "Reservation_EitMonitor.hpp"
#include "Reservation_ExecutionSettings.hpp"
#include "Reservation_TvTestController.hpp"
#include "TimeKeeper.hpp"
#include "Reservation_RawTsRecorder.hpp"
#include "TaskListManager.hpp"
#include "RecordingEitAnalyzer.hpp"

class ConfigManager;
class IBonDriver2;
class TaskSystemConnector;

// tasks.iniの番組予約を実際の視聴/録画処理へ振り分ける実行管理ユニット。
// 現段階では視聴予約をTVTestプラグイン経由で実行し、録画予約は未実装ログに留める。
class Reservation_ExecutionManager final
{
public:
    struct ActiveState
    {
        bool prepareSeen = false;
        bool prepareSucceeded = false;
        bool preparedTvTestReady = false;
        bool preparedServiceSelected = false;

        bool startSeen = false;
        bool stopSeen = false;
        bool startSucceeded = false;
        bool stopSucceeded = false;
        bool anyFailure = false;

        std::wstring title;
        std::wstring serviceName;
        std::wstring reserveMode;
        std::wstring recordFilePath;

        bool internalRecordStarting = false;
        bool internalRecordRunning = false;
        bool internalRecordStartFailed = false;
        bool internalRecordStartAttempted = false;
        int internalRecordTunerIndex = -1;

        bool tvTestRecordStarting = false;
        bool tvTestRecordRunning = false;
        bool tvTestRecordStartFailed = false;
        bool tvTestRecordStartAttempted = false;
        int tvTestRecordTunerIndex = -1;
    };

public:
    Reservation_ExecutionManager() = default;

    Reservation_ExecutionManager(const Reservation_ExecutionManager&) = delete;
    Reservation_ExecutionManager& operator=(const Reservation_ExecutionManager&) = delete;

    void EnsureDefaultConfig(ConfigManager& cfg);
    void ConfigureFromConfig(ConfigManager& cfg);
    void Configure(const Reservation_ExecutionSettings& settings);

    void SetTaskSystem(TaskSystemConnector* taskSystem) noexcept;

    void OnRecPrepare(int tunerIndex, IBonDriver2* bon, const TimeKeeper::TaskRequest& req);
    void OnRecStart(int tunerIndex, IBonDriver2* bon, const TimeKeeper::TaskRequest& req);
    void OnRecStop(int tunerIndex, IBonDriver2* bon, const TimeKeeper::TaskRequest& req);

    void Reset() noexcept;

private:
    bool FindReservationEntry_(std::uint32_t instance, TaskListManager::TaskEntry& out) const;

    static bool IsWatch_(const TaskListManager::TaskEntry& entry) noexcept;
    static bool IsRecord_(const TaskListManager::TaskEntry& entry) noexcept;
    static std::wstring SafeTitle_(const TaskListManager::TaskEntry& entry);
    static std::wstring SafeService_(const TaskListManager::TaskEntry& entry);
    static std::wstring ModeLabel_(const TaskListManager::TaskEntry& entry);
    static void WriteSystemStartLog_(const TaskListManager::TaskEntry& entry,int tunerIndex,const Reservation_ExecutionSettings& settings);
    static void WriteSystemSelectedLog_(const TaskListManager::TaskEntry& entry);
    static void WriteSystemEndLog_(const TaskListManager::TaskEntry& entry, bool succeeded);
    static void WriteSystemFailLog_(const TaskListManager::TaskEntry& entry,const wchar_t* stage,const std::wstring& reason);

    Reservation_TvTestController BuildTvTestController_(
        int tunerIndex,
        bool recordLaunchMode = false) const;

    void UpdateActivePrepare_(const TaskListManager::TaskEntry& entry, const TimeKeeper::TaskRequest& req);
    void UpdateActiveStart_(const TaskListManager::TaskEntry& entry, const TimeKeeper::TaskRequest& req, bool succeeded);
    void UpdateActiveStop_(const TaskListManager::TaskEntry& entry, const TimeKeeper::TaskRequest& req, bool succeeded);
    void LogBothFailedIfNeeded_(std::uint32_t instance, const ActiveState& st) const;

    bool CompleteProgramReservationGroup_(std::uint32_t instance);

    void UpdateActivePrepareResult_(
        const TaskListManager::TaskEntry& entry,
        const TimeKeeper::TaskRequest& req,
        bool tvtestReady,
        bool serviceSelected);

    bool WasServicePrepared_(std::uint32_t instance) const;

    void SetActiveRecordFilePath_(
        std::uint32_t instance,
        const std::wstring& path);

    std::wstring GetActiveRecordFilePath_(
        std::uint32_t instance) const;

    static bool TryGetReserveStartTimeMs_(
        const TaskListManager::TaskEntry& entry,
        ULONGLONG& outStartTickMs,
        DWORD fallbackMaxMs) noexcept;

    bool ShouldKeepTvTestAliveAfterStop_(
        const TaskListManager::TaskEntry& currentEntry,
        const TimeKeeper::TaskRequest& req,
        int tunerIndex) const;

    bool HasNearbyFollowingProgramReservationOnSameTuner_(
        const TaskListManager::TaskEntry& currentEntry,
        const TimeKeeper::TaskRequest& req,
        int tunerIndex) const;

    bool StartRecordReservation_(
        const TaskListManager::TaskEntry& entry,
        const TimeKeeper::TaskRequest& req,
        int tunerIndex,
        IBonDriver2* bon);

    bool StopRecordReservation_(
        const TaskListManager::TaskEntry& entry,
        const TimeKeeper::TaskRequest& req,
        int tunerIndex);

    std::unordered_map<int, std::uint32_t> internalRecordOwnerByTuner_{};
    std::unordered_map<int, std::uint32_t> tvTestRecordOwnerByTuner_{};

private:
    mutable std::mutex mutex_{};
    Reservation_ExecutionSettings settings_{};
    TaskSystemConnector* taskSystem_ = nullptr;
    std::unordered_map<std::uint32_t, ActiveState> active_{};
    Reservation_EitMonitor eitMonitor_{};
    Reservation_RawTsRecorder rawTsRecorder_{};
    RecordingEitAnalyzer recordingEitAnalyzer_{};

    bool StartTvTestRecordReservation_(
        const TaskListManager::TaskEntry& entry,
        const TimeKeeper::TaskRequest& req,
        int tunerIndex,
        const std::filesystem::path& recordPath);

    bool StopTvTestRecordReservation_(
        const TaskListManager::TaskEntry& entry,
        const TimeKeeper::TaskRequest& req,
        int tunerIndex,
        const std::wstring& recordPath);

    bool StartInternalRecordReservation_(
        const TaskListManager::TaskEntry& entry,
        const TimeKeeper::TaskRequest& req,
        int tunerIndex,
        IBonDriver2* bon,
        const std::filesystem::path& recordPath);

    bool StopInternalRecordReservation_(
        const TaskListManager::TaskEntry& entry,
        const TimeKeeper::TaskRequest& req,
        int tunerIndex,
        const std::wstring& recordPath);

    bool IsInternalRecordStartingOrRunning_(std::uint32_t instance) const;

    bool IsInternalRecordOwner_(
        int tunerIndex,
        std::uint32_t instance) const;

    std::uint32_t GetInternalRecordOwnerOnTuner_(
        int tunerIndex) const;

    bool TryAcquireInternalRecordTuner_(
        int tunerIndex,
        std::uint32_t instance,
        std::uint32_t& busyInstance);

    void ReleaseInternalRecordTuner_(
        int tunerIndex,
        std::uint32_t instance) noexcept;

    bool StopInternalRecordOwnerForPreparePriority_(
        int tunerIndex,
        std::uint32_t requestInstance,
        const TaskListManager::TaskEntry& requestEntry);

    bool TryBeginInternalRecordStart_(
        std::uint32_t instance,
        int tunerIndex,
        const std::wstring& recordPath,
        bool& alreadyActive);

    void FinishInternalRecordStart_(
        std::uint32_t instance,
        bool succeeded) noexcept;

    void MarkInternalRecordStopped_(
        std::uint32_t instance,
        int tunerIndex) noexcept;

    bool IsTvTestRecordStartingOrRunning_(std::uint32_t instance) const;

    bool IsTvTestRecordOwner_(
        int tunerIndex,
        std::uint32_t instance) const;

    bool WasTvTestRecordStartFailed_(
        std::uint32_t instance) const;

    std::uint32_t GetTvTestRecordOwnerOnTuner_(int tunerIndex) const;

    bool TryAcquireTvTestRecordTuner_(
        int tunerIndex,
        std::uint32_t instance,
        std::uint32_t& busyInstance);

    void ReleaseTvTestRecordTuner_(
        int tunerIndex,
        std::uint32_t instance) noexcept;

    bool TryBeginTvTestRecordStart_(
        std::uint32_t instance,
        int tunerIndex,
        const std::wstring& recordPath,
        bool& alreadyActive);

    void FinishTvTestRecordStart_(
        std::uint32_t instance,
        bool succeeded) noexcept;

    void MarkTvTestRecordStopped_(
        std::uint32_t instance,
        int tunerIndex) noexcept;

    bool StopTvTestRecordOwnerForPreparePriority_(
        int tunerIndex,
        std::uint32_t requestInstance,
        const TaskListManager::TaskEntry& requestEntry);

    void StopTvTestRecordOnlyFromMonitor_(
        std::uint32_t instance,
        int tunerIndex,
        const TaskListManager::TaskEntry& entry,
        const std::wstring& recordPath) noexcept;

    void NotifyRecordingEitUpdated_(
        std::uint32_t instance,
        const RecordingEitAnalyzer::FlushResult& result) noexcept;



};

#endif // RESERVATION_EXECUTION_MANAGER_HPP
