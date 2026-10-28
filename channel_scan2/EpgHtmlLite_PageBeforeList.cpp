#include "EpgHtmlLite_PageBeforeList.hpp"
#include "EpgHtmlLite_Common.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
    using EpgHtmlLite::ProgramEntry;

    std::wstring HtmlEscape_(
        std::wstring_view s)
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
                if (ch == L'\r' ||
                    ch == L'\n' ||
                    ch == L'\t') {
                    out.push_back(L' ');
                }
                else {
                    out.push_back(ch);
                }
                break;
            }
        }

        return out;
    }

    void AppendUtf8CodePoint_(
        std::string& out,
        std::uint32_t cp)
    {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
            return;
        }

        if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            return;
        }

        if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            return;
        }

        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }

    std::string WideToUtf8_(
        std::wstring_view s)
    {
        std::string out;
        out.reserve(s.size() * 3 + 8);

        for (std::size_t i = 0; i < s.size(); ++i) {
            std::uint32_t cp =
                static_cast<std::uint32_t>(s[i]);

#if WCHAR_MAX <= 0xFFFF
            if (cp >= 0xD800 &&
                cp <= 0xDBFF &&
                (i + 1) < s.size()) {
                const std::uint32_t lo =
                    static_cast<std::uint32_t>(s[i + 1]);

                if (lo >= 0xDC00 &&
                    lo <= 0xDFFF) {
                    cp =
                        0x10000 +
                        (((cp - 0xD800) << 10) |
                            (lo - 0xDC00));
                    ++i;
                }
            }
#endif

            AppendUtf8CodePoint_(out, cp);
        }

        return out;
    }

    bool WriteUtf8File_(
        const std::filesystem::path& path,
        const std::wstring& text)
    {
        std::error_code ec;
        std::filesystem::create_directories(
            path.parent_path(),
            ec);

        if (ec) {
            return false;
        }

        std::ofstream ofs(
            path,
            std::ios::binary | std::ios::trunc);

        if (!ofs) {
            return false;
        }

        const std::string bytes =
            WideToUtf8_(text);

        if (!bytes.empty()) {
            ofs.write(
                bytes.data(),
                static_cast<std::streamsize>(bytes.size()));
        }

        ofs.flush();
        return ofs.good();
    }

    std::wstring DateLabel_(
        const std::wstring& key)
    {
        if (key.size() == 8) {
            return key.substr(0, 4) +
                L"/" +
                key.substr(4, 2) +
                L"/" +
                key.substr(6, 2);
        }

        return key;
    }

    std::wstring ClipText_(
        std::wstring s,
        std::size_t maxLen)
    {
        if (maxLen == 0) {
            return {};
        }

        if (s.size() <= maxLen) {
            return s;
        }

        s.resize(maxLen);
        s += L"…";
        return s;
    }

    std::wstring FormatMinuteOfDayText_(
        int minute)
    {
        if (minute < 0) {
            return L"--:--";
        }

        minute %= 24 * 60;

        if (minute < 0) {
            minute += 24 * 60;
        }

        wchar_t buf[16]{};
        swprintf_s(
            buf,
            L"%02d:%02d",
            minute / 60,
            minute % 60);

        return std::wstring(buf);
    }

    bool IsInvalidTimeText_(
        const std::wstring& s)
    {
        std::wstring t = s;

        auto isSpace = [](wchar_t ch) noexcept {
            return ch == L' ' ||
                ch == L'\t' ||
                ch == L'\r' ||
                ch == L'\n' ||
                ch == L'　';
            };

        while (!t.empty() && isSpace(t.front())) {
            t.erase(t.begin());
        }

        while (!t.empty() && isSpace(t.back())) {
            t.pop_back();
        }

        if (t.empty()) {
            return true;
        }

        return t == L"--:--" ||
            t == L"--" ||
            t == L"-" ||
            t == L"??:??";
    }

    std::wstring PickStartTimeText_(
        const ProgramEntry& e)
    {
        if (e.startMinutesOfDay >= 0) {
            return FormatMinuteOfDayText_(e.startMinutesOfDay);
        }

        if (!IsInvalidTimeText_(e.startText)) {
            return e.startText;
        }

        if (!IsInvalidTimeText_(e.startTime)) {
            return e.startTime;
        }

        return L"--:--";
    }

    std::wstring PickEndTimeText_(
        const ProgramEntry& e)
    {
        if (e.endMinutesOfDay >= 0) {
            return FormatMinuteOfDayText_(e.endMinutesOfDay);
        }

        if (!IsInvalidTimeText_(e.endText)) {
            return e.endText;
        }

        if (!IsInvalidTimeText_(e.endTime)) {
            return e.endTime;
        }

        return L"--:--";
    }

    std::wstring TimeRangeText_(
        const ProgramEntry& e)
    {
        const std::wstring start =
            PickStartTimeText_(e);

        const std::wstring end =
            PickEndTimeText_(e);

        if (start == L"--:--" && end == L"--:--") {
            return L"--:--";
        }

        return start + L"～" + end;
    }

    std::wstring ResolveServiceName_(
        const EpgHtmlLite::PageBeforeList::DatePageRequest& request,
        const ProgramEntry& e)
    {
        if (request.resolveServiceName) {
            const std::wstring name =
                request.resolveServiceName(e);

            if (!name.empty()) {
                return name;
            }
        }

        if (!e.serviceName.empty()) {
            return e.serviceName;
        }

        return L"SID " +
            std::to_wstring(
                static_cast<unsigned int>(e.serviceId));
    }

    std::vector<const ProgramEntry*> SortRows_(
        const std::vector<const ProgramEntry*>& rows)
    {
        std::vector<const ProgramEntry*> sorted = rows;

        std::sort(
            sorted.begin(),
            sorted.end(),
            [](const ProgramEntry* a, const ProgramEntry* b) {
                if (!a || !b) {
                    return a < b;
                }

                if (a->startMinutesOfDay != b->startMinutesOfDay) {
                    return a->startMinutesOfDay < b->startMinutesOfDay;
                }

                if (a->serviceName != b->serviceName) {
                    return a->serviceName < b->serviceName;
                }

                if (a->sortOrder != b->sortOrder) {
                    return a->sortOrder < b->sortOrder;
                }

                if (a->title != b->title) {
                    return a->title < b->title;
                }

                return a->eventId < b->eventId;
            });

        return sorted;
    }

    void AppendBeforeListCss_(
        std::wostringstream& oss)
    {
        oss << L".before-date-grid{\n";
        oss << L"  display:grid;\n";
        oss << L"  grid-template-columns:repeat(auto-fill,minmax(180px,1fr));\n";
        oss << L"  gap:8px;\n";
        oss << L"}\n";

        oss << L".before-date-link{\n";
        oss << L"  display:block;\n";
        oss << L"  border:1px solid #333;\n";
        oss << L"  background:#181818;\n";
        oss << L"  padding:9px 10px;\n";
        oss << L"  color:#9cf;\n";
        oss << L"  text-decoration:none;\n";
        oss << L"}\n";

        oss << L".before-date-link:hover{\n";
        oss << L"  background:#243044;\n";
        oss << L"}\n";

        oss << L".before-list-table{\n";
        oss << L"  width:100%;\n";
        oss << L"  min-width:880px;\n";
        oss << L"  border-collapse:collapse;\n";
        oss << L"  table-layout:fixed;\n";
        oss << L"}\n";

        oss << L".before-list-table th,\n";
        oss << L".before-list-table td{\n";
        oss << L"  border:1px solid #303846;\n";
        oss << L"  padding:6px 7px;\n";
        oss << L"  vertical-align:top;\n";
        oss << L"}\n";

        oss << L".before-list-table th{\n";
        oss << L"  background:#202020;\n";
        oss << L"  color:#fff;\n";
        oss << L"}\n";

        oss << L".before-list-table tbody tr:nth-child(even) td{\n";
        oss << L"  background:#171717;\n";
        oss << L"}\n";

        oss << L".before-list-table tbody tr:hover td{\n";
        oss << L"  background:#243044!important;\n";
        oss << L"}\n";

        oss << L".before-list-wrap{\n";
        oss << L"  overflow:auto;\n";
        oss << L"  border:1px solid #303846;\n";
        oss << L"  background:#111;\n";
        oss << L"}\n";

        oss << L".c-time{width:8em;}\n";
        oss << L".c-svc{width:14em;}\n";
        oss << L".c-title{width:auto;}\n";

        oss << L".program-title{\n";
        oss << L"  display:block;\n";
        oss << L"  font-weight:bold;\n";
        oss << L"  color:#fff;\n";
        oss << L"  line-height:1.36;\n";
        oss << L"  overflow-wrap:anywhere;\n";
        oss << L"}\n";

        oss << L".program-sub{\n";
        oss << L"  display:block;\n";
        oss << L"  color:#cfd6e0;\n";
        oss << L"  font-size:11px;\n";
        oss << L"  line-height:1.45;\n";
        oss << L"  margin-top:4px;\n";
        oss << L"  overflow-wrap:anywhere;\n";
        oss << L"}\n";

        oss << L".genre{\n";
        oss << L"  color:#8fb;\n";
        oss << L"}\n";

        oss << L".page-note{\n";
        oss << L"  color:#aab6c8;\n";
        oss << L"  font-size:12px;\n";
        oss << L"  margin:0 0 8px;\n";
        oss << L"}\n";
    }

    EpgHtmlLite::Common::HeaderContext BuildHeader_(
        const std::wstring& title,
        const std::wstring& subtitle,
        int space)
    {
        EpgHtmlLite::Common::HeaderContext header{};
        header.title = title;
        header.subtitle = subtitle;

        header.mainLinks =
            EpgHtmlLite::Common::BuildMainMenuLinks(
                space,
                EpgHtmlLite::MenuId::PastPrograms);

        header.broadcastLinks =
            EpgHtmlLite::Common::BuildBroadcastLinks(
                EpgHtmlLite::Common::GuidePageKind::BeforeList,
                space);

        header.guideScopeLinks =
            EpgHtmlLite::Common::BuildGuideScopeLinks(
                space,
                EpgHtmlLite::Common::GuidePageKind::BeforeList);

        header.guideViewLinks =
            EpgHtmlLite::Common::BuildGuideViewLinks(
                space,
                EpgHtmlLite::Common::GuidePageKind::BeforeList);

        header.timeBlockLinks =
            EpgHtmlLite::Common::BuildTimeBlockLinks(
                space,
                EpgHtmlLite::Common::GuidePageKind::BeforeList,
                0);

        header.genreLinks =
            EpgHtmlLite::Common::BuildGenreLinks(
                space,
                EpgHtmlLite::Common::GuidePageKind::BeforeList,
                0,
                L"all");

        header.showMainMenuRow = true;
        header.showGuideControlRow = true;
        header.showGenreRow = true;

        return header;
    }

    std::wstring BuildIndexHtml_(
        const EpgHtmlLite::PageBeforeList::IndexPageRequest& request)
    {
        const std::wstring title =
            request.pageTitle.empty()
            ? L"EPG Lite 過去アーカイブ"
            : request.pageTitle;

        const EpgHtmlLite::Common::HeaderContext header =
            BuildHeader_(
                title,
                L"過去アーカイブ / 日付選択",
                request.space);

        std::wostringstream oss;

        oss << L"<!doctype html>\n";
        oss << L"<html lang=\"ja\">\n";
        oss << L"<head>\n";
        oss << L"<meta charset=\"utf-8\">\n";
        oss << L"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n";
        oss << L"<title>" << HtmlEscape_(title) << L"</title>\n";
        oss << L"<style>\n";
        oss << EpgHtmlLite::Common::BuildCommonCss();
        AppendBeforeListCss_(oss);
        oss << L"</style>\n";
        oss << L"</head>\n";
        oss << L"<body>\n";

        oss << EpgHtmlLite::Common::BuildLayoutBegin(header);

        oss << L"<section class=\"panel\">\n";
        oss << L"<h2>過去アーカイブ</h2>\n";

        if (request.dateKeys.empty()) {
            oss << L"<div class=\"empty-box\">過去アーカイブはありません。</div>\n";
        }
        else {
            oss << L"<div class=\"page-note\">日付を選択してください。</div>\n";
            oss << L"<div class=\"before-date-grid\">\n";

            for (const std::wstring& d : request.dateKeys) {
                oss << L"<a class=\"before-date-link\" href=\""
                    << HtmlEscape_(d + L".html")
                    << L"\">";
                oss << HtmlEscape_(DateLabel_(d));
                oss << L"</a>\n";
            }

            oss << L"</div>\n";
        }

        oss << L"</section>\n";
        oss << EpgHtmlLite::Common::BuildLayoutEnd();
        oss << L"</body>\n";
        oss << L"</html>\n";

        return oss.str();
    }

    std::wstring BuildDateHtml_(
        const EpgHtmlLite::PageBeforeList::DatePageRequest& request)
    {
        const std::wstring title =
            request.pageTitle.empty()
            ? L"EPG Lite 過去アーカイブ"
            : request.pageTitle;

        const std::vector<const ProgramEntry*> rows =
            SortRows_(request.rows);

        const EpgHtmlLite::Common::HeaderContext header =
            BuildHeader_(
                title,
                DateLabel_(request.dateKey) + L" / 過去アーカイブ / 表一覧",
                request.space);

        std::wostringstream oss;

        oss << L"<!doctype html>\n";
        oss << L"<html lang=\"ja\">\n";
        oss << L"<head>\n";
        oss << L"<meta charset=\"utf-8\">\n";
        oss << L"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n";
        oss << L"<title>" << HtmlEscape_(title) << L"</title>\n";
        oss << L"<style>\n";
        oss << EpgHtmlLite::Common::BuildCommonCss();
        AppendBeforeListCss_(oss);
        oss << L"</style>\n";
        oss << L"</head>\n";
        oss << L"<body>\n";

        oss << EpgHtmlLite::Common::BuildLayoutBegin(header);

        oss << L"<section class=\"panel\">\n";
        oss << L"<h2>過去番組 表一覧</h2>\n";
        oss << L"<div class=\"page-note\">日付: "
            << HtmlEscape_(DateLabel_(request.dateKey))
            << L" / 件数: "
            << rows.size()
            << L"</div>\n";

        if (rows.empty()) {
            oss << L"<div class=\"empty-box\">過去番組がありません。</div>\n";
        }
        else {
            oss << L"<div class=\"before-list-wrap\">\n";
            oss << L"<table class=\"before-list-table\">\n";
            oss << L"<thead><tr>";
            oss << L"<th class=\"c-time\">時刻</th>";
            oss << L"<th class=\"c-svc\">放送局</th>";
            oss << L"<th class=\"c-title\">番組</th>";
            oss << L"</tr></thead>\n";
            oss << L"<tbody>\n";

            for (const ProgramEntry* p : rows) {
                if (!p) {
                    continue;
                }

                const ProgramEntry& e = *p;

                oss << L"<tr>";
                oss << L"<td>"
                    << HtmlEscape_(TimeRangeText_(e))
                    << L"</td>";

                oss << L"<td>"
                    << HtmlEscape_(ResolveServiceName_(request, e))
                    << L"</td>";

                oss << L"<td>";
                oss << L"<span class=\"program-title\">"
                    << HtmlEscape_(ClipText_(e.title, 120))
                    << L"</span>";

                if (!e.description.empty()) {
                    oss << L"<span class=\"program-sub\">"
                        << HtmlEscape_(ClipText_(e.description, 160))
                        << L"</span>";
                }

                if (!e.genreKey.empty()) {
                    oss << L"<span class=\"program-sub genre\">"
                        << HtmlEscape_(e.genreKey)
                        << L"</span>";
                }

                oss << L"</td>";
                oss << L"</tr>\n";
            }

            oss << L"</tbody>\n";
            oss << L"</table>\n";
            oss << L"</div>\n";
        }

        oss << L"</section>\n";
        oss << EpgHtmlLite::Common::BuildLayoutEnd();
        oss << L"</body>\n";
        oss << L"</html>\n";

        return oss.str();
    }

    std::wstring FileNameFor_(
        int space)
    {
        return EpgHtmlLite::Common::BuildGuidePageFileName(
            EpgHtmlLite::Common::GuidePageKind::BeforeList,
            space);
    }

    std::vector<const ProgramEntry*> CollectRowsForContext_(
        const EpgHtmlLite::Context& ctx)
    {
        std::vector<const ProgramEntry*> rows;

        if (!ctx.sharedData) {
            return rows;
        }

        for (const ProgramEntry& e : ctx.sharedData->entries) {
            if (e.space != ctx.space) {
                continue;
            }

            if (!e.isPast) {
                continue;
            }

            if (e.title.empty()) {
                continue;
            }

            rows.push_back(&e);
        }

        return rows;
    }
}

