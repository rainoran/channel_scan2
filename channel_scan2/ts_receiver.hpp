#pragma once
#ifndef TS_RECEIVER_HPP
#define TS_RECEIVER_HPP

#include <windows.h>
#include "IBonDriver2.h"
#include "TSBuffer.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>

class TsReceiver {
public:
    TsReceiver(size_t bufferSize = 32 * 1024 * 1024); // デフォルト32MB
    ~TsReceiver();

    void Start(IBonDriver2* bonDriver, bool saveToFile = false, const std::wstring& savePath = L"");
    void Stop();
    bool IsRunning() const;

    size_t GetBufferedTSStream(std::vector<uint8_t>& outData);

private:
    void ReceiveLoop();

    IBonDriver2* bon;
    TSBuffer tsBuffer;
    std::atomic<bool> running;
    std::thread receiverThread;
    bool saveToFile;
    std::wstring outputPath;
};

#endif // TS_RECEIVER_HPP
