#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "AppWritePriorityGate.hpp"

#include <array>

namespace AppWritePriorityGate
{
    void Gate::LockRead() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex_);

        cv_.wait(
            lock,
            [this]()
            {
                return
                    !activeWriter_ &&
                    waitingWriters_ == 0;
            });

        ++activeReaders_;
    }

    bool Gate::TryLockRead() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (activeWriter_ || waitingWriters_ != 0) {
            return false;
        }

        ++activeReaders_;
        return true;
    }

    void Gate::UnlockRead() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (activeReaders_ > 0) {
                --activeReaders_;
            }
        }

        cv_.notify_all();
    }

    void Gate::LockCriticalRead() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex_);

        cv_.wait(
            lock,
            [this]()
            {
                // 予約実行確認読み込みは、
                // 待機中Writerより優先する。
                // ただし、最終置換中のactiveWriterだけは待つ。
                return !activeWriter_;
            });

        ++activeCriticalReaders_;
    }

    bool Gate::TryLockCriticalRead() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (activeWriter_) {
            return false;
        }

        ++activeCriticalReaders_;
        return true;
    }

    void Gate::UnlockCriticalRead() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (activeCriticalReaders_ > 0) {
                --activeCriticalReaders_;
            }
        }

        cv_.notify_all();
    }

    void Gate::LockWriteCommit() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex_);

        ++waitingWriters_;

        cv_.wait(
            lock,
            [this]()
            {
                return
                    !activeWriter_ &&
                    activeReaders_ == 0 &&
                    activeCriticalReaders_ == 0;
            });

        --waitingWriters_;
        activeWriter_ = true;
    }

    void Gate::UnlockWriteCommit() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            activeWriter_ = false;
        }

        cv_.notify_all();
    }

    Gate& GetGate(Domain domain) noexcept
    {
        static std::array<Gate, 5> gates{};

        const int index =
            static_cast<int>(domain);

        if (index < 0 ||
            index >= static_cast<int>(gates.size())) {
            return gates[0];
        }

        return gates[static_cast<std::size_t>(index)];
    }

    ReadLock ReadLock::Acquire(Domain domain) noexcept
    {
        ReadLock out;
        out.gate_ = &GetGate(domain);
        out.gate_->LockRead();
        out.locked_ = true;
        out.critical_ = false;
        return out;
    }

    ReadLock ReadLock::TryAcquire(Domain domain) noexcept
    {
        ReadLock out;
        out.gate_ = &GetGate(domain);

        if (out.gate_->TryLockRead()) {
            out.locked_ = true;
            out.critical_ = false;
        }

        return out;
    }

    ReadLock ReadLock::AcquireCritical(Domain domain) noexcept
    {
        ReadLock out;
        out.gate_ = &GetGate(domain);
        out.gate_->LockCriticalRead();
        out.locked_ = true;
        out.critical_ = true;
        return out;
    }

    ReadLock ReadLock::TryAcquireCritical(Domain domain) noexcept
    {
        ReadLock out;
        out.gate_ = &GetGate(domain);

        if (out.gate_->TryLockCriticalRead()) {
            out.locked_ = true;
            out.critical_ = true;
        }

        return out;
    }

    ReadLock::~ReadLock() noexcept
    {
        Release();
    }

    ReadLock::ReadLock(ReadLock&& other) noexcept
    {
        gate_ = other.gate_;
        locked_ = other.locked_;
        critical_ = other.critical_;

        other.gate_ = nullptr;
        other.locked_ = false;
        other.critical_ = false;
    }

    ReadLock& ReadLock::operator=(ReadLock&& other) noexcept
    {
        if (this != &other) {
            Release();

            gate_ = other.gate_;
            locked_ = other.locked_;
            critical_ = other.critical_;

            other.gate_ = nullptr;
            other.locked_ = false;
            other.critical_ = false;
        }

        return *this;
    }

    bool ReadLock::IsLocked() const noexcept
    {
        return locked_;
    }

    bool ReadLock::IsCritical() const noexcept
    {
        return critical_;
    }

    void ReadLock::Release() noexcept
    {
        if (locked_ && gate_ != nullptr) {
            if (critical_) {
                gate_->UnlockCriticalRead();
            }
            else {
                gate_->UnlockRead();
            }
        }

        gate_ = nullptr;
        locked_ = false;
        critical_ = false;
    }

    WriteCommitLock WriteCommitLock::Acquire(Domain domain) noexcept
    {
        WriteCommitLock out;
        out.gate_ = &GetGate(domain);
        out.gate_->LockWriteCommit();
        out.locked_ = true;
        return out;
    }

    WriteCommitLock::~WriteCommitLock() noexcept
    {
        Release();
    }

    WriteCommitLock::WriteCommitLock(WriteCommitLock&& other) noexcept
    {
        gate_ = other.gate_;
        locked_ = other.locked_;

        other.gate_ = nullptr;
        other.locked_ = false;
    }

    WriteCommitLock& WriteCommitLock::operator=(WriteCommitLock&& other) noexcept
    {
        if (this != &other) {
            Release();

            gate_ = other.gate_;
            locked_ = other.locked_;

            other.gate_ = nullptr;
            other.locked_ = false;
        }

        return *this;
    }

    bool WriteCommitLock::IsLocked() const noexcept
    {
        return locked_;
    }

    void WriteCommitLock::Release() noexcept
    {
        if (locked_ && gate_ != nullptr) {
            gate_->UnlockWriteCommit();
        }

        gate_ = nullptr;
        locked_ = false;
    }
}