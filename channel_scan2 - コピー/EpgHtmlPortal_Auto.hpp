#pragma once
#ifndef EPG_HTML_PORTAL_AUTO_HPP
#define EPG_HTML_PORTAL_AUTO_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <filesystem>

#include "EpgHtmlPortal.hpp"

namespace EpgHtmlPortal_Auto
{
    std::filesystem::path MakeAutoReservePagePath(
        const EpgHtmlPortal::WriteOptions& opt);

    bool WriteAutoReservePage(
        const EpgHtmlPortal::WriteOptions& opt);

    std::filesystem::path MakeAutoReserveRuleDetailPagePath(
        const EpgHtmlPortal::WriteOptions& opt);

    bool WriteAutoReserveRuleDetailPage(
        const EpgHtmlPortal::WriteOptions& opt);
}

#endif // EPG_HTML_PORTAL_AUTO_HPP