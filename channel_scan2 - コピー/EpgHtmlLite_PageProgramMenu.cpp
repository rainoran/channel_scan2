#include "EpgHtmlLite_PageProgramMenu.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <Windows.h>
#endif

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

#include "EpgHtmlLite_Common.hpp"

namespace
{
    using namespace EpgHtmlLite;

    std::wstring HtmlEscape(const std::wstring_view s)
    {
        std::wstring out;
        out.reserve(s.size() + 16);

        for (const wchar_t ch : s) {
            switch (ch) {
            case L'&': out += L"&amp;";  break;
            case L'<': out += L"&lt;";   break;
            case L'>': out += L"&gt;";   break;
            case L'"': out += L"&quot;"; break;
            default:   out.push_back(ch); break;
            }
        }

        return out;
    }

    bool WriteUtf8BomTextFile(const std::filesystem::path& path, const std::wstring& content)
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return false;
        }

        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        ofs.write(reinterpret_cast<const char*>(bom), sizeof(bom));

#ifdef _WIN32
        const int required = ::WideCharToMultiByte(
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

        std::string utf8(static_cast<std::size_t>(required), '\0');

        const int written = ::WideCharToMultiByte(
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

        ofs.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
#else
        for (wchar_t wc : content) {
            ofs.put(static_cast<char>(wc & 0xFF));
        }
#endif

        return ofs.good();
    }

    std::wstring MakeSpaceLabel(const int space)
    {
        switch (space) {
        case 0: return L"地上波";
        case 1: return L"BS";
        case 2: return L"CS";
        default: return L"SPACE " + std::to_wstring(space);
        }
    }

    struct ProgramMenuPageInfo
    {
        Common::GuidePageKind activeKind = Common::GuidePageKind::Unknown;
        std::wstring title;
        std::wstring fileName;
        std::wstring timetableLabel;
        std::wstring timetableFile;
        std::wstring listLabel;
        std::wstring listFile;
        std::wstring description;
    };

    ProgramMenuPageInfo BuildNowProgramMenuInfo(const Context& ctx)
    {
        const std::wstring suffix = L"_space" + std::to_wstring(ctx.space);

        ProgramMenuPageInfo info{};
        info.activeKind = Common::GuidePageKind::NowTimetable;
        info.title = L"番組表メニュー";
        info.fileName = L"epg_lite_program_menu" + suffix + L".html";
        info.timetableLabel = L"現在番組 時間割";
        info.timetableFile = Common::BuildGuidePageFileName(Common::GuidePageKind::NowTimetable, ctx.space);
        info.listLabel = L"現在番組 表一覧";
        info.listFile = Common::BuildGuidePageFileName(Common::GuidePageKind::NowList, ctx.space);
        info.description = L"現在番組の時間割ページと表一覧ページへ分岐するための中間メニュー。";
        return info;
    }

    ProgramMenuPageInfo BuildPastProgramMenuInfo(const Context& ctx)
    {
        const std::wstring suffix = L"_space" + std::to_wstring(ctx.space);

        ProgramMenuPageInfo info{};
        info.activeKind = Common::GuidePageKind::BeforeTimetable;
        info.title = L"過去番組メニュー";
        info.fileName = L"epg_lite_past_program_menu" + suffix + L".html";
        info.timetableLabel = L"過去番組 時間割";
        info.timetableFile = Common::BuildGuidePageFileName(Common::GuidePageKind::BeforeTimetable, ctx.space);
        info.listLabel = L"過去番組 表一覧";
        info.listFile = Common::BuildGuidePageFileName(Common::GuidePageKind::BeforeList, ctx.space);
        info.description = L"過去番組の時間割ページと表一覧ページへ分岐するための中間メニュー。";
        return info;
    }

    std::wstring BuildProgramMenuHtml(const Context& ctx, const ProgramMenuPageInfo& info)
    {
        Common::HeaderContext header{};
        header.title = info.title;
        header.subtitle = L"Lite Program Menu / " + MakeSpaceLabel(ctx.space) + L" / シェル子ページ";

        // 現在番組メニューか過去番組メニューかで active を切り替える
        const MenuId activeMenu =
            (info.activeKind == Common::GuidePageKind::BeforeTimetable ||
                info.activeKind == Common::GuidePageKind::BeforeList)
            ? MenuId::PastPrograms
            : MenuId::ProgramGuide;

        header.mainLinks = Common::BuildMainMenuLinks(ctx.space, activeMenu);
        header.broadcastLinks = Common::BuildBroadcastLinks(info.activeKind, ctx.space);
        header.guideScopeLinks = Common::BuildGuideScopeLinks(ctx.space, info.activeKind);
        header.guideViewLinks = Common::BuildGuideViewLinks(ctx.space, info.activeKind);
        header.timeBlockLinks = Common::BuildTimeBlockLinks(ctx.space, info.activeKind, 0);
        header.genreLinks = Common::BuildGenreLinks(ctx.space, info.activeKind, 0, L"all");

        // 最上段 index は親シェルが持つので下段では出さない
        header.showMainMenuRow = false;

        // 番組表系の操作列は残す
        header.showGuideControlRow = true;
        header.showGenreRow = true;
        std::wostringstream oss;
        oss << LR"HTML(<!DOCTYPE html>
<html lang="ja">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>)HTML" << HtmlEscape(info.title) << LR"HTML(</title>
<style>
)HTML";
        oss << Common::BuildCommonCss();
        oss << LR"CSS(
.page-section{
    background:var(--lite-panel2);
    border:1px solid var(--lite-line);
    border-radius:12px;
    padding:14px 16px;
    margin-bottom:14px;
}
.page-section h2{
    margin:0 0 10px;
    font-size:18px;
}
.page-grid{
    display:grid;
    grid-template-columns:repeat(auto-fit,minmax(220px,1fr));
    gap:12px;
}
.page-card{
    display:block;
    text-decoration:none;
    color:var(--lite-text);
    background:#161d27;
    border:1px solid var(--lite-line);
    border-radius:12px;
    padding:14px;
    min-height:96px;
}
.page-card:hover{
    border-color:var(--lite-accent);
    transform:translateY(-1px);
}
.page-card .title{
    display:block;
    font-size:16px;
    font-weight:700;
    margin-bottom:8px;
}
.page-card .file{
    display:block;
    font-size:12px;
    color:var(--lite-sub);
    word-break:break-all;
}
.page-info{
    margin-top:14px;
    width:100%;
    border-collapse:collapse;
}
.page-info th,.page-info td{
    padding:9px 8px;
    border-top:1px solid var(--lite-line);
    text-align:left;
    vertical-align:top;
    font-size:13px;
}
.page-info th{
    width:150px;
    color:var(--lite-sub);
}
.page-note{
    margin-top:10px;
    color:var(--lite-sub);
    font-size:12px;
    line-height:1.7;
}
)CSS";
        oss << LR"HTML(</style>
