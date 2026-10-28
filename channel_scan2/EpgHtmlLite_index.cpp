#include "EpgHtmlLite_index.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <Windows.h>
#endif

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using namespace EpgHtmlLite;

    std::wstring HtmlEscape(
        const std::wstring_view s)
    {
        std::wstring out;
        out.reserve(s.size() + 16);

        for (const wchar_t ch : s) {
            switch (ch) {
            case L'&':
                out += L"&amp;";
                break;
            case L'<':
                out += L"&lt;";
                break;
            case L'>':
                out += L"&gt;";
                break;
            case L'"':
                out += L"&quot;";
                break;
            default:
                out.push_back(ch);
                break;
            }
        }

        return out;
    }

    bool WriteUtf8BomTextFile(
        const std::filesystem::path& path,
        const std::wstring& content)
    {
        std::ofstream ofs(
            path,
            std::ios::binary | std::ios::trunc);

        if (!ofs) {
            return false;
        }

        const unsigned char bom[3] = {
            0xEF,
            0xBB,
            0xBF
        };

        ofs.write(
            reinterpret_cast<const char*>(bom),
            static_cast<std::streamsize>(sizeof(bom)));

#ifdef _WIN32
        if (content.empty()) {
            return ofs.good();
        }

        const int required =
            ::WideCharToMultiByte(
                CP_UTF8,
                0,
                content.c_str(),
                static_cast<int>(content.size()),
                nullptr,
                0,
                nullptr,
                nullptr);

        if (required <= 0) {
            return false;
        }

        std::string utf8(
            static_cast<std::size_t>(required),
            '\0');

        const int written =
            ::WideCharToMultiByte(
                CP_UTF8,
                0,
                content.c_str(),
                static_cast<int>(content.size()),
                utf8.data(),
                required,
                nullptr,
                nullptr);

        if (written != required) {
            return false;
        }

        ofs.write(
            utf8.data(),
            static_cast<std::streamsize>(utf8.size()));
#else
        for (wchar_t wc : content) {
            ofs.put(
                static_cast<char>(wc & 0xFF));
        }
#endif

        return ofs.good();
    }

    std::wstring MakeSpaceLabel(
        const int space)
    {
        switch (space) {
        case 0:
            return L"地上波";
        case 1:
            return L"BS";
        case 2:
            return L"CS";
        default:
            return L"SPACE " + std::to_wstring(space);
        }
    }

    bool IsHandledByChildUnit(
        const MenuId id) noexcept
    {
        switch (id) {
        case MenuId::ProgramGuide:
        case MenuId::ReservationList:
        case MenuId::KeywordSearch:
        case MenuId::AutoKeywordReservationList:
        case MenuId::PastPrograms:
        case MenuId::Log:
            return true;
        default:
            return false;
        }
    }

    bool IsPrimaryShellMenuLink_(
        const MenuId id) noexcept
    {
        switch (id) {
        case MenuId::ProgramGuide:
        case MenuId::ReservationList:
        case MenuId::KeywordSearch:
        case MenuId::AutoKeywordReservationList:
        case MenuId::PastPrograms:
        case MenuId::Log:
            return true;
        default:
            return false;
        }
    }

    std::wstring GetDefaultFrameSource(
        const std::vector<MenuLink>& links)
    {
        for (const auto& link : links) {
            if (link.id == MenuId::ProgramGuide) {
                return link.fileName;
            }
        }

        if (!links.empty()) {
            return links.front().fileName;
        }

        return L"";
    }

    std::wstring BuildShellMenuLinkClass_(
        const MenuLink& link,
        const std::wstring& defaultFrameSrc)
    {
        std::wstring cls = L"index-menu-link";

        if (link.fileName == defaultFrameSrc) {
            cls += L" active";
        }

        if (!link.implemented) {
            cls += L" placeholder";
        }

        return cls;
    }

    void AppendShellMenuLink_(
        std::wostringstream& oss,
        const MenuLink& link,
        const std::wstring& defaultFrameSrc)
    {
        oss << L"<a class=\"";
        oss << BuildShellMenuLinkClass_(
            link,
            defaultFrameSrc);
        oss << L"\" href=\"";
        oss << HtmlEscape(link.fileName);
        oss << L"\" target=\"lite_content_frame\"";
        oss << L" data-shell-menu-link=\"1\"";
        oss << L" data-menu-id=\"";
        oss << static_cast<int>(link.id);
        oss << L"\">";
        oss << HtmlEscape(link.label);
        oss << L"</a>\n";
    }

    bool HasDevelopmentLinks_(
        const std::vector<MenuLink>& links) noexcept
    {
        for (const auto& link : links) {
            if (!IsPrimaryShellMenuLink_(link.id)) {
                return true;
            }
        }

        return false;
    }

    static std::wstring BuildIndexHtml(
        const Context& ctx,
        const std::vector<MenuLink>& links)
    {
        const std::wstring defaultFrameSrc =
            GetDefaultFrameSource(links);

        std::wostringstream oss;

        oss << L"<!DOCTYPE html>\n";
        oss << L"<html lang=\"ja\">\n";
        oss << L"<head>\n";
        oss << L"<meta charset=\"UTF-8\">\n";
        oss << L"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
        oss << L"<title>";
        oss << HtmlEscape(ctx.title);
        oss << L" - Lite Shell</title>\n";

        oss << L"<style>\n";

        oss << L":root{\n";
        oss << L"    --bg:#0f1115;\n";
        oss << L"    --header:#11161d;\n";
        oss << L"    --panel:#171c24;\n";
        oss << L"    --line:#2c3442;\n";
        oss << L"    --text:#e8edf5;\n";
        oss << L"    --sub:#aab6c8;\n";
        oss << L"    --accent:#4da3ff;\n";
        oss << L"    --active-bg:#1d4d7f;\n";
        oss << L"    --active-line:#6bb7ff;\n";
        oss << L"    --todo-bg:#2a2d33;\n";
        oss << L"    --todo-line:#5d6572;\n";
        oss << L"}\n";

        oss << L"*{\n";
        oss << L"    box-sizing:border-box;\n";
        oss << L"}\n";

        oss << L"html,body{\n";
        oss << L"    margin:0;\n";
        oss << L"    width:100%;\n";
        oss << L"    height:100%;\n";
        oss << L"}\n";

        oss << L"body{\n";
        oss << L"    font-family:\"Segoe UI\",\"Yu Gothic UI\",\"Meiryo\",sans-serif;\n";
        oss << L"    background:var(--bg);\n";
        oss << L"    color:var(--text);\n";
        oss << L"}\n";

        oss << L".shell{\n";
        oss << L"    height:100vh;\n";
        oss << L"    height:100dvh;\n";
        oss << L"    min-height:0;\n";
        oss << L"    display:grid;\n";
        oss << L"    grid-template-rows:auto minmax(0,1fr);\n";
        oss << L"}\n";

        oss << L".shell-header{\n";
        oss << L"    position:sticky;\n";
        oss << L"    top:0;\n";
        oss << L"    z-index:300;\n";
        oss << L"    background:var(--header);\n";
        oss << L"    border-bottom:1px solid var(--line);\n";
        oss << L"    box-shadow:0 4px 12px rgba(0,0,0,0.28);\n";
        oss << L"    padding:6px 10px 7px;\n";
        oss << L"}\n";

        oss << L".function-menu-wrap{\n";
        oss << L"    display:flex;\n";
        oss << L"    align-items:center;\n";
        oss << L"    gap:8px;\n";
        oss << L"    min-width:0;\n";
        oss << L"}\n";

        oss << L".function-menu-label{\n";
        oss << L"    flex:0 0 auto;\n";
        oss << L"    color:var(--sub);\n";
        oss << L"    font-size:11px;\n";
        oss << L"    font-weight:700;\n";
        oss << L"    letter-spacing:0.04em;\n";
        oss << L"    white-space:nowrap;\n";
        oss << L"}\n";

        oss << L".index-menu-strip{\n";
        oss << L"    flex:1 1 auto;\n";
        oss << L"    min-width:0;\n";
        oss << L"    overflow-x:auto;\n";
        oss << L"    overflow-y:hidden;\n";
        oss << L"    padding-bottom:2px;\n";
        oss << L"    scrollbar-width:thin;\n";
        oss << L"    scrollbar-color:#41506a #11161d;\n";
        oss << L"}\n";

        oss << L".index-menu-strip::-webkit-scrollbar{\n";
        oss << L"    height:8px;\n";
        oss << L"}\n";

        oss << L".index-menu-strip::-webkit-scrollbar-track{\n";
        oss << L"    background:#11161d;\n";
        oss << L"}\n";

        oss << L".index-menu-strip::-webkit-scrollbar-thumb{\n";
        oss << L"    background:#41506a;\n";
        oss << L"    border-radius:999px;\n";
        oss << L"}\n";

        oss << L".index-menu-row{\n";
        oss << L"    display:flex;\n";
        oss << L"    flex-wrap:nowrap;\n";
        oss << L"    gap:6px;\n";
        oss << L"    min-width:max-content;\n";
        oss << L"}\n";

        oss << L".index-menu-link{\n";
        oss << L"    flex:0 0 auto;\n";
        oss << L"    display:inline-flex;\n";
        oss << L"    align-items:center;\n";
        oss << L"    justify-content:center;\n";
        oss << L"    min-height:31px;\n";
        oss << L"    padding:5px 12px;\n";
        oss << L"    border-radius:9px;\n";
        oss << L"    border:1px solid #425067;\n";
        oss << L"    background:#1a2330;\n";
        oss << L"    color:var(--text);\n";
        oss << L"    text-decoration:none;\n";
        oss << L"    text-align:center;\n";
        oss << L"    font-size:12px;\n";
        oss << L"    font-weight:700;\n";
        oss << L"    line-height:1.1;\n";
        oss << L"    white-space:nowrap;\n";
        oss << L"    letter-spacing:0;\n";
        oss << L"}\n";

        oss << L".index-menu-link:hover{\n";
        oss << L"    transform:translateY(-1px);\n";
        oss << L"    border-color:var(--accent);\n";
        oss << L"}\n";

        oss << L".index-menu-link.active{\n";
        oss << L"    background:var(--active-bg);\n";
        oss << L"    border-color:var(--active-line);\n";
        oss << L"    color:#eef7ff;\n";
        oss << L"}\n";

        oss << L".index-menu-link.placeholder{\n";
        oss << L"    background:var(--todo-bg);\n";
        oss << L"    border-color:var(--todo-line);\n";
        oss << L"    border-style:dashed;\n";
        oss << L"    color:#d6dbe4;\n";
        oss << L"}\n";

        oss << L".shell-title{\n";
        oss << L"    margin-top:5px;\n";
        oss << L"    display:flex;\n";
        oss << L"    align-items:baseline;\n";
        oss << L"    gap:10px;\n";
        oss << L"    color:var(--sub);\n";
        oss << L"    font-size:11px;\n";
        oss << L"    line-height:1.3;\n";
        oss << L"}\n";

        oss << L".shell-title strong{\n";
        oss << L"    color:var(--text);\n";
        oss << L"    font-size:13px;\n";
        oss << L"}\n";

        oss << L".dev-menu{\n";
        oss << L"    margin-top:5px;\n";
        oss << L"    color:var(--sub);\n";
        oss << L"    font-size:11px;\n";
        oss << L"}\n";

        oss << L".dev-menu summary{\n";
        oss << L"    cursor:pointer;\n";
        oss << L"    user-select:none;\n";
        oss << L"}\n";

        oss << L".dev-menu-row{\n";
        oss << L"    display:flex;\n";
        oss << L"    flex-wrap:wrap;\n";
        oss << L"    gap:5px;\n";
        oss << L"    margin-top:5px;\n";
        oss << L"}\n";

        oss << L".dev-menu-row .index-menu-link{\n";
        oss << L"    min-height:25px;\n";
        oss << L"    padding:3px 8px;\n";
        oss << L"    font-size:10px;\n";
        oss << L"}\n";

        oss << L".shell-main{\n";
        oss << L"    min-height:0;\n";
        oss << L"    padding:8px 10px 10px;\n";
        oss << L"    display:flex;\n";
        oss << L"    flex-direction:column;\n";
        oss << L"    overflow:hidden;\n";
        oss << L"}\n";

        oss << L".frame-wrap{\n";
        oss << L"    width:100%;\n";
        oss << L"    height:100%;\n";
        oss << L"    flex:1 1 auto;\n";
        oss << L"    min-height:0;\n";
        oss << L"    background:var(--panel);\n";
        oss << L"    border:1px solid var(--line);\n";
        oss << L"    border-radius:10px;\n";
        oss << L"    overflow:hidden;\n";
        oss << L"}\n";

        oss << L".frame-wrap iframe{\n";
        oss << L"    width:100%;\n";
        oss << L"    height:100%;\n";
        oss << L"    border:0;\n";
        oss << L"    background:#0f131a;\n";
        oss << L"}\n";

        oss << L"@media (max-width: 900px){\n";
        oss << L"    .function-menu-wrap{\n";
        oss << L"        align-items:flex-start;\n";
        oss << L"        flex-direction:column;\n";
        oss << L"        gap:4px;\n";
        oss << L"    }\n";
        oss << L"    .index-menu-strip{\n";
        oss << L"        width:100%;\n";
        oss << L"    }\n";
        oss << L"    .index-menu-link{\n";
        oss << L"        min-height:28px;\n";
        oss << L"        padding:4px 9px;\n";
        oss << L"        font-size:11px;\n";
        oss << L"    }\n";
        oss << L"    .shell-title{\n";
        oss << L"        display:block;\n";
        oss << L"    }\n";

        oss << L"}\n";

        // スマートフォンではメニューを折りたたみ、iframeの表示面積を優先。
        oss << L".mobile-menu-toggle{display:none;}\n";
        oss << L"@media (max-width:700px){\n";
        oss << L"  .shell-header{position:relative;padding:5px 8px;}\n";
        oss << L"  .mobile-menu-toggle{display:flex;align-items:center;justify-content:space-between;gap:8px;width:100%;min-height:38px;padding:6px 12px;border:1px solid var(--line);border-radius:8px;background:#1a2330;color:var(--text);font-family:inherit;font-size:13px;font-weight:700;cursor:pointer;}\n";
        oss << L"  .function-menu-wrap{display:none;margin-top:6px;}\n";
        oss << L"  .shell-header.menu-open .function-menu-wrap{display:flex;}\n";
        oss << L"  .index-menu-strip{overflow:visible;max-height:38vh;overflow-y:auto;}\n";
        oss << L"  .index-menu-row{min-width:0;flex-wrap:wrap;}\n";
        oss << L"  .shell-title{display:none;}\n";
        oss << L"  .shell-header .dev-menu{display:none;}\n";
        oss << L"  .shell-header.menu-open .dev-menu{display:block;}\n";
        oss << L"  .shell-main{padding:3px;}\n";
        oss << L"  .frame-wrap{border-radius:5px;}\n";
        oss << L"  .frame-wrap iframe{display:block;}\n";
        oss << L"}\n";
        oss << L"@media (max-height:480px) and (max-width:900px){\n";
        oss << L"  .index-menu-strip{max-height:36vh;}\n";
        oss << L"}\n";

        oss << L"</style>\n";
        oss << L"</head>\n";
        oss << L"<body>\n";

        oss << L"<div class=\"shell\">\n";
        oss << L"<header class=\"shell-header\" id=\"lite_shell_header\">\n";
        oss << L"<button type=\"button\" class=\"mobile-menu-toggle\" id=\"lite_menu_toggle\" aria-expanded=\"false\" aria-controls=\"lite_function_menu\">機能メニュー <span aria-hidden=\"true\">☰</span></button>\n";

        oss << L"<nav class=\"function-menu-wrap\" id=\"lite_function_menu\" aria-label=\"機能切り替え\">\n";
        oss << L"<div class=\"function-menu-label\">機能</div>\n";
        oss << L"<div class=\"index-menu-strip\">\n";
        oss << L"<div class=\"index-menu-row\">\n";

        for (const auto& link : links) {
            if (!IsPrimaryShellMenuLink_(link.id)) {
                continue;
            }

            AppendShellMenuLink_(
                oss,
                link,
                defaultFrameSrc);
        }

        oss << L"</div>\n";
        oss << L"</div>\n";
        oss << L"</nav>\n";

        oss << L"<div class=\"shell-title\">\n";
        oss << L"<strong>";
        oss << HtmlEscape(ctx.title);
        oss << L"</strong>\n";
        oss << L"<span>Lite Shell / ";
        oss << HtmlEscape(MakeSpaceLabel(ctx.space));
        oss << L" / 最上段固定メニュー + 下段iframe表示</span>\n";
        oss << L"</div>\n";

        if (HasDevelopmentLinks_(links)) {
            oss << L"<details class=\"dev-menu\">\n";
            oss << L"<summary>開発中ページ</summary>\n";
            oss << L"<div class=\"dev-menu-row\">\n";

            for (const auto& link : links) {
                if (IsPrimaryShellMenuLink_(link.id)) {
                    continue;
                }

                AppendShellMenuLink_(
                    oss,
                    link,
                    defaultFrameSrc);
            }

            oss << L"</div>\n";
            oss << L"</details>\n";
        }

        oss << L"</header>\n";

        oss << L"<main class=\"shell-main\">\n";
        oss << L"<div class=\"frame-wrap\">\n";
        oss << L"<iframe id=\"lite_content_frame\"";
        oss << L" name=\"lite_content_frame\"";
        oss << L" title=\"Lite Content Frame\"";
        oss << L" src=\"";
        oss << HtmlEscape(defaultFrameSrc);
        oss << L"\"></iframe>\n";
        oss << L"</div>\n";
        oss << L"</main>\n";
        oss << L"</div>\n";

        oss << L"<script>\n";
        oss << L"window.EPG_RUNTIME = Object.assign({}, window.EPG_RUNTIME || {}, {\n";
        oss << L"    defaultSpace:";
        oss << ctx.space;
        oss << L",\n";
        oss << L"    reservationApiBase:'/api/reservation',\n";
        oss << L"    reservationPreferredTuner:-1,\n";
        oss << L"    reservationSwitchDebug:false\n";
        oss << L"});\n";
        oss << L"</script>\n";

        oss << L"<script>\n";
        oss << L"(function () {\n";
        oss << L"'use strict';\n";

        oss << L"const shellHeader = document.getElementById('lite_shell_header');\n";
        oss << L"const menuToggle = document.getElementById('lite_menu_toggle');\n";
        oss << L"function closeMobileMenu() {\n";
        oss << L"    if (shellHeader) shellHeader.classList.remove('menu-open');\n";
        oss << L"    if (menuToggle) menuToggle.setAttribute('aria-expanded', 'false');\n";
        oss << L"}\n";
        oss << L"if (menuToggle && shellHeader) {\n";
        oss << L"    menuToggle.addEventListener('click', function () {\n";
        oss << L"        const opened = shellHeader.classList.toggle('menu-open');\n";
        oss << L"        menuToggle.setAttribute('aria-expanded', opened ? 'true' : 'false');\n";
        oss << L"    });\n";
        oss << L"}\n";
        oss << L"function setActiveByMenuId(menuId) {\n";
        oss << L"    document.querySelectorAll('[data-shell-menu-link]').forEach(function (item) {\n";
        oss << L"        const itemId = parseInt(item.getAttribute('data-menu-id') || '-1', 10);\n";
        oss << L"        if (itemId === menuId) {\n";
        oss << L"            item.classList.add('active');\n";
        oss << L"        } else {\n";
        oss << L"            item.classList.remove('active');\n";
        oss << L"        }\n";
        oss << L"    });\n";
        oss << L"}\n";

        oss << L"function resolveMenuIdFromFileName(fileName) {\n";
        oss << L"    const lower = (fileName || '').toLowerCase();\n";

        oss << L"    if (lower.indexOf('auto_keyword_reservation_list') !== -1) {\n";
        oss << L"        return ";
        oss << static_cast<int>(MenuId::AutoKeywordReservationList);
        oss << L";\n";
        oss << L"    }\n";

        oss << L"    if (lower.indexOf('keyword_search') !== -1) {\n";
        oss << L"        return ";
        oss << static_cast<int>(MenuId::KeywordSearch);
        oss << L";\n";
        oss << L"    }\n";

        oss << L"    if (lower.indexOf('reservation_list') !== -1) {\n";
        oss << L"        return ";
        oss << static_cast<int>(MenuId::ReservationList);
        oss << L";\n";
        oss << L"    }\n";

        oss << L"    if (lower.indexOf('past_program_menu') !== -1 ||\n";
        oss << L"        lower.indexOf('before_timetable') !== -1 ||\n";
        oss << L"        lower.indexOf('before_list') !== -1) {\n";
        oss << L"        return ";
        oss << static_cast<int>(MenuId::PastPrograms);
        oss << L";\n";
        oss << L"    }\n";

        oss << L"    if (lower.indexOf('program_menu') !== -1 ||\n";
        oss << L"        lower.indexOf('now_timetable') !== -1 ||\n";
        oss << L"        lower.indexOf('now_list') !== -1) {\n";
        oss << L"        return ";
        oss << static_cast<int>(MenuId::ProgramGuide);
        oss << L";\n";
        oss << L"    }\n";

        oss << L"    return -1;\n";
        oss << L"}\n";

        oss << L"document.addEventListener('click', function (event) {\n";
        oss << L"    const link = event.target.closest('[data-shell-menu-link]');\n";
        oss << L"    if (!link) {\n";
        oss << L"        return;\n";
        oss << L"    }\n";
        oss << L"    const menuId = parseInt(link.getAttribute('data-menu-id') || '-1', 10);\n";
        oss << L"    if (menuId >= 0) {\n";
        oss << L"        setActiveByMenuId(menuId);\n";
        oss << L"    }\n";
        oss << L"    closeMobileMenu();\n";
        oss << L"});\n";

        oss << L"const frame = document.getElementById('lite_content_frame');\n";
        oss << L"if (frame) {\n";
        oss << L"    frame.addEventListener('load', function () {\n";
        oss << L"        try {\n";
        oss << L"            const href = frame.contentWindow.location.href || '';\n";
        oss << L"            const fileName = href.split('/').pop() || '';\n";
        oss << L"            const menuId = resolveMenuIdFromFileName(fileName);\n";
        oss << L"            if (menuId >= 0) {\n";
        oss << L"                setActiveByMenuId(menuId);\n";
        oss << L"            }\n";
        oss << L"        } catch (e) {\n";
        oss << L"        }\n";
        oss << L"    });\n";
        oss << L"}\n";

        oss << L"})();\n";
        oss << L"</script>\n";

        oss << L"</body>\n";
        oss << L"</html>\n";

        return oss.str();
    }

    std::wstring BuildPlaceholderHtml(
        const Context& ctx,
        const MenuLink& link)
    {
        std::wostringstream oss;

        oss << L"<!DOCTYPE html>\n";
        oss << L"<html lang=\"ja\">\n";
        oss << L"<head>\n";
        oss << L"<meta charset=\"UTF-8\">\n";
        oss << L"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
        oss << L"<title>";
        oss << HtmlEscape(link.label);
        oss << L"</title>\n";

        oss << L"<style>\n";
        oss << L":root{\n";
        oss << L"    --bg:#10151c;\n";
        oss << L"    --panel:#1a212c;\n";
        oss << L"    --line:#2b3544;\n";
        oss << L"    --text:#ebf0f7;\n";
        oss << L"    --sub:#aeb8c7;\n";
        oss << L"}\n";

        oss << L"*{\n";
        oss << L"    box-sizing:border-box;\n";
        oss << L"}\n";

        oss << L"body{\n";
        oss << L"    margin:0;\n";
        oss << L"    font-family:\"Segoe UI\",\"Yu Gothic UI\",\"Meiryo\",sans-serif;\n";
        oss << L"    background:var(--bg);\n";
        oss << L"    color:var(--text);\n";
        oss << L"}\n";

        oss << L"header{\n";
        oss << L"    padding:16px 20px 12px;\n";
        oss << L"    border-bottom:1px solid var(--line);\n";
        oss << L"    background:#11161d;\n";
        oss << L"}\n";

        oss << L"header h1{\n";
        oss << L"    margin:0 0 6px;\n";
        oss << L"    font-size:24px;\n";
        oss << L"}\n";

        oss << L"header .sub{\n";
        oss << L"    color:var(--sub);\n";
        oss << L"    font-size:13px;\n";
        oss << L"}\n";

        oss << L"main{\n";
        oss << L"    padding:18px;\n";
        oss << L"}\n";

        oss << L".panel{\n";
        oss << L"    background:var(--panel);\n";
        oss << L"    border:1px solid var(--line);\n";
        oss << L"    border-radius:12px;\n";
        oss << L"    padding:16px;\n";
        oss << L"}\n";

        oss << L"table{\n";
        oss << L"    width:100%;\n";
        oss << L"    border-collapse:collapse;\n";
        oss << L"    margin-top:10px;\n";
        oss << L"}\n";

        oss << L"th,td{\n";
        oss << L"    padding:10px 8px;\n";
        oss << L"    border-top:1px solid var(--line);\n";
        oss << L"    text-align:left;\n";
        oss << L"    vertical-align:top;\n";
        oss << L"    font-size:13px;\n";
        oss << L"}\n";

        oss << L"th{\n";
        oss << L"    width:140px;\n";
        oss << L"    color:var(--sub);\n";
        oss << L"}\n";

        oss << L".note{\n";
        oss << L"    margin-top:12px;\n";
        oss << L"    color:var(--sub);\n";
        oss << L"    font-size:12px;\n";
        oss << L"    line-height:1.7;\n";
        oss << L"}\n";

        oss << L"</style>\n";
        oss << L"</head>\n";
        oss << L"<body>\n";

        oss << L"<header>\n";
        oss << L"<h1>";
        oss << HtmlEscape(link.label);
        oss << L"</h1>\n";
        oss << L"<div class=\"sub\">Lite Placeholder / ";
        oss << HtmlEscape(MakeSpaceLabel(ctx.space));
        oss << L"</div>\n";
        oss << L"</header>\n";

        oss << L"<main>\n";
        oss << L"<div class=\"panel\">\n";
        oss << L"<table>\n";

        oss << L"<tr><th>ページ名</th><td>";
        oss << HtmlEscape(link.label);
        oss << L"</td></tr>\n";

        oss << L"<tr><th>出力ファイル</th><td>";
        oss << HtmlEscape(link.fileName);
        oss << L"</td></tr>\n";

        oss << L"<tr><th>状態</th><td>プレースホルダ</td></tr>\n";
        oss << L"<tr><th>次段階</th><td>Lite 子ユニットで個別実装</td></tr>\n";

        oss << L"</table>\n";
        oss << L"<div class=\"note\">\n";
        oss << L"このページはシェルの下段表示枠に読み込まれる仮ページです。<br>\n";
        oss << L"次の段階で専用HTMLへ順次差し替えます。\n";
        oss << L"</div>\n";
        oss << L"</div>\n";
        oss << L"</main>\n";
        oss << L"</body>\n";
        oss << L"</html>\n";

        return oss.str();
    }
}

