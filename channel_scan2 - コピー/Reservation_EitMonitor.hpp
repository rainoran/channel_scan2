#pragma once
#ifndef RESERVATION_EIT_MONITOR_HPP
#define RESERVATION_EIT_MONITOR_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "Reservation_ExecutionSettings.hpp"
#include "Reservation_TvTestController.hpp"
#include "TaskListManager.hpp"

class TaskSystemConnector;

class Reservation_EitMonitor final
{
public:
    struct Snapshot
    {
        bool ok = false;
        std::wstring rawLine;

        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;
        std::uint16_t serviceId = 0;

        std::uint16_t eventIdNow = 0;
        std::uint16_t eventIdNext = 0;

        std::wstring eventNameNow;
        std::wstring eventNameNext;

        // TVTest側 GET_PROGRAM が返せる場合だけ入る。
        // 推奨形式:
        // start_now=2026-06-04T19:00:00
        // end_now=2026-06-04T19:30:00
        // duration_now=1800
        std::wstring startDateNow;
        std::wstring startTimeNow;
        std::wstring endDateNow;
        std::wstring endTimeNow;
        std::uint32_t durationSecNow = 0;

        std::wstring startDateNext;
        std::wstring startTimeNext;
        std::wstring endDateNext;
        std::wstring endTimeNext;
        std::uint32_t durationSecNext = 0;
    };

    using QueryCallback = std::function<Snapshot()>;
    using NeedStopCallback = std::function<void(std::uint32_t)>;

public:
    Reservation_EitMonitor() = default;
    ~Reservation_EitMonitor() noexcept;

    Reservation_EitMonitor(const Reservation_EitMonitor&) = delete;
    Reservation_EitMonitor& operator=(const Reservation_EitMonitor&) = delete;

    void Configure(const Reservation_ExecutionSettings& settings);
    void Reset() noexcept;

    void StartWatchMonitor(
        std::uint32_t instance,
        const TaskListManager::TaskEntry& entry,
        Reservation_TvTestController& controller);

    void StartWatchMonitor(
        std::uint32_t instance,
        const TaskListManager::TaskEntry& entry,
        Reservation_TvTestController& controller,
        TaskSystemConnector* taskSystem);

    void StopWatchMonitor(std::uint32_t instance);

    // TVTest録画中の番組照合。
    // QueryCallback は ExecutionManager 側で TVTest Controller を構築して QueryOnce する。
    // ここでは TVTest / BonDriver の所有権を持たない。
    void StartRecordMonitor(
        std::uint32_t instance,
        const TaskListManager::TaskEntry& entry,
        QueryCallback query,
        TaskSystemConnector* taskSystem,
        NeedStopCallback onNeedStop);

    void StopRecordMonitor(std::uint32_t instance);

    Snapshot QueryOnce(Reservation_TvTestController& controller) const;

private:
    struct RecordSession
    {
        std::atomic_bool stopRequested{ false };
        std::atomic_bool stopIssuedByMonitor{ false };

        std::atomic_bool nowAdjustLogged{ false };
        std::atomic_bool nextShortDelayLogged{ false };
        std::atomic_bool nextShortDelayAdjusted{ false };
        std::atomic_bool nextLongDelayLogged{ false };
        std::atomic_bool nextLongDelayAdjusted{ false };
        std::atomic_bool mismatchLogged{ false };

        // 同一録画セッション中、補正済み後のskipログを連発させないためのカウンタ。
        std::atomic_uint32_t nowAdjustSkipCount{ 0 };
        std::atomic_uint32_t nextShortDelayAdjustSkipCount{ 0 };
        std::atomic_uint32_t nextLongDelayAdjustSkipCount{ 0 };
        std::atomic_bool serviceMismatchLogged{ false };
        std::atomic_bool queryFailureLogged{ false };

        std::uint32_t instance = 0;
        TaskListManager::TaskEntry entry{};

        QueryCallback query;
        TaskSystemConnector* taskSystem = nullptr;
        NeedStopCallback onNeedStop;

        std::thread worker;
    };

private:
    void CompareAndLog_(
        std::uint32_t instance,
        const TaskListManager::TaskEntry& entry,
        const Snapshot& snap,
        bool recordMode,
        TaskSystemConnector* taskSystem,
        const NeedStopCallback& onNeedStop,
        std::atomic_bool* stopIssued,
        RecordSession* recordSession) const;

    void RecordLoop_(const std::shared_ptr<RecordSession>& session) noexcept;

private:
    mutable std::mutex mutex_{};
    Reservation_ExecutionSettings settings_{};

    std::unordered_map<std::uint32_t, std::shared_ptr<RecordSession>> recordSessions_{};
};

#endif // RESERVATION_EIT_MONITOR_HPP