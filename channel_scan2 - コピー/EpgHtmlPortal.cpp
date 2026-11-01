#include "EpgHtmlPortal.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>
#include <cwctype>
#include <cstddef>
#include <cstdint>

#include "DebugManager.h"
#include "EpgHtmlPortal_Common.hpp"
#include "EpgHtmlPortal_index.hpp"
#include "EpgHtmlPortal_ProgramPageWriter.hpp"
#include "EpgHtmlPortal_Auto.hpp"

// 既存HTML生成器。
// 実体は既存ユニット側にある想定。
extern bool GenerateEITHtmlProgramTable(
    const std::wstring& mergedEpgPath,
    const std::wstring& channelListPath,
    const std::wstring& outputHtmlPath);

namespace EpgHtmlPortal
{
    namespace
    {
        static std::wstring ToLowerCopy_(std::wstring s)
        {
            for (auto& ch : s) {
                ch = static_cast<wchar_t>(
                    ::towlower(static_cast<wint_t>(ch)));
            }
            return s;
        }

        static std::wstring TrimAsciiWsW_(std::wstring s)
        {
            const auto isWs = [](wchar_t ch) noexcept {
                return ch == L' ' ||
                    ch == L'\t' ||
                    ch == L'\r' ||
                    ch == L'\n' ||
                    ch == L'\u3000';
                };

            while (!s.empty() && isWs(s.front())) {
                s.erase(s.begin());
            }

            while (!s.empty() && isWs(s.back())) {
                s.pop_back();
            }

            return s;
        }

        static std::vector<std::wstring> ParseCsvLooseW_(
            std::wstring_view s)
        {
            std::vector<std::wstring> out;
            std::wstring cur;
            bool inQ = false;

            for (std::size_t i = 0; i < s.size(); ++i) {
                const wchar_t c = s[i];

                if (inQ) {
                    if (c == L'"') {
                        if ((i + 1) < s.size() && s[i + 1] == L'"') {
                            cur.push_back(L'"');
                            ++i;
                        }
                        else {
                            inQ = false;
                        }
                    }
                    else {
                        cur.push_back(c);
                    }
                }
                else {
                    if (c == L'"') {
                        inQ = true;
                    }
                    else if (c == L',') {
                        out.push_back(TrimAsciiWsW_(cur));
                        cur.clear();
                    }
                    else {
                        cur.push_back(c);
                    }
                }
            }

            out.push_back(TrimAsciiWsW_(cur));
            return out;
        }

        static int ToIntLooseW_(
            const std::wstring& s,
            int def = 0) noexcept
        {
            try {
                std::size_t pos = 0;
                const int v = std::stoi(s, &pos, 10);
                (void)pos;
                return v;
            }
            catch (...) {
                return def;
            }
        }

        static bool ExtractBodyInnerHtml_(
            const std::wstring& html,
            std::wstring& outBody)
        {
            const std::wstring lower = ToLowerCopy_(html);

            const std::size_t bodyPos = lower.find(L"<body");
            if (bodyPos == std::wstring::npos) {
                return false;
            }

            const std::size_t bodyOpenEnd = lower.find(L'>', bodyPos);
            if (bodyOpenEnd == std::wstring::npos) {
                return false;
            }

            const std::size_t bodyClose = lower.rfind(L"</body>");
            if (bodyClose == std::wstring::npos || bodyClose <= bodyOpenEnd) {
                return false;
            }

            outBody.assign(
                html.begin() + static_cast<std::ptrdiff_t>(bodyOpenEnd + 1),
                html.begin() + static_cast<std::ptrdiff_t>(bodyClose));

            return true;
        }

        static std::wstring ExtractAttrValue_(
            const std::wstring& tag,
            const std::wstring& attrName)
        {
            const std::wstring key = attrName + L"=\"";
            const std::size_t pos = tag.find(key);
            if (pos == std::wstring::npos) {
                return {};
            }

            const std::size_t begin = pos + key.size();
            const std::size_t end = tag.find(L'"', begin);
            if (end == std::wstring::npos || end <= begin) {
                return {};
            }

            return tag.substr(begin, end - begin);
        }

        static std::wstring ExtractInnerByMarkers_(
            const std::wstring& src,
            const std::wstring& beginMarker,
            const std::wstring& endMarker)
        {
            const std::size_t begin = src.find(beginMarker);
            if (begin == std::wstring::npos) {
                return {};
            }

            const std::size_t contentBegin = begin + beginMarker.size();
            const std::size_t end = src.find(endMarker, contentBegin);
            if (end == std::wstring::npos || end <= contentBegin) {
                return {};
            }

            return src.substr(contentBegin, end - contentBegin);
        }

