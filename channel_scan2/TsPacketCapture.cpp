#include "TsPacketCapture.hpp"

#include <chrono>
#include <thread>

bool TsPacketCaptureSession::IsCanceled_(std::atomic_bool* c) noexcept
{
    return c && c->load(std::memory_order_relaxed);
}

TsPacketCaptureCounts TsPacketCaptureSession::GetCounts() const noexcept
{
    TsPacketCaptureCounts out{};
    out.pushOk = pushOk_.load(std::memory_order_relaxed);
    out.pushFail = pushFail_.load(std::memory_order_relaxed);
    out.receiveCall = receiveCall_.load(std::memory_order_relaxed);
    out.receiveFail = receiveFail_.load(std::memory_order_relaxed);
    return out;
}

bool TsPacketCaptureSession::Start(
    IBonDriver2* bon,
    MPMCRingBuffer<TsPacket>* ringBuffer,
    const TsPacketCaptureOptions& opt,
    std::atomic_bool* cancelRequested)
{
    if (!bon || !ringBuffer) return false;
    if (!opt.receiveFn) return false;

    // 既に動いているなら拒否
    if (th_.joinable()) return false;

    bon_ = bon;
    ring_ = ringBuffer;
    opt_ = opt;
    cancel_ = cancelRequested;

    pushOk_.store(0, std::memory_order_relaxed);
    pushFail_.store(0, std::memory_order_relaxed);
    receiveCall_.store(0, std::memory_order_relaxed);
    receiveFail_.store(0, std::memory_order_relaxed);

    running_.store(true, std::memory_order_relaxed);

    // スレッド開始
    th_ = std::thread([this]() noexcept { ThreadMain_(); });
    return true;
}

void TsPacketCaptureSession::StopAndJoin() noexcept
{
    running_.store(false, std::memory_order_relaxed);

    if (th_.joinable()) {
        th_.join();
    }

    // 参照を切っておく（誤用防止）
    bon_ = nullptr;
    ring_ = nullptr;
    cancel_ = nullptr;
    opt_ = TsPacketCaptureOptions{};
}

void TsPacketCaptureSession::ThreadMain_() noexcept
{
    // Start() が成功している前提
    std::vector<TsPacket> packets;
    packets.reserve(opt_.packetReserve);

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const auto limit = std::chrono::seconds(opt_.captureSec);

    while (running_.load(std::memory_order_relaxed) &&
        !IsCanceled_(cancel_) &&
        (clock::now() - t0) < limit)
    {
        packets.clear();
        receiveCall_.fetch_add(1, std::memory_order_relaxed);

        // 受信関数は注入（cppは ts_receiver.hpp に依存しない）
        if (opt_.receiveFn(bon_, packets)) {
            for (const auto& pkt : packets) {
                if (ring_->Push(pkt)) {
                    pushOk_.fetch_add(1, std::memory_order_relaxed);
                }
                else {
                    pushFail_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        else {
            receiveFail_.fetch_add(1, std::memory_order_relaxed);

            // 過負荷を避ける
            if (opt_.sleepOnFailMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(opt_.sleepOnFailMs));
            }
        }
    }
}
