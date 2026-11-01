#pragma once
#pragma once
#ifndef TSCEPG_TS_RING_MEMORY_POLICY_HPP
#define TSCEPG_TS_RING_MEMORY_POLICY_HPP

#include <cstddef>
#include <cstdint>

// C++20 / MSVC Win32 + x64
// EPGのTS受信キュー容量をアーキテクチャ別に制御する。
// 注意: TSを捨てる機能ではない。満杯時は既存producerのPush再試行を使う。
namespace TSCEPG::TsRingMemoryPolicy
{
#if defined(_M_IX86) || defined(_X86_)
    inline constexpr bool kIsX86 = true;
    inline constexpr std::size_t kMinEpgPackets = 16384;   // 約2.94 MiB (188B/packet)
    inline constexpr std::size_t kMaxEpgPackets = 65536;   // 約11.75 MiB
    inline constexpr std::size_t kMaxHistoryPackets = 8192; // 約1.47 MiB
#else
    inline constexpr bool kIsX86 = false;
    inline constexpr std::size_t kMinEpgPackets = 32768;   // 約5.88 MiB
    inline constexpr std::size_t kMaxEpgPackets = 262144;  // 約47 MiB
    inline constexpr std::size_t kMaxHistoryPackets = 65536; // 約11.75 MiB
#endif

    // 診断用TS履歴は通常のEPG取得では不要なため両アーキテクチャで既定OFF。
    // 必要なときはFilterManagerRunnerの第3引数で要求する。
    inline constexpr std::size_t kDefaultHistoryPackets = 0;

    [[nodiscard]] constexpr std::size_t NormalizeEpgPackets(
        std::size_t requested) noexcept
    {
        std::size_t cap = requested;
        if (cap < kMinEpgPackets) cap = kMinEpgPackets;
        if (cap > kMaxEpgPackets) cap = kMaxEpgPackets;

        std::size_t pow2 = 1;
        while (pow2 < cap) pow2 <<= 1;
        return pow2;
    }

    [[nodiscard]] constexpr std::size_t NormalizeHistoryPackets(
        std::size_t requested) noexcept
    {
        // 0 = 無効。正の値は最大値でクランプする。
        return requested > kMaxHistoryPackets ? kMaxHistoryPackets : requested;
    }
}

#endif // TSCEPG_TS_RING_MEMORY_POLICY_HPP
