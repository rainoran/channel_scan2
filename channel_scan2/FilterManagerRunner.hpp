#pragma once
#ifndef FILTER_MANAGER_RUNNER_HPP
#define FILTER_MANAGER_RUNNER_HPP

// C++20: 無制限のraw TS履歴を排除したFilterManagerRunner。
// TS解析はMPMCリングから取り出したパケットを従来通り全件処理する。
// 診断用TS履歴が必要なときだけ、上限付き循環配列へコピーする。
// Win32 / x64両対応。GetExtractedPackets()はStop()後にのみ参照すること。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <thread>
#include <vector>

#include "MPMCRingBuffer.hpp"
#include "FilterManager.hpp"
#include "TsRingMemoryPolicy.hpp"

#if defined(_M_IX86)
#include "Memory_policy.hpp"
#endif

class FilterManagerRunner final
{
public:
    using TsPacket = std::array<std::uint8_t, TS_PACKET_SIZE>;
    using Ring = MPMCRingBuffer<TsPacket>;

    enum class WorkerError : std::uint8_t
    {
        None = 0,
        InvalidArguments = 1,
        HistoryAllocation = 2,
        BadAlloc = 3,
        StandardException = 4,
        UnknownException = 5,
        ThreadStart = 6
    };

    explicit FilterManagerRunner(
        FilterManager* manager,
        Ring* ring,
        std::size_t historyPackets = TSCEPG::TsRingMemoryPolicy::kDefaultHistoryPackets) noexcept
        : filterManager_(manager),
        ringBuffer_(ring),
        historyCapacity_(TSCEPG::TsRingMemoryPolicy::NormalizeHistoryPackets(historyPackets))
    {
    }

    FilterManagerRunner(const FilterManagerRunner&) = delete;
    FilterManagerRunner& operator=(const FilterManagerRunner&) = delete;

    ~FilterManagerRunner() noexcept
    {
        // 通常経路はStop()を明示実行。デストラクタは異常経路の保険。
        Stop();
    }

    void Start() noexcept
    {
        if (worker_.joinable()) return;

        if (filterManager_ == nullptr || ringBuffer_ == nullptr) {
            workerError_.store(WorkerError::InvalidArguments, std::memory_order_release);
            return;
        }

        StopAndClearHistory_();
        workerError_.store(WorkerError::None, std::memory_order_release);
        processedPackets_.store(0, std::memory_order_relaxed);
        overwrittenHistoryPackets_.store(0, std::memory_order_relaxed);
        stopFlag_.store(false, std::memory_order_release);

        if (historyCapacity_ != 0) {
            try {
                // 診断リングは実行前に一括確保し、ホットパスで再確保しない。
                extractedPackets_.resize(historyCapacity_);
            }
            catch (const std::bad_alloc&) {
                RecordBadAlloc_(L"TS_RUNNER_HISTORY_ALLOC");
                workerError_.store(WorkerError::HistoryAllocation, std::memory_order_release);
                return;
            }
            catch (...) {
                workerError_.store(WorkerError::HistoryAllocation, std::memory_order_release);
                return;
            }
        }

        try {
            worker_ = std::thread([this]() noexcept { Run_(); });
        }
        catch (...) {
            workerError_.store(WorkerError::ThreadStart, std::memory_order_release);
            stopFlag_.store(true, std::memory_order_release);
        }
    }

