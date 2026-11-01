#include "PacketDumpRingBuffer.hpp"

void PacketDumpRingBuffer::Push(const PacketDumpEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex);
    buffer.push_back(entry);
}

std::vector<PacketDumpEntry> PacketDumpRingBuffer::PopAll() {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<PacketDumpEntry> result;
    result.swap(buffer);
    return result;
}

bool PacketDumpRingBuffer::IsOverThreshold() {
    std::lock_guard<std::mutex> lock(mutex);
    return buffer.size() > kFlushThreshold * 0.6;
}
