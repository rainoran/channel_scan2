#pragma once
#ifndef PMTFILTER_HPP
#define PMTFILTER_HPP

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <vector>
#include <shared_mutex>

#include "TSBufferConstants.hpp"

class TSParser;
class PIDDataBuffer;

class PMTFilter
{
public:
    PMTFilter();
    PMTFilter(TSParser* parser, PIDDataBuffer* buffer);

    void Reset();

    void AddTargetPID(uint16_t pid);
    bool IsTargetPID(uint16_t pid) const;

    void InputPacket(const uint8_t* data, size_t size);

    bool IsRecovered() const;

    bool WasLastValidationSuccessful() const;
    size_t GetValidationFailCount() const;

private:
    static uint32_t ComputePsiCrc32(const uint8_t* data, size_t length) noexcept;
    void ConsumePayloadBytes(uint16_t pid, const uint8_t* p, size_t n);
    void ParsePMTSection(const uint8_t* section, size_t length);

private:
    TSParser* tsParser = nullptr;
    PIDDataBuffer* pidBuffer = nullptr;

    // elementary_pid -> stream_type（最低限の成果物）
    std::map<uint16_t, uint8_t> streamMap;

    std::unordered_set<uint16_t> targetPIDs;

    struct ReasmContext {
        std::vector<uint8_t> buf;
        size_t expected = 0;
        bool haveLastCC = false;
        uint8_t lastCC = 0xFF;
    };
    std::unordered_map<uint16_t, ReasmContext> reasm; // key: PMT PID

    mutable std::shared_mutex mtx;

    bool recovered = false;
    bool lastValidationSuccess = false;
    size_t validationFailCount = 0;
};

#endif // PMTFILTER_HPP
