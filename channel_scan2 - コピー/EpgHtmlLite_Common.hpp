#pragma once
#ifndef EPG_HTML_LITE_COMMON_HPP
#define EPG_HTML_LITE_COMMON_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "EpgHtmlLite.hpp"

namespace EpgHtmlLite::Common
{
    enum class GuidePageKind : std::uint32_t
    {
        Unknown = 0,
        NowTimetable,
        NowList,
        BeforeTimetable,
        BeforeList
    };

    struct NavLink
    {
        std::wstring label;
        std::wstring href;
        bool active = false;
        bool implemented = true;
    };

    struct HeaderContext
    {
        std::wstring title;
        std::wstring subtitle;

        std::vector<NavLink> mainLinks;        // 最上段 index メニュー
        std::vector<NavLink> broadcastLinks;   // 地上波 / BS / CS
        std::vector<NavLink> guideScopeLinks;  // 現在 / 過去
        std::vector<NavLink> guideViewLinks;   // 時間割 / 表一覧
        std::vector<NavLink> timeBlockLinks;   // 04-10 等
        std::vector<NavLink> genreLinks;       // ジャンル

        bool showMainMenuRow = true;
        bool showGuideControlRow = true;
        bool showGenreRow = true;
    };

    std::wstring BuildGuidePageFileName(GuidePageKind kind, int space);
    std::wstring BuildAutoReserveEntryFileName(int space);
    std::wstring BuildAutoReserveEntryUrl(
        std::wstring_view programTitle,
        std::wstring_view configuredEntryUrl,
        int space);

    std::vector<NavLink> BuildMainMenuLinks(int space, MenuId activeMenu);
    std::vector<NavLink> BuildBroadcastLinks(GuidePageKind kind, int activeSpace);
    std::vector<NavLink> BuildGuideScopeLinks(int space, GuidePageKind activeKind);
    std::vector<NavLink> BuildGuideViewLinks(int space, GuidePageKind activeKind);
    std::vector<NavLink> BuildTimeBlockLinks(int space, GuidePageKind activeKind, int activeBlock);
    std::vector<NavLink> BuildGenreLinks(int space, GuidePageKind activeKind, int activeBlock, std::wstring_view activeGenreKey);

    const std::array<std::wstring_view, 12>& GetGenreMajorNames();

    std::wstring NormalizeGenreMajorName(std::wstring_view genreText);

    std::wstring BuildCommonCss();
    std::wstring BuildLayoutBegin(const HeaderContext& ctx);
    std::wstring BuildLayoutEnd();
    std::wstring BuildDummyPanelHtml(const std::wstring& title, const std::wstring& description);
}

#endif // EPG_HTML_LITE_COMMON_HPP
