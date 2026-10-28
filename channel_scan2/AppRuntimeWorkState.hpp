#pragma once
#ifndef APP_RUNTIME_WORK_STATE_HPP
#define APP_RUNTIME_WORK_STATE_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <atomic>
#include <cstdint>

namespace AppRuntimeWorkState
{
    class ScopedEpgUpdateActive final
    {
    public:
        ScopedEpgUpdateActive() noexcept;
        ~ScopedEpgUpdateActive() noexcept;

        ScopedEpgUpdateActive(const ScopedEpgUpdateActive&) = delete;
        ScopedEpgUpdateActive& operator=(const ScopedEpgUpdateActive&) = delete;

        bool IsActive() const noexcept;

    private:
        bool active_ = false;
    };

    void BeginEpgUpdate() noexcept;
    void EndEpgUpdate() noexcept;

    bool IsEpgUpdateActive() noexcept;

    std::uint64_t GetEpgUpdateGeneration() noexcept;

    void RequestTimeShiftListRefreshAfterEpg() noexcept;
    bool ConsumeTimeShiftListRefreshAfterEpg() noexcept;
}

#endif // APP_RUNTIME_WORK_STATE_HPP
