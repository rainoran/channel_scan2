#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "TimeShift_SegmentRecorder.hpp"

#include <algorithm>
#include <utility>

TimeShift_SegmentRecorder::TimeShift_SegmentRecorder() = default;

TimeShift_SegmentRecorder::~TimeShift_SegmentRecorder() noexcept
{
    Stop();
}

bool TimeShift_SegmentRecorder::Start(const StartRequest& req)
{
    if (req.space < 0 || req.channel < 0) {
        return false;
    }

    Stop();

    {
        std::lock_guard<std::mutex> lock(mutex_);

        settings_ = req.settings;

        if (settings_.maxQueueBytes < 4ull * 1024ull * 1024ull) {
            settings_.maxQueueBytes = 4ull * 1024ull * 1024ull;
        }

        key_.space = req.space;
        key_.channel = req.channel;

        tunerIndex_ = req.tunerIndex;

        pushedBytes_.store(0);
        writtenBytes_.store(0);
        droppedBytes_.store(0);
        pushedBlocks_.store(0);
        writtenBlocks_.store(0);
        droppedBlocks_.store(0);

        ClearQueueLocked_();

        stopping_ = false;
        running_ = false;
    }

    store_.Configure(settings_.storeSettings);

    if (!store_.StartChannel(key_)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = true;
    }

    try {
        writerThread_ = std::thread(&TimeShift_SegmentRecorder::WriterLoop_, this);
    }
    catch (...) {
        Stop();
        return false;
    }

    return true;
}

void TimeShift_SegmentRecorder::Stop() noexcept
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && !writerThread_.joinable()) {
            store_.StopChannel(key_);
            return;
        }

        stopping_ = true;
    }

    cv_.notify_all();

    if (writerThread_.joinable()) {
        try {
            writerThread_.join();
        }
        catch (...) {
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        stopping_ = false;
        ClearQueueLocked_();
    }

    store_.StopChannel(key_);
}

void TimeShift_SegmentRecorder::Reset() noexcept
{
    Stop();

    std::lock_guard<std::mutex> lock(mutex_);

    pushedBytes_.store(0);
    writtenBytes_.store(0);
    droppedBytes_.store(0);
    pushedBlocks_.store(0);
    writtenBlocks_.store(0);
    droppedBlocks_.store(0);

    ClearQueueLocked_();
}

bool TimeShift_SegmentRecorder::IsRunning() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ && !stopping_;
}

bool TimeShift_SegmentRecorder::PushBytes(const void* data, std::size_t bytes) noexcept
{
    if (data == nullptr || bytes == 0) {
        return false;
    }

    Block block{};

    try {
        block.data.resize(bytes);
        const auto* src = static_cast<const std::uint8_t*>(data);
        std::copy(src, src + bytes, block.data.begin());
    }
    catch (...) {
        droppedBytes_.fetch_add(static_cast<std::uint64_t>(bytes));
        droppedBlocks_.fetch_add(1);
        return false;
    }

    bool ok = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_ || stopping_) {
            droppedBytes_.fetch_add(static_cast<std::uint64_t>(bytes));
            droppedBlocks_.fetch_add(1);
            return false;
        }

        ok = PushBlockLocked_(std::move(block));
    }

    if (ok) {
        pushedBytes_.fetch_add(static_cast<std::uint64_t>(bytes));
        pushedBlocks_.fetch_add(1);
        cv_.notify_one();
    }
    else {
        droppedBytes_.fetch_add(static_cast<std::uint64_t>(bytes));
        droppedBlocks_.fetch_add(1);
    }

    return ok;
}

bool TimeShift_SegmentRecorder::PushBytes(
    const std::vector<std::uint8_t>& bytes) noexcept
{
    if (bytes.empty()) {
        return false;
    }

    return PushBytes(bytes.data(), bytes.size());
}