namespace EpgHtmlLite::Index
{
    std::vector<MenuLink> BuildDefaultMenuLinks(
        const int space)
    {
        const std::wstring suffix =
            L"_space" + std::to_wstring(space);

        std::vector<MenuLink> links;
        links.reserve(11);

        links.push_back({
            MenuId::ProgramGuide,
            L"番組表",
            L"epg_lite_program_menu" + suffix + L".html",
            true
            });

        links.push_back({
            MenuId::ReservationList,
            L"予約リスト",
            L"epg_lite_reservation_list" + suffix + L".html",
            true
            });

        links.push_back({
            MenuId::KeywordSearch,
            L"キーワード検索",
            L"epg_lite_keyword_search" + suffix + L".html",
            true
            });

        links.push_back({
            MenuId::AutoKeywordReservationList,
            L"自動検索予約リスト",
            L"epg_lite_auto_keyword_reservation_list" + suffix + L".html",
            true
            });

        links.push_back({
            MenuId::PastPrograms,
            L"過去番組",
            L"epg_lite_past_program_menu" + suffix + L".html",
            true
            });

        links.push_back({
            MenuId::ReservationEdit,
            L"拡張ページ",
            L"epg_lite_extension" + suffix + L".html",
            false
            });

        links.push_back({
            MenuId::Controller,
            L"コントローラー",
            L"epg_lite_controller" + suffix + L".html",
            false
            });

        links.push_back({
            MenuId::NewPrograms,
            L"新番組",
            L"epg_lite_new_programs" + suffix + L".html",
            false
            });

        links.push_back({
            MenuId::Recommended,
            L"オススメ",
            L"epg_lite_recommended" + suffix + L".html",
            false
            });

        links.push_back({
            MenuId::Movie,
            L"ムービー",
            L"epg_lite_movie" + suffix + L".html",
            false
            });

        links.push_back({
            MenuId::Log,
            L"ログ",
            L"epg_lite_log" + suffix + L".html",
            true
            });

        return links;
    }

    bool GenerateIndexBundle(
        const Context& ctx,
        Result& ioResult)
    {
        ioResult.menuLinks =
            BuildDefaultMenuLinks(
                ctx.space);

        const std::filesystem::path indexPath =
            ctx.outputDir /
            std::filesystem::path(ctx.indexFileName);

        const std::wstring indexHtml =
            BuildIndexHtml(
                ctx,
                ioResult.menuLinks);

        if (!WriteUtf8BomTextFile(
            indexPath,
            indexHtml)) {
            return false;
        }

        ioResult.generatedFiles.push_back(
            indexPath.wstring());

        ioResult.indexPath =
            indexPath.wstring();

        for (const auto& link : ioResult.menuLinks) {
            if (IsHandledByChildUnit(link.id)) {
                continue;
            }

            const std::filesystem::path pagePath =
                ctx.outputDir /
                std::filesystem::path(link.fileName);

            const std::wstring pageHtml =
                BuildPlaceholderHtml(
                    ctx,
                    link);

            if (!WriteUtf8BomTextFile(
                pagePath,
                pageHtml)) {
                return false;
            }

            ioResult.generatedFiles.push_back(
                pagePath.wstring());
        }

        return true;
    }
}
