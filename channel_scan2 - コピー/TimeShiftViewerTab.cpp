#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "TimeShiftViewerTab.hpp"


#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <system_error>
#include <sstream>
#include <iterator>
#include <ctime>
#include <initializer_list>
#include <atomic>
#include <memory>
#include <thread>
#include <limits>
#include <vector>
#include <cstdint>
#include <chrono>


#include "ConfigManager.h"
#include "TimeShiftProgramListWindow.hpp"
#include "AppWorkerPool.hpp"
#include "DebugManager.h"
#include "SIMDData.hpp"
#include "AppSimdAvx2.hpp"
#include "AppSimdSse2.hpp"
#include "AppSimdSse42.hpp"


namespace
{
    constexpr int IDC_TS_BTN_RELOAD = 7201;
    constexpr int IDC_TS_SUBTAB = 7202;
    
    
    constexpr int IDC_TS_BTN_OPEN_PROGRAM_WINDOW = 7206;

    
    constexpr int IDC_TS_LIST_STATUS = 7211;
    constexpr int IDC_TS_LIST_CHANNELS = 7212;
    constexpr int IDC_TS_LIST_SEGMENTS = 7213;

    constexpr wchar_t kClassName[] = L"TSCEPG_TimeShiftViewerTab";

    constexpr UINT WM_TS_REBUILD_DONE = WM_APP + 0x4A1;

    constexpr UINT_PTR IDT_TS_AUTO_REBUILD = 7301;

    // 初回チェックは少し早め。
    // 起動直後にセグメント生成が間に合わない場合の拾い直し用。
    constexpr DWORD kTimeShiftAutoRebuildInitialDelayMs = 60u * 1000u;

    // 通常の自動チェック間隔。
    // まずは5分。重ければ10分へ。
    constexpr DWORD kTimeShiftAutoRebuildIntervalMs = 5u * 60u * 1000u;

    // 再構築中にタイマーが来た場合の再試行。
    constexpr DWORD kTimeShiftAutoRebuildBusyRetryMs = 60u * 1000u;

    // セグメント数の小変化だけで重いEPG照合を走らせないための下限。
// 1分セグメントなので 20 = 約20分ぶん。
// 録画Writerを優先するため、自動再構築は粗めにする。
    constexpr std::size_t kTimeShiftAutoRebuildMinSegmentDelta = 20u;

    // EPG更新中の .epg を TimeShiftViewerTab が読みに行かないための保護。
// EPGファイルのサイズ/更新時刻が変化中、または更新直後なら一覧更新を延期する。
    constexpr DWORD kTimeShiftEpgReadSafetyQuietMs = 60u * 1000u;
    constexpr DWORD kTimeShiftEpgReadSafetyProbeWaitMs = 700u;
    constexpr DWORD kTimeShiftAutoRebuildEpgBusyRetryMs = 30u * 1000u;

    // ------------------------------------------------------------
// Forward declarations
// ------------------------------------------------------------
    std::filesystem::path GetExeDir_();
    std::filesystem::path GetDefaultConfigIniPath_();
    std::filesystem::path ResolveEpgOutFolder_();

    struct TimeShiftTargetChannel_;

    std::filesystem::path MakeTimeShiftTargetEpgPath_(
        const std::filesystem::path& epgOutFolder,
        const TimeShiftTargetChannel_& target);

    struct TimeShiftTargetChannel_
    {
        bool valid = false;

        int tunerIndex = -1;
        int space = 0;
        int channel = -1;

        std::uint16_t serviceId = 0;
        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;

        std::wstring name;
    };

    std::wstring MakeIndexedIniKey_(
        std::wstring_view base,
        int index)
    {
        std::wstring key(base);
        key += std::to_wstring(index);
        return key;
    }

    std::wstring ReadIniStringHeap_(
        const std::filesystem::path& iniPath,
        const std::wstring& section,
        const std::wstring& key)
    {
        std::vector<wchar_t> buf(4096u, L'\0');

        GetPrivateProfileStringW(
            section.c_str(),
            key.c_str(),
            L"",
            buf.data(),
            static_cast<DWORD>(buf.size()),
            iniPath.wstring().c_str());

        return std::wstring(buf.data());
    }

    int ReadIndexedIniInt_(
        const std::filesystem::path& iniPath,
        const std::wstring& section,
        std::wstring_view baseKey,
        int index,
        int defaultValue)
    {
        const std::wstring key =
            MakeIndexedIniKey_(baseKey, index);

        return GetPrivateProfileIntW(
            section.c_str(),
            key.c_str(),
            defaultValue,
            iniPath.wstring().c_str());
    }

    std::wstring ReadIndexedIniString_(
        const std::filesystem::path& iniPath,
        const std::wstring& section,
        std::wstring_view baseKey,
        int index)
    {
        const std::wstring key =
            MakeIndexedIniKey_(baseKey, index);

        return ReadIniStringHeap_(
            iniPath,
            section,
            key);
    }

    int ReadIndexedOrPlainIniInt_(
        const std::filesystem::path& iniPath,
        const std::wstring& section,
        std::wstring_view baseKey,
        int index,
        int defaultValue)
    {
        constexpr int kMissingValue =
            (std::numeric_limits<int>::min)();

        const std::wstring indexedKey =
            MakeIndexedIniKey_(baseKey, index);

        const int indexedValue =
            GetPrivateProfileIntW(
                section.c_str(),
                indexedKey.c_str(),
                kMissingValue,
                iniPath.wstring().c_str());

        if (indexedValue != kMissingValue) {
            return indexedValue;
        }

        const std::wstring plainKey(baseKey);

        return GetPrivateProfileIntW(
            section.c_str(),
            plainKey.c_str(),
            defaultValue,
            iniPath.wstring().c_str());
    }

    std::wstring ReadIndexedOrPlainIniString_(
        const std::filesystem::path& iniPath,
        const std::wstring& section,
        std::wstring_view baseKey,
        int index)
    {
        const std::wstring indexedKey =
            MakeIndexedIniKey_(baseKey, index);

        std::wstring value =
            ReadIniStringHeap_(
                iniPath,
                section,
                indexedKey);

        if (!value.empty()) {
            return value;
        }

        const std::wstring plainKey(baseKey);

        return ReadIniStringHeap_(
            iniPath,
            section,
            plainKey);
    }

    TimeShiftTargetChannel_ LoadTimeShiftTargetChannelFromConfig_()
    {
        const std::filesystem::path iniPath =
            GetDefaultConfigIniPath_();

        TimeShiftTargetChannel_ out{};

        for (int i = 1; i <= 32; ++i) {
            std::wstring section = L"Tuner";
            section += std::to_wstring(i);

            const int enable =
                ReadIndexedOrPlainIniInt_(
                    iniPath,
                    section,
                    L"Enable",
                    i,
                    0);

            const int useRole =
                ReadIndexedOrPlainIniInt_(
                    iniPath,
                    section,
                    L"UseRole",
                    i,
                    0);

            // UseRole=5 を TimeShift 用チューナーとして扱う。
            if (enable == 0 || useRole != 5) {
                continue;
            }

            const int space =
                ReadIndexedOrPlainIniInt_(
                    iniPath,
                    section,
                    L"TimeShiftLockSpace",
                    i,
                    -1);

            const int channel =
                ReadIndexedOrPlainIniInt_(
                    iniPath,
                    section,
                    L"TimeShiftLockChannel",
                    i,
                    -1);

            const int serviceId =
                ReadIndexedOrPlainIniInt_(
                    iniPath,
                    section,
                    L"TimeShiftLockServiceId",
                    i,
                    0);

            const int networkId =
                ReadIndexedOrPlainIniInt_(
                    iniPath,
                    section,
                    L"TimeShiftLockNetworkId",
                    i,
                    0);

            const int tsId =
                ReadIndexedOrPlainIniInt_(
                    iniPath,
                    section,
                    L"TimeShiftLockTsId",
                    i,
                    0);

            if (space < 0 || channel < 0 || serviceId <= 0) {
                DebugLog(
                    L"[TimeShiftViewer][Target] skip invalid Tuner%d enable=%d useRole=%d space=%d ch=%d sid=%d nid=%d tsid=%d",
                    i,
                    enable,
                    useRole,
                    space,
                    channel,
                    serviceId,
                    networkId,
                    tsId);
                continue;
            }

            out.valid = true;
            out.tunerIndex = i;
            out.space = space;
            out.channel = channel;
            out.serviceId = static_cast<std::uint16_t>(serviceId);
            out.networkId = static_cast<std::uint16_t>(networkId);
            out.tsId = static_cast<std::uint16_t>(tsId);
            out.name =
                ReadIndexedOrPlainIniString_(
                    iniPath,
                    section,
                    L"TimeShiftLockName",
                    i);

            DebugLog(
                L"[TimeShiftViewer][Target] found tuner=%d space=%d ch=%d sid=%u nid=%u tsid=%u name=%ls",
                out.tunerIndex,
                out.space,
                out.channel,
                static_cast<unsigned>(out.serviceId),
                static_cast<unsigned>(out.networkId),
                static_cast<unsigned>(out.tsId),
                out.name.c_str());

            return out;
        }

        DebugLog(L"[TimeShiftViewer][Target] TimeShift target tuner not found");
        return out;
    }

    std::filesystem::path MakeTimeShiftTargetEpgPath_(
        const std::filesystem::path& epgOutFolder,
        const TimeShiftTargetChannel_& target)
    {
        std::wstring fileName = L"EpgFull_space";
        fileName += std::to_wstring(target.space);
        fileName += L"_ch";
        fileName += std::to_wstring(target.channel);
        fileName += L"_EIT.epg";

        return epgOutFolder / fileName;
    }

    struct TimeShiftRebuildResult_
    {
        std::uint64_t requestId = 0;

        TimeShift_ViewIndex::BuildResult index{};

        TimeShiftTargetChannel_ target{};
        std::filesystem::path targetEpgPath{};
        std::filesystem::path epgSourceCachePath{};

        bool targetEpgFound = false;
        bool epgSourceCacheWritten = false;

        std::size_t sourceCount = 0;
        std::size_t usableCount = 0;
        std::size_t partialCount = 0;
        std::size_t edgeGapCandidateCount = 0;
        std::size_t smallMissingCount = 0;
        std::uint32_t maxMissingSec = 0;
        std::uint32_t maxGapSec = 0;
        std::size_t deletedCount = 0;
        std::size_t recordingCount = 0;

        std::size_t rawSourceCount = 0;
        std::size_t filteredByServiceId = 0;
        std::size_t filteredByNetworkId = 0;
        std::size_t filteredByTsId = 0;
        std::wstring firstRawSource;
        std::wstring firstAcceptedSource;

        std::size_t epgRawEvents = 0;
        std::size_t epgAcceptedEvents = 0;
        std::size_t epgSkipNoStartDate = 0;
        std::size_t epgSkipNoStartTime = 0;
        std::size_t epgSkipNoDuration = 0;
        std::size_t epgSkipNoTitle = 0;
        std::size_t epgSkipNoServiceId = 0;
        std::size_t epgSkipEndTimeFailed = 0;
        std::size_t epgSkipChannel = 0;
        std::size_t epgServiceMapHit = 0;
        std::size_t epgServiceMapMiss = 0;
        std::wstring epgFirstAcceptedTitle;
        std::wstring epgFirstSkippedTitle;

        // EPG本文診断
        std::size_t epgTextChars = 0;
        std::size_t epgLineCount = 0;
        std::size_t tableIdLineCount = 0;
        std::size_t tableIdPrefixLineCount = 0;
        std::size_t serviceIdLineCount = 0;
        std::size_t eventIdLineCount = 0;
        std::size_t startTimeLineCount = 0;
        std::size_t startTimeRawLineCount = 0;
        std::size_t durationLineCount = 0;
        std::size_t durationHmsLineCount = 0;
        std::size_t titleLineCount = 0;

        std::wstring firstNonEmptyLine;
        std::wstring firstTableIdLine;
        std::wstring firstServiceIdLine;
        std::wstring firstEventIdLine;
        std::wstring firstStartTimeLine;
        std::wstring firstDurationLine;
        std::wstring firstTitleLine;

        bool epgReadDeferredBySafeGuard = false;
        std::wstring epgReadSafeReason;
        std::uint64_t epgReadSafeAgeMs = 0;
        std::uint64_t epgReadSizeBefore = 0;
        std::uint64_t epgReadSizeAfter = 0;

        std::wstring message;


    };

    constexpr bool kTimeShiftGentleRebuildDefault = true;

    // 以前: Sleep=2ms / 最小間隔=20ms
    // 今回: 低負荷を維持しつつ、再構築の待ちを半分〜1/4程度へ短縮。
    constexpr DWORD kTimeShiftGentleSleepMs = 1;
    constexpr DWORD kTimeShiftGentleMinIntervalMs = 8;

    class ScopedBackgroundThreadMode_
    {
    public:
        ScopedBackgroundThreadMode_()
        {
            hThread_ = GetCurrentThread();

            backgroundMode_ =
                SetThreadPriority(
                    hThread_,
                    THREAD_MODE_BACKGROUND_BEGIN) != FALSE;

            if (!backgroundMode_) {
                oldPriority_ = GetThreadPriority(hThread_);
                if (oldPriority_ == THREAD_PRIORITY_ERROR_RETURN) {
                    oldPriority_ = THREAD_PRIORITY_NORMAL;
                }

                SetThreadPriority(hThread_, THREAD_PRIORITY_BELOW_NORMAL);
            }
        }

        ~ScopedBackgroundThreadMode_()
        {
            if (hThread_ == nullptr) {
                return;
            }

            if (backgroundMode_) {
                SetThreadPriority(hThread_, THREAD_MODE_BACKGROUND_END);
            }
            else {
                SetThreadPriority(hThread_, oldPriority_);
            }
        }

        ScopedBackgroundThreadMode_(const ScopedBackgroundThreadMode_&) = delete;
        ScopedBackgroundThreadMode_& operator=(const ScopedBackgroundThreadMode_&) = delete;

    private:
        HANDLE hThread_ = nullptr;
        bool backgroundMode_ = false;
        int oldPriority_ = THREAD_PRIORITY_NORMAL;
    };

    class TimeShiftGentleThrottle_
    {
    public:
        explicit TimeShiftGentleThrottle_(bool enabled) noexcept
            : enabled_(enabled)
        {
            lastSleepTick_ = GetTickCount64();
        }

        void Tick(std::size_t unit = 1) noexcept
        {
            if (!enabled_) {
                return;
            }

            counter_ += unit;

            const ULONGLONG now = GetTickCount64();

            if (counter_ < 256 &&
                now - lastSleepTick_ < kTimeShiftGentleMinIntervalMs) {
                return;
            }

            counter_ = 0;
            lastSleepTick_ = now;

            Sleep(kTimeShiftGentleSleepMs);
        }

        void PhaseBreak() noexcept
        {
            if (!enabled_) {
                return;
            }

            counter_ = 0;
            lastSleepTick_ = GetTickCount64();

            Sleep(kTimeShiftGentleSleepMs * 4);
        }

    private:
        bool enabled_ = true;
        std::size_t counter_ = 0;
        ULONGLONG lastSleepTick_ = 0;
    };

    void SetFontIf_(HWND h, HFONT f)
    {
        if (h != nullptr && f != nullptr) {
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        }
    }

    void AddColumn_(HWND hList, int index, const wchar_t* text, int width)
    {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<LPWSTR>(text);
        col.cx = width;
        col.iSubItem = index;

        ListView_InsertColumn(hList, index, &col);
    }

    void SetSubItemText_(HWND hList, int row, int col, const std::wstring& text)
    {
        ListView_SetItemText(
            hList,
            row,
            col,
            const_cast<LPWSTR>(text.c_str()));
    }

    int MaxInt_(int a, int b) noexcept
    {
        return (a > b) ? a : b;
    }

    int RectWidth_(const RECT& rc) noexcept
    {
        const LONG w = rc.right - rc.left;
        return (w > 0) ? static_cast<int>(w) : 0;
    }

    int RectHeight_(const RECT& rc) noexcept
    {
        const LONG h = rc.bottom - rc.top;
        return (h > 0) ? static_cast<int>(h) : 0;
    }

    HWND CreatePage_(HWND parent, HINSTANCE hInst)
    {
        return CreateWindowExW(
            WS_EX_CONTROLPARENT,
            L"STATIC",
            L"",
            WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0,
            0,
            100,
            100,
            parent,
            nullptr,
            hInst,
            nullptr);
    }

    void InitListView_(HWND hList)
    {
        if (hList == nullptr) {
            return;
        }

        ListView_SetExtendedListViewStyle(
            hList,
            LVS_EX_FULLROWSELECT |
            LVS_EX_GRIDLINES |
            LVS_EX_DOUBLEBUFFER);
    }

    std::size_t CountSegmentsForChannel_(
        const TimeShift_ViewIndex::BuildResult& index,
        const std::wstring& channelKey,
        bool closedOnly)
    {
        std::size_t count = 0;

        for (const auto& seg : index.segments) {
            if (seg.channelKey != channelKey) {
                continue;
            }

            if (closedOnly && !seg.closed) {
                continue;
            }

            ++count;
        }

        return count;
    }

    
        struct TimeShiftServiceMapEntry_
        {
            int space = 0;
            int channel = -1;

            std::uint16_t networkId = 0;
            std::uint16_t tsId = 0;
            std::uint16_t serviceId = 0;

            std::wstring serviceName;
        };

        std::wstring MakeServiceMapKey_(
            std::uint16_t networkId,
            std::uint16_t tsId,
            std::uint16_t serviceId)
        {
            std::wstring key = std::to_wstring(networkId);
            key += L"-";
            key += std::to_wstring(tsId);
            key += L"-";
            key += std::to_wstring(serviceId);
            return key;
        }

        std::filesystem::path GetExeDir_()
        {
            wchar_t path[MAX_PATH]{};
            const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) {
                return L".";
            }

