
// RingBuffer.hpp
#pragma once
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdint>

class RingBuffer {
public:
    RingBuffer(size_t capacity);
    bool Push(const uint8_t* data, size_t size);
    bool Pop(uint8_t* out, size_t size);
    size_t Available() const;
    void Clear();

private:
    std::vector<uint8_t> buffer_;
    size_t head_;
    size_t tail_;
    size_t capacity_;
    mutable std::mutex mutex_;
    std::atomic<bool> overflowed_;
};
