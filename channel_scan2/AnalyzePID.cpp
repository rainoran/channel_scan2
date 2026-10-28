#include "AnalyzePID.hpp"
#include "CPUFeature.h"
#include "DebugManager.h"

#include <immintrin.h>
#include <iostream>
#include <iomanip>
#include <mutex>        // unique_lock / shared_lock
#include <algorithm>    // min

AnalyzePID::AnalyzePID()
    : optimizedProcessPacket(nullptr)
{
    // 連続ストリーム中の std::cout は詰まりやすいので、必要なら DebugManager 経由に寄せるのがおすすめ
    // std::cout << "[INFO] AnalyzePID initialized." << std::endl;
}

void AnalyzePID::InitAnalyzePID()
{
    const auto& features = CPUFeature::GetInstance();

    if (features.hasAVX2) {
        optimizedProcessPacket = &AnalyzePID::ProcessPacket_AVX2;
    }
    else if (features.hasSSE4_2) {
        optimizedProcessPacket = &AnalyzePID::ProcessPacket_SSE4_2;
    }
    else if (features.hasSSE2) {
        optimizedProcessPacket = &AnalyzePID::ProcessPacket_SSE2;
    }
    else {
        optimizedProcessPacket = &AnalyzePID::ProcessPacket_Scalar;
    }

    // DebugManager をここで毎回生成するのは重い可能性があるので、
    // 可能なら DebugManager をどこかで共有（Singleton等）するのが理想。
    DebugManager debugManager(ConfigManager::GetInstance());
    if (debugManager.IsDebugEnabled()) {
        std::cout << "[INFO] Using "
            << (features.hasAVX2 ? "AVX2" : features.hasSSE4_2 ? "SSE4.2" : features.hasSSE2 ? "SSE2" : "Scalar")
            << " for PID processing." << std::endl;
    }
}

void AnalyzePID::ProcessPacket(const uint8_t* tsPacket, size_t packetSize)
{
    if (!optimizedProcessPacket) {
        InitAnalyzePID();
    }
    (this->*optimizedProcessPacket)(tsPacket, packetSize);
}

size_t AnalyzePID::DetectUnit(const uint8_t* data, size_t size) noexcept
{
    // 2パケット分の先頭syncで推定
    if (size >= kTs204 * 2 && data[0] == 0x47 && data[kTs204] == 0x47) return kTs204;
    if (size >= kTs188 * 2 && data[0] == 0x47 && data[kTs188] == 0x47) return kTs188;
    return kTs188; // フォールバック（不明時は188扱い）
}

void AnalyzePID::ProcessPacket_Scalar(const uint8_t* tsPacket, size_t packetSize)
{
    if (tsPacket == nullptr || packetSize < kTs188) {
        return;
    }

    const size_t unit = DetectUnit(tsPacket, packetSize);

    // C6262対策:
    // std::array<uint32_t, 0x2000> をスタックに置くと約32KB使う。
    // 連続ストリーム処理なので、毎回heap確保しないよう thread_local で保持する。
    thread_local std::vector<uint32_t> localCount;
    thread_local std::vector<uint16_t> touchedPids;

    if (localCount.size() != kPidCountSize) {
        localCount.assign(kPidCountSize, 0u);
    }

    touchedPids.clear();

    if (touchedPids.capacity() < 256u) {
        touchedPids.reserve(256u);
    }

    for (size_t offset = 0; offset + unit <= packetSize; offset += unit) {
        const uint8_t* packet = tsPacket + offset;

        if (packet[0] != 0x47) {
            continue;
        }

        // 204byte TSでも先頭188byteがTS本体。
        const uint8_t* p = packet;

        const uint16_t pid =
            static_cast<uint16_t>(
                ((p[1] & 0x1F) << 8) |
                p[2]);

        if (pid > kPidMax) {
            continue;
        }

        if (localCount[pid] == 0u) {
            touchedPids.push_back(pid);
        }

        ++localCount[pid];
    }

    if (touchedPids.empty()) {
        return;
    }

    {
        std::unique_lock<std::shared_mutex> lock(pidMutex);

        for (const uint16_t pid : touchedPids) {
            pidCount[pid] += localCount[pid];
        }
    }

    // 次回呼び出し用に、触ったPIDだけ0へ戻す。
    // 全8192要素を毎回clearしないので軽い。
    for (const uint16_t pid : touchedPids) {
        localCount[pid] = 0u;
    }
}

void AnalyzePID::ProcessPacket_SSE2(const uint8_t* tsPacket, size_t packetSize)
{
    // 現状はScalarと同じ処理でOK（将来SIMD化）
    ProcessPacket_Scalar(tsPacket, packetSize);
}

void AnalyzePID::ProcessPacket_SSE4_2(const uint8_t* tsPacket, size_t packetSize)
{
    ProcessPacket_Scalar(tsPacket, packetSize);
}

void AnalyzePID::ProcessPacket_AVX2(const uint8_t* tsPacket, size_t packetSize)
{
    ProcessPacket_Scalar(tsPacket, packetSize);
}

void AnalyzePID::PrintPIDSummary() const
{
    std::shared_lock<std::shared_mutex> lock(pidMutex);

    std::cout << "================ PID Summary ================" << std::endl;
    for (const auto& entry : pidCount) {
        std::cout << "PID 0x" << std::hex << std::setw(4) << std::setfill('0') << entry.first
            << " (" << std::dec << entry.first << ") Count: " << entry.second << std::endl;
    }
    std::cout << "=============================================" << std::endl;
}

void AnalyzePID::Reset()
{
    std::unique_lock<std::shared_mutex> lock(pidMutex);
    pidCount.clear();
    dropCounter.clear();

    // 連続ログは避けたいなら DebugManager 条件で出す
    std::cout << "[INFO] PID data reset." << std::endl;
}
