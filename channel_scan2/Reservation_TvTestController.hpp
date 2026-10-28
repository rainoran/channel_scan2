#pragma once
#ifndef RESERVATION_TVTEST_CONTROLLER_HPP
#define RESERVATION_TVTEST_CONTROLLER_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <string>

#include "Reservation_ExecutionSettings.hpp"
#include "TaskListManager.hpp"
#include "TVTestIntegration.hpp"

class ConfigManager;

// 予約実行からTVTestプラグインを裏で自動操作する実体ユニット。
// 既存のTVTestIntegrationを利用し、予約実行側からはこのControllerだけを見る。
class Reservation_TvTestController final
{
public:
    struct Result
    {
        bool ok = false;
        std::wstring response;
        std::wstring reason;
    };

public:
    Reservation_TvTestController() = default;

    void Configure(const Reservation_ExecutionSettings& settings);
    void SetRecordLaunchMode(bool recordLaunchMode) noexcept;
    bool ConfigureForTuner(ConfigManager& cfg, int tunerIndex);

    bool IsEnabled() const noexcept;

    Result EnsureReady();
    Result ApplyViewOptions();
    Result SelectReservationService(const TaskListManager::TaskEntry& entry);
    Result QueryStatus();
    Result QueryProgram();
    Result EnableTs(bool enable);
    Result StartRecord(const std::wstring& outputPath);
    Result StopRecord();
    Result CloseTvTest();
    Result SendRawCommand(const std::wstring& command);

    Result WaitReservationServiceReady(
        const TaskListManager::TaskEntry& entry,
        DWORD timeoutMs = 6000,
        DWORD intervalMs = 300);


private:
    Result Fail_(std::wstring reason) const;
    Result Ok_(std::wstring response = {}) const;
    std::wstring BuildReservationChannelCommand_(const TaskListManager::TaskEntry& entry) const;


    Reservation_ExecutionSettings settings_{};
    TVTestIntegration tvtest_{};
    int tunerIndex_ = -1;
    bool recordLaunchMode_ = false;

    Result ValidateReservationServiceState_(
        const TaskListManager::TaskEntry& entry,
        const std::wstring& stateText) const;

};

#endif // RESERVATION_TVTEST_CONTROLLER_HPP
