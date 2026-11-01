
#pragma once
#ifndef PACKET_COUNTER_H
#define PACKET_COUNTER_H

#include <iostream>
#include <fstream>

class PacketCounter {
public:
    PacketCounter();

    void IncrementPacketDrop();
    void IncrementError();
    void Reset();

    int GetPacketDropCount() const;
    int GetErrorCount() const;

    void LogResults(const std::wstring& filename, bool debugMode);

private:
    int packetDropCounter;
    int errorCounter;
};

#endif // PACKET_COUNTER_H
