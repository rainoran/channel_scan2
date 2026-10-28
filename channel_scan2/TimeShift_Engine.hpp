
#pragma once
#ifndef TIMESHIFT_ENGINE_HPP
#define TIMESHIFT_ENGINE_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "TimeShift_Manager.hpp"
#include "TunerPortManager.hpp"
#include "TsStreamTypes.hpp"

class BonDriverLoader;
class ConfigManager;
class IBonDriver2;

class TimeShift_Engine final
{
public:
    struct Settings
    {
        TimeShift_Manager::Settings managerSettings{};

        bool waitForSignalLock = true;
        int signalLockTimeoutMs = 5000;

        DWORD settleSleepMs = 300;
        int discardInitialBlocks = 3;

        DWORD idleSleepMs = 5;
        DWORD noDataTimeoutMs = 30000;

        std::uint32_t statusLogIntervalMs = 30000;
    };

    struct TunerStatus
    {
        bool active = false;
        bool workerRunning = false;
        bool failed = false;

        int tunerIndex = -1;
        int space = 0;
        int channel = -1;

        std::wstring bonDriverPath;
        std::wstring serviceName;
        std::wstring reason;

        std::uint64_t receiveBlocks = 0;
        std::uint64_t receivePackets = 0;
        std::uint64_t receiveBytes = 0;
        std::uint64_t receiveFailures = 0;
        std::uint64_t pushFailures = 0;

        TimeShift_SegmentRecorder::Status recorderStatus{};
    };

public:
    TimeShift_Engine();
    ~TimeShift_Engine() noexcept;

    TimeShift_Engine(const TimeShift_Engine&) = delete;
    TimeShift_Engine& operator=(const TimeShift_Engine&) = delete;

    void Configure(const Settings& settings);
    void SetTunerPortManager(TunerPortManager* manager) noexcept;

    // ConfigManager から TimeShiftOnly チューナーを拾い、
    // BonDriverロード、OpenTuner、SetChannel、受信スレッド起動まで行う。
    bool StartFromConfig(const ConfigManager& cfg);

    void StopAll() noexcept;
    void ResetAll() noexcept;

    bool StartTunerFromConfig(const ConfigManager& cfg, int tunerIndex);
    void StopTuner(int tunerIndex) noexcept;

    bool IsRunning(int tunerIndex) const noexcept;

    std::vector<TunerStatus> GetStatuses() const;

private:
    struct Session
    {
        std::atomic_bool stopRequested{ false };
        std::atomic_bool workerRunning{ false };
        std::atomic_bool failed{ false };

        int tunerIndex = -1;
        int space = 0;
        int channel = -1;

        std::uint16_t serviceId = 0;
        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;

        std::wstring serviceName;
        std::wstring bonDriverPath;
        std::wstring reason;

        std::unique_ptr<BonDriverLoader> loader;
        TunerPortManager::Lease tunerLease{};
        IBonDriver2* bon = nullptr;

        std::thread worker;

        mutable std::mutex statMutex;
        std::uint64_t receiveBlocks = 0;
        std::uint64_t receivePackets = 0;
        std::uint64_t receiveBytes = 0;
        std::uint64_t receiveFailures = 0;
        std::uint64_t pushFailures = 0;

        ULONGLONG lastDataTickMs = 0;
        ULONGLONG lastStatusLogTickMs = 0;
    };

private:
    bool BuildSessionFromConfig_(
        const ConfigManager& cfg,
        int tunerIndex,
        std::shared_ptr<Session>& outSession,
        std::wstring& reason) const;

    bool PrepareBonDriver_(const std::shared_ptr<Session>& session) noexcept;

    void Worker_(const std::shared_ptr<Session>& session) noexcept;

    static std::uint64_t PacketsToBytes_(
        const std::vector<TsPacket>& packets,
        std::vector<std::uint8_t>& outBytes);

    static void AddReceiveFailure_(const std::shared_ptr<Session>& session) noexcept;
    static void AddPushFailure_(const std::shared_ptr<Session>& session) noexcept;

    static void AddReceiveStats_(
        const std::shared_ptr<Session>& session,
        std::uint64_t blocks,
        std::uint64_t packets,
        std::uint64_t bytes) noexcept;

    static void SetFailure_(
        const std::shared_ptr<Session>& session,
        const std::wstring& reason) noexcept;

private:
    mutable std::mutex mutex_{};
    Settings settings_{};

    TimeShift_Manager manager_{};
    TunerPortManager* tunerPortManager_ = nullptr;

    std::unordered_map<int, std::shared_ptr<Session>> sessions_{};
};

#endif // TIMESHIFT_ENGINE_HPP