            std::filesystem::path p(path);
            return p.parent_path();
        }

        std::filesystem::path GetDefaultConfigIniPath_()
        {
            return GetExeDir_() / L"config.ini";
        }

        std::filesystem::path ResolveEpgOutFolder_()
        {
            std::error_code ec;

            const std::filesystem::path exeEpgOut =
                GetExeDir_() / L"epg_out";

            if (std::filesystem::exists(exeEpgOut, ec) &&
                std::filesystem::is_directory(exeEpgOut, ec)) {
                return exeEpgOut;
            }

            ec.clear();

            const std::filesystem::path cwd =
                std::filesystem::current_path(ec);

            if (!ec) {
                const std::filesystem::path cwdEpgOut =
                    cwd / L"epg_out";

                if (std::filesystem::exists(cwdEpgOut, ec) &&
                    std::filesystem::is_directory(cwdEpgOut, ec)) {
                    return cwdEpgOut;
                }
            }

            return L"epg_out";
        }

        std::wstring MakeTimeShiftCopyPlanText_(
            const TimeShift_ViewIndex::ProgramEntry& program,
            const std::filesystem::path& outputPath)
        {
            std::wstring msg;

            msg += L"[TimeShiftViewer] 番組コピー計画";
            msg += L" title=";
            msg += program.title;

            msg += L" program=";
            msg += program.startText;
            msg += L"～";
            msg += program.endText;

            msg += L" coverage=";
            msg += std::to_wstring(program.coveragePermille);
            msg += L"/1000";

            msg += L" joinSegments=";
            msg += std::to_wstring(program.joinSegments.size());

            msg += L" padding=前";
            msg += std::to_wstring(program.joinPaddingBeforeCount);
            msg += L"/後";
            msg += std::to_wstring(program.joinPaddingAfterCount);

            msg += L" joinRange=";

            if (!program.joinPlanStartText.empty() ||
                !program.joinPlanEndText.empty()) {
                msg += program.joinPlanStartText;
                msg += L"～";
                msg += program.joinPlanEndText;
            }
            else if (!program.joinSegments.empty()) {
                msg += program.joinSegments.front().startText;
                msg += L"～";
                msg += program.joinSegments.back().endText;
            }
            else {
                msg += L"(empty)";
            }

            msg += L" bytes=";
            msg += TimeShift_ViewIndex::FormatBytes(program.totalBytes);

            msg += L" output=";
            msg += outputPath.wstring();

            return msg;
        }

        std::uint32_t ClampIniUInt_(
            int value,
            std::uint32_t defaultValue,
            std::uint32_t minValue,
            std::uint32_t maxValue) noexcept
        {
            if (value < static_cast<int>(minValue)) {
                return defaultValue;
            }

            const std::uint32_t u =
                static_cast<std::uint32_t>(value);

            if (u > maxValue) {
                return maxValue;
            }

            return u;
        }

        TimeShift_ViewIndex::ProgramBuildOptions LoadTimeShiftProgramBuildOptions_()
        {
            const std::filesystem::path iniPath =
                GetDefaultConfigIniPath_();

            TimeShift_ViewIndex::ProgramBuildOptions opt{};

            const int segmentSeconds =
                GetPrivateProfileIntW(
                    L"TimeShift",
                    L"SegmentSeconds",
                    60,
                    iniPath.wstring().c_str());

            const int boundaryProbe =
                GetPrivateProfileIntW(
                    L"TimeShift",
                    L"BoundaryProbeSegmentCount",
                    1,
                    iniPath.wstring().c_str());

            const int joinPadding =
                GetPrivateProfileIntW(
                    L"TimeShift",
                    L"JoinPaddingSegmentCount",
                    1,
                    iniPath.wstring().c_str());

            const int requiredCoverage =
                GetPrivateProfileIntW(
                    L"TimeShift",
                    L"RequiredCoveragePermille",
                    1000,
                    iniPath.wstring().c_str());

            const int includeRecording =
                GetPrivateProfileIntW(
                    L"TimeShift",
                    L"IncludeRecordingPrograms",
                    0,
                    iniPath.wstring().c_str());

            opt.segmentSeconds =
                ClampIniUInt_(
                    segmentSeconds,
                    60u,
                    1u,
                    3600u);

            opt.boundaryProbeSegmentCount =
                ClampIniUInt_(
                    boundaryProbe,
                    1u,
                    0u,
                    10u);

            opt.joinPaddingSegmentCount =
                ClampIniUInt_(
                    joinPadding,
                    1u,
                    0u,
                    10u);

            opt.requiredCoveragePermille =
                ClampIniUInt_(
                    requiredCoverage,
                    1000u,
                    1u,
                    1000u);

            opt.includeRecording =
                includeRecording != 0;

            DebugLog(
                L"[TimeShiftViewer][ProgramBuildOptions] segmentSec=%u boundaryProbe=%u joinPadding=%u required=%u includeRecording=%d",
                static_cast<unsigned>(opt.segmentSeconds),
                static_cast<unsigned>(opt.boundaryProbeSegmentCount),
                static_cast<unsigned>(opt.joinPaddingSegmentCount),
                static_cast<unsigned>(opt.requiredCoveragePermille),
                opt.includeRecording ? 1 : 0);

            return opt;
        }

        std::filesystem::file_time_type GetLastWriteTimeSafe_(
            const std::filesystem::path& path,
            bool& exists)
        {
            exists = false;

            std::error_code ec;

            if (path.empty() ||
                !std::filesystem::exists(path, ec) ||
                !std::filesystem::is_regular_file(path, ec)) {
                return {};
            }

            ec.clear();

            const auto t =
                std::filesystem::last_write_time(path, ec);

            if (ec) {
                return {};
            }

            exists = true;
            return t;
        }

        struct TimeShiftEpgReadSafetyProbe_
        {
            bool safeToRead = true;
            bool exists = false;
            bool regularFile = false;

            bool sizeStable = true;
            bool writeTimeStable = true;
            bool quietEnough = true;

            std::uint64_t sizeBefore = 0;
            std::uint64_t sizeAfter = 0;
            std::uint64_t lastWriteAgeMs = 0;

            std::wstring reason;
        };

        TimeShiftEpgReadSafetyProbe_ ProbeTimeShiftEpgReadSafety_(
            const std::filesystem::path& epgPath) noexcept
        {
            TimeShiftEpgReadSafetyProbe_ out{};

            try {
                if (epgPath.empty()) {
                    out.safeToRead = true;
                    out.reason = L"EPG path is empty";
                    return out;
                }

                std::error_code ec;

                if (!std::filesystem::exists(epgPath, ec)) {
                    out.safeToRead = true;
                    out.exists = false;
                    out.reason = L"EPG file not found";
                    return out;
                }

                if (ec) {
                    out.safeToRead = false;
                    out.reason = L"EPG exists check failed";
                    return out;
                }

                out.exists = true;

                if (!std::filesystem::is_regular_file(epgPath, ec)) {
                    out.safeToRead = false;
                    out.regularFile = false;
                    out.reason = L"EPG path is not regular file";
                    return out;
                }

                if (ec) {
                    out.safeToRead = false;
                    out.reason = L"EPG regular file check failed";
                    return out;
                }

                out.regularFile = true;

                const auto write0 =
                    std::filesystem::last_write_time(epgPath, ec);

                if (ec) {
                    out.safeToRead = false;
                    out.reason = L"EPG last_write_time before probe failed";
                    return out;
                }

                const auto size0 =
                    std::filesystem::file_size(epgPath, ec);

                if (ec) {
                    out.safeToRead = false;
                    out.reason = L"EPG file_size before probe failed";
                    return out;
                }

                out.sizeBefore =
                    static_cast<std::uint64_t>(size0);

                Sleep(kTimeShiftEpgReadSafetyProbeWaitMs);

                if (!std::filesystem::exists(epgPath, ec)) {
                    out.safeToRead = false;
                    out.exists = false;
                    out.reason = L"EPG file disappeared during probe";
                    return out;
                }

                const auto write1 =
                    std::filesystem::last_write_time(epgPath, ec);

                if (ec) {
                    out.safeToRead = false;
                    out.reason = L"EPG last_write_time after probe failed";
                    return out;
                }

                const auto size1 =
                    std::filesystem::file_size(epgPath, ec);

                if (ec) {
                    out.safeToRead = false;
                    out.reason = L"EPG file_size after probe failed";
                    return out;
                }

                out.sizeAfter =
                    static_cast<std::uint64_t>(size1);

                if (size0 != size1) {
                    out.safeToRead = false;
                    out.sizeStable = false;
                    out.reason = L"EPG file size is changing";
                    return out;
                }

                if (write0 != write1) {
                    out.safeToRead = false;
                    out.writeTimeStable = false;
                    out.reason = L"EPG last_write_time is changing";
                    return out;
                }

                const auto now =
                    std::filesystem::file_time_type::clock::now();

                const auto age =
                    now >= write1
                    ? now - write1
                    : std::filesystem::file_time_type::duration::zero();

                const auto ageMsSigned =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        age).count();

                out.lastWriteAgeMs =
                    ageMsSigned > 0
                    ? static_cast<std::uint64_t>(ageMsSigned)
                    : 0u;

                if (out.lastWriteAgeMs < kTimeShiftEpgReadSafetyQuietMs) {
                    out.safeToRead = false;
                    out.quietEnough = false;
                    out.reason = L"EPG file was updated too recently";
                    return out;
                }

                out.safeToRead = true;
                out.reason = L"EPG file is stable";
                return out;
            }
            catch (...) {
                out.safeToRead = false;
                out.reason = L"exception in EPG read safety probe";
                return out;
            }
        }

        std::wstring MakeTimeShiftEpgSafetyInfoText_(
            const TimeShiftEpgReadSafetyProbe_& probe,
            const std::filesystem::path& epgPath)
        {
            std::wstring msg;

            msg += L"EPG更新中、またはEPGファイル更新直後の可能性があるため、";
            msg += L"タイムシフト番組一覧の自動更新を延期しました。\r\n";

            msg += L"録画中のTimeShift TS保存は継続します。\r\n";

            msg += L"\r\nreason: ";
            msg += probe.reason;

            msg += L"\r\nageMs: ";
            msg += std::to_wstring(probe.lastWriteAgeMs);

            msg += L"\r\nsizeBefore: ";
            msg += std::to_wstring(probe.sizeBefore);

            msg += L"\r\nsizeAfter: ";
            msg += std::to_wstring(probe.sizeAfter);

            msg += L"\r\npath: ";
            msg += epgPath.wstring();

            return msg;
        }

        std::vector<std::wstring> SplitCsvSimple_(const std::wstring& s)
        {
            std::vector<std::wstring> out;
            std::wstring cur;

            for (const wchar_t ch : s) {
                if (ch == L',') {
                    out.push_back(cur);
                    cur.clear();
                }
                else {
                    cur.push_back(ch);
                }
            }

            out.push_back(cur);
            return out;
        }

        unsigned long ToULong_(const std::wstring& s) noexcept
        {
            if (s.empty()) {
                return 0;
            }

            wchar_t* endPtr = nullptr;
            const unsigned long v = std::wcstoul(s.c_str(), &endPtr, 10);
            if (endPtr == s.c_str()) {
                return 0;
            }

            return v;
        }

        int ToInt_(const std::wstring& s) noexcept
        {
            if (s.empty()) {
                return 0;
            }

            wchar_t* endPtr = nullptr;
            const long v = std::wcstol(s.c_str(), &endPtr, 10);
            if (endPtr == s.c_str()) {
                return 0;
            }

            return static_cast<int>(v);
        }

        std::unordered_map<std::wstring, TimeShiftServiceMapEntry_>
            LoadTimeShiftServiceMapFromConfig_(int space)
        {
            std::unordered_map<std::wstring, TimeShiftServiceMapEntry_> out;

            const std::filesystem::path iniPath = GetDefaultConfigIniPath_();

            std::wstring section = L"EpgServiceEnable_space";
            section += std::to_wstring(space);

            std::vector<wchar_t> buf(512u * 1024u);

            DWORD read = 0;
            for (;;) {
                read = GetPrivateProfileSectionW(
                    section.c_str(),
                    buf.data(),
                    static_cast<DWORD>(buf.size()),
                    iniPath.wstring().c_str());

                if (read + 2 < buf.size()) {
                    break;
                }

                if (buf.size() >= 4u * 1024u * 1024u) {
                    break;
                }

                buf.resize(buf.size() * 2u);
            }

            if (read == 0) {
                return out;
            }

            const wchar_t* p = buf.data();

            while (*p != L'\0') {
                std::wstring line = p;
                p += line.size() + 1;

                const std::size_t eq = line.find(L'=');
                if (eq == std::wstring::npos) {
                    continue;
                }

                const std::wstring keyText = line.substr(0, eq);
                const std::wstring valueText = line.substr(eq + 1);

                const auto cols = SplitCsvSimple_(valueText);

                // value:
                // enabled,serviceName,space,channel,serviceType,serviceId,networkId,tsId,...
                if (cols.size() < 8) {
                    continue;
                }

                TimeShiftServiceMapEntry_ e{};
                e.serviceName = cols[1];
                e.space = ToInt_(cols[2]);
                e.channel = ToInt_(cols[3]);
                e.serviceId = static_cast<std::uint16_t>(ToULong_(cols[5]));
                e.networkId = static_cast<std::uint16_t>(ToULong_(cols[6]));
                e.tsId = static_cast<std::uint16_t>(ToULong_(cols[7]));

                if (e.channel < 0 || e.serviceId == 0) {
                    continue;
                }

                const std::wstring mapKey =
                    MakeServiceMapKey_(e.networkId, e.tsId, e.serviceId);

                out[mapKey] = std::move(e);

                // 念のため、左辺の network-ts-service キーでも引けるようにする。
                if (!keyText.empty()) {
                    out[keyText] = out[mapKey];
                }
            }

            return out;
        }

        std::wstring Utf8ToWide_(const std::string& s)
        {
            if (s.empty()) {
                return {};
            }

            const int len = MultiByteToWideChar(
                CP_UTF8,
                0,
                s.data(),
                static_cast<int>(s.size()),
                nullptr,
                0);

            if (len <= 0) {
                return {};
            }

            std::wstring out(static_cast<std::size_t>(len), L'\0');

            MultiByteToWideChar(
                CP_UTF8,
                0,
                s.data(),
                static_cast<int>(s.size()),
                out.data(),
                len);

            return out;
        }

        std::wstring ReadUtf8FileToWide_(const std::filesystem::path& path)
        {
            try {
                std::ifstream ifs(path, std::ios::binary | std::ios::in);
                if (!ifs.is_open()) {
                    return {};
                }

                std::string bytes(
                    (std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>());

                ifs.close();

                if (bytes.size() >= 3 &&
                    static_cast<unsigned char>(bytes[0]) == 0xEF &&
                    static_cast<unsigned char>(bytes[1]) == 0xBB &&
                    static_cast<unsigned char>(bytes[2]) == 0xBF) {
                    bytes.erase(bytes.begin(), bytes.begin() + 3);
                }

                return Utf8ToWide_(bytes);
            }
            catch (...) {
                return {};
            }
        }

        std::string WideToUtf8_(const std::wstring& s)
        {
            if (s.empty()) {
                return {};
            }

            const int len =
                WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    s.data(),
                    static_cast<int>(s.size()),
                    nullptr,
                    0,
                    nullptr,
                    nullptr);

            if (len <= 0) {
                return {};
            }

            std::string out(static_cast<std::size_t>(len), '\0');

            WideCharToMultiByte(
                CP_UTF8,
                0,
                s.data(),
                static_cast<int>(s.size()),
                out.data(),
                len,
                nullptr,
                nullptr);

            return out;
        }

        std::wstring EscapeTsvCell_(const std::wstring& s)
        {
            std::wstring out;
            out.reserve(s.size());

            for (const wchar_t ch : s) {
                if (ch == L'\t' || ch == L'\r' || ch == L'\n') {
                    out.push_back(L' ');
                }
                else {
                    out.push_back(ch);
                }
            }

            return out;
        }

        std::filesystem::path MakeTimeShiftEpgSourceCachePath_(
            const std::filesystem::path& timeShiftRoot,
            const TimeShiftTargetChannel_& target)
        {
            std::wstring fileName = L"space";
            fileName += std::to_wstring(target.space);
            fileName += L"_ch";
            fileName += std::to_wstring(target.channel);
            fileName += L"_sid";
            fileName += std::to_wstring(target.serviceId);
            fileName += L".tsv";

            return
                timeShiftRoot /
                L"cache" /
                L"epg_source" /
                fileName;
        }

        bool WriteTimeShiftEpgSourceCache_(
            const std::filesystem::path& cachePath,
            const std::vector<TimeShift_ViewIndex::ProgramSourceEntry>& sources)
        {
            std::error_code ec;

            std::filesystem::create_directories(
                cachePath.parent_path(),
                ec);

            std::ofstream ofs(
                cachePath,
                std::ios::binary | std::ios::trunc);

            if (!ofs.is_open()) {
                return false;
            }

            const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
            ofs.write(reinterpret_cast<const char*>(bom), 3);

            ofs
                << "space\tchannel\tnetworkId\ttsId\tserviceId\teventId\t"
                << "start\tend\tserviceName\ttitle\n";

            for (const auto& s : sources) {
                std::wstring line;

                line += std::to_wstring(s.space);
                line += L"\t";
                line += std::to_wstring(s.channel);
                line += L"\t";
                line += std::to_wstring(s.networkId);
                line += L"\t";
                line += std::to_wstring(s.tsId);
                line += L"\t";
                line += std::to_wstring(s.serviceId);
                line += L"\t";
                line += std::to_wstring(s.eventId);
                line += L"\t";
                line += EscapeTsvCell_(s.startText);
                line += L"\t";
                line += EscapeTsvCell_(s.endText);
                line += L"\t";
                line += EscapeTsvCell_(s.serviceName);
                line += L"\t";
                line += EscapeTsvCell_(s.title);
                line += L"\n";

                const std::string u8 = WideToUtf8_(line);
                ofs.write(
                    u8.data(),
                    static_cast<std::streamsize>(u8.size()));
            }

            return true;
        }

        bool HasSuffixText_(const std::wstring& s, const std::wstring& suffix) noexcept
        {
            if (s.size() < suffix.size()) {
                return false;
            }

            return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
        }

        std::wstring HtmlDecodeOnce_(const std::wstring& s)
        {
            std::wstring out;
            out.reserve(s.size());

            for (std::size_t i = 0; i < s.size();) {
                if (s[i] != L'&') {
                    out.push_back(s[i]);
                    ++i;
                    continue;
                }

                if (s.compare(i, 5, L"&amp;") == 0) {
                    out.push_back(L'&');
                    i += 5;
                }
                else if (s.compare(i, 6, L"&quot;") == 0) {
                    out.push_back(L'"');
                    i += 6;
                }
                else if (s.compare(i, 5, L"&#39;") == 0) {
                    out.push_back(L'\'');
                    i += 5;
                }
                else if (s.compare(i, 4, L"&lt;") == 0) {
                    out.push_back(L'<');
                    i += 4;
                }
                else if (s.compare(i, 4, L"&gt;") == 0) {
                    out.push_back(L'>');
                    i += 4;
                }
                else if (s.compare(i, 8, L"&#12316;") == 0) {
                    out.push_back(L'〜');
                    i += 8;
                }
                else {
                    out.push_back(s[i]);
                    ++i;
                }
            }

            return out;
        }

        std::wstring HtmlDecode_(const std::wstring& s)
        {
            // data-time が &amp;#12316; のように二段階で入ることがあるため2回通す。
            return HtmlDecodeOnce_(HtmlDecodeOnce_(s));
        }

        std::wstring ExtractAttr_(const std::wstring& tag, const wchar_t* attrName)
        {
            std::wstring key = attrName;
            key += L"=\"";

            const std::size_t pos = tag.find(key);
            if (pos == std::wstring::npos) {
                return {};
            }

            const std::size_t valueStart = pos + key.size();
            const std::size_t valueEnd = tag.find(L'"', valueStart);
            if (valueEnd == std::wstring::npos || valueEnd < valueStart) {
                return {};
            }

            return HtmlDecode_(tag.substr(valueStart, valueEnd - valueStart));
        }

        std::wstring NormalizeDateText_(const std::wstring& dateText)
        {
            // 2026/06/17 -> 2026-06-17
            std::wstring d = dateText;

            for (auto& ch : d) {
                if (ch == L'/') {
                    ch = L'-';
                }
            }

            return d;
        }

        bool ParseTimeRange_(
            const std::wstring& dataTime,
            std::wstring& startHHMM,
            std::wstring& endHHMM)
        {
            startHHMM.clear();
            endHHMM.clear();

            std::wstring s = HtmlDecode_(dataTime);

            std::size_t sep = s.find(L'〜');
            if (sep == std::wstring::npos) {
                sep = s.find(L'～');
            }
            if (sep == std::wstring::npos) {
                sep = s.find(L"&#12316;");
            }
            if (sep == std::wstring::npos) {
                return false;
            }

            if (sep < 5 || sep + 5 >= s.size()) {
                return false;
            }

            startHHMM = s.substr(sep - 5, 5);
            endHHMM = s.substr(sep + 1, 5);

            if (startHHMM.size() != 5 || endHHMM.size() != 5) {
                return false;
            }

            return true;
        }

        int ParseHHMMMinutes_(const std::wstring& hhmm) noexcept
        {
            if (hhmm.size() < 5) {
                return -1;
            }

            const int h =
                (hhmm[0] - L'0') * 10 +
                (hhmm[1] - L'0');

            const int m =
                (hhmm[3] - L'0') * 10 +
                (hhmm[4] - L'0');

            if (h < 0 || h > 23 || m < 0 || m > 59) {
                return -1;
            }

            return h * 60 + m;
        }

        bool MakeStartEndText_(
            const std::wstring& dateText,
            const std::wstring& dataTime,
            std::wstring& startText,
            std::wstring& endText)
        {
            startText.clear();
            endText.clear();

            std::wstring startHHMM;
            std::wstring endHHMM;

            if (!ParseTimeRange_(dataTime, startHHMM, endHHMM)) {
                return false;
            }

            const std::wstring d = NormalizeDateText_(dateText);
            if (d.size() < 10) {
                return false;
            }

            startText = d + L" " + startHHMM + L":00";

            int startMin = ParseHHMMMinutes_(startHHMM);
            int endMin = ParseHHMMMinutes_(endHHMM);

            std::wstring endDate = d;

            // 同一番組が日跨ぎする場合。
            if (startMin >= 0 && endMin >= 0 && endMin < startMin) {
                SYSTEMTIME st{};
                st.wYear = static_cast<WORD>(ToInt_(d.substr(0, 4)));
                st.wMonth = static_cast<WORD>(ToInt_(d.substr(5, 2)));
                st.wDay = static_cast<WORD>(ToInt_(d.substr(8, 2)));

                FILETIME ft{};
                if (SystemTimeToFileTime(&st, &ft)) {
                    ULARGE_INTEGER ui{};
                    ui.LowPart = ft.dwLowDateTime;
                    ui.HighPart = ft.dwHighDateTime;
                    ui.QuadPart += 24ull * 60ull * 60ull * 10000000ull;

                    ft.dwLowDateTime = ui.LowPart;
                    ft.dwHighDateTime = ui.HighPart;

                    SYSTEMTIME next{};
                    if (FileTimeToSystemTime(&ft, &next)) {
                        wchar_t buf[16]{};
                        swprintf_s(
                            buf,
                            L"%04u-%02u-%02u",
                            static_cast<unsigned>(next.wYear),
                            static_cast<unsigned>(next.wMonth),
                            static_cast<unsigned>(next.wDay));
                        endDate = buf;
                    }
                }
            }

            endText = endDate + L" " + endHHMM + L":00";
            return true;
        }

        std::wstring TrimW_(const std::wstring& s)
        {
            std::size_t first = 0;
            while (first < s.size() &&
                (s[first] == L' ' ||
                    s[first] == L'\t' ||
                    s[first] == L'\r' ||
                    s[first] == L'\n')) {
                ++first;
            }

            std::size_t last = s.size();
            while (last > first &&
                (s[last - 1] == L' ' ||
                    s[last - 1] == L'\t' ||
                    s[last - 1] == L'\r' ||
                    s[last - 1] == L'\n')) {
                --last;
            }

            return s.substr(first, last - first);
        }

        std::wstring ExtractAfterPrefixTrim_(
            const std::wstring& line,
            const std::wstring& prefix)
        {
            if (line.rfind(prefix, 0) != 0) {
                return {};
            }

            return TrimW_(line.substr(prefix.size()));
        }

        std::wstring LimitDebugText_(
            const std::wstring& text,
            std::size_t maxChars = 180)
        {
            std::wstring out = TrimW_(text);

            for (auto& ch : out) {
                if (ch == L'\t' || ch == L'\r' || ch == L'\n') {
                    ch = L' ';
                }
            }

            if (out.size() > maxChars) {
                out.resize(maxChars);
                out += L"...";
            }

            return out;
        }

        bool ContainsAnyText_(
            std::wstring_view text,
            std::initializer_list<std::wstring_view> keys)
        {
            for (const auto key : keys) {
                if (!key.empty() &&
                    text.find(key) != std::wstring_view::npos) {
                    return true;
                }
            }

            return false;
        }

        void ProbeEpgTextForTimeShift_(
            const std::wstring& text,
            TimeShiftRebuildResult_* result)
        {
            if (result == nullptr) {
                return;
            }

            result->epgTextChars = text.size();

            std::wstringstream ss(text);
            std::wstring line;

            while (std::getline(ss, line)) {
                if (!line.empty() && line.back() == L'\r') {
                    line.pop_back();
                }

                const std::wstring trimmed = TrimW_(line);

                ++result->epgLineCount;

                if (!trimmed.empty() &&
                    result->firstNonEmptyLine.empty()) {
                    result->firstNonEmptyLine = LimitDebugText_(trimmed);
                }

                if (ContainsAnyText_(
                    trimmed,
                    { L"TableID", L"Table ID", L"table_id", L"tableId" })) {
                    ++result->tableIdLineCount;

                    if (result->firstTableIdLine.empty()) {
                        result->firstTableIdLine = LimitDebugText_(trimmed);
                    }
                }

                if (trimmed.rfind(L"TableID:", 0) == 0) {
                    ++result->tableIdPrefixLineCount;
                }

                if (ContainsAnyText_(
                    trimmed,
                    { L"ServiceID", L"ServiceId", L"SID", L"service_id" })) {
                    ++result->serviceIdLineCount;

                    if (result->firstServiceIdLine.empty()) {
                        result->firstServiceIdLine = LimitDebugText_(trimmed);
                    }
                }

                if (ContainsAnyText_(
                    trimmed,
                    { L"EventID", L"EventId", L"EID", L"event_id" })) {
                    ++result->eventIdLineCount;

                    if (result->firstEventIdLine.empty()) {
                        result->firstEventIdLine = LimitDebugText_(trimmed);
                    }
                }

                if (ContainsAnyText_(
                    trimmed,
                    { L"StartTime", L"Start Time", L"開始", L"start_time" })) {
                    ++result->startTimeLineCount;

                    if (result->firstStartTimeLine.empty()) {
                        result->firstStartTimeLine = LimitDebugText_(trimmed);
                    }
                }

                if (trimmed.rfind(L"StartTime(raw MJD+BCD40bit):", 0) == 0) {
                    ++result->startTimeRawLineCount;
                }

                if (ContainsAnyText_(
                    trimmed,
                    { L"Duration", L"duration", L"長さ", L"継続" })) {
                    ++result->durationLineCount;

                    if (result->firstDurationLine.empty()) {
                        result->firstDurationLine = LimitDebugText_(trimmed);
                    }
                }

                if (trimmed.rfind(L"Duration(HH:MM:SS):", 0) == 0) {
                    ++result->durationHmsLineCount;
                }

                if (ContainsAnyText_(
                    trimmed,
                    {
                        L"[短形式タイトル]",
                        L"[タイトル]",
                        L"[イベント名]",
                        L"ShortEvent",
                        L"EventName",
                        L"title",
                        L"Title"
                    })) {
                    ++result->titleLineCount;

                    if (result->firstTitleLine.empty()) {
                        result->firstTitleLine = LimitDebugText_(trimmed);
                    }
                }
            }
        }

        bool IsNameChar_(wchar_t ch) noexcept
        {
            return
                (ch >= L'0' && ch <= L'9') ||
                (ch >= L'A' && ch <= L'Z') ||
                (ch >= L'a' && ch <= L'z') ||
                ch == L'_';
        }

        int HexValue_(wchar_t ch) noexcept
        {
            if (ch >= L'0' && ch <= L'9') {
                return static_cast<int>(ch - L'0');
            }
            if (ch >= L'A' && ch <= L'F') {
                return 10 + static_cast<int>(ch - L'A');
            }
            if (ch >= L'a' && ch <= L'f') {
                return 10 + static_cast<int>(ch - L'a');
            }

            return -1;
        }

        int FindNumberAfterKey_(
            std::wstring_view line,
            std::wstring_view key)
        {
            if (line.empty() || key.empty()) {
                return 0;
            }

            std::size_t searchPos = 0;

            while (searchPos < line.size()) {
                const std::size_t pos = line.find(key, searchPos);
                if (pos == std::wstring_view::npos) {
                    return 0;
                }

                const bool leftOk =
                    pos == 0 ||
                    !IsNameChar_(line[pos - 1]);

                std::size_t cur = pos + key.size();

                const bool rightOk =
                    cur >= line.size() ||
                    !IsNameChar_(line[cur]);

                if (!leftOk || !rightOk) {
                    searchPos = pos + 1;
                    continue;
                }

                while (cur < line.size() &&
                    (line[cur] == L' ' ||
                        line[cur] == L'\t' ||
                        line[cur] == L'=' ||
                        line[cur] == L':' ||
                        line[cur] == L'(' ||
                        line[cur] == L')')) {
                    ++cur;
                }

                if (cur >= line.size()) {
                    return 0;
                }

                bool isHex = false;

                if ((cur + 1) < line.size() &&
                    line[cur] == L'0' &&
                    (line[cur + 1] == L'x' || line[cur + 1] == L'X')) {
                    isHex = true;
                    cur += 2;
                }

                std::size_t end = cur;

                if (isHex) {
                    while (end < line.size() && HexValue_(line[end]) >= 0) {
                        ++end;
                    }
                }
                else {
                    while (end < line.size() &&
                        line[end] >= L'0' &&
                        line[end] <= L'9') {
                        ++end;
                    }
                }

                if (end <= cur) {
                    return 0;
                }

                try {
                    const std::wstring numberText(line.substr(cur, end - cur));

                    if (isHex) {
                        return std::stoi(numberText, nullptr, 16);
                    }

                    return std::stoi(numberText, nullptr, 10);
                }
                catch (...) {
                    return 0;
                }
            }

            return 0;
        }

        int FindNumberAny_(
            std::wstring_view line,
            std::initializer_list<std::wstring_view> keys)
        {
            for (const auto key : keys) {
                const int v = FindNumberAfterKey_(line, key);
                if (v > 0) {
                    return v;
                }
            }

            return 0;
        }

        std::wstring FormatIsoDate_(int y, int m, int d)
        {
            wchar_t buf[16]{};
            swprintf_s(buf, L"%04d-%02d-%02d", y, m, d);
            return buf;
        }

        std::wstring FormatIsoTime_(int h, int m, int s)
        {
            wchar_t buf[16]{};
            swprintf_s(buf, L"%02d:%02d:%02d", h, m, s);
            return buf;
        }

        bool DecodeMjdToDate_(int mjd, int& y, int& m, int& d)
        {
            long j = static_cast<long>(mjd) + 2400001L + 68569L;
            long c = 4L * j / 146097L;
            j = j - (146097L * c + 3L) / 4L;
            long yy = 4000L * (j + 1L) / 1461001L;
            j = j - 1461L * yy / 4L + 31L;
            long mm = 80L * j / 2447L;
            long dd = j - 2447L * mm / 80L;
            j = mm / 11L;
            mm = mm + 2L - 12L * j;
            yy = 100L * (c - 49L) + yy + j;

            y = static_cast<int>(yy);
            m = static_cast<int>(mm);
            d = static_cast<int>(dd);
            return true;
        }

        int DecodeBcdByte_(std::uint8_t v) noexcept
        {
            return static_cast<int>(((v >> 4) & 0x0F) * 10 + (v & 0x0F));
        }

        bool ParseStartHexLine_(
            std::wstring_view line,
            std::wstring& outDate,
            std::wstring& outTime)
        {
            outDate.clear();
            outTime.clear();

            const std::size_t hexPos = line.find(L"0x");
            if (hexPos == std::wstring_view::npos) {
                return false;
            }

            std::wstring hex;
            hex.reserve(16);

            for (std::size_t i = hexPos + 2; i < line.size(); ++i) {
                const wchar_t c = line[i];

                const bool isHex =
                    (c >= L'0' && c <= L'9') ||
                    (c >= L'A' && c <= L'F') ||
                    (c >= L'a' && c <= L'f');

                if (!isHex) {
                    break;
                }

                hex.push_back(c);

                if (hex.size() >= 16) {
                    break;
                }
            }

            if (hex.size() < 10) {
                return false;
            }

            std::uint64_t raw = 0;

            try {
                raw = static_cast<std::uint64_t>(
                    std::stoull(hex, nullptr, 16));
            }
            catch (...) {
                return false;
            }

            const int mjd =
                static_cast<int>((raw >> 24) & 0xFFFFULL);

            int y = 0;
            int mo = 0;
            int dd = 0;

            if (!DecodeMjdToDate_(mjd, y, mo, dd)) {
                return false;
            }

            const int hh =
                DecodeBcdByte_(
                    static_cast<std::uint8_t>((raw >> 16) & 0xFFULL));

            const int mm =
                DecodeBcdByte_(
                    static_cast<std::uint8_t>((raw >> 8) & 0xFFULL));

            const int ss =
                DecodeBcdByte_(
                    static_cast<std::uint8_t>(raw & 0xFFULL));

            if (hh > 23 || mm > 59 || ss > 59) {
                return false;
            }

            outDate = FormatIsoDate_(y, mo, dd);
            outTime = FormatIsoTime_(hh, mm, ss);

            return true;
        }

        bool ParseDurationLine_(
            const std::wstring& line,
            std::uint32_t& outDurationSec)
        {
            outDurationSec = 0;

            // 例:
            // Duration(HH:MM:SS): 00:17:00, RunningStatus: 0, FreeCA: 0
            //
            // line.find(L':') だと "HH:MM:SS" 内の ':' を拾ってしまうため、
            // ラベル終端の "):" を優先して探す。
            std::size_t valueStart = std::wstring::npos;

            const std::size_t labelEnd = line.find(L"):");
            if (labelEnd != std::wstring::npos) {
                valueStart = labelEnd + 2;
            }
            else {
                const std::size_t keyPos = line.find(L"Duration");
                if (keyPos != std::wstring::npos) {
                    const std::size_t colonPos = line.find(L':', keyPos + 8);
                    if (colonPos != std::wstring::npos) {
                        valueStart = colonPos + 1;
                    }
                }
            }

            if (valueStart == std::wstring::npos ||
                valueStart >= line.size()) {
                return false;
            }

            std::wstring s = TrimW_(line.substr(valueStart));

            // ", RunningStatus..." など後続情報を切る。
            const std::size_t commaPos = s.find(L',');
            if (commaPos != std::wstring::npos) {
                s = TrimW_(s.substr(0, commaPos));
            }

            int h = 0;
            int m = 0;
            int sec = 0;

            if (swscanf_s(s.c_str(), L"%d:%d:%d", &h, &m, &sec) != 3) {
                return false;
            }

            if (h < 0 || m < 0 || sec < 0 ||
                m > 59 || sec > 59) {
                return false;
            }

            outDurationSec =
                static_cast<std::uint32_t>(
                    h * 3600 + m * 60 + sec);

            return outDurationSec > 0;
        }

        bool AddSecondsToIsoDateTime_(
            const std::wstring& startDate,
            const std::wstring& startTime,
            std::uint32_t durationSec,
            std::wstring& outDate,
            std::wstring& outTime)
        {
            outDate.clear();
            outTime.clear();

            if (startDate.size() < 10 ||
                startTime.size() < 5 ||
                durationSec == 0) {
                return false;
            }

            int y = 0;
            int mo = 0;
            int dd = 0;
            int h = 0;
            int mi = 0;
            int ss = 0;

            if (swscanf_s(startDate.c_str(), L"%d-%d-%d", &y, &mo, &dd) != 3) {
                return false;
            }

            if (swscanf_s(startTime.c_str(), L"%d:%d:%d", &h, &mi, &ss) < 2) {
                return false;
            }

            std::tm tmv{};
            tmv.tm_year = y - 1900;
            tmv.tm_mon = mo - 1;
            tmv.tm_mday = dd;
            tmv.tm_hour = h;
            tmv.tm_min = mi;
            tmv.tm_sec = ss;
            tmv.tm_isdst = -1;

            std::time_t t = std::mktime(&tmv);
            if (t == static_cast<std::time_t>(-1)) {
                return false;
            }

            t += static_cast<std::time_t>(durationSec);

            if (localtime_s(&tmv, &t) != 0) {
                return false;
            }

            outDate = FormatIsoDate_(
                tmv.tm_year + 1900,
                tmv.tm_mon + 1,
                tmv.tm_mday);

            outTime = FormatIsoTime_(
                tmv.tm_hour,
                tmv.tm_min,
                tmv.tm_sec);

            return true;
        }

        bool ParseSpaceChannelFromEpgFileName_(
            const std::wstring& fileName,
            int& space,
            int& channel)
        {
            space = 0;
            channel = -1;

            const std::size_t spPos = fileName.find(L"_space");
            if (spPos == std::wstring::npos) {
                return false;
            }

            const std::size_t spStart = spPos + 6;
            std::size_t spEnd = spStart;

            while (spEnd < fileName.size() &&
                fileName[spEnd] >= L'0' &&
                fileName[spEnd] <= L'9') {
                ++spEnd;
            }

            const std::size_t chPos = fileName.find(L"_ch", spEnd);
            if (chPos == std::wstring::npos) {
                return false;
            }

            const std::size_t chStart = chPos + 3;
            std::size_t chEnd = chStart;

            while (chEnd < fileName.size() &&
                fileName[chEnd] >= L'0' &&
                fileName[chEnd] <= L'9') {
                ++chEnd;
            }

            if (spEnd <= spStart || chEnd <= chStart) {
                return false;
            }

            space = ToInt_(fileName.substr(spStart, spEnd - spStart));
            channel = ToInt_(fileName.substr(chStart, chEnd - chStart));

            return channel >= 0;
        }

        struct ParsedEpgEvent_
        {
            int fallbackSpace = 0;
            int fallbackChannel = -1;

            std::uint16_t networkId = 0;
            std::uint16_t tsId = 0;
            std::uint16_t serviceId = 0;
            std::uint16_t eventId = 0;

            std::wstring startDate;
            std::wstring startTime;
            std::wstring endDate;
            std::wstring endTime;
            std::uint32_t durationSec = 0;

            std::wstring title;
        };

        struct TimeShiftEpgSourceLoadStats_
        {
            std::size_t rawEvents = 0;
            std::size_t accepted = 0;

            std::size_t skipNoStartDate = 0;
            std::size_t skipNoStartTime = 0;
            std::size_t skipNoDuration = 0;
            std::size_t skipNoTitle = 0;
            std::size_t skipNoServiceId = 0;
            std::size_t skipEndTimeFailed = 0;
            std::size_t skipChannel = 0;

            std::size_t serviceMapHit = 0;
            std::size_t serviceMapMiss = 0;

            std::wstring firstAcceptedTitle;
            std::wstring firstSkippedTitle;
        };

        void AddParsedEpgEventToSources_(
            const ParsedEpgEvent_& ev,
            const std::unordered_map<std::wstring, TimeShiftServiceMapEntry_>& serviceMap,
            std::vector<TimeShift_ViewIndex::ProgramSourceEntry>& out,
            TimeShiftEpgSourceLoadStats_* stats)
        {
            if (stats != nullptr) {
                ++stats->rawEvents;
            }

            if (ev.startDate.empty()) {
                if (stats != nullptr) {
                    ++stats->skipNoStartDate;
                    if (stats->firstSkippedTitle.empty()) {
                        stats->firstSkippedTitle = ev.title;
                    }
                }
                return;
            }

            if (ev.startTime.empty()) {
                if (stats != nullptr) {
                    ++stats->skipNoStartTime;
                    if (stats->firstSkippedTitle.empty()) {
                        stats->firstSkippedTitle = ev.title;
                    }
                }
                return;
            }

            if (ev.durationSec == 0) {
                if (stats != nullptr) {
                    ++stats->skipNoDuration;
                    if (stats->firstSkippedTitle.empty()) {
                        stats->firstSkippedTitle = ev.title;
                    }
                }
                return;
            }

            if (ev.title.empty()) {
                if (stats != nullptr) {
                    ++stats->skipNoTitle;
                }
                return;
            }

            if (ev.serviceId == 0) {
                if (stats != nullptr) {
                    ++stats->skipNoServiceId;
                    if (stats->firstSkippedTitle.empty()) {
                        stats->firstSkippedTitle = ev.title;
                    }
                }
                return;
            }

            std::wstring endDate = ev.endDate;
            std::wstring endTime = ev.endTime;

            if (endDate.empty() || endTime.empty()) {
                if (!AddSecondsToIsoDateTime_(
                    ev.startDate,
                    ev.startTime,
                    ev.durationSec,
                    endDate,
                    endTime)) {
                    if (stats != nullptr) {
                        ++stats->skipEndTimeFailed;
                        if (stats->firstSkippedTitle.empty()) {
                            stats->firstSkippedTitle = ev.title;
                        }
                    }
                    return;
                }
            }

            int space = ev.fallbackSpace;
            int channel = ev.fallbackChannel;
            std::wstring serviceName;

            const std::wstring mapKey =
                MakeServiceMapKey_(ev.networkId, ev.tsId, ev.serviceId);

            const auto it = serviceMap.find(mapKey);
            if (it != serviceMap.end()) {
                space = it->second.space;
                channel = it->second.channel;
                serviceName = it->second.serviceName;

                if (stats != nullptr) {
                    ++stats->serviceMapHit;
                }
            }
            else {
                if (stats != nullptr) {
                    ++stats->serviceMapMiss;
                }
            }

            if (channel < 0) {
                if (stats != nullptr) {
                    ++stats->skipChannel;
                    if (stats->firstSkippedTitle.empty()) {
                        stats->firstSkippedTitle = ev.title;
                    }
                }
                return;
            }

            TimeShift_ViewIndex::ProgramSourceEntry src{};
            src.space = space;
            src.channel = channel;

            src.networkId = ev.networkId;
            src.tsId = ev.tsId;
            src.serviceId = ev.serviceId;
            src.eventId = ev.eventId;

            src.serviceName =
                !serviceName.empty()
                ? serviceName
                : (L"SID " + std::to_wstring(ev.serviceId));

            src.title = ev.title;
            src.startText = ev.startDate + L" " + ev.startTime;
            src.endText = endDate + L" " + endTime;

            out.push_back(std::move(src));

            if (stats != nullptr) {
                ++stats->accepted;

                if (stats->firstAcceptedTitle.empty()) {
                    stats->firstAcceptedTitle = ev.title;
                }
            }
        }

        void AppendProgramSourcesFromEpgText_(
            const std::wstring& text,
            int fallbackSpace,
            int fallbackChannel,
            const std::unordered_map<std::wstring, TimeShiftServiceMapEntry_>& serviceMap,
            std::vector<TimeShift_ViewIndex::ProgramSourceEntry>& out,
            TimeShiftEpgSourceLoadStats_* stats)
        {
            ParsedEpgEvent_ cur{};
            bool hasEvent = false;

            auto flush = [&]()
                {
                    if (!hasEvent) {
                        return;
                    }

                    AddParsedEpgEventToSources_(
                        cur,
                        serviceMap,
                        out,
                        stats);

                    cur = ParsedEpgEvent_{};
                    cur.fallbackSpace = fallbackSpace;
                    cur.fallbackChannel = fallbackChannel;
                    hasEvent = false;
                };

            cur.fallbackSpace = fallbackSpace;
            cur.fallbackChannel = fallbackChannel;

            std::wstringstream ss(text);
            std::wstring line;

            while (std::getline(ss, line)) {
                if (!line.empty() && line.back() == L'\r') {
                    line.pop_back();
                }

                const std::wstring trimmed = TrimW_(line);

                if (trimmed.empty()) {
                    continue;
                }

                if (trimmed.rfind(L"TableID:", 0) == 0) {
                    flush();

                    hasEvent = true;
                    cur = ParsedEpgEvent_{};
                    cur.fallbackSpace = fallbackSpace;
                    cur.fallbackChannel = fallbackChannel;

                    cur.networkId =
                        static_cast<std::uint16_t>(
                            FindNumberAny_(
                                trimmed,
                                { L"OriginalNetworkID", L"NetworkID", L"ONID" }));

                    cur.tsId =
                        static_cast<std::uint16_t>(
                            FindNumberAny_(
                                trimmed,
                                { L"TransportStreamID", L"TSID", L"TsId" }));

                    cur.serviceId =
                        static_cast<std::uint16_t>(
                            FindNumberAny_(
                                trimmed,
                                { L"ServiceID", L"ServiceId", L"SID" }));

                    cur.eventId =
                        static_cast<std::uint16_t>(
                            FindNumberAny_(
                                trimmed,
                                { L"EventID", L"EventId", L"EID" }));

                    continue;
                }

                if (!hasEvent) {
                    continue;
                }

                const int networkId =
                    FindNumberAny_(
                        trimmed,
                        { L"OriginalNetworkID", L"NetworkID", L"ONID" });
                if (networkId > 0) {
                    cur.networkId = static_cast<std::uint16_t>(networkId);
                }

                const int tsId =
                    FindNumberAny_(
                        trimmed,
                        { L"TransportStreamID", L"TSID", L"TsId" });
                if (tsId > 0) {
                    cur.tsId = static_cast<std::uint16_t>(tsId);
                }

                const int serviceId =
                    FindNumberAny_(
                        trimmed,
                        { L"ServiceID", L"ServiceId", L"SID" });
                if (serviceId > 0) {
                    cur.serviceId = static_cast<std::uint16_t>(serviceId);
                }

                const int eventId =
                    FindNumberAny_(
                        trimmed,
                        { L"EventID", L"EventId", L"EID" });
                if (eventId > 0) {
                    cur.eventId = static_cast<std::uint16_t>(eventId);
                }

                if (trimmed.rfind(L"StartTime(raw MJD+BCD40bit):", 0) == 0) {
                    std::wstring startDate;
                    std::wstring startTime;

                    if (ParseStartHexLine_(trimmed, startDate, startTime)) {
                        cur.startDate = startDate;
                        cur.startTime = startTime;
                    }

                    continue;
                }

                if (trimmed.rfind(L"Duration(HH:MM:SS):", 0) == 0) {
                    std::uint32_t durationSec = 0;

                    if (ParseDurationLine_(trimmed, durationSec)) {
                        cur.durationSec = durationSec;
                    }

                    continue;
                }

                if (trimmed.rfind(L"[短形式タイトル]", 0) == 0) {
                    cur.title =
                        ExtractAfterPrefixTrim_(trimmed, L"[短形式タイトル]");
                    continue;
                }

                if (trimmed.rfind(L"[タイトル]", 0) == 0) {
                    cur.title =
                        ExtractAfterPrefixTrim_(trimmed, L"[タイトル]");
                    continue;
                }

                if (trimmed.rfind(L"[イベント名]", 0) == 0) {
                    cur.title =
                        ExtractAfterPrefixTrim_(trimmed, L"[イベント名]");
                    continue;
                }
            }

            flush();
        }

        struct TimeShiftEpgParallelParseReport_
        {
            std::size_t blockCount = 0;
            std::size_t workerCount = 1;
            std::size_t failedWorkerCount = 0;
            bool usedParallel = false;

            std::wstring offsetRequested = L"SCALAR";
            std::wstring offsetActual = L"SCALAR";
            std::wstring offsetReason;
            bool offsetFallback = false;
            std::uint64_t offsetScanMs = 0;
        };

        void MergeTimeShiftEpgSourceLoadStats_(
            TimeShiftEpgSourceLoadStats_& dst,
            const TimeShiftEpgSourceLoadStats_& src)
        {
            dst.rawEvents += src.rawEvents;
            dst.accepted += src.accepted;

            dst.skipNoStartDate += src.skipNoStartDate;
            dst.skipNoStartTime += src.skipNoStartTime;
            dst.skipNoDuration += src.skipNoDuration;
            dst.skipNoTitle += src.skipNoTitle;
            dst.skipNoServiceId += src.skipNoServiceId;
            dst.skipEndTimeFailed += src.skipEndTimeFailed;
            dst.skipChannel += src.skipChannel;

            dst.serviceMapHit += src.serviceMapHit;
            dst.serviceMapMiss += src.serviceMapMiss;

            if (dst.firstAcceptedTitle.empty() &&
                !src.firstAcceptedTitle.empty()) {
                dst.firstAcceptedTitle = src.firstAcceptedTitle;
            }

            if (dst.firstSkippedTitle.empty() &&
                !src.firstSkippedTitle.empty()) {
                dst.firstSkippedTitle = src.firstSkippedTitle;
            }
        }

        bool IsEpgTableIdPrefixAt_(
            const std::wstring& text,
            std::size_t pos) noexcept
        {
            if (pos + 8u > text.size()) {
                return false;
            }

            return
                text[pos + 0u] == L'T' &&
                text[pos + 1u] == L'a' &&
                text[pos + 2u] == L'b' &&
                text[pos + 3u] == L'l' &&
                text[pos + 4u] == L'e' &&
                text[pos + 5u] == L'I' &&
                text[pos + 6u] == L'D' &&
                text[pos + 7u] == L':';
        }

        bool IsEpgTableIdLineHeadAt_(
            const std::wstring& text,
            std::size_t pos) noexcept
        {
            if (!IsEpgTableIdPrefixAt_(text, pos)) {
                return false;
            }

            return
                pos == 0u ||
                text[pos - 1u] == L'\n' ||
                text[pos - 1u] == L'\r';
        }

        std::vector<std::size_t> CollectEpgTableBlockOffsetsScalar_(
            const std::wstring& text)
        {
            std::vector<std::size_t> offsets;

            std::size_t pos = 0;

            for (;;) {
                const std::size_t found =
                    text.find(L"TableID:", pos);

                if (found == std::wstring::npos) {
                    break;
                }

                if (IsEpgTableIdLineHeadAt_(text, found)) {
                    offsets.push_back(found);
                }

                pos = found + 8u;
            }

            return offsets;
        }

        bool TryCollectEpgTableBlockOffsetsSse2_(
            const std::wstring& text,
            std::vector<std::size_t>& offsets) noexcept
        {
            return AppSimdSse2::CollectTimeShiftEpgTableBlockOffsets(
                text,
                offsets);
        }

        bool TryCollectEpgTableBlockOffsetsSse42_(
            const std::wstring& text,
            std::vector<std::size_t>& offsets) noexcept
        {
            return AppSimdSse42::CollectTimeShiftEpgTableBlockOffsets(
                text,
                offsets);
        }

        bool TryCollectEpgTableBlockOffsetsAvx2_(
            const std::wstring& text,
            std::vector<std::size_t>& offsets) noexcept
        {
            return AppSimdAvx2::CollectTimeShiftEpgTableBlockOffsets(
                text,
                offsets);
        }

        std::vector<std::size_t> CollectEpgTableBlockOffsets_(
            const std::wstring& text,
            TimeShiftEpgParallelParseReport_* report)
        {
            const auto t0 = std::chrono::steady_clock::now();

            const SIMDData::SelectedPath selected =
                SIMDData::SelectPathFromConfig();

            SIMDData::ExecPath actualPath = selected.actual;
            bool fallback = selected.fallback;
            std::wstring reason = selected.reason;

            std::vector<std::size_t> offsets;

            switch (selected.actual) {
            case SIMDData::ExecPath::Avx2:
                if (!TryCollectEpgTableBlockOffsetsAvx2_(
                    text,
                    offsets)) {
                    offsets =
                        CollectEpgTableBlockOffsetsScalar_(text);

                    actualPath = SIMDData::ExecPath::Scalar;
                    fallback = true;
                    reason = L"AVX2 kernel unavailable, fallback to SCALAR";
                }
                break;

            case SIMDData::ExecPath::Sse42:
                if (!TryCollectEpgTableBlockOffsetsSse42_(
                    text,
                    offsets)) {
                    if (!TryCollectEpgTableBlockOffsetsSse2_(
                        text,
                        offsets)) {
                        offsets =
                            CollectEpgTableBlockOffsetsScalar_(text);

                        actualPath = SIMDData::ExecPath::Scalar;
                        fallback = true;
                        reason = L"SSE4.2 kernel unavailable and SSE2 kernel unavailable, fallback to SCALAR";
                    }
                    else {
                        actualPath = SIMDData::ExecPath::Sse2;
                        fallback = true;
                        reason = L"SSE4.2 kernel unavailable, fallback to SSE2";
                    }
                }
                else {
                    actualPath = SIMDData::ExecPath::Sse42;

                    if (!reason.empty()) {
                        reason += L"; ";
                    }

                    reason += L"kernel=SSE4.2 cmpestri via AppSimdSse42";
                }
                break;

            case SIMDData::ExecPath::Sse2:
                if (!TryCollectEpgTableBlockOffsetsSse2_(
                    text,
                    offsets)) {
                    offsets =
                        CollectEpgTableBlockOffsetsScalar_(text);

                    actualPath = SIMDData::ExecPath::Scalar;
                    fallback = true;
                    reason = L"SSE2 kernel unavailable, fallback to SCALAR";
                }
                break;

            case SIMDData::ExecPath::Auto:
            case SIMDData::ExecPath::Scalar:
            default:
                offsets = CollectEpgTableBlockOffsetsScalar_(text);
                actualPath = SIMDData::ExecPath::Scalar;
                break;
            }

            const auto t1 = std::chrono::steady_clock::now();

            const auto elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    t1 - t0).count();

            if (report != nullptr) {
                report->offsetRequested =
                    SIMDData::ToString(selected.requested);

                report->offsetActual =
                    SIMDData::ToString(actualPath);

                report->offsetFallback = fallback;
                report->offsetReason = reason;

                report->offsetScanMs =
                    elapsedMs >= 0
                    ? static_cast<std::uint64_t>(elapsedMs)
                    : 0u;
            }

            return offsets;
        }

        std::size_t DecideTimeShiftEpgParseWorkerCount_(
            std::size_t blockCount) noexcept
        {
            if (blockCount < 512u) {
                return 1u;
            }

            unsigned int hc = std::thread::hardware_concurrency();

            if (hc < 2u) {
                hc = 2u;
            }

            std::size_t workers = static_cast<std::size_t>(hc);

            // EPG読込はI/Oと文字列処理が混ざるため、最初は最大4まで。
            // ここを増やしすぎると録画WriterやGUIに影響しやすい。
            if (workers > 4u) {
                workers = 4u;
            }

            if (workers > blockCount) {
                workers = blockCount;
            }

            return workers == 0u ? 1u : workers;
        }

        void AppendProgramSourcesFromEpgTextParallel_(
            const std::wstring& text,
            int fallbackSpace,
            int fallbackChannel,
            const std::unordered_map<std::wstring, TimeShiftServiceMapEntry_>& serviceMap,
            std::vector<TimeShift_ViewIndex::ProgramSourceEntry>& out,
            TimeShiftEpgSourceLoadStats_* stats,
            TimeShiftEpgParallelParseReport_* report)
        {
            if (report != nullptr) {
                *report = TimeShiftEpgParallelParseReport_{};
            }

            const std::vector<std::size_t> offsets =
                CollectEpgTableBlockOffsets_(text,report);

            if (report != nullptr) {
                report->blockCount = offsets.size();
            }

            const std::size_t workerCount =
                DecideTimeShiftEpgParseWorkerCount_(offsets.size());

            if (report != nullptr) {
                report->workerCount = workerCount;
                report->usedParallel = workerCount >= 2u;
            }

            if (offsets.empty() || workerCount <= 1u) {
                AppendProgramSourcesFromEpgText_(
                    text,
                    fallbackSpace,
                    fallbackChannel,
                    serviceMap,
                    out,
                    stats);
                return;
            }

            struct WorkerResult_
            {
                std::vector<TimeShift_ViewIndex::ProgramSourceEntry> sources;
                TimeShiftEpgSourceLoadStats_ stats;
                bool ok = true;
            };

            std::vector<WorkerResult_> results(workerCount);
            std::vector<std::thread> threads;
            threads.reserve(workerCount);

            const std::size_t blockCount = offsets.size();

            for (std::size_t worker = 0; worker < workerCount; ++worker) {
                const std::size_t beginBlock =
                    blockCount * worker / workerCount;

                const std::size_t endBlock =
                    blockCount * (worker + 1u) / workerCount;

                threads.emplace_back(
                    [&, worker, beginBlock, endBlock]()
                    {
                        try {
                            if (beginBlock >= endBlock ||
                                beginBlock >= offsets.size()) {
                                return;
                            }

                            const std::size_t beginPos =
                                offsets[beginBlock];

                            const std::size_t endPos =
                                endBlock < offsets.size()
                                ? offsets[endBlock]
                                : text.size();

                            if (endPos <= beginPos) {
                                return;
                            }

                            const std::wstring chunk =
                                text.substr(beginPos, endPos - beginPos);

                            AppendProgramSourcesFromEpgText_(
                                chunk,
                                fallbackSpace,
                                fallbackChannel,
                                serviceMap,
                                results[worker].sources,
                                &results[worker].stats);
                        }
                        catch (...) {
                            results[worker].ok = false;
                        }
                    });
            }

            for (auto& th : threads) {
                if (th.joinable()) {
                    th.join();
                }
            }

            std::size_t failedWorkers = 0;

            for (const auto& r : results) {
                if (!r.ok) {
                    ++failedWorkers;
                }
            }

            if (report != nullptr) {
                report->failedWorkerCount = failedWorkers;
            }

            if (failedWorkers != 0u) {
                // 並列解析中に例外が出た場合、一部イベント欠落のまま成功扱いにしない。
                // 安全側として従来の単一スレッド解析へフォールバックする。
                AppendProgramSourcesFromEpgText_(
                    text,
                    fallbackSpace,
                    fallbackChannel,
                    serviceMap,
                    out,
                    stats);
                return;
            }

            std::size_t totalSources = 0;
            for (const auto& r : results) {
                totalSources += r.sources.size();
            }

            out.reserve(out.size() + totalSources);

            TimeShiftEpgSourceLoadStats_ merged{};

            for (auto& r : results) {
                MergeTimeShiftEpgSourceLoadStats_(
                    merged,
                    r.stats);

                for (auto& src : r.sources) {
                    out.push_back(std::move(src));
                }
            }

            if (stats != nullptr) {
                *stats = merged;
            }
        }

        void ProbeTimeShiftEpgSimdConvert_(
            const std::wstring& text,
            const std::filesystem::path& epgPath) noexcept
        {
            if (text.empty()) {
                return;
            }

            const SIMDData::SelectedPath selected =
                SIMDData::SelectPathFromConfig();

            SIMDData::EpgConvertOptions options{};
            options.keepTitleText = true;
            options.keepDescriptionText = true;
            options.keepGenre = true;

            SIMDData::EpgColumnStore store{};
            SIMDData::EpgConvertResult conv{};

            const bool ok =
                SIMDData::ConvertEpgDumpTextToColumnStore(
                    text,
                    options,
                    store,
                    conv);

            std::wstring fileName;

            try {
                fileName = epgPath.filename().wstring();
            }
            catch (...) {
                fileName = L"(unknown)";
            }

            const bool sizeOk =
                store.size() == conv.acceptedEventCount;

            DebugLog(
                L"[TimeShift][EPG-SIMD-CONVERT] file=%ls requested=%ls actual=%ls fallback=%d ok=%d resultPath=%ls lines=%zu headers=%zu accepted=%zu skipped=%zu titles=%zu desc=%zu genres=%zu store=%zu sizeOk=%d reason=%ls message=%ls",
                fileName.c_str(),
                SIMDData::ToString(selected.requested),
                SIMDData::ToString(selected.actual),
                selected.fallback ? 1 : 0,
                ok ? 1 : 0,
                SIMDData::ToString(conv.path),
                conv.lineCount,
                conv.eventHeaderCount,
                conv.acceptedEventCount,
                conv.skippedEventCount,
                conv.titleCount,
                conv.descriptionCount,
                conv.genreCount,
                store.size(),
                sizeOk ? 1 : 0,
                selected.reason.c_str(),
                conv.message.c_str());
        }

        std::vector<TimeShift_ViewIndex::ProgramSourceEntry>
            LoadProgramSourcesFromTargetEpgFile_(
                const std::filesystem::path& epgPath,
                const TimeShiftTargetChannel_& target,
                TimeShiftGentleThrottle_* throttle,
                TimeShiftRebuildResult_* result)
        {
            std::vector<TimeShift_ViewIndex::ProgramSourceEntry> out;

            if (!target.valid) {
                return out;
            }

            std::error_code ec;

            if (epgPath.empty() ||
                !std::filesystem::exists(epgPath, ec) ||
                !std::filesystem::is_regular_file(epgPath, ec)) {
                return out;
            }

            if (throttle != nullptr) {
                throttle->PhaseBreak();
            }

            {
                const TimeShiftEpgReadSafetyProbe_ safety =
                    ProbeTimeShiftEpgReadSafety_(epgPath);

                if (!safety.safeToRead) {
                    if (result != nullptr) {
                        result->epgReadDeferredBySafeGuard = true;
                        result->epgReadSafeReason = safety.reason;
                        result->epgReadSafeAgeMs = safety.lastWriteAgeMs;
                        result->epgReadSizeBefore = safety.sizeBefore;
                        result->epgReadSizeAfter = safety.sizeAfter;
                    }

                    DebugLog(
                        L"[TimeShift][EPG-SAFE] defer worker EPG read reason=%ls ageMs=%llu sizeBefore=%llu sizeAfter=%llu path=%ls",
                        safety.reason.c_str(),
                        static_cast<unsigned long long>(safety.lastWriteAgeMs),
                        static_cast<unsigned long long>(safety.sizeBefore),
                        static_cast<unsigned long long>(safety.sizeAfter),
                        epgPath.wstring().c_str());

                    return out;
                }
            }

            const std::wstring text =
                ReadUtf8FileToWide_(epgPath);

            if (text.empty()) {
                return out;
            }

            ProbeTimeShiftEpgSimdConvert_(
                text,
                epgPath);

            

            ProbeEpgTextForTimeShift_(
                text,
                result);

            const auto serviceMap =
                LoadTimeShiftServiceMapFromConfig_(target.space);

            std::vector<TimeShift_ViewIndex::ProgramSourceEntry> raw;

            TimeShiftEpgSourceLoadStats_ loadStats{};

            TimeShiftEpgParallelParseReport_ parseReport{};

            AppendProgramSourcesFromEpgTextParallel_(
                text,
                target.space,
                target.channel,
                serviceMap,
                raw,
                &loadStats,
                &parseReport);

            if (result != nullptr) {
                result->epgRawEvents = loadStats.rawEvents;
                result->epgAcceptedEvents = loadStats.accepted;
                result->epgSkipNoStartDate = loadStats.skipNoStartDate;
                result->epgSkipNoStartTime = loadStats.skipNoStartTime;
                result->epgSkipNoDuration = loadStats.skipNoDuration;
                result->epgSkipNoTitle = loadStats.skipNoTitle;
                result->epgSkipNoServiceId = loadStats.skipNoServiceId;
                result->epgSkipEndTimeFailed = loadStats.skipEndTimeFailed;
                result->epgSkipChannel = loadStats.skipChannel;
                result->epgServiceMapHit = loadStats.serviceMapHit;
                result->epgServiceMapMiss = loadStats.serviceMapMiss;
                result->epgFirstAcceptedTitle = loadStats.firstAcceptedTitle;
                result->epgFirstSkippedTitle = loadStats.firstSkippedTitle;
            }

            DebugLog(
                L"[TimeShift][EPG-LOAD] raw=%zu accepted=%zu blocks=%zu workers=%zu parallel=%d failedWorkers=%zu offsetRequested=%ls offsetActual=%ls offsetFallback=%d offsetScanMs=%llu offsetReason=%ls skipStartDate=%zu skipStartTime=%zu skipDuration=%zu skipTitle=%zu skipServiceId=%zu skipEndTime=%zu skipChannel=%zu serviceMapHit=%zu serviceMapMiss=%zu firstAccepted=\"%ls\" firstSkipped=\"%ls\"",
                loadStats.rawEvents,
                loadStats.accepted,
                parseReport.blockCount,
                parseReport.workerCount,
                parseReport.usedParallel ? 1 : 0,
                parseReport.failedWorkerCount,
                parseReport.offsetRequested.c_str(),
                parseReport.offsetActual.c_str(),
                parseReport.offsetFallback ? 1 : 0,
                static_cast<unsigned long long>(parseReport.offsetScanMs),
                parseReport.offsetReason.c_str(),
                loadStats.skipNoStartDate,
                loadStats.skipNoStartTime,
                loadStats.skipNoDuration,
                loadStats.skipNoTitle,
                loadStats.skipNoServiceId,
                loadStats.skipEndTimeFailed,
                loadStats.skipChannel,
                loadStats.serviceMapHit,
                loadStats.serviceMapMiss,
                loadStats.firstAcceptedTitle.c_str(),
                loadStats.firstSkippedTitle.c_str());

            if (result != nullptr) {
                result->rawSourceCount = raw.size();

                if (!raw.empty()) {
                    const auto& r = raw.front();

                    result->firstRawSource =
                        L"space=" + std::to_wstring(r.space) +
                        L" ch=" + std::to_wstring(r.channel) +
                        L" sid=" + std::to_wstring(r.serviceId) +
                        L" nid=" + std::to_wstring(r.networkId) +
                        L" tsid=" + std::to_wstring(r.tsId) +
                        L" eid=" + std::to_wstring(r.eventId) +
                        L" start=" + r.startText +
                        L" end=" + r.endText +
                        L" title=" + r.title;
                }
            }

            if (throttle != nullptr) {
                throttle->PhaseBreak();
            }

            out.reserve(raw.size());

            for (auto& s : raw) {
                if (target.serviceId != 0 &&
                    s.serviceId != target.serviceId) {
                    if (result != nullptr) {
                        ++result->filteredByServiceId;
                    }
                    continue;
                }

                if (target.networkId != 0 &&
                    s.networkId != target.networkId) {
                    if (result != nullptr) {
                        ++result->filteredByNetworkId;
                    }
                    continue;
                }

                if (target.tsId != 0 &&
                    s.tsId != target.tsId) {
                    if (result != nullptr) {
                        ++result->filteredByTsId;
                    }
                    continue;
                }

                s.space = target.space;
                s.channel = target.channel;

                if (!target.name.empty()) {
                    s.serviceName = target.name;
                }

                if (result != nullptr && result->firstAcceptedSource.empty()) {
                    result->firstAcceptedSource =
                        L"space=" + std::to_wstring(s.space) +
                        L" ch=" + std::to_wstring(s.channel) +
                        L" sid=" + std::to_wstring(s.serviceId) +
                        L" nid=" + std::to_wstring(s.networkId) +
                        L" tsid=" + std::to_wstring(s.tsId) +
                        L" eid=" + std::to_wstring(s.eventId) +
                        L" start=" + s.startText +
                        L" end=" + s.endText +
                        L" title=" + s.title;
                }

                out.push_back(std::move(s));

                if (throttle != nullptr) {
                    throttle->Tick();
                }
            }

            std::sort(
                out.begin(),
                out.end(),
                [](const auto& a, const auto& b)
                {
                    if (a.startText != b.startText) {
                        return a.startText < b.startText;
                    }

                    if (a.endText != b.endText) {
                        return a.endText < b.endText;
                    }

                    if (a.eventId != b.eventId) {
                        return a.eventId < b.eventId;
                    }

                    return a.title < b.title;
                });

            out.erase(
                std::unique(
                    out.begin(),
                    out.end(),
                    [](const auto& a, const auto& b)
                    {
                        return
                            a.space == b.space &&
                            a.channel == b.channel &&
                            a.networkId == b.networkId &&
                            a.tsId == b.tsId &&
                            a.serviceId == b.serviceId &&
                            a.eventId == b.eventId &&
                            a.startText == b.startText &&
                            a.endText == b.endText;
                    }),
                out.end());

            return out;
        }

        std::vector<TimeShift_ViewIndex::ProgramSourceEntry>
            LoadProgramSourcesFromEpgOutCore_(
                const std::filesystem::path& epgOutFolder,
                TimeShiftGentleThrottle_* throttle = nullptr)
        {
            std::vector<TimeShift_ViewIndex::ProgramSourceEntry> out;

            std::error_code ec;

            if (epgOutFolder.empty() ||
                !std::filesystem::exists(epgOutFolder, ec) ||
                !std::filesystem::is_directory(epgOutFolder, ec)) {
                return out;
            }

            constexpr int kSpace = 0;

            const auto serviceMap =
                LoadTimeShiftServiceMapFromConfig_(kSpace);

            if (throttle != nullptr) {
                throttle->PhaseBreak();
            }

            for (std::filesystem::directory_iterator it(epgOutFolder, ec), end;
                !ec && it != end;
                it.increment(ec)) {

                std::error_code fileEc;
                if (!it->is_regular_file(fileEc)) {
                    if (throttle != nullptr) {
                        throttle->Tick();
                    }
                    continue;
                }

                const std::filesystem::path path = it->path();

                const std::wstring ext = path.extension().wstring();
                if (ext != L".epg" && ext != L".EPG") {
                    if (throttle != nullptr) {
                        throttle->Tick();
                    }
                    continue;
                }

                const std::wstring fileName = path.filename().wstring();

                int fallbackSpace = 0;
                int fallbackChannel = -1;

                ParseSpaceChannelFromEpgFileName_(
                    fileName,
                    fallbackSpace,
                    fallbackChannel);

                const std::wstring text =
                    ReadUtf8FileToWide_(path);

                if (text.empty()) {
                    if (throttle != nullptr) {
                        throttle->PhaseBreak();
                    }
                    continue;
                }

                AppendProgramSourcesFromEpgText_(
                    text,
                    fallbackSpace,
                    fallbackChannel,
                    serviceMap,
                    out,
                    nullptr);

                if (throttle != nullptr) {
                    throttle->PhaseBreak();
                }
            }

            std::sort(
                out.begin(),
                out.end(),
                [](const auto& a, const auto& b)
                {
                    if (a.space != b.space) {
                        return a.space < b.space;
                    }

                    if (a.channel != b.channel) {
                        return a.channel < b.channel;
                    }

                    if (a.startText != b.startText) {
                        return a.startText < b.startText;
                    }

                    if (a.serviceId != b.serviceId) {
                        return a.serviceId < b.serviceId;
                    }

                    return a.eventId < b.eventId;
                });

            if (throttle != nullptr) {
                throttle->PhaseBreak();
            }

            out.erase(
                std::unique(
                    out.begin(),
                    out.end(),
                    [](const auto& a, const auto& b)
                    {
                        return
                            a.space == b.space &&
                            a.channel == b.channel &&
                            a.networkId == b.networkId &&
                            a.tsId == b.tsId &&
                            a.serviceId == b.serviceId &&
                            a.eventId == b.eventId &&
                            a.startText == b.startText &&
                            a.endText == b.endText;
                    }),
                out.end());

            if (throttle != nullptr) {
                throttle->PhaseBreak();
            }

            return out;
        }


}// namespace

