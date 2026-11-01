#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "TimeShift_Engine.hpp"

#include "ConfigManager.h"
#include "DebugManager.h"
#include "IBonDriver2.h"
#include "TSStreamUtils.hpp"
#include "bondriver_load.h"

#include <algorithm>
#include <utility>

TimeShift_Engine::TimeShift_Engine() = default;

TimeShift_Engine::~TimeShift_Engine() noexcept
{
    StopAll();
}

void TimeShift_Engine::Configure(const Settings& settings)
{
    std::lock_guard<std::mutex> lock(mutex_);

    settings_ = settings;

    if (settings_.signalLockTimeoutMs < 0) {
        settings_.signalLockTimeoutMs = 0;
    }

    if (settings_.discardInitialBlocks < 0) {
        settings_.discardInitialBlocks = 0;
    }

    if (settings_.idleSleepMs == 0) {
        settings_.idleSleepMs = 1;
    }

    if (settings_.noDataTimeoutMs < 1000) {
        settings_.noDataTimeoutMs = 1000;
    }

    if (settings_.statusLogIntervalMs < 5000) {
        settings_.statusLogIntervalMs = 5000;
    }

    manager_.Configure(settings_.managerSettings);
}

void TimeShift_Engine::SetTunerPortManager(
    TunerPortManager* manager) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    tunerPortManager_ = manager;
}

bool TimeShift_Engine::StartFromConfig(const ConfigManager& cfg)
{
    StopAll();

    manager_.Configure(settings_.managerSettings);

    if (!manager_.LoadTargetsFromConfig(cfg)) {
        DebugLog(L"[TimeShift][Engine] LoadTargetsFromConfig failed");
        return false;
    }

    const auto targets = manager_.GetTargets();
    if (targets.empty()) {
        DebugLog(L"[TimeShift][Engine] no TimeShift targets");
        return true;
    }

    bool anyStarted = false;

    for (const auto& target : targets) {
        if (StartTunerFromConfig(cfg, target.tunerIndex)) {
            anyStarted = true;
        }
    }

    return anyStarted;
}

void TimeShift_Engine::StopAll() noexcept
{
    std::unordered_map<int, std::shared_ptr<Session>> local;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        local.swap(sessions_);
    }

    for (auto& pair : local) {
        const auto& session = pair.second;
        if (!session) {
            continue;
        }

        session->stopRequested.store(true, std::memory_order_release);
    }

    for (auto& pair : local) {
        const auto& session = pair.second;
        if (!session) {
            continue;
        }

        if (session->worker.joinable()) {
            try {
                session->worker.join();
            }
            catch (...) {
            }
        }

        if (session->tunerLease.IsValid() && tunerPortManager_ != nullptr) {
            (void)tunerPortManager_->Release(session->tunerLease);
            session->bon = nullptr;
        }
        else if (session->bon != nullptr) {
            try {
                session->bon->PurgeTsStream();
                session->bon->CloseTuner();
            }
            catch (...) {
            }
            session->bon = nullptr;
        }

        if (session->loader) {
            try {
                session->loader->Unload();
            }
            catch (...) {
            }
            session->loader.reset();
        }
    }

    manager_.StopAll();

    DebugLog(L"[TimeShift][Engine] StopAll done");
}

void TimeShift_Engine::ResetAll() noexcept
{
    StopAll();
    manager_.ResetAll();
}

