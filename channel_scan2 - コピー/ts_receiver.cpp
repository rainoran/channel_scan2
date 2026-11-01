#include "ts_receiver.hpp"
#include "DebugManager.h"
#include <chrono>
#include <thread>
#include <fstream>

TsReceiver::TsReceiver(size_t bufferSize)
    : bon(nullptr), tsBuffer(bufferSize), running(false), saveToFile(false) {
}

TsReceiver::~TsReceiver() {
    Stop();
}

void TsReceiver::Start(IBonDriver2* bonDriver, bool saveToFile, const std::wstring& savePath) {
    this->bon = bonDriver;
    this->saveToFile = saveToFile;
    this->outputPath = savePath;
    this->tsBuffer.Reset();
    this->running = true;

    receiverThread = std::thread(&TsReceiver::ReceiveLoop, this);

    DebugLog(L"[DEBUG] Start: saveToFile=");
    DebugLog(saveToFile ? L"true" : L"false");
    DebugLog(L"[DEBUG] 出力パス: ");
    DebugLog(savePath.c_str());
}

void TsReceiver::Stop() {
    if (running.exchange(false)) {
        if (receiverThread.joinable()) {
            receiverThread.join();
        }
    }
}

bool TsReceiver::IsRunning() const {
    return running.load();
}

size_t TsReceiver::GetBufferedTSStream(std::vector<uint8_t>& outData) {
    return tsBuffer.GetBufferedTSStream(outData);
}

void TsReceiver::ReceiveLoop()
{
    DebugLog(L"[DEBUG] ReceiveLoop: 開始");

    // ロック後の安定待機
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    if (bon)
    {
        float level = bon->GetSignalLevel();
        DebugLogFormat(L"[INFO] 現在の信号レベル: %.2f dB", level);
    }

    constexpr int maxAttempts = 5000;  // 約5秒 (1回10ms)
    constexpr int pollIntervalMs = 10;

    for (int i = 0; i < maxAttempts && running; ++i)
    {
        BYTE* pSrc = nullptr;
        DWORD size = 0, remain = 0;

        if (bon->GetTsStream(&pSrc, &size, &remain) && pSrc && size > 0)
        {
            DebugLogFormat(L"[DEBUG] GetTsStream 成功: size=%lu, remain=%lu", size, remain);
            auto status = tsBuffer.PushPacket(reinterpret_cast<const uint8_t*>(pSrc), static_cast<size_t>(size));
            if (status != TSBufferStatus::Success)
            {
                DebugLogFormat(L"[WARN] PushPacket に失敗: ステータス = %d", static_cast<int>(status));
            }
        }
        else
        {
            DebugLog(L"[TRACE] GetTsStream(): データなし, 次回へ");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
    }

    if (!running)
    {
        DebugLog(L"[INFO] スレッド停止要求を検出しました");
    }

    DebugLog(L"[DEBUG] ReceiveLoop: 終了");
}