HMENU TimeShiftViewerTab::CtrlId_(int id) noexcept
{
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

std::wstring TimeShiftViewerTab::MakeChannelDisplayName_(
    int space,
    int channel)
{
    std::wstring name = L"地デジ ";
    name += L"ch";
    name += std::to_wstring(channel);

    if (space != 0) {
        name = L"space";
        name += std::to_wstring(space);
        name += L" ch";
        name += std::to_wstring(channel);
    }

    return name;
}

std::wstring TimeShiftViewerTab::MakeUserStatusText_(
    const std::wstring& rawStatus)
{
    if (rawStatus == L"Recording") {
        return L"保存中";
    }
    if (rawStatus == L"Active") {
        return L"保存済み";
    }
    if (rawStatus == L"Empty") {
        return L"保存なし";
    }

    return rawStatus;
}

std::wstring TimeShiftViewerTab::MakeRecordingRangeText_(
    const TimeShift_ViewIndex::ChannelEntry& ch)
{
    if (ch.firstStartText.empty() && ch.lastStartText.empty()) {
        return L"-";
    }

    if (ch.firstStartText == ch.lastStartText) {
        return ch.firstStartText;
    }

    std::wstring text = ch.firstStartText;
    text += L" ～ ";
    text += ch.lastStartText;
    return text;
}

bool TimeShiftViewerTab::Create(
    HWND hParent,
    const RECT& rc,
    HINSTANCE hInst,
    HFONT hFont,
    std::function<void(std::wstring_view)>&& log,
    AppWorker::AppWorkerPool* workerPool)
{
    hInst_ = hInst;
    hFont_ = hFont;
    log_ = std::move(log);
    workerPool_ = workerPool;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TimeShiftViewerTab::WndProc;
    wc.hInstance = hInst_;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);

    RegisterClassExW(&wc);

    hWnd_ = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kClassName,
        L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        rc.left,
        rc.top,
        rc.right - rc.left,
        rc.bottom - rc.top,
        hParent,
        nullptr,
        hInst_,
        this);

    if (hWnd_ == nullptr) {
        return false;
    }

    return true;
}

