#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "AppSimdPolicy.hpp"
#include "ConfigManager.h"

#include "AppSimdAvx2.hpp"
#include "AppSimdSse2.hpp"
#include "AppSimdSse42.hpp"
#include "DebugManager.h"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    struct PathOrder_
    {
        AppSimdPolicy::Path values[4]{};
        std::uint32_t count = 0;
        bool forcedScalar = false;
        const wchar_t* reason = L"auto scalar";
    };

    bool CanUsePath_(
        const ConfigManager& cfg,
        AppSimdPolicy::Path path) noexcept
    {
        switch (path) {
        case AppSimdPolicy::Path::Avx2:
            return
                cfg.GetCpuDetectedAVX2() &&
                cfg.GetCpuUseAVX2();

        case AppSimdPolicy::Path::Sse42:
            return
                cfg.GetCpuDetectedSSE4_2() &&
                cfg.GetCpuUseSSE4_2();

        case AppSimdPolicy::Path::Sse2:
            return
                cfg.GetCpuDetectedSSE2() &&
                cfg.GetCpuUseSSE2();

        case AppSimdPolicy::Path::Scalar:
            return true;

        case AppSimdPolicy::Path::Auto:
        default:
            return false;
        }
    }

    AppSimdPolicy::Vendor DetectVendor_(
        const ConfigManager& cfg)
    {
        const std::wstring vendor =
            cfg.GetCpuDetectedVendor();

        if (vendor == L"GenuineIntel") {
            return AppSimdPolicy::Vendor::Intel;
        }

        if (vendor == L"AuthenticAMD") {
            return AppSimdPolicy::Vendor::AMD;
        }

        return AppSimdPolicy::Vendor::Unknown;
    }

    AppSimdPolicy::Path ParseSelectedPath_(
        const ConfigManager& cfg)
    {
        const std::wstring selected =
            cfg.GetSelectedCPUFeature();

        if (selected == L"SCALAR") {
            return AppSimdPolicy::Path::Scalar;
        }

        if (selected == L"SSE2") {
            return AppSimdPolicy::Path::Sse2;
        }

        if (selected == L"SSE4.2") {
            return AppSimdPolicy::Path::Sse42;
        }

        if (selected == L"AVX2") {
            return AppSimdPolicy::Path::Avx2;
        }

        return AppSimdPolicy::Path::Auto;
    }

    PathOrder_ MakeBlockAutoOrder_(
        AppSimdPolicy::BlockKind blockKind) noexcept
    {
        PathOrder_ order{};

        switch (blockKind) {
        case AppSimdPolicy::BlockKind::RealtimeTsReceive:
        case AppSimdPolicy::BlockKind::RealtimePidDispatch:
        case AppSimdPolicy::BlockKind::RealtimeFilterDispatch:
        case AppSimdPolicy::BlockKind::AribDecode:
        {
            order.values[0] = AppSimdPolicy::Path::Scalar;
            order.count = 1;
            order.forcedScalar = true;
            order.reason = L"auto block scalar fixed";
            return order;
        }

        case AppSimdPolicy::BlockKind::EpgFixedTextSearch:
        case AppSimdPolicy::BlockKind::ReservationSearch:
        case AppSimdPolicy::BlockKind::CrcOrHash:
        {
            order.values[0] = AppSimdPolicy::Path::Sse42;
            order.values[1] = AppSimdPolicy::Path::Sse2;
            order.values[2] = AppSimdPolicy::Path::Scalar;
            order.count = 3;
            order.reason = L"auto block sse4.2 priority";
            return order;
        }

        case AppSimdPolicy::BlockKind::EpgBlockOffsetScan:
        case AppSimdPolicy::BlockKind::TsBatchSyncCheck:
        case AppSimdPolicy::BlockKind::TsBatchContinuityCheck:
        case AppSimdPolicy::BlockKind::TsBatchErrorCheck:
        {
            order.values[0] = AppSimdPolicy::Path::Avx2;
            order.values[1] = AppSimdPolicy::Path::Sse2;
            order.values[2] = AppSimdPolicy::Path::Sse42;
            order.values[3] = AppSimdPolicy::Path::Scalar;
            order.count = 4;
            order.reason = L"auto block avx2 priority";
            return order;
        }

        case AppSimdPolicy::BlockKind::EpgDumpParse:
        case AppSimdPolicy::BlockKind::EitEventBuild:
        case AppSimdPolicy::BlockKind::EpgHtmlBuild:
        case AppSimdPolicy::BlockKind::ReservationCacheBuild:
        case AppSimdPolicy::BlockKind::LogParse:
        {
            order.values[0] = AppSimdPolicy::Path::Scalar;
            order.count = 1;
            order.forcedScalar = true;
            order.reason = L"auto block scalar with multicore";
            return order;
        }

        case AppSimdPolicy::BlockKind::Unknown:
        default:
        {
            order.values[0] = AppSimdPolicy::Path::Scalar;
            order.count = 1;
            order.forcedScalar = true;
            order.reason = L"auto unknown scalar";
            return order;
        }
        }
    }

    bool FindPathInOrder_(
        const PathOrder_& order,
        AppSimdPolicy::Path path,
        std::uint32_t& index) noexcept
    {
        for (std::uint32_t i = 0; i < order.count; ++i) {
            if (order.values[i] == path) {
                index = i;
                return true;
            }
        }

        index = 0;
        return false;
    }

    AppSimdPolicy::Path SelectFirstAvailable_(
        const ConfigManager& cfg,
        const PathOrder_& order,
        std::uint32_t startIndex) noexcept
    {
        if (startIndex >= order.count) {
            startIndex = 0;
        }

        for (std::uint32_t i = startIndex; i < order.count; ++i) {
            const AppSimdPolicy::Path path =
                order.values[i];

            if (CanUsePath_(cfg, path)) {
                return path;
            }
        }

        return AppSimdPolicy::Path::Scalar;
    }

    const wchar_t* MakeExplicitReason_(
        AppSimdPolicy::Path requested,
        bool fallback,
        bool forcedScalarBlock) noexcept
    {
        if (forcedScalarBlock) {
            return fallback
                ? L"selected fallback by scalar fixed block"
                : L"selected scalar fixed block";
        }

        switch (requested) {
        case AppSimdPolicy::Path::Avx2:
            return fallback
                ? L"selected avx2 fallback"
                : L"selected avx2";

        case AppSimdPolicy::Path::Sse42:
            return fallback
                ? L"selected sse4.2 fallback"
                : L"selected sse4.2";

        case AppSimdPolicy::Path::Sse2:
            return fallback
                ? L"selected sse2 fallback"
                : L"selected sse2";

        case AppSimdPolicy::Path::Scalar:
            return L"selected scalar";

        case AppSimdPolicy::Path::Auto:
        default:
            return L"selected auto";
        }
    }

    std::uint64_t ElapsedMsFrom_(
        const std::chrono::steady_clock::time_point& begin) noexcept
    {
        try {
            const auto end =
                std::chrono::steady_clock::now();

            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    end - begin).count());
        }
        catch (...) {
            return 0;
        }
    }

    bool IsTableIdMarker_(
        std::wstring_view marker) noexcept
    {
        return marker == L"TableID";
    }

    void AppendFallbackReason_(
        AppSimdPolicy::FixedTextScanResult& result,
        const wchar_t* reason)
    {
        if (reason == nullptr || reason[0] == L'\0') {
            return;
        }

        if (!result.fallbackReason.empty()) {
            result.fallbackReason += L"; ";
        }

        result.fallbackReason += reason;
    }

    bool CollectFixedTextOffsetsScalar_(
        const std::wstring& text,
        std::wstring_view marker,
        std::vector<std::size_t>& outOffsets) noexcept
    {
        try {
            outOffsets.clear();

            if (text.empty() || marker.empty()) {
                return true;
            }

            const std::wstring markerText(marker);

            std::size_t pos = 0;

            while (pos < text.size()) {
                const std::size_t hit =
                    text.find(markerText, pos);

                if (hit == std::wstring::npos) {
                    break;
                }

                outOffsets.push_back(hit);

                const std::size_t next =
                    hit + markerText.size();

                if (next <= hit) {
                    break;
                }

                pos = next;
            }

            return true;
        }
        catch (...) {
            outOffsets.clear();
            return false;
        }
    }

    bool TryCollectFixedTextOffsetsAvx2_(
        const std::wstring& text,
        std::wstring_view marker,
        std::vector<std::size_t>& outOffsets) noexcept
    {
        if (IsTableIdMarker_(marker)) {
            return AppSimdAvx2::CollectEpgSearchTableIdOffsets(
                text,
                outOffsets);
        }

        return AppSimdAvx2::CollectEpgSearchFixedTextOffsets(
            text,
            marker,
            outOffsets);
    }

    bool TryCollectFixedTextOffsetsSse42_(
        const std::wstring& text,
        std::wstring_view marker,
        std::vector<std::size_t>& outOffsets) noexcept
    {
        if (IsTableIdMarker_(marker)) {
            return AppSimdSse42::CollectEpgSearchTableIdOffsets(
                text,
                outOffsets);
        }

        return AppSimdSse42::CollectEpgSearchFixedTextOffsets(
            text,
            marker,
            outOffsets);
    }

    bool TryCollectFixedTextOffsetsSse2_(
        const std::wstring& text,
        std::wstring_view marker,
        std::vector<std::size_t>& outOffsets) noexcept
    {
        if (IsTableIdMarker_(marker)) {
            return AppSimdSse2::CollectEpgSearchTableIdOffsets(
                text,
                outOffsets);
        }

        return AppSimdSse2::CollectEpgSearchFixedTextOffsets(
            text,
            marker,
            outOffsets);
    }
}

