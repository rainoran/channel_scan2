#pragma once
#ifndef TIMESHIFT_SEGMENT_STORE_HPP
#define TIMESHIFT_SEGMENT_STORE_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <thread>

class TimeShift_SegmentStore final
{
public:
    struct ChannelKey
    {
        int space = 0;
        int channel = -1;
    };

    struct Settings
    {
        std::filesystem::path rootFolder = L"timeshift";

        // タイムシフト rootFolder 全体の上限。
// rootFolder/space*_ch*/seg_*.ts を全チャンネル合算で監視する。
        std::uint64_t maxBytes = 100ull * 1024ull * 1024ull * 1024ull;

        // 80%を超えたら削除開始。
        int highWatermarkPercent = 80;

        // 70%まで戻す。
        int lowWatermarkPercent = 70;

        // セグメント切替条件。
        std::uint32_t segmentSeconds = 60;
        std::uint64_t segmentMaxBytes = 256ull * 1024ull * 1024ull;

        // TSパケット換算用。基本は188。
        std::uint32_t packetSize = 188;

        // QuotaCleaner。
// Writer hot path では削除せず、別スレッドで容量監視する。
        std::uint32_t quotaCleanerIntervalSec = 15;

        // 最新セグメント保護時間。
        // 1分セグメントなら1200秒=20本前後を削除対象から外す。
        std::uint32_t quotaCleanerProtectRecentSec = 20u * 60u;

        // 1回のCleanerで削除する最大セグメント数。
        // 削除I/Oのバーストを避ける。
        std::uint32_t quotaCleanerMaxDeleteSegmentsPerPass = 16;
    };

    struct SegmentInfo
    {
        std::uint64_t sequence = 0;

        std::wstring startText;
        std::wstring endText;

        std::filesystem::path filePath;
        std::uint64_t bytes = 0;
        std::uint64_t packetCount = 0;

        bool closed = true;
    };

    struct Status
    {
        bool active = false;

        ChannelKey key{};

        std::filesystem::path channelFolder;
        std::filesystem::path currentTempPath;
        std::filesystem::path currentFinalPath;

        std::uint64_t totalClosedBytes = 0;
        std::uint64_t currentBytes = 0;
        std::uint64_t currentPackets = 0;
        std::uint64_t dropBytes = 0;

        std::size_t closedSegmentCount = 0;
    };

    struct QuotaStatus
    {
        bool ok = false;

        std::filesystem::path rootFolder;

        std::uint64_t maxBytes = 0;
        std::uint64_t highBytes = 0;
        std::uint64_t lowBytes = 0;

        std::uint64_t totalBefore = 0;
        std::uint64_t totalAfter = 0;
        std::uint64_t deletedBytes = 0;

        std::size_t scannedSegments = 0;
        std::size_t deletedSegments = 0;
        std::size_t failedDeletes = 0;
        std::size_t cleanupDeleteFiles = 0;
        std::size_t manifestUpdated = 0;

        bool overHighWatermark = false;
        bool reducedToLowWatermark = false;

        std::wstring reason;
    };

public:
    TimeShift_SegmentStore() = default;
    ~TimeShift_SegmentStore() noexcept;

    TimeShift_SegmentStore(const TimeShift_SegmentStore&) = delete;
    TimeShift_SegmentStore& operator=(const TimeShift_SegmentStore&) = delete;

    void Configure(const Settings& settings);

    bool StartChannel(const ChannelKey& key);
    void StopChannel(const ChannelKey& key) noexcept;
    void StopAll() noexcept;

    bool WriteBytes(
        const ChannelKey& key,
        const void* data,
        std::size_t bytes) noexcept;

    bool Rotate(const ChannelKey& key) noexcept;

    std::vector<SegmentInfo> ListSegments(const ChannelKey& key);

    Status GetStatus(const ChannelKey& key) const;

    // rootFolder 全体の容量を監視し、古い closed セグメントから削除する。
// Writer hot path からは直接呼ばない。
// デバッグ/手動メンテ用に外からも呼べるようにする。
    QuotaStatus EnforceQuota() noexcept;

    bool StartQuotaCleaner() noexcept;
    void StopQuotaCleaner() noexcept;
    void RequestQuotaCleanup() noexcept;

    // チャンネルフォルダを空にする。
    // 実行中の場合は一度Stopしてから削除する。
    bool ResetChannel(const ChannelKey& key) noexcept;

