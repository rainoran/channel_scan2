#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "TunerPortManager.hpp"

#include "DebugManager.h"
#include "IBonDriver2.h"
#include "bondriver_load.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <new>
#include <unordered_set>
#include <utility>

TunerPortManager::TunerPortManager() noexcept = default;

TunerPortManager::~TunerPortManager() noexcept
{
    StopAll();
}

bool TunerPortManager::Configure(
    const std::vector<PortConfig>& configs) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& owner : ports_) {
        const Port* port = owner.get();
        if (port != nullptr && port->acquired) {
            DebugLog(L"[TunerPortManager][CONFIG] rejected: active lease exists");
            return false;
        }
    }

    try {
        std::unordered_set<int> usedIndices;
        std::vector<std::unique_ptr<Port>> newPorts;

        newPorts.reserve(configs.size());

        for (const PortConfig& source : configs) {
            if (source.tunerIndex < 0) {
                continue;
            }

            if (!usedIndices.insert(source.tunerIndex).second) {
                DebugLogFormat(
                    L"[TunerPortManager][CONFIG] duplicate tuner=%d",
                    source.tunerIndex);

                return false;
            }

            auto owner = std::make_unique<Port>();
            owner->config = source;

            if (owner->config.allowedPurposes == 0) {
                owner->config.allowedPurposes = AllPurposes();
            }

            newPorts.push_back(std::move(owner));
        }

        std::sort(
            newPorts.begin(),
            newPorts.end(),
            [](const std::unique_ptr<Port>& a,
                const std::unique_ptr<Port>& b) {
                    if (a->config.priority != b->config.priority) {
                        return a->config.priority > b->config.priority;
                    }
                    return a->config.tunerIndex < b->config.tunerIndex;
            });

        ports_ = std::move(newPorts);

        DebugLogFormat(
            L"[TunerPortManager][CONFIG] ports=%zu loaded=0",
            ports_.size());
        return true;
    }
    catch (...) {
        DebugLog(L"[TunerPortManager][CONFIG] exception");
        return false;
    }
}

bool TunerPortManager::Acquire(
    const AcquireRequest& request,
    Lease& outLease) noexcept
{
    outLease.Reset();

    if (request.purpose == Purpose::None) {
        DebugLog(L"[TunerPortManager][ACQUIRE] invalid purpose");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (request.preferredTunerIndex >= 0) {
        Port* preferred = FindPortLocked_(request.preferredTunerIndex);
        if (preferred != nullptr &&
            TryAcquirePortLocked_(*preferred, request, outLease)) {
            return true;
        }

        if (!request.allowFallback) {
            return false;
        }
    }

    for (const auto& owner : ports_) {
        Port* port = owner.get();
        if (port == nullptr ||
            port->config.tunerIndex == request.preferredTunerIndex) {
            continue;
        }

        if (TryAcquirePortLocked_(*port, request, outLease)) {
            return true;
        }
    }

    DebugLogFormat(
        L"[TunerPortManager][ACQUIRE] no free port purpose=%ls preferred=%d owner=%u",
        PurposeName_(request.purpose),
        request.preferredTunerIndex,
        static_cast<unsigned>(request.ownerInstance));
    return false;
}

bool TunerPortManager::Release(Lease& lease) noexcept
{
    if (!lease.IsValid()) {
        lease.Reset();
        return true;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    Port* port = FindPortLocked_(lease.tunerIndex);
    if (port == nullptr ||
        !port->acquired ||
        port->leaseId != lease.leaseId ||
        port->ownerInstance != lease.ownerInstance ||
        port->purpose != lease.purpose) {
        DebugLogFormat(
            L"[TunerPortManager][RELEASE] lease mismatch tuner=%d lease=%llu owner=%u",
            lease.tunerIndex,
            static_cast<unsigned long long>(lease.leaseId),
            static_cast<unsigned>(lease.ownerInstance));
        lease.Reset();
        return false;
    }

    DebugLogFormat(
        L"[TunerPortManager][RELEASE] tuner=%d lease=%llu purpose=%ls owner=%u",
        port->config.tunerIndex,
        static_cast<unsigned long long>(port->leaseId),
        PurposeName_(port->purpose),
        static_cast<unsigned>(port->ownerInstance));

    ReleasePortLocked_(*port);
    lease.Reset();
    return true;
}

void TunerPortManager::StopAll() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& owner : ports_) {
        Port* port = owner.get();
        if (port != nullptr) {
            ReleasePortLocked_(*port);
        }
    }

    DebugLog(L"[TunerPortManager] StopAll done");
}

