#pragma once
#ifndef NITFILTER_HPP
#define NITFILTER_HPP

#include <vector>
#include <cstddef>
#include <cstdint>
#include "NITNetworkInfo.hpp"

class TSParser;
class PIDDataBuffer;

class NITFilter
{
public:
    NITFilter();
    NITFilter(TSParser* parser, PIDDataBuffer* buffer);

    void InputPacket(const uint8_t* data, std::size_t size);
    void Reset() noexcept;

    bool IsRecovered() const noexcept { return recovered; }
    const std::vector<NITNetworkInfo>& GetNetworkList() const noexcept { return networkList; }

    bool IsTargetPID(uint16_t pid) const noexcept;

private:
    static uint32_t ComputePsiCrc32(const uint8_t* data, size_t length) noexcept;

    void FeedBytes(const uint8_t* p, size_t n);
    void TryConsumeSections();
    void InputSection(const uint8_t* section, std::size_t length);

private:
    TSParser* tsParser = nullptr;
    PIDDataBuffer* pidBuffer = nullptr;

    std::vector<uint8_t> sectionBuffer;
    std::size_t expectedSectionSize = 0;

    // CC（簡易）
    bool haveLastCC = false;
    uint8_t lastCC = 0xFF;

    std::vector<NITNetworkInfo> networkList;
    bool recovered = false;
};

#endif // NITFILTER_HPP