TimeShift_SegmentRecorder::Status
TimeShift_SegmentRecorder::GetStatus() const
{
    Status st{};
    TimeShift_SegmentStore::ChannelKey keyCopy{};

    {
        std::lock_guard<std::mutex> lock(mutex_);

        st.running = running_;
        st.stopping = stopping_;

        st.tunerIndex = tunerIndex_;
        st.space = key_.space;
        st.channel = key_.channel;

        st.queueBytes = queueBytes_;
        st.queueBlocks = queue_.size();

        keyCopy = key_;
    }

    st.pushedBytes = pushedBytes_.load();
    st.writtenBytes = writtenBytes_.load();
    st.droppedBytes = droppedBytes_.load();

    st.pushedBlocks = pushedBlocks_.load();
    st.writtenBlocks = writtenBlocks_.load();
    st.droppedBlocks = droppedBlocks_.load();

    st.storeStatus = store_.GetStatus(keyCopy);

    return st;
}

void TimeShift_SegmentRecorder::WriterLoop_() noexcept
{
    for (;;) {
        Block block{};

        {
            std::unique_lock<std::mutex> lock(mutex_);

            cv_.wait(
                lock,
                [this]()
                {
                    return stopping_ || !queue_.empty();
                });

            if (queue_.empty()) {
                if (stopping_) {
                    break;
                }
                continue;
            }

            block = std::move(queue_.front());
            queue_.pop_front();

            if (queueBytes_ >= block.data.size()) {
                queueBytes_ -= static_cast<std::uint64_t>(block.data.size());
            }
            else {
                queueBytes_ = 0;
            }
        }

        if (block.data.empty()) {
            continue;
        }

        const bool ok =
            store_.WriteBytes(
                key_,
                block.data.data(),
                block.data.size());

        if (ok) {
            writtenBytes_.fetch_add(static_cast<std::uint64_t>(block.data.size()));
            writtenBlocks_.fetch_add(1);
        }
        else {
            droppedBytes_.fetch_add(static_cast<std::uint64_t>(block.data.size()));
            droppedBlocks_.fetch_add(1);
        }
    }

    // Stop時に残りキューを可能な範囲で書き出す。
    for (;;) {
        Block block{};

        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (queue_.empty()) {
                queueBytes_ = 0;
                break;
            }

            block = std::move(queue_.front());
            queue_.pop_front();

            if (queueBytes_ >= block.data.size()) {
                queueBytes_ -= static_cast<std::uint64_t>(block.data.size());
            }
            else {
                queueBytes_ = 0;
            }
        }

        if (block.data.empty()) {
            continue;
        }

        const bool ok =
            store_.WriteBytes(
                key_,
                block.data.data(),
                block.data.size());

        if (ok) {
            writtenBytes_.fetch_add(static_cast<std::uint64_t>(block.data.size()));
            writtenBlocks_.fetch_add(1);
        }
        else {
            droppedBytes_.fetch_add(static_cast<std::uint64_t>(block.data.size()));
            droppedBlocks_.fetch_add(1);
        }
    }

    store_.Rotate(key_);
    store_.StopChannel(key_);
}

bool TimeShift_SegmentRecorder::PushBlockLocked_(Block&& block) noexcept
{
    const std::uint64_t blockBytes =
        static_cast<std::uint64_t>(block.data.size());

    if (blockBytes == 0) {
        return false;
    }

    if (blockBytes > settings_.maxQueueBytes) {
        return false;
    }

    if (!settings_.dropOldestWhenFull) {
        if (queueBytes_ + blockBytes > settings_.maxQueueBytes) {
            return false;
        }

        queueBytes_ += blockBytes;
        queue_.push_back(std::move(block));
        return true;
    }

    while (!queue_.empty() &&
        queueBytes_ + blockBytes > settings_.maxQueueBytes) {
        const std::uint64_t oldBytes =
            static_cast<std::uint64_t>(queue_.front().data.size());

        if (queueBytes_ >= oldBytes) {
            queueBytes_ -= oldBytes;
        }
        else {
            queueBytes_ = 0;
        }

        droppedBytes_.fetch_add(oldBytes);
        droppedBlocks_.fetch_add(1);

        queue_.pop_front();
    }

    if (queueBytes_ + blockBytes > settings_.maxQueueBytes) {
        return false;
    }

    queueBytes_ += blockBytes;
    queue_.push_back(std::move(block));

    return true;
}

void TimeShift_SegmentRecorder::ClearQueueLocked_() noexcept
{
    queue_.clear();
    queueBytes_ = 0;
}

std::uint64_t TimeShift_SegmentRecorder::GetQueueBytesLocked_() const noexcept
{
    return queueBytes_;
}