bool TimeShift_Engine::StartTunerFromConfig(
    const ConfigManager& cfg,
    int tunerIndex)
{
    StopTuner(tunerIndex);

    manager_.Configure(settings_.managerSettings);
    manager_.LoadTargetsFromConfig(cfg);

    std::shared_ptr<Session> session;
    std::wstring reason;

    if (!BuildSessionFromConfig_(cfg, tunerIndex, session, reason)) {
        DebugLogFormat(
            L"[TimeShift][Engine] build session failed tuner=%d reason=%ls",
            tunerIndex,
            reason.c_str());
        return false;
    }

    if (!PrepareBonDriver_(session)) {
        DebugLogFormat(
            L"[TimeShift][Engine] prepare BonDriver failed tuner=%d reason=%ls",
            tunerIndex,
            session ? session->reason.c_str() : L"(null)");

        if (session && session->tunerLease.IsValid() &&
            tunerPortManager_ != nullptr) {
            (void)tunerPortManager_->Release(session->tunerLease);
            session->bon = nullptr;
        }
        else if (session && session->loader) {
            session->loader->Unload();
        }

        return false;
    }

    if (!manager_.StartTuner(tunerIndex)) {
        DebugLogFormat(
            L"[TimeShift][Engine] manager StartTuner failed tuner=%d",
            tunerIndex);

        if (session->tunerLease.IsValid() && tunerPortManager_ != nullptr) {
            (void)tunerPortManager_->Release(session->tunerLease);
            session->bon = nullptr;
        }
        else if (session->bon != nullptr) {
            try {
                session->bon->CloseTuner();
            }
            catch (...) {
            }
            session->bon = nullptr;
        }

        if (session->loader) {
            session->loader->Unload();
            session->loader.reset();
        }

        return false;
    }

    session->stopRequested.store(false, std::memory_order_release);
    session->workerRunning.store(false, std::memory_order_release);
    session->lastDataTickMs = ::GetTickCount64();
    session->lastStatusLogTickMs = session->lastDataTickMs;

    try {
        session->worker = std::thread(&TimeShift_Engine::Worker_, this, session);
    }
    catch (...) {
        manager_.StopTuner(tunerIndex);

        if (session->tunerLease.IsValid() && tunerPortManager_ != nullptr) {
            (void)tunerPortManager_->Release(session->tunerLease);
            session->bon = nullptr;
        }
        else if (session->bon != nullptr) {
            try {
                session->bon->CloseTuner();
            }
            catch (...) {
            }
            session->bon = nullptr;
        }

        if (session->loader) {
            session->loader->Unload();
            session->loader.reset();
        }

        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_[tunerIndex] = session;
    }

    DebugLogFormat(
        L"[TimeShift][Engine] started tuner=%d space=%d ch=%d name=%ls path=%ls",
        session->tunerIndex,
        session->space,
        session->channel,
        session->serviceName.c_str(),
        session->bonDriverPath.c_str());

    return true;
}

void TimeShift_Engine::StopTuner(int tunerIndex) noexcept
{
    std::shared_ptr<Session> session;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = sessions_.find(tunerIndex);
        if (it == sessions_.end()) {
            manager_.StopTuner(tunerIndex);
            return;
        }

        session = it->second;
        sessions_.erase(it);
    }

    if (session) {
        session->stopRequested.store(true, std::memory_order_release);

        if (session->worker.joinable()) {
            try {
                session->worker.join();
            }
            catch (...) {
            }
        }

        if (session->tunerLease.IsValid() && tunerPortManager_ != nullptr) {
            (void)tunerPortManager_->Release(session->tunerLease);
            session->bon = nullptr;
        }
        else if (session->bon != nullptr) {
            try {
                session->bon->PurgeTsStream();
                session->bon->CloseTuner();
            }
            catch (...) {
            }

            session->bon = nullptr;
        }

        if (session->loader) {
            try {
                session->loader->Unload();
            }
            catch (...) {
            }

            session->loader.reset();
        }
    }

    manager_.StopTuner(tunerIndex);

    DebugLogFormat(
        L"[TimeShift][Engine] stopped tuner=%d",
        tunerIndex);
}

bool TimeShift_Engine::IsRunning(int tunerIndex) const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(tunerIndex);
    if (it == sessions_.end() || !it->second) {
        return false;
    }

    return it->second->workerRunning.load(std::memory_order_acquire);
}

