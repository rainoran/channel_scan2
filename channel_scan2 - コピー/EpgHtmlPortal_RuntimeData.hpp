#pragma once
#ifndef EPG_HTML_PORTAL_RUNTIME_DATA_HPP
#define EPG_HTML_PORTAL_RUNTIME_DATA_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "EpgHtmlPortal.hpp"

namespace EpgHtmlPortal_RuntimeData
{
    struct SpaceEntry
    {
        int space = 0;
        std::wstring label;

        // 既存互換用。
        std::filesystem::path currentPath;
        std::filesystem::path beforePath;
        bool hasCurrent = false;
        bool hasBefore = false;

        // 分離HTML用。
        std::filesystem::path currentTimetablePath;
        std::filesystem::path currentListPath;
        std::filesystem::path beforeTimetablePath;
        std::filesystem::path beforeListPath;

        bool hasCurrentTimetable = false;
        bool hasCurrentList = false;
        bool hasBeforeTimetable = false;
        bool hasBeforeList = false;
    };

    std::wstring MakeStandardHtmlFileName(int space);
    std::wstring MakeStandardBeforeHtmlFileName(int space);

    std::wstring MakeStandardTimetableHtmlFileName(int space);
    std::wstring MakeStandardListHtmlFileName(int space);
    std::wstring MakeStandardBeforeTimetableHtmlFileName(int space);
    std::wstring MakeStandardBeforeListHtmlFileName(int space);

    std::wstring MakeSpaceLabel(int space);
    std::wstring MakeMissingMessage(int space, bool before);

    SpaceEntry BuildSpaceEntry(
        const EpgHtmlPortal::WriteOptions& opt,
        int space);

    std::vector<SpaceEntry> BuildKnownSpaces(
        const EpgHtmlPortal::WriteOptions& opt);

    std::wstring BuildSpaceDatasetJs(
        const std::vector<SpaceEntry>& spaces);

    std::wstring BuildConfigServiceDatasetJs(
        const EpgHtmlPortal::WriteOptions& opt);

    std::wstring BuildRuntimeScriptBlock(
        std::wstring_view jsSpaceData,
        std::wstring_view jsConfigServiceData,
        int initialSpace,
        std::wstring_view runtimeScriptFileName = L"epg_portal_runtime.js");
}

#endif // EPG_HTML_PORTAL_RUNTIME_DATA_HPP