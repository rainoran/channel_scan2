#pragma once
#ifndef TSBUFFER_H
#define TSBUFFER_H

#include <vector>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <algorithm>
#include "TSBufferStatus.hpp" 


class TSBuffer {
public:
    explicit TSBuffer(size_t capacity = 32 * 1024 * 1024); // 32MB default
    ~TSBuffer();

    void Initialize();
    void Reset();

    void StorePacket(const uint8_t* data, size_t size);
    size_t GetBufferedTSStream(std::vector<uint8_t>& out);
    bool WaitTsStream(int timeoutMs);

    size_t GetBufferedSize() const;
    size_t GetFreeSize() const;

    TSBufferStatus PushPacket(const uint8_t* data, size_t size);
    TSBufferStatus PopPacket(std::vector<uint8_t>& out);

private:
    std::vector<uint8_t> buffer;
    size_t capacity;
    size_t writePos;
    size_t readPos;
    size_t usedSize;

    mutable std::mutex mtx;
    std::condition_variable cv;
};
#endif // TSBUFFER_H

