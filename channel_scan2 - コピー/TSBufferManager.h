#pragma once
#ifndef TSBUFFERMANAGER_HPP
#define TSBUFFERMANAGER_HPP

#include "TSBuffer.h"
#include "TSBufferStatus.hpp"
#include <vector>
#include <cstdint>

class TSBufferManager {
public:
    explicit TSBufferManager(size_t initialSize);
    ~TSBufferManager();

    void PushPacket(const uint8_t* data, size_t size);
    bool PopPacket(std::vector<uint8_t>& outPacket);
    void Reset();
    size_t GetBufferedSize() const;

private:
    TSBuffer tsBuffer;
};

#endif // TSBUFFERMANAGER_HPP
