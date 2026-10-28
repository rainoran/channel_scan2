#pragma once
#ifndef ITS_FILTER_HPP
#define ITS_FILTER_HPP

#include <cstdint>
#include <vector>
#include <unordered_set>

class ITSFilter {
public:
    ITSFilter();

    void Reset();
    void InputPacket(const uint8_t* data, size_t size);
    bool IsRecovered() const;
    virtual void ProcessPacket(const uint8_t* packet, size_t size) = 0;

private:
    bool recovered = false;
    std::unordered_set<uint8_t> parsedSectionNumbers;
    std::vector<uint8_t> buffer;
    size_t bufferIndex = 0;

    void ParseSection(const uint8_t* data, size_t size);
};

#endif // ITS_FILTER_HPP
