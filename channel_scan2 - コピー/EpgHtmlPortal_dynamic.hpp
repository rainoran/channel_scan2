#ifndef EPG_HTML_PORTAL_DYNAMIC_HPP
#define EPG_HTML_PORTAL_DYNAMIC_HPP
#pragma once

#include <string_view>

#include "EpgHtmlPortal.hpp"

namespace EpgHtmlPortal_dynamic
{
    bool WriteDynamicAssets(const EpgHtmlPortal::WriteOptions& opt);

    std::wstring BuildRuntimeScriptBlock(
        std::wstring_view jsSpaceData,
        std::wstring_view jsConfigServiceData,
        int initialSpace);
}

#endif // EPG_HTML_PORTAL_DYNAMIC_HPP