void TimeShiftViewerTab::SetWorkerPool(
    AppWorker::AppWorkerPool* workerPool) noexcept
{
    workerPool_ = workerPool;
}

void TimeShiftViewerTab::Show(bool show)
{
    if (hWnd_ != nullptr) {
        ShowWindow(hWnd_, show ? SW_SHOW : SW_HIDE);
    }
}

void TimeShiftViewerTab::Move(const RECT& rc)
{
    if (hWnd_ != nullptr) {
        MoveWindow(
            hWnd_,
            rc.left,
            rc.top,
            rc.right - rc.left,
            rc.bottom - rc.top,
            TRUE);
    }
}

void TimeShiftViewerTab::ApplyToConfig()
{
}

void TimeShiftViewerTab::ReloadFromConfig()
{
    OnReload();
}

LRESULT CALLBACK TimeShiftViewerTab::WndProc(
    HWND hWnd,
    UINT msg,
    WPARAM wp,
    LPARAM lp)
{
    TimeShiftViewerTab* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<TimeShiftViewerTab*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hWnd_ = hWnd;
    }
    else {
        self = reinterpret_cast<TimeShiftViewerTab*>(
            GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->HandleMessage(msg, wp, lp);
    }

    return DefWindowProcW(hWnd, msg, wp, lp);
}

