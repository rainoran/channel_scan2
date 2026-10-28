#pragma once
#ifndef EPG_HTML_PORTAL_SEARCH_DATA_HPP
#define EPG_HTML_PORTAL_SEARCH_DATA_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "AutoReserveRuleStore.hpp"
#include "EpgHtmlPortal.hpp"

namespace EpgHtmlPortal_SearchData
{
    std::vector<AutoReserve::SearchProgram> LoadCurrentAutoReserveSearchPrograms(
        const EpgHtmlPortal::WriteOptions& opt);

    AutoReserve::RuleMatchResult FindAutoReserveMatchesFromCurrentSearchData(
        const EpgHtmlPortal::WriteOptions& opt,
        const std::vector<AutoReserve::Rule>& rules,
        const AutoReserve::RuleMatchOptions& matchOptions);

    AutoReserve::RuleMatchResult RunAutoReserveMatchDryRunFromCurrentSearchData(
        const EpgHtmlPortal::WriteOptions& opt,
        std::size_t maxMatches);

    std::wstring MakeStandardMergedEpgFileName(
        int space);

    std::filesystem::path ResolveMergedEpgPathForSpace(
        const EpgHtmlPortal::WriteOptions& opt,
        int space);

    bool WriteSearchDataJs(
        const EpgHtmlPortal::WriteOptions& opt);
}

#endif // EPG_HTML_PORTAL_SEARCH_DATA_HPP