        static std::wstring EscapeAttr_(
            std::wstring_view s)
        {
            std::wstring out;
            out.reserve(s.size() + 16);

            for (const wchar_t ch : s) {
                switch (ch) {
                case L'&':  out += L"&amp;";  break;
                case L'<':  out += L"&lt;";   break;
                case L'>':  out += L"&gt;";   break;
                case L'"':  out += L"&quot;"; break;
                default:
                    if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
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

        static std::wstring EnrichProgramCells_(
            const std::wstring& bodyHtml)
        {
            const std::wstring cellStart = L"<td class=\"prog-cell";
            const std::wstring cellEnd = L"</td>";

            std::wstring out;
            out.reserve(bodyHtml.size() + bodyHtml.size() / 8);

            std::size_t pos = 0;

            while (true) {
                const std::size_t tdPos = bodyHtml.find(cellStart, pos);
                if (tdPos == std::wstring::npos) {
                    out.append(bodyHtml, pos, std::wstring::npos);
                    break;
                }

                out.append(bodyHtml, pos, tdPos - pos);

                const std::size_t tagEnd = bodyHtml.find(L'>', tdPos);
                if (tagEnd == std::wstring::npos) {
                    out.append(bodyHtml, tdPos, std::wstring::npos);
                    break;
                }

                const std::size_t cellClose =
                    bodyHtml.find(cellEnd, tagEnd + 1);

                if (cellClose == std::wstring::npos) {
                    out.append(bodyHtml, tdPos, std::wstring::npos);
                    break;
                }

                std::wstring openTag =
                    bodyHtml.substr(tdPos, tagEnd - tdPos + 1);

                const std::wstring cellInner =
                    bodyHtml.substr(
                        tagEnd + 1,
                        cellClose - (tagEnd + 1));

                if (openTag.find(L"data-epg-program=") == std::wstring::npos) {
                    const std::wstring title =
                        ExtractAttrValue_(openTag, L"title");

                    const std::wstring eid =
                        ExtractInnerByMarkers_(
                            cellInner,
                            L"<span class=\"eid\">",
                            L"</span>");

                    const std::wstring tm =
                        ExtractInnerByMarkers_(
                            cellInner,
                            L"<div class=\"tm\">",
                            L"</div>");

                    const std::wstring summary =
                        ExtractInnerByMarkers_(
                            cellInner,
                            L"<div class=\"s\">",
                            L"</div>");

                    openTag.pop_back();
                    openTag += L" data-epg-program=\"1\"";

                    if (!title.empty()) {
                        openTag += L" data-title=\"";
                        openTag += EscapeAttr_(title);
                        openTag += L"\"";
                    }

                    if (!eid.empty()) {
                        openTag += L" data-event-id=\"";
                        openTag += EscapeAttr_(eid);
                        openTag += L"\"";
                    }

                    if (!tm.empty()) {
                        openTag += L" data-time=\"";
                        openTag += EscapeAttr_(tm);
                        openTag += L"\"";
                    }

                    if (!summary.empty()) {
                        openTag += L" data-summary=\"";
                        openTag += EscapeAttr_(summary);
                        openTag += L"\"";
                    }

                    openTag += L'>';
                }

                out += openTag;
                out += cellInner;
                out += cellEnd;

                pos = cellClose + cellEnd.size();
            }

            return out;
        }

        static std::wstring BuildGeneratedListFromProgCells_(
            const std::wstring& legacyBody)
        {
            const std::wstring cellStart = L"<td class=\"prog-cell";
            const std::wstring cellEnd = L"</td>";

            std::wstring out;
            out.reserve(legacyBody.size() / 2 + 4096);

            out += L"<div class=\"epg-list-generated\">\n";
            out += L"<table class=\"program-list-table\">\n";
            out += L"<thead><tr>";
            out += L"<th>時刻</th>";
            out += L"<th>番組名</th>";
            out += L"<th>内容</th>";
            out += L"<th>イベントID</th>";
            out += L"</tr></thead>\n";
            out += L"<tbody>\n";

            std::size_t count = 0;
            std::size_t pos = 0;

            while (true) {
                const std::size_t tdPos = legacyBody.find(cellStart, pos);
                if (tdPos == std::wstring::npos) {
                    break;
                }

                const std::size_t tagEnd = legacyBody.find(L'>', tdPos);
                if (tagEnd == std::wstring::npos) {
                    break;
                }

                const std::size_t cellClose =
                    legacyBody.find(cellEnd, tagEnd + 1);

                if (cellClose == std::wstring::npos) {
                    break;
                }

                const std::wstring openTag =
                    legacyBody.substr(tdPos, tagEnd - tdPos + 1);

                const std::wstring inner =
                    legacyBody.substr(
                        tagEnd + 1,
                        cellClose - (tagEnd + 1));

                std::wstring title = ExtractAttrValue_(openTag, L"data-title");
                if (title.empty()) {
                    title = ExtractAttrValue_(openTag, L"title");
                }
                if (title.empty()) {
                    title = ExtractInnerByMarkers_(
                        inner,
                        L"<div class=\"t\">",
                        L"</div>");
                }

                if (!title.empty()) {
                    std::wstring tm = ExtractAttrValue_(openTag, L"data-time");
                    if (tm.empty()) {
                        tm = ExtractInnerByMarkers_(
                            inner,
                            L"<div class=\"tm\">",
                            L"</div>");
                    }

                    std::wstring summary =
                        ExtractAttrValue_(openTag, L"data-summary");

                    if (summary.empty()) {
                        summary = ExtractInnerByMarkers_(
                            inner,
                            L"<div class=\"s\">",
                            L"</div>");
                    }

                    std::wstring eid =
                        ExtractAttrValue_(openTag, L"data-event-id");

                    if (eid.empty()) {
                        eid = ExtractInnerByMarkers_(
                            inner,
                            L"<span class=\"eid\">",
                            L"</span>");
                    }

                    out += L"<tr class=\"program-list-row\" data-epg-program=\"1\"";

                    out += L" data-title=\"";
                    out += EscapeAttr_(title);
                    out += L"\"";

                    if (!tm.empty()) {
                        out += L" data-time=\"";
                        out += EscapeAttr_(tm);
                        out += L"\"";
                    }

                    if (!eid.empty()) {
                        out += L" data-event-id=\"";
                        out += EscapeAttr_(eid);
                        out += L"\"";
                    }

                    if (!summary.empty()) {
                        out += L" data-summary=\"";
                        out += EscapeAttr_(summary);
                        out += L"\"";
                    }

                    out += L">";

                    out += L"<td class=\"program-list-time\">";
                    out += EpgHtmlPortal_Common::EscapeHtmlW(
                        tm.empty() ? L"--:--" : tm);
                    out += L"</td>";

                    out += L"<td class=\"program-list-title\">";
                    out += EpgHtmlPortal_Common::EscapeHtmlW(title);
                    out += L"</td>";

                    out += L"<td class=\"program-list-summary\">";
                    out += EpgHtmlPortal_Common::EscapeHtmlW(summary);
                    out += L"</td>";

                    out += L"<td class=\"program-list-eid\">";
                    out += EpgHtmlPortal_Common::EscapeHtmlW(eid);
                    out += L"</td>";

                    out += L"</tr>\n";

                    ++count;
                }

                pos = cellClose + cellEnd.size();
            }

            out += L"</tbody>\n";
            out += L"</table>\n";
            out += L"</div>\n";

            DebugLogFormat(
                L"[EPG-PORTAL][GEN] list rows=%zu",
                count);

            return out;
        }

        static bool IsHtmlNameCharForPortalSplit_(wchar_t ch) noexcept
        {
            return
                (ch >= L'a' && ch <= L'z') ||
                (ch >= L'A' && ch <= L'Z') ||
                (ch >= L'0' && ch <= L'9') ||
                ch == L'_' ||
                ch == L'-' ||
                ch == L':';
        }

        static bool IsHtmlTokenBoundaryForPortalSplit_(wchar_t ch) noexcept
        {
            return !IsHtmlNameCharForPortalSplit_(ch);
        }

        static bool OpenTagHasClassTokenForPortalSplit_(
            const std::wstring& openTagLower,
            std::wstring_view tokenLower)
        {
            if (openTagLower.find(L"class") == std::wstring::npos ||
                tokenLower.empty()) {
                return false;
            }

            const std::wstring token(tokenLower);
            std::size_t pos = 0;

            while (true) {
                pos = openTagLower.find(token, pos);
                if (pos == std::wstring::npos) {
                    return false;
                }

                const bool leftOk =
                    pos == 0 ||
                    IsHtmlTokenBoundaryForPortalSplit_(openTagLower[pos - 1]);

                const std::size_t right = pos + token.size();
                const bool rightOk =
                    right >= openTagLower.size() ||
                    IsHtmlTokenBoundaryForPortalSplit_(openTagLower[right]);

                if (leftOk && rightOk) {
                    return true;
                }

                ++pos;
            }
        }

        static std::wstring ParseOpenTagNameLowerForPortalSplit_(
            const std::wstring& lower,
            std::size_t tagStart)
        {
            if (tagStart >= lower.size() || lower[tagStart] != L'<') {
                return {};
            }

            std::size_t i = tagStart + 1;
            if (i >= lower.size() ||
                lower[i] == L'/' ||
                lower[i] == L'!' ||
                lower[i] == L'?') {
                return {};
            }

            std::wstring name;
            while (i < lower.size() &&
                IsHtmlNameCharForPortalSplit_(lower[i])) {
                name.push_back(lower[i]);
                ++i;
            }

            return name;
        }

        static bool StartsWithOpenTagNameForPortalSplit_(
            const std::wstring& lower,
            std::size_t tagStart,
            const std::wstring& tagName)
        {
            if (tagStart >= lower.size() || lower[tagStart] != L'<') {
                return false;
            }

            const std::size_t nameStart = tagStart + 1;
            if (nameStart + tagName.size() > lower.size()) {
                return false;
            }

            if (lower.compare(nameStart, tagName.size(), tagName) != 0) {
                return false;
            }

            const std::size_t next = nameStart + tagName.size();
            return next >= lower.size() ||
                IsHtmlTokenBoundaryForPortalSplit_(lower[next]);
        }

        static bool StartsWithCloseTagNameForPortalSplit_(
            const std::wstring& lower,
            std::size_t tagStart,
            const std::wstring& tagName)
        {
            if (tagStart + 2 >= lower.size() ||
                lower[tagStart] != L'<' ||
                lower[tagStart + 1] != L'/') {
                return false;
            }

            const std::size_t nameStart = tagStart + 2;
            if (nameStart + tagName.size() > lower.size()) {
                return false;
            }

            if (lower.compare(nameStart, tagName.size(), tagName) != 0) {
                return false;
            }

            const std::size_t next = nameStart + tagName.size();
            return next >= lower.size() ||
                IsHtmlTokenBoundaryForPortalSplit_(lower[next]);
        }

        static bool IsSelfClosingOpenTagForPortalSplit_(
            const std::wstring& lower,
            std::size_t tagStart,
            std::size_t tagEnd)
        {
            if (tagEnd == std::wstring::npos || tagEnd <= tagStart) {
                return false;
            }

            std::size_t i = tagEnd;
            while (i > tagStart) {
                --i;

                const wchar_t ch = lower[i];
                if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n') {
                    continue;
                }

                return ch == L'/';
            }

            return false;
        }

        static std::size_t FindBalancedElementEndForPortalSplit_(
            const std::wstring& lower,
            std::size_t tagStart)
        {
            const std::wstring tagName =
                ParseOpenTagNameLowerForPortalSplit_(lower, tagStart);

            if (tagName.empty()) {
                return std::wstring::npos;
            }

            const std::size_t firstOpenEnd = lower.find(L'>', tagStart);
            if (firstOpenEnd == std::wstring::npos) {
                return std::wstring::npos;
            }

            if (IsSelfClosingOpenTagForPortalSplit_(
                lower,
                tagStart,
                firstOpenEnd)) {
                return firstOpenEnd + 1;
            }

            int depth = 1;
            std::size_t pos = firstOpenEnd + 1;

            while (pos < lower.size()) {
                const std::size_t nextTag = lower.find(L'<', pos);
                if (nextTag == std::wstring::npos) {
                    return std::wstring::npos;
                }

                if (StartsWithCloseTagNameForPortalSplit_(
                    lower,
                    nextTag,
                    tagName)) {
                    const std::size_t closeEnd = lower.find(L'>', nextTag);
                    if (closeEnd == std::wstring::npos) {
                        return std::wstring::npos;
                    }

                    --depth;
                    if (depth == 0) {
                        return closeEnd + 1;
                    }

                    pos = closeEnd + 1;
                    continue;
                }

                if (StartsWithOpenTagNameForPortalSplit_(
                    lower,
                    nextTag,
                    tagName)) {
                    const std::size_t openEnd = lower.find(L'>', nextTag);
                    if (openEnd == std::wstring::npos) {
                        return std::wstring::npos;
                    }

                    if (!IsSelfClosingOpenTagForPortalSplit_(
                        lower,
                        nextTag,
                        openEnd)) {
                        ++depth;
                    }

                    pos = openEnd + 1;
                    continue;
                }

                ++pos;
            }

            return std::wstring::npos;
        }

        static bool ExtractFirstElementByClassTokenForPortalSplit_(
            const std::wstring& html,
            std::wstring_view classToken,
            std::wstring& outElement)
        {
            outElement.clear();

            if (html.empty() || classToken.empty()) {
                return false;
            }

            const std::wstring lower = ToLowerCopy_(html);
            const std::wstring tokenLower =
                ToLowerCopy_(std::wstring(classToken));

            std::size_t pos = 0;

            while (true) {
                pos = lower.find(tokenLower, pos);
                if (pos == std::wstring::npos) {
                    return false;
                }

                const std::size_t tagStart = lower.rfind(L'<', pos);
                const std::size_t tagEnd = lower.find(L'>', pos);

                if (tagStart == std::wstring::npos ||
                    tagEnd == std::wstring::npos ||
                    tagStart > pos ||
                    tagEnd < pos) {
                    ++pos;
                    continue;
                }

                const std::wstring openTagLower =
                    lower.substr(tagStart, tagEnd - tagStart + 1);

                if (!OpenTagHasClassTokenForPortalSplit_(
                    openTagLower,
                    tokenLower)) {
                    ++pos;
                    continue;
                }

                const std::size_t elementEnd =
                    FindBalancedElementEndForPortalSplit_(lower, tagStart);

                if (elementEnd == std::wstring::npos ||
                    elementEnd <= tagStart ||
                    elementEnd > html.size()) {
                    ++pos;
                    continue;
                }

                outElement.assign(
                    html.begin() + static_cast<std::ptrdiff_t>(tagStart),
                    html.begin() + static_cast<std::ptrdiff_t>(elementEnd));

                return !outElement.empty();
            }
        }

        static bool ExtractAllElementsByClassTokenForPortalSplit_(
            const std::wstring& html,
            std::wstring_view classToken,
            std::vector<std::wstring>& outElements)
        {
            outElements.clear();

            if (html.empty() || classToken.empty()) {
                return false;
            }

            const std::wstring lower = ToLowerCopy_(html);
            const std::wstring tokenLower =
                ToLowerCopy_(std::wstring(classToken));

            std::size_t pos = 0;

            while (true) {
                pos = lower.find(tokenLower, pos);
                if (pos == std::wstring::npos) {
                    break;
                }

                const std::size_t tagStart = lower.rfind(L'<', pos);
                const std::size_t tagEnd = lower.find(L'>', pos);

                if (tagStart == std::wstring::npos ||
                    tagEnd == std::wstring::npos ||
                    tagStart > pos ||
                    tagEnd < pos) {
                    ++pos;
                    continue;
                }

                const std::wstring openTagLower =
                    lower.substr(tagStart, tagEnd - tagStart + 1);

                if (!OpenTagHasClassTokenForPortalSplit_(
                    openTagLower,
                    tokenLower)) {
                    ++pos;
                    continue;
                }

                const std::size_t elementEnd =
                    FindBalancedElementEndForPortalSplit_(lower, tagStart);

                if (elementEnd == std::wstring::npos ||
                    elementEnd <= tagStart ||
                    elementEnd > html.size()) {
                    ++pos;
                    continue;
                }

                std::wstring element;
                element.assign(
                    html.begin() + static_cast<std::ptrdiff_t>(tagStart),
                    html.begin() + static_cast<std::ptrdiff_t>(elementEnd));

                if (!element.empty()) {
                    outElements.push_back(std::move(element));
                }

                pos = elementEnd;
            }

            return !outElements.empty();
        }

        static bool ExtractAllElementsByClassTokensForPortalSplit_(
            const std::wstring& html,
            std::initializer_list<std::wstring_view> classTokens,
            std::vector<std::wstring>& outElements)
        {
            for (const auto token : classTokens) {
                if (ExtractAllElementsByClassTokenForPortalSplit_(
                    html,
                    token,
                    outElements)) {
                    return true;
                }
            }

            outElements.clear();
            return false;
        }

        static bool ExtractFirstElementByClassTokensForPortalSplit_(
            const std::wstring& html,
            std::initializer_list<std::wstring_view> classTokens,
            std::wstring& outElement)
        {
            for (const auto token : classTokens) {
                if (ExtractFirstElementByClassTokenForPortalSplit_(
                    html,
                    token,
                    outElement)) {
                    return true;
                }
            }

            outElement.clear();
            return false;
        }

        static void AppendOptionalClassElementForPortalSplit_(
            const std::wstring& legacyBody,
            std::wstring_view classToken,
            std::wstring& out)
        {
            std::wstring part;
            if (ExtractFirstElementByClassTokenForPortalSplit_(
                legacyBody,
                classToken,
                part)) {
                out += part;
                out += L"\n";
            }
        }

        static std::wstring WrapSplitBodyForPortal_(
            std::wstring_view modeClass,
            std::wstring_view note,
            const std::wstring& body)
        {
            std::wstring out;
            out.reserve(body.size() + 512);

            out += L"<div class=\"epg-separated-page ";
            out += std::wstring(modeClass);
            out += L"\">\n";

            out += L"<div class=\"program-table-note\">";
            out += EpgHtmlPortal_Common::EscapeHtmlW(note);
            out += L"</div>\n";

            out += body;
            out += L"\n</div>\n";

            return out;
        }

        static std::wstring FindDateYmdSlashInTextForPortal_(
            std::wstring_view text)
        {
            for (std::size_t i = 0; i + 10 <= text.size(); ++i) {
                if (text[i + 4] != L'/' || text[i + 7] != L'/') {
                    continue;
                }

                const bool ok =
                    std::iswdigit(text[i + 0]) &&
                    std::iswdigit(text[i + 1]) &&
                    std::iswdigit(text[i + 2]) &&
                    std::iswdigit(text[i + 3]) &&
                    std::iswdigit(text[i + 5]) &&
                    std::iswdigit(text[i + 6]) &&
                    std::iswdigit(text[i + 8]) &&
                    std::iswdigit(text[i + 9]);

                if (ok) {
                    return std::wstring(text.substr(i, 10));
                }
            }

            return {};
        }

        static std::wstring ExtractDateFromTimetableGridForPortal_(
            const std::wstring& gridHtml)
        {
            const std::wstring lower = ToLowerCopy_(gridHtml);

            std::size_t pos = 0;

            while (true) {
                const std::size_t thPos = lower.find(L"<th", pos);
                if (thPos == std::wstring::npos) {
                    break;
                }

                const std::size_t tagEnd = lower.find(L'>', thPos);
                if (tagEnd == std::wstring::npos) {
                    break;
                }

                const std::wstring openTagLower =
                    lower.substr(thPos, tagEnd - thPos + 1);

                if (!OpenTagHasClassTokenForPortalSplit_(
                    openTagLower,
                    L"time-col")) {
                    pos = tagEnd + 1;
                    continue;
                }

                const std::size_t closePos = lower.find(L"</th>", tagEnd + 1);
                if (closePos == std::wstring::npos) {
                    break;
                }

                const std::wstring inner =
                    gridHtml.substr(tagEnd + 1, closePos - (tagEnd + 1));

                const std::wstring date =
                    FindDateYmdSlashInTextForPortal_(inner);

                if (!date.empty()) {
                    return date;
                }

                pos = closePos + 5;
            }

            return FindDateYmdSlashInTextForPortal_(gridHtml);
        }

        static std::wstring BuildTimetableBodyFromLegacyForPortal_(
            const std::wstring& legacyBody)
        {
            std::vector<std::wstring> grids;

            if (!ExtractAllElementsByClassTokensForPortalSplit_(
                legacyBody,
                {
                    L"grid-view",
                    L"timetable-view",
                    L"epg-grid-view",
                    L"program-grid-view"
                },
                grids)) {
                DebugLog(L"[EPG-PORTAL][SPLIT][WARN] timetable blocks not found. fallback full legacy body.");

                return WrapSplitBodyForPortal_(
                    L"epg-separated-timetable",
                    L"時間割表示です。時間割ブロックが見つからないため互換表示を使用しています。",
                    legacyBody);
            }

            std::wstring body;
            body.reserve(legacyBody.size() + 4096);

            AppendOptionalClassElementForPortalSplit_(
                legacyBody,
                L"day-bar",
                body);

            AppendOptionalClassElementForPortalSplit_(
                legacyBody,
                L"genre-bar",
                body);

            body += L"<div id=\"epgTimetableMount\" class=\"epg-timetable-mount\"></div>\n";
            body += L"<div id=\"epgTimetableTemplates\" class=\"epg-timetable-template-store\" hidden>\n";

            std::unordered_set<std::wstring> seenDays;
            std::size_t written = 0;

            for (const auto& grid : grids) {
                const std::wstring day =
                    ExtractDateFromTimetableGridForPortal_(grid);

                if (day.empty()) {
                    DebugLog(L"[EPG-PORTAL][SPLIT][WARN] timetable grid date not found. skip template.");
                    continue;
                }

                if (!seenDays.insert(day).second) {
                    DebugLogFormat(
                        L"[EPG-PORTAL][SPLIT][WARN] duplicate timetable day skipped: %ls",
                        day.c_str());
                    continue;
                }

                body += L"<template class=\"epg-timetable-template\" data-day=\"";
                body += EscapeAttr_(day);
                body += L"\">\n";
                body += grid;
                body += L"\n</template>\n";

                ++written;
            }

            body += L"</div>\n";

            if (written == 0) {
                DebugLog(L"[EPG-PORTAL][SPLIT][WARN] no timetable templates generated. fallback inline grids.");

                body += L"<div class=\"epg-timetable-day-stack\" data-inline-days=\"";
                body += std::to_wstring(grids.size());
                body += L"\">\n";

                for (const auto& grid : grids) {
                    body += grid;
                    body += L"\n";
                }

                body += L"</div>\n";
            }

            DebugLogFormat(
                L"[EPG-PORTAL][SPLIT] timetable templates=%zu sourceGrids=%zu",
                written,
                grids.size());

            return WrapSplitBodyForPortal_(
                L"epg-separated-timetable",
                L"時間割表示です。",
                body);
        }

        static std::vector<std::wstring> ExtractDayButtonDaysForPortal_(
            const std::wstring& legacyBody)
        {
            std::vector<std::wstring> days;

            const std::wstring lower = ToLowerCopy_(legacyBody);
            std::size_t pos = 0;

            while (true) {
                pos = lower.find(L"data-day=", pos);
                if (pos == std::wstring::npos) {
                    break;
                }

                const std::size_t quote1 = legacyBody.find(L'"', pos);
                if (quote1 == std::wstring::npos) {
                    ++pos;
                    continue;
                }

                const std::size_t quote2 = legacyBody.find(L'"', quote1 + 1);
                if (quote2 == std::wstring::npos || quote2 <= quote1 + 1) {
                    ++pos;
                    continue;
                }

                const std::wstring day =
                    legacyBody.substr(quote1 + 1, quote2 - quote1 - 1);

                if (day != L"all" &&
                    day.size() == 10 &&
                    day[4] == L'/' &&
                    day[7] == L'/') {
                    if (std::find(days.begin(), days.end(), day) == days.end()) {
                        days.push_back(day);
                    }
                }

                pos = quote2 + 1;
            }

            return days;
        }

        static std::wstring GetFirstProgramDateFromListViewForPortal_(
            const std::wstring& listHtml)
        {
            const std::wstring lower = ToLowerCopy_(listHtml);
            std::size_t pos = 0;

            while (true) {
                pos = lower.find(L"data-date=", pos);
                if (pos == std::wstring::npos) {
                    break;
                }

                const std::size_t quote1 = listHtml.find(L'"', pos);
                if (quote1 == std::wstring::npos) {
                    ++pos;
                    continue;
                }

                const std::size_t quote2 = listHtml.find(L'"', quote1 + 1);
                if (quote2 == std::wstring::npos || quote2 <= quote1 + 1) {
                    ++pos;
                    continue;
                }

                const std::wstring day =
                    listHtml.substr(quote1 + 1, quote2 - quote1 - 1);

                if (day.size() == 10 &&
                    day[4] == L'/' &&
                    day[7] == L'/') {
                    return day;
                }

                pos = quote2 + 1;
            }

            return {};
        }

        static std::wstring BuildListBodyFromLegacyForPortal_(
            const std::wstring& legacyBody)
        {
            std::vector<std::wstring> lists;

            if (!ExtractAllElementsByClassTokensForPortalSplit_(
                legacyBody,
                {
                    L"genre-view",
                    L"list-view",
                    L"program-list-view"
                },
                lists)) {
                DebugLog(L"[EPG-PORTAL][SPLIT][WARN] original list blocks not found. fallback generated list.");

                return WrapSplitBodyForPortal_(
                    L"epg-separated-list",
                    L"一覧表示です。一覧ブロックが見つからないため、時間割セルから再構成しています。",
                    BuildGeneratedListFromProgCells_(legacyBody));
            }

            const std::vector<std::wstring> buttonDays =
                ExtractDayButtonDaysForPortal_(legacyBody);

            std::wstring body;
            body.reserve(legacyBody.size() + 4096);

            // top-links は一時HTML由来の過去リンクを含むため出力しない。
            // 過去表示は index.html 側に集約する。

            AppendOptionalClassElementForPortalSplit_(
                legacyBody,
                L"day-bar",
                body);

            AppendOptionalClassElementForPortalSplit_(
                legacyBody,
                L"genre-bar",
                body);

            body += L"<div id=\"epgListMount\" class=\"epg-list-mount\"></div>\n";
            body += L"<div id=\"epgListTemplates\" class=\"epg-list-template-store\" hidden>\n";

            std::unordered_set<std::wstring> seenDays;
            std::size_t written = 0;

            for (std::size_t i = 0; i < lists.size(); ++i) {
                std::wstring day;

                if (i < buttonDays.size()) {
                    // 放送日ベースのボタン順に合わせる。
                    // 深夜 0〜4時を翌日実日付で持っている場合でも、放送日単位を維持できる。
                    day = buttonDays[i];
                }
                else {
                    // 念のため fallback。
                    day = GetFirstProgramDateFromListViewForPortal_(lists[i]);
                }

                if (day.empty()) {
                    DebugLog(L"[EPG-PORTAL][SPLIT][WARN] list block date not found. skip template.");
                    continue;
                }

                if (!seenDays.insert(day).second) {
                    DebugLogFormat(
                        L"[EPG-PORTAL][SPLIT][WARN] duplicate list day skipped: %ls",
                        day.c_str());
                    continue;
                }

                body += L"<template class=\"epg-list-template\" data-day=\"";
                body += EscapeAttr_(day);
                body += L"\">\n";
                body += lists[i];
                body += L"\n</template>\n";

                ++written;
            }

            body += L"</div>\n";

            if (written == 0) {
                DebugLog(L"[EPG-PORTAL][SPLIT][WARN] no list templates generated. fallback inline lists.");

                body += L"<div class=\"epg-list-day-stack\" data-inline-days=\"";
                body += std::to_wstring(lists.size());
                body += L"\">\n";

                for (const auto& list : lists) {
                    body += list;
                    body += L"\n";
                }

                body += L"</div>\n";
            }

            DebugLogFormat(
                L"[EPG-PORTAL][SPLIT] list templates=%zu sourceLists=%zu dayButtons=%zu",
                written,
                lists.size(),
                buttonDays.size());

            return WrapSplitBodyForPortal_(
                L"epg-separated-list",
                L"一覧表示です。",
                body);
        }

        static std::wstring WrapBody_(
            std::wstring_view modeClass,
            std::wstring_view note,
            const std::wstring& body)
        {
            std::wstring out;
            out.reserve(body.size() + 512);

            out += L"<div class=\"epg-separated-page ";
            out += std::wstring(modeClass);
            out += L"\">\n";

            out += L"<div class=\"program-table-note\">";
            out += EpgHtmlPortal_Common::EscapeHtmlW(note);
            out += L"</div>\n";

            out += body;
            out += L"\n</div>\n";

            return out;
        }

        static std::wstring MakeSpaceLabel_(
            int space)
        {
            switch (space) {
            case 0: return L"地上波";
            case 1: return L"BS";
            case 2: return L"CS";
            default:
                return L"Space " + std::to_wstring(space);
            }
        }

        static std::wstring DefaultFileName_(
            const std::wstring& custom,
            const std::wstring& fallback)
        {
            return custom.empty() ? fallback : custom;
        }

        static std::filesystem::path OutputPath_(
            const std::filesystem::path& outputDir,
            const std::wstring& fileName)
        {
            if (fileName.empty()) {
                return {};
            }

            const std::filesystem::path p(fileName);
            if (p.is_absolute()) {
                return p;
            }

            return outputDir / p;
        }

        static std::vector<EpgHtmlPortal_ProgramPageWriter::ChannelServiceInfo>
            LoadServiceMapFromChList_(
                const std::filesystem::path& chListPath,
                int targetSpace)
        {
            std::vector<EpgHtmlPortal_ProgramPageWriter::ChannelServiceInfo> out;

            if (chListPath.empty() || !std::filesystem::exists(chListPath)) {
                return out;
            }

            std::wstring text;
            if (!EpgHtmlPortal_Common::ReadTextFileToWide(chListPath, text)) {
                return out;
            }

            std::unordered_set<std::wstring> seenKeys;

            std::wistringstream iss(text);
            std::wstring line;

            while (std::getline(iss, line)) {
                line = TrimAsciiWsW_(line);

                if (line.empty() || line[0] == L';' || line[0] == L'#') {
                    continue;
                }

                const auto cols = ParseCsvLooseW_(line);
                if (cols.size() < 7) {
                    continue;
                }

                const std::wstring serviceName = cols[0];
                const int space = ToIntLooseW_(cols[1], -1);
                if (space != targetSpace || serviceName.empty()) {
                    continue;
                }

                int physicalChannel = -1;
                int serviceId = 0;
                int networkId = 0;
                int tsId = 0;
                std::wstring remoteKey;

                if (cols.size() >= 12) {
                    physicalChannel = ToIntLooseW_(cols[2], -1);
                    serviceId = ToIntLooseW_(cols[4], 0);
                    networkId = ToIntLooseW_(cols[5], 0);
                    tsId = ToIntLooseW_(cols[6], 0);
                    remoteKey = cols[11];
                }
                else if (cols.size() >= 9) {
                    physicalChannel = ToIntLooseW_(cols[2], -1);
                    remoteKey = cols[3];
                    serviceId = ToIntLooseW_(cols[5], 0);
                    networkId = ToIntLooseW_(cols[6], 0);
                    tsId = ToIntLooseW_(cols[7], 0);
                }
                else {
                    physicalChannel = ToIntLooseW_(cols[2], -1);
                    serviceId = ToIntLooseW_(cols[4], 0);
                    networkId = ToIntLooseW_(cols[5], 0);
                    tsId = ToIntLooseW_(cols[6], 0);
                }

                if (serviceId <= 0 || networkId <= 0 || tsId <= 0) {
                    continue;
                }

                EpgHtmlPortal_ProgramPageWriter::ChannelServiceInfo svc{};
                svc.space = space;
                svc.physicalChannel = physicalChannel;
                svc.networkId = networkId;
                svc.tsId = tsId;
                svc.serviceId = serviceId;
                svc.serviceName = serviceName;
                svc.channelName = serviceName;
                svc.remoteControlKeyId = remoteKey;
                svc.serviceKey =
                    EpgHtmlPortal_ProgramPageWriter::MakeServiceKey(
                        space,
                        networkId,
                        tsId,
                        serviceId);

                if (svc.serviceKey.empty()) {
                    continue;
                }

                if (!seenKeys.insert(svc.serviceKey).second) {
                    continue;
                }

                out.push_back(std::move(svc));
            }

            DebugLogFormat(
                L"[EPG-PORTAL][GEN] service map loaded: space=%d count=%zu",
                targetSpace,
                out.size());

            return out;
        }

        static bool ReadGeneratedBody_(
            const std::filesystem::path& htmlPath,
            std::wstring& outBody)
        {
            outBody.clear();

            std::wstring html;
            if (!EpgHtmlPortal_Common::ReadTextFileToWide(htmlPath, html)) {
                return false;
            }

            if (!ExtractBodyInnerHtml_(html, outBody)) {
                outBody = std::move(html);
            }

            outBody = EnrichProgramCells_(outBody);
            return true;
        }

        static bool FileExistsNoThrow_(
            const std::filesystem::path& path) noexcept
        {
            if (path.empty()) {
                return false;
            }

            std::error_code ec;

            if (!std::filesystem::exists(path, ec) || ec) {
                return false;
            }

            ec.clear();
            if (!std::filesystem::is_regular_file(path, ec) || ec) {
                return false;
            }

            return true;
        }

        struct BeforeArchiveSourceStamp_
        {
            std::uintmax_t fileSize = 0;
            long long writeTicks = 0;
            bool valid = false;
        };

        static std::filesystem::path MakeBeforeArchiveStampPath_(
            const EpgHtmlPortal::GenerateFromEpgRequest& req)
        {
            return req.outputDir /
                L"past_data" /
                (L"before_archive_space" + std::to_wstring(req.space) + L".stamp");
        }

        static std::filesystem::path MakeBeforeArchiveFolderManifestPath_(
            const EpgHtmlPortal::GenerateFromEpgRequest& req)
        {
            return req.outputDir /
                (L"epg_space" + std::to_wstring(req.space) + L"_before") /
                L"manifest.js";
        }

        static std::filesystem::path MakeBeforeArchiveSpaceManifestPath_(
            const EpgHtmlPortal::GenerateFromEpgRequest& req)
        {
            return req.outputDir /
                L"past_data" /
                (L"manifest_space" + std::to_wstring(req.space) + L".js");
        }

        static std::filesystem::path MakeBeforeArchiveRootManifestPath_(
            const EpgHtmlPortal::GenerateFromEpgRequest& req)
        {
            return req.outputDir / L"past_data" / L"manifest.js";
        }

        static bool TryGetBeforeArchiveSourceStamp_(
            const std::filesystem::path& sourcePath,
            BeforeArchiveSourceStamp_& out) noexcept
        {
            out = {};

            if (sourcePath.empty()) {
                return false;
            }

            try {
                std::error_code ec;

                if (!std::filesystem::exists(sourcePath, ec) || ec) {
                    return false;
                }

                ec.clear();
                if (!std::filesystem::is_regular_file(sourcePath, ec) || ec) {
                    return false;
                }

                ec.clear();
                const std::uintmax_t size =
                    std::filesystem::file_size(sourcePath, ec);

                if (ec) {
                    return false;
                }

                ec.clear();
                const auto writeTime =
                    std::filesystem::last_write_time(sourcePath, ec);

                if (ec) {
                    return false;
                }

                out.fileSize = size;
                out.writeTicks =
                    static_cast<long long>(
                        writeTime.time_since_epoch().count());
                out.valid = true;
                return true;
            }
            catch (...) {
                out = {};
                return false;
            }
        }

        static std::wstring BuildBeforeArchiveStampText_(
            const BeforeArchiveSourceStamp_& stamp)
        {
            if (!stamp.valid) {
                return {};
            }

            std::wstring text;
            text.reserve(96);

            text += L"size=";
            text += std::to_wstring(stamp.fileSize);
            text += L"\n";

            text += L"ticks=";
            text += std::to_wstring(stamp.writeTicks);
            text += L"\n";

            return text;
        }

        static std::wstring NormalizeBeforeArchiveStampText_(
            std::wstring text)
        {
            if (!text.empty() && text.front() == L'\ufeff') {
                text.erase(text.begin());
            }

            return TrimAsciiWsW_(std::move(text));
        }

        static bool HasBeforeArchiveManifestSet_(
            const EpgHtmlPortal::GenerateFromEpgRequest& req) noexcept
        {
            return
                FileExistsNoThrow_(MakeBeforeArchiveFolderManifestPath_(req)) &&
                FileExistsNoThrow_(MakeBeforeArchiveSpaceManifestPath_(req)) &&
                FileExistsNoThrow_(MakeBeforeArchiveRootManifestPath_(req));
        }

        static bool IsBeforeArchiveCacheFresh_(
            const EpgHtmlPortal::GenerateFromEpgRequest& req)
        {
            if (req.beforeMergedEpgPath.empty()) {
                return false;
            }

            if (!FileExistsNoThrow_(req.beforeMergedEpgPath)) {
                return false;
            }

            if (!HasBeforeArchiveManifestSet_(req)) {
                return false;
            }

            BeforeArchiveSourceStamp_ sourceStamp{};
            if (!TryGetBeforeArchiveSourceStamp_(
                req.beforeMergedEpgPath,
                sourceStamp)) {
                return false;
            }

            const std::filesystem::path stampPath =
                MakeBeforeArchiveStampPath_(req);

            std::wstring saved;
            if (!EpgHtmlPortal_Common::ReadTextFileToWide(stampPath, saved)) {
                return false;
            }

            const std::wstring expected =
                NormalizeBeforeArchiveStampText_(
                    BuildBeforeArchiveStampText_(sourceStamp));

            const std::wstring actual =
                NormalizeBeforeArchiveStampText_(
                    std::move(saved));

            return !expected.empty() && actual == expected;
        }

        static bool WriteBeforeArchiveStamp_(
            const EpgHtmlPortal::GenerateFromEpgRequest& req)
        {
            if (req.beforeMergedEpgPath.empty()) {
                return false;
            }

            BeforeArchiveSourceStamp_ sourceStamp{};
            if (!TryGetBeforeArchiveSourceStamp_(
                req.beforeMergedEpgPath,
                sourceStamp)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][BEFORE][CACHE][WARN] source stamp read failed: %ls",
                    req.beforeMergedEpgPath.wstring().c_str());
                return false;
            }

            const std::filesystem::path stampPath =
                MakeBeforeArchiveStampPath_(req);

            try {
                std::error_code ec;
                std::filesystem::create_directories(
                    stampPath.parent_path(),
                    ec);

                if (ec) {
                    DebugLogFormat(
                        L"[EPG-PORTAL][BEFORE][CACHE][WARN] stamp dir create failed: %ls",
                        stampPath.parent_path().wstring().c_str());
                    return false;
                }
            }
            catch (...) {
                return false;
            }

            const bool ok =
                EpgHtmlPortal_Common::WriteUtf8BomTextFile(
                    stampPath,
                    BuildBeforeArchiveStampText_(sourceStamp));

            DebugLogFormat(
                ok
                ? L"[EPG-PORTAL][BEFORE][CACHE] stamp written: %ls"
                : L"[EPG-PORTAL][BEFORE][CACHE][WARN] stamp write failed: %ls",
                stampPath.wstring().c_str());

            return ok;
        }

