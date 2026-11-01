#include "PacketCounter.h"

PacketCounter::PacketCounter() : packetDropCounter(0), errorCounter(0) {}

void PacketCounter::IncrementPacketDrop() {
    packetDropCounter++;
}

void PacketCounter::IncrementError() {
    errorCounter++;
}

void PacketCounter::Reset() {
    packetDropCounter = 0;
    errorCounter = 0;
}

int PacketCounter::GetPacketDropCount() const {
    return packetDropCounter;
}

int PacketCounter::GetErrorCount() const {
    return errorCounter;
}

void PacketCounter::LogResults(const std::wstring& filename, bool debugMode) {
    std::wofstream logFile(filename, std::ios::app);
    if (!logFile) {
        std::wcerr << L"[ERROR] Failed to open log file: " << filename << std::endl;
        return;
    }

    logFile << L"Packet Drops: " << packetDropCounter << L"\n";
    logFile << L"Errors: " << errorCounter << L"\n";
    logFile.close();

    if (debugMode) {
        std::wcout << L"[DEBUG] Packet Drops: " << packetDropCounter << L", Errors: " << errorCounter << std::endl;
    }
}
