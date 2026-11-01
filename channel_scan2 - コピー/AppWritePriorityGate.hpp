#pragma once
#ifndef APP_WRITE_PRIORITY_GATE_HPP
#define APP_WRITE_PRIORITY_GATE_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace AppWritePriorityGate
{
    enum class Domain : int
    {
        EpgFiles = 0,
        EpgCache = 1,
        ReservationFiles = 2,
        ConfigIni = 3,
        TimeShiftIndex = 4
    };

    class Gate final
    {
    public:
        Gate() = default;

        Gate(const Gate&) = delete;
        Gate& operator=(const Gate&) = delete;

        void LockRead() noexcept;
        bool TryLockRead() noexcept;
        void UnlockRead() noexcept;

        void LockCriticalRead() noexcept;
        bool TryLockCriticalRead() noexcept;
        void UnlockCriticalRead() noexcept;

        void LockWriteCommit() noexcept;
        void UnlockWriteCommit() noexcept;

    private:
        std::mutex mutex_;
        std::condition_variable cv_;

        std::uint32_t activeReaders_ = 0;
        std::uint32_t activeCriticalReaders_ = 0;
        std::uint32_t waitingWriters_ = 0;
        bool activeWriter_ = false;
    };

    Gate& GetGate(Domain domain) noexcept;

    class ReadLock final
    {
    public:
        ReadLock() noexcept = default;
        ~ReadLock() noexcept;

        ReadLock(const ReadLock&) = delete;
        ReadLock& operator=(const ReadLock&) = delete;

        ReadLock(ReadLock&& other) noexcept;
        ReadLock& operator=(ReadLock&& other) noexcept;

        static ReadLock Acquire(Domain domain) noexcept;
        static ReadLock TryAcquire(Domain domain) noexcept;

        static ReadLock AcquireCritical(Domain domain) noexcept;
        static ReadLock TryAcquireCritical(Domain domain) noexcept;

        bool IsLocked() const noexcept;
        bool IsCritical() const noexcept;

        void Release() noexcept;

    private:
        Gate* gate_ = nullptr;
        bool locked_ = false;
        bool critical_ = false;
    };

    class WriteCommitLock final
    {
    public:
        WriteCommitLock() noexcept = default;
        ~WriteCommitLock() noexcept;

        WriteCommitLock(const WriteCommitLock&) = delete;
        WriteCommitLock& operator=(const WriteCommitLock&) = delete;

        WriteCommitLock(WriteCommitLock&& other) noexcept;
        WriteCommitLock& operator=(WriteCommitLock&& other) noexcept;

        static WriteCommitLock Acquire(Domain domain) noexcept;

        bool IsLocked() const noexcept;
        void Release() noexcept;

    private:
        Gate* gate_ = nullptr;
        bool locked_ = false;
    };
}

#endif // APP_WRITE_PRIORITY_GATE_HPP