        static std::filesystem::path MakeGeneratedBeforeSidecarPath_(
            const std::filesystem::path& outputHtmlPath)
        {
            if (outputHtmlPath.empty()) {
                return {};
            }

            const std::filesystem::path parent =
                outputHtmlPath.parent_path();

            const std::wstring stem =
                outputHtmlPath.stem().wstring();

            const std::wstring ext =
                outputHtmlPath.extension().wstring();

            std::wstring name;
            name.reserve(stem.size() + 16 + ext.size());

            name += stem;
            name += L"_before";

            if (!ext.empty()) {
                name += ext;
            }
            else {
                name += L".html";
            }

            if (parent.empty()) {
                return std::filesystem::path(name);
            }

            return parent / name;
        }

        static bool TryReadGeneratedBodyIfExists_(
            const std::filesystem::path& htmlPath,
            std::wstring& outBody,
            const wchar_t* logTag)
        {
            outBody.clear();

            if (!FileExistsNoThrow_(htmlPath)) {
                return false;
            }

            if (!ReadGeneratedBody_(htmlPath, outBody)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][BEFORE][WARN] %ls read failed: %ls",
                    logTag != nullptr ? logTag : L"generated body",
                    htmlPath.wstring().c_str());

                outBody.clear();
                return false;
            }