LRESULT TimeShiftViewerTab::HandleMessage(
    UINT msg,
    WPARAM wp,
    LPARAM lp)
{
    if (msg == WM_TS_REBUILD_DONE) {
        OnRebuildProgramTableDone_(lp);
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        OnCreate();
        return 0;

    case WM_SIZE:
        OnSize(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_TIMER:
        if (wp == IDT_TS_AUTO_REBUILD) {
            OnAutoRebuildTimer_();
            return 0;
        }
        break;

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_TS_BTN_RELOAD) {
            OnReload();
            return 0;
        }

        

        if (LOWORD(wp) == IDC_TS_BTN_OPEN_PROGRAM_WINDOW) {
            OnProgramWindowSwitch_();
            return 0;
        }

        break;

    case WM_DESTROY:
        StopAutoRebuildTimer_();
        programListWindow_.Destroy();
        return 0;

    case WM_NOTIFY:
    {
        const auto* nm = reinterpret_cast<const NMHDR*>(lp);
        if (nm == nullptr) {
            break;
        }

        if (nm->hwndFrom == hSubTab_ && nm->code == TCN_SELCHANGE) {
            OnSubTabChanged();
            return 0;
        }


        if (nm->hwndFrom == hListChannels_ &&
            nm->code == LVN_ITEMCHANGED) {
            OnChannelChanged();
            return 0;
        }

        break;
    }

    default:
        break;
    }

    return DefWindowProcW(hWnd_, msg, wp, lp);
}

void TimeShiftViewerTab::OnCreate()
{
    hLblRoot_ = CreateWindowExW(
        0,
        L"STATIC",
        L"TimeShift",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        10,
        8,
        600,
        24,
        hWnd_,
        nullptr,
        hInst_,
        nullptr);

    hBtnReload_ = CreateWindowExW(
        0,
        L"BUTTON",
        L"更新",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        10,
        36,
        80,
        28,
        hWnd_,
        CtrlId_(IDC_TS_BTN_RELOAD),
        hInst_,
        nullptr);

    

    hBtnOpenProgramWindow_ = CreateWindowExW(
        0,
        L"BUTTON",
        L"一覧窓を表示/前面へ",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        100,
        36,
        160,
        28,
        hWnd_,
        CtrlId_(IDC_TS_BTN_OPEN_PROGRAM_WINDOW),
        hInst_,
        nullptr);

    SetFontIf_(hLblRoot_, hFont_);
    SetFontIf_(hBtnReload_, hFont_);
    SetFontIf_(hBtnOpenProgramWindow_, hFont_);

    hSubTab_ = CreateWindowExW(
        0,
        WC_TABCONTROLW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        10,
        72,
        900,
        420,
        hWnd_,
        CtrlId_(IDC_TS_SUBTAB),
        hInst_,
        nullptr);

    SetFontIf_(hSubTab_, hFont_);

    CreateSubTabs_();

    OnReload();

    // OnReloadで再構築が走らなかった場合でも、
    // 少し後に自動チェックする。
    StartAutoRebuildTimer_(kTimeShiftAutoRebuildInitialDelayMs);
}

