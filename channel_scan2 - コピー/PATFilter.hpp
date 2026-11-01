#pragma once
#ifndef PAT_FILTER_HPP
#define PAT_FILTER_HPP

#include <vector>
#include <map>
#include <shared_mutex>
#include <cstdint>
#include <array>

#include "TSBufferConstants.hpp"

class TSParser;
class PIDDataBuffer;

class PATFilter
{
public:
    PATFilter(TSParser* parser, PIDDataBuffer* buffer);

    void Reset();
    void InputPacket(const uint8_t* data, size_t size);
    void InputPacket(const std::array<uint8_t, TS_PACKET_SIZE>& pkt);

    bool IsRecovered() const;
    std::vector<uint16_t> GetPMTPIDs() const;

    bool IsTargetPID(uint16_t pid) const { return pid == 0x0000; }

private:
    static uint32_t ComputePsiCrc32(const uint8_t* data, size_t length) noexcept;
    void ConsumePayloadBytes(const uint8_t* p, size_t n);
    void ParsePATSection(const uint8_t* section, size_t length);

private:
    TSParser* tsParser = nullptr;
    PIDDataBuffer* tsBuffer = nullptr;

    mutable std::shared_mutex patMutex;
    std::map<uint16_t, uint16_t> programMap; // program_number -> PMT_PID

    // PSI セクション再構成
    std::vector<uint8_t> sectionBuffer;
    size_t expectedSectionSize = 0;

    // CC チェック（簡易）
    bool     haveLastCC = false;
    uint8_t  lastCC = 0xFF;
};

#endif // PAT_FILTER_HPP
