#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "TimeShift_Manager.hpp"

#include "ConfigManager.h"
#include "DebugManager.h"

#include <algorithm>
#include <utility>

TimeShift_Manager::TimeShift_Manager() = default;

TimeShift_Manager::~TimeShift_Manager() noexcept
{
    StopAll();
}

void TimeShift_Manager::Configure(const Settings& settings)
{
    std::lock_guard<std::mutex> lock(mutex_);

    settings_ = settings;

    if (settings_.rootFolder.empty()) {
        settings_.rootFolder = L"timeshift";
    }

    if (settings_.maxBytesPerChannel < 64ull * 1024ull * 1024ull) {
        settings_.maxBytesPerChannel = 64ull * 1024ull * 1024ull;
    }

    if (settings_.highWatermarkPercent < 10) {
        settings_.highWatermarkPercent = 10;
    }
    if (settings_.highWatermarkPercent > 95) {
        settings_.highWatermarkPercent = 95;
    }

    if (settings_.lowWatermarkPercent < 1) {
        settings_.lowWatermarkPercent = 1;
    }
    if (settings_.lowWatermarkPercent >= settings_.highWatermarkPercent) {
        settings_.lowWatermarkPercent = settings_.highWatermarkPercent - 1;
    }

    if (settings_.segmentSeconds == 0) {
        settings_.segmentSeconds = 60;
    }

    if (settings_.segmentMaxBytes < 16ull * 1024ull * 1024ull) {
        settings_.segmentMaxBytes = 16ull * 1024ull * 1024ull;
    }

    if (settings_.writerQueueBytes < 4ull * 1024ull * 1024ull) {
        settings_.writerQueueBytes = 4ull * 1024ull * 1024ull;
    }
}

bool TimeShift_Manager::LoadTargetsFromConfig(const ConfigManager& cfg)
{
    std::lock_guard<std::mutex> lock(mutex_);

    targets_.clear();

    int count = cfg.GetTunersCount();
    if (count < 0) {
        count = 0;
    }

    for (int i = 0; i < count; ++i) {
        Target t{};
        t.tunerIndex = i;
        t.enabled = cfg.GetTunerEnabled(i);

        const auto role = cfg.GetTunerUseRole(i);
        if (role != ConfigManager::TunerUseRole::TimeShiftOnly) {
            continue;
        }

        t.space = cfg.GetTunerTimeShiftLockSpace(i);
        t.channel = cfg.GetTunerTimeShiftLockChannel(i);
        t.serviceId = cfg.GetTunerTimeShiftLockServiceId(i);
        t.networkId = cfg.GetTunerTimeShiftLockNetworkId(i);
        t.tsId = cfg.GetTunerTimeShiftLockTsId(i);
        t.serviceName = cfg.GetTunerTimeShiftLockName(i);

        if (!IsValidTarget_(t)) {
            DebugLogFormat(
                L"[TimeShift][Manager] skip invalid target tuner=%d enabled=%d space=%d ch=%d name=%ls",
                i,
                t.enabled ? 1 : 0,
                t.space,
                t.channel,
                t.serviceName.c_str());
            continue;
        }

        targets_[i] = t;

        DebugLogFormat(
            L"[TimeShift][Manager] target loaded tuner=%d space=%d ch=%d sid=%u nid=%u tsid=%u name=%ls",
            i,
            t.space,
            t.channel,
            static_cast<unsigned>(t.serviceId),
            static_cast<unsigned>(t.networkId),
            static_cast<unsigned>(t.tsId),
            t.serviceName.c_str());
    }

    return true;
}

bool TimeShift_Manager::StartAll()
{
    std::vector<int> tunerIndexes;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        tunerIndexes.reserve(targets_.size());

        for (const auto& pair : targets_) {
            tunerIndexes.push_back(pair.first);
        }
    }

    bool allOk = true;

    for (int tunerIndex : tunerIndexes) {
        if (!StartTuner(tunerIndex)) {
            allOk = false;
        }
    }

    return allOk;
}

void TimeShift_Manager::StopAll() noexcept
{
    std::unordered_map<int, std::unique_ptr<TimeShift_SegmentRecorder>> local;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        local.swap(recorders_);
    }

    for (auto& pair : local) {
        if (pair.second) {
            pair.second->Stop();
        }
    }
}

void TimeShift_Manager::ResetAll() noexcept
{
    StopAll();

    std::lock_guard<std::mutex> lock(mutex_);
    recorders_.clear();
}

