#pragma once
// FilterManagerRunner.hpp
#pragma once
#include <thread>
#include <atomic>
#include "MPMCRingBuffer.hpp"
#include "FilterManager.hpp"

class FilterManagerRunner {
public:
    FilterManagerRunner(FilterManager* manager, MPMCRingBuffer<std::array<uint8_t, TS_PACKET_SIZE>>* ring)
        : filterManager(manager), ringBuffer(ring), stopFlag(false) {
    }

    void Start() {
        worker = std::thread([this]() { this->Run(); });
    }

    void Stop() {
        stopFlag.store(true);
        if (worker.joinable()) {
            worker.join();
        }
    }

    ~FilterManagerRunner() {
        Stop();
    }

    const std::vector<std::array<uint8_t, TS_PACKET_SIZE>>& GetExtractedPackets() const {
        return extractedPackets;
    }

private:
    void Run() {
        std::array<uint8_t, TS_PACKET_SIZE> pkt;
        while (!stopFlag.load()) {
            if (ringBuffer->Pop(pkt)) {
                // ✅ デバッグ出力せず破損調査用に保存（PID振り分け前）
                extractedPackets.push_back(pkt);

                // フィルター処理（PIDに応じた振り分け）
                filterManager->InputPacket(pkt);
            }
            else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));  // 軽い待機
            }
        }
    }

    FilterManager* filterManager;
    MPMCRingBuffer<std::array<uint8_t, TS_PACKET_SIZE>>* ringBuffer;
    std::thread worker;
    std::atomic<bool> stopFlag;

    std::vector<std::array<uint8_t, TS_PACKET_SIZE>> extractedPackets;
};
