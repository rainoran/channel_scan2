#include "MPMCRingBufferTest.hpp"
#include "MPMCRingBuffer.hpp"
#include "TSStreamUtils.hpp"
#include "TsStreamTypes.hpp"
#include "DebugManager.h"
#include "BonDriverHelper.hpp"
#include "AnalyzePID.hpp"
#include "IBonDriver2.h"
#include "windows.h"

#include <thread>
#include <atomic>
#include <chrono>
using namespace TSStreamUtils;

// === A案 === 純 MPMC リングバッファ テスト ===
void RunMPMCRingBufferTest(IBonDriver2* bon)
{
    DebugLog(L"[DEBUG] MPMCリングバッファ テスト開始（純 MPMC）");

    if (!bon) {
        DebugLog(L"[ERROR] BonDriver が無効です。");
        return;
    }

    int ch = 0;
    if (!FindAndSetValidChannel_Terrestrial(bon, ch)) {
        DebugLog(L"[ERROR] 有効なチャンネルが見つかりません。");
        return;
    }

    DebugLog(L"[INFO] 信号ロック済み: %.1f dB", bon->GetSignalLevel());

    constexpr size_t bufferSize = 1024;
    MPMCRingBuffer<TsPacket> ringBuffer(bufferSize);

    std::atomic<bool> running(true);
    std::atomic<size_t> writeCount(0);
    std::atomic<size_t> writeFailCount(0);
    std::atomic<size_t> readCount(0);

    // Producer thread
    std::thread producer([&]() {
        std::vector<TsPacket> packets;

        while (running.load()) {
            if (ReceiveTsStreamBlock(bon, packets)) {
                for (const auto& packet : packets) {
                    if (!ringBuffer.Push(packet)) {
                        writeFailCount++;
                        DebugLog(L"[DEBUG] Push 失敗（バッファ満）");
                    }
                    else {
                        writeCount++;
                        DebugLog(L"[DEBUG] Push 成功：%zu", writeCount.load());
                    }
                }
            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        });


    // Consumer thread
    std::thread consumer([&]() {
        while (running.load()) {
            TsPacket pktOut;

            if (ringBuffer.Pop(pktOut)) {
                // ★ AnalyzePID 呼ばず → 純粋な Pop 動作確認
                readCount++;
                DebugLog(L"[DEBUG] Pop 成功：%zu\n", readCount.load());
            }
            else {
                std::this_thread::sleep_for(std::chrono::microseconds(800));
            }
        }
        });

    // Run test for 10 seconds
    std::this_thread::sleep_for(std::chrono::seconds(20));
    running = false;

    producer.join();
    consumer.join();

    DebugLog(L"[DEBUG] MPMC テスト終了（純 MPMC）");
    DebugLog(L"[DEBUG] [RESULT] テスト時間：10.00 秒");
    DebugLog(L"[DEBUG] [RESULT] 書き込み成功：%zu 件", writeCount.load());
    DebugLog(L"[DEBUG] [RESULT] 書き込み失敗（バッファ満）：%zu 件", writeFailCount.load());
    DebugLog(L"[DEBUG] [RESULT] 読み出し成功：%zu 件", readCount.load());
    DebugLog(L"[DEBUG] [RESULT] 差分：%zu 件", (writeCount.load() - readCount.load()));
}

// === B案 === MPMC + PID テスト ===
void RunMPMCRingBufferWithPIDTest(IBonDriver2* bon)
{
    DebugLog(L"[DEBUG] MPMCリングバッファ + PIDフィルター テスト開始（WithPID）");

    if (!bon) {
        DebugLog(L"[ERROR] BonDriver が無効です。");
        return;
    }

    int ch = 0;
    if (!FindAndSetValidChannel_Terrestrial(bon, ch)) {
        DebugLog(L"[ERROR] 有効なチャンネルが見つかりません。");
        return;
    }

    DebugLog(L"[INFO] 信号ロック済み: %.1f dB", bon->GetSignalLevel());

    constexpr size_t bufferSize = 1024;
    MPMCRingBuffer<TsPacket> ringBuffer(bufferSize);

    std::atomic<bool> running(true);
    std::atomic<size_t> writeCount(0);
    std::atomic<size_t> writeFailCount(0);
    std::atomic<size_t> readCount(0);

    // Producer thread
    std::thread producer([&]() {
        std::vector<TsPacket> packets;

        while (running.load()) {
            if (ReceiveTsStreamBlock(bon, packets)) {
                for (const auto& packet : packets) {
                    if (!ringBuffer.Push(packet)) {
                        writeFailCount++;
                        DebugLog(L"[DEBUG] Push 失敗（バッファ満）");
                    }
                    else {
                        writeCount++;
                        DebugLog(L"[DEBUG] Push 成功：%zu", writeCount.load());
                    }
                }
            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        });


    // Consumer thread
    std::thread consumer([&]() {
        AnalyzePID analyzer;

        while (running.load()) {
            TsPacket pktOut;

            if (ringBuffer.Pop(pktOut)) {
                analyzer.ProcessPacket(pktOut.data(), pktOut.size());
                readCount++;
                DebugLog(L"[DEBUG] Pop 成功（PID処理）：%zu\n", readCount.load());
            }
            else {
                std::this_thread::sleep_for(std::chrono::microseconds(800));
            }
        }
        });

    // Run test for 10 seconds
    std::this_thread::sleep_for(std::chrono::seconds(10));
    running = false;

    producer.join();
    consumer.join();

    DebugLog(L"[DEBUG] MPMC + PID テスト終了（WithPID）");
    DebugLog(L"[DEBUG] [RESULT] テスト時間：10.00 秒");
    DebugLog(L"[DEBUG] [RESULT] 書き込み成功：%zu 件", writeCount.load());
    DebugLog(L"[DEBUG] [RESULT] 書き込み失敗（バッファ満）：%zu 件", writeFailCount.load());
    DebugLog(L"[DEBUG] [RESULT] 読み出し成功（PID処理）：%zu 件", readCount.load());
    DebugLog(L"[DEBUG] [RESULT] 差分：%zu 件", (writeCount.load() - readCount.load()));
}
