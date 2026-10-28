#pragma once
#ifndef SIMD_DATA_HPP
#define SIMD_DATA_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace SIMDData
{
    enum class ExecPath : int
    {
        Auto = -1,
        Scalar = 0,
        Sse2 = 1,
        Sse42 = 2,
        Avx2 = 3
    };

    struct CpuCaps
    {
        bool sse2 = false;
        bool sse42 = false;
        bool avx = false;
        bool osAvx = false;
        bool avx2 = false;
    };

    struct SelectedPath
    {
        ExecPath requested = ExecPath::Scalar;
        ExecPath actual = ExecPath::Scalar;
        bool fallback = false;
        std::wstring reason;
    };

    struct EpgConvertOptions
    {
        bool keepTitleText = true;
        bool keepDescriptionText = true;
        bool keepGenre = true;
    };

    struct EpgColumnStore
    {
        std::vector<std::uint16_t> tableIds;
        std::vector<std::uint16_t> serviceIds;
        std::vector<std::uint16_t> eventIds;
        std::vector<std::uint16_t> tsIds;
        std::vector<std::uint16_t> onIds;

        // EPG dump の raw MJD+BCD40bit。
        // MJD + BCD時刻なので、同一形式内では時刻順比較に使いやすい。
        std::vector<std::uint64_t> startRaw40;

        std::vector<std::uint32_t> durationSec;

        std::vector<std::uint8_t> genreMajor;
        std::vector<std::uint8_t> genreMinor;

        // UTF-16テキストを連結して保持する。
        // titleOffset/titleLength で titleTextPool を参照する。
        std::wstring titleTextPool;
        std::vector<std::uint32_t> titleOffset;
        std::vector<std::uint32_t> titleLength;

        std::wstring descriptionTextPool;
        std::vector<std::uint32_t> descriptionOffset;
        std::vector<std::uint32_t> descriptionLength;

        void Clear()
        {
            tableIds.clear();
            serviceIds.clear();
            eventIds.clear();
            tsIds.clear();
            onIds.clear();
            startRaw40.clear();
            durationSec.clear();
            genreMajor.clear();
            genreMinor.clear();

            titleTextPool.clear();
            titleOffset.clear();
            titleLength.clear();

            descriptionTextPool.clear();
            descriptionOffset.clear();
            descriptionLength.clear();
        }

        std::size_t size() const noexcept
        {
            return eventIds.size();
        }

        bool empty() const noexcept
        {
            return eventIds.empty();
        }
    };

    struct EpgConvertResult
    {
        bool ok = false;
        ExecPath path = ExecPath::Scalar;

        std::size_t lineCount = 0;
        std::size_t eventHeaderCount = 0;
        std::size_t acceptedEventCount = 0;
        std::size_t skippedEventCount = 0;

        std::size_t titleCount = 0;
        std::size_t descriptionCount = 0;
        std::size_t genreCount = 0;

        std::wstring message;
    };

    CpuCaps DetectCpuCaps() noexcept;
    ExecPath ParseExecPathName(std::wstring_view name) noexcept;
    const wchar_t* ToString(ExecPath path) noexcept;
    SelectedPath SelectPathFromConfig() noexcept;

    bool ConvertEpgDumpTextToColumnStore(
        std::wstring_view epgText,
        const EpgConvertOptions& options,
        EpgColumnStore& outStore,
        EpgConvertResult& outResult) noexcept;

    bool ConvertEpgDumpFileToColumnStore(
        const std::filesystem::path& path,
        const EpgConvertOptions& options,
        EpgColumnStore& outStore,
        EpgConvertResult& outResult) noexcept;

    // スカラー以外の入口。
    // 今回は入口とデータ型を確定させる段階なので、
    // 内部変換は共通実装へ通す。
    bool ConvertEpgDumpTextToColumnStoreSse2(
        std::wstring_view epgText,
        const EpgConvertOptions& options,
        EpgColumnStore& outStore,
        EpgConvertResult& outResult) noexcept;

    bool ConvertEpgDumpTextToColumnStoreSse42(
        std::wstring_view epgText,
        const EpgConvertOptions& options,
        EpgColumnStore& outStore,
        EpgConvertResult& outResult) noexcept;

    bool ConvertEpgDumpTextToColumnStoreAvx2(
        std::wstring_view epgText,
        const EpgConvertOptions& options,
        EpgColumnStore& outStore,
        EpgConvertResult& outResult) noexcept;
}

#endif // SIMD_DATA_HPP