namespace AppSimdPolicy
{
    const wchar_t* ToText(
        Path path) noexcept
    {
        switch (path) {
        case Path::Auto:
            return L"AUTO";

        case Path::Avx2:
            return L"AVX2";

        case Path::Sse42:
            return L"SSE4.2";

        case Path::Sse2:
            return L"SSE2";

        case Path::Scalar:
        default:
            return L"SCALAR";
        }
    }

    const wchar_t* ToText(
        Vendor vendor) noexcept
    {
        switch (vendor) {
        case Vendor::Intel:
            return L"Intel";

        case Vendor::AMD:
            return L"AMD";

        case Vendor::Unknown:
        default:
            return L"Unknown";
        }
    }

    const wchar_t* ToText(
        Kernel kernel) noexcept
    {
        switch (kernel) {
        case Kernel::Intel:
            return L"Intel";

        case Kernel::AMD:
            return L"AMD";

        case Kernel::Common:
        default:
            return L"Common";
        }
    }

    const wchar_t* ToText(
        BlockKind blockKind) noexcept
    {
        switch (blockKind) {
        case BlockKind::RealtimeTsReceive:
            return L"RealtimeTsReceive";

        case BlockKind::RealtimePidDispatch:
            return L"RealtimePidDispatch";

        case BlockKind::RealtimeFilterDispatch:
            return L"RealtimeFilterDispatch";

        case BlockKind::AribDecode:
            return L"AribDecode";

        case BlockKind::EpgFixedTextSearch:
            return L"EpgFixedTextSearch";

        case BlockKind::EpgBlockOffsetScan:
            return L"EpgBlockOffsetScan";

        case BlockKind::EpgDumpParse:
            return L"EpgDumpParse";

        case BlockKind::EitEventBuild:
            return L"EitEventBuild";

        case BlockKind::EpgHtmlBuild:
            return L"EpgHtmlBuild";

        case BlockKind::ReservationCacheBuild:
            return L"ReservationCacheBuild";

        case BlockKind::ReservationSearch:
            return L"ReservationSearch";

        case BlockKind::TsBatchSyncCheck:
            return L"TsBatchSyncCheck";

        case BlockKind::TsBatchContinuityCheck:
            return L"TsBatchContinuityCheck";

        case BlockKind::TsBatchErrorCheck:
            return L"TsBatchErrorCheck";

        case BlockKind::CrcOrHash:
            return L"CrcOrHash";

        case BlockKind::LogParse:
            return L"LogParse";

        case BlockKind::Unknown:
        default:
            return L"Unknown";
        }
    }

