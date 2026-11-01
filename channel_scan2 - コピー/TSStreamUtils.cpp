#include "TSStreamUtils.hpp"

#include <iostream>
#include <cstdio>
#include <iomanip>
#include <memory>
#include <sstream>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <new>
#include <cstring>
#include <algorithm>

#include "PacketDumpManager.hpp"

namespace TSStreamUtils
{
    namespace {
        using CaptureBlock = std::vector<TsPacket>;

        constexpr std::size_t kMaxCaptureBlocks = 64;
        constexpr std::size_t kMaxCaptureTotalPackets = 200000;
        constexpr DWORD kMaxSafeBlockBytes = 4u * 1024u * 1024u;
        constexpr std::size_t kTs188 = 188;
        constexpr std::size_t kTs204 = 204;

        std::deque<CaptureBlock> g_capturedBlocks;
        std::size_t g_captureTotalPackets = 0;
        std::size_t g_captureDroppedBlocks = 0;
        std::mutex g_captureMtx;
        std::atomic_bool g_capturePacketsForDump{ false };

        std::atomic<size_t> g_tsStreamFailureCount{ 0 };
        std::atomic<size_t> g_tsStreamCallCount{ 0 };
        std::atomic<size_t> g_tsStreamSuccessCount{ 0 };
        std::atomic<size_t> g_tsStreamPacketCount{ 0 };
        std::atomic<size_t> g_invalidBlockCount{ 0 };
        std::atomic<size_t> g_invalidSyncCount{ 0 };
        std::atomic<size_t> g_oversizedBlockCount{ 0 };
        std::atomic<size_t> g_converted204PacketCount{ 0 };

        void EnforceCaptureLimitLocked()
        {
            while (!g_capturedBlocks.empty() &&
                (g_capturedBlocks.size() > kMaxCaptureBlocks ||
                    g_captureTotalPackets > kMaxCaptureTotalPackets))
            {
                g_captureTotalPackets -= g_capturedBlocks.front().size();
                g_capturedBlocks.pop_front();
                ++g_captureDroppedBlocks;
            }
        }

        void AppendCapturedPacketsSafe(const std::vector<TsPacket>& packets)
        {
            if (!g_capturePacketsForDump.load(std::memory_order_relaxed) || packets.empty()) return;

            std::lock_guard<std::mutex> lock(g_captureMtx);

            try {
                g_capturedBlocks.emplace_back(packets.begin(), packets.end());
                g_captureTotalPackets += packets.size();
                EnforceCaptureLimitLocked();
            }
            catch (const std::bad_alloc&) {
                g_capturedBlocks.clear();
                g_captureTotalPackets = 0;
                ++g_captureDroppedBlocks;
                DebugLog(L"[TSStreamUtils][WARN] Capture buffer OOM -> cleared (continue)");
            }
        }

        bool HasSyncAtStride_(const BYTE* p, std::size_t size, std::size_t stride) noexcept
        {
            if (!p || stride < kTs188 || size < stride || (size % stride) != 0) {
                return false;
            }

            const std::size_t count = size / stride;
            if (count == 0) {
                return false;
            }

            // 全件検査でも軽いが、大ブロック時の負荷を抑えるため先頭中心に検査する。
            const std::size_t samples = (std::min)(count, static_cast<std::size_t>(64));
            std::size_t ok = 0;
            for (std::size_t i = 0; i < samples; ++i) {
                if (p[i * stride] == 0x47) {
                    ++ok;
                }
            }

            // 1パケットだけのブロックは完全一致、それ以外は 90% 以上で判定。
            if (samples <= 1) {
                return ok == samples;
            }
            return ok * 10 >= samples * 9;
        }

        std::size_t DetectPacketStride_(const BYTE* p, std::size_t size) noexcept
        {
            if (HasSyncAtStride_(p, size, kTs188)) return kTs188;
            if (HasSyncAtStride_(p, size, kTs204)) return kTs204;
            return 0;
        }

        bool TryExtractResync188_(const BYTE* p, std::size_t size, std::vector<TsPacket>& out)
        {
            if (!p || size < kTs188) return false;

            bool found = false;
            for (std::size_t offset = 0; offset < kTs188 && offset + kTs188 <= size; ++offset) {
                std::size_t syncHits = 0;
                std::size_t samples = 0;
                for (std::size_t pos = offset; pos + kTs188 <= size && samples < 64; pos += kTs188, ++samples) {
                    if (p[pos] == 0x47) ++syncHits;
                }
                if (samples >= 2 && syncHits * 10 >= samples * 9) {
                    for (std::size_t pos = offset; pos + kTs188 <= size; pos += kTs188) {
                        if (p[pos] != 0x47) {
                            ++g_invalidSyncCount;
                            continue;
                        }
                        TsPacket packet{};
                        std::memcpy(packet.data(), p + pos, kTs188);
                        out.emplace_back(std::move(packet));
                    }
                    found = !out.empty();
                    break;
                }
            }
            return found;
        }

