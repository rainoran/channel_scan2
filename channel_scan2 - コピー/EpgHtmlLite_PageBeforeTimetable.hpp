#pragma once
#ifndef EPG_HTML_LITE_PAGE_BEFORE_TIMETABLE_HPP
#define EPG_HTML_LITE_PAGE_BEFORE_TIMETABLE_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "EpgHtmlLite.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace EpgHtmlLite::PageBeforeTimetable
{
    struct DatePageRequest
    {
        std::filesystem::path outputPath;
        std::wstring pageTitle;
        std::wstring dateKey;

        // 0=04-10 / 1=10-16 / 2=16-22 / 3=22-04
        int block = 0;
        int space = 0;

        std::vector<const ProgramEntry*> rows;

        std::function<std::wstring(const ProgramEntry&)> resolveServiceName;
        std::function<bool(const ProgramEntry&)> isSubchannel;
    };

    bool Generate(const Context& ctx, Result& ioResult);

    bool GenerateDatePage(
        const DatePageRequest& request,
        Result& ioResult);
}

#endif // EPG_HTML_LITE_PAGE_BEFORE_TIMETABLE_HPP