    SelectResult Select(
        const ConfigManager& cfg,
        BlockKind blockKind) noexcept
    {
        SelectResult out{};

        out.blockKind = blockKind;
        out.vendor = DetectVendor_(cfg);
        out.kernel = Kernel::Common;

        const Path requested =
            ParseSelectedPath_(cfg);

        const PathOrder_ autoOrder =
            MakeBlockAutoOrder_(blockKind);

        out.requested = requested;
        out.forcedScalarBlock = autoOrder.forcedScalar;

        if (requested == Path::Auto) {
            out.actual =
                SelectFirstAvailable_(
                    cfg,
                    autoOrder,
                    0);

            out.fallback = false;
            out.reason = autoOrder.reason;
            return out;
        }

        if (requested == Path::Scalar) {
            out.actual = Path::Scalar;
            out.fallback = false;
            out.reason = L"selected scalar";
            return out;
        }

        std::uint32_t selectedIndex = 0;

        if (FindPathInOrder_(
            autoOrder,
            requested,
            selectedIndex)) {
            out.actual =
                SelectFirstAvailable_(
                    cfg,
                    autoOrder,
                    selectedIndex);
        }
        else {
            out.actual =
                SelectFirstAvailable_(
                    cfg,
                    autoOrder,
                    0);
        }

        out.fallback =
            out.actual != requested;

        out.reason =
            MakeExplicitReason_(
                requested,
                out.fallback,
                autoOrder.forcedScalar);

        return out;
    }