    void Stop() noexcept
    {
        stopFlag_.store(true, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
        FinalizeHistory_();
    }

    // ストリームが止まった状態だけで呼ぶこと。
    void Reset() noexcept
    {
        Stop();
        std::vector<TsPacket>().swap(extractedPackets_); // メモリを明示解放
        historyCount_ = 0;
        historyNext_ = 0;
        historyFinalized_ = true;
        processedPackets_.store(0, std::memory_order_relaxed);
        overwrittenHistoryPackets_.store(0, std::memory_order_relaxed);
        workerError_.store(WorkerError::None, std::memory_order_release);
    }

    [[nodiscard]] WorkerError GetWorkerError() const noexcept
    {
        return workerError_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool HasWorkerError() const noexcept
    {
        return GetWorkerError() != WorkerError::None;
    }

    [[nodiscard]] std::uint64_t GetProcessedPackets() const noexcept
    {
        return processedPackets_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t GetOverwrittenHistoryPackets() const noexcept
    {
        return overwrittenHistoryPackets_.load(std::memory_order_relaxed);
    }

    // 既存I/Fを維持。Stop()後は古いものから新しいものの順。
    // 診断履歴を無効化した場合は空vector。
    // 実行中の参照はデータ競合になるため禁止。
    [[nodiscard]] const std::vector<TsPacket>& GetExtractedPackets() const noexcept
    {
        return extractedPackets_;
    }

private:
    static void RecordBadAlloc_(const wchar_t* stage) noexcept
    {
#if defined(_M_IX86)
        TSCEPG::MemoryPolicy::AllocationFailure(stage);
#else
        (void)stage;
#endif
    }

    void StopAndClearHistory_() noexcept
    {
        extractedPackets_.clear(); // 容量は再利用
        historyCount_ = 0;
        historyNext_ = 0;
        historyFinalized_ = false;
    }

    // worker停止後にのみ使用。循環配列の物理順を時系列順にする。
    void FinalizeHistory_() noexcept
    {
        if (historyFinalized_) return;
        if (historyCapacity_ == 0) {
            extractedPackets_.clear();
        }
        else if (historyCount_ < historyCapacity_) {
            extractedPackets_.resize(historyCount_); // 縮小のみ、追加確保なし
        }
        else if (historyNext_ != 0) {
            std::rotate(extractedPackets_.begin(),
                extractedPackets_.begin() + static_cast<std::ptrdiff_t>(historyNext_),
                extractedPackets_.end());
        }
        historyFinalized_ = true;
    }

    void RecordPacket_(const TsPacket& pkt) noexcept
    {
        if (historyCapacity_ == 0) return;

        extractedPackets_[historyNext_] = pkt;
        ++historyNext_;
        if (historyNext_ == historyCapacity_) historyNext_ = 0;

        if (historyCount_ < historyCapacity_) {
            ++historyCount_;
        }
        else {
            overwrittenHistoryPackets_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void Run_() noexcept
    {
        try {
            TsPacket pkt{};
            while (!stopFlag_.load(std::memory_order_acquire)) {
                if (ringBuffer_->Pop(pkt)) {
                    RecordPacket_(pkt);               // 診断用の有限履歴（任意）
                    filterManager_->InputPacket(pkt); // TS解析は全パケット実行
                    processedPackets_.fetch_add(1, std::memory_order_relaxed);
                }
                else {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            }
        }
        catch (const std::bad_alloc&) {
            RecordBadAlloc_(L"TS_RUNNER_FILTER_BAD_ALLOC");
            workerError_.store(WorkerError::BadAlloc, std::memory_order_release);
            stopFlag_.store(true, std::memory_order_release);
        }
        catch (const std::exception&) {
            workerError_.store(WorkerError::StandardException, std::memory_order_release);
            stopFlag_.store(true, std::memory_order_release);
        }
        catch (...) {
            workerError_.store(WorkerError::UnknownException, std::memory_order_release);
            stopFlag_.store(true, std::memory_order_release);
        }
    }

    FilterManager* filterManager_ = nullptr; // 所有しない。Stop()まで寿命を保証する。
    Ring* ringBuffer_ = nullptr;             // 同上。
    std::thread worker_{};
    std::atomic<bool> stopFlag_{ true };
    std::atomic<WorkerError> workerError_{ WorkerError::None };
    std::atomic<std::uint64_t> processedPackets_{ 0 };
    std::atomic<std::uint64_t> overwrittenHistoryPackets_{ 0 };

    std::size_t historyCapacity_ = 0;
    std::size_t historyCount_ = 0;
    std::size_t historyNext_ = 0;
    bool historyFinalized_ = true;
    std::vector<TsPacket> extractedPackets_;
};

#endif // FILTER_MANAGER_RUNNER_HPP
