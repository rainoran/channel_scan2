#pragma once
#ifndef TIMESHIFT_SEGMENT_RECORDER_HPP
#define TIMESHIFT_SEGMENT_RECORDER_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "TimeShift_SegmentStore.hpp"

class TimeShift_SegmentRecorder final
{
public:
    struct Settings
    {
        TimeShift_SegmentStore::Settings storeSettings{};

        // Writerスレッドへ渡す前の一時キュー上限。
        // 書き込み詰まりでメモリを食い潰さないための上限。
        std::uint64_t maxQueueBytes = 64ull * 1024ull * 1024ull;

        // true:
        //   キュー満杯時に古いブロックから捨てる。
        // false:
        //   キュー満杯時に新しいブロックを捨てる。
        bool dropOldestWhenFull = true;
    };

    struct StartRequest
    {
        int tunerIndex = -1;

        int space = 0;
        int channel = -1;

        Settings settings{};
    };

    struct Status
    {
        bool running = false;
        bool stopping = false;

        int tunerIndex = -1;
        int space = 0;
        int channel = -1;

        std::uint64_t pushedBytes = 0;
        std::uint64_t writtenBytes = 0;
        std::uint64_t droppedBytes = 0;
        std::uint64_t pushedBlocks = 0;
        std::uint64_t writtenBlocks = 0;
        std::uint64_t droppedBlocks = 0;

        std::uint64_t queueBytes = 0;
        std::size_t queueBlocks = 0;

        TimeShift_SegmentStore::Status storeStatus{};
    };

public:
    TimeShift_SegmentRecorder();
    ~TimeShift_SegmentRecorder() noexcept;

    TimeShift_SegmentRecorder(const TimeShift_SegmentRecorder&) = delete;
    TimeShift_SegmentRecorder& operator=(const TimeShift_SegmentRecorder&) = delete;

    bool Start(const StartRequest& req);
    void Stop() noexcept;
    void Reset() noexcept;

    bool IsRunning() const noexcept;

    // TS受信側から呼ぶ。
    // ここでは長時間ブロックしない。キュー上限を超える場合はdropして継続する。
    bool PushBytes(const void* data, std::size_t bytes) noexcept;

    bool PushBytes(const std::vector<std::uint8_t>& bytes) noexcept;

    Status GetStatus() const;

private:
    struct Block
    {
        std::vector<std::uint8_t> data;
    };

private:
    void WriterLoop_() noexcept;

    bool PushBlockLocked_(Block&& block) noexcept;
    void ClearQueueLocked_() noexcept;

    std::uint64_t GetQueueBytesLocked_() const noexcept;

private:
    mutable std::mutex mutex_{};
    std::condition_variable cv_{};

    Settings settings_{};
    TimeShift_SegmentStore store_{};
    TimeShift_SegmentStore::ChannelKey key_{};

    int tunerIndex_ = -1;

    bool running_ = false;
    bool stopping_ = false;

    std::thread writerThread_{};

    std::deque<Block> queue_{};
    std::uint64_t queueBytes_ = 0;

    std::atomic_uint64_t pushedBytes_{ 0 };
    std::atomic_uint64_t writtenBytes_{ 0 };
    std::atomic_uint64_t droppedBytes_{ 0 };

    std::atomic_uint64_t pushedBlocks_{ 0 };
    std::atomic_uint64_t writtenBlocks_{ 0 };
    std::atomic_uint64_t droppedBlocks_{ 0 };
};

#endif // TIMESHIFT_SEGMENT_RECORDER_HPP
