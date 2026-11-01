#include "TSBufferManager.h"
#include "DebugManager.h"
#include <iostream>

TSBufferManager::TSBufferManager(size_t capacity)
    : tsBuffer(capacity) {
}

TSBufferManager::~TSBufferManager() = default;

void TSBufferManager::PushPacket(const uint8_t* data, size_t size) {
    if (!data || size != 188) {
        std::cerr << "[ERROR] Invalid packet size in PushPacket.\n";
        return;
    }
    auto status = tsBuffer.PushPacket(data, size);
    if (status != TSBufferStatus::Success) {
        std::cerr << "[WARN] TSBuffer push failed: " << static_cast<int>(status) << "\n";
    }
}

bool TSBufferManager::PopPacket(std::vector<uint8_t>& outPacket) {
    auto status = tsBuffer.PopPacket(outPacket);
    return status == TSBufferStatus::Success;
}

void TSBufferManager::Reset() {
    tsBuffer.Reset();
}

size_t TSBufferManager::GetBufferedSize() const {
    return tsBuffer.GetBufferedSize();
}
