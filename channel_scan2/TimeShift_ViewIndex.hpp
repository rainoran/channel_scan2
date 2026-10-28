#pragma once
#ifndef TIMESHIFT_VIEW_INDEX_HPP
#define TIMESHIFT_VIEW_INDEX_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class TimeShift_ViewIndex final
{
public:
    struct SegmentEntry
    {
        std::wstring channelKey;
        int space = 0;
        int channel = -1;

        std::uint64_t sequence = 0;

        std::wstring startText;
        std::wstring endText;

        std::uint64_t startTick = 0;
        std::uint64_t endTick = 0;

        std::wstring status;

        std::filesystem::path filePath;
        std::uint64_t bytes = 0;

        bool closed = false;
        bool copyAllowed = false;
    };

    struct ChannelEntry
    {
        std::wstring channelKey;
        int space = 0;
        int channel = -1;

        std::filesystem::path folder;
        std::uint64_t totalBytes = 0;
        std::size_t segmentCount = 0;

        std::wstring firstStartText;
        std::wstring lastStartText;
        std::wstring status;
    };

    // 既存EPG側から渡す中間番組データ。
    // REGZA風の「過去番組表」はこの番組単位を基準にする。
    struct ProgramSourceEntry
    {
        int space = 0;
        int channel = -1;

        std::uint16_t serviceId = 0;
        std::uint16_t eventId = 0;
        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;

        std::wstring serviceName;
        std::wstring title;

        // YYYY-MM-DD HH:MM:SS
        std::wstring startText;
        std::wstring endText;
    };

    struct JoinSegment
    {
        std::filesystem::path filePath;

        std::wstring startText;
        std::wstring endText;

        std::uint64_t startTick = 0;
        std::uint64_t endTick = 0;

        std::uint64_t bytes = 0;
        std::uint64_t sequence = 0;

        bool closed = false;
    };

    struct ProgramEntry
    {
        std::wstring programKey;
        std::wstring channelKey;

        int space = 0;
        int channel = -1;

        std::uint16_t serviceId = 0;
        std::uint16_t eventId = 0;
        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;

        std::wstring serviceName;
        std::wstring title;

        std::wstring startText;
        std::wstring endText;

        std::uint64_t startTick = 0;
        std::uint64_t endTick = 0;

        std::uint32_t durationSec = 0;
        std::uint32_t coveredSec = 0;
        std::uint32_t coveragePermille = 0;

        // 欠損診断。
        // 1000/1000判定そのものは従来どおり維持する。
        std::uint32_t missingSec = 0;
        std::uint32_t maxGapSec = 0;
        std::wstring firstMissingStartText;
        std::wstring firstMissingEndText;

        // 番組境界診断。
        // 1分セグメント方式では、番組開始/終了の数秒ズレで
        // coverage=999/1000 になることがある。
        // これは copyAllowed 判定には使わず、理由表示と調査にだけ使う。
        std::uint32_t boundaryProbeBeforeCount = 0;
        std::uint32_t boundaryProbeAfterCount = 0;
        bool boundaryProbeMayBeEdgeGap = false;
        std::wstring boundaryProbeBeforeText;
        std::wstring boundaryProbeAfterText;

        // 結合保護。
// 番組本体の100%判定には使わない。
// ExportJoinedTs() で作るTSに、番組開始前/終了後のセグメントを
// 保険として含めるための情報。
        std::uint32_t joinPaddingBeforeCount = 0;
        std::uint32_t joinPaddingAfterCount = 0;
        std::wstring joinPlanStartText;
        std::wstring joinPlanEndText;

        std::uint64_t totalBytes = 0;
        std::size_t segmentCount = 0;

        // Available / Partial / Recording / Deleted / Unavailable
        std::wstring status;

        bool viewAllowed = false;
        bool copyAllowed = false;

        std::wstring reason;

        // 番組を構成するセグメントの結合計画。
        std::vector<JoinSegment> joinSegments;
    };

    struct ProgramBuildOptions
    {
        // TimeShift_SegmentStore の保存セグメント秒数。
        std::uint32_t segmentSeconds = 60u;

        // 欠損診断用。
        // 1なら番組開始前1セグメント、番組終了後1セグメントまで確認する。
        // copyAllowed 判定には使わない。
        std::uint32_t boundaryProbeSegmentCount = 1u;

        // 結合保護用。
// 1なら ExportJoinedTs() 用の joinSegments に
// 番組開始前1セグメント、番組終了後1セグメントを追加する。
//
// 重要:
// これは coveragePermille / copyAllowed 判定には使わない。
// 番組境界を跨ぐ1分セグメントの取りこぼし防止だけに使う。
        std::uint32_t joinPaddingSegmentCount = 1u;

        // 1000 = 100%。
        // REGZA風のコピー可判定では基本1000。
        std::uint32_t requiredCoveragePermille = 1000u;

        // trueなら現在保存中に重なる番組も Recording として表示する。
        // ただし閲覧・コピーは不可。
        bool includeRecording = true;
    };

    struct JoinExportOptions
    {
        // 空なら joinSegments[0] から timeshift root を推定して
        // rootFolder/cache/join_work を使う。
        std::filesystem::path cacheRoot;

        // 同一ボリュームならハードリンクで退避する。
        // 失敗した場合は CopyFileW にフォールバックする。
        bool useHardLink = true;

        // 成功時に作業キャッシュを削除する。
        bool deleteCacheOnSuccess = true;

        // 失敗時も作業キャッシュを削除する。
        // 調査したい場合は false にする。
        bool deleteCacheOnFailure = false;
    };

    struct BuildResult
    {
        bool ok = false;
        std::filesystem::path rootFolder;
        std::wstring reason;

        std::vector<ChannelEntry> channels;
        std::vector<SegmentEntry> segments;

        // REGZA風過去番組表の本体。
        std::vector<ProgramEntry> programs;
    };

public:
    static BuildResult Build(
        const std::filesystem::path& rootFolder,
        std::uint32_t segmentSeconds = 60u);

    static void BuildProgramsFromSources(
        BuildResult& index,
        const std::vector<ProgramSourceEntry>& programs,
        const ProgramBuildOptions& options);

    static bool ExportJoinedTs(
        const ProgramEntry& program,
        const std::filesystem::path& outputPath,
        const JoinExportOptions& options,
        std::wstring& reason) noexcept;

    static bool ExportJoinedTs(
        const ProgramEntry& program,
        const std::filesystem::path& outputPath,
        std::wstring& reason) noexcept;

    static std::wstring FormatBytes(std::uint64_t bytes);
    static std::wstring MakeChannelKey(int space, int channel);

private:
    static bool HasPrefix_(const std::wstring& s, const std::wstring& prefix) noexcept;
    static bool HasSuffix_(const std::wstring& s, const std::wstring& suffix) noexcept;

    static bool ParseChannelFolderName_(
        const std::wstring& name,
        int& space,
        int& channel);

    static bool ParseSegmentFileName_(
        const std::wstring& fileName,
        std::wstring& startText,
        std::uint64_t& sequence,
        bool& closed);

    static std::wstring NormalizeStartText_(const std::wstring& raw);

    static bool ParseDateTimeText_(
        const std::wstring& text,
        std::uint64_t& tick) noexcept;

    static std::wstring TickToText_(std::uint64_t tick);

    static std::uint64_t AddSeconds_(
        std::uint64_t tick,
        std::uint32_t sec) noexcept;

    static std::wstring MakeProgramKey_(
        const ProgramSourceEntry& src,
        std::uint64_t startTick);

    static void BuildJoinPlan_(
        const BuildResult& index,
        const ProgramBuildOptions& options,
        ProgramEntry& program);
};

#endif // TIMESHIFT_VIEW_INDEX_HPP