void TimeShiftViewerTab::CreateSubTabs_()
{
    if (hSubTab_ == nullptr) {
        return;
    }

    TCITEMW item{};
    item.mask = TCIF_TEXT;

    item.pszText = const_cast<LPWSTR>(L"過去番組表");
    TabCtrl_InsertItem(hSubTab_, 0, &item);

    item.pszText = const_cast<LPWSTR>(L"保存状況");
    TabCtrl_InsertItem(hSubTab_, 1, &item);

    item.pszText = const_cast<LPWSTR>(L"詳細");
    TabCtrl_InsertItem(hSubTab_, 2, &item);

    item.pszText = const_cast<LPWSTR>(L"診断");
    TabCtrl_InsertItem(hSubTab_, 3, &item);

    hPageProgram_ = CreatePage_(hSubTab_, hInst_);
    hPageStatus_ = CreatePage_(hSubTab_, hInst_);
    hPageDetail_ = CreatePage_(hSubTab_, hInst_);
    hPageDiagnostic_ = CreatePage_(hSubTab_, hInst_);

    // ------------------------------------------------------------
// 過去番組表 : 別窓へ移動
// ------------------------------------------------------------
    hProgramNotice_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"タイムシフト番組一覧は別窓に移動しました。\r\n"
        L"上部の「一覧窓を表示/前面へ」を押すと、別窓を表示または前面復帰します。\r\n"
        L"番組一覧の再構築は、保存セグメントと .epg をもとに自動で行います。\r\n"
        L"別窓側で表示フィルタ、選択番組コピーを操作できます。",
        WS_CHILD | WS_VISIBLE |
        ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        0,
        0,
        100,
        100,
        hPageProgram_,
        nullptr,
        hInst_,
        nullptr);

    SetFontIf_(hProgramNotice_, hFont_);

    

    // ------------------------------------------------------------
    // 保存状況 : 管理向け
    // ------------------------------------------------------------
    hListStatus_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0,
        0,
        100,
        100,
        hPageStatus_,
        CtrlId_(IDC_TS_LIST_STATUS),
        hInst_,
        nullptr);

    SetFontIf_(hListStatus_, hFont_);
    InitListView_(hListStatus_);

    AddColumn_(hListStatus_, 0, L"チャンネル", 120);
    AddColumn_(hListStatus_, 1, L"容量", 100);
    AddColumn_(hListStatus_, 2, L"セグメント数", 100);
    AddColumn_(hListStatus_, 3, L"保存開始", 160);
    AddColumn_(hListStatus_, 4, L"最終更新", 160);
    AddColumn_(hListStatus_, 5, L"状態", 100);

    // ------------------------------------------------------------
    // 詳細 : 開発者向け
    // ------------------------------------------------------------
    hListChannels_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0,
        0,
        280,
        300,
        hPageDetail_,
        CtrlId_(IDC_TS_LIST_CHANNELS),
        hInst_,
        nullptr);

    hListSegments_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        290,
        0,
        600,
        300,
        hPageDetail_,
        CtrlId_(IDC_TS_LIST_SEGMENTS),
        hInst_,
        nullptr);

    hInfo_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        0,
        310,
        890,
        100,
        hPageDetail_,
        nullptr,
        hInst_,
        nullptr);

    SetFontIf_(hListChannels_, hFont_);
    SetFontIf_(hListSegments_, hFont_);
    SetFontIf_(hInfo_, hFont_);

    InitListView_(hListChannels_);
    InitListView_(hListSegments_);

    AddColumn_(hListChannels_, 0, L"チャンネル", 90);
    AddColumn_(hListChannels_, 1, L"容量", 90);
    AddColumn_(hListChannels_, 2, L"状態", 80);

    AddColumn_(hListSegments_, 0, L"開始", 150);
    AddColumn_(hListSegments_, 1, L"Seq", 70);
    AddColumn_(hListSegments_, 2, L"容量", 90);
    AddColumn_(hListSegments_, 3, L"状態", 90);
    AddColumn_(hListSegments_, 4, L"ファイル", 340);

    // ------------------------------------------------------------
    // 診断
    // ------------------------------------------------------------
    hDiag_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        0,
        0,
        100,
        100,
        hPageDiagnostic_,
        nullptr,
        hInst_,
        nullptr);

    SetFontIf_(hDiag_, hFont_);

    ShowSubPage_(SubPage::Program);
}

void TimeShiftViewerTab::OnSize(int w, int h)
{
    const int pad = 10;
    const int topH = 66;

    if (hLblRoot_ != nullptr) {
        MoveWindow(hLblRoot_, pad, 8, MaxInt_(0, w - pad * 2), 22, TRUE);
    }

    if (hBtnReload_ != nullptr) {
        MoveWindow(hBtnReload_, pad, 36, 80, 28, TRUE);
    }

    if (hBtnOpenProgramWindow_ != nullptr) {
        MoveWindow(hBtnOpenProgramWindow_, pad + 90, 36, 160, 28, TRUE);
    }
    

    const int tabY = topH + pad;
    const int tabH = MaxInt_(80, h - tabY - pad);

    if (hSubTab_ != nullptr) {
        MoveWindow(
            hSubTab_,
            pad,
            tabY,
            MaxInt_(100, w - pad * 2),
            tabH,
            TRUE);
    }

    RECT rcPage = GetSubPageRect_();

    if (hPageProgram_ != nullptr) {
        MoveWindow(
            hPageProgram_,
            rcPage.left,
            rcPage.top,
            rcPage.right - rcPage.left,
            rcPage.bottom - rcPage.top,
            TRUE);
    }

    if (hPageStatus_ != nullptr) {
        MoveWindow(
            hPageStatus_,
            rcPage.left,
            rcPage.top,
            rcPage.right - rcPage.left,
            rcPage.bottom - rcPage.top,
            TRUE);
    }

    if (hPageDetail_ != nullptr) {
        MoveWindow(
            hPageDetail_,
            rcPage.left,
            rcPage.top,
            rcPage.right - rcPage.left,
            rcPage.bottom - rcPage.top,
            TRUE);
    }

    if (hPageDiagnostic_ != nullptr) {
        MoveWindow(
            hPageDiagnostic_,
            rcPage.left,
            rcPage.top,
            rcPage.right - rcPage.left,
            rcPage.bottom - rcPage.top,
            TRUE);
    }

    const int pageW = RectWidth_(rcPage);
    const int pageH = RectHeight_(rcPage);

    if (hProgramNotice_ != nullptr) {
        MoveWindow(hProgramNotice_, 0, 0, pageW, pageH, TRUE);
    }

    if (hListStatus_ != nullptr) {
        MoveWindow(hListStatus_, 0, 0, pageW, pageH, TRUE);
    }

    if (hPageDetail_ != nullptr) {
        const int infoH = 110;
        const int detailListH = MaxInt_(80, pageH - infoH - pad);
        const int chW = 300;
        const int segX = chW + pad;
        const int segW = MaxInt_(100, pageW - segX);

        if (hListChannels_ != nullptr) {
            MoveWindow(hListChannels_, 0, 0, chW, detailListH, TRUE);
        }

        if (hListSegments_ != nullptr) {
            MoveWindow(hListSegments_, segX, 0, segW, detailListH, TRUE);
        }

        if (hInfo_ != nullptr) {
            MoveWindow(hInfo_, 0, detailListH + pad, pageW, infoH, TRUE);
        }
    }

    if (hDiag_ != nullptr) {
        MoveWindow(hDiag_, 0, 0, pageW, pageH, TRUE);
    }
}

RECT TimeShiftViewerTab::GetSubPageRect_() const
{
    RECT rc{};
    if (hSubTab_ == nullptr) {
        return rc;
    }

    GetClientRect(hSubTab_, &rc);
    TabCtrl_AdjustRect(hSubTab_, FALSE, &rc);

    return rc;
}

void TimeShiftViewerTab::OnSubTabChanged()
{
    if (hSubTab_ == nullptr) {
        currentPage_ = SubPage::Program;
        ShowSubPage_(currentPage_);
        RefreshCurrentSubPage_();
        return;
    }

    const int sel = TabCtrl_GetCurSel(hSubTab_);
    if (sel < 0 || sel >= static_cast<int>(SubPage::Count)) {
        currentPage_ = SubPage::Program;
    }
    else {
        currentPage_ = static_cast<SubPage>(sel);
    }

    ShowSubPage_(currentPage_);
    RefreshCurrentSubPage_();
}

void TimeShiftViewerTab::ShowSubPage_(SubPage page)
{
    currentPage_ = page;

    const auto show = [page](HWND h, SubPage target)
        {
            if (h != nullptr) {
                ShowWindow(h, page == target ? SW_SHOW : SW_HIDE);
            }
        };

    show(hPageProgram_, SubPage::Program);
    show(hPageStatus_, SubPage::Status);
    show(hPageDetail_, SubPage::Detail);
    show(hPageDiagnostic_, SubPage::Diagnostic);

    if (hSubTab_ != nullptr) {
        TabCtrl_SetCurSel(hSubTab_, static_cast<int>(page));
    }

    const bool isProgramPage = (page == SubPage::Program);

    

   

    if (hBtnOpenProgramWindow_ != nullptr) {
        ShowWindow(hBtnOpenProgramWindow_, isProgramPage ? SW_SHOW : SW_HIDE);
    }

    
}

void TimeShiftViewerTab::OnReload()
{
    if (log_) {
        log_(L"[TimeShiftViewer] OnReload enter");
    }

    if (rebuildBusy_) {
        SetInfo_(L"タイムシフト番組表を再構築中のため、更新は後で実行してください。");

        if (log_) {
            log_(L"[TimeShiftViewer] OnReload skipped: rebuild busy");
        }

        return;
    }

    auto& cfg = ConfigManager::GetInstance();

    const std::filesystem::path root =
        cfg.GetTimeShiftRootFolder();

    index_ = TimeShift_ViewIndex::Build(root);
    selectedChannel_ = -1;

    std::wstring label = L"TimeShift root: ";
    label += root.wstring();

    if (hLblRoot_ != nullptr) {
        SetWindowTextW(hLblRoot_, label.c_str());
    }

    RefreshCurrentSubPage_();

    if (log_) {
        std::wstring msg = L"[TimeShiftViewer] reload channels=";
        msg += std::to_wstring(index_.channels.size());
        msg += L" segments=";
        msg += std::to_wstring(index_.segments.size());
        msg += L" programs=";
        msg += std::to_wstring(index_.programs.size());
        log_(msg);
    }

    if (programListWindow_.IsCreated()) {
        SyncProgramListWindow_();

        if (programListWindow_.IsVisible()) {
            programListWindow_.ShowOrActivate(false);
        }
    }

    // 重要:
    // 別窓が未作成でも、TimeShift保存データ + チューナー指定EPGから
    // 裏で番組一覧キャッシュを構築する。
    RequestProgramTableRebuildIfNeeded_();
}

void TimeShiftViewerTab::OnChannelChanged()
{
    const int sel = ListView_GetNextItem(
        hListChannels_,
        -1,
        LVNI_SELECTED);

    if (sel == selectedChannel_) {
        return;
    }

    selectedChannel_ = sel;
    FillSegments_();
}



void TimeShiftViewerTab::FillStatusView_()
{
    if (hListStatus_ == nullptr) {
        return;
    }

    ListView_DeleteAllItems(hListStatus_);

    int row = 0;

    for (const auto& ch : index_.channels) {
        const std::wstring channelName =
            MakeChannelDisplayName_(ch.space, ch.channel);

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(channelName.c_str());

        ListView_InsertItem(hListStatus_, &item);

        SetSubItemText_(
            hListStatus_,
            row,
            1,
            TimeShift_ViewIndex::FormatBytes(ch.totalBytes));

        SetSubItemText_(
            hListStatus_,
            row,
            2,
            std::to_wstring(ch.segmentCount));

        SetSubItemText_(hListStatus_, row, 3, ch.firstStartText);
        SetSubItemText_(hListStatus_, row, 4, ch.lastStartText);
        SetSubItemText_(hListStatus_, row, 5, MakeUserStatusText_(ch.status));

        ++row;
    }
}

void TimeShiftViewerTab::FillChannels_()
{
    if (hListChannels_ == nullptr) {
        return;
    }

    ListView_DeleteAllItems(hListChannels_);

    for (int i = 0; i < static_cast<int>(index_.channels.size()); ++i) {
        const auto& ch = index_.channels[static_cast<std::size_t>(i)];

        const std::wstring name =
            MakeChannelDisplayName_(ch.space, ch.channel);

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(name.c_str());

        ListView_InsertItem(hListChannels_, &item);

        SetSubItemText_(
            hListChannels_,
            i,
            1,
            TimeShift_ViewIndex::FormatBytes(ch.totalBytes));

        SetSubItemText_(hListChannels_, i, 2, ch.status);
    }

    if (!index_.channels.empty()) {
        ListView_SetItemState(
            hListChannels_,
            0,
            LVIS_SELECTED | LVIS_FOCUSED,
            LVIS_SELECTED | LVIS_FOCUSED);

        selectedChannel_ = 0;
    }
}

void TimeShiftViewerTab::FillSegments_()
{
    if (hListSegments_ == nullptr) {
        return;
    }

    ListView_DeleteAllItems(hListSegments_);

    std::wstring selectedKey;

    if (selectedChannel_ >= 0 &&
        selectedChannel_ < static_cast<int>(index_.channels.size())) {
        selectedKey =
            index_.channels[static_cast<std::size_t>(selectedChannel_)].channelKey;
    }

    int row = 0;

    for (const auto& seg : index_.segments) {
        if (!selectedKey.empty() && seg.channelKey != selectedKey) {
            continue;
        }

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(seg.startText.c_str());

        ListView_InsertItem(hListSegments_, &item);

        SetSubItemText_(
            hListSegments_,
            row,
            1,
            std::to_wstring(seg.sequence));

        SetSubItemText_(
            hListSegments_,
            row,
            2,
            TimeShift_ViewIndex::FormatBytes(seg.bytes));

        SetSubItemText_(hListSegments_, row, 3, seg.status);
        SetSubItemText_(hListSegments_, row, 4, seg.filePath.filename().wstring());

        ++row;
    }

    std::wstring info;

    if (!index_.ok) {
        info = L"インデックス作成失敗: ";
        info += index_.reason;
    }
    else {
        info = L"チャンネル数: ";
        info += std::to_wstring(index_.channels.size());
        info += L"\r\nセグメント数: ";
        info += std::to_wstring(index_.segments.size());

        if (selectedChannel_ >= 0 &&
            selectedChannel_ < static_cast<int>(index_.channels.size())) {
            const auto& ch =
                index_.channels[static_cast<std::size_t>(selectedChannel_)];

            info += L"\r\n選択チャンネル: ";
            info += MakeChannelDisplayName_(ch.space, ch.channel);
            info += L"\r\n保存容量: ";
            info += TimeShift_ViewIndex::FormatBytes(ch.totalBytes);
            info += L"\r\n保存区間: ";
            info += MakeRecordingRangeText_(ch);
            info += L"\r\n閉じたセグメント数: ";
            info += std::to_wstring(
                CountSegmentsForChannel_(index_, ch.channelKey, true));
        }
    }

    SetInfo_(info);
}

void TimeShiftViewerTab::FillDiagnostic_()
{
    if (hDiag_ == nullptr) {
        return;
    }

    std::uint64_t totalBytes = 0;
    std::size_t recordingSegments = 0;
    std::size_t closedSegments = 0;

    for (const auto& seg : index_.segments) {
        totalBytes += seg.bytes;
        if (seg.closed) {
            ++closedSegments;
        }
        else {
            ++recordingSegments;
        }
    }

    std::wstring text;

    text += L"TimeShift Viewer diagnostic\r\n";
    text += L"\r\nrootFolder: ";
    text += index_.rootFolder.wstring();

    text += L"\r\nok: ";
    text += index_.ok ? L"1" : L"0";

    if (!index_.reason.empty()) {
        text += L"\r\nreason: ";
        text += index_.reason;
    }

    text += L"\r\nchannels: ";
    text += std::to_wstring(index_.channels.size());

    text += L"\r\nsegments: ";
    text += std::to_wstring(index_.segments.size());

    text += L"\r\nclosedSegments: ";
    text += std::to_wstring(closedSegments);

    text += L"\r\nrecordingSegments: ";
    text += std::to_wstring(recordingSegments);

    text += L"\r\ntotalBytes: ";
    text += std::to_wstring(totalBytes);

    text += L"\r\ntotalSize: ";
    text += TimeShift_ViewIndex::FormatBytes(totalBytes);

    SetWindowTextW(hDiag_, text.c_str());
}

void TimeShiftViewerTab::SetInfo_(std::wstring_view text)
{
    if (hInfo_ == nullptr) {
        return;
    }

    std::wstring s(text);
    SetWindowTextW(hInfo_, s.c_str());
}