            DebugLogFormat(
                L"[EPG-PORTAL][BEFORE] %ls loaded: %ls chars=%zu",
                logTag != nullptr ? logTag : L"generated body",
                htmlPath.wstring().c_str(),
                outBody.size());

            return !outBody.empty();
        }

        static bool TryGenerateBeforeBodyFromEpg_(
            const EpgHtmlPortal::GenerateFromEpgRequest& req,
            const std::filesystem::path& tempBefore,
            std::wstring& outBeforeBody)
        {
            outBeforeBody.clear();

            if (req.beforeMergedEpgPath.empty()) {
                DebugLog(
                    L"[EPG-PORTAL][BEFORE] beforeMergedEpgPath is empty. before split skipped.");
                return false;
            }

            if (!FileExistsNoThrow_(req.beforeMergedEpgPath)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][BEFORE][WARN] beforeMergedEpgPath missing: %ls",
                    req.beforeMergedEpgPath.wstring().c_str());
                return false;
            }

            const std::filesystem::path sidecarBefore =
                MakeGeneratedBeforeSidecarPath_(tempBefore);

            std::error_code ec;

            // stale防止。
            std::filesystem::remove(tempBefore, ec);
            ec.clear();

            if (!sidecarBefore.empty() && sidecarBefore != tempBefore) {
                std::filesystem::remove(sidecarBefore, ec);
                ec.clear();
            }

            if (!GenerateEITHtmlProgramTable(
                req.beforeMergedEpgPath.wstring(),
                req.channelListPath.wstring(),
                tempBefore.wstring())) {
                DebugLogFormat(
                    L"[EPG-PORTAL][BEFORE][WARN] temp before generation failed: %ls",
                    req.beforeMergedEpgPath.wstring().c_str());
                return false;
            }

            // 通常出力をまず読む。
            if (TryReadGeneratedBodyIfExists_(
                tempBefore,
                outBeforeBody,
                L"temp before body")) {
                DebugLogFormat(
                    L"[EPG-PORTAL][BEFORE] before body generated: epg=%ls chars=%zu source=%ls",
                    req.beforeMergedEpgPath.wstring().c_str(),
                    outBeforeBody.size(),
                    tempBefore.wstring().c_str());

                return true;
            }

            // GenerateEITHtmlProgramTable() が before sidecar 側だけを出す場合はこちら。
            if (!sidecarBefore.empty() &&
                sidecarBefore != tempBefore &&
                TryReadGeneratedBodyIfExists_(
                    sidecarBefore,
                    outBeforeBody,
                    L"temp before sidecar body")) {
                DebugLogFormat(
                    L"[EPG-PORTAL][BEFORE] before body generated from sidecar: epg=%ls chars=%zu source=%ls",
                    req.beforeMergedEpgPath.wstring().c_str(),
                    outBeforeBody.size(),
                    sidecarBefore.wstring().c_str());

                return true;
            }

            DebugLogFormat(
                L"[EPG-PORTAL][BEFORE][WARN] generated before body not found: main=%ls sidecar=%ls",
                tempBefore.wstring().c_str(),
                sidecarBefore.empty()
                ? L"(empty)"
                : sidecarBefore.wstring().c_str());

            outBeforeBody.clear();
            return false;
        }

        static bool TryReadBeforeBodyFromSidecarTemp_(
            const std::filesystem::path& tempBefore,
            std::wstring& outBeforeBody)
        {
            outBeforeBody.clear();

            if (!FileExistsNoThrow_(tempBefore)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][BEFORE] sidecar temp before not found: %ls",
                    tempBefore.wstring().c_str());
                return false;
            }

            if (!ReadGeneratedBody_(tempBefore, outBeforeBody)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][BEFORE][WARN] sidecar temp before read failed: %ls",
                    tempBefore.wstring().c_str());
                outBeforeBody.clear();
                return false;
            }

            DebugLogFormat(
                L"[EPG-PORTAL][BEFORE] before body loaded from sidecar temp: %ls chars=%zu",
                tempBefore.wstring().c_str(),
                outBeforeBody.size());

            return !outBeforeBody.empty();
        }

        static bool WriteOneProgramPage_(
            const EpgHtmlPortal::WriteOptions& opt,
            int space,
            EpgHtmlPortal_ProgramPageWriter::ProgramPageKind kind,
            EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind viewKind,
            const std::wstring& outputFileName,
            const std::wstring& title,
            const std::wstring& body,
            const std::vector<EpgHtmlPortal_ProgramPageWriter::ChannelServiceInfo>& services,
            bool writeServiceMap)
        {
            EpgHtmlPortal_ProgramPageWriter::ProgramPageRequest pageReq{};
            pageReq.space = space;
            pageReq.kind = kind;
            pageReq.viewKind = viewKind;
            pageReq.outputFileName = outputFileName;
            pageReq.htmlTitle = title;
            pageReq.spaceLabel = MakeSpaceLabel_(space);

            pageReq.tableHtml = body;

            if (viewKind == EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::Timetable) {
                pageReq.timetableHtml = body;
            }
            else if (viewKind == EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::List) {
                pageReq.listHtml = body;
            }

            pageReq.services = services;
            pageReq.writeServiceMap = writeServiceMap;
            pageReq.serviceMapFileName = L"epg_service_map.js";

            if (viewKind == EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::Timetable) {
                return EpgHtmlPortal_ProgramPageWriter::WriteProgramTimetablePage(
                    opt,
                    pageReq);
            }

            if (viewKind == EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::List) {
                return EpgHtmlPortal_ProgramPageWriter::WriteProgramListPage(
                    opt,
                    pageReq);
            }

            return EpgHtmlPortal_ProgramPageWriter::WriteProgramPage(
                opt,
                pageReq);
        }

        struct BeforeArchiveDayPage_
        {
            std::wstring daySlash;
            std::wstring ymd;

            std::wstring timetableHtml;
            std::wstring listHtml;

            std::wstring timetableSrc;
            std::wstring listSrc;
        };

        static bool IsBeforeOutputRequested_(
            const EpgHtmlPortal::GenerateFromEpgRequest& req) noexcept
        {
            return req.generateBeforeLegacy ||
                req.generateBeforeTimetable ||
                req.generateBeforeList ||
                req.generatePastArchive;
        }

        static std::wstring CompactYmdFromSlashDate_(
            std::wstring_view day)
        {
            if (day.size() == 10 &&
                day[4] == L'/' &&
                day[7] == L'/') {
                std::wstring ymd;
                ymd.reserve(8);
                ymd.append(day.substr(0, 4));
                ymd.append(day.substr(5, 2));
                ymd.append(day.substr(8, 2));
                return ymd;
            }

            std::wstring ymd;
            ymd.reserve(8);

            for (const wchar_t ch : day) {
                if (ch >= L'0' && ch <= L'9') {
                    ymd.push_back(ch);
                }
            }

            return ymd.size() == 8 ? ymd : std::wstring{};
        }

        static BeforeArchiveDayPage_* FindOrAppendBeforeArchiveDay_(
            std::vector<BeforeArchiveDayPage_>& days,
            const std::wstring& daySlash)
        {
            const std::wstring ymd =
                CompactYmdFromSlashDate_(daySlash);

            if (ymd.empty()) {
                return nullptr;
            }

            for (auto& day : days) {
                if (day.ymd == ymd) {
                    return &day;
                }
            }

            BeforeArchiveDayPage_ item{};
            item.daySlash = daySlash;
            item.ymd = ymd;

            days.push_back(std::move(item));
            return &days.back();
        }

        static std::wstring MakeBeforeArchiveFolderName_(
            int space)
        {
            return L"epg_space" + std::to_wstring(space) + L"_before";
        }

        static std::wstring MakeBeforeArchiveDayFileName_(
            int space,
            const std::wstring& ymd,
            EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind viewKind)
        {
            std::wstring name;
            name.reserve(96);

            const std::wstring folder =
                MakeBeforeArchiveFolderName_(space);

            name += folder;
            name += L"/";
            name += ymd;

            if (viewKind == EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::List) {
                name += L"_list.html";
            }
            else {
                name += L"_timetable.html";
            }

            return name;
        }

        static std::vector<BeforeArchiveDayPage_> CollectBeforeArchiveDays_(
            const std::wstring& beforeBody)
        {
            std::vector<BeforeArchiveDayPage_> days;

            if (beforeBody.empty()) {
                return days;
            }

            std::vector<std::wstring> grids;
            if (ExtractAllElementsByClassTokensForPortalSplit_(
                beforeBody,
                {
                    L"grid-view",
                    L"timetable-view",
                    L"epg-grid-view",
                    L"program-grid-view"
                },
                grids)) {
                for (const auto& grid : grids) {
                    const std::wstring daySlash =
                        ExtractDateFromTimetableGridForPortal_(grid);

                    auto* item =
                        FindOrAppendBeforeArchiveDay_(
                            days,
                            daySlash);

                    if (item != nullptr && item->timetableHtml.empty()) {
                        item->timetableHtml = grid;
                    }
                }
            }

            std::vector<std::wstring> lists;
            if (ExtractAllElementsByClassTokensForPortalSplit_(
                beforeBody,
                {
                    L"genre-view",
                    L"list-view",
                    L"program-list-view"
                },
                lists)) {
                const std::vector<std::wstring> buttonDays =
                    ExtractDayButtonDaysForPortal_(beforeBody);

                for (std::size_t i = 0; i < lists.size(); ++i) {
                    std::wstring daySlash;

                    if (i < buttonDays.size()) {
                        daySlash = buttonDays[i];
                    }

                    if (daySlash.empty()) {
                        daySlash = GetFirstProgramDateFromListViewForPortal_(lists[i]);
                    }

                    auto* item =
                        FindOrAppendBeforeArchiveDay_(
                            days,
                            daySlash);

                    if (item != nullptr && item->listHtml.empty()) {
                        item->listHtml = lists[i];
                    }
                }
            }

            std::sort(
                days.begin(),
                days.end(),
                [](const BeforeArchiveDayPage_& a,
                    const BeforeArchiveDayPage_& b) {
                        return a.ymd > b.ymd;
                });

            return days;
        }

        static std::wstring MakeBeforeArchiveLandingBody_(
            int space,
            EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind viewKind)
        {
            const bool listView =
                viewKind == EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::List;

            const std::wstring viewName =
                listView ? L"list" : L"timetable";

            const std::wstring viewLabel =
                listView ? L"一覧" : L"時間割";

            const std::wstring folder =
                MakeBeforeArchiveFolderName_(space);

            std::wstring body;
            body.reserve(8192);

            body += L"<div class=\"epg-before-archive-entry\" data-space=\"";
            body += std::to_wstring(space);
            body += L"\" data-view=\"";
            body += EscapeAttr_(viewName);
            body += L"\">\n";

            body += L"<style>\n";
            body += L".epg-before-archive-entry{display:grid;gap:12px;padding:16px;border:1px solid #d7deea;border-radius:12px;background:#fff;}\n";
            body += L".epg-before-archive-entry h2{margin:0;font-size:18px;}\n";
            body += L".epg-before-archive-entry p{margin:0;color:#536070;line-height:1.65;}\n";
            body += L".epg-before-archive-entry .before-controls{display:flex;flex-wrap:wrap;gap:8px;align-items:center;}\n";
            body += L".epg-before-archive-entry select{min-width:180px;padding:8px;border:1px solid #c9d3e2;border-radius:8px;}\n";
            body += L".epg-before-archive-entry button,.epg-before-archive-entry a{padding:8px 12px;border:0;border-radius:8px;background:#2d6cdf;color:#fff;text-decoration:none;font:inherit;}\n";
            body += L".epg-before-archive-entry button[disabled]{opacity:.45;cursor:not-allowed;}\n";
            body += L".epg-before-archive-entry .before-status{padding:10px 12px;border-radius:8px;background:#f3f6fb;color:#445066;}\n";
            body += L"</style>\n";

            body += L"<h2>過去番組表 ";
            body += EpgHtmlPortal_Common::EscapeHtmlW(viewLabel);
            body += L"</h2>\n";

            body += L"<p>";
            body += L"過去番組表は日付別に分割されています。";
            body += L"日付を選ぶと epg_spaceX_before フォルダ内の1日分だけを開きます。";
            body += L"</p>\n";

            body += L"<div class=\"before-controls\">\n";
            body += L"<label>表示日 ";
            body += L"<select class=\"before-date-select\" aria-label=\"過去番組表の日付\"></select>";
            body += L"</label>\n";
            body += L"<button type=\"button\" class=\"before-open-button\">表示</button>\n";
            body += L"<a class=\"before-open-link\" href=\"#\" target=\"_blank\" rel=\"noopener\">別タブで開く</a>\n";
            body += L"</div>\n";

            body += L"<div class=\"before-status\">過去番組表データを確認しています...</div>\n";
            body += L"</div>\n";

            body += L"<script src=\"";
            body += EscapeAttr_(folder);
            body += L"/manifest.js\"></script>\n";

            body += L"<script>\n";
            body += L"(function(){\n";
            body += L"'use strict';\n";
            body += L"const space=";
            body += std::to_wstring(space);
            body += L";\n";
            body += L"const view=";
            body += EpgHtmlPortal_Common::ToJsStringLiteral(viewName);
            body += L";\n";
            body += L"function ymd(v){const s=String(v||'').replace(/[^0-9]/g,'');return s.length===8?s:'';}\n";
            body += L"function label(d){d=ymd(d);return d?d.slice(0,4)+'/'+d.slice(4,6)+'/'+d.slice(6,8):'';}\n";
            body += L"function manifest(){try{if(window.EPG_SPACE_BEFORE_MANIFEST)return window.EPG_SPACE_BEFORE_MANIFEST;}catch(_){ }try{if(window.EPG_PAST_MANIFEST)return window.EPG_PAST_MANIFEST;}catch(_){ }return {};}\n";
            body += L"function entries(){const m=manifest();const a=m[String(space)]||m[space]||[];return Array.isArray(a)?a.slice().sort(function(x,y){return ymd(y.date).localeCompare(ymd(x.date));}):[];}\n";
            body += L"function srcOf(e){if(!e)return'';if(view==='list')return String(e.listSrc||'');return String(e.timetableSrc||'');}\n";
            body += L"function render(){\n";
            body += L"const root=document.querySelector('.epg-before-archive-entry[data-space=\"'+String(space)+'\"]');\n";
            body += L"if(!root)return;\n";
            body += L"const sel=root.querySelector('.before-date-select');\n";
            body += L"const btn=root.querySelector('.before-open-button');\n";
            body += L"const link=root.querySelector('.before-open-link');\n";
            body += L"const status=root.querySelector('.before-status');\n";
            body += L"const list=entries();\n";
            body += L"if(sel)sel.textContent='';\n";
            body += L"if(list.length===0){\n";
            body += L"if(status)status.textContent='過去番組表の分割データがありません。';\n";
            body += L"if(btn)btn.disabled=true;\n";
            body += L"if(link){link.href='#';link.style.opacity='.45';link.style.pointerEvents='none';}\n";
            body += L"return;\n";
            body += L"}\n";
            body += L"list.forEach(function(e){const opt=document.createElement('option');opt.value=ymd(e.date);opt.textContent=label(e.date);if(sel)sel.appendChild(opt);});\n";
            body += L"function selected(){const d=sel?ymd(sel.value):ymd(list[0]&&list[0].date);for(const e of list){if(ymd(e.date)===d)return e;}return list[0]||null;}\n";
            body += L"function update(){const e=selected();const src=srcOf(e);if(link){link.href=src||'#';link.style.opacity=src?'1':'.45';link.style.pointerEvents=src?'auto':'none';}if(status)status.textContent=src?('選択中: '+label(e.date)):'表示先がありません。';return src;}\n";
            body += L"if(sel)sel.addEventListener('change',update);\n";
            body += L"if(btn)btn.addEventListener('click',function(){const src=update();if(src)window.location.href=src;});\n";
            body += L"update();\n";
            body += L"}\n";
            body += L"if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',render,{once:true});else render();\n";
            body += L"})();\n";
            body += L"</script>\n";

            return body;
        }

        static std::wstring BuildBeforeManifestScript_(
            int space,
            const std::vector<BeforeArchiveDayPage_>& days,
            bool useSpaceBeforeName,
            bool writeReadyFlag = true)
        {
            std::wstring js;
            js.reserve(days.size() * 256 + 1024);

            if (useSpaceBeforeName) {
                js += L"window.EPG_SPACE_BEFORE_MANIFEST = window.EPG_SPACE_BEFORE_MANIFEST || {};\n";
                js += L"window.EPG_SPACE_BEFORE_MANIFEST[";
            }
            else {
                js += L"window.EPG_PAST_MANIFEST = window.EPG_PAST_MANIFEST || {};\n";
                js += L"window.EPG_PAST_MANIFEST[";
            }

            js += EpgHtmlPortal_Common::ToJsStringLiteral(std::to_wstring(space));
            js += L"] = [\n";

            bool first = true;

            for (const auto& day : days) {
                if (day.ymd.empty()) {
                    continue;
                }

                if (day.timetableSrc.empty() && day.listSrc.empty()) {
                    continue;
                }

                if (!first) {
                    js += L",\n";
                }

                first = false;

                js += L"  { date: ";
                js += EpgHtmlPortal_Common::ToJsStringLiteral(day.ymd);

                if (!day.timetableSrc.empty()) {
                    js += L", timetableSrc: ";
                    js += EpgHtmlPortal_Common::ToJsStringLiteral(day.timetableSrc);
                }

                if (!day.listSrc.empty()) {
                    js += L", listSrc: ";
                    js += EpgHtmlPortal_Common::ToJsStringLiteral(day.listSrc);
                }

                js += L" }";
            }

            js += L"\n];\n";

            if (writeReadyFlag) {
                if (useSpaceBeforeName) {
                    js += L"window.EPG_SPACE_BEFORE_MANIFEST_READY = true;\n";
                }
                else {
                    js += L"window.EPG_PAST_MANIFEST_READY = true;\n";
                }
            }

            return js;
        }

        static void CleanBeforeArchiveGeneratedFiles_(
            const std::filesystem::path& folderPath) noexcept
        {
            std::error_code ec;

            if (!std::filesystem::exists(folderPath, ec) || ec) {
                return;
            }

            ec.clear();
            if (!std::filesystem::is_directory(folderPath, ec) || ec) {
                return;
            }

            for (std::filesystem::directory_iterator it(
                folderPath,
                std::filesystem::directory_options::skip_permission_denied,
                ec),
                end;
                it != end;
                it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }

                const auto& entry = *it;

                ec.clear();
                if (!entry.is_regular_file(ec) || ec) {
                    ec.clear();
                    continue;
                }

                const std::filesystem::path p = entry.path();
                const std::wstring nameLower =
                    ToLowerCopy_(p.filename().wstring());

                const std::wstring extLower =
                    ToLowerCopy_(p.extension().wstring());

                const bool generatedHtml =
                    extLower == L".html" &&
                    (
                        nameLower.find(L"_timetable.html") != std::wstring::npos ||
                        nameLower.find(L"_list.html") != std::wstring::npos
                        );

                const bool generatedManifest =
                    nameLower == L"manifest.js";

                if (generatedHtml || generatedManifest) {
                    ec.clear();
                    std::filesystem::remove(p, ec);
                    ec.clear();
                }
            }
        }

        static std::filesystem::path MakePastDataSpaceManifestPath_(
            const std::filesystem::path& outputDir,
            int space)
        {
            return outputDir /
                L"past_data" /
                (L"manifest_space" + std::to_wstring(space) + L".js");
        }

        static void StripWideBomIfExists_(
            std::wstring& text)
        {
            if (!text.empty() && text.front() == L'\ufeff') {
                text.erase(text.begin());
            }
        }

        static std::wstring BuildMergedPastDataManifestScript_(
            const std::filesystem::path& outputDir)
        {
            const std::filesystem::path pastDataDir =
                outputDir / L"past_data";

            std::wstring js;
            js.reserve(16 * 1024);

            js += L"window.EPG_PAST_MANIFEST = window.EPG_PAST_MANIFEST || {};\n";
            js += L"window.EPG_PAST_MANIFEST_READY = false;\n";

            for (int space = 0; space <= 2; ++space) {
                const std::filesystem::path path =
                    pastDataDir /
                    (L"manifest_space" + std::to_wstring(space) + L".js");

                std::wstring part;
                if (EpgHtmlPortal_Common::ReadTextFileToWide(path, part)) {
                    StripWideBomIfExists_(part);

                    js += L"\n// space";
                    js += std::to_wstring(space);
                    js += L"\n";

                    js += part;

                    if (!part.empty() && part.back() != L'\n') {
                        js += L'\n';
                    }
                }
                else {
                    js += L"window.EPG_PAST_MANIFEST[";
                    js += EpgHtmlPortal_Common::ToJsStringLiteral(std::to_wstring(space));
                    js += L"] = window.EPG_PAST_MANIFEST[";
                    js += EpgHtmlPortal_Common::ToJsStringLiteral(std::to_wstring(space));
                    js += L"] || [];\n";
                }
            }

            js += L"window.EPG_PAST_MANIFEST_READY = true;\n";

            return js;
        }

        static bool WriteBeforeArchiveManifests_(
            const std::filesystem::path& outputDir,
            int space,
            const std::vector<BeforeArchiveDayPage_>& days)
        {
            const std::wstring folder =
                MakeBeforeArchiveFolderName_(space);

            const std::filesystem::path folderDir =
                outputDir / folder;

            const std::filesystem::path pastDataDir =
                outputDir / L"past_data";

            std::error_code ec;
            std::filesystem::create_directories(folderDir, ec);
            if (ec) {
                DebugLogFormat(
                    L"[EPG-PORTAL][BEFORE][WARN] before folder create failed: %ls",
                    folderDir.wstring().c_str());
                return false;
            }

            ec.clear();
            std::filesystem::create_directories(pastDataDir, ec);
            if (ec) {
                DebugLogFormat(
                    L"[EPG-PORTAL][BEFORE][WARN] past_data folder create failed: %ls",
                    pastDataDir.wstring().c_str());
                return false;
            }

            const std::filesystem::path folderManifestPath =
                folderDir / L"manifest.js";

            const std::filesystem::path spacePastManifestPath =
                MakePastDataSpaceManifestPath_(
                    outputDir,
                    space);

            const std::filesystem::path rootManifestPath =
                pastDataDir / L"manifest.js";

            bool ok = true;

            // epg_spaceX_before/manifest.js
            // 入口ページ専用。該当 space だけでよい。
            ok = EpgHtmlPortal_Common::WriteUtf8BomTextFile(
                folderManifestPath,
                BuildBeforeManifestScript_(
                    space,
                    days,
                    true,
                    true)) && ok;

            // past_data/manifest_spaceX.js
            // space別保存。これを残しておくことで、space0/1/2 の生成順に依存しない。
            ok = EpgHtmlPortal_Common::WriteUtf8BomTextFile(
                spacePastManifestPath,
                BuildBeforeManifestScript_(
                    space,
                    days,
                    false,
                    false)) && ok;

            // past_data/manifest.js
            // index.html 互換用。manifest_space0/1/2.js を合成して作る。
            ok = EpgHtmlPortal_Common::WriteUtf8BomTextFile(
                rootManifestPath,
                BuildMergedPastDataManifestScript_(outputDir)) && ok;

            return ok;
        }



        static bool WriteBeforeArchiveDailyPages_(
            const EpgHtmlPortal::GenerateFromEpgRequest& req,
            const EpgHtmlPortal::WriteOptions& opt,
            const std::wstring& baseTitle,
            const std::wstring& beforeBody,
            const std::vector<EpgHtmlPortal_ProgramPageWriter::ChannelServiceInfo>& services,
            std::vector<std::filesystem::path>& generatedFiles)
        {
            std::vector<BeforeArchiveDayPage_> days =
                CollectBeforeArchiveDays_(beforeBody);

            const std::wstring folder =
                MakeBeforeArchiveFolderName_(req.space);

            const std::filesystem::path folderPath =
                req.outputDir / folder;

            std::error_code ec;
            std::filesystem::create_directories(folderPath, ec);

            if (ec) {
                DebugLogFormat(
                    L"[EPG-PORTAL][BEFORE][ERROR] archive folder create failed: %ls",
                    folderPath.wstring().c_str());
                return false;
            }

            CleanBeforeArchiveGeneratedFiles_(folderPath);

            bool ok = true;

            for (auto& day : days) {
                if (day.ymd.empty()) {
                    continue;
                }

                if (!day.timetableHtml.empty()) {
                    day.timetableSrc =
                        MakeBeforeArchiveDayFileName_(
                            req.space,
                            day.ymd,
                            EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::Timetable);

                    EpgHtmlPortal_ProgramPageWriter::ProgramPageRequest pageReq{};
                    pageReq.space = req.space;
                    pageReq.kind = EpgHtmlPortal_ProgramPageWriter::ProgramPageKind::Before;
                    pageReq.viewKind = EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::Timetable;
                    pageReq.outputFileName = day.timetableSrc;
                    pageReq.htmlTitle = baseTitle + L" 過去 " + day.daySlash + L" 時間割";
                    pageReq.spaceLabel = MakeSpaceLabel_(req.space);
                    pageReq.archiveDateKey = day.ymd;
                    pageReq.archiveDateLabel = day.daySlash;
                    pageReq.timetableHtml =
                        WrapSplitBodyForPortal_(
                            L"epg-separated-timetable epg-before-day-page",
                            L"過去番組表の日付分割ページです。タイムシフト連携用の属性を保持します。",
                            day.timetableHtml);
                    pageReq.services = services;
                    pageReq.enableReservationSwitch = false;
                    pageReq.enableTimeShiftSwitch = true;

                    ok = EpgHtmlPortal_ProgramPageWriter::WritePastProgramTimetablePage(
                        opt,
                        pageReq) && ok;

                    generatedFiles.push_back(
                        OutputPath_(req.outputDir, day.timetableSrc));
                }

                if (!day.listHtml.empty()) {
                    day.listSrc =
                        MakeBeforeArchiveDayFileName_(
                            req.space,
                            day.ymd,
                            EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::List);

                    EpgHtmlPortal_ProgramPageWriter::ProgramPageRequest pageReq{};
                    pageReq.space = req.space;
                    pageReq.kind = EpgHtmlPortal_ProgramPageWriter::ProgramPageKind::Before;
                    pageReq.viewKind = EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::List;
                    pageReq.outputFileName = day.listSrc;
                    pageReq.htmlTitle = baseTitle + L" 過去 " + day.daySlash + L" 一覧";
                    pageReq.spaceLabel = MakeSpaceLabel_(req.space);
                    pageReq.archiveDateKey = day.ymd;
                    pageReq.archiveDateLabel = day.daySlash;
                    pageReq.listHtml =
                        WrapSplitBodyForPortal_(
                            L"epg-separated-list epg-before-day-page",
                            L"過去番組表の日付分割ページです。タイムシフト連携用の属性を保持します。",
                            day.listHtml);
                    pageReq.services = services;
                    pageReq.enableReservationSwitch = false;
                    pageReq.enableTimeShiftSwitch = true;

                    ok = EpgHtmlPortal_ProgramPageWriter::WritePastProgramListPage(
                        opt,
                        pageReq) && ok;

                    generatedFiles.push_back(
                        OutputPath_(req.outputDir, day.listSrc));
                }
            }

            if (!WriteBeforeArchiveManifests_(
                req.outputDir,
                req.space,
                days)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][BEFORE][WARN] before archive manifest write failed: space=%d",
                    req.space);
                ok = false;
            }

            generatedFiles.push_back(req.outputDir / folder / L"manifest.js");
            generatedFiles.push_back(
                MakePastDataSpaceManifestPath_(
                    req.outputDir,
                    req.space));
            generatedFiles.push_back(req.outputDir / L"past_data" / L"manifest.js");

            DebugLogFormat(
                L"[EPG-PORTAL][BEFORE] split archive generated: space=%d days=%zu ok=%d",
                req.space,
                days.size(),
                ok ? 1 : 0);

            return ok;
        }
	}// namespace

    bool EnsurePlaceholderHtml(
        const std::filesystem::path& htmlPath,
        std::wstring_view title,
        std::wstring_view message)
    {
        return EpgHtmlPortal_index::EnsurePlaceholderHtml(
            htmlPath,
            title,
            message);
    }

    bool WritePortalIndexHtml(
        const WriteOptions& opt)
    {
        return EpgHtmlPortal_index::WritePortalIndexHtml(opt);
    }

    GenerateFromEpgResult GenerateFromEpg(
        const GenerateFromEpgRequest& req)
    {
        GenerateFromEpgResult result{};

        if (req.outputDir.empty() ||
            req.mergedEpgPath.empty() ||
            req.channelListPath.empty()) {
            DebugLog(L"[EPG-PORTAL][GEN][ERROR] invalid request");
            return result;
        }

        std::error_code ec;
        std::filesystem::create_directories(req.outputDir, ec);
        if (ec) {
            DebugLogFormat(
                L"[EPG-PORTAL][GEN][ERROR] outputDir create failed: %ls",
                req.outputDir.wstring().c_str());
            return result;
        }

        const std::wstring sp = std::to_wstring(req.space);

        const std::wstring currentLegacyName =
            DefaultFileName_(
                req.currentLegacyFileName,
                L"epg_space" + sp + L".html");

        const std::wstring currentTimetableName =
            DefaultFileName_(
                req.currentTimetableFileName,
                L"epg_space" + sp + L"_timetable.html");

        const std::wstring currentListName =
            DefaultFileName_(
                req.currentListFileName,
                L"epg_space" + sp + L"_list.html");

        const std::wstring beforeLegacyName =
            DefaultFileName_(
                req.beforeLegacyFileName,
                L"epg_space" + sp + L"_before.html");

        const std::wstring beforeTimetableName =
            DefaultFileName_(
                req.beforeTimetableFileName,
                L"epg_space" + sp + L"_before_timetable.html");

        const std::wstring beforeListName =
            DefaultFileName_(
                req.beforeListFileName,
                L"epg_space" + sp + L"_before_list.html");

        const std::filesystem::path tempCurrent =
            req.outputDir / (L"epg_space" + sp + L".portal_tmp.html");

        const std::filesystem::path tempBefore =
            req.outputDir / (L"epg_space" + sp + L".portal_tmp_before.html");

        std::filesystem::remove(tempCurrent, ec);
        ec.clear();
        std::filesystem::remove(tempBefore, ec);

        if (!GenerateEITHtmlProgramTable(
            req.mergedEpgPath.wstring(),
            req.channelListPath.wstring(),
            tempCurrent.wstring())) {
            DebugLog(L"[EPG-PORTAL][GEN][ERROR] temporary legacy generation failed");
            return result;
        }

        std::wstring currentBody;
        if (!ReadGeneratedBody_(tempCurrent, currentBody)) {
            DebugLogFormat(
                L"[EPG-PORTAL][GEN][ERROR] temp current read failed: %ls",
                tempCurrent.wstring().c_str());
            return result;
        }

        const bool beforeRequested =
            IsBeforeOutputRequested_(req);

        // 明示的に過去アーカイブ生成を要求しているか。
        // beforeMergedEpgPath が空でも、既存生成器が sidecar の
        // epg_spaceX.portal_tmp_before.html を吐く場合があるため、
        // beforeRequested とは分けて扱う。
        const bool beforeArchiveSourceRequested =
            req.generatePastArchive ||
            !req.beforeMergedEpgPath.empty();

        std::wstring beforeBody;

        const bool beforeArchiveCacheFresh =
            beforeArchiveSourceRequested &&
            IsBeforeArchiveCacheFresh_(req);

        if (beforeArchiveCacheFresh) {
            DebugLogFormat(
                L"[EPG-PORTAL][BEFORE][CACHE] fresh. daily archive generation skipped: space=%d beforeEpg=%ls",
                req.space,
                req.beforeMergedEpgPath.wstring().c_str());
        }
        else {
            if (!req.beforeMergedEpgPath.empty()) {
                (void)TryGenerateBeforeBodyFromEpg_(
                    req,
                    tempBefore,
                    beforeBody);
            }

            // old / beforeMergedEpgPath が無い現環境でも、
            // GenerateEITHtmlProgramTable() が tempBefore を sidecar 出力している場合は拾う。
            if (beforeBody.empty() && beforeRequested) {
                (void)TryReadBeforeBodyFromSidecarTemp_(
                    tempBefore,
                    beforeBody);
            }
        }

        

        const std::wstring beforeLegacyBody =
            beforeRequested
            ? MakeBeforeArchiveLandingBody_(
                req.space,
                EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::Timetable)
            : std::wstring{};

        const std::wstring beforeTimetableBody =
            beforeRequested
            ? MakeBeforeArchiveLandingBody_(
                req.space,
                EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::Timetable)
            : std::wstring{};

        const std::wstring beforeListBody =
            beforeRequested
            ? MakeBeforeArchiveLandingBody_(
                req.space,
                EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::List)
            : std::wstring{};

        const auto services =
            LoadServiceMapFromChList_(
                req.channelListPath,
                req.space);

        WriteOptions opt{};
        opt.space = req.space;
        opt.outputDir = req.outputDir;

        opt.currentHtmlPath =
            OutputPath_(req.outputDir, currentLegacyName);

        opt.beforeHtmlPath =
            OutputPath_(req.outputDir, beforeLegacyName);

        bool ok = true;

        const std::wstring baseTitle =
            req.title.empty()
            ? L"EIT 番組表"
            : req.title;

        if (req.generateCurrentLegacy) {
            result.currentLegacyPath =
                OutputPath_(req.outputDir, currentLegacyName);

            ok = WriteOneProgramPage_(
                opt,
                req.space,
                EpgHtmlPortal_ProgramPageWriter::ProgramPageKind::Current,
                EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::Legacy,
                currentLegacyName,
                baseTitle,
                currentBody,
                services,
                true) && ok;

            result.generatedFiles.push_back(result.currentLegacyPath);
        }

        if (req.generateCurrentTimetable) {
            const std::wstring body =
                BuildTimetableBodyFromLegacyForPortal_(currentBody);

            result.currentTimetablePath =
                OutputPath_(req.outputDir, currentTimetableName);

            ok = WriteOneProgramPage_(
                opt,
                req.space,
                EpgHtmlPortal_ProgramPageWriter::ProgramPageKind::Current,
                EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::Timetable,
                currentTimetableName,
                baseTitle + L" 時間割",
                body,
                services,
                false) && ok;

            result.generatedFiles.push_back(result.currentTimetablePath);
        }

        if (req.generateCurrentList) {
            const std::wstring body =
                BuildListBodyFromLegacyForPortal_(currentBody);

            result.currentListPath =
                OutputPath_(req.outputDir, currentListName);

            ok = WriteOneProgramPage_(
                opt,
                req.space,
                EpgHtmlPortal_ProgramPageWriter::ProgramPageKind::Current,
                EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::List,
                currentListName,
                baseTitle + L" 一覧",
                body,
                services,
                false) && ok;

            result.generatedFiles.push_back(result.currentListPath);
        }

        if (req.generateBeforeLegacy) {
            result.beforeLegacyPath =
                OutputPath_(req.outputDir, beforeLegacyName);

            ok = WriteOneProgramPage_(
                opt,
                req.space,
                EpgHtmlPortal_ProgramPageWriter::ProgramPageKind::Before,
                EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::Legacy,
                beforeLegacyName,
                baseTitle + L" 過去",
                beforeLegacyBody,
                services,
                false) && ok;

            result.generatedFiles.push_back(result.beforeLegacyPath);
        }

        if (req.generateBeforeTimetable) {
            result.beforeTimetablePath =
                OutputPath_(req.outputDir, beforeTimetableName);

            ok = WriteOneProgramPage_(
                opt,
                req.space,
                EpgHtmlPortal_ProgramPageWriter::ProgramPageKind::Before,
                EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::Timetable,
                beforeTimetableName,
                baseTitle + L" 過去 時間割",
                beforeTimetableBody,
                services,
                false) && ok;

            result.generatedFiles.push_back(result.beforeTimetablePath);
        }

        if (req.generateBeforeList) {
            result.beforeListPath =
                OutputPath_(req.outputDir, beforeListName);

            ok = WriteOneProgramPage_(
                opt,
                req.space,
                EpgHtmlPortal_ProgramPageWriter::ProgramPageKind::Before,
                EpgHtmlPortal_ProgramPageWriter::ProgramPageViewKind::List,
                beforeListName,
                baseTitle + L" 過去 一覧",
                beforeListBody,
                services,
                false) && ok;

            result.generatedFiles.push_back(result.beforeListPath);
        }

        if (beforeArchiveCacheFresh) {
            // beforeMerged が前回生成時から変わっていないため、
            // 既存の日別HTML / manifest をそのまま使う。
            // beforeBody は生成していないので、空でも警告にしない。
            DebugLogFormat(
                L"[EPG-PORTAL][BEFORE][CACHE] daily archive reuse: space=%d beforeEpg=%ls",
                req.space,
                req.beforeMergedEpgPath.empty()
                ? L"(empty)"
                : req.beforeMergedEpgPath.wstring().c_str());
        }
        else if (beforeRequested && !beforeBody.empty()) {
            const bool archiveOk =
                WriteBeforeArchiveDailyPages_(
                    req,
                    opt,
                    baseTitle,
                    beforeBody,
                    services,
                    result.generatedFiles);

            ok = archiveOk && ok;

            if (archiveOk) {
                (void)WriteBeforeArchiveStamp_(req);
            }

            beforeBody.clear();
            beforeBody.shrink_to_fit();
        }
        else if (beforeArchiveSourceRequested) {
            // 明示的に過去アーカイブ生成を要求しているのに入力が空の場合だけログを出す。
            // 入口ページだけ生成したいケースでは、既存 manifest を空で上書きしない。
            DebugLogFormat(
                L"[EPG-PORTAL][BEFORE][WARN] before archive source requested but source body is empty. keep existing manifests. space=%d beforeEpg=%ls",
                req.space,
                req.beforeMergedEpgPath.empty()
                ? L"(empty)"
                : req.beforeMergedEpgPath.wstring().c_str());
        }

        if (req.generateIndex) {
            WriteOptions indexOpt = opt;
            indexOpt.indexFileName =
                req.indexFileName.empty()
                ? std::filesystem::path(L"index.html")
                : std::filesystem::path(req.indexFileName);

            indexOpt.portalTitle =
                req.title.empty()
                ? std::wstring(L"EPGポータル")
                : (req.title + L" ポータル");

            indexOpt.mergedEpgPathsBySpace.assign(
                3,
                std::filesystem::path{});

            if (req.space >= 0 && req.space <= 2) {
                indexOpt.mergedEpgPathsBySpace[
                    static_cast<std::size_t>(req.space)] =
                    req.mergedEpgPath;
            }

            ok = WritePortalIndexHtml(indexOpt) && ok;

            result.indexPath = req.outputDir / indexOpt.indexFileName;
            result.generatedFiles.push_back(result.indexPath);

            const std::filesystem::path autoReservePagePath =
                EpgHtmlPortal_Auto::MakeAutoReservePagePath(indexOpt);

            if (!autoReservePagePath.empty()) {
                result.generatedFiles.push_back(autoReservePagePath);
            }
        }

        std::filesystem::remove(tempCurrent, ec);
        ec.clear();
        std::filesystem::remove(tempBefore, ec);

        result.ok = ok;

        DebugLogFormat(
            L"[EPG-PORTAL][GEN] result ok=%d files=%zu out=%ls",
            result.ok ? 1 : 0,
            result.generatedFiles.size(),
            req.outputDir.wstring().c_str());

        return result;
    }
}