</head>
<body>
)HTML";

        oss << Common::BuildLayoutBegin(header);

        oss << LR"HTML(<section class="page-section">
<h2>)HTML" << HtmlEscape(info.title) << LR"HTML(</h2>
<div class="page-grid">
<a class="page-card" href=")HTML" << HtmlEscape(info.timetableFile) << LR"HTML(">
<span class="title">)HTML" << HtmlEscape(info.timetableLabel) << LR"HTML(</span>
<span class="file">)HTML" << HtmlEscape(info.timetableFile) << LR"HTML(</span>
</a>
<a class="page-card" href=")HTML" << HtmlEscape(info.listFile) << LR"HTML(">
<span class="title">)HTML" << HtmlEscape(info.listLabel) << LR"HTML(</span>
<span class="file">)HTML" << HtmlEscape(info.listFile) << LR"HTML(</span>
</a>
</div>

<table class="page-info">
<tr><th>ページ名</th><td>)HTML" << HtmlEscape(info.title) << LR"HTML(</td></tr>
<tr><th>役割</th><td>)HTML" << HtmlEscape(info.description) << LR"HTML(</td></tr>
<tr><th>分岐1</th><td>)HTML" << HtmlEscape(info.timetableLabel) << LR"HTML(</td></tr>
<tr><th>分岐2</th><td>)HTML" << HtmlEscape(info.listLabel) << LR"HTML(</td></tr>
</table>

<div class="page-note">
上段メニューは Common ベースへ統一しました。<br>
時間帯とジャンルのリンクは今は配置先行で、後から実データの block / genre 切替へ差し替える想定です。
</div>
</section>
)HTML";

        oss << Common::BuildLayoutEnd();

        oss << LR"HTML(</body>