bool TimeShift_Manager::StartTuner(int tunerIndex)
{
    Target target{};

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = targets_.find(tunerIndex);
        if (it == targets_.end()) {
            return false;
        }

        target = it->second;

        auto recIt = recorders_.find(tunerIndex);
        if (recIt != recorders_.end() && recIt->second && recIt->second->IsRunning()) {
            return true;
        }
    }

    if (!IsValidTarget_(target)) {
        return false;
    }

    auto recorder = std::make_unique<TimeShift_SegmentRecorder>();
    auto req = MakeRecorderStartRequest_(target);

    if (!recorder->Start(req)) {
        DebugLogFormat(
            L"[TimeShift][Manager] recorder start failed tuner=%d space=%d ch=%d",
            target.tunerIndex,
            target.space,
            target.channel);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        recorders_[tunerIndex] = std::move(recorder);
    }

    DebugLogFormat(
        L"[TimeShift][Manager] recorder started tuner=%d space=%d ch=%d name=%ls",
        target.tunerIndex,
        target.space,
        target.channel,
        target.serviceName.c_str());

    return true;
}

void TimeShift_Manager::StopTuner(int tunerIndex) noexcept
{
    std::unique_ptr<TimeShift_SegmentRecorder> recorder;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = recorders_.find(tunerIndex);
        if (it == recorders_.end()) {
            return;
        }

        recorder = std::move(it->second);
        recorders_.erase(it);
    }

    if (recorder) {
        recorder->Stop();
    }

    DebugLogFormat(
        L"[TimeShift][Manager] recorder stopped tuner=%d",
        tunerIndex);
}

bool TimeShift_Manager::IsRunning(int tunerIndex) const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = recorders_.find(tunerIndex);
    if (it == recorders_.end() || !it->second) {
        return false;
    }

    return it->second->IsRunning();
}

bool TimeShift_Manager::PushBytesForTuner(
    int tunerIndex,
    const void* data,
    std::size_t bytes) noexcept
{
    TimeShift_SegmentRecorder* recorder = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = recorders_.find(tunerIndex);
        if (it == recorders_.end() || !it->second) {
            return false;
        }

        recorder = it->second.get();
    }

    return recorder->PushBytes(data, bytes);
}

bool TimeShift_Manager::PushBytesForTuner(
    int tunerIndex,
    const std::vector<std::uint8_t>& bytes) noexcept
{
    if (bytes.empty()) {
        return false;
    }

    return PushBytesForTuner(tunerIndex, bytes.data(), bytes.size());
}

std::vector<TimeShift_Manager::Target>
TimeShift_Manager::GetTargets() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Target> out;
    out.reserve(targets_.size());

    for (const auto& pair : targets_) {
        out.push_back(pair.second);
    }

    std::sort(
        out.begin(),
        out.end(),
        [](const Target& a, const Target& b)
        {
            return a.tunerIndex < b.tunerIndex;
        });

    return out;
}

std::vector<TimeShift_Manager::Status>
TimeShift_Manager::GetStatuses() const
{
    std::vector<std::pair<int, TimeShift_SegmentRecorder*>> recs;
    std::unordered_map<int, Target> targetsCopy;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        targetsCopy = targets_;

        recs.reserve(recorders_.size());
        for (const auto& pair : recorders_) {
            if (pair.second) {
                recs.push_back({ pair.first, pair.second.get() });
            }
        }
    }

    std::vector<Status> out;
    out.reserve(recs.size());

    for (const auto& pair : recs) {
        Status st{};

        auto targetIt = targetsCopy.find(pair.first);
        if (targetIt != targetsCopy.end()) {
            st.target = targetIt->second;
        }

        if (pair.second) {
            st.recorderStatus = pair.second->GetStatus();
            st.running = st.recorderStatus.running;
        }

        out.push_back(std::move(st));
    }

    std::sort(
        out.begin(),
        out.end(),
        [](const Status& a, const Status& b)
        {
            return a.target.tunerIndex < b.target.tunerIndex;
        });

    return out;
}

bool TimeShift_Manager::HasTarget(int tunerIndex) const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return targets_.find(tunerIndex) != targets_.end();
}

bool TimeShift_Manager::IsValidTarget_(const Target& target) noexcept
{
    if (!target.enabled) {
        return false;
    }

    if (target.tunerIndex < 0) {
        return false;
    }

    if (target.space < 0) {
        return false;
    }

    if (target.channel < 0) {
        return false;
    }

    return true;
}

TimeShift_SegmentRecorder::StartRequest
TimeShift_Manager::MakeRecorderStartRequest_(const Target& target) const
{
    TimeShift_SegmentRecorder::StartRequest req{};

    req.tunerIndex = target.tunerIndex;
    req.space = target.space;
    req.channel = target.channel;

    req.settings.storeSettings.rootFolder = settings_.rootFolder;
    req.settings.storeSettings.maxBytes = settings_.maxBytesPerChannel;
    req.settings.storeSettings.highWatermarkPercent = settings_.highWatermarkPercent;
    req.settings.storeSettings.lowWatermarkPercent = settings_.lowWatermarkPercent;
    req.settings.storeSettings.segmentSeconds = settings_.segmentSeconds;
    req.settings.storeSettings.segmentMaxBytes = settings_.segmentMaxBytes;
    req.settings.storeSettings.packetSize = 188;

    req.settings.maxQueueBytes = settings_.writerQueueBytes;
    req.settings.dropOldestWhenFull = settings_.dropOldestWhenFull;

    return req;
}