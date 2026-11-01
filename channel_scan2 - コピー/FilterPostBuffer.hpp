#pragma once
#pragma once

#include <vector>
#include <deque>
#include <mutex>
#include <array>
#include <cstdint>
#include <algorithm>

class FilterPostBuffer
{
public:
    FilterPostBuffer(size_t maxSize = 64) : maxBufferSize(maxSize) {}

    void Push(const std::array<uint8_t, 188>& packet)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_.push_back(packet);

        if (buffer_.size() >= maxBufferSize * 0.6)
        {
            RemoveDuplicates();
        }

        if (buffer_.size() > maxBufferSize)
        {
            buffer_.pop_front();
        }
    }

    std::vector<std::array<uint8_t, 188>> GetAll()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::vector<std::array<uint8_t, 188>>(buffer_.begin(), buffer_.end());
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_.clear();
    }

private:
    void RemoveDuplicates()
    {
        std::deque<std::array<uint8_t, 188>> unique;
        for (const auto& pkt : buffer_)
        {
            if (std::find(unique.begin(), unique.end(), pkt) == unique.end())
            {
                unique.push_back(pkt);
            }
        }
        buffer_ = std::move(unique);
    }

    std::deque<std::array<uint8_t, 188>> buffer_;
    size_t maxBufferSize;
    std::mutex mutex_;
};
