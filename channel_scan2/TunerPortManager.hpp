#pragma once
#ifndef TUNER_PORT_MANAGER_HPP
#define TUNER_PORT_MANAGER_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "bondriver_load.h"

class IBonDriver2;

class TunerPortManager final
{
public:
    enum class Purpose : std::uint32_t
    {
        None = 0,
        ChannelScan = 1u << 0,
        EpgCapture = 1u << 1,
        ReservationRecord = 1u << 2,
        TimeShift = 1u << 3
    };

    using PurposeMask = std::uint32_t;

    struct PortConfig
    {
        int tunerIndex = -1;
        bool enabled = false;
        bool allowParallel = true;
        int priority = 0;
        std::wstring name;
        std::wstring bonDriverPath;
        PurposeMask allowedPurposes = 0;
    };

    struct AcquireRequest
    {
        Purpose purpose = Purpose::None;
        int preferredTunerIndex = -1;
        std::uint32_t ownerInstance = 0;
        bool allowFallback = true;
    };

    struct Lease
    {
        int tunerIndex = -1;
        IBonDriver2* bon = nullptr;
        std::uint64_t leaseId = 0;
        std::uint32_t ownerInstance = 0;
        Purpose purpose = Purpose::None;

        bool IsValid() const noexcept
        {
            return tunerIndex >= 0 && bon != nullptr && leaseId != 0;
        }

        void Reset() noexcept
        {
            tunerIndex = -1;
            bon = nullptr;
            leaseId = 0;
            ownerInstance = 0;
            purpose = Purpose::None;
        }
    };

    struct PortStatus
    {
        int tunerIndex = -1;
        bool enabled = false;
        bool acquired = false;
        bool driverLoaded = false;
        int priority = 0;
        std::uint32_t ownerInstance = 0;
        std::uint64_t leaseId = 0;
        Purpose purpose = Purpose::None;
        std::wstring name;
        std::wstring bonDriverPath;
    };

    TunerPortManager() noexcept;
    ~TunerPortManager() noexcept;

    TunerPortManager(const TunerPortManager&) = delete;
    TunerPortManager& operator=(const TunerPortManager&) = delete;

    // 設定だけを保持する。この時点ではBonDriverをロードしない。
    // 使用中ポートがある場合は失敗する。
    bool Configure(const std::vector<PortConfig>& configs) noexcept;

    // 空きポートをロックし、ここで初めてBonDriverをロードする。
    bool Acquire(const AcquireRequest& request, Lease& outLease) noexcept;

    // 受信スレッドを停止・joinした後に呼ぶ。
    // leaseIdが一致するポートだけを解放する。
    bool Release(Lease& lease) noexcept;

    // アプリ終了時の最終安全処理。通常は各機能が先にReleaseする。
    void StopAll() noexcept;

    std::vector<PortStatus> GetStatuses() const;
    bool HasActiveLease() const noexcept;

    static constexpr PurposeMask ToMask(Purpose purpose) noexcept
    {
        return static_cast<PurposeMask>(purpose);
    }

    static constexpr bool HasPurpose(
        PurposeMask mask,
        Purpose purpose) noexcept
    {
        return (mask & ToMask(purpose)) != 0;
    }

    static constexpr PurposeMask AllPurposes() noexcept
    {
        return ToMask(Purpose::ChannelScan) |
            ToMask(Purpose::EpgCapture) |
            ToMask(Purpose::ReservationRecord) |
            ToMask(Purpose::TimeShift);
    }

private:
    struct Port
    {
        PortConfig config{};
        std::unique_ptr<BonDriverLoader> loader{};
        IBonDriver2* bon = nullptr;
        bool acquired = false;
        std::uint32_t ownerInstance = 0;
        std::uint64_t leaseId = 0;
        Purpose purpose = Purpose::None;
    };

    static bool IsPurposeAllowed_(
        const PortConfig& config,
        Purpose purpose) noexcept;

    static const wchar_t* PurposeName_(Purpose purpose) noexcept;

    Port* FindPortLocked_(int tunerIndex) noexcept;
    const Port* FindPortLocked_(int tunerIndex) const noexcept;

    bool TryAcquirePortLocked_(
        Port& port,
        const AcquireRequest& request,
        Lease& outLease) noexcept;

    static void ReleasePortLocked_(Port& port) noexcept;

    std::uint64_t NextLeaseIdLocked_() noexcept;

private:
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Port>> ports_;
    std::uint64_t nextLeaseId_ = 1;
};

#endif // TUNER_PORT_MANAGER_HPP
