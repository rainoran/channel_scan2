#pragma once
#pragma once
#ifndef EPG_HTML_LITE_INDEX_HPP
#define EPG_HTML_LITE_INDEX_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "EpgHtmlLite.hpp"

namespace EpgHtmlLite::Index
{
    std::vector<MenuLink> BuildDefaultMenuLinks(int space);

    bool GenerateIndexBundle(
        const Context& ctx,
        Result& ioResult);
}

#endif // EPG_HTML_LITE_INDEX_HPP