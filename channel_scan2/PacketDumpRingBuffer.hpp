// PacketDumpRingBuffer.hpp
#pragma once
#ifndef PACKET_DUMP_RING_BUFFER_HPP
#define PACKET_DUMP_RING_BUFFER_HPP

#include <vector>
#include <string>
#include <mutex>

struct PacketDumpEntry {
    std::wstring label;
    std::vector<uint8_t> data;
    std::wstring filename;
    bool append = true;
    bool fixedSize = false;
    uint8_t paddingValue = 0x00;
};

class PacketDumpRingBuffer {
public:
    static void Push(const PacketDumpEntry& entry);
    static std::vector<PacketDumpEntry> PopAll();
    static bool IsOverThreshold();

private:
    static inline std::vector<PacketDumpEntry> buffer;
    static inline const size_t kFlushThreshold = 1000;
    static inline std::mutex mutex;
};

#endif // PACKET_DUMP_RING_BUFFER_HPP
