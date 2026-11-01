#define NOMINMAX
#include "TSBuffer.h"
#include "DebugManager.h"
#include <algorithm>
#include <cstring>
#include <chrono>
#include <thread>


TSBuffer::TSBuffer(size_t bufferSize)
    : buffer(bufferSize), capacity(bufferSize), readPos(0), writePos(0), usedSize(0) {
}

TSBuffer::~TSBuffer() {}

void TSBuffer::Initialize() {
    Reset();
}

void TSBuffer::StorePacket(const uint8_t* data, size_t size) {
    std::unique_lock<std::mutex> lock(mtx);
    DebugLog((L"[TSBuffer] StorePacket 呼び出し, size = " + std::to_wstring(size)).c_str());

    size_t freeSize = capacity - usedSize;
    if (size > freeSize) {
        DebugLog(L"[TSBuffer] バッファオーバーフロー - パケットを破棄");
        return;
    }

    size_t firstPart = std::min(size, capacity - writePos);
    std::memcpy(&buffer[writePos], data, firstPart);
    std::memcpy(&buffer[0], data + firstPart, size - firstPart);
    writePos = (writePos + size) % capacity;
    usedSize += size;

    cv.notify_one();
}

size_t TSBuffer::GetBufferedTSStream(std::vector<uint8_t>& out) {
    std::unique_lock<std::mutex> lock(mtx);
    size_t packetSize = 188;

    if (usedSize < packetSize) return 0;

    out.resize(packetSize);
    size_t firstPart = std::min(packetSize, capacity - readPos);
    std::memcpy(out.data(), &buffer[readPos], firstPart);
    std::memcpy(out.data() + firstPart, &buffer[0], packetSize - firstPart);

    readPos = (readPos + packetSize) % capacity;
    usedSize -= packetSize;

    return packetSize;
}

bool TSBuffer::WaitTsStream(int timeoutMs) {
    std::unique_lock<std::mutex> lock(mtx);
    return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return usedSize > 0; });
}

size_t TSBuffer::GetBufferedSize() const {
    std::unique_lock<std::mutex> lock(mtx);
    return usedSize;
}

size_t TSBuffer::GetFreeSize() const {
    std::unique_lock<std::mutex> lock(mtx);
    return capacity - usedSize;
}

void TSBuffer::Reset() {
    std::unique_lock<std::mutex> lock(mtx);
    readPos = 0;
    writePos = 0;
    usedSize = 0;
}



TSBufferStatus TSBuffer::PushPacket(const uint8_t* data, size_t size) {
    std::unique_lock<std::mutex> lock(mtx);

    if (size > capacity) {
        DebugLog(L"[TSBuffer] PushPacket: データサイズがバッファ容量を超えています");
        return TSBufferStatus::OverFlow;
    }

    size_t freeSize = capacity - usedSize;
    if (size > freeSize) {
        DebugLog(L"[TSBuffer] PushPacket: 空き容量不足によりドロップ");
        return TSBufferStatus::OverFlow;
    }

    size_t firstPart = std::min(size, capacity - writePos);
    std::memcpy(&buffer[writePos], data, firstPart);
    if (size > firstPart) {
        std::memcpy(&buffer[0], data + firstPart, size - firstPart);
    }

    writePos = (writePos + size) % capacity;
    usedSize += size;

    cv.notify_one();
    return TSBufferStatus::Success;
}

TSBufferStatus TSBuffer::PopPacket(std::vector<uint8_t>& out) {
    std::unique_lock<std::mutex> lock(mtx);
    const size_t packetSize = 188;

    if (usedSize < packetSize) {
        return TSBufferStatus::UnderFlow;
    }

    out.resize(packetSize);
    size_t firstPart = std::min(packetSize, capacity - readPos);
    std::memcpy(out.data(), &buffer[readPos], firstPart);
    if (packetSize > firstPart) {
        std::memcpy(out.data() + firstPart, &buffer[0], packetSize - firstPart);
    }

    readPos = (readPos + packetSize) % capacity;
    usedSize -= packetSize;

    return TSBufferStatus::Success;
}
