#pragma once
#ifndef EPG_HTML_LITE_PAGE_NOW_TIMETABLE_HPP
#define EPG_HTML_LITE_PAGE_NOW_TIMETABLE_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "EpgHtmlLite.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace EpgHtmlLite::PageNowTimetable
{
    struct BlockPageRequest
    {
        std::filesystem::path outputPath;
        std::wstring pageTitle;
        std::wstring dateKey;
        int block = 0;
        int space = 0;

        std::vector<const ProgramEntry*> rows;

        std::function<std::wstring(const ProgramEntry&)> resolveServiceName;
        std::function<bool(const ProgramEntry&)> isSubchannel;

        // 末尾追加にして、既存の集成体初期化との互換性を維持する。
        bool enableAutoReserveAssist = true;
        std::wstring autoReserveEntryUrl;
    };

    bool Generate(const Context& ctx, Result& ioResult);

    bool GenerateBlockPage(
        const BlockPageRequest& request,
        Result& ioResult);
}

#endif // EPG_HTML_LITE_PAGE_NOW_TIMETABLE_HPP
