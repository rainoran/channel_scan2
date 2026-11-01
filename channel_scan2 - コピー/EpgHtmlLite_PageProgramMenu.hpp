#pragma once
#ifndef EPG_HTML_LITE_PAGE_PROGRAM_MENU_HPP
#define EPG_HTML_LITE_PAGE_PROGRAM_MENU_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "EpgHtmlLite.hpp"

namespace EpgHtmlLite::PageProgramMenu
{
    bool Generate(const Context& ctx, Result& ioResult);
}

#endif // EPG_HTML_LITE_PAGE_PROGRAM_MENU_HPP