    static std::wstring MakeChannelFolderName(const ChannelKey& key);

private:
    struct Session
    {
        bool active = false;

        ChannelKey key{};
        std::filesystem::path folder;

        std::ofstream currentFile;
        std::filesystem::path currentTempPath;
        std::filesystem::path currentFinalPath;

        std::uint64_t currentBytes = 0;
        std::uint64_t currentPackets = 0;
        std::uint64_t nextSequence = 1;

        std::wstring currentStartText;
        std::uint64_t currentStartTickMs = 0;

        std::uint64_t dropBytes = 0;

        std::vector<SegmentInfo> segments;
    };

    struct RootSegmentInfo
    {
        std::filesystem::path filePath;
        std::filesystem::path channelFolder;

        std::wstring startText;
        std::uint64_t sequence = 0;
        std::uint64_t bytes = 0;
    };

private:
    static std::wstring MakeNowText_();
    static std::uint64_t GetTickMs_() noexcept;
    static std::wstring MakeMapKey_(const ChannelKey& key);
    static bool IsValidKey_(const ChannelKey& key) noexcept;

    // rootFolder 全体の容量削除は、複数 TimeShift_SegmentStore インスタンス間で共有する。
    // SegmentRecorder はチューナーごとに store_ を持つため、store_ 個別 mutex_ だけでは
    // rootFolder 全体削除の同時実行を止められない。
    static std::mutex& RootQuotaMutex_() noexcept;

    std::filesystem::path MakeChannelFolderPath_(const ChannelKey& key) const;
    std::filesystem::path MakeManifestPath_(const Session& s) const;

    bool RecoverChannelLocked_(Session& s) noexcept;
    bool OpenNewSegmentLocked_(Session& s) noexcept;
    bool CloseCurrentSegmentLocked_(Session& s) noexcept;

    bool WriteManifestLocked_(const Session& s) noexcept;

    QuotaStatus EnforceQuotaLocked_() noexcept;

    QuotaStatus EnforceQuotaFromCleaner_() noexcept;

    void QuotaCleanerLoop_() noexcept;

    bool ScanRootSegmentsForCleaner_(
        const Settings& settings,
        std::vector<RootSegmentInfo>& segments,
        QuotaStatus& result) const noexcept;

    void CleanupDeleteLeftoversForCleaner_(
        const Settings& settings,
        QuotaStatus& result) const noexcept;

    bool DeleteRootSegmentForCleaner_(
        const RootSegmentInfo& seg,
        QuotaStatus& result) noexcept;

    static std::wstring MakeProtectCutoffText_(
        std::uint32_t protectRecentSec) noexcept;

    bool ScanRootSegmentsLocked_(
        std::vector<RootSegmentInfo>& segments,
        QuotaStatus& result) const noexcept;

    void CleanupDeleteLeftoversLocked_(QuotaStatus& result) const noexcept;

    bool DeleteRootSegmentLocked_(
        const RootSegmentInfo& seg,
        QuotaStatus& result) noexcept;

    void RefreshManifestsAfterRootDeleteLocked_(
        const std::vector<std::filesystem::path>& touchedFolders,
        QuotaStatus& result) noexcept;

    bool RebuildManifestFromFolderLocked_(
        const std::filesystem::path& folder) noexcept;

    static bool IsSegmentTsPath_(const std::filesystem::path& p);
    static std::wstring ParseStartTextFromSegmentName_(const std::filesystem::path& p);
    static std::wstring MakePathKey_(const std::filesystem::path& p);

    std::uint64_t CalcClosedBytesLocked_(const Session& s) const noexcept;
    void SortSegmentsLocked_(Session& s) noexcept;

    static std::uint64_t ParseSequenceFromSegmentName_(const std::filesystem::path& p) noexcept;
    static bool HasPrefix_(const std::wstring& s, const std::wstring& prefix) noexcept;

private:
    mutable std::mutex mutex_{};
    Settings settings_{};

    std::unordered_map<std::wstring, Session> sessions_{};

    std::atomic_bool quotaCleanerRunning_{ false };
    std::atomic_bool quotaCleanerStopRequested_{ false };
    std::atomic_bool quotaCleanupRequested_{ false };

    std::mutex quotaCleanerMutex_{};
    std::condition_variable quotaCleanerCv_{};
    std::thread quotaCleanerThread_{};
};

#endif // TIMESHIFT_SEGMENT_STORE_HPP