std::vector<TunerPortManager::PortStatus>
TunerPortManager::GetStatuses() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<PortStatus> result;
    result.reserve(ports_.size());

    for (const auto& owner : ports_) {
        const Port* port = owner.get();
        if (port == nullptr) {
            continue;
        }

        PortStatus status{};
        status.tunerIndex = port->config.tunerIndex;
        status.enabled = port->config.enabled;
        status.acquired = port->acquired;
        status.driverLoaded =
            port->loader != nullptr && port->loader->IsLoaded();
        status.priority = port->config.priority;
        status.ownerInstance = port->ownerInstance;
        status.leaseId = port->leaseId;
        status.purpose = port->purpose;
        status.name = port->config.name;
        status.bonDriverPath = port->config.bonDriverPath;
        result.push_back(std::move(status));
    }

    return result;
}

bool TunerPortManager::HasActiveLease() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(
        ports_.begin(),
        ports_.end(),
        [](const std::unique_ptr<Port>& port) {
            return port != nullptr && port->acquired;
        });
}

bool TunerPortManager::IsPurposeAllowed_(
    const PortConfig& config,
    Purpose purpose) noexcept
{
    return (config.allowedPurposes & ToMask(purpose)) != 0;
}

const wchar_t* TunerPortManager::PurposeName_(Purpose purpose) noexcept
{
    switch (purpose) {
    case Purpose::ChannelScan:
        return L"ChannelScan";
    case Purpose::EpgCapture:
        return L"EpgCapture";
    case Purpose::ReservationRecord:
        return L"ReservationRecord";
    case Purpose::TimeShift:
        return L"TimeShift";
    default:
        return L"None";
    }
}

TunerPortManager::Port*
TunerPortManager::FindPortLocked_(int tunerIndex) noexcept
{
    for (const auto& owner : ports_) {
        Port* port = owner.get();
        if (port != nullptr && port->config.tunerIndex == tunerIndex) {
            return port;
        }
    }
    return nullptr;
}

const TunerPortManager::Port*
TunerPortManager::FindPortLocked_(int tunerIndex) const noexcept
{
    for (const auto& owner : ports_) {
        const Port* port = owner.get();
        if (port != nullptr && port->config.tunerIndex == tunerIndex) {
            return port;
        }
    }
    return nullptr;
}

bool TunerPortManager::TryAcquirePortLocked_(
    Port& port,
    const AcquireRequest& request,
    Lease& outLease) noexcept
{
    const PortConfig& config = port.config;

    if (!config.enabled ||
        config.bonDriverPath.empty() ||
        port.acquired ||
        !IsPurposeAllowed_(config, request.purpose)) {
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::exists(config.bonDriverPath, ec) || ec) {
        DebugLogFormat(
            L"[TunerPortManager][ACQUIRE] driver missing tuner=%d path=%ls ec=%d",
            config.tunerIndex,
            config.bonDriverPath.c_str(),
            ec.value());
        return false;
    }

    try {
        port.loader = std::make_unique<BonDriverLoader>();
        if (!port.loader->Load(config.bonDriverPath)) {
            port.loader.reset();
            DebugLogFormat(
                L"[TunerPortManager][ACQUIRE] load failed tuner=%d path=%ls",
                config.tunerIndex,
                config.bonDriverPath.c_str());
            return false;
        }

        port.bon = port.loader->GetBonDriver2();
        if (port.bon == nullptr) {
            port.loader->Unload();
            port.loader.reset();
            return false;
        }

        port.acquired = true;
        port.ownerInstance = request.ownerInstance;
        port.purpose = request.purpose;
        port.leaseId = NextLeaseIdLocked_();

        outLease.tunerIndex = config.tunerIndex;
        outLease.bon = port.bon;
        outLease.leaseId = port.leaseId;
        outLease.ownerInstance = port.ownerInstance;
        outLease.purpose = port.purpose;

        DebugLogFormat(
            L"[TunerPortManager][ACQUIRE] tuner=%d lease=%llu purpose=%ls owner=%u path=%ls",
            config.tunerIndex,
            static_cast<unsigned long long>(port.leaseId),
            PurposeName_(port.purpose),
            static_cast<unsigned>(port.ownerInstance),
            config.bonDriverPath.c_str());
        return true;
    }
    catch (...) {
        ReleasePortLocked_(port);
        return false;
    }
}

void TunerPortManager::ReleasePortLocked_(Port& port) noexcept
{
    if (port.bon != nullptr) {
        try {
            port.bon->PurgeTsStream();
        }
        catch (...) {
        }
    }

    // BonDriverLoader::Unload() が CloseTuner -> Release -> FreeLibrary
    // の順で処理する。CloseTunerをここで重複実行しない。
    if (port.loader) {
        try {
            port.loader->Unload();
        }
        catch (...) {
        }
        port.loader.reset();
    }

    port.bon = nullptr;
    port.acquired = false;
    port.ownerInstance = 0;
    port.leaseId = 0;
    port.purpose = Purpose::None;
}

std::uint64_t TunerPortManager::NextLeaseIdLocked_() noexcept
{
    std::uint64_t value = nextLeaseId_++;
    if (value == 0) {
        value = nextLeaseId_++;
    }
    return value;
}