</html>
)HTML";

        return oss.str();
    }

    std::wstring BuildLeafPlaceholderHtml(
        const Context& ctx,
        const std::wstring& title,
        const std::wstring& fileName,
        const std::wstring& roleText)
    {
        Common::HeaderContext header{};
        header.title = title;
        header.subtitle = L"Lite Placeholder / " + MakeSpaceLabel(ctx.space) + L" / シェル子ページ";

        // fileName から active メニューを判定
        const MenuId activeMenu =
            (fileName.find(L"before_") != std::wstring::npos ||
                fileName.find(L"past_") != std::wstring::npos)
            ? MenuId::PastPrograms
            : MenuId::ProgramGuide;

        const Common::GuidePageKind pageKind =
            (fileName.find(L"before_timetable") != std::wstring::npos)
            ? Common::GuidePageKind::BeforeTimetable
            : (fileName.find(L"before_list") != std::wstring::npos)
            ? Common::GuidePageKind::BeforeList
            : (fileName.find(L"now_list") != std::wstring::npos)
            ? Common::GuidePageKind::NowList
            : Common::GuidePageKind::NowTimetable;

        header.mainLinks = Common::BuildMainMenuLinks(ctx.space, activeMenu);
        header.broadcastLinks = Common::BuildBroadcastLinks(pageKind, ctx.space);
        header.guideScopeLinks = Common::BuildGuideScopeLinks(ctx.space, pageKind);
        header.guideViewLinks = Common::BuildGuideViewLinks(ctx.space, pageKind);
        header.timeBlockLinks = Common::BuildTimeBlockLinks(ctx.space, pageKind, 0);
        header.genreLinks = Common::BuildGenreLinks(ctx.space, pageKind, 0, L"all");

        header.showMainMenuRow = false;
        header.showGuideControlRow = true;
        header.showGenreRow = true;

        std::wostringstream oss;
        oss << LR"HTML(<!DOCTYPE html>
<html lang="ja">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>)HTML" << HtmlEscape(title) << LR"HTML(</title>
<style>
)HTML";
        oss << Common::BuildCommonCss();
        oss << LR"HTML(</style>
</head>
<body>
)HTML";
        oss << Common::BuildLayoutBegin(header);
        oss << Common::BuildDummyPanelHtml(title, roleText);
        oss << Common::BuildLayoutEnd();
        oss << LR"HTML(</body>
</html>
)HTML";
        return oss.str();
    }

    bool WriteProgramMenuPage(
        const Context& ctx,
        Result& ioResult,
        const ProgramMenuPageInfo& info)
    {
        const std::filesystem::path pagePath =
            ctx.outputDir / std::filesystem::path(info.fileName);

        const std::wstring html = BuildProgramMenuHtml(ctx, info);
        if (!WriteUtf8BomTextFile(pagePath, html)) {
            return false;
        }

        ioResult.generatedFiles.push_back(pagePath.wstring());
        return true;
    }

    bool WriteLeafPlaceholderPage(
        const Context& ctx,
        Result& ioResult,
        const std::wstring& title,
        const std::wstring& fileName,
        const std::wstring& roleText)
    {
        const std::filesystem::path pagePath =
            ctx.outputDir / std::filesystem::path(fileName);

        const std::wstring html = BuildLeafPlaceholderHtml(
            ctx,
            title,
            fileName,
            roleText);

        if (!WriteUtf8BomTextFile(pagePath, html)) {
            return false;
        }

        ioResult.generatedFiles.push_back(pagePath.wstring());
        return true;
    }

    void MarkMenuImplemented(Result& ioResult, const MenuId id)
    {
        for (auto& link : ioResult.menuLinks) {
            if (link.id == id) {
                link.implemented = true;
                return;
            }
        }
    }
}

namespace EpgHtmlLite::PageProgramMenu
{
    bool Generate(const Context& ctx, Result& ioResult)
    {
        const ProgramMenuPageInfo nowInfo =
            BuildNowProgramMenuInfo(ctx);

        const ProgramMenuPageInfo pastInfo =
            BuildPastProgramMenuInfo(ctx);

        if (!WriteProgramMenuPage(
            ctx,
            ioResult,
            nowInfo)) {
            return false;
        }

        if (!WriteProgramMenuPage(
            ctx,
            ioResult,
            pastInfo)) {
            return false;
        }

        MarkMenuImplemented(
            ioResult,
            MenuId::ProgramGuide);

        MarkMenuImplemented(
            ioResult,
            MenuId::PastPrograms);

        return true;
    }
}