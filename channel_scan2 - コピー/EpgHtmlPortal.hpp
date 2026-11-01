#pragma once
#ifndef EPG_HTML_PORTAL_HPP
#define EPG_HTML_PORTAL_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace EpgHtmlPortal
{
    struct ServiceEntry
    {
        std::uint16_t serviceId = 0;
        std::wstring serviceName;
    };

    struct WriteOptions
    {
        int space = 0;

        std::filesystem::path outputDir;
        std::filesystem::path indexFileName = L"index.html";
        std::filesystem::path searchDataJsFileName = L"epg_search_data.js";
        std::filesystem::path reservationDataJsFileName = L"reservation_data.js";

        std::filesystem::path currentHtmlPath;
        std::filesystem::path beforeHtmlPath;

        // exe / config.ini と同じ階層を優先探索する
        std::filesystem::path taskIniPath = L"tasks.ini";

        std::wstring portalTitle = L"EPGポータル";
        bool generateBeforeStubIfMissing = true;
        bool generateSpaceStubIfMissing = true;

        // index = space(0:地上波 / 1:BS / 2:CS)
        std::vector<std::vector<ServiceEntry>> enabledServicesBySpace;

        // 任意: 検索データ生成元の merged epg パス
        // index = space(0:地上波 / 1:BS / 2:CS)
        // 未指定または空なら標準名 EpgMerged_spaceX_All_EIT.epg を使う
        std::vector<std::filesystem::path> mergedEpgPathsBySpace;
    };

    // EpgHtmlPortal 専用の親生成リクエスト。
    // 共通OutputPlan化はしない。
    // Portal側のファイル構造はPortal側が独自に決める。
    struct GenerateFromEpgRequest
    {
        int space = 0;

        std::filesystem::path outputDir;

        // 現在・今後番組表用EPG。
        std::filesystem::path mergedEpgPath;

        // 過去番組表分割用EPG。
        // 空なら epg_spaceX_before/ の日付別生成は行わない。
        std::filesystem::path beforeMergedEpgPath;

        std::filesystem::path channelListPath;

        std::wstring title;

        bool generateCurrentLegacy = true;
        bool generateCurrentTimetable = true;
        bool generateCurrentList = true;

        bool generateBeforeLegacy = true;
        bool generateBeforeTimetable = true;
        bool generateBeforeList = true;

        bool generateIndex = false;
        bool generateSearchData = false;
        bool generateReservationData = false;

        bool generatePastArchive = false;

        // 空ならPortal既定名。
        std::wstring currentLegacyFileName;
        std::wstring currentTimetableFileName;
        std::wstring currentListFileName;

        std::wstring beforeLegacyFileName;
        std::wstring beforeTimetableFileName;
        std::wstring beforeListFileName;

        std::wstring indexFileName;
    };

    struct GenerateFromEpgResult
    {
        bool ok = false;

        std::vector<std::filesystem::path> generatedFiles;

        std::filesystem::path currentLegacyPath;
        std::filesystem::path currentTimetablePath;
        std::filesystem::path currentListPath;

        std::filesystem::path beforeLegacyPath;
        std::filesystem::path beforeTimetablePath;
        std::filesystem::path beforeListPath;

        std::filesystem::path indexPath;
    };

    bool EnsurePlaceholderHtml(
        const std::filesystem::path& htmlPath,
        std::wstring_view title,
        std::wstring_view message);

    bool WritePortalIndexHtml(
        const WriteOptions& opt);

    GenerateFromEpgResult GenerateFromEpg(
        const GenerateFromEpgRequest& req);
}

#endif // EPG_HTML_PORTAL_HPP