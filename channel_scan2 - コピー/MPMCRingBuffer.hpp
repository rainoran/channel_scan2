// MPMCRingBuffer.hpp
#ifndef MPMC_RING_BUFFER_HPP
#define MPMC_RING_BUFFER_HPP

#include <Windows.h>
#include <atomic>
#include <vector>
#include <cstddef>
#include <stdexcept>
#include <chrono>
#include "DebugManager.h"

// ===== 追加トグル（既定ON推奨：薄いスピンロックでMP/MC競合を止める） =====
#ifndef MPMC_SAFE_LOCKS
#define MPMC_SAFE_LOCKS 1
#endif

template<typename T>
class MPMCRingBuffer {
public:
    explicit MPMCRingBuffer(size_t capacity)
        : buffer(capacity), capacity(capacity), head(0), tail(0)
    {
        if ((capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("Capacity must be a power of 2");
        }
    }

    // 満杯なら false。落ちない・壊れないことを最優先
    bool Push(const T& item) {
        auto t_start = std::chrono::high_resolution_clock::now();

#if MPMC_SAFE_LOCKS
        SpinLock push_guard(pushLock);
#endif
        const size_t currentTail = tail.load(std::memory_order_relaxed);
        const size_t nextTail = Increment(currentTail);

        // 満杯判定
        if (nextTail == head.load(std::memory_order_acquire)) {
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - t_start).count();
            if (duration > 100) {
                DebugLogFormat(L"[MPMC] Push失敗 (満) 時間: %lldμs", (long long)duration);
            }
            return false;
        }

        // スロット書き込み（Tが非trivialでもOK）
        buffer[currentTail] = item;

        // tailを前進
        tail.store(nextTail, std::memory_order_release);

        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - t_start).count();
        if (duration > 100) {
            DebugLogFormat(L"[MPMC] Push成功 時間: %lldμs", (long long)duration);
        }
        return true;
    }

    // 期限付きPush（必要なら呼び出し側を最小変更でタイムアウト制御可能）
    template<class Rep, class Period>
    bool TryPushFor(const T& item, const std::chrono::duration<Rep, Period>& timeout) {
        const auto deadline = std::chrono::high_resolution_clock::now() + timeout;
        do {
            if (Push(item)) return true;
            // 満杯なら短い休止でスピン抑制（Windows固有でも標準関数でもOK）
            YieldShort_();
        } while (std::chrono::high_resolution_clock::now() < deadline);
        return false;
    }

    bool Pop(T& item) {
#if MPMC_SAFE_LOCKS
        SpinLock pop_guard(popLock);
#endif
        const size_t currentHead = head.load(std::memory_order_relaxed);
        if (currentHead == tail.load(std::memory_order_acquire)) {
            return false; // 空
        }

        item = buffer[currentHead];
        head.store(Increment(currentHead), std::memory_order_release);
        return true;
    }

    bool Peek(T& item) const {
        const size_t currentHead = head.load(std::memory_order_acquire);
        if (currentHead == tail.load(std::memory_order_acquire))
            return false;
        item = buffer[currentHead];
        return true;
    }

    bool IsEmpty() const {
        return head.load(std::memory_order_acquire) == tail.load(std::memory_order_acquire);
    }
    bool IsFull() const {
        return Increment(tail.load(std::memory_order_acquire)) == head.load(std::memory_order_acquire);
    }

    // 注意：稼働中に Clear は基本禁止。テストや停止時のみ使用推奨。
    void Clear() {
#if MPMC_SAFE_LOCKS
        SpinLock push_guard(pushLock);
        SpinLock pop_guard(popLock);
#endif
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
    }

    size_t Capacity() const { return capacity; }

    size_t Size() const {
        const size_t h = head.load(std::memory_order_acquire);
        const size_t t = tail.load(std::memory_order_acquire);
        return (t + capacity - h) & (capacity - 1);
    }

private:
    // ====== 追加：超軽量スピンロック（false sharing 回避のためパディング）======
#if MPMC_SAFE_LOCKS
    struct SpinLock {
        explicit SpinLock(std::atomic_flag& f) : flag(f) { lock(); }
        ~SpinLock() { unlock(); }
        void lock() {
            // 短い指数バックオフでCPU燃焼を抑制
            for (uint32_t spins = 1;; spins = (spins < 32 ? spins << 1 : 32)) {
                for (uint32_t i = 0; i < spins; ++i) {
                    if (!flag.test_and_set(std::memory_order_acquire)) return;
                    YieldProcessor_(); // very short pause
                }
                // スピン拡大の合間に OS へも一瞬譲る
                ::SwitchToThread();
            }
        }
        void unlock() { flag.clear(std::memory_order_release); }
        std::atomic_flag& flag;
    };
    static inline void YieldProcessor_() noexcept {
        // x86ならPAUSE、なければ Sleep(0) 相当
#if defined(_M_IX86) || defined(_M_X64)
        ::YieldProcessor(); // <Windows.h>
#else
        ::SwitchToThread();
#endif
    }
    static inline void YieldShort_() noexcept {
        // 微小休止（log連打やCPU燃焼を抑止）
        YieldProcessor_();
    }

    // push/pop 用の独立ロック（false sharing防止のため64B境界）
    alignas(64) mutable std::atomic_flag pushLock = ATOMIC_FLAG_INIT;
    alignas(64) mutable std::atomic_flag popLock = ATOMIC_FLAG_INIT;
#endif

    size_t Increment(size_t i) const { return (i + 1) & (capacity - 1); }

    std::vector<T> buffer;
    const size_t capacity;

    // head/tail は既にキャッシュライン分離済み
    alignas(64) std::atomic<size_t> head;
    alignas(64) std::atomic<size_t> tail;
};

#endif // MPMC_RING_BUFFER_HPP
