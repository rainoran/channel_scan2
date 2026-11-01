#pragma once
#ifndef TUNER_TASK_GATE_HPP
#define TUNER_TASK_GATE_HPP

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

class TunerTaskGate final
{
public:
    enum class TaskKind : uint32_t {
        None = 0,
        ChannelScan,
        EpgLight,
        EpgFull,
        LiveView,
        TsRecordLite,
        TvTestExternal,
        Other
    };

    struct Status {
        bool busy = false;
        TaskKind kind = TaskKind::None;
        uint64_t token = 0;
        std::wstring owner;
    };

    static TunerTaskGate& Instance() noexcept
    {
        static TunerTaskGate inst;
        return inst;
    }

    // コピー禁止（シングルトン運用前提）
    TunerTaskGate(const TunerTaskGate&) = delete;
    TunerTaskGate& operator=(const TunerTaskGate&) = delete;

    bool TryAcquire(TaskKind kind, std::wstring_view owner, uint64_t& outToken) noexcept
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (kind_ != TaskKind::None) return false;

        // lock 内なので relaxed で十分
        const uint64_t tok = tokenSeq_.fetch_add(1, std::memory_order_relaxed) + 1;

        kind_ = kind;
        token_ = tok;
        owner_.assign(owner.begin(), owner.end());
        outToken = tok;
        return true;
    }

    bool Release(uint64_t token) noexcept
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (kind_ == TaskKind::None) return false;
        if (token_ != token) return false;

        kind_ = TaskKind::None;
        token_ = 0;
        owner_.clear();
        return true;
    }

    void ForceClear() noexcept
    {
        std::lock_guard<std::mutex> lk(mtx_);
        kind_ = TaskKind::None;
        token_ = 0;
        owner_.clear();
    }

    Status GetStatus() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        Status s;
        s.busy = (kind_ != TaskKind::None);
        s.kind = kind_;
        s.token = token_;
        s.owner = owner_;
        return s;
    }

    bool IsBusy() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return kind_ != TaskKind::None;
    }

private:
    TunerTaskGate() = default;

    mutable std::mutex mtx_;
    TaskKind kind_ = TaskKind::None;
    uint64_t token_ = 0;
    std::wstring owner_;

    std::atomic_uint64_t tokenSeq_{ 0 };
};

// 明示 Reset 前提（dtorは“最小限の保険”）
class TunerTaskHandle final
{
public:
    TunerTaskHandle() = default;

    // ★重要：コピー禁止（これが最大の事故要因）
    TunerTaskHandle(const TunerTaskHandle&) = delete;
    TunerTaskHandle& operator=(const TunerTaskHandle&) = delete;

    // ムーブは許可（所有権移動）
    TunerTaskHandle(TunerTaskHandle&& other) noexcept
    {
        token_ = other.token_;
        acquired_ = other.acquired_;
        other.token_ = 0;
        other.acquired_ = false;
    }

    TunerTaskHandle& operator=(TunerTaskHandle&& other) noexcept
    {
        if (this == &other) return *this;
        Reset(); // いま保持していれば先に解放

        token_ = other.token_;
        acquired_ = other.acquired_;
        other.token_ = 0;
        other.acquired_ = false;
        return *this;
    }

    bool Acquire(TunerTaskGate::TaskKind kind, std::wstring_view owner) noexcept
    {
        Reset();
        uint64_t tok = 0;
        if (!TunerTaskGate::Instance().TryAcquire(kind, owner, tok)) return false;
        token_ = tok;
        acquired_ = true;
        return true;
    }

    bool Reset() noexcept
    {
        if (!acquired_) return false;

        const bool ok = TunerTaskGate::Instance().Release(token_);
        token_ = 0;
        acquired_ = false;

        // Release が失敗しても handle 側は確実に無効化（暴走防止）
        return ok;
    }

    bool IsAcquired() const noexcept { return acquired_; }
    uint64_t Token() const noexcept { return token_; }

    // デストラクタでの自動解放は「保険」だが、あなたの方針では頼らない前提。
    // どうしても不安ならコメントアウトでもOK（MainUIは明示 Reset 済み）
    ~TunerTaskHandle() { (void)Reset(); }

private:
    uint64_t token_ = 0;
    bool acquired_ = false;
};

#endif // TUNER_TASK_GATE_HPP