#ifndef SDT_FILTER_HPP
#define SDT_FILTER_HPP

#include "TSParser.h"
#include "TSSectionValidator.hpp"
#include "AribDecoder.hpp"
#include "DebugManager.h"
#include "ServiceInfo.hpp"
#include "PayloadSlotBuffer.hpp"
#include "TSBufferConstants.hpp"

#include <map>
#include <vector>
#include <string>
#include <set>
#include <unordered_set>
#include <cstdint>
#include <array>

class PIDDataBuffer;

class SDTFilter
{
public:
    SDTFilter(TSParser* parser, PIDDataBuffer* buffer);

    bool IsTargetPID(uint16_t pid) const;
    void InputPacket(const std::array<uint8_t, 188>& pkt);
    bool IsRecovered() const;

    const std::vector<ServiceInfo>& GetServiceList() const;

    std::vector<ServiceInfo> GetSortedServiceList() const;

    const std::vector<std::array<uint8_t, 188>>& GetSDTPacketsAfter() const { return sdtRawPacketsAfter; }

private:
    void ParseSDT(const uint8_t* section, size_t length, const std::array<uint8_t, 188>& pkt);

private:
    TSParser* parser = nullptr;
    PIDDataBuffer* pidBuffer = nullptr;

    std::vector<ServiceInfo> serviceList;

    std::vector<uint8_t> sectionBuffer;
    std::vector<std::array<uint8_t, 188>> sdtRawPacketsAfter;

    PayloadSlotBuffer payloadSlotBuffer;

    // recovered 判定（自前）
    bool recovered = false;
    int  lastSectionNumber = -1;
    std::unordered_set<uint8_t> receivedSections;
};

#endif // SDT_FILTER_HPP