    bool CollectFixedTextOffsets(
        const ConfigManager& cfg,
        BlockKind blockKind,
        const std::wstring& text,
        std::wstring_view marker,
        std::vector<std::size_t>& outOffsets,
        FixedTextScanResult* outResult) noexcept
    {
        const auto begin =
            std::chrono::steady_clock::now();

        FixedTextScanResult result{};
        result.policy =
            Select(
                cfg,
                blockKind);

        result.textChars = text.size();
        result.backendPrepared =
            result.policy.actual != Path::Scalar;

        bool ok = false;

        auto runScalar = [&]() noexcept -> bool {
            result.executedPath =
                Path::Scalar;

            return CollectFixedTextOffsetsScalar_(
                text,
                marker,
                outOffsets);
            };

        try {
            switch (result.policy.actual) {
            case Path::Avx2:
                result.simdTried = true;

                if (TryCollectFixedTextOffsetsAvx2_(
                    text,
                    marker,
                    outOffsets)) {
                    result.executedPath =
                        Path::Avx2;
                    ok = true;
                    break;
                }

                AppendFallbackReason_(
                    result,
                    L"AVX2 fixed text kernel unavailable");

                if (TryCollectFixedTextOffsetsSse2_(
                    text,
                    marker,
                    outOffsets)) {
                    result.executedPath =
                        Path::Sse2;
                    ok = true;
                    break;
                }

                AppendFallbackReason_(
                    result,
                    L"SSE2 fixed text kernel unavailable");

                ok = runScalar();
                break;

            case Path::Sse42:
                result.simdTried = true;

                if (TryCollectFixedTextOffsetsSse42_(
                    text,
                    marker,
                    outOffsets)) {
                    result.executedPath =
                        Path::Sse42;
                    ok = true;
                    break;
                }

                AppendFallbackReason_(
                    result,
                    L"SSE4.2 fixed text kernel unavailable");

                if (TryCollectFixedTextOffsetsSse2_(
                    text,
                    marker,
                    outOffsets)) {
                    result.executedPath =
                        Path::Sse2;
                    ok = true;
                    break;
                }

                AppendFallbackReason_(
                    result,
                    L"SSE2 fixed text kernel unavailable");

                ok = runScalar();
                break;

            case Path::Sse2:
                result.simdTried = true;

                if (TryCollectFixedTextOffsetsSse2_(
                    text,
                    marker,
                    outOffsets)) {
                    result.executedPath =
                        Path::Sse2;
                    ok = true;
                    break;
                }

                AppendFallbackReason_(
                    result,
                    L"SSE2 fixed text kernel unavailable");

                ok = runScalar();
                break;

            case Path::Auto:
            case Path::Scalar:
            default:
                ok = runScalar();
                break;
            }
        }
        catch (...) {
            outOffsets.clear();

            result.executedPath =
                Path::Scalar;

            result.fallbackReason =
                L"fixed text dispatch exception, fallback to SCALAR";

            ok = CollectFixedTextOffsetsScalar_(
                text,
                marker,
                outOffsets);
        }

        result.backendUsed =
            result.executedPath != Path::Scalar;

        result.backendDeferred =
            result.backendPrepared && !result.backendUsed;

        result.matchCount =
            ok ? outOffsets.size() : 0;

        result.elapsedMs =
            ElapsedMsFrom_(begin);

        if (outResult != nullptr) {
            *outResult = result;
        }

        return ok;
    }

