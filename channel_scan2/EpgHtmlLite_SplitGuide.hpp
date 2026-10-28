#pragma once
#ifndef EPG_HTML_LITE_SPLIT_GUIDE_HPP
#define EPG_HTML_LITE_SPLIT_GUIDE_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "EpgHtmlLite.hpp"

namespace EpgHtmlLite::SplitGuide
{
    bool Generate(const Context& ctx, Result& result);
}

#endif // EPG_HTML_LITE_SPLIT_GUIDE_HPP