void TimeShiftViewerTab::OnRebuildProgramTable_()
{
    if (rebuildBusy_) {
        SetInfo_(L"現在、タイムシフト番組表を再構築中です。");
        return;
    }

    // 再構築中にタイマーが重複発火しないように、
    // 完了後に次回タイマーを登録する。
    StopAutoRebuildTimer_();

    auto& cfg = ConfigManager::GetInstance();

    const std::filesystem::path root =
        cfg.GetTimeShiftRootFolder();

    const TimeShiftTargetChannel_ target =
        LoadTimeShiftTargetChannelFromConfig_();

    const std::filesystem::path epgOutFolder =
        ResolveEpgOutFolder_();

    const std::filesystem::path targetEpgPath =
        target.valid
        ? MakeTimeShiftTargetEpgPath_(epgOutFolder, target)
        : std::filesystem::path{};

    if (target.valid && !targetEpgPath.empty()) {
        const TimeShiftEpgReadSafetyProbe_ safety =
            ProbeTimeShiftEpgReadSafety_(targetEpgPath);

        if (!safety.safeToRead) {
            autoRebuildDeferredByEpgWrite_ = true;
            ++autoRebuildEpgWriteSkipCount_;

            SetInfo_(
                MakeTimeShiftEpgSafetyInfoText_(
                    safety,
                    targetEpgPath));

            if (log_) {
                std::wstring msg =
                    L"[TimeShiftViewer] rebuild deferred before worker: EPG file is not stable";

                msg += L" reason=";
                msg += safety.reason;

                msg += L" ageMs=";
                msg += std::to_wstring(safety.lastWriteAgeMs);

                msg += L" sizeBefore=";
                msg += std::to_wstring(safety.sizeBefore);

                msg += L" sizeAfter=";
                msg += std::to_wstring(safety.sizeAfter);

                msg += L" skipCount=";
                msg += std::to_wstring(autoRebuildEpgWriteSkipCount_);

                msg += L" path=";
                msg += targetEpgPath.wstring();

                log_(msg);
            }

            StartAutoRebuildTimer_(kTimeShiftAutoRebuildEpgBusyRetryMs);
            return;
        }
    }

    if (log_) {
        std::wstring msg = L"[TimeShiftViewer] rebuild begin";

        msg += L" targetValid=";
        msg += target.valid ? L"1" : L"0";

        if (target.valid) {
            msg += L" tuner=";
            msg += std::to_wstring(target.tunerIndex);

            msg += L" space=";
            msg += std::to_wstring(target.space);

            msg += L" ch=";
            msg += std::to_wstring(target.channel);

            msg += L" sid=";
            msg += std::to_wstring(target.serviceId);

            msg += L" nid=";
            msg += std::to_wstring(target.networkId);

            msg += L" tsid=";
            msg += std::to_wstring(target.tsId);
        }

        msg += L" epgOut=";
        msg += epgOutFolder.wstring();

        msg += L" targetEpg=";
        msg += targetEpgPath.wstring();

        log_(msg);
    }

    const std::uint64_t requestId = ++rebuildRequestId_;

    SetRebuildBusy_(
        true,
        L"TimeShift: チューナー設定のEPGファイルを低負荷で読み込み中...");

    std::wstring info =
        L"TimeShiftLockSpace/Channel に対応する EPG を読み込み中です。\r\n";

    if (target.valid) {
        info += L"target: space=";
        info += std::to_wstring(target.space);

        info += L" ch=";
        info += std::to_wstring(target.channel);

        info += L" sid=";
        info += std::to_wstring(target.serviceId);

        info += L"\r\nepg: ";
        info += targetEpgPath.wstring();
    }
    else {
        info += L"TimeShift用チューナー設定 UseRole=5 が見つかりません。";
    }

    SetInfo_(info);

    const HWND notifyWnd = hWnd_;
    const bool gentleMode = kTimeShiftGentleRebuildDefault;

    if (workerPool_ == nullptr ||
        !workerPool_->IsRunning() ||
        !workerPool_->IsAccepting()) {
        SetRebuildBusy_(
            false,
            L"TimeShift: AppWorkerPool が利用できないため再構築できません");

        StartAutoRebuildTimer_(kTimeShiftAutoRebuildBusyRetryMs);

        if (log_) {
            log_(L"[TimeShiftViewer] rebuild skipped: worker pool unavailable");
        }

        return;
    }

    const AppWorker::TaskId taskId =
        workerPool_->PostLow(
            [notifyWnd, root, epgOutFolder, target, targetEpgPath, requestId, gentleMode]
            (AppWorker::TaskContext& ctx)
            {
                TimeShiftGentleThrottle_ throttle(gentleMode);

                if (ctx.IsStopRequested()) {
                    return;
                }

                auto result =
                    std::make_unique<TimeShiftRebuildResult_>();

                result->requestId = requestId;
                result->target = target;
                result->targetEpgPath = targetEpgPath;

                throttle.PhaseBreak();

                if (ctx.IsStopRequested()) {
                    return;
                }

                result->index = TimeShift_ViewIndex::Build(root);

                throttle.PhaseBreak();

                if (ctx.IsStopRequested()) {
                    return;
                }

                std::vector<TimeShift_ViewIndex::ProgramSourceEntry> sources;

                std::error_code ec;

                if (target.valid &&
                    !targetEpgPath.empty() &&
                    std::filesystem::exists(targetEpgPath, ec) &&
                    std::filesystem::is_regular_file(targetEpgPath, ec)) {
                    result->targetEpgFound = true;

                    sources =
                        LoadProgramSourcesFromTargetEpgFile_(
                            targetEpgPath,
                            target,
                            &throttle,
                            result.get());

                    if (!result->epgReadDeferredBySafeGuard) {
                        result->epgSourceCachePath =
                            MakeTimeShiftEpgSourceCachePath_(
                                root,
                                target);

                        result->epgSourceCacheWritten =
                            WriteTimeShiftEpgSourceCache_(
                                result->epgSourceCachePath,
                                sources);
                    }
                }

                result->sourceCount = sources.size();

                throttle.PhaseBreak();

                if (ctx.IsStopRequested()) {
                    return;
                }

                if (!sources.empty()) {
                    const TimeShift_ViewIndex::ProgramBuildOptions opt =
                        LoadTimeShiftProgramBuildOptions_();

                    TimeShift_ViewIndex::BuildProgramsFromSources(
                        result->index,
                        sources,
                        opt);
                }

                throttle.PhaseBreak();

                if (ctx.IsStopRequested()) {
                    return;
                }

                for (const auto& p : result->index.programs) {
                    if (p.status == L"Partial") {
                        ++result->partialCount;

                        if (p.boundaryProbeMayBeEdgeGap) {
                            ++result->edgeGapCandidateCount;
                        }

                        if (p.missingSec > 0 && p.missingSec <= 4u) {
                            ++result->smallMissingCount;
                        }

                        if (p.missingSec > result->maxMissingSec) {
                            result->maxMissingSec = p.missingSec;
                        }

                        if (p.maxGapSec > result->maxGapSec) {
                            result->maxGapSec = p.maxGapSec;
                        }
                    }
                    else if (p.status == L"Deleted") {
                        ++result->deletedCount;
                    }
                    else if (p.status == L"Recording") {
                        ++result->recordingCount;
                    }
                    else if (p.viewAllowed && p.copyAllowed) {
                        ++result->usableCount;
                    }

                    throttle.Tick();
                }

                result->message =
                    L"[TimeShiftViewer] チューナー指定EPG再構築: sources=";

                result->message += std::to_wstring(result->sourceCount);

                result->message += L" programs=";
                result->message += std::to_wstring(result->index.programs.size());

                result->message += L" usable=";
                result->message += std::to_wstring(result->usableCount);

                result->message += L" partial=";
                result->message += std::to_wstring(result->partialCount);

                result->message += L" edgeGapCandidates=";
                result->message += std::to_wstring(result->edgeGapCandidateCount);

                result->message += L" smallMissing=";
                result->message += std::to_wstring(result->smallMissingCount);

                result->message += L" maxMissingSec=";
                result->message += std::to_wstring(result->maxMissingSec);

                result->message += L" maxGapSec=";
                result->message += std::to_wstring(result->maxGapSec);

                result->message += L" deleted=";
                result->message += std::to_wstring(result->deletedCount);

                result->message += L" recording=";
                result->message += std::to_wstring(result->recordingCount);

                result->message += L" segments=";
                result->message += std::to_wstring(result->index.segments.size());

                result->message += L" epgOut=";
                result->message += epgOutFolder.wstring();

                result->message += L" targetValid=";
                result->message += target.valid ? L"1" : L"0";

                if (target.valid) {
                    result->message += L" tuner=";
                    result->message += std::to_wstring(target.tunerIndex);

                    result->message += L" space=";
                    result->message += std::to_wstring(target.space);

                    result->message += L" ch=";
                    result->message += std::to_wstring(target.channel);

                    result->message += L" sid=";
                    result->message += std::to_wstring(target.serviceId);

                    result->message += L" nid=";
                    result->message += std::to_wstring(target.networkId);

                    result->message += L" tsid=";
                    result->message += std::to_wstring(target.tsId);

                    if (!target.name.empty()) {
                        result->message += L" name=";
                        result->message += target.name;
                    }
                }

                result->message += L" targetEpgFound=";
                result->message += result->targetEpgFound ? L"1" : L"0";

                result->message += L" targetEpg=";
                result->message += targetEpgPath.wstring();

                result->message += L" epgTextChars=";
                result->message += std::to_wstring(result->epgTextChars);

                result->message += L" epgLines=";
                result->message += std::to_wstring(result->epgLineCount);

                result->message += L" tableIdLines=";
                result->message += std::to_wstring(result->tableIdLineCount);

                result->message += L" tableIdPrefixLines=";
                result->message += std::to_wstring(result->tableIdPrefixLineCount);

                result->message += L" serviceIdLines=";
                result->message += std::to_wstring(result->serviceIdLineCount);

                result->message += L" eventIdLines=";
                result->message += std::to_wstring(result->eventIdLineCount);

                result->message += L" startTimeLines=";
                result->message += std::to_wstring(result->startTimeLineCount);

                result->message += L" startTimeRawLines=";
                result->message += std::to_wstring(result->startTimeRawLineCount);

                result->message += L" durationLines=";
                result->message += std::to_wstring(result->durationLineCount);

                result->message += L" durationHmsLines=";
                result->message += std::to_wstring(result->durationHmsLineCount);

                result->message += L" titleLines=";
                result->message += std::to_wstring(result->titleLineCount);

                result->message += L" epgRawEvents=";
                result->message += std::to_wstring(result->epgRawEvents);

                result->message += L" epgAcceptedEvents=";
                result->message += std::to_wstring(result->epgAcceptedEvents);

                result->message += L" epgSkipStartDate=";
                result->message += std::to_wstring(result->epgSkipNoStartDate);

                result->message += L" epgSkipStartTime=";
                result->message += std::to_wstring(result->epgSkipNoStartTime);

                result->message += L" epgSkipDuration=";
                result->message += std::to_wstring(result->epgSkipNoDuration);

                result->message += L" epgSkipTitle=";
                result->message += std::to_wstring(result->epgSkipNoTitle);

                result->message += L" epgSkipServiceId=";
                result->message += std::to_wstring(result->epgSkipNoServiceId);

                result->message += L" epgSkipEndTime=";
                result->message += std::to_wstring(result->epgSkipEndTimeFailed);

                result->message += L" epgSkipChannel=";
                result->message += std::to_wstring(result->epgSkipChannel);

                result->message += L" epgServiceMapHit=";
                result->message += std::to_wstring(result->epgServiceMapHit);

                result->message += L" epgServiceMapMiss=";
                result->message += std::to_wstring(result->epgServiceMapMiss);

                if (!result->epgFirstAcceptedTitle.empty()) {
                    result->message += L" epgFirstAcceptedTitle=[";
                    result->message += result->epgFirstAcceptedTitle;
                    result->message += L"]";
                }

                if (!result->epgFirstSkippedTitle.empty()) {
                    result->message += L" epgFirstSkippedTitle=[";
                    result->message += result->epgFirstSkippedTitle;
                    result->message += L"]";
                }

                if (!result->firstRawSource.empty()) {
                    result->message += L" firstRaw=[";
                    result->message += result->firstRawSource;
                    result->message += L"]";
                }

                if (!result->firstAcceptedSource.empty()) {
                    result->message += L" firstAccepted=[";
                    result->message += result->firstAcceptedSource;
                    result->message += L"]";
                }

                result->message += L" rawSources=";
                result->message += std::to_wstring(result->rawSourceCount);

                result->message += L" dropSid=";
                result->message += std::to_wstring(result->filteredByServiceId);

                result->message += L" dropNid=";
                result->message += std::to_wstring(result->filteredByNetworkId);

                result->message += L" dropTsid=";
                result->message += std::to_wstring(result->filteredByTsId);

                result->message += L" epgSourceCacheWritten=";
                result->message += result->epgSourceCacheWritten ? L"1" : L"0";

                result->message += L" epgReadDeferred=";
                result->message += result->epgReadDeferredBySafeGuard ? L"1" : L"0";

                if (!result->epgReadSafeReason.empty()) {
                    result->message += L" epgReadSafeReason=[";
                    result->message += result->epgReadSafeReason;
                    result->message += L"]";

                    result->message += L" epgReadAgeMs=";
                    result->message += std::to_wstring(result->epgReadSafeAgeMs);

                    result->message += L" epgReadSizeBefore=";
                    result->message += std::to_wstring(result->epgReadSizeBefore);

                    result->message += L" epgReadSizeAfter=";
                    result->message += std::to_wstring(result->epgReadSizeAfter);
                }

                if (!result->epgSourceCachePath.empty()) {
                    result->message += L" epgSourceCache=";
                    result->message += result->epgSourceCachePath.wstring();
                }

                if (ctx.IsStopRequested()) {
                    return;
                }

                TimeShiftRebuildResult_* raw = result.release();

                if (!IsWindow(notifyWnd) ||
                    !PostMessageW(
                        notifyWnd,
                        WM_TS_REBUILD_DONE,
                        0,
                        reinterpret_cast<LPARAM>(raw))) {
                    delete raw;
                }
            });

    if (taskId == 0) {
        SetRebuildBusy_(
            false,
            L"TimeShift: AppWorkerPool への再構築登録に失敗しました");

        StartAutoRebuildTimer_(kTimeShiftAutoRebuildBusyRetryMs);

        if (log_) {
            log_(L"[TimeShiftViewer] rebuild post failed: worker pool rejected task");
        }

        return;
    }

    if (log_) {
        std::wstring msg = L"[TimeShiftViewer] rebuild posted to AppWorkerPool taskId=";
        msg += std::to_wstring(taskId);
        log_(msg);
    }
}


std::wstring TimeShiftViewerTab::MakeSafeFileNamePart_(
    const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size());

    for (const wchar_t ch : text) {
        switch (ch) {
        case L'<':
        case L'>':
        case L':':
        case L'"':
        case L'/':
        case L'\\':
        case L'|':
        case L'?':
        case L'*':
        case L'\r':
        case L'\n':
        case L'\t':
            out.push_back(L'_');
            break;

        default:
            if (ch < 0x20) {
                out.push_back(L'_');
            }
            else {
                out.push_back(ch);
            }
            break;
        }
    }

    if (out.empty()) {
        out = L"timeshift_program";
    }

    if (out.size() > 120) {
        out.resize(120);
    }

    return out;
}

std::filesystem::path TimeShiftViewerTab::MakeProgramCopyOutputPath_(
    const TimeShift_ViewIndex::ProgramEntry& program) const
{
    std::filesystem::path base = L"timeshift_export";

    auto& cfg = ConfigManager::GetInstance();

    const std::filesystem::path root =
        cfg.GetTimeShiftRootFolder();

    if (!root.empty()) {
        base = root / L"export";
    }

    std::wstring name;

    if (!program.startText.empty()) {
        name += program.startText;
        for (auto& ch : name) {
            if (ch == L':' || ch == L' ' || ch == L'-') {
                ch = L'_';
            }
        }
    }
    else {
        name += L"unknown_time";
    }

    name += L"_";

    if (!program.title.empty()) {
        name += program.title;
    }
    else {
        name += L"timeshift_program";
    }

    name = MakeSafeFileNamePart_(name);
    name += L".ts";

    return base / name;
}

void TimeShiftViewerTab::RefreshCurrentSubPage_()
{
    switch (currentPage_) {
    case SubPage::Program:
        RefreshProgramTablePage_();
        break;

    case SubPage::Status:
        RefreshStorageStatusPage_();
        break;

    case SubPage::Detail:
        RefreshDetailPage_();
        break;

    case SubPage::Diagnostic:
        RefreshDiagnosticPage_();
        break;

    default:
        RefreshProgramTablePage_();
        break;
    }
}

void TimeShiftViewerTab::RefreshProgramTablePage_()
{
    if (hProgramNotice_ != nullptr) {
        SetWindowTextW(
            hProgramNotice_,
            L"タイムシフト番組一覧は別窓に移動しました。\r\n"
            L"上部の「一覧窓を表示/前面へ」を押すと、別窓を表示または前面復帰します。\r\n"
            L"番組一覧の再構築は、保存セグメントと .epg をもとに自動で行います。\r\n"
            L"別窓側で表示フィルタ、選択番組コピーを操作できます。");
    }

    SyncProgramListWindow_();
}

void TimeShiftViewerTab::RefreshStorageStatusPage_()
{
    FillStatusView_();
}

void TimeShiftViewerTab::RefreshDetailPage_()
{
    FillChannels_();
    FillSegments_();
}

void TimeShiftViewerTab::RefreshDiagnosticPage_()
{
    FillDiagnostic_();
}

std::vector<TimeShift_ViewIndex::ProgramSourceEntry>
TimeShiftViewerTab::LoadProgramSourcesFromBeforeFolder_(
    const std::filesystem::path& beforeFolder) const
{
    std::vector<TimeShift_ViewIndex::ProgramSourceEntry> out;

    std::error_code ec;

    if (beforeFolder.empty() ||
        !std::filesystem::exists(beforeFolder, ec) ||
        !std::filesystem::is_directory(beforeFolder, ec)) {
        return out;
    }

    constexpr int kSpace = 0;

    const auto serviceMap =
        LoadTimeShiftServiceMapFromConfig_(kSpace);

    for (std::filesystem::directory_iterator it(beforeFolder, ec), end;
        !ec && it != end;
        it.increment(ec)) {

        std::error_code fileEc;
        if (!it->is_regular_file(fileEc)) {
            continue;
        }

        const std::filesystem::path path = it->path();
        const std::wstring fileName = path.filename().wstring();

        // タイムシフト抽出元は *_list.html 固定。
        // *_timetable.html は表示用だが identity=0 の未解決が混ざるので使わない。
        if (!HasSuffixText_(fileName, L"_list.html")) {
            continue;
        }

        const std::wstring html = ReadUtf8FileToWide_(path);
        if (html.empty()) {
            continue;
        }

        std::size_t pos = 0;

        for (;;) {
            const std::size_t mark =
                html.find(L"data-epg-program=\"1\"", pos);

            if (mark == std::wstring::npos) {
                break;
            }

            const std::size_t tagStart = html.rfind(L'<', mark);
            const std::size_t tagEnd = html.find(L'>', mark);

            if (tagStart == std::wstring::npos ||
                tagEnd == std::wstring::npos ||
                tagEnd <= tagStart) {
                pos = mark + 1;
                continue;
            }

            const std::wstring tag =
                html.substr(tagStart, tagEnd - tagStart + 1);

            pos = tagEnd + 1;

            const std::wstring dateText = ExtractAttr_(tag, L"data-date");
            const std::wstring timeText = ExtractAttr_(tag, L"data-time");

            std::wstring startText;
            std::wstring endText;

            if (!MakeStartEndText_(dateText, timeText, startText, endText)) {
                continue;
            }

            const std::uint16_t networkId =
                static_cast<std::uint16_t>(
                    ToULong_(ExtractAttr_(tag, L"data-network-id")));

            const std::uint16_t tsId =
                static_cast<std::uint16_t>(
                    ToULong_(ExtractAttr_(tag, L"data-ts-id")));

            const std::uint16_t serviceId =
                static_cast<std::uint16_t>(
                    ToULong_(ExtractAttr_(tag, L"data-service-id")));

            const std::uint16_t eventId =
                static_cast<std::uint16_t>(
                    ToULong_(ExtractAttr_(tag, L"data-event-id")));

            if (networkId == 0 || tsId == 0 || serviceId == 0) {
                continue;
            }

            const std::wstring mapKey =
                MakeServiceMapKey_(networkId, tsId, serviceId);

            auto mapIt = serviceMap.find(mapKey);
            if (mapIt == serviceMap.end()) {
                continue;
            }

            TimeShift_ViewIndex::ProgramSourceEntry src{};

            src.space = mapIt->second.space;
            src.channel = mapIt->second.channel;

            src.networkId = networkId;
            src.tsId = tsId;
            src.serviceId = serviceId;
            src.eventId = eventId;

            src.serviceName = ExtractAttr_(tag, L"data-service");
            if (src.serviceName.empty()) {
                src.serviceName = mapIt->second.serviceName;
            }

            src.title = ExtractAttr_(tag, L"data-title");

            src.startText = startText;
            src.endText = endText;

            if (src.channel < 0 || src.title.empty()) {
                continue;
            }

            out.push_back(std::move(src));
        }
    }

    std::sort(
        out.begin(),
        out.end(),
        [](const auto& a, const auto& b)
        {
            if (a.space != b.space) {
                return a.space < b.space;
            }

            if (a.channel != b.channel) {
                return a.channel < b.channel;
            }

            if (a.startText != b.startText) {
                return a.startText < b.startText;
            }

            if (a.serviceId != b.serviceId) {
                return a.serviceId < b.serviceId;
            }

            return a.eventId < b.eventId;
        });

    out.erase(
        std::unique(
            out.begin(),
            out.end(),
            [](const auto& a, const auto& b)
            {
                return
                    a.space == b.space &&
                    a.channel == b.channel &&
                    a.networkId == b.networkId &&
                    a.tsId == b.tsId &&
                    a.serviceId == b.serviceId &&
                    a.eventId == b.eventId &&
                    a.startText == b.startText &&
                    a.endText == b.endText;
            }),
        out.end());

    return out;
}

