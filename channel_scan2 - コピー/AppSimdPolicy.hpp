#pragma once
#ifndef APP_SIMD_POLICY_HPP
#define APP_SIMD_POLICY_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class ConfigManager;

namespace AppSimdPolicy
{
    enum class Path : std::uint8_t
    {
        Auto = 0,
        Scalar = 1,
        Sse2 = 2,
        Sse42 = 3,
        Avx2 = 4
    };

    enum class Vendor : std::uint8_t
    {
        Unknown = 0,
        Intel = 1,
        AMD = 2
    };

    enum class Kernel : std::uint8_t
    {
        Common = 0,
        Intel = 1,
        AMD = 2
    };

    enum class BlockKind : std::uint8_t
    {
        RealtimeTsReceive = 0,
        RealtimePidDispatch = 1,
        RealtimeFilterDispatch = 2,

        AribDecode = 10,

        EpgFixedTextSearch = 20,
        EpgBlockOffsetScan = 21,
        EpgDumpParse = 22,
        EitEventBuild = 23,
        EpgHtmlBuild = 24,

        ReservationCacheBuild = 30,
        ReservationSearch = 31,

        TsBatchSyncCheck = 40,
        TsBatchContinuityCheck = 41,
        TsBatchErrorCheck = 42,

        CrcOrHash = 50,

        LogParse = 60,

        Unknown = 255
    };

    struct SelectResult
    {
        BlockKind blockKind = BlockKind::Unknown;

        Path requested = Path::Auto;
        Path actual = Path::Scalar;

        Vendor vendor = Vendor::Unknown;
        Kernel kernel = Kernel::Common;

        bool fallback = false;
        bool forcedScalarBlock = false;

        const wchar_t* reason = L"scalar";
    };


    struct FixedTextScanResult
    {
        SelectResult policy{};

        Path executedPath = Path::Scalar;

        bool backendPrepared = false;
        bool backendUsed = false;
        bool backendDeferred = false;
        bool simdTried = false;

        std::size_t matchCount = 0;
        std::size_t textChars = 0;
        std::uint64_t elapsedMs = 0;

        std::wstring fallbackReason;
    };

    bool CollectFixedTextOffsets(
        const ConfigManager& cfg,
        BlockKind blockKind,
        const std::wstring& text,
        std::wstring_view marker,
        std::vector<std::size_t>& outOffsets,
        FixedTextScanResult* outResult = nullptr) noexcept;

    bool CollectFixedTextOffsetsFromCurrentConfig(
        BlockKind blockKind,
        const std::wstring& text,
        std::wstring_view marker,
        std::vector<std::size_t>& outOffsets,
        FixedTextScanResult* outResult = nullptr) noexcept;

    void LogFixedTextScanResult(
        const FixedTextScanResult& result,
        const wchar_t* tag) noexcept;

    SelectResult Select(
        const ConfigManager& cfg,
        BlockKind blockKind) noexcept;

    const wchar_t* ToText(Path path) noexcept;
    const wchar_t* ToText(Vendor vendor) noexcept;
    const wchar_t* ToText(Kernel kernel) noexcept;
    const wchar_t* ToText(BlockKind blockKind) noexcept;
}

#endif // APP_SIMD_POLICY_HPP