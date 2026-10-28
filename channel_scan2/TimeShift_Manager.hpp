#pragma once
#ifndef TIMESHIFT_MANAGER_HPP
#define TIMESHIFT_MANAGER_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "TimeShift_SegmentRecorder.hpp"

class ConfigManager;

class TimeShift_Manager final
{
public:
    struct Settings
    {
        std::filesystem::path rootFolder = L"timeshift";

        std::uint64_t maxBytesPerChannel =
            8ull * 1024ull * 1024ull * 1024ull;

        int highWatermarkPercent = 80;
        int lowWatermarkPercent = 70;

        std::uint32_t segmentSeconds = 60;
        std::uint64_t segmentMaxBytes =
            256ull * 1024ull * 1024ull;

        std::uint64_t writerQueueBytes =
            64ull * 1024ull * 1024ull;

        bool dropOldestWhenFull = true;
    };

    struct Target
    {
        int tunerIndex = -1;

        int space = 0;
        int channel = -1;

        std::uint16_t serviceId = 0;
        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;

        std::wstring serviceName;

        bool enabled = false;
    };

    struct Status
    {
        bool running = false;

        Target target{};

        TimeShift_SegmentRecorder::Status recorderStatus{};
    };

public:
    TimeShift_Manager();
    ~TimeShift_Manager() noexcept;

    TimeShift_Manager(const TimeShift_Manager&) = delete;
    TimeShift_Manager& operator=(const TimeShift_Manager&) = delete;

    void Configure(const Settings& settings);

    // ConfigManager から UseRole=TimeShiftOnly のチューナーを拾う。
    bool LoadTargetsFromConfig(const ConfigManager& cfg);

    bool StartAll();
    void StopAll() noexcept;
    void ResetAll() noexcept;

    bool StartTuner(int tunerIndex);
    void StopTuner(int tunerIndex) noexcept;

    bool IsRunning(int tunerIndex) const noexcept;

    // TS受信側から呼ぶ入口。
    bool PushBytesForTuner(
        int tunerIndex,
        const void* data,
        std::size_t bytes) noexcept;

    bool PushBytesForTuner(
        int tunerIndex,
        const std::vector<std::uint8_t>& bytes) noexcept;

    std::vector<Target> GetTargets() const;
    std::vector<Status> GetStatuses() const;

    bool HasTarget(int tunerIndex) const noexcept;

private:
    static bool IsValidTarget_(const Target& target) noexcept;

    TimeShift_SegmentRecorder::StartRequest
        MakeRecorderStartRequest_(const Target& target) const;

private:
    mutable std::mutex mutex_{};

    Settings settings_{};

    std::unordered_map<int, Target> targets_{};
    std::unordered_map<int, std::unique_ptr<TimeShift_SegmentRecorder>> recorders_{};
};

#endif // TIMESHIFT_MANAGER_HPP