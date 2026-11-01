#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "AppRuntimeWorkState.hpp"

namespace
{
    std::atomic_uint32_t g_epgUpdateActiveCount{ 0 };
    std::atomic_uint64_t g_epgUpdateGeneration{ 0 };
    std::atomic_bool g_timeShiftRefreshPendingAfterEpg{ false };
}

namespace AppRuntimeWorkState
{
    ScopedEpgUpdateActive::ScopedEpgUpdateActive() noexcept
    {
        BeginEpgUpdate();
        active_ = true;
    }

    ScopedEpgUpdateActive::~ScopedEpgUpdateActive() noexcept
    {
        if (active_) {
            EndEpgUpdate();
            active_ = false;
        }
    }

    bool ScopedEpgUpdateActive::IsActive() const noexcept
    {
        return active_;
    }

    void BeginEpgUpdate() noexcept
    {
        g_epgUpdateActiveCount.fetch_add(
            1u,
            std::memory_order_acq_rel);
    }

    void EndEpgUpdate() noexcept
    {
        std::uint32_t cur =
            g_epgUpdateActiveCount.load(
                std::memory_order_acquire);

        while (cur > 0u) {
            if (g_epgUpdateActiveCount.compare_exchange_weak(
                cur,
                cur - 1u,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
                break;
            }
        }

        if (g_epgUpdateActiveCount.load(
            std::memory_order_acquire) == 0u) {
            g_epgUpdateGeneration.fetch_add(
                1u,
                std::memory_order_acq_rel);
        }
    }

    bool IsEpgUpdateActive() noexcept
    {
        return g_epgUpdateActiveCount.load(
            std::memory_order_acquire) != 0u;
    }

    std::uint64_t GetEpgUpdateGeneration() noexcept
    {
        return g_epgUpdateGeneration.load(
            std::memory_order_acquire);
    }

    void RequestTimeShiftListRefreshAfterEpg() noexcept
    {
        g_timeShiftRefreshPendingAfterEpg.store(
            true,
            std::memory_order_release);
    }

    bool ConsumeTimeShiftListRefreshAfterEpg() noexcept
    {
        return g_timeShiftRefreshPendingAfterEpg.exchange(
            false,
            std::memory_order_acq_rel);
    }
}