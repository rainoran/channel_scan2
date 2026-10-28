#pragma once
#ifndef EPG_HTML_LITE_PAGE_BEFORE_LIST_HPP
#define EPG_HTML_LITE_PAGE_BEFORE_LIST_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "EpgHtmlLite.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace EpgHtmlLite::PageBeforeList
{
    struct IndexPageRequest
    {
        std::filesystem::path outputPath;
        std::wstring pageTitle;
        std::vector<std::wstring> dateKeys;
        int space = 0;
    };

    struct DatePageRequest
    {
        std::filesystem::path outputPath;
        std::wstring pageTitle;
        std::wstring dateKey;
        std::vector<const ProgramEntry*> rows;
        int space = 0;

        std::function<std::wstring(const ProgramEntry&)> resolveServiceName;
    };

    bool Generate(const Context& ctx, Result& ioResult);

    bool GenerateIndexPage(
        const IndexPageRequest& request,
        Result& ioResult);

    bool GenerateDatePage(
        const DatePageRequest& request,
        Result& ioResult);
}

#endif // EPG_HTML_LITE_PAGE_BEFORE_LIST_HPP