std::vector<TimeShift_Engine::TunerStatus>
TimeShift_Engine::GetStatuses() const
{
    std::vector<std::shared_ptr<Session>> sessionList;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        sessionList.reserve(sessions_.size());
        for (const auto& pair : sessions_) {
            if (pair.second) {
                sessionList.push_back(pair.second);
            }
        }
    }

    std::vector<TunerStatus> out;
    out.reserve(sessionList.size());

    for (const auto& session : sessionList) {
        TunerStatus st{};

        st.active = true;
        st.workerRunning =
            session->workerRunning.load(std::memory_order_acquire);
        st.failed =
            session->failed.load(std::memory_order_acquire);

        st.tunerIndex = session->tunerIndex;
        st.space = session->space;
        st.channel = session->channel;
        st.bonDriverPath = session->bonDriverPath;
        st.serviceName = session->serviceName;
        st.reason = session->reason;

        {
            std::lock_guard<std::mutex> statLock(session->statMutex);

            st.receiveBlocks = session->receiveBlocks;
            st.receivePackets = session->receivePackets;
            st.receiveBytes = session->receiveBytes;
            st.receiveFailures = session->receiveFailures;
            st.pushFailures = session->pushFailures;
        }

        st.recorderStatus = manager_.GetStatuses().empty()
            ? TimeShift_SegmentRecorder::Status{}
        : TimeShift_SegmentRecorder::Status{};

        const auto managerStatuses = manager_.GetStatuses();
        for (const auto& ms : managerStatuses) {
            if (ms.target.tunerIndex == session->tunerIndex) {
                st.recorderStatus = ms.recorderStatus;
                break;
            }
        }

        out.push_back(std::move(st));
    }

    std::sort(
        out.begin(),
        out.end(),
        [](const TunerStatus& a, const TunerStatus& b)
        {
            return a.tunerIndex < b.tunerIndex;
        });

    return out;
}

bool TimeShift_Engine::BuildSessionFromConfig_(
    const ConfigManager& cfg,
    int tunerIndex,
    std::shared_ptr<Session>& outSession,
    std::wstring& reason) const
{
    outSession.reset();
    reason.clear();

    if (tunerIndex < 0 || tunerIndex >= cfg.GetTunersCount()) {
        reason = L"tunerIndex が範囲外です。";
        return false;
    }

    if (!cfg.GetTunerEnabled(tunerIndex)) {
        reason = L"チューナーが無効です。";
        return false;
    }

    if (cfg.GetTunerUseRole(tunerIndex) !=
        ConfigManager::TunerUseRole::TimeShiftOnly) {
        reason = L"用途が TimeShiftOnly ではありません。";
        return false;
    }

    const int space = cfg.GetTunerTimeShiftLockSpace(tunerIndex);
    const int channel = cfg.GetTunerTimeShiftLockChannel(tunerIndex);

    if (space < 0 || channel < 0) {
        reason = L"タイムシフト固定チャンネルが未設定です。";
        return false;
    }

    const std::wstring bonPath =
        cfg.GetBonDriverPathForTunerOrDefault(tunerIndex);

    if (bonPath.empty()) {
        reason = L"BonDriver パスが空です。";
        return false;
    }

    auto session = std::make_shared<Session>();

    session->tunerIndex = tunerIndex;
    session->space = space;
    session->channel = channel;
    session->serviceId = cfg.GetTunerTimeShiftLockServiceId(tunerIndex);
    session->networkId = cfg.GetTunerTimeShiftLockNetworkId(tunerIndex);
    session->tsId = cfg.GetTunerTimeShiftLockTsId(tunerIndex);
    session->serviceName = cfg.GetTunerTimeShiftLockName(tunerIndex);
    session->bonDriverPath = bonPath;

    outSession = std::move(session);
    return true;
}