        bool CopyPacketsFromBlock_(const BYTE* pBuf, DWORD dwSize, std::vector<TsPacket>& packetsOut)
        {
            packetsOut.clear();

            if (!pBuf || dwSize == 0) {
                ++g_invalidBlockCount;
                return false;
            }
            if (dwSize > kMaxSafeBlockBytes) {
                ++g_oversizedBlockCount;
                return false;
            }

            const std::size_t size = static_cast<std::size_t>(dwSize);
            const std::size_t stride = DetectPacketStride_(pBuf, size);

            try {
                if (stride == kTs188 || stride == kTs204) {
                    const std::size_t packetCount = size / stride;
                    packetsOut.reserve(packetCount);

                    for (std::size_t i = 0; i < packetCount; ++i) {
                        const BYTE* src = pBuf + i * stride;
                        if (src[0] != 0x47) {
                            ++g_invalidSyncCount;
                            continue;
                        }

                        TsPacket packet{};
                        std::memcpy(packet.data(), src, kTs188);
                        packetsOut.emplace_back(std::move(packet));
                    }

                    if (stride == kTs204 && !packetsOut.empty()) {
                        g_converted204PacketCount.fetch_add(packetsOut.size(), std::memory_order_relaxed);
                    }
                }
                else {
                    // サイズ端数や先頭ズレが疑われる場合だけ、188同期を探索して救済する。
                    // 誤検出を避けるため、204の末尾16バイトはここでは扱わない。
                    packetsOut.reserve(size / kTs188);
                    if (!TryExtractResync188_(pBuf, size, packetsOut)) {
                        ++g_invalidBlockCount;
                        return false;
                    }
                }
            }
            catch (const std::bad_alloc&) {
                packetsOut.clear();
                ++g_invalidBlockCount;
                DebugLogFormat(L"[TSStreamUtils][ERROR] packet copy OOM: size=%u stride=%zu", dwSize, stride);
                return false;
            }
            catch (...) {
                packetsOut.clear();
                ++g_invalidBlockCount;
                DebugLogFormat(L"[TSStreamUtils][ERROR] packet copy exception: size=%u stride=%zu", dwSize, stride);
                return false;
            }

            return !packetsOut.empty();
        }
    } // namespace

    size_t GetTsStreamFailureCount()
    {
        return g_tsStreamFailureCount.load(std::memory_order_relaxed);
    }

