// RingBuffer.cpp
#include "RingBuffer.hpp"
#include <cstring>

RingBuffer::RingBuffer(size_t capacity)
    : buffer_(capacity), capacity_(capacity), head_(0), tail_(0) {
}

bool RingBuffer::Push(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t freeSpace = (tail_ > head_)
        ? tail_ - head_ - 1
        : capacity_ - (head_ - tail_) - 1;

    if (size > freeSpace) {
        overflowed_ = true;
        return false; // バッファオーバーフロー
    }

    size_t firstPart = std::min(size, capacity_ - head_);
    std::memcpy(&buffer_[head_], data, firstPart);
    std::memcpy(&buffer_[0], data + firstPart, size - firstPart);

    head_ = (head_ + size) % capacity_;
    return true;
}

bool RingBuffer::Pop(uint8_t* out, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t available = (head_ >= tail_)
        ? head_ - tail_
        : capacity_ - (tail_ - head_);

    if (size > available) {
        return false; // 読み出しデータ不足
    }

    size_t firstPart = std::min(size, capacity_ - tail_);
    std::memcpy(out, &buffer_[tail_], firstPart);
    std::memcpy(out + firstPart, &buffer_[0], size - firstPart);

    tail_ = (tail_ + size) % capacity_;
    return true;
}

size_t RingBuffer::Available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (head_ >= tail_) ? head_ - tail_ : capacity_ - (tail_ - head_);
}

void RingBuffer::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = tail_ = 0;
    overflowed_ = false;
}
