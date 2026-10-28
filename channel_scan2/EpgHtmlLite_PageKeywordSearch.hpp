
#pragma once
#ifndef EPG_HTML_LITE_PAGE_KEYWORD_SEARCH_HPP
#define EPG_HTML_LITE_PAGE_KEYWORD_SEARCH_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "EpgHtmlLite.hpp"

namespace EpgHtmlLite::PageKeywordSearch
{
    bool Generate(
        const Context& ctx,
        Result& ioResult);
}

#endif // EPG_HTML_LITE_PAGE_KEYWORD_SEARCH_HPP