std::vector<TimeShift_ViewIndex::ProgramSourceEntry>
TimeShiftViewerTab::LoadProgramSourcesFromEpgOut_(
    const std::filesystem::path& epgOutFolder) const
{
    return LoadProgramSourcesFromEpgOutCore_(epgOutFolder, nullptr);
}


void TimeShiftViewerTab::StartAutoRebuildTimer_(DWORD delayMs) noexcept
{
    if (hWnd_ == nullptr) {
        return;
    }

    if (delayMs < 1000u) {
        delayMs = 1000u;
    }

    KillTimer(hWnd_, IDT_TS_AUTO_REBUILD);

    SetTimer(
        hWnd_,
        IDT_TS_AUTO_REBUILD,
        delayMs,
        nullptr);
}

void TimeShiftViewerTab::StopAutoRebuildTimer_() noexcept
{
    if (hWnd_ == nullptr) {
        return;
    }

    KillTimer(hWnd_, IDT_TS_AUTO_REBUILD);
}

void TimeShiftViewerTab::OnAutoRebuildTimer_()
{
    StopAutoRebuildTimer_();

    if (rebuildBusy_) {
        if (log_) {
            log_(L"[TimeShiftViewer] auto rebuild skipped: busy");
        }

        StartAutoRebuildTimer_(kTimeShiftAutoRebuildBusyRetryMs);
        return;
    }

    auto& cfg = ConfigManager::GetInstance();

    const std::filesystem::path root =
        cfg.GetTimeShiftRootFolder();

    const TimeShiftTargetChannel_ target =
        LoadTimeShiftTargetChannelFromConfig_();

    const std::filesystem::path epgOutFolder =
        ResolveEpgOutFolder_();

    const std::filesystem::path targetEpgPath =
        target.valid
        ? MakeTimeShiftTargetEpgPath_(epgOutFolder, target)
        : std::filesystem::path{};

    // EPG更新中または更新直後なら、TimeShift一覧の自動再構築は行わない。
    // ここではTimeShift録画本体は止めない。
    if (target.valid && !targetEpgPath.empty()) {
        const TimeShiftEpgReadSafetyProbe_ safety =
            ProbeTimeShiftEpgReadSafety_(targetEpgPath);

        if (!safety.safeToRead) {
            autoRebuildDeferredByEpgWrite_ = true;
            ++autoRebuildEpgWriteSkipCount_;

            if (log_) {
                std::wstring msg =
                    L"[TimeShiftViewer] auto rebuild deferred: EPG file is not stable";

                msg += L" reason=";
                msg += safety.reason;

                msg += L" ageMs=";
                msg += std::to_wstring(safety.lastWriteAgeMs);

                msg += L" sizeBefore=";
                msg += std::to_wstring(safety.sizeBefore);

                msg += L" sizeAfter=";
                msg += std::to_wstring(safety.sizeAfter);

                msg += L" skipCount=";
                msg += std::to_wstring(autoRebuildEpgWriteSkipCount_);

                msg += L" path=";
                msg += targetEpgPath.wstring();

                log_(msg);
            }

            SetInfo_(
                MakeTimeShiftEpgSafetyInfoText_(
                    safety,
                    targetEpgPath));

            StartAutoRebuildTimer_(kTimeShiftAutoRebuildEpgBusyRetryMs);
            return;
        }

        if (autoRebuildDeferredByEpgWrite_ && log_) {
            std::wstring msg =
                L"[TimeShiftViewer] auto rebuild resumes after EPG file became stable";

            msg += L" skipCount=";
            msg += std::to_wstring(autoRebuildEpgWriteSkipCount_);

            msg += L" path=";
            msg += targetEpgPath.wstring();

            log_(msg);
        }

        autoRebuildDeferredByEpgWrite_ = false;
    }

    const TimeShift_ViewIndex::BuildResult probe =
        TimeShift_ViewIndex::Build(root);

    if (!probe.ok) {
        std::wstring msg = L"[TimeShiftViewer] auto rebuild skipped: index failed";

        if (!probe.reason.empty()) {
            msg += L" reason=";
            msg += probe.reason;
        }

        if (log_) {
            log_(msg);
        }

        StartAutoRebuildTimer_(kTimeShiftAutoRebuildIntervalMs);
        return;
    }

    if (probe.segments.empty()) {
        if (log_) {
            log_(L"[TimeShiftViewer] auto rebuild skipped: segments empty");
        }

        StartAutoRebuildTimer_(kTimeShiftAutoRebuildIntervalMs);
        return;
    }

    bool epgExists = false;

    const std::filesystem::file_time_type epgWrite =
        GetLastWriteTimeSafe_(
            targetEpgPath,
            epgExists);

    const std::size_t segmentCount =
        probe.segments.size();

    const bool first =
        !hasLastAutoRebuildState_;

    std::size_t segmentDelta = 0;

    if (!first) {
        segmentDelta =
            segmentCount > lastAutoRebuildSegmentCount_
            ? segmentCount - lastAutoRebuildSegmentCount_
            : lastAutoRebuildSegmentCount_ - segmentCount;
    }

    const bool segmentChanged =
        first ||
        segmentDelta >= kTimeShiftAutoRebuildMinSegmentDelta;

    const bool epgChanged =
        first ||
        epgWrite != lastAutoRebuildEpgWrite_;

    const bool noPrograms =
        index_.programs.empty();

    const bool shouldRebuild =
        noPrograms ||
        segmentChanged ||
        epgChanged;

    if (log_) {
        std::wstring msg = L"[TimeShiftViewer] auto rebuild check";

        msg += L" first=";
        msg += first ? L"1" : L"0";

        msg += L" epgExists=";
        msg += epgExists ? L"1" : L"0";

        msg += L" noPrograms=";
        msg += noPrograms ? L"1" : L"0";

        msg += L" segmentChanged=";
        msg += segmentChanged ? L"1" : L"0";

        msg += L" epgChanged=";
        msg += epgChanged ? L"1" : L"0";

        msg += L" lastSegments=";
        msg += std::to_wstring(lastAutoRebuildSegmentCount_);

        msg += L" nowSegments=";
        msg += std::to_wstring(segmentCount);

        msg += L" segmentDelta=";
        msg += std::to_wstring(segmentDelta);

        msg += L" minSegmentDelta=";
        msg += std::to_wstring(kTimeShiftAutoRebuildMinSegmentDelta);

        msg += L" rebuild=";
        msg += shouldRebuild ? L"1" : L"0";

        log_(msg);
    }

    if (!shouldRebuild) {
        StartAutoRebuildTimer_(kTimeShiftAutoRebuildIntervalMs);
        return;
    }

    hasLastAutoRebuildState_ = true;
    lastAutoRebuildSegmentCount_ = segmentCount;
    lastAutoRebuildEpgWrite_ = epgWrite;

    OnRebuildProgramTable_();
}






void TimeShiftViewerTab::SetRebuildBusy_(
    bool busy,
    std::wstring_view text)
{
    rebuildBusy_ = busy;

    if (hBtnReload_ != nullptr) {
        EnableWindow(hBtnReload_, busy ? FALSE : TRUE);
    }

    

    if (hLblRoot_ != nullptr) {
        std::wstring label(text);
        SetWindowTextW(hLblRoot_, label.c_str());
        UpdateWindow(hLblRoot_);
    }

    if (hWnd_ != nullptr) {
        SetCursor(LoadCursorW(nullptr, busy ? IDC_WAIT : IDC_ARROW));
        UpdateWindow(hWnd_);
    }

    if (programListWindow_.IsCreated()) {
        programListWindow_.SetBusy(busy, text);
    }
}


void TimeShiftViewerTab::OnRebuildProgramTableDone_(LPARAM lp)
{
    std::unique_ptr<TimeShiftRebuildResult_> result(
        reinterpret_cast<TimeShiftRebuildResult_*>(lp));

    if (!result) {
        SetRebuildBusy_(false, L"TimeShift: 再構築結果なし");
        return;
    }

    if (result->requestId != rebuildRequestId_) {
        return;
    }

    if (result->epgReadDeferredBySafeGuard) {
        autoRebuildDeferredByEpgWrite_ = true;
        ++autoRebuildEpgWriteSkipCount_;

        SetRebuildBusy_(
            false,
            L"TimeShift: EPG更新中のため番組一覧更新を延期");

        std::wstring msg =
            L"EPG更新中、またはEPGファイル更新直後の可能性があるため、";
        msg += L"タイムシフト番組一覧の反映を延期しました。\r\n";
        msg += L"既存の一覧は維持します。\r\n";
        msg += L"TimeShift録画本体は継続しています。\r\n";

        msg += L"\r\nreason: ";
        msg += result->epgReadSafeReason;

        msg += L"\r\nageMs: ";
        msg += std::to_wstring(result->epgReadSafeAgeMs);

        msg += L"\r\nsizeBefore: ";
        msg += std::to_wstring(result->epgReadSizeBefore);

        msg += L"\r\nsizeAfter: ";
        msg += std::to_wstring(result->epgReadSizeAfter);

        msg += L"\r\npath: ";
        msg += result->targetEpgPath.wstring();

        SetInfo_(msg);

        if (log_) {
            std::wstring logMsg =
                L"[TimeShiftViewer] rebuild result ignored: EPG read deferred";

            logMsg += L" reason=";
            logMsg += result->epgReadSafeReason;

            logMsg += L" ageMs=";
            logMsg += std::to_wstring(result->epgReadSafeAgeMs);

            logMsg += L" skipCount=";
            logMsg += std::to_wstring(autoRebuildEpgWriteSkipCount_);

            logMsg += L" path=";
            logMsg += result->targetEpgPath.wstring();

            log_(logMsg);

            if (!result->message.empty()) {
                log_(result->message);
            }
        }

        StartAutoRebuildTimer_(kTimeShiftAutoRebuildEpgBusyRetryMs);
        return;
    }

    index_ = std::move(result->index);

    // 自動更新タイマーの比較基準を、完了した再構築結果で更新する。
    // これを入れないと次回タイマーで first=1 扱いになり、
    // 余計な再構築が走りやすくなる。
    hasLastAutoRebuildState_ = true;
    lastAutoRebuildSegmentCount_ = index_.segments.size();

    {
        bool epgExists = false;
        lastAutoRebuildEpgWrite_ =
            GetLastWriteTimeSafe_(
                result->targetEpgPath,
                epgExists);
    }

    selectedChannel_ = -1;

    // 自動更新でタブページを勝手に「過去番組表」へ戻さない。
    // 現在表示中のページだけ更新する。
    RefreshCurrentSubPage_();

    SetRebuildBusy_(
        false,
        L"TimeShift: .epg番組表 再構築完了");

    SyncProgramListWindow_();

    // 再構築完了から一定時間後に次回チェックする。
    // これにより「処理時間 + 待機時間」の間隔になる。
    StartAutoRebuildTimer_(kTimeShiftAutoRebuildIntervalMs);

    if (log_) {
        log_(result->message);
    }

    if (result->sourceCount == 0) {
        std::wstring msg = result->message;

        msg += L"\r\n\r\nチューナー設定に対応するEPGイベントを読み込めませんでした。";

        if (!result->target.valid) {
            msg += L"\r\nUseRole=5 の TimeShift チューナー設定が見つかりません。";
        }
        else if (!result->targetEpgFound) {
            msg += L"\r\n対象EPGファイルが見つかりません。";
            msg += L"\r\n期待パス: ";
            msg += result->targetEpgPath.wstring();
        }
        else {
            msg += L"\r\nEPGファイルは見つかっていますが、";
            msg += L"ServiceID / NetworkID / TSID の一致イベントがありません。";
        }

        SetInfo_(msg);
        return;
    }

    if (result->usableCount == 0) {
        SetInfo_(
            result->message +
            L"\r\n\r\nセグメントが100%揃っている番組はありません。\r\n"
            L"表示条件を「不可」「欠損あり」に切り替えると判定結果を確認できます。");
        return;
    }

    SetInfo_(
        result->message +
        L"\r\n\r\n標準表示は「可能」のみです。\r\n"
        L"この一覧に出ている番組だけ、タイムシフトセグメントが完全に揃っています。");
}

bool TimeShiftViewerTab::IsProgramWindowTopMostSwitchOn_() const noexcept
{
    // 常時最前面固定は廃止。
    // ボタンを押した時だけ一覧窓を前面へ出す。
    return false;
}

void TimeShiftViewerTab::OnProgramWindowSwitch_()
{
    if (!programListWindow_.IsCreated()) {
        const bool created =
            programListWindow_.Create(
                hWnd_,
                hInst_,
                hFont_,
                [this](const TimeShift_ViewIndex::ProgramEntry& program)
                {
                    CopyProgramEntry_(program);
                },
                log_);

        if (!created) {
            SetInfo_(L"タイムシフト番組一覧ウィンドウを作成できませんでした。");
            return;
        }
    }

    SyncProgramListWindow_();

    // 常時最前面固定は使わない。
    // 押した時だけ通常ウィンドウとして前面へ出す。
    programListWindow_.ShowOrActivate(false);

    std::wstring msg = L"タイムシフト番組一覧を表示しました。";

    if (index_.programs.empty()) {
        msg += L"\r\n番組データが未構築のため、保存セグメントと .epg の照合を開始します。";
    }

    SetInfo_(msg);

    if (log_) {
        log_(L"[TimeShiftViewer] program list window show");
    }

    RequestProgramTableRebuildIfNeeded_();
}

void TimeShiftViewerTab::SyncProgramListWindow_()
{
    if (!programListWindow_.IsCreated()) {
        return;
    }

    programListWindow_.SetSnapshot(index_);
}

void TimeShiftViewerTab::CopyProgramEntry_(
    const TimeShift_ViewIndex::ProgramEntry& program)
{
    if (!program.copyAllowed) {
        SetInfo_(L"この番組は100%保存済みではないためコピーできません。");
        return;
    }

    if (program.joinSegments.empty()) {
        SetInfo_(L"結合対象セグメントがありません。");
        if (log_) {
            log_(L"[TimeShiftViewer] 番組コピー中止: joinSegments empty");
        }
        return;
    }

    const std::filesystem::path outputPath =
        MakeProgramCopyOutputPath_(program);

    TimeShift_ViewIndex::JoinExportOptions opt{};

    auto& cfg = ConfigManager::GetInstance();

    opt.cacheRoot =
        std::filesystem::path(cfg.GetTimeShiftRootFolder()) /
        L"cache" /
        L"join_work";

    opt.useHardLink = true;
    opt.deleteCacheOnSuccess = true;
    opt.deleteCacheOnFailure = false;

    const std::wstring planText =
        MakeTimeShiftCopyPlanText_(
            program,
            outputPath);

    if (log_) {
        log_(planText);
    }

    SetInfo_(planText);

    std::wstring reason;

    const bool ok =
        TimeShift_ViewIndex::ExportJoinedTs(
            program,
            outputPath,
            opt,
            reason);

    if (ok) {
        std::wstring msg = L"[TimeShiftViewer] 番組コピー完了: ";
        msg += outputPath.wstring();

        msg += L" joinSegments=";
        msg += std::to_wstring(program.joinSegments.size());

        msg += L" padding=前";
        msg += std::to_wstring(program.joinPaddingBeforeCount);
        msg += L"/後";
        msg += std::to_wstring(program.joinPaddingAfterCount);

        msg += L" joinRange=";
        if (!program.joinPlanStartText.empty() ||
            !program.joinPlanEndText.empty()) {
            msg += program.joinPlanStartText;
            msg += L"～";
            msg += program.joinPlanEndText;
        }
        else {
            msg += program.joinSegments.front().startText;
            msg += L"～";
            msg += program.joinSegments.back().endText;
        }

        msg += L" size=";
        msg += TimeShift_ViewIndex::FormatBytes(program.totalBytes);

        if (log_) {
            log_(msg);
        }

        SetInfo_(msg);
        return;
    }

    std::wstring msg = L"[TimeShiftViewer] 番組コピー失敗: ";
    msg += reason;

    msg += L" joinSegments=";
    msg += std::to_wstring(program.joinSegments.size());

    msg += L" padding=前";
    msg += std::to_wstring(program.joinPaddingBeforeCount);
    msg += L"/後";
    msg += std::to_wstring(program.joinPaddingAfterCount);

    if (log_) {
        log_(msg);
    }

    SetInfo_(msg);
}

void TimeShiftViewerTab::RequestProgramTableRebuildIfNeeded_()
{
    if (log_) {
        std::wstring msg = L"[TimeShiftViewer] rebuild request check";

        msg += L" busy=";
        msg += rebuildBusy_ ? L"1" : L"0";

        msg += L" ok=";
        msg += index_.ok ? L"1" : L"0";

        msg += L" segments=";
        msg += std::to_wstring(index_.segments.size());

        msg += L" programs=";
        msg += std::to_wstring(index_.programs.size());

        log_(msg);
    }

    if (rebuildBusy_) {
        if (log_) {
            log_(L"[TimeShiftViewer] rebuild request skipped: busy");
        }
        return;
    }

    if (!index_.programs.empty()) {
        if (log_) {
            log_(L"[TimeShiftViewer] rebuild request skipped: programs already built");
        }
        return;
    }

    if (!index_.ok) {
        std::wstring msg = L"タイムシフトインデックス作成に失敗しています。";

        if (!index_.reason.empty()) {
            msg += L"\r\n理由: ";
            msg += index_.reason;
        }

        SetInfo_(msg);

        if (log_) {
            std::wstring logMsg = L"[TimeShiftViewer] rebuild request skipped: index failed ";
            logMsg += index_.reason;
            log_(logMsg);
        }

        return;
    }

    if (index_.segments.empty()) {
        SetInfo_(
            L"タイムシフト保存セグメントが見つかりません。\r\n"
            L"TimeShift root 設定、または保存先フォルダを確認してください。");

        if (log_) {
            log_(L"[TimeShiftViewer] rebuild request skipped: segments empty");
        }

        return;
    }

    const TimeShiftTargetChannel_ target =
        LoadTimeShiftTargetChannelFromConfig_();

    const std::filesystem::path epgOutFolder =
        ResolveEpgOutFolder_();

    const std::filesystem::path targetEpgPath =
        target.valid
        ? MakeTimeShiftTargetEpgPath_(epgOutFolder, target)
        : std::filesystem::path{};

    if (log_) {
        std::wstring msg = L"[TimeShiftViewer] rebuild request accepted";

        msg += L" targetValid=";
        msg += target.valid ? L"1" : L"0";

        if (target.valid) {
            msg += L" tuner=";
            msg += std::to_wstring(target.tunerIndex);

            msg += L" space=";
            msg += std::to_wstring(target.space);

            msg += L" ch=";
            msg += std::to_wstring(target.channel);

            msg += L" sid=";
            msg += std::to_wstring(target.serviceId);
        }

        msg += L" epg=";
        msg += targetEpgPath.wstring();

        log_(msg);
    }

    SetInfo_(
        L"保存セグメントは見つかりましたが、番組一覧が未構築です。\r\n"
        L"チューナー設定に対応する EPG ファイルを読み込み、"
        L"低優先度で番組一覧を再構築します。");

    OnRebuildProgramTable_();
}