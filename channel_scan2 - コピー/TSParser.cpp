#include "TSParser.h"
#include <iostream>
#include <cstring>


TSParser::TSParser(size_t bufferSize)
    : tsBuffer(bufferSize) {
    std::cout << "[INFO] TSParser initialized with buffer size: " << bufferSize << " bytes" << std::endl;
}

void TSParser::RegisterFilter(uint16_t pid, ITSFilter* filter) {
    filterMap[pid] = filter;
    std::wcout << L"[DEBUG] フィルター登録: PID = 0x" << std::hex << pid << std::dec << std::endl;
}

TSBuffer& TSParser::GetTSBuffer() {
    return tsBuffer;
}

void TSParser::ParseTS(const uint8_t* data, size_t size) {
    size_t offset = 0;
    const size_t packetSize = 188;

    while (offset + packetSize <= size) {
        const uint8_t* packet = data + offset;
        if (packet[0] != 0x47) {
            std::cerr << "[ERROR] Invalid TS sync byte at offset " << offset << std::endl;
            offset++;
            continue;
        }

        uint16_t pid = ((packet[1] & 0x1F) << 8) | packet[2];

        // Store the packet into TSBuffer (optional or logging)
        tsBuffer.StorePacket(packet, packetSize);

        auto it = filterMap.find(pid);
        if (it != filterMap.end() && it->second) {
            std::wcout << L"[DEBUG] フィルター実行: PID = 0x" << std::hex << pid << std::dec << std::endl;
            it->second->ProcessPacket(packet, packetSize); // ← 修正ポイント
        }

        offset += packetSize;
    }
}

void TSParser::DispatchPacket(const uint8_t* packet, size_t size) {
    if (size != 188 || packet[0] != 0x47) {
        std::cerr << "[ERROR] Invalid TS packet in DispatchPacket\n";
        return;
    }

    uint16_t pid = ((packet[1] & 0x1F) << 8) | packet[2];
    auto it = filterMap.find(pid);
    if (it != filterMap.end() && it->second) {
        std::wcout << L"[DEBUG] DispatchPacket: PID = 0x" << std::hex << pid << std::dec << std::endl;
        it->second->ProcessPacket(packet, size); // ← 修正ポイント
    }
}



void TSParser::DistributePackets() {
    std::vector<uint8_t> packet;
    TSBufferStatus status;

    while ((status = tsBuffer.PopPacket(packet)) == TSBufferStatus::Success) {
        if (packet.size() == 188 && packet[0] == 0x47) {
            DispatchPacket(packet.data(), packet.size());
        }
        else {
            std::cerr << "[WARN] 不正なTSパケットまたはサイズ: " << packet.size() << std::endl;
        }
    }
}

