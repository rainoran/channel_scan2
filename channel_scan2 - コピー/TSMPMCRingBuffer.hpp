#pragma once
#ifndef TSMPMCRINGBUFFER_HPP
#define TSMPMCRINGBUFFER_HPP

#include <array>
#include <vector>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include "TSBufferConstants.hpp"

// ★ TSパケット型 → std::array に変更 → FilterManager と統一！
using TsPacket = std::array<uint8_t, TS_PACKET_SIZE>;

class TSMPMCRingBuffer
{
public:
    explicit TSMPMCRingBuffer(size_t size)
        : buffer(size), head(0), tail(0) {
    }

    bool Push(const TsPacket& packet)
    {
        std::unique_lock<std::shared_mutex> lock(bufferMutex);
        size_t nextHead = (head + 1) % buffer.size();
        if (nextHead == tail)
        {
            // バッファが満杯
            return false;
        }

        buffer[head] = packet;
        head = nextHead;
        return true;
    }

    bool Pop(TsPacket& packet)
    {
        std::unique_lock<std::shared_mutex> lock(bufferMutex);
        if (head == tail)
        {
            // バッファが空
            return false;
        }

        packet = buffer[tail];
        tail = (tail + 1) % buffer.size();
        return true;
    }

    void Clear()
    {
        std::unique_lock<std::shared_mutex> lock(bufferMutex);
        head = tail = 0;
    }

    size_t Size() const
    {
        std::shared_lock<std::shared_mutex> lock(bufferMutex);
        if (head >= tail)
            return head - tail;
        else
            return buffer.size() - (tail - head);
    }

    size_t Capacity() const
    {
        return buffer.size() - 1; // 1つは空けて満杯判定する
    }

private:
    std::vector<TsPacket> buffer;
    size_t head;
    size_t tail;
    mutable std::shared_mutex bufferMutex;
};

#endif // TSMPMCRINGBUFFER_HPP
