#pragma once
#ifndef EPG_BUILD_GUI_HPP
#define EPG_BUILD_GUI_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <functional>

class IBonDriver2;
enum class FilterManagerMode;

namespace AutoReserve
{
    struct RuleMatchResult;
}

namespace EpgHtmlPortal
{
    struct WriteOptions;
}

namespace EpgBuildGUI
{
    enum class Status : uint32_t
    {
        Ok = 0,
        Cancelled,
        InvalidArgument,
        ChannelListMissing,
        ChannelListLoadFailed,
        NoTargets,
        NoEpgGenerated,
        MergeFailed,
        HtmlFailed
    };

    enum class HtmlRenderRoute : uint32_t
    {
        FixedLegacy = 0,
        FixedProgramPageWriter = 1,
        FixedPlugin = 2,
        AutoPluginThenLegacy = 3,
        FixedLite = 4
    };

    enum class EpgCaptureMode : uint32_t
    {
        AutoByExistingEpg = 0,
        FixedInitial = 1,
        FixedRefresh = 2,
        FixedManual = 3
    };

    struct Result
    {
        Status status = Status::InvalidArgument;
        std::size_t targetChannels = 0;
        std::size_t processedChannels = 0;

        std::wstring channelListPath;
        std::wstring mergedEpgPath;
        std::wstring htmlPath;

        bool IsOk() const noexcept
        {
            return status == Status::Ok;
        }

        bool IsCancelled() const noexcept
        {
            return status == Status::Cancelled;
        }
    };

    struct Callbacks
    {
        std::function<void(std::wstring_view)> onLog;

        std::function<void(
            std::size_t done,
            std::size_t total)> onOverallProgress;

        std::function<void(
            int space,
            int ch,
            std::wstring_view chName)> onChannelStart;

        std::function<void(
            int space,
            int ch,
            int cycle,
            std::size_t rawEvents,
            std::size_t storeSize,
            std::size_t processedPackets)> onCycleProgress;

        std::function<void(
            const AutoReserve::RuleMatchResult& matchResult,
            const EpgHtmlPortal::WriteOptions& portalOpt,
            const std::wstring& reason)> onAutoReserveMatchResult;

        std::function<void(
            const EpgHtmlPortal::WriteOptions& portalOpt,
            const std::wstring& reason)> onAutoReserveRegisterRequest;
    };

    class CancelToken
    {
    public:
        void RequestCancel() noexcept;
        void Reset() noexcept;
        bool IsCancelled() const noexcept;

    private:
        std::atomic_bool cancelled_{ false };
    };

    struct HtmlContext
    {
        std::wstring mergedEpgPath;

        // 過去番組表分割用EPG。
        // 空なら epg_spaceX_before/ の日付別生成は行わない。
        std::wstring beforeMergedEpgPath;

        std::wstring channelListPath;

        // 互換用。
        // 旧LegacyやPluginが使う場合に残す。
        std::wstring outputHtmlPath;
        std::wstring beforeHtmlPath;

        // EpgHtmlPortal 側の親生成ユニットへ渡す材料。
        int space = 0;
        std::wstring outputDir;

        std::wstring title;
        std::wstring pluginSettings;
        HtmlRenderRoute renderRoute = HtmlRenderRoute::FixedLegacy;
    };

    class IHtmlRendererPlugin
    {
    public:
        virtual const wchar_t* GetPluginName() const = 0;
        virtual bool GenerateHtml(const HtmlContext& ctx) = 0;
        virtual void Release() = 0;
    };

    class HtmlRendererHost
    {
    public:
        using CreateFn = IHtmlRendererPlugin * (__cdecl*)();

        HtmlRendererHost() = default;

        void Reset() noexcept;
        bool Load(const std::wstring& dllPath);
        void Unload() noexcept;
        bool Generate(const HtmlContext& ctx);
        bool HasPlugin() const noexcept;

    private:
        void* hmod_{ nullptr };
        IHtmlRendererPlugin* plugin_{ nullptr };
    };

    struct Options
    {
        int space = 0;

        // 実際にロード済みの BonDriver パス。
        // EpgBuild_GUI 自体は BonDriver をロードしないため、呼び出し側が設定する。
        std::wstring bonDriverPathForLog;

        std::wstring channelListPathOverride;
        std::vector<int> selectedPhysicalChannels;

        bool useEnabledServicesFromConfig = true;
        std::wstring configIniPath = L"config.ini";

        std::wstring outputDir = L"epg_out";
        std::wstring basePrefix = L"EpgTest";

        std::wstring mergedEpgFileName;
        std::wstring htmlFileName;
        std::wstring beforeHtmlFileName;
        std::wstring beforeMergedEpgFileName;

        std::wstring htmlTitle = L"EPG";
        std::wstring topIndexFileName = L"index.html";

        bool enableTopIndexHtml = true;
        bool generateBeforeHtmlStubIfMissing = true;

        std::wstring htmlPluginDllPath;
        std::wstring htmlPluginSettings;

        HtmlRenderRoute htmlRenderRoute = HtmlRenderRoute::FixedLegacy;

        bool enableEitDump = true;
        bool enableUpdateMerge = true;
        bool enableAribTrace = false;

        // EPG取得時間モード。
        // 試験中はここを切り替えることで、
        // 自動判別 / 初回固定 / 短縮固定 / 手動固定を比較できる。
        EpgCaptureMode captureMode = EpgCaptureMode::AutoByExistingEpg;

        // 初回取得用。
        int captureSecondsPerCycle = 120;
        int maxCycles = 2;

        // 既存EPGありの更新用。
        int refreshCaptureSecondsPerCycle = 60;
        int refreshMaxCycles = 1;

        // 手動固定テスト用。
        int manualCaptureSecondsPerCycle = 120;
        int manualMaxCycles = 1;

        // 既存EPG判定の下限。
        std::uintmax_t existingEpgMinBytes = 4096;

        // 短縮更新に失敗した場合、既存EPGをマージ対象に戻す。
        bool reuseExistingEpgOnShortUpdateFailure = true;

        std::size_t ringBufferSize = 8192 * 2;
        int pushRetryCount = 8;
        int pushRetrySleepUs = 100;
    };

    class EpgBuild_GUI
    {
    public:
        EpgBuild_GUI() = default;

        void RequestCancel() noexcept;
        void ResetCancel() noexcept;
        bool IsCancelled() const noexcept;
        void Reset() noexcept;

        Result Run(
            IBonDriver2* bon,
            FilterManagerMode mode,
            const Options& opt,
            const Callbacks* cb = nullptr);

    private:
        CancelToken cancel_{};

        bool ProcessOneChannel(
            IBonDriver2* bon,
            FilterManagerMode mode,
            const Options& o,
            int ch,
            const std::wstring& chNameSafe,
            const Callbacks* cb,
            std::filesystem::path& outEpgPath);
    };
}

#endif // EPG_BUILD_GUI_HPP