bool TimeShift_Engine::PrepareBonDriver_(
    const std::shared_ptr<Session>& session) noexcept
{
    if (!session) {
        return false;
    }

    if (tunerPortManager_ != nullptr) {
        TunerPortManager::AcquireRequest request{};
        request.purpose = TunerPortManager::Purpose::TimeShift;
        request.preferredTunerIndex = session->tunerIndex;
        request.ownerInstance =
            static_cast<std::uint32_t>(session->tunerIndex + 1);
        request.allowFallback = false;

        if (!tunerPortManager_->Acquire(request, session->tunerLease)) {
            SetFailure_(session, L"タイムシフト用チューナーポートを取得できません。");
            return false;
        }
        session->bon = session->tunerLease.bon;
    }
    else {
        session->loader = std::make_unique<BonDriverLoader>();

        if (!session->loader->LoadBonDriver(session->bonDriverPath)) {
            SetFailure_(session, L"BonDriver のロードに失敗しました。");
            return false;
        }

        session->bon = session->loader->GetBonDriver();
    }

    if (session->bon == nullptr) {
        SetFailure_(session, L"IBonDriver2 の取得に失敗しました。");
        return false;
    }

    try {
        if (!session->bon->IsTunerOpening()) {
            if (!session->bon->OpenTuner()) {
                SetFailure_(session, L"OpenTuner に失敗しました。");
                return false;
            }
        }

        if (!session->bon->SetChannel(
            static_cast<DWORD>(session->space),
            static_cast<DWORD>(session->channel))) {
            SetFailure_(session, L"SetChannel に失敗しました。");
            return false;
        }

        if (settings_.settleSleepMs > 0) {
            ::Sleep(settings_.settleSleepMs);
        }

        session->bon->PurgeTsStream();

        if (settings_.waitForSignalLock) {
            if (!TSStreamUtils::WaitForSignalLock(
                session->bon,
                settings_.signalLockTimeoutMs)) {
                SetFailure_(session, L"信号ロックに失敗しました。");
                return false;
            }
        }

        std::vector<TsPacket> drainPackets;
        drainPackets.reserve(4096);

        for (int i = 0; i < settings_.discardInitialBlocks; ++i) {
            drainPackets.clear();
            TSStreamUtils::ReceiveTsStreamBlock(session->bon, drainPackets);
            ::Sleep(10);
        }
    }
    catch (...) {
        SetFailure_(session, L"BonDriver 準備中に例外が発生しました。");
        return false;
    }

    return true;
}

void TimeShift_Engine::Worker_(
    const std::shared_ptr<Session>& session) noexcept
{
    if (!session || session->bon == nullptr) {
        return;
    }

    session->workerRunning.store(true, std::memory_order_release);

    std::vector<TsPacket> packets;
    packets.reserve(4096);

    std::vector<std::uint8_t> bytes;
    bytes.reserve(4096u * 188u);

    session->lastDataTickMs = ::GetTickCount64();
    session->lastStatusLogTickMs = session->lastDataTickMs;

    while (!session->stopRequested.load(std::memory_order_acquire)) {
        packets.clear();

        bool received = false;

        try {
            received =
                TSStreamUtils::ReceiveTsStreamBlock(
                    session->bon,
                    packets);
        }
        catch (...) {
            AddReceiveFailure_(session);
            SetFailure_(session, L"ReceiveTsStreamBlock で例外が発生しました。");
            break;
        }

        if (!received || packets.empty()) {
            AddReceiveFailure_(session);

            const ULONGLONG now = ::GetTickCount64();
            const ULONGLONG elapsedNoData =
                now - session->lastDataTickMs;

            if (elapsedNoData >=
                static_cast<ULONGLONG>(settings_.noDataTimeoutMs)) {
                SetFailure_(session, L"一定時間 TS データを受信できませんでした。");
                break;
            }

            ::Sleep(settings_.idleSleepMs);
            continue;
        }

        session->lastDataTickMs = ::GetTickCount64();

        const std::uint64_t blockBytes =
            PacketsToBytes_(packets, bytes);

        if (blockBytes == 0 || bytes.empty()) {
            AddPushFailure_(session);
            ::Sleep(settings_.idleSleepMs);
            continue;
        }

        const bool pushed =
            manager_.PushBytesForTuner(
                session->tunerIndex,
                bytes.data(),
                bytes.size());

        if (!pushed) {
            AddPushFailure_(session);
        }
        else {
            AddReceiveStats_(
                session,
                1,
                static_cast<std::uint64_t>(packets.size()),
                blockBytes);
        }

        const ULONGLONG now = ::GetTickCount64();
        if (settings_.statusLogIntervalMs > 0 &&
            now - session->lastStatusLogTickMs >=
            static_cast<ULONGLONG>(settings_.statusLogIntervalMs)) {
            session->lastStatusLogTickMs = now;

            TunerStatus st{};
            {
                std::lock_guard<std::mutex> statLock(session->statMutex);
                st.receiveBlocks = session->receiveBlocks;
                st.receivePackets = session->receivePackets;
                st.receiveBytes = session->receiveBytes;
                st.receiveFailures = session->receiveFailures;
                st.pushFailures = session->pushFailures;
            }

            DebugLogFormat(
                L"[TimeShift][Engine] running tuner=%d space=%d ch=%d blocks=%llu packets=%llu bytes=%llu recvFail=%llu pushFail=%llu",
                session->tunerIndex,
                session->space,
                session->channel,
                static_cast<unsigned long long>(st.receiveBlocks),
                static_cast<unsigned long long>(st.receivePackets),
                static_cast<unsigned long long>(st.receiveBytes),
                static_cast<unsigned long long>(st.receiveFailures),
                static_cast<unsigned long long>(st.pushFailures));
        }
    }

    manager_.StopTuner(session->tunerIndex);

    session->workerRunning.store(false, std::memory_order_release);

    DebugLogFormat(
        L"[TimeShift][Engine] worker end tuner=%d failed=%d reason=%ls",
        session->tunerIndex,
        session->failed.load(std::memory_order_acquire) ? 1 : 0,
        session->reason.c_str());
}