namespace EpgHtmlLite::PageBeforeList
{
    bool GenerateIndexPage(
        const IndexPageRequest& request,
        Result& ioResult)
    {
        try {
            if (request.outputPath.empty()) {
                return false;
            }

            const std::wstring html =
                BuildIndexHtml_(request);

            if (!WriteUtf8File_(
                request.outputPath,
                html)) {
                return false;
            }

            ioResult.generatedFiles.push_back(
                request.outputPath.wstring());

            return true;
        }
        catch (...) {
            return false;
        }
    }

    bool GenerateDatePage(
        const DatePageRequest& request,
        Result& ioResult)
    {
        try {
            if (request.outputPath.empty()) {
                return false;
            }

            const std::wstring html =
                BuildDateHtml_(request);

            if (!WriteUtf8File_(
                request.outputPath,
                html)) {
                return false;
            }

            ioResult.generatedFiles.push_back(
                request.outputPath.wstring());

            return true;
        }
        catch (...) {
            return false;
        }
    }

    bool Generate(
        const Context& ctx,
        Result& ioResult)
    {
        try {
            DatePageRequest request{};
            request.outputPath =
                ctx.outputDir / FileNameFor_(ctx.space);
            request.pageTitle =
                ctx.title.empty()
                ? L"EPG Lite 過去番組"
                : ctx.title + L" Lite 過去番組";
            request.dateKey = L"";
            request.space = ctx.space;
            request.rows =
                CollectRowsForContext_(ctx);

            request.resolveServiceName =
                [](const ProgramEntry& e) {
                if (!e.serviceName.empty()) {
                    return e.serviceName;
                }

                return L"SID " +
                    std::to_wstring(
                        static_cast<unsigned int>(e.serviceId));
                };

            return GenerateDatePage(
                request,
                ioResult);
        }
        catch (...) {
            return false;
        }
    }
}