    TsStreamStats GetTsStreamStats()
    {
        TsStreamStats s;
        s.callCount = g_tsStreamCallCount.load(std::memory_order_relaxed);
        s.successCount = g_tsStreamSuccessCount.load(std::memory_order_relaxed);
        s.failureCount = g_tsStreamFailureCount.load(std::memory_order_relaxed);
        s.packetCount = g_tsStreamPacketCount.load(std::memory_order_relaxed);
        s.invalidBlockCount = g_invalidBlockCount.load(std::memory_order_relaxed);
        s.invalidSyncCount = g_invalidSyncCount.load(std::memory_order_relaxed);
        s.oversizedBlockCount = g_oversizedBlockCount.load(std::memory_order_relaxed);
        s.converted204PacketCount = g_converted204PacketCount.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(g_captureMtx);
            s.captureDroppedBlocks = g_captureDroppedBlocks;
        }
        return s;
    }

    void ResetTsStreamStats()
    {
        g_tsStreamCallCount.store(0, std::memory_order_relaxed);
        g_tsStreamSuccessCount.store(0, std::memory_order_relaxed);
        g_tsStreamFailureCount.store(0, std::memory_order_relaxed);
        g_tsStreamPacketCount.store(0, std::memory_order_relaxed);
        g_invalidBlockCount.store(0, std::memory_order_relaxed);
        g_invalidSyncCount.store(0, std::memory_order_relaxed);
        g_oversizedBlockCount.store(0, std::memory_order_relaxed);
        g_converted204PacketCount.store(0, std::memory_order_relaxed);
        ClearCapturedPackets();
    }

    void SetCapturedPacketDumpEnabled(bool enabled) noexcept
    {
        g_capturePacketsForDump.store(enabled, std::memory_order_relaxed);
        if (!enabled) {
            ClearCapturedPackets();
        }
    }

    bool IsCapturedPacketDumpEnabled() noexcept
    {
        return g_capturePacketsForDump.load(std::memory_order_relaxed);
    }

    void DumpAllCapturedPackets()
    {
        std::deque<CaptureBlock> snapshot;
        std::size_t dropped = 0;

        {
            std::lock_guard<std::mutex> lock(g_captureMtx);
            snapshot = g_capturedBlocks;
            dropped = g_captureDroppedBlocks;
        }

        const std::filesystem::path outdir = L"TS_Captures";
        std::error_code ec;
        std::filesystem::create_directories(outdir, ec);

        if (dropped > 0) {
            DebugLogFormat(L"[TSStreamUtils][INFO] Capture dropped blocks=%zu (limit protection)", dropped);
        }

        for (size_t i = 0; i < snapshot.size(); ++i) {
            std::wstringstream name;
            name << L"TS_Capture_" << std::setw(4) << std::setfill(L'0') << i << L".txt";
            const auto outPath = (outdir / std::filesystem::path(name.str())).wstring();
            PacketDumpManager::DumpHexPackets(snapshot[i], outPath);
        }
    }

    void ClearCapturedPackets()
    {
        std::lock_guard<std::mutex> lock(g_captureMtx);
        std::deque<CaptureBlock> empty;
        g_capturedBlocks.swap(empty);
        g_captureTotalPackets = 0;
    }

    bool WaitForSignalLock(IBonDriver2* bon, int timeout_ms)
    {
        if (!bon) return false;

        const ULONGLONG start = GetTickCount64();
        while (GetTickCount64() - start < static_cast<ULONGLONG>(timeout_ms)) {
            const float level = bon->GetSignalLevel();
            if (level > 0.0f) {
                std::wcout << L"[INFO] 信号ロック済み: " << level << L" dB" << std::endl;
                return true;
            }
            Sleep(100);
        }
        std::wcerr << L"[WARN] 信号ロック失敗: タイムアウト" << std::endl;
        return false;
    }

    bool ReceiveTsToBuffer(IBonDriver2* bon,
        std::deque<std::array<uint8_t, TS_PACKET_SIZE>>& tsBuffer,
        int msDuration)
    {
        if (!bon) return false;

        const ULONGLONG endTick = GetTickCount64() + static_cast<ULONGLONG>(msDuration);
        std::vector<TsPacket> packets;
        packets.reserve(4096);

        while (GetTickCount64() < endTick) {
            packets.clear();
            if (!ReceiveTsStreamBlock(bon, packets)) {
                Sleep(1);
                continue;
            }

            for (const auto& src : packets) {
                if (tsBuffer.size() >= MAX_BUFFER_PACKETS) {
                    tsBuffer.pop_front();
                }

                std::array<uint8_t, TS_PACKET_SIZE> pkt{};
                std::memcpy(pkt.data(), src.data(), TS_PACKET_SIZE);
                tsBuffer.push_back(std::move(pkt));
            }
        }

        std::wcout << L"[INFO] 受信TSパケット数: " << tsBuffer.size() << std::endl;
        return !tsBuffer.empty();
    }

    bool ReceiveTsStreamBlock(IBonDriver2* bon, std::vector<TsPacket>& packetsOut)
    {
        if (!bon) {
            DebugLog(L"[TSStreamUtils] bon is null");
            return false;
        }

        g_tsStreamCallCount.fetch_add(1, std::memory_order_relaxed);

        BYTE* pBuf = nullptr;
        DWORD dwSize = 0;
        DWORD dwRemain = 0;

        if (!bon->GetTsStream(&pBuf, &dwSize, &dwRemain)) {
            g_tsStreamFailureCount.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (!CopyPacketsFromBlock_(pBuf, dwSize, packetsOut)) {
            g_tsStreamFailureCount.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        g_tsStreamSuccessCount.fetch_add(1, std::memory_order_relaxed);
        g_tsStreamPacketCount.fetch_add(packetsOut.size(), std::memory_order_relaxed);

        AppendCapturedPacketsSafe(packetsOut);
        return true;
    }

    void PrintHex(const std::vector<uint8_t>& data, size_t bytesPerLine)
    {
        PrintHex(data.data(), data.size(), bytesPerLine);
    }

    void PrintHex(const uint8_t* data, size_t size, size_t bytesPerLine)
    {
        if (!data || size == 0) {
            std::cout << "[EMPTY]" << std::endl;
            return;
        }
        for (size_t i = 0; i < size; i += bytesPerLine) {
            std::printf("[%04zx] ", i);
            for (size_t j = 0; j < bytesPerLine; ++j) {
                if (i + j < size) std::printf("%02X ", data[i + j]);
                else              std::printf("   ");
            }
            std::printf("\n");
        }
    }
}
