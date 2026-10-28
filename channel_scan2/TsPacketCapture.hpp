#pragma once
#ifndef TS_PACKET_CAPTURE_HPP
#define TS_PACKET_CAPTURE_HPP

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include "TsStreamTypes.hpp"     // TsPacket
#include "MPMCRingBuffer.hpp"    // MPMCRingBuffer<TsPacket>

class IBonDriver2;

// 呼び出し元で見えている ReceiveTsStreamBlock を渡す（cppは知らなくてOK）
using ReceiveTsBlockFn = bool(*)(IBonDriver2* bon, std::vector<TsPacket>& outPackets);

struct TsPacketCaptureOptions
{
    int captureSec = 18;
    std::size_t packetReserve = 4096;
    int sleepOnFailMs = 1;       // Receive失敗時の負荷軽減
    ReceiveTsBlockFn receiveFn = nullptr;
};

struct TsPacketCaptureCounts
{
    std::size_t pushOk = 0;
    std::size_t pushFail = 0;
    std::size_t receiveCall = 0;
    std::size_t receiveFail = 0;
};

// 1ch分の「受信→リング投入」だけを担当（RunnerやFilterManagerは別）
class TsPacketCaptureSession
{
public:
    TsPacketCaptureSession() = default;
    TsPacketCaptureSession(const TsPacketCaptureSession&) = delete;
    TsPacketCaptureSession& operator=(const TsPacketCaptureSession&) = delete;

    // join漏れで std::terminate しないための保険
    ~TsPacketCaptureSession() { StopAndJoin(); }

    bool Start(
        IBonDriver2* bon,
        MPMCRingBuffer<TsPacket>* ringBuffer,
        const TsPacketCaptureOptions& opt,
        std::atomic_bool* cancelRequested = nullptr
    );

    void RequestStop() noexcept { running_.store(false, std::memory_order_relaxed); }

    void StopAndJoin() noexcept;

    bool IsRunning() const noexcept { return running_.load(std::memory_order_relaxed); }

    TsPacketCaptureCounts GetCounts() const noexcept;

private:
    void ThreadMain_() noexcept;
    static bool IsCanceled_(std::atomic_bool* c) noexcept;

private:
    IBonDriver2* bon_ = nullptr;
    MPMCRingBuffer<TsPacket>* ring_ = nullptr;
    TsPacketCaptureOptions opt_{};
    std::atomic_bool* cancel_ = nullptr;

    std::atomic_bool running_{ false };
    std::thread th_;

    // カウンタは他スレッドからも見えるので atomic で保持
    std::atomic_size_t pushOk_{ 0 };
    std::atomic_size_t pushFail_{ 0 };
    std::atomic_size_t receiveCall_{ 0 };
    std::atomic_size_t receiveFail_{ 0 };
};

#endif // TS_PACKET_CAPTURE_HPP