std::uint64_t TimeShift_Engine::PacketsToBytes_(
    const std::vector<TsPacket>& packets,
    std::vector<std::uint8_t>& outBytes)
{
    outBytes.clear();

    if (packets.empty()) {
        return 0;
    }

    std::uint64_t totalBytes = 0;

    for (const auto& pkt : packets) {
        totalBytes += static_cast<std::uint64_t>(pkt.size());
    }

    if (totalBytes == 0) {
        return 0;
    }

    try {
        outBytes.reserve(static_cast<std::size_t>(totalBytes));
    }
    catch (...) {
        outBytes.clear();
        return 0;
    }

    for (const auto& pkt : packets) {
        const auto* p = pkt.data();
        const std::size_t n = pkt.size();

        if (p == nullptr || n == 0) {
            continue;
        }

        try {
            outBytes.insert(outBytes.end(), p, p + n);
        }
        catch (...) {
            outBytes.clear();
            return 0;
        }
    }

    return static_cast<std::uint64_t>(outBytes.size());
}

void TimeShift_Engine::AddReceiveFailure_(
    const std::shared_ptr<Session>& session) noexcept
{
    if (!session) {
        return;
    }

    std::lock_guard<std::mutex> lock(session->statMutex);
    ++session->receiveFailures;
}

void TimeShift_Engine::AddPushFailure_(
    const std::shared_ptr<Session>& session) noexcept
{
    if (!session) {
        return;
    }

    std::lock_guard<std::mutex> lock(session->statMutex);
    ++session->pushFailures;
}

void TimeShift_Engine::AddReceiveStats_(
    const std::shared_ptr<Session>& session,
    std::uint64_t blocks,
    std::uint64_t packets,
    std::uint64_t bytes) noexcept
{
    if (!session) {
        return;
    }

    std::lock_guard<std::mutex> lock(session->statMutex);

    session->receiveBlocks += blocks;
    session->receivePackets += packets;
    session->receiveBytes += bytes;
}

void TimeShift_Engine::SetFailure_(
    const std::shared_ptr<Session>& session,
    const std::wstring& reason) noexcept
{
    if (!session) {
        return;
    }

    session->failed.store(true, std::memory_order_release);
    session->reason = reason;
}
