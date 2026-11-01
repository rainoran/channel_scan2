#pragma once
#ifndef MULTI_TUNER_TASK_RUNNER_HPP
#define MULTI_TUNER_TASK_RUNNER_HPP

#include <Windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <vector>

#include "TimeKeeper.hpp"

class IBonDriver2;

class MultiTunerTaskRunner final
{
public:
    struct Settings {
        int tunerCount = 1;
        bool enableUtilityWorker = true; // requiredTuners==0 タスク用
        int maxQueueSize = 256;          // 異常時の暴走防止
    };

    struct Hooks {
        std::function<void(const std::wstring&)> Log;
        std::function<bool()> CanRun;
    };

    using TaskHandler = std::function<void(int tunerIndex, IBonDriver2* bon, const TimeKeeper::TaskRequest& req)>;

public:
    MultiTunerTaskRunner() = default;
    ~MultiTunerTaskRunner();

    bool Initialize(const Settings& s, const Hooks& h);
    void SetTuner(int tunerIndex, IBonDriver2* bon);
    void SetTunerAvailable(int tunerIndex, bool available);

    bool RegisterHandler(std::uint32_t kind, TaskHandler handler);

    bool Start();
    void Stop(bool clearQueue = false);

    bool Enqueue(const TimeKeeper::TaskRequest& req);

    int GetTunerCount() const { return tunerCount_; }
    bool IsRunning() const { return running_.load(); }

private:
    static std::uint64_t MakeKey_(std::uint32_t kind, std::uint32_t instance);

    void WorkerLoopTuner_(int tunerIndex);
    void WorkerLoopUtility_();

    bool PopNextForTuner_(int tunerIndex, TimeKeeper::TaskRequest& out);
    bool PopNextForUtility_(TimeKeeper::TaskRequest& out);

    int TotalQueueSizeUnsafe_() const;
    TaskHandler FindHandler_(std::uint32_t kind) const;
    void CompleteKey_(std::uint64_t key) noexcept;

    void Log_(const std::wstring& msg) const;

private:
    Settings settings_{};
    Hooks hooks_{};

    int tunerCount_ = 0;
    std::vector<IBonDriver2*> tuners_;
    std::vector<bool> tunerAvailable_;

    std::unordered_map<std::uint32_t, TaskHandler> handlers_;

    mutable std::mutex mtx_;
    std::condition_variable cv_;

    // ★変更：tuner専用キュー + 任意キュー
    std::vector<std::deque<TimeKeeper::TaskRequest>> qTunerPer_;
    std::deque<TimeKeeper::TaskRequest> qTunerAny_;

    // utility
    std::deque<TimeKeeper::TaskRequest> qUtility_;

    // coalesce
    std::unordered_set<std::uint64_t> pendingOrRunning_;

    std::atomic<bool> running_{ false };
    std::vector<std::thread> tunerWorkers_;
    std::thread utilityWorker_;
};

#endif // MULTI_TUNER_TASK_RUNNER_HPP
