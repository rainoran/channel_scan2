#pragma once
#ifndef TSCEPG_MEMORY_POLICY_HPP
#define TSCEPG_MEMORY_POLICY_HPP

#include <cstddef>
#include <cstdint>
#include <limits>

// Win32 (x86) でのみメモリ診断を有効化する。
// 必要な場合は Win32 のプリプロセッサ定義で TSCEPG_MEMORY_POLICY_ENABLE_X86=0 とする。
#ifndef TSCEPG_MEMORY_POLICY_ENABLE_X86
#define TSCEPG_MEMORY_POLICY_ENABLE_X86 1
#endif

#if defined(_M_IX86) && (TSCEPG_MEMORY_POLICY_ENABLE_X86 != 0)
#define TSCEPG_MEMORY_POLICY_ACTIVE 1
#else
#define TSCEPG_MEMORY_POLICY_ACTIVE 0
#endif

namespace TSCEPG::MemoryPolicy {

    inline constexpr bool kEnabledForThisBuild =
        (TSCEPG_MEMORY_POLICY_ACTIVE != 0);

    // 診断のためのスナップショット。すべてバイト単位。
    // virtualFreeBytes / largestFreeBlockBytes はアドレス空間の空きを示し、
    // 物理メモリの空きや「必ず確保できるサイズ」を示すものではない。
    struct Snapshot final {
        std::uint64_t elapsedMs = 0;          // GetTickCount64() による時刻
        std::uint64_t privateBytes = 0;        // PROCESS_MEMORY_COUNTERS_EX::PrivateUsage
        std::uint64_t workingSetBytes = 0;     // WorkingSetSize
        std::uint64_t pagefileBytes = 0;       // PagefileUsage
        std::uint64_t virtualFreeBytes = 0;    // MEM_FREE の合計
        std::uint64_t largestFreeBlockBytes = 0;
        std::uint64_t virtualReservedBytes = 0;
        std::uint64_t virtualCommittedBytes = 0;
        std::uint32_t processQueryError = 0;
        std::uint32_t virtualQueryError = 0;
        bool processValid = false;
        bool virtualValid = false;
    };

    // ログは exe と同じフォルダーの memory_policy_x86.log に追記。
    // fileName を指定する場合はファイル名のみ（パス区切りを含めない）。
    // x64 では常に false を返し、ログファイルは作らない。
    [[nodiscard]] bool Initialize(
        const wchar_t* fileName = L"memory_policy_x86.log") noexcept;

    // 正常終了時に呼び出す。常駐ワーカーや動的確保オブジェクトは持たない。
    void Shutdown() noexcept;

    // 定期記録の最小間隔。最低1000ms、初期値5000ms。
    void SetPulseIntervalMs(std::uint32_t intervalMs) noexcept;

    // 診断用にメモリ状態を取得。x64 では false（出力値はゼロ）。
    [[nodiscard]] bool ReadSnapshot(
        Snapshot& out,
        bool scanVirtualAddressSpace = true) noexcept;

    // 明示した段階を必ず記録。開始・終了・統合前後など低頻度箇所で呼ぶ。
    // scanVirtualAddressSpace=true の場合は VirtualQuery による全域走査も実行。
    void Checkpoint(
        const wchar_t* stage,
        bool scanVirtualAddressSpace = true) noexcept;

    // 高頻度処理から呼べる間引き版。初期値では5秒に一度だけ記録。
    // ただし各回の実行時にアドレス空間全域を走査するため、
    // TSパケット処理のホットパスより外側から呼ぶことを推奨。
    void Pulse(const wchar_t* stage = L"EIT_RUNNING") noexcept;

    // bad_alloc を捕捉した場所で呼ぶ。追加のヒープ確保を行わず強制記録。
    // 例外を握りつぶさず、呼び出し側で安全な停止または再送出を行うこと。
    void AllocationFailure(const wchar_t* stage) noexcept;

    // 要素数 * 要素サイズ の size_t オーバーフロー判定（両ビルド共通）。
    [[nodiscard]] inline bool TryCalculateBytes(
        std::size_t elementCount,
        std::size_t elementSize,
        std::size_t& outBytes) noexcept
    {
        outBytes = 0;
        if (elementSize != 0 &&
            elementCount > (std::numeric_limits<std::size_t>::max)() / elementSize) {
            return false;
        }
        outBytes = elementCount * elementSize;
        return true;
    }

} // namespace TSCEPG::MemoryPolicy

#endif // TSCEPG_MEMORY_POLICY_HPP
