#pragma once
#ifndef EPG_HTML_LITE_LOG_HPP
#define EPG_HTML_LITE_LOG_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "EpgHtmlLite.hpp"

namespace EpgHtmlLite::Log
{
    // system.log の内容はHTMLへ埋め込まず、生成したJavaScriptが
    // /api/system/log から随時取得する。
    bool Generate(
        const Context& ctx,
        Result& ioResult);
}

#endif // EPG_HTML_LITE_LOG_HPP