    bool CollectFixedTextOffsetsFromCurrentConfig(
        BlockKind blockKind,
        const std::wstring& text,
        std::wstring_view marker,
        std::vector<std::size_t>& outOffsets,
        FixedTextScanResult* outResult) noexcept
    {
        try {
            const ConfigManager& cfg =
                ConfigManager::GetInstance();

            return CollectFixedTextOffsets(
                cfg,
                blockKind,
                text,
                marker,
                outOffsets,
                outResult);
        }
        catch (...) {
            FixedTextScanResult result{};
            result.policy.blockKind = blockKind;
            result.policy.requested = Path::Auto;
            result.policy.actual = Path::Scalar;
            result.policy.vendor = Vendor::Unknown;
            result.policy.kernel = Kernel::Common;
            result.policy.fallback = true;
            result.policy.forcedScalarBlock = true;
            result.policy.reason = L"fixed text current config exception scalar";
            result.executedPath = Path::Scalar;
            result.textChars = text.size();
            result.backendPrepared = false;
            result.backendUsed = false;
            result.backendDeferred = false;
            result.simdTried = false;
            result.fallbackReason = L"current config exception";

            const auto begin =
                std::chrono::steady_clock::now();

            const bool ok =
                CollectFixedTextOffsetsScalar_(
                    text,
                    marker,
                    outOffsets);

            result.matchCount =
                ok ? outOffsets.size() : 0;

            result.elapsedMs =
                ElapsedMsFrom_(begin);

            if (outResult != nullptr) {
                *outResult = result;
            }

            return ok;
        }
    }

    void LogFixedTextScanResult(
        const FixedTextScanResult& result,
        const wchar_t* tag) noexcept
    {
        try {
            DebugLogFormat(
                L"[APP-SIMD][TEXT] tag=%ls block=%ls vendor=%ls requested=%ls policyActual=%ls executed=%ls kernel=%ls fallback=%d forcedScalar=%d backendPrepared=%d backendUsed=%d backendDeferred=%d simdTried=%d matches=%zu chars=%zu ms=%llu reason=%ls fallbackReason=%ls",
                tag ? tag : L"",
                ToText(result.policy.blockKind),
                ToText(result.policy.vendor),
                ToText(result.policy.requested),
                ToText(result.policy.actual),
                ToText(result.executedPath),
                ToText(result.policy.kernel),
                result.policy.fallback ? 1 : 0,
                result.policy.forcedScalarBlock ? 1 : 0,
                result.backendPrepared ? 1 : 0,
                result.backendUsed ? 1 : 0,
                result.backendDeferred ? 1 : 0,
                result.simdTried ? 1 : 0,
                result.matchCount,
                result.textChars,
                static_cast<unsigned long long>(result.elapsedMs),
                result.policy.reason ? result.policy.reason : L"",
                result.fallbackReason.c_str());
        }
        catch (...) {
            DebugLogFormat(
                L"[APP-SIMD][TEXT][WARN] log failed: tag=%ls",
                tag ? tag : L"");
        }
    }
}
