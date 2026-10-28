#include "EpgHtmlPortal_ProgramPageWriter.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <locale>
#include <vector>


#include "DebugManager.h"
#include "EpgHtmlPortal_Common.hpp"

namespace EpgHtmlPortal_ProgramPageWriter
{
    namespace
    {

        std::wstring EscapeAttr_(std::wstring_view s);

        std::wstring MakeServiceKeySafe_(
            int space,
            int networkId,
            int tsId,
            int serviceId);

        wchar_t ToHtmlTagLowerAscii_(wchar_t ch) noexcept
        {
            if (ch >= L'A' && ch <= L'Z') {
                return static_cast<wchar_t>(ch - L'A' + L'a');
            }

            return ch;
        }

        std::size_t WcsLenSafe_(const wchar_t* s) noexcept
        {
            return s == nullptr ? 0 : std::char_traits<wchar_t>::length(s);
        }

        std::wstring EscapeHtml_(std::wstring_view s)
        {
            std::wstring out;
            out.reserve(s.size() + 32);

            for (const wchar_t ch : s) {
                switch (ch) {
                case L'&':  out += L"&amp;";  break;
                case L'<':  out += L"&lt;";   break;
                case L'>':  out += L"&gt;";   break;
                case L'"':  out += L"&quot;"; break;
                default:    out.push_back(ch); break;
                }
            }

            return out;
        }

        int ParsePositiveIntAttr_(
            const std::wstring& tag,
            const wchar_t* name,
            int defaultValue) noexcept
        {
            if (name == nullptr || *name == L'\0') {
                return defaultValue;
            }

            const std::wstring needle = std::wstring(L" ") + name + L"=";
            const std::size_t p = tag.find(needle);
            if (p == std::wstring::npos) {
                return defaultValue;
            }

            std::size_t vpos = p + needle.size();
            if (vpos >= tag.size()) {
                return defaultValue;
            }

            wchar_t quote = 0;
            if (tag[vpos] == L'"' || tag[vpos] == L'\'') {
                quote = tag[vpos];
                ++vpos;
            }

            std::size_t vend = vpos;
            while (vend < tag.size()) {
                if (quote != 0) {
                    if (tag[vend] == quote) {
                        break;
                    }
                }
                else {
                    const wchar_t ch = tag[vend];
                    if (ch == L' ' || ch == L'\t' || ch == L'\r' ||
                        ch == L'\n' || ch == L'>') {
                        break;
                    }
                }
                ++vend;
            }

            if (vend <= vpos) {
                return defaultValue;
            }

            int value = 0;
            for (std::size_t i = vpos; i < vend; ++i) {
                const wchar_t ch = tag[i];
                if (ch < L'0' || ch > L'9') {
                    return defaultValue;
                }

                value = (value * 10) + static_cast<int>(ch - L'0');
                if (value > 1000000) {
                    return defaultValue;
                }
            }

            return value > 0 ? value : defaultValue;
        }

        std::wstring GetAttrValue_(
            const std::wstring& tag,
            const wchar_t* name)
        {
            if (name == nullptr || *name == L'\0') {
                return L"";
            }

            const std::wstring needle = std::wstring(L" ") + name + L"=";
            const std::size_t p = tag.find(needle);
            if (p == std::wstring::npos) {
                return L"";
            }

            std::size_t vpos = p + needle.size();
            if (vpos >= tag.size()) {
                return L"";
            }

            wchar_t quote = 0;
            if (tag[vpos] == L'"' || tag[vpos] == L'\'') {
                quote = tag[vpos];
                ++vpos;
            }

            std::size_t vend = vpos;
            while (vend < tag.size()) {
                if (quote != 0) {
                    if (tag[vend] == quote) {
                        break;
                    }
                }
                else {
                    const wchar_t ch = tag[vend];
                    if (ch == L' ' || ch == L'\t' || ch == L'\r' ||
                        ch == L'\n' || ch == L'>') {
                        break;
                    }
                }
                ++vend;
            }

            if (vend <= vpos || vend > tag.size()) {
                return L"";
            }

            return tag.substr(vpos, vend - vpos);
        }

        void EraseAttr_(
            std::wstring& tag,
            const wchar_t* name)
        {
            if (name == nullptr || *name == L'\0') {
                return;
            }

            const std::wstring needle = std::wstring(L" ") + name + L"=";

            for (;;) {
                const std::size_t p = tag.find(needle);
                if (p == std::wstring::npos) {
                    return;
                }

                std::size_t end = p + needle.size();
                if (end < tag.size() &&
                    (tag[end] == L'"' || tag[end] == L'\'')) {
                    const wchar_t quote = tag[end];
                    ++end;
                    while (end < tag.size() && tag[end] != quote) {
                        ++end;
                    }
                    if (end < tag.size()) {
                        ++end;
                    }
                }
                else {
                    while (end < tag.size()) {
                        const wchar_t ch = tag[end];
                        if (ch == L' ' || ch == L'\t' || ch == L'\r' ||
                            ch == L'\n' || ch == L'>') {
                            break;
                        }
                        ++end;
                    }
                }

                tag.erase(p, end - p);
            }
        }

        void SetAttr_(
            std::wstring& tag,
            const wchar_t* name,
            const std::wstring& value)
        {
            if (name == nullptr || *name == L'\0') {
                return;
            }

            EraseAttr_(tag, name);

            std::size_t insertPos = tag.rfind(L'>');
            if (insertPos == std::wstring::npos) {
                insertPos = tag.size();
            }

            tag.insert(
                insertPos,
                std::wstring(L" ") + name + L"=\"" + EscapeAttr_(value) + L"\"");
        }

        bool HasClass_(
            const std::wstring& tag,
            const wchar_t* className)
        {
            if (className == nullptr || *className == L'\0') {
                return false;
            }

            const std::wstring classes = GetAttrValue_(tag, L"class");
            if (classes.empty()) {
                return false;
            }

            std::size_t pos = 0;
            while (pos < classes.size()) {
                while (pos < classes.size() &&
                    (classes[pos] == L' ' || classes[pos] == L'\t' ||
                        classes[pos] == L'\r' || classes[pos] == L'\n')) {
                    ++pos;
                }

                const std::size_t start = pos;

                while (pos < classes.size() &&
                    classes[pos] != L' ' && classes[pos] != L'\t' &&
                    classes[pos] != L'\r' && classes[pos] != L'\n') {
                    ++pos;
                }

                if (pos > start &&
                    classes.compare(start, pos - start, className) == 0) {
                    return true;
                }
            }

            return false;
        }

        bool IsStartTag_(
            const std::wstring& tag,
            const wchar_t* name)
        {
            if (name == nullptr || *name == L'\0') {
                return false;
            }

            if (tag.size() < 3 || tag[0] != L'<') {
                return false;
            }

            std::size_t p = 1;
            while (p < tag.size() &&
                (tag[p] == L' ' || tag[p] == L'\t' ||
                    tag[p] == L'\r' || tag[p] == L'\n')) {
                ++p;
            }

            const std::size_t n = WcsLenSafe_(name);
            if (n == 0 || p + n >= tag.size()) {
                return false;
            }

            for (std::size_t i = 0; i < n; ++i) {
                const wchar_t a = ToHtmlTagLowerAscii_(tag[p + i]);
                const wchar_t b = ToHtmlTagLowerAscii_(name[i]);

                if (a != b) {
                    return false;
                }
            }

            const wchar_t next = tag[p + n];
            return next == L' ' || next == L'\t' ||
                next == L'\r' || next == L'\n' || next == L'>';
        }

        bool IsProgramCellTag_(const std::wstring& tag)
        {
            if (!IsStartTag_(tag, L"td")) {
                return false;
            }

            if (!HasClass_(tag, L"prog-cell")) {
                return false;
            }

            const std::wstring epgProgram =
                GetAttrValue_(tag, L"data-epg-program");

            return epgProgram.empty() || epgProgram == L"1";
        }

        bool IsServiceHeaderTag_(const std::wstring& tag)
        {
            if (!IsStartTag_(tag, L"th")) {
                return false;
            }

            if (HasClass_(tag, L"time-col")) {
                return false;
            }

            return HasClass_(tag, L"svc") || HasClass_(tag, L"service");
        }

        void ApplyServiceInfoToTag_(
            std::wstring& tag,
            const ChannelServiceInfo& service,
            bool isProgramCell,
            const wchar_t* identitySource)
        {
            const std::wstring key =
                service.serviceKey.empty()
                ? MakeServiceKeySafe_(
                    service.space,
                    service.networkId,
                    service.tsId,
                    service.serviceId)
                : service.serviceKey;

            SetAttr_(tag, L"data-space", std::to_wstring(service.space));
            SetAttr_(tag, L"data-service-key", key);
            SetAttr_(tag, L"data-network-id", std::to_wstring(service.networkId));
            SetAttr_(tag, L"data-ts-id", std::to_wstring(service.tsId));
            SetAttr_(tag, L"data-service-id", std::to_wstring(service.serviceId));

            if (!service.serviceName.empty()) {
                SetAttr_(tag, L"data-service-name", service.serviceName);
                SetAttr_(tag, L"data-service", service.serviceName);
            }

            if (service.physicalChannel >= 0) {
                SetAttr_(
                    tag,
                    L"data-physical-channel",
                    std::to_wstring(service.physicalChannel));
            }

            if (!service.channelName.empty()) {
                SetAttr_(tag, L"data-channel-name", service.channelName);
            }

            if (!service.remoteControlKeyId.empty()) {
                SetAttr_(
                    tag,
                    L"data-remote-control-key-id",
                    service.remoteControlKeyId);
            }

            const bool serviceValid =
                service.networkId > 0 &&
                service.tsId > 0 &&
                service.serviceId > 0;

            SetAttr_(
                tag,
                L"data-service-identity-valid",
                serviceValid ? L"1" : L"0");

            SetAttr_(
                tag,
                L"data-service-identity-source",
                identitySource != nullptr && identitySource[0] != L'\0'
                ? identitySource
                : (isProgramCell ? L"cpp-program-id" : L"cpp-header"));

            if (isProgramCell) {
                SetAttr_(tag, L"data-service-identity-lock", L"1");

                const int eventId =
                    ParsePositiveIntAttr_(tag, L"data-event-id", 0);
                const std::wstring title = GetAttrValue_(tag, L"data-title");

                const bool programValid =
                    serviceValid &&
                    eventId > 0 &&
                    !title.empty();

                SetAttr_(
                    tag,
                    L"data-program-identity-valid",
                    programValid ? L"1" : L"0");
            }
        }



        struct ProgramIdentityItem_
        {
            int eventId = 0;
            std::wstring titleKey;
            std::wstring timeKey;
            std::wstring dateKey;
            ChannelServiceInfo service;
        };

        void ReplaceAllInPlace_(
            std::wstring& s,
            std::wstring_view from,
            std::wstring_view to)
        {
            if (from.empty()) {
                return;
            }

            std::size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::wstring::npos) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
        }

        std::wstring DecodeSmallHtmlEntities_(std::wstring s)
        {
            ReplaceAllInPlace_(s, L"&amp;", L"&");
            ReplaceAllInPlace_(s, L"&lt;", L"<");
            ReplaceAllInPlace_(s, L"&gt;", L">");
            ReplaceAllInPlace_(s, L"&quot;", L"\"");
            ReplaceAllInPlace_(s, L"&#39;", L"'");
            ReplaceAllInPlace_(s, L"&#12316;", L"〜");
            ReplaceAllInPlace_(s, L"&amp;#12316;", L"〜");
            return s;
        }

        std::wstring NormalizeProgramTitleKey_(std::wstring s)
        {
            s = DecodeSmallHtmlEntities_(s);

            std::wstring out;
            out.reserve(s.size());

            for (const wchar_t ch : s) {
                if (ch == L' ' || ch == L'\t' ||
                    ch == L'\r' || ch == L'\n' ||
                    ch == L'　') {
                    continue;
                }

                out.push_back(ch);
            }

            return out;
        }

        std::wstring NormalizeProgramTimeKey_(std::wstring s)
        {
            s = DecodeSmallHtmlEntities_(s);

            ReplaceAllInPlace_(s, L"～", L"-");
            ReplaceAllInPlace_(s, L"〜", L"-");
            ReplaceAllInPlace_(s, L"\\u301c", L"-");
            ReplaceAllInPlace_(s, L"\\uff5e", L"-");

            std::wstring out;
            out.reserve(s.size());

            for (const wchar_t ch : s) {
                if (ch == L' ' || ch == L'\t' ||
                    ch == L'\r' || ch == L'\n' ||
                    ch == L'　') {
                    continue;
                }

                out.push_back(ch);
            }

            return out;
        }

        std::wstring NormalizeProgramDateKey_(std::wstring s)
        {
            s = DecodeSmallHtmlEntities_(s);

            std::wstring out;
            out.reserve(8);

            for (const wchar_t ch : s) {
                if (ch >= L'0' && ch <= L'9') {
                    out.push_back(ch);
                }
            }

            return out;
        }

        int ParseNonNegativeIntAttr_(
            const std::wstring& tag,
            const wchar_t* name,
            int defaultValue) noexcept
        {
            if (name == nullptr || *name == L'\0') {
                return defaultValue;
            }

            const std::wstring needle = std::wstring(L" ") + name + L"=";
            const std::size_t p = tag.find(needle);
            if (p == std::wstring::npos) {
                return defaultValue;
            }

            std::size_t vpos = p + needle.size();
            if (vpos >= tag.size()) {
                return defaultValue;
            }

            wchar_t quote = 0;
            if (tag[vpos] == L'"' || tag[vpos] == L'\'') {
                quote = tag[vpos];
                ++vpos;
            }

            std::size_t vend = vpos;
            while (vend < tag.size()) {
                if (quote != 0) {
                    if (tag[vend] == quote) {
                        break;
                    }
                }
                else {
                    const wchar_t ch = tag[vend];
                    if (ch == L' ' || ch == L'\t' || ch == L'\r' ||
                        ch == L'\n' || ch == L'>') {
                        break;
                    }
                }

                ++vend;
            }

            if (vend <= vpos) {
                return defaultValue;
            }

            int value = 0;
            for (std::size_t i = vpos; i < vend; ++i) {
                const wchar_t ch = tag[i];
                if (ch < L'0' || ch > L'9') {
                    return defaultValue;
                }

                value = (value * 10) + static_cast<int>(ch - L'0');
                if (value > 1000000) {
                    return defaultValue;
                }
            }

            return value;
        }

        std::wstring FindEnclosingTrStartTag_(
            const std::wstring& html,
            std::size_t pos)
        {
            if (pos == std::wstring::npos || pos >= html.size()) {
                return L"";
            }

            const std::size_t trStart = html.rfind(L"<tr", pos);
            if (trStart == std::wstring::npos) {
                return L"";
            }

            const std::size_t trEnd = html.find(L">", trStart);
            if (trEnd == std::wstring::npos || trEnd > pos) {
                return L"";
            }

            const std::size_t trClose = html.find(L"</tr>", trStart);
            if (trClose != std::wstring::npos && trClose < pos) {
                return L"";
            }

            return html.substr(trStart, trEnd - trStart + 1);
        }

        int GetPositiveAttrFromTagOrRow_(
            const std::wstring& tag,
            const std::wstring& rowTag,
            const wchar_t* name,
            int defaultValue) noexcept
        {
            int v = ParsePositiveIntAttr_(tag, name, 0);
            if (v > 0) {
                return v;
            }

            v = ParsePositiveIntAttr_(rowTag, name, 0);
            if (v > 0) {
                return v;
            }

            return defaultValue;
        }

        int GetNonNegativeAttrFromTagOrRow_(
            const std::wstring& tag,
            const std::wstring& rowTag,
            const wchar_t* name,
            int defaultValue) noexcept
        {
            const int v1 = ParseNonNegativeIntAttr_(tag, name, -1);
            if (v1 >= 0) {
                return v1;
            }

            const int v2 = ParseNonNegativeIntAttr_(rowTag, name, -1);
            if (v2 >= 0) {
                return v2;
            }

            return defaultValue;
        }

        std::wstring GetAttrFromTagOrRow_(
            const std::wstring& tag,
            const std::wstring& rowTag,
            const wchar_t* name)
        {
            std::wstring v = GetAttrValue_(tag, name);
            if (!v.empty()) {
                return v;
            }

            return GetAttrValue_(rowTag, name);
        }

        std::wstring ResolveIdentitySourceHtml_(
            const ProgramPageRequest& req)
        {
            if (!req.listHtml.empty()) {
                return req.listHtml;
            }

            if (req.viewKind == ProgramPageViewKind::Timetable &&
                !req.tableHtml.empty()) {
                return req.tableHtml;
            }

            return L"";
        }

        std::vector<ProgramIdentityItem_> BuildProgramIdentityListFromListHtml_(
            const std::wstring& listHtml,
            int fallbackSpace)
        {
            std::vector<ProgramIdentityItem_> out;

            if (listHtml.empty()) {
                return out;
            }

            std::size_t pos = 0;

            for (;;) {
                const std::size_t dataPos =
                    listHtml.find(L"data-epg-program=\"1\"", pos);

                if (dataPos == std::wstring::npos) {
                    break;
                }

                const std::size_t tagStart = listHtml.rfind(L"<", dataPos);
                const std::size_t tagEnd = listHtml.find(L">", dataPos);

                if (tagStart == std::wstring::npos ||
                    tagEnd == std::wstring::npos ||
                    tagStart > tagEnd) {
                    pos = dataPos + 1;
                    continue;
                }

                const std::wstring tag =
                    listHtml.substr(tagStart, tagEnd - tagStart + 1);

                const std::wstring rowTag =
                    FindEnclosingTrStartTag_(listHtml, tagStart);

                ProgramIdentityItem_ item{};
                item.eventId = ParsePositiveIntAttr_(tag, L"data-event-id", 0);

                item.service.space =
                    GetNonNegativeAttrFromTagOrRow_(
                        tag,
                        rowTag,
                        L"data-space",
                        fallbackSpace);

                item.service.networkId =
                    GetPositiveAttrFromTagOrRow_(
                        tag,
                        rowTag,
                        L"data-network-id",
                        0);

                item.service.tsId =
                    GetPositiveAttrFromTagOrRow_(
                        tag,
                        rowTag,
                        L"data-ts-id",
                        0);

                item.service.serviceId =
                    GetPositiveAttrFromTagOrRow_(
                        tag,
                        rowTag,
                        L"data-service-id",
                        0);

                item.service.serviceKey =
                    GetAttrFromTagOrRow_(tag, rowTag, L"data-service-key");

                item.service.serviceName =
                    GetAttrFromTagOrRow_(tag, rowTag, L"data-service-name");

                if (item.service.serviceName.empty()) {
                    item.service.serviceName =
                        GetAttrFromTagOrRow_(tag, rowTag, L"data-service");
                }

                item.titleKey =
                    NormalizeProgramTitleKey_(
                        GetAttrFromTagOrRow_(tag, rowTag, L"data-title"));

                item.timeKey =
                    NormalizeProgramTimeKey_(
                        GetAttrFromTagOrRow_(tag, rowTag, L"data-time"));

                item.dateKey =
                    NormalizeProgramDateKey_(
                        GetAttrFromTagOrRow_(tag, rowTag, L"data-start-date"));

                if (item.dateKey.empty()) {
                    item.dateKey =
                        NormalizeProgramDateKey_(
                            GetAttrFromTagOrRow_(tag, rowTag, L"data-date"));
                }

                if (item.eventId > 0 &&
                    item.service.networkId > 0 &&
                    item.service.tsId > 0 &&
                    item.service.serviceId > 0 &&
                    !item.titleKey.empty()) {
                    if (item.service.serviceKey.empty()) {
                        item.service.serviceKey =
                            MakeServiceKeySafe_(
                                item.service.space,
                                item.service.networkId,
                                item.service.tsId,
                                item.service.serviceId);
                    }

                    out.push_back(item);
                }

                pos = tagEnd + 1;
            }

            return out;
        }

        bool ProgramIdentityItemMatches_(
            const ProgramIdentityItem_& item,
            int eventId,
            const std::wstring& titleKey,
            const std::wstring& timeKey,
            const std::wstring& dateKey) noexcept
        {
            if (item.eventId != eventId) {
                return false;
            }

            if (item.titleKey != titleKey) {
                return false;
            }

            if (!timeKey.empty() &&
                !item.timeKey.empty() &&
                item.timeKey != timeKey) {
                return false;
            }

            if (!dateKey.empty() &&
                !item.dateKey.empty() &&
                item.dateKey != dateKey) {
                return false;
            }

            return true;
        }

        bool TryResolveProgramIdentityFromList_(
            const std::wstring& programTag,
            const std::vector<ProgramIdentityItem_>& list,
            ChannelServiceInfo& outService)
        {
            outService = {};

            if (list.empty()) {
                return false;
            }

            const int eventId =
                ParsePositiveIntAttr_(programTag, L"data-event-id", 0);

            const std::wstring titleKey =
                NormalizeProgramTitleKey_(
                    GetAttrValue_(programTag, L"data-title"));

            const std::wstring timeKey =
                NormalizeProgramTimeKey_(
                    GetAttrValue_(programTag, L"data-time"));

            std::wstring dateKey =
                NormalizeProgramDateKey_(
                    GetAttrValue_(programTag, L"data-start-date"));

            if (dateKey.empty()) {
                dateKey =
                    NormalizeProgramDateKey_(
                        GetAttrValue_(programTag, L"data-date"));
            }

            if (eventId <= 0 || titleKey.empty()) {
                return false;
            }

            const ProgramIdentityItem_* exact = nullptr;
            int exactCount = 0;

            for (const auto& item : list) {
                if (!ProgramIdentityItemMatches_(
                    item,
                    eventId,
                    titleKey,
                    timeKey,
                    dateKey)) {
                    continue;
                }

                exact = &item;
                ++exactCount;
            }

            if (exactCount == 1 && exact != nullptr) {
                outService = exact->service;
                return true;
            }

            const ProgramIdentityItem_* loose = nullptr;
            int looseCount = 0;

            for (const auto& item : list) {
                if (item.eventId == eventId &&
                    item.titleKey == titleKey) {
                    loose = &item;
                    ++looseCount;
                }
            }

            if (looseCount == 1 && loose != nullptr) {
                outService = loose->service;
                return true;
            }

            return false;
        }

        void MarkProgramIdentityUnresolved_(std::wstring& tag)
        {
            SetAttr_(tag, L"data-network-id", L"0");
            SetAttr_(tag, L"data-ts-id", L"0");
            SetAttr_(tag, L"data-service-id", L"0");
            SetAttr_(tag, L"data-service-identity-valid", L"0");
            SetAttr_(tag, L"data-service-identity-source", L"cpp-program-unresolved");
            SetAttr_(tag, L"data-service-identity-lock", L"1");
            SetAttr_(tag, L"data-program-identity-valid", L"0");
        }

        std::wstring CanonicalizeTimetableIdentityByServiceOrder_(
            const std::wstring& source,
            const ProgramPageRequest& req)
        {
            if (source.empty()) {
                return source;
            }

            const std::wstring identitySourceHtml =
                ResolveIdentitySourceHtml_(req);

            const std::vector<ProgramIdentityItem_> identityList =
                BuildProgramIdentityListFromListHtml_(
                    identitySourceHtml,
                    req.space);

            if (identityList.empty()) {
                DebugLogFormat(
                    L"[EPG-PAGE][IDENTITY][WARN] identity map is empty. "
                    L"timetable reservation identity will be locked when unresolved.");
            }

            std::wstring out;
            out.reserve(source.size() + identityList.size() * 64);

            std::size_t pos = 0;
            int resolvedCount = 0;
            int unresolvedCount = 0;
            int skippedCount = 0;

            for (;;) {
                const std::size_t dataPos =
                    source.find(L"data-epg-program=\"1\"", pos);

                if (dataPos == std::wstring::npos) {
                    out.append(source, pos, std::wstring::npos);
                    break;
                }

                const std::size_t tagStart = source.rfind(L"<", dataPos);
                const std::size_t tagEnd = source.find(L">", dataPos);

                if (tagStart == std::wstring::npos ||
                    tagEnd == std::wstring::npos ||
                    tagStart > tagEnd) {
                    out.append(source, pos, dataPos + 1 - pos);
                    pos = dataPos + 1;
                    ++skippedCount;
                    continue;
                }

                out.append(source, pos, tagStart - pos);

                std::wstring tag =
                    source.substr(tagStart, tagEnd - tagStart + 1);

                if (IsProgramCellTag_(tag)) {
                    ChannelServiceInfo service{};
                    if (TryResolveProgramIdentityFromList_(
                        tag,
                        identityList,
                        service)) {
                        ApplyServiceInfoToTag_(
                            tag,
                            service,
                            true,
                            L"cpp-program-id");
                        ++resolvedCount;
                    }
                    else {
                        MarkProgramIdentityUnresolved_(tag);
                        ++unresolvedCount;
                    }
                }
                else {
                    ++skippedCount;
                }

                out += tag;
                pos = tagEnd + 1;
            }

            DebugLogFormat(
                L"[EPG-PAGE][IDENTITY] timetable identity map=%zu resolved=%d unresolved=%d skipped=%d",
                identityList.size(),
                resolvedCount,
                unresolvedCount,
                skippedCount);

            return out;
        }

        std::wstring EscapeAttr_(std::wstring_view s)
        {
            std::wstring out = EscapeHtml_(s);

            for (auto& ch : out) {
                if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
                    ch = L' ';
                }
            }

            return out;
        }

        std::wstring ToJsString_(std::wstring_view s)
        {
            return EpgHtmlPortal_Common::ToJsStringLiteral(std::wstring(s));
        }

        std::wstring MakeServiceKeySafe_(
            int space,
            int networkId,
            int tsId,
            int serviceId)
        {
            std::wstring key;
            key.reserve(64);

            key += L"s";
            key += std::to_wstring(space);
            key += L"_n";
            key += std::to_wstring(networkId);
            key += L"_t";
            key += std::to_wstring(tsId);
            key += L"_sid";
            key += std::to_wstring(serviceId);

            return key;
        }

        std::wstring ResolveServiceKey_(const ProgramCellMeta& meta)
        {
            if (!meta.serviceKey.empty()) {
                return meta.serviceKey;
            }

            return MakeServiceKeySafe_(
                meta.space,
                meta.networkId,
                meta.tsId,
                meta.serviceId);
        }

        std::wstring PageKindToText_(ProgramPageKind kind)
        {
            return kind == ProgramPageKind::Before ? L"before" : L"now";
        }

        std::wstring ViewKindToText_(ProgramPageViewKind viewKind)
        {
            switch (viewKind) {
            case ProgramPageViewKind::Timetable:
                return L"timetable";
            case ProgramPageViewKind::List:
                return L"list";
            case ProgramPageViewKind::Legacy:
            default:
                return L"legacy";
            }
        }

        std::wstring ViewKindToBodyClass_(ProgramPageViewKind viewKind)
        {
            switch (viewKind) {
            case ProgramPageViewKind::Timetable:
                return L"epg-page-timetable";
            case ProgramPageViewKind::List:
                return L"epg-page-list";
            case ProgramPageViewKind::Legacy:
            default:
                return L"epg-page-legacy";
            }
        }

        bool IsPastProgramPage_(
            const ProgramPageRequest& req) noexcept
        {
            return req.kind == ProgramPageKind::Before ||
                !req.archiveDateKey.empty();
        }

        bool ShouldEnableReservationSwitch_(
            const ProgramPageRequest& req) noexcept
        {
            if (IsPastProgramPage_(req)) {
                return false;
            }

            return req.enableReservationSwitch;
        }

        bool ShouldEnableTimeShiftSwitch_(
            const ProgramPageRequest& req) noexcept
        {
            if (req.enableTimeShiftSwitch) {
                return true;
            }

            return IsPastProgramPage_(req);
        }

        bool ShouldEnableAutoReserveAssist_(
            const ProgramPageRequest& req)
        {
            if (IsPastProgramPage_(req)) {
                return false;
            }

            return req.enableAutoReserveAssist &&
                !EpgHtmlPortal_Common::TrimW(req.autoReserveEntryUrl).empty();
        }

        std::wstring ViewKindToNote_(ProgramPageViewKind viewKind)
        {
            switch (viewKind) {
            case ProgramPageViewKind::Timetable:
                return L"時間割表示です。番組セルの予約スイッチから視聴予約 / 録画予約を登録できます。";
            case ProgramPageViewKind::List:
                return L"表一覧表示です。番組行の予約スイッチから視聴予約 / 録画予約を登録できます。";
            case ProgramPageViewKind::Legacy:
            default:
                return L"番組セルに data-epg-program 属性が入っている場合、視聴予約 / 録画予約スイッチを自動で差し込みます。";
            }
        }

        std::wstring ResolvePageTitle_(const ProgramPageRequest& req)
        {
            if (!req.htmlTitle.empty()) {
                return req.htmlTitle;
            }

            std::wstring base =
                req.spaceLabel.empty()
                ? (L"Space " + std::to_wstring(req.space))
                : req.spaceLabel;

            if (req.kind == ProgramPageKind::Before) {
                base += L" 過去";
            }

            switch (req.viewKind) {
            case ProgramPageViewKind::Timetable:
                base += L" 時間割";
                break;
            case ProgramPageViewKind::List:
                base += L" 表一覧";
                break;
            case ProgramPageViewKind::Legacy:
            default:
                base += L" 番組表";
                break;
            }

            return base;
        }

        std::wstring ResolveBodyHtml_(const ProgramPageRequest& req)
        {
            if (req.viewKind == ProgramPageViewKind::Timetable &&
                !req.timetableHtml.empty()) {
                return req.timetableHtml;
            }

            if (req.viewKind == ProgramPageViewKind::List &&
                !req.listHtml.empty()) {
                return req.listHtml;
            }

            return req.tableHtml;
        }

        std::filesystem::path FindConfigIniPath_()
        {
            try {
                wchar_t modulePath[MAX_PATH]{};
                const DWORD n = ::GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
                if (n > 0 && n < MAX_PATH) {
                    const std::filesystem::path exeDir =
                        std::filesystem::path(modulePath).parent_path();
                    const std::filesystem::path candidate = exeDir / L"config.ini";
                    if (std::filesystem::exists(candidate)) {
                        return candidate;
                    }
                }
            }
            catch (...) {
            }

            try {
                const std::filesystem::path candidate =
                    std::filesystem::current_path() / L"config.ini";
                if (std::filesystem::exists(candidate)) {
                    return candidate;
                }
            }
            catch (...) {
            }

            return {};
        }

        std::wstring ReadIniValueLike_(
            const std::filesystem::path& iniPath,
            std::wstring_view sectionName,
            std::wstring_view keyName)
        {
            if (iniPath.empty()) {
                return {};
            }

            wchar_t buf[512]{};
            ::GetPrivateProfileStringW(
                std::wstring(sectionName).c_str(),
                std::wstring(keyName).c_str(),
                L"",
                buf,
                static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])),
                iniPath.c_str());

            return std::wstring(buf);
        }

        bool ReadIniBoolLike_(
            const std::filesystem::path& iniPath,
            std::wstring_view sectionName,
            std::wstring_view keyName,
            bool defaultValue)
        {
            const std::wstring raw = ReadIniValueLike_(iniPath, sectionName, keyName);
            if (raw.empty()) {
                return defaultValue;
            }

            std::wstring v = EpgHtmlPortal_Common::ToLowerAsciiW(
                EpgHtmlPortal_Common::TrimW(raw));

            if (v == L"1" || v == L"true" || v == L"yes" || v == L"on") {
                return true;
            }

            if (v == L"0" || v == L"false" || v == L"no" || v == L"off") {
                return false;
            }

            return defaultValue;
        }

        int ReadIniIntLike_(
            const std::filesystem::path& iniPath,
            std::wstring_view sectionName,
            std::wstring_view keyName,
            int defaultValue)
        {
            const std::wstring raw = ReadIniValueLike_(iniPath, sectionName, keyName);
            if (raw.empty()) {
                return defaultValue;
            }

            try {
                return std::stoi(raw);
            }
            catch (...) {
                return defaultValue;
            }
        }

        int ResolveReservationPreferredTuner_()
        {
            const std::filesystem::path iniPath = FindConfigIniPath_();
            if (iniPath.empty()) {
                return -1;
            }

            return (std::clamp)(
                ReadIniIntLike_(iniPath, L"Tuners", L"ActiveIndex", 0),
                0,
                64);
        }

        std::wstring ResolveReservationApiBase_()
        {
            const std::filesystem::path iniPath = FindConfigIniPath_();
            if (iniPath.empty()) {
                return {};
            }

            const bool enableServer =
                ReadIniBoolLike_(iniPath, L"Network", L"EnableServer", false);
            const bool autoStartServer =
                ReadIniBoolLike_(iniPath, L"Network", L"AutoStartServer", false);

            if (!enableServer || !autoStartServer) {
                return {};
            }

            std::wstring bindAddress =
                ReadIniValueLike_(iniPath, L"Network", L"BindAddress");
            const bool allowRemote =
                ReadIniBoolLike_(iniPath, L"Network", L"AllowRemoteAccess", false);
            const int httpPort =
                (std::clamp)(
                    ReadIniIntLike_(iniPath, L"Network", L"HttpPort", 8080),
                    1,
                    65535);

            if (bindAddress.empty()) {
                bindAddress = allowRemote ? L"0.0.0.0" : L"127.0.0.1";
            }

            bindAddress = EpgHtmlPortal_Common::TrimW(std::move(bindAddress));

            if (bindAddress == L"0.0.0.0" ||
                bindAddress == L"::" ||
                bindAddress == L"[::]") {
                bindAddress = L"127.0.0.1";
            }

            std::wstring base = L"http://";
            base += bindAddress;
            base += L":";
            base += std::to_wstring(httpPort);
            base += L"/api/reservation";
            return base;
        }

        void AppendPageStyle_(std::wstring& html)
        {
            html += LR"HTML(<style>
:root{
  color-scheme:light;
  --bg:#e9eef5;
  --panel:#f7f9fc;
  --panel2:#e5ebf3;
  --line:#bcc8d6;
  --line-strong:#93a5bb;
  --text:#16202b;
  --sub:#536274;
  --accent:#2e68cc;
  --accent2:#bf4f68;
  --ok:#2f7d59;
  --warn:#a06a18;
  --table-head:#dde6f1;
  --table-time:#edf2f8;
  --table-cell:#f8fbff;
  --table-empty:#eef3f8;
  --table-alt:#f1f5fa;
  --table-sep:#c7d2df;
  --reserve-watch-bg:#eaf2ff;
  --reserve-watch-bar:#7da6ea;
  --reserve-record-bg:#fbeef2;
  --reserve-record-bar:#d38aa0;
}
*{box-sizing:border-box;}
html,body{
  margin:0;
  padding:0;
  background:var(--bg);
  color:var(--text);
  font-family:"Yu Gothic UI","Meiryo UI","Segoe UI",sans-serif;
}
body.epg-program-page{min-height:100vh;}
.page-shell{
  width:min(1800px,calc(100% - 24px));
  margin:12px auto;
}
.page-header{
  margin-bottom:12px;
  padding:14px 16px;
  border:1px solid var(--line);
  background:linear-gradient(180deg,#ffffff,#f6f9fc);
  border-radius:14px;
  box-shadow:0 2px 10px rgba(20,35,60,.05);
}
.page-title{
  margin:0 0 6px 0;
  font-size:24px;
  font-weight:700;
  color:var(--text);
}
.page-meta{
  margin:0;
  color:var(--sub);
  font-size:13px;
  line-height:1.7;
}
.page-main{
  border:1px solid var(--line);
  background:var(--panel);
  border-radius:14px;
  padding:12px;
  overflow:auto;
  box-shadow:0 2px 10px rgba(20,35,60,.05);
}
.program-table-wrap{min-height:320px;}
.program-table-wrap table{width:100%;}
.program-table-note{
  margin-top:8px;
  color:var(--sub);
  font-size:12px;
}
</style>
)HTML";
        }

        void AppendReservationSwitchStyle_(std::wstring& html)
        {
            html += LR"HTML(<style>
.epg-inline-switch-host{
  display:flex;
  gap:6px;
  flex-wrap:wrap;
  margin-top:6px;
}
.epg-inline-switch-btn{
  appearance:none;
  border:1px solid var(--line-strong);
  background:#ffffff;
  color:var(--text);
  border-radius:8px;
  padding:4px 8px;
  font:inherit;
  font-size:12px;
  cursor:pointer;
}
.epg-inline-switch-btn:hover{background:#f3f7fd;}
.epg-inline-switch-btn.watch.active{
  background:#dfeafb;
  border-color:#7c9fdb;
  color:#1d4f9f;
}
.epg-inline-switch-btn.record.active{
  background:#f7e3ea;
  border-color:#c98aa0;
  color:#962b4a;
}
[data-epg-program="1"].epg-inline-watch-mark{
  background:linear-gradient(90deg,var(--reserve-watch-bg) 0%,var(--table-cell) 22%) !important;
  box-shadow:inset 3px 0 0 0 var(--reserve-watch-bar),inset 0 0 0 1px rgba(125,166,234,.20);
}
[data-epg-program="1"].epg-inline-record-mark{
  background:linear-gradient(90deg,var(--reserve-record-bg) 0%,var(--table-cell) 22%) !important;
  box-shadow:inset 3px 0 0 0 var(--reserve-record-bar),inset 0 0 0 1px rgba(211,138,160,.22);
}
</style>
)HTML";
        }

        void AppendTimeShiftSwitchStyle_(std::wstring& html)
        {
            html += L"<style>\n";
            html += L".epg-timeshift-switch-host{\n";
            html += L"  display:flex;\n";
            html += L"  gap:6px;\n";
            html += L"  flex-wrap:wrap;\n";
            html += L"  margin-top:6px;\n";
            html += L"}\n";
            html += L".epg-timeshift-btn{\n";
            html += L"  appearance:none;\n";
            html += L"  border:1px solid var(--line-strong);\n";
            html += L"  background:#f4f4f4;\n";
            html += L"  color:#687386;\n";
            html += L"  border-radius:8px;\n";
            html += L"  padding:4px 8px;\n";
            html += L"  font:inherit;\n";
            html += L"  font-size:12px;\n";
            html += L"  cursor:not-allowed;\n";
            html += L"}\n";
            html += L".epg-timeshift-note{\n";
            html += L"  color:#687386;\n";
            html += L"  font-size:11px;\n";
            html += L"}\n";
            html += L"</style>\n";
        }

        void AppendAutoReserveAssistStyle_(std::wstring& html)
        {
            html += L"<style>\n";
            html += L".epg-auto-reserve-assist-host{\n";
            html += L"  display:flex;\n";
            html += L"  align-items:center;\n";
            html += L"  flex-wrap:wrap;\n";
            html += L"  gap:5px;\n";
            html += L"  margin-top:5px;\n";
            html += L"}\n";
            html += L".epg-auto-reserve-assist-link{\n";
            html += L"  display:inline-block;\n";
            html += L"  border:1px solid #7590b5;\n";
            html += L"  border-radius:7px;\n";
            html += L"  background:#eef4fd;\n";
            html += L"  color:#24528f;\n";
            html += L"  padding:3px 7px;\n";
            html += L"  font-size:11px;\n";
            html += L"  font-weight:600;\n";
            html += L"  line-height:1.35;\n";
            html += L"  text-decoration:none;\n";
            html += L"  white-space:normal;\n";
            html += L"}\n";
            html += L".epg-auto-reserve-assist-link:hover{\n";
            html += L"  background:#dfeafb;\n";
            html += L"  border-color:#527caf;\n";
            html += L"  color:#173f75;\n";
            html += L"}\n";
            html += L".epg-auto-reserve-assist-link:focus-visible{\n";
            html += L"  outline:2px solid #2e68cc;\n";
            html += L"  outline-offset:2px;\n";
            html += L"}\n";
            html += L"</style>\n";
        }

        void AppendControlBarStyle_(std::wstring& html)
        {
            html += LR"HTML(<style>
.top-links{
  margin:6px 0 10px 0;
  font-size:12px;
}
.top-links a{
  color:var(--accent);
  text-decoration:none;
}
.top-links a:hover{text-decoration:underline;}
.view-bar,.layout-bar,.day-bar,.genre-bar{
  display:flex;
  gap:6px;
  align-items:center;
  flex-wrap:wrap;
  margin:6px 0 10px 0;
}
.view-bar button,.layout-bar button,.day-bar button,.genre-bar button{
  padding:4px 10px;
  border:1px solid var(--line-strong);
  border-radius:6px;
  background:#ffffff;
  color:var(--text);
  cursor:pointer;
  font-size:12px;
}
.view-bar button.active{background:#e8f1ff;border-color:#78a3ef;}
.layout-bar button.active{background:#fff4d9;border-color:#d9a73f;}
.day-bar button.active{background:#e8f8ea;border-color:#71b87a;}
.genre-bar button.active{background:#fdebf3;border-color:#d889a4;}
.view-bar .note,.layout-bar .note,.day-bar .note,.genre-bar .note{
  color:var(--sub);
  font-size:11px;
  margin-left:6px;
}
.table-wrap,.epg-wrap{
  /* 番組表専用の縦スクロールを1本だけ持たせる。 */
  overflow-y:auto;
  overflow-x:auto;
  scrollbar-gutter:stable;
  overscroll-behavior:contain;
  border:1px solid var(--line);
  background:#fff;
  max-height:720px;
  max-width:calc(100vw - 24px);
  border-radius:10px;
}
body[data-view="main"] .col-sub{display:none;}
body[data-view="sub"] .col-main{display:none;}
body[data-layout="grid"] .genre-view{display:none;}
body[data-layout="genre"] .grid-view{display:none;}
.genre-dim{opacity:.28;}
.day-dim{opacity:.28;}
.epg-day-anchor-highlight{
  outline:2px solid rgba(46,104,204,.45);
  outline-offset:-2px;
}
.epg-local-notice,
.epg-timetable-local-notice,
.epg-list-local-notice{
  margin:6px 0 10px 0;
  padding:6px 10px;
  border:1px solid var(--line);
  border-radius:8px;
  background:#fff8df;
  color:#6f4d00;
  font-size:12px;
}
</style>
)HTML";
        }

        void AppendListTableStyle_(std::wstring& html)
        {
            html += LR"HTML(<style>
table.prog,table.glist{
  border-collapse:collapse;
  width:max-content;
  min-width:100%;
  margin:0;
  table-layout:fixed;
  background:#fff;
}
table.prog th,table.prog td,
table.glist th,table.glist td{
  border:1px solid var(--table-sep);
  padding:4px 6px;
  vertical-align:top;
  color:var(--text);
  background:var(--table-cell);
}
table.prog th,table.glist th{background:var(--table-head);}
table.prog th.time,table.prog td.time{white-space:nowrap;width:6em;}
table.prog th.dur,table.prog td.dur{width:3em;text-align:right;}
table.prog th.eid,table.prog td.eid{width:4em;text-align:right;}
table.prog th.title{width:18em;}
table.prog td.title{font-weight:700;}
table.prog td.summary{font-size:90%;color:#425062;}
table.prog tr.date-sep th{
  background:#dde7f3;
  color:var(--text);
  text-align:left;
  font-weight:700;
}
table.glist th.time,table.glist td.time{white-space:nowrap;width:8em;}
table.glist th.svc,table.glist td.svc{white-space:nowrap;width:12em;}
table.glist th.genre,table.glist td.genre{white-space:nowrap;width:12em;}
table.glist td.title{font-weight:700;}
table.glist tr.genre-sep th{
  background:#f4ead4;
  color:var(--text);
  text-align:left;
  font-weight:700;
}
body.epg-page-list .page-shell{
  width:min(1600px,calc(100% - 24px));
}
.epg-list-template-store{
  display:none!important;
}
.epg-list-mount{
  min-height:320px;
}
body.epg-page-list .day-btn[data-day="all"]{
  display:none;
}
</style>
)HTML";
        }

        void AppendTimetableStyle_(std::wstring& html)
        {
            html += LR"HTML(<style>
table.epg{
  border-collapse:collapse;
  table-layout:fixed;
  width:max-content;
  min-width:100%;
  background:#fff;
}
table.epg th,table.epg td{
  border:1px solid var(--table-sep);
  padding:3px 5px;
  vertical-align:top;
  color:var(--text);
}
table.epg thead th{
  position:sticky;
  top:0;
  background:var(--table-head);
  z-index:3;
}
table.epg th.time-col{
  position:sticky;
  left:0;
  background:var(--table-time);
  z-index:4;
  white-space:nowrap;
  width:6.5em;
  color:#334155;
}
table.epg th.svc{
  width:240px;
  max-width:240px;
}
table.epg td.empty{background:var(--table-empty);}
table.epg td.prog-cell{
  background:var(--table-cell);
  white-space:normal;
  overflow-wrap:anywhere;
  word-break:break-word;
  color:var(--text);
}
table.epg td.prog-cell:nth-child(even){background:var(--table-alt);}
table.epg td.prog-cell .t{
  font-weight:700;
  display:-webkit-box;
  -webkit-box-orient:vertical;
  -webkit-line-clamp:2;
  overflow:hidden;
  color:#101923;
}
table.epg td.prog-cell .tm{
  font-size:11px;
  color:#47586c;
  margin-top:2px;
}
table.epg td.prog-cell .s{
  font-size:11px;
  color:#334355;
  line-height:1.35;
  display:-webkit-box;
  -webkit-box-orient:vertical;
  -webkit-line-clamp:3;
  overflow:hidden;
  margin-top:2px;
}
table.epg td.prog-cell .eid{
  float:none;
  display:block;
  text-align:right;
  font-size:10px;
  color:#627285;
}
table.epg tr.tick th,table.epg tr.tick td{
  border-top:1px solid var(--line-strong);
}
table.epg tr.hour th,table.epg tr.hour td{
  border-top:2px solid #7c8ea6;
}
body.epg-page-timetable .page-shell{
  width:min(1900px,calc(100% - 16px));
}
.epg-timetable-template-store{
  display:none!important;
}
.epg-timetable-mount{
  min-height:320px;
}
body.epg-page-timetable .day-btn[data-day="all"]{
  display:none;
}
</style>
)HTML";
        }

        void AppendStyles_(std::wstring& html, ProgramPageViewKind viewKind)
        {
            AppendPageStyle_(html);
            AppendReservationSwitchStyle_(html);
            AppendControlBarStyle_(html);

            if (viewKind == ProgramPageViewKind::Timetable) {
                AppendTimetableStyle_(html);
            }
            else if (viewKind == ProgramPageViewKind::List) {
                AppendListTableStyle_(html);
            }
            else {
                AppendListTableStyle_(html);
                AppendTimetableStyle_(html);
            }

            AppendTimeShiftSwitchStyle_(html);
            AppendAutoReserveAssistStyle_(html);
        }

        void AppendRuntimeMetaScript_(
            std::wstring& html,
            const ProgramPageRequest& req,
            const std::wstring& reservationApiBase,
            int reservationPreferredTuner)
        {
            const std::wstring kindText = PageKindToText_(req.kind);
            const std::wstring viewText = ViewKindToText_(req.viewKind);

            html += L"<script>\n";
            html += L"window.EPG_PROGRAM_PAGE_META = {\n";
            html += L"  space: " + std::to_wstring(req.space) + L",\n";
            html += L"  kind: " + ToJsString_(kindText) + L",\n";
            html += L"  viewKind: " + ToJsString_(viewText) + L",\n";
            html += L"  label: " + ToJsString_(req.spaceLabel) + L",\n";
            html += L"  reservationEnabled: ";
            html += ShouldEnableReservationSwitch_(req) ? L"true,\n" : L"false,\n";
            html += L"  timeShiftEnabled: ";
            html += ShouldEnableTimeShiftSwitch_(req) ? L"true,\n" : L"false,\n";
            html += L"  autoReserveAssistEnabled: ";
            html += ShouldEnableAutoReserveAssist_(req) ? L"true,\n" : L"false,\n";
            html += L"  autoReserveEntryUrl: ";
            html += ToJsString_(req.autoReserveEntryUrl);
            html += L"\n";
            html += L"};\n\n";

            html += L"window.EPG_RUNTIME = Object.assign({}, window.EPG_RUNTIME || {}, {\n";
            html += L"  reservationApiBase: " + ToJsString_(reservationApiBase) + L",\n";
            html += L"  reservationPreferredTuner: " + std::to_wstring(reservationPreferredTuner) + L",\n";
            html += L"  reservationViewCacheUrl: (\n";
            html += L"    location.protocol === 'file:'\n";
            html += L"      ? '../cache/reservation/view/tasks_view_index.tsv'\n";
            html += L"      : '/cache/reservation/view/tasks_view_index.tsv'\n";
            html += L"  )\n";
            html += L"});\n";
            html += L"</script>\n";
        }

        bool ShouldShowSubChannelSwitch_(
            ProgramPageViewKind viewKind) noexcept
        {
            return viewKind == ProgramPageViewKind::Timetable ||
                viewKind == ProgramPageViewKind::List;
        }

        void AppendSubChannelViewBarHtml_(
            std::wstring& html,
            ProgramPageViewKind viewKind)
        {
            if (!ShouldShowSubChannelSwitch_(viewKind)) {
                return;
            }

            html += L"<div class=\"view-bar\" id=\"subChannelViewBar\">\n";

            html += L"<button type=\"button\" class=\"view-btn active\" ";
            html += L"data-view-mode=\"main\" id=\"btn-view-main\">";
            html += L"メインのみ";
            html += L"</button>\n";

            html += L"<button type=\"button\" class=\"view-btn\" ";
            html += L"data-view-mode=\"all\" id=\"btn-view-all\">";
            html += L"サブ含む";
            html += L"</button>\n";

            html += L"<button type=\"button\" class=\"view-btn\" ";
            html += L"data-view-mode=\"sub\" id=\"btn-view-sub\">";
            html += L"サブのみ";
            html += L"</button>\n";

            html += L"<span class=\"note\">";
            html += L"※サブチャンネル列の表示/非表示を切替";
            html += L"</span>\n";

            html += L"</div>\n";
        }

        void AppendSubChannelViewSwitchScript_(
            std::wstring& html)
        {
            html += L"<script>\n";
            html += L"(function(){\n";
            html += L"'use strict';\n";

            html += L"function qsa_(s,root){return Array.prototype.slice.call((root||document).querySelectorAll(s));}\n";
            html += L"function qs_(s,root){return (root||document).querySelector(s);}\n";

            html += L"const STORAGE_KEY='epg.portal.programPage.subChannelView';\n";

            html += L"function normalizeMode_(mode){\n";
            html += L"  mode=String(mode||'').trim().toLowerCase();\n";
            html += L"  if(mode==='main'||mode==='sub'||mode==='all')return mode;\n";
            html += L"  return 'main';\n";
            html += L"}\n";

            html += L"function loadMode_(){\n";
            html += L"  try{return normalizeMode_(localStorage.getItem(STORAGE_KEY)||'main');}\n";
            html += L"  catch(_){return 'main';}\n";
            html += L"}\n";

            html += L"function saveMode_(mode){\n";
            html += L"  try{localStorage.setItem(STORAGE_KEY,normalizeMode_(mode));}catch(_){ }\n";
            html += L"}\n";

            html += L"function applyMode_(mode){\n";
            html += L"  mode=normalizeMode_(mode);\n";
            html += L"  if(mode==='all'){\n";
            html += L"    document.body.removeAttribute('data-view');\n";
            html += L"  }else{\n";
            html += L"    document.body.setAttribute('data-view',mode);\n";
            html += L"  }\n";
            html += L"  qsa_('#subChannelViewBar .view-btn').forEach(function(btn){\n";
            html += L"    const m=normalizeMode_(btn.getAttribute('data-view-mode')||'main');\n";
            html += L"    const active=(m===mode);\n";
            html += L"    btn.classList.toggle('active',active);\n";
            html += L"    btn.setAttribute('aria-pressed',active?'true':'false');\n";
            html += L"  });\n";
            html += L"  saveMode_(mode);\n";
            html += L"}\n";

            html += L"function init_(){\n";
            html += L"  const bar=qs_('#subChannelViewBar');\n";
            html += L"  if(!bar)return;\n";
            html += L"  qsa_('.view-btn',bar).forEach(function(btn){\n";
            html += L"    btn.addEventListener('click',function(){\n";
            html += L"      applyMode_(btn.getAttribute('data-view-mode')||'main');\n";
            html += L"    });\n";
            html += L"  });\n";
            html += L"  applyMode_(loadMode_());\n";
            html += L"}\n";

            html += L"window.__epgProgramPageSetSubChannelView=applyMode_;\n";
            html += L"window.__epgProgramPageGetSubChannelView=loadMode_;\n";

            html += L"if(document.readyState==='loading'){\n";
            html += L"  document.addEventListener('DOMContentLoaded',init_,{once:true});\n";
            html += L"}else{\n";
            html += L"  init_();\n";
            html += L"}\n";

            html += L"})();\n";
            html += L"</script>\n";
        }

        void AppendExternalScripts_(std::wstring& html, const ProgramPageRequest& req)
        {
            if (!ShouldEnableReservationSwitch_(req)) {
                return;
            }

            const std::wstring serviceMapFile =
                req.serviceMapFileName.empty()
                ? L"epg_service_map.js"
                : req.serviceMapFileName;

            html += L"<script defer src=\"";
            html += EscapeAttr_(serviceMapFile);
            html += L"\"></script>\n";

            html += L"<script defer src=\"reservation_data.js\"></script>\n";

            if (req.viewKind == ProgramPageViewKind::Timetable ||
                req.viewKind == ProgramPageViewKind::List) {
                html += L"<script defer src=\"portal_program_table_switch.js\"></script>\n";
                return;
            }

            html += L"<script defer src=\"portal_runtime_switch.js\"></script>\n";
            html += L"<script defer src=\"portal_program_table_switch.js\"></script>\n";
        }

        void AppendProgramPageReservationSyncScript_Open_(std::wstring& html)
        {
            html += LR"HTML(<script>
(function(){
'use strict';

function epgProgramPageCallSafe_(fn){
  try {
    if (typeof fn === 'function') {
      return fn();
    }
  } catch (e) {
    try {
      console.warn('[program-page-reservation-sync] call failed', e);
    } catch (_) {
    }
  }
  return null;
}

function epgProgramPageGetTop_(){
  try {
    if (window.top && window.top !== window) {
      return window.top;
    }
  } catch (_) {
  }
  return null;
}
)HTML";
        }

        void AppendProgramPageReservationSyncScript_Data_(std::wstring& html)
        {
            html += LR"HTML(
function epgProgramPageSeedLocalReservationData_(){
  try {
    if (typeof window.__epgPortalSetReservationList !== 'function') {
      return false;
    }

    let list = null;

    if (Array.isArray(window.EPG_RESERVATION_DATA)) {
      list = window.EPG_RESERVATION_DATA;
    } else if (window.EPG_RESERVATION_DATA &&
               Array.isArray(window.EPG_RESERVATION_DATA.items)) {
      list = window.EPG_RESERVATION_DATA.items;
    } else if (Array.isArray(window.gReservationList)) {
      list = window.gReservationList;
    }

    if (!Array.isArray(list)) {
      return false;
    }

    window.__epgPortalSetReservationList(list, Date.now());
    return true;
  } catch (e) {
    try {
      console.warn('[program-page-reservation-sync] seed local failed', e);
    } catch (_) {
    }
    return false;
  }
}
)HTML";
        }

        void AppendProgramPageReservationSyncScript_Sync_(std::wstring& html)
        {
            html += LR"HTML(
function epgProgramPageSyncReservationCache_(){
  const topWindow = epgProgramPageGetTop_();

  if (topWindow &&
      typeof topWindow.__epgPortalReloadReservationViewCache === 'function') {
    epgProgramPageCallSafe_(function(){
      topWindow.__epgPortalReloadReservationViewCache();
    });

    epgProgramPageCallSafe_(function(){
      if (typeof topWindow.__epgPortalQueueReservationUiSync === 'function') {
        topWindow.__epgPortalQueueReservationUiSync(
          'program-page-initial-cache-sync',
          true
        );
      }
    });

    epgProgramPageCallSafe_(function(){
      if (typeof topWindow.__epgPortalFlushVisibleProgramTableFramePendingRenders === 'function') {
        topWindow.__epgPortalFlushVisibleProgramTableFramePendingRenders(
          'program-page-initial-cache-sync',
          true
        );
      }
    });

    return true;
  }

  let cacheOk = false;

  if (typeof window.__epgPortalReloadReservationViewCache === 'function') {
    const r = epgProgramPageCallSafe_(function(){
      return window.__epgPortalReloadReservationViewCache();
    });

    cacheOk = !!r;
  }

  if (!cacheOk) {
    epgProgramPageSeedLocalReservationData_();
  }

  epgProgramPageCallSafe_(function(){
    if (typeof window.__epgPortalSyncReservationListFromViewCache === 'function') {
      window.__epgPortalSyncReservationListFromViewCache();
    }
  });

  return true;
}
)HTML";
        }

        void AppendProgramPageReservationSyncScript_Render_(std::wstring& html)
        {
            html += LR"HTML(
function epgProgramPageRenderReservationSwitches_(){
  if (typeof window.__epgProgramPageRequestReservationRender === 'function') {
    epgProgramPageCallSafe_(function(){
      window.__epgProgramPageRequestReservationRender('reservation-sync');
    });
    return;
  }

  epgProgramPageCallSafe_(function(){
    if (typeof window.__epgProgramPageAnnotateForReservation === 'function') {
      window.__epgProgramPageAnnotateForReservation();
    }
  });

  epgProgramPageCallSafe_(function(){
    if (typeof window.__epgPortalProgramTableRenderNow === 'function') {
      window.__epgPortalProgramTableRenderNow();
      return;
    }

    if (typeof window.__epgPortalProgramTableRenderAll === 'function') {
      window.__epgPortalProgramTableRenderAll();
      return;
    }
  });
}

function epgProgramPageRunReservationSync_(){
  epgProgramPageSyncReservationCache_();

  window.setTimeout(function(){
    epgProgramPageRenderReservationSwitches_();
  }, 0);

  window.setTimeout(function(){
    epgProgramPageRenderReservationSwitches_();
  }, 180);

  window.setTimeout(function(){
    epgProgramPageRenderReservationSwitches_();
  }, 700);
}

if (document.readyState === 'loading') {
  document.addEventListener(
    'DOMContentLoaded',
    epgProgramPageRunReservationSync_,
    { once:true }
  );
} else {
  window.setTimeout(epgProgramPageRunReservationSync_, 0);
}

window.addEventListener('pageshow', function(){
  window.setTimeout(epgProgramPageRunReservationSync_, 0);
});

window.__epgProgramPageRunReservationSync =
  epgProgramPageRunReservationSync_;
)HTML";
        }

        void AppendProgramPageReservationSyncScript_Close_(std::wstring& html)
        {
            html += LR"HTML(
})();
</script>
)HTML";
        }

        void AppendProgramPageReservationSyncScript_(std::wstring& html)
        {
            AppendProgramPageReservationSyncScript_Open_(html);
            AppendProgramPageReservationSyncScript_Data_(html);
            AppendProgramPageReservationSyncScript_Sync_(html);
            AppendProgramPageReservationSyncScript_Render_(html);
            AppendProgramPageReservationSyncScript_Close_(html);
        }

        void AppendLocalScriptOpen_(std::wstring& html)
        {
            html += L"<script>\n";
            html += L"(function(){\n";
            html += L"'use strict';\n";
            html += L"function qsa_(s,root){return Array.prototype.slice.call((root||document).querySelectorAll(s));}\n";
            html += L"function qs_(s,root){return (root||document).querySelector(s);}\n";
            html += L"let activeDay_='';\n";
            html += L"let activeGenre_='all';\n";
            html += L"let reservationRenderSeq_=0;\n";
        }

        void AppendLocalScriptClose_(std::wstring& html)
        {
            html += L"if(document.readyState==='loading'){\n";
            html += L"  document.addEventListener('DOMContentLoaded',init_,{once:true});\n";
            html += L"}else{\n";
            html += L"  init_();\n";
            html += L"}\n";
            html += L"})();\n";
            html += L"</script>\n";
        }

        void AppendLocalDateTimeUtils_(std::wstring& html)
        {
            html += L"function normDate_(s){\n";
            html += L"  s=String(s||'').trim();\n";
            html += L"  if(s==='all')return'all';\n";
            html += L"  const m=s.match(/(\\d{4})[\\/\\-](\\d{1,2})[\\/\\-](\\d{1,2})/);\n";
            html += L"  if(!m)return'';\n";
            html += L"  return m[1]+'/'+String(m[2]).padStart(2,'0')+'/'+String(m[3]).padStart(2,'0');\n";
            html += L"}\n";

            html += L"function isoDateFromSlash_(d){\n";
            html += L"  const n=normDate_(d);\n";
            html += L"  if(!n||n==='all')return'';\n";
            html += L"  return n.replace(/\\//g,'-');\n";
            html += L"}\n";

            html += L"function dateKey_(d){return String(d||'').replace(/[^0-9]/g,'');}\n";

            html += L"function padTimeSec_(t){\n";
            html += L"  t=String(t||'').trim();\n";
            html += L"  const m=t.match(/^(\\d{1,2}):(\\d{2})(?::(\\d{2}))?/);\n";
            html += L"  if(!m)return'';\n";
            html += L"  return String(m[1]).padStart(2,'0')+':'+String(m[2]).padStart(2,'0')+':'+String(m[3]||'00').padStart(2,'0');\n";
            html += L"}\n";

            html += L"function shiftIsoDateByOne_(iso){\n";
            html += L"  const m=String(iso||'').match(/^(\\d{4})-(\\d{2})-(\\d{2})$/);\n";
            html += L"  if(!m)return iso||'';\n";
            html += L"  const d=new Date(Number(m[1]),Number(m[2])-1,Number(m[3])+1);\n";
            html += L"  return String(d.getFullYear()).padStart(4,'0')+'-'+String(d.getMonth()+1).padStart(2,'0')+'-'+String(d.getDate()).padStart(2,'0');\n";
            html += L"}\n";

            html += L"function parseTimeRangeText_(s){\n";
            html += L"  s=String(s||'')\n";
            html += L"    .replace(/&amp;#12316;|&#12316;/g,'〜')\n";
            html += L"    .replace(/\\u301c|\\uff5e/g,'〜')\n";
            html += L"    .replace(/[～〜]/g,'-');\n";
            html += L"  const m=s.match(/(\\d{1,2}:\\d{2})\\s*-\\s*(\\d{1,2}:\\d{2})/);\n";
            html += L"  if(!m){\n";
            html += L"    const one=s.match(/(\\d{1,2}:\\d{2})/);\n";
            html += L"    return {startTime:one?padTimeSec_(one[1]):'',endTime:'',crossesDay:false,durationSec:0};\n";
            html += L"  }\n";
            html += L"  const st=padTimeSec_(m[1]);\n";
            html += L"  const et=padTimeSec_(m[2]);\n";
            html += L"  const sh=Number(st.slice(0,2));\n";
            html += L"  const sm=Number(st.slice(3,5));\n";
            html += L"  const eh=Number(et.slice(0,2));\n";
            html += L"  const em=Number(et.slice(3,5));\n";
            html += L"  let dur=((eh*60+em)-(sh*60+sm))*60;\n";
            html += L"  let cross=false;\n";
            html += L"  if(dur<0){dur+=24*60*60;cross=true;}\n";
            html += L"  return {startTime:st,endTime:et,crossesDay:cross,durationSec:dur};\n";
            html += L"}\n";
        }

        void AppendLocalServiceMapUtils_(std::wstring& html)
        {
            html += L"function normalizeServiceNameForLocalMap_(s){\n";
            html += L"  return String(s||'')\n";
            html += L"    .replace(/&amp;/g,'&')\n";
            html += L"    .replace(/&lt;/g,'<')\n";
            html += L"    .replace(/&gt;/g,'>')\n";
            html += L"    .replace(/&quot;/g,'\\\"')\n";
            html += L"    .replace(/&#39;/g,\"'\")\n";
            html += L"    .replace(/\\s+/g,'')\n";
            html += L"    .trim();\n";
            html += L"}\n";

            html += L"function findServiceInfoByName_(serviceName){\n";
            html += L"  const target=normalizeServiceNameForLocalMap_(serviceName);\n";
            html += L"  if(!target)return null;\n";
            html += L"  const map=window.EPG_SERVICE_MAP||{};\n";
            html += L"  const keys=Object.keys(map);\n";
            html += L"  for(const key of keys){\n";
            html += L"    const x=map[key]||{};\n";
            html += L"    const n1=normalizeServiceNameForLocalMap_(x.serviceName||'');\n";
            html += L"    const n2=normalizeServiceNameForLocalMap_(x.channelName||'');\n";
            html += L"    if(n1===target||n2===target)return Object.assign({serviceKey:key},x);\n";
            html += L"  }\n";
            html += L"  return null;\n";
            html += L"}\n";
        }

        void AppendLocalReservationRenderUtils_(std::wstring& html)
        {
            html += L"function countReservationButtons_(){\n";
            html += L"  const mount=getMount_()||document;\n";
            html += L"  return qsa_('.epg-inline-switch-btn',mount).length;\n";
            html += L"}\n";

            html += L"function renderReservationSwitchesOnce_(){\n";
            html += L"  annotateReservationMeta_();\n";
            html += L"  try{\n";
            html += L"    if(typeof window.__epgPortalProgramTableRenderNow==='function'){\n";
            html += L"      window.__epgPortalProgramTableRenderNow();\n";
            html += L"      return true;\n";
            html += L"    }\n";
            html += L"    if(typeof window.__epgPortalProgramTableRenderAll==='function'){\n";
            html += L"      window.__epgPortalProgramTableRenderAll();\n";
            html += L"      return true;\n";
            html += L"    }\n";
            html += L"  }catch(e){try{console.warn('[program-page-local] reservation render failed',e);}catch(_){}}\n";
            html += L"  return false;\n";
            html += L"}\n";

            html += L"function requestReservationRender_(reason){\n";
            html += L"  const seq=++reservationRenderSeq_;\n";
            html += L"  const delays=[0,80,200,500,1000,1600];\n";
            html += L"  delays.forEach(function(delay){\n";
            html += L"    window.setTimeout(function(){\n";
            html += L"      if(seq!==reservationRenderSeq_)return;\n";
            html += L"      const before=countReservationButtons_();\n";
            html += L"      const called=renderReservationSwitchesOnce_();\n";
            html += L"      const after=countReservationButtons_();\n";
            html += L"      if(after>0&&after>=before)return;\n";
            html += L"      if(!called){try{console.warn('[program-page-local] reservation renderer unavailable',reason||'');}catch(_){}}\n";
            html += L"    },delay);\n";
            html += L"  });\n";
            html += L"}\n";
        }

        void AppendLocalButtonUtils_(std::wstring& html)
        {
            html += L"function setActive_(buttons,btn){\n";
            html += L"  buttons.forEach(function(b){b.classList.remove('active');b.setAttribute('aria-pressed','false');});\n";
            html += L"  if(btn){btn.classList.add('active');btn.setAttribute('aria-pressed','true');}\n";
            html += L"}\n";

            html += L"function getDayButtons_(){return qsa_('.day-btn');}\n";
            html += L"function getGenreButtons_(){return qsa_('.genre-btn');}\n";

            html += L"function findDayButton_(day){\n";
            html += L"  const d=normDate_(day);\n";
            html += L"  const buttons=getDayButtons_();\n";
            html += L"  for(const b of buttons){\n";
            html += L"    if(normDate_(b.getAttribute('data-day')||'')===d)return b;\n";
            html += L"  }\n";
            html += L"  return null;\n";
            html += L"}\n";
        }

        void AppendTimetableMountScript_(std::wstring& html)
        {
            html += L"function getMount_(){return qs_('#epgTimetableMount')||qs_('.epg-timetable-mount');}\n";
            html += L"function getTemplates_(){return qsa_('.epg-timetable-template');}\n";

            html += L"function templateDays_(){\n";
            html += L"  const days=[];\n";
            html += L"  getTemplates_().forEach(function(t){\n";
            html += L"    const d=normDate_(t.getAttribute('data-day')||'');\n";
            html += L"    if(d&&days.indexOf(d)<0)days.push(d);\n";
            html += L"  });\n";
            html += L"  return days;\n";
            html += L"}\n";

            html += L"function firstTemplateDay_(){\n";
            html += L"  const days=templateDays_();\n";
            html += L"  return days.length?days[0]:'';\n";
            html += L"}\n";

            html += L"function findTemplateByDay_(day){\n";
            html += L"  const d=normDate_(day);\n";
            html += L"  if(!d)return null;\n";
            html += L"  const list=getTemplates_();\n";
            html += L"  for(const t of list){\n";
            html += L"    if(normDate_(t.getAttribute('data-day')||'')===d)return t;\n";
            html += L"  }\n";
            html += L"  return null;\n";
            html += L"}\n";

            html += L"function ensureNotice_(){\n";
            html += L"  let n=qs_('#epgTimetableLocalNotice');\n";
            html += L"  if(n)return n;\n";
            html += L"  const daybar=qs_('.day-bar');\n";
            html += L"  n=document.createElement('div');\n";
            html += L"  n.id='epgTimetableLocalNotice';\n";
            html += L"  n.className='epg-local-notice epg-timetable-local-notice';\n";
            html += L"  n.hidden=true;\n";
            html += L"  if(daybar&&daybar.parentNode){daybar.parentNode.insertBefore(n,daybar.nextSibling);}else{document.body.insertBefore(n,document.body.firstChild);}\n";
            html += L"  return n;\n";
            html += L"}\n";

            html += L"function setNotice_(text){\n";
            html += L"  const n=ensureNotice_();\n";
            html += L"  if(!text){n.hidden=true;n.textContent='';return;}\n";
            html += L"  n.textContent=text;\n";
            html += L"  n.hidden=false;\n";
            html += L"}\n";
        }

        void AppendTimetableReservationMetaScript_(std::wstring& html)
        {
            html += L"function readDate_(el){\n";
            html += L"  if(!el||!el.getAttribute)return'';\n";
            html += L"  return normDate_(el.getAttribute('data-date')||el.getAttribute('data-start-date')||el.getAttribute('data-row-date')||'');\n";
            html += L"}\n";

            html += L"function readGenre_(el){\n";
            html += L"  if(!el||!el.getAttribute)return'';\n";
            html += L"  return String(el.getAttribute('data-gm')||el.getAttribute('data-genre-main')||'');\n";
            html += L"}\n";

            html += L"function parseCellTimeRange_(cell){\n";
            html += L"  let s='';\n";
            html += L"  if(cell&&cell.getAttribute){s=cell.getAttribute('data-time')||'';}\n";
            html += L"  if(!s&&cell){\n";
            html += L"    const tm=cell.querySelector&&cell.querySelector('.tm');\n";
            html += L"    if(tm)s=tm.textContent||'';\n";
            html += L"  }\n";
            html += L"  return parseTimeRangeText_(s);\n";
            html += L"}\n";

            html += L"function annotateTimetableDates_(){\n";
            html += L"  const mount=getMount_()||document;\n";
            html += L"  let currentDate=activeDay_||firstTemplateDay_();\n";
            html += L"  let previousClock=-1;\n";
            html += L"  const seen={};\n";
            html += L"  qsa_('table.epg tbody tr',mount).forEach(function(row){\n";
            html += L"    const th=qs_('th.time-col',row);\n";
            html += L"    const label=th?String(th.textContent||'').trim():'';\n";
            html += L"    const explicitDate=normDate_(label);\n";
            html += L"    if(explicitDate){\n";
            html += L"      currentDate=explicitDate;\n";
            html += L"      previousClock=-1;\n";
            html += L"    }\n";
            html += L"    const timeMatch=label.match(/(\\d{1,2}):(\\d{2})/);\n";
            html += L"    let rowClock=-1;\n";
            html += L"    if(timeMatch){\n";
            html += L"      rowClock=Number(timeMatch[1])*60+Number(timeMatch[2]);\n";
            html += L"      if(previousClock>=0&&rowClock+720<previousClock&&currentDate){\n";
            html += L"        currentDate=normDate_(shiftIsoDateByOne_(isoDateFromSlash_(currentDate)));\n";
            html += L"      }\n";
            html += L"      previousClock=rowClock;\n";
            html += L"    }\n";
            html += L"    if(!currentDate)return;\n";
            html += L"    row.setAttribute('data-row-date',currentDate);\n";
            html += L"    if(!seen[currentDate]){\n";
            html += L"      seen[currentDate]=true;\n";
            html += L"      row.id='epg-day-'+dateKey_(currentDate);\n";
            html += L"      row.classList.add('epg-day-anchor');\n";
            html += L"    }\n";
            html += L"    qsa_('td.prog-cell',row).forEach(function(cell){\n";
            html += L"      if(cell.getAttribute('data-date')||cell.getAttribute('data-start-date'))return;\n";
            html += L"      let cellDate=currentDate;\n";
            html += L"      const parsed=parseCellTimeRange_(cell);\n";
            html += L"      const cellClock=parsed.startTime?Number(parsed.startTime.slice(0,2))*60+Number(parsed.startTime.slice(3,5)):-1;\n";
            html += L"      if(rowClock>=0&&cellClock>=0&&cellClock+720<rowClock){\n";
            html += L"        cellDate=normDate_(shiftIsoDateByOne_(isoDateFromSlash_(currentDate)));\n";
            html += L"      } else if(rowClock<0&&previousClock>=0&&cellClock>=0&&cellClock+720<previousClock){\n";
            html += L"        cellDate=normDate_(shiftIsoDateByOne_(isoDateFromSlash_(currentDate)));\n";
            html += L"      }\n";
            html += L"      cell.setAttribute('data-date',cellDate);\n";
            html += L"    });\n";
            html += L"  });\n";
            html += L"}\n";

            html += L"function hasStrictServiceIdentity_(el){\n";
            html += L"  if(!el||!el.getAttribute)return false;\n";
            html += L"  const nid=Number(el.getAttribute('data-network-id')||0);\n";
            html += L"  const tsid=Number(el.getAttribute('data-ts-id')||0);\n";
            html += L"  const sid=Number(el.getAttribute('data-service-id')||0);\n";
            html += L"  return nid>0&&tsid>0&&sid>0;\n";
            html += L"}\n";

            html += L"function buildServiceHeaderInfo_(th,index){\n";
            html += L"  const name=String(\n";
            html += L"    th.getAttribute('data-service-name')||\n";
            html += L"    th.getAttribute('data-service')||\n";
            html += L"    th.textContent||''\n";
            html += L"  ).trim();\n";
            html += L"  return {\n";
            html += L"    index:index,\n";
            html += L"    serviceName:name,\n";
            html += L"    serviceKey:String(th.getAttribute('data-service-key')||''),\n";
            html += L"    networkId:Number(th.getAttribute('data-network-id')||0),\n";
            html += L"    tsId:Number(th.getAttribute('data-ts-id')||0),\n";
            html += L"    serviceId:Number(th.getAttribute('data-service-id')||0)\n";
            html += L"  };\n";
            html += L"}\n";

            html += L"function applyServiceHeaderInfoToCell_(cell,info){\n";
            html += L"  if(!cell||!info)return;\n";
            html += L"  if(cell.getAttribute('data-service-identity-lock')==='1')return;\n";
            html += L"  if(hasStrictServiceIdentity_(cell)){\n";
            html += L"    if(!cell.getAttribute('data-service-identity-source')){\n";
            html += L"      cell.setAttribute('data-service-identity-source','cell');\n";
            html += L"    }\n";
            html += L"    cell.setAttribute('data-service-identity-valid','1');\n";
            html += L"    return;\n";
            html += L"  }\n";
            html += L"  if(info.serviceName){\n";
            html += L"    if(!cell.getAttribute('data-service-name'))cell.setAttribute('data-service-name',info.serviceName);\n";
            html += L"    if(!cell.getAttribute('data-service'))cell.setAttribute('data-service',info.serviceName);\n";
            html += L"  }\n";
            html += L"  if(info.serviceKey&&!cell.getAttribute('data-service-key'))cell.setAttribute('data-service-key',String(info.serviceKey));\n";
            html += L"  if(info.networkId>0&&!cell.getAttribute('data-network-id'))cell.setAttribute('data-network-id',String(info.networkId));\n";
            html += L"  if(info.tsId>0&&!cell.getAttribute('data-ts-id'))cell.setAttribute('data-ts-id',String(info.tsId));\n";
            html += L"  if(info.serviceId>0&&!cell.getAttribute('data-service-id'))cell.setAttribute('data-service-id',String(info.serviceId));\n";
            html += L"  if(!cell.getAttribute('data-service-identity-source'))cell.setAttribute('data-service-identity-source','fallback-grid');\n";
            html += L"  cell.setAttribute('data-service-identity-valid',hasStrictServiceIdentity_(cell)?'1':'0');\n";
            html += L"}\n";

            html += L"function collectServiceHeaders_(table){\n";
            html += L"  const headers=[];\n";
            html += L"  qsa_('thead th.svc, thead th.service',table).forEach(function(th){\n";
            html += L"    headers.push(buildServiceHeaderInfo_(th,headers.length));\n";
            html += L"  });\n";
            html += L"  if(headers.length>0)return headers;\n";
            html += L"  qsa_('thead th',table).forEach(function(th){\n";
            html += L"    if(th.classList&&th.classList.contains('time-col'))return;\n";
            html += L"    const info=buildServiceHeaderInfo_(th,headers.length);\n";
            html += L"    if(info.serviceName)headers.push(info);\n";
            html += L"  });\n";
            html += L"  return headers;\n";
            html += L"}\n";

            html += L"function annotateServiceNamesForMountedTable_(){\n";
            html += L"  const mount=getMount_()||document;\n";
            html += L"  qsa_('table.epg',mount).forEach(function(table){\n";
            html += L"    const serviceHeaders=collectServiceHeaders_(table);\n";
            html += L"    if(serviceHeaders.length<=0)return;\n";
            html += L"    const rows=qsa_('tbody tr',table);\n";
            html += L"    const blockedUntil=[];\n";
            html += L"    rows.forEach(function(row,rowIndex){\n";
            html += L"      let col=0;\n";
            html += L"      Array.prototype.slice.call(row.cells||[]).forEach(function(cell){\n";
            html += L"        while((blockedUntil[col]||0)>rowIndex)++col;\n";
            html += L"        const rs=Math.max(1,Number(cell.rowSpan||1));\n";
            html += L"        const cs=Math.max(1,Number(cell.colSpan||1));\n";
            html += L"        if(cell.classList&&cell.classList.contains('prog-cell')){\n";
            html += L"          const serviceIndex=col-1;\n";
            html += L"          const info=serviceIndex>=0?serviceHeaders[serviceIndex]:null;\n";
            html += L"          applyServiceHeaderInfoToCell_(cell,info);\n";
            html += L"        }\n";
            html += L"        if(rs>1){\n";
            html += L"          const until=rowIndex+rs;\n";
            html += L"          for(let c=0;c<cs;++c){\n";
            html += L"            blockedUntil[col+c]=Math.max(blockedUntil[col+c]||0,until);\n";
            html += L"          }\n";
            html += L"        }\n";
            html += L"        col+=cs;\n";
            html += L"      });\n";
            html += L"    });\n";
            html += L"  });\n";
            html += L"}\n";

            html += L"function annotateServiceIdsForMountedCells_(){\n";
            html += L"  const mount=getMount_()||document;\n";
            html += L"  qsa_('td.prog-cell[data-epg-program=\"1\"]',mount).forEach(function(cell){\n";
            html += L"    if(cell.getAttribute('data-service-identity-lock')==='1')return;\n";
            html += L"    if(hasStrictServiceIdentity_(cell)){\n";
            html += L"      if(!cell.getAttribute('data-service-identity-source'))cell.setAttribute('data-service-identity-source','cell-or-header');\n";
            html += L"      cell.setAttribute('data-service-identity-valid','1');\n";
            html += L"      return;\n";
            html += L"    }\n";
            html += L"    const serviceName=String(cell.getAttribute('data-service-name')||cell.getAttribute('data-service')||'').trim();\n";
            html += L"    const info=findServiceInfoByName_(serviceName);\n";
            html += L"    if(!info){\n";
            html += L"      cell.setAttribute('data-service-identity-source','missing');\n";
            html += L"      cell.setAttribute('data-service-identity-valid','0');\n";
            html += L"      return;\n";
            html += L"    }\n";
            html += L"    if(info.serviceKey&&!cell.getAttribute('data-service-key'))cell.setAttribute('data-service-key',String(info.serviceKey));\n";
            html += L"    if(info.networkId!=null&&!cell.getAttribute('data-network-id'))cell.setAttribute('data-network-id',String(info.networkId));\n";
            html += L"    if(info.tsId!=null&&!cell.getAttribute('data-ts-id'))cell.setAttribute('data-ts-id',String(info.tsId));\n";
            html += L"    if(info.serviceId!=null&&!cell.getAttribute('data-service-id'))cell.setAttribute('data-service-id',String(info.serviceId));\n";
            html += L"    if(!cell.getAttribute('data-service-identity-source'))cell.setAttribute('data-service-identity-source','fallback-name');\n";
            html += L"    cell.setAttribute('data-service-identity-valid',hasStrictServiceIdentity_(cell)?'1':'0');\n";
            html += L"  });\n";
            html += L"}\n";



            html += L"function annotateReservationMeta_(){\n";
            html += L"  annotateTimetableDates_();\n";
            html += L"  annotateServiceNamesForMountedTable_();\n";
            html += L"  annotateServiceIdsForMountedCells_();\n";
            html += L"  const mount=getMount_()||document;\n";
            html += L"  qsa_('td.prog-cell[data-epg-program=\"1\"]',mount).forEach(function(cell){\n";
            html += L"    const slashDate=readDate_(cell)||activeDay_||firstTemplateDay_();\n";
            html += L"    const startDate=isoDateFromSlash_(slashDate);\n";
            html += L"    const parsed=parseCellTimeRange_(cell);\n";
            html += L"    if(startDate&&!cell.getAttribute('data-start-date'))cell.setAttribute('data-start-date',startDate);\n";
            html += L"    if(parsed.startTime&&!cell.getAttribute('data-start-time'))cell.setAttribute('data-start-time',parsed.startTime);\n";
            html += L"    if(parsed.endTime&&!cell.getAttribute('data-end-time'))cell.setAttribute('data-end-time',parsed.endTime);\n";
            html += L"    if(startDate&&!cell.getAttribute('data-end-date'))cell.setAttribute('data-end-date',parsed.crossesDay?shiftIsoDateByOne_(startDate):startDate);\n";
            html += L"    if(parsed.durationSec>0&&!cell.getAttribute('data-duration-sec'))cell.setAttribute('data-duration-sec',String(parsed.durationSec));\n";
            html += L"    if(!cell.getAttribute('data-date')&&slashDate)cell.setAttribute('data-date',slashDate);\n";
            html += L"    if(!cell.getAttribute('data-space'))cell.setAttribute('data-space',String((window.EPG_PROGRAM_PAGE_META&&window.EPG_PROGRAM_PAGE_META.space)||0));\n";
            html += L"  });\n";
            html += L"}\n";

            html += L"function collectMountedDates_(){\n";
            html += L"  annotateTimetableDates_();\n";
            html += L"  const mount=getMount_()||document;\n";
            html += L"  const map={};\n";
            html += L"  qsa_('table.epg tbody tr[data-row-date]',mount).forEach(function(row){\n";
            html += L"    const d=normDate_(row.getAttribute('data-row-date')||'');\n";
            html += L"    if(d)map[d]=true;\n";
            html += L"  });\n";
            html += L"  return Object.keys(map).sort();\n";
            html += L"}\n";
        }

        void AppendTimetableFilterInitScript_(std::wstring& html)
        {
            html += L"function applyTimetableFilter_(){\n";
            html += L"  annotateTimetableDates_();\n";
            html += L"  const mount=getMount_()||document;\n";
            html += L"  qsa_('td.prog-cell',mount).forEach(function(cell){\n";
            html += L"    const g=readGenre_(cell);\n";
            html += L"    const genreOk=(activeGenre_==='all'||!g||g===activeGenre_);\n";
            html += L"    cell.classList.toggle('genre-dim',activeGenre_!=='all'&&!genreOk);\n";
            html += L"  });\n";
            html += L"}\n";

            html += L"function mountDay_(day){\n";
            html += L"  const mount=getMount_();\n";
            html += L"  if(!mount){setNotice_('時間割の描画領域が見つかりません。');return false;}\n";
            html += L"  let d=normDate_(day);\n";
            html += L"  if(!d||d==='all')d=firstTemplateDay_();\n";
            html += L"  const tpl=findTemplateByDay_(d);\n";
            html += L"  if(!tpl){\n";
            html += L"    setNotice_('選択した日付 '+String(day||'')+' の時間割データが見つかりません。内包日付: '+(templateDays_().join(', ')||'なし'));\n";
            html += L"    try{console.warn('[timetable-local] template not found',day,templateDays_());}catch(_){ }\n";
            html += L"    return false;\n";
            html += L"  }\n";
            html += L"  setNotice_('');\n";
            html += L"  mount.textContent='';\n";
            html += L"  mount.appendChild(tpl.content.cloneNode(true));\n";
            html += L"  activeDay_=d;\n";
            html += L"  annotateReservationMeta_();\n";
            html += L"  applyTimetableFilter_();\n";
            html += L"  const wrap=qs_('.epg-wrap',mount)||qs_('.table-wrap',mount);\n";
            html += L"  if(wrap){wrap.scrollTop=0;wrap.scrollLeft=0;}\n";
            html += L"  requestReservationRender_('mount-day');\n";
            html += L"  return true;\n";
            html += L"}\n";

            html += L"function selectDay_(btn){\n";
            html += L"  if(!btn)return;\n";
            html += L"  const raw=btn.getAttribute('data-day')||'';\n";
            html += L"  const day=(raw==='all')?firstTemplateDay_():normDate_(raw);\n";
            html += L"  if(!day)return;\n";
            html += L"  setActive_(getDayButtons_(),btn);\n";
            html += L"  mountDay_(day);\n";
            html += L"}\n";

            html += L"function selectDayByValue_(day){\n";
            html += L"  const btn=findDayButton_(day);\n";
            html += L"  if(btn){selectDay_(btn);return;}\n";
            html += L"  mountDay_(day);\n";
            html += L"}\n";

            html += L"function selectGenre_(btn){\n";
            html += L"  if(!btn)return;\n";
            html += L"  activeGenre_=String(btn.getAttribute('data-genre')||'all');\n";
            html += L"  setActive_(getGenreButtons_(),btn);\n";
            html += L"  applyTimetableFilter_();\n";
            html += L"}\n";

            html += L"function moveDay_(delta){\n";
            html += L"  const days=templateDays_();\n";
            html += L"  if(days.length===0)return;\n";
            html += L"  let idx=days.indexOf(activeDay_);\n";
            html += L"  if(idx<0)idx=delta>0?-1:days.length;\n";
            html += L"  idx=Math.max(0,Math.min(days.length-1,idx+delta));\n";
            html += L"  selectDayByValue_(days[idx]);\n";
            html += L"}\n";

            html += L"function initDayButtons_(){\n";
            html += L"  const buttons=getDayButtons_();\n";
            html += L"  buttons.forEach(function(btn){btn.addEventListener('click',function(){selectDay_(btn);});});\n";
            html += L"  const prev=qs_('#btn-day-prev');\n";
            html += L"  const next=qs_('#btn-day-next');\n";
            html += L"  if(prev)prev.addEventListener('click',function(){moveDay_(-1);});\n";
            html += L"  if(next)next.addEventListener('click',function(){moveDay_(1);});\n";
            html += L"  const first=firstTemplateDay_();\n";
            html += L"  const firstBtn=findDayButton_(first)||getDayButtons_().find(function(b){return (b.getAttribute('data-day')||'')!=='all';});\n";
            html += L"  if(firstBtn){selectDay_(firstBtn);}else if(first){mountDay_(first);}\n";
            html += L"}\n";

            html += L"function initGenreButtons_(){\n";
            html += L"  const buttons=getGenreButtons_();\n";
            html += L"  buttons.forEach(function(btn){btn.addEventListener('click',function(){selectGenre_(btn);});});\n";
            html += L"  const all=qs_('.genre-btn[data-genre=\"all\"]')||buttons[0];\n";
            html += L"  if(all){activeGenre_=all.getAttribute('data-genre')||'all';setActive_(buttons,all);}\n";
            html += L"}\n";

            html += L"function initKeyboard_(){\n";
            html += L"  document.addEventListener('keydown',function(ev){\n";
            html += L"    if(ev.key==='ArrowLeft'){moveDay_(-1);ev.preventDefault();}\n";
            html += L"    else if(ev.key==='ArrowRight'){moveDay_(1);ev.preventDefault();}\n";
            html += L"  });\n";
            html += L"}\n";

            html += L"function init_(){\n";
            html += L"  initGenreButtons_();\n";
            html += L"  initKeyboard_();\n";
            html += L"  initDayButtons_();\n";
            html += L"}\n";
        }

        void AppendTimetablePublicApiScript_(std::wstring& html)
        {
            html += L"window.__epgProgramPageApplyLocalFilters=applyTimetableFilter_;\n";
            html += L"window.__epgProgramPageAnnotateForReservation=annotateReservationMeta_;\n";
            html += L"window.__epgProgramPageRequestReservationRender=requestReservationRender_;\n";
            html += L"window.__epgPortalProgramTableFlushPendingReservationRender=function(reason){requestReservationRender_(reason||'parent-flush');return true;};\n";
            html += L"window.__epgTimetableShowDay=selectDayByValue_;\n";
            html += L"window.__epgTimetableLocalDebug=function(){\n";
            html += L"  const mount=getMount_()||document;\n";
            html += L"  return {\n";
            html += L"    activeDay:activeDay_,\n";
            html += L"    activeGenre:activeGenre_,\n";
            html += L"    templateDays:templateDays_(),\n";
            html += L"    mountedDates:collectMountedDates_(),\n";
            html += L"    templates:getTemplates_().length,\n";
            html += L"    programCells:qsa_('td.prog-cell',mount).length,\n";
            html += L"    reservationMetaCells:qsa_('td.prog-cell[data-start-date][data-start-time][data-service-name]',mount).length,\n";
            html += L"    reservationButtons:qsa_('.epg-inline-switch-btn',mount).length,\n";
            html += L"    renderNow:typeof window.__epgPortalProgramTableRenderNow,\n";
            html += L"    renderAll:typeof window.__epgPortalProgramTableRenderAll\n";
            html += L"  };\n";
            html += L"};\n";
        }

        void AppendTimetableLocalControlsScript_(std::wstring& html)
        {
            AppendLocalScriptOpen_(html);
            AppendLocalDateTimeUtils_(html);
            AppendLocalServiceMapUtils_(html);
            AppendTimetableMountScript_(html);
            AppendTimetableReservationMetaScript_(html);
            AppendLocalReservationRenderUtils_(html);
            AppendLocalButtonUtils_(html);
            AppendTimetableFilterInitScript_(html);
            AppendTimetablePublicApiScript_(html);
            AppendLocalScriptClose_(html);
        }

        void AppendListMountScript_(std::wstring& html)
        {
            html += L"function getMount_(){return qs_('#epgListMount')||qs_('.epg-list-mount');}\n";
            html += L"function getTemplates_(){return qsa_('.epg-list-template');}\n";

            html += L"function templateDays_(){\n";
            html += L"  const days=[];\n";
            html += L"  getTemplates_().forEach(function(t){\n";
            html += L"    const d=normDate_(t.getAttribute('data-day')||'');\n";
            html += L"    if(d&&days.indexOf(d)<0)days.push(d);\n";
            html += L"  });\n";
            html += L"  return days;\n";
            html += L"}\n";

            html += L"function firstTemplateDay_(){\n";
            html += L"  const days=templateDays_();\n";
            html += L"  return days.length?days[0]:'';\n";
            html += L"}\n";

            html += L"function findTemplateByDay_(day){\n";
            html += L"  const d=normDate_(day);\n";
            html += L"  if(!d)return null;\n";
            html += L"  const list=getTemplates_();\n";
            html += L"  for(const t of list){\n";
            html += L"    if(normDate_(t.getAttribute('data-day')||'')===d)return t;\n";
            html += L"  }\n";
            html += L"  return null;\n";
            html += L"}\n";
        }

        void AppendListReservationMetaScript_(std::wstring& html)
        {
            html += L"function readGenre_(row){\n";
            html += L"  if(!row)return'';\n";
            html += L"  if(row.getAttribute){\n";
            html += L"    const g=row.getAttribute('data-gm')||row.getAttribute('data-genre-main')||'';\n";
            html += L"    if(g)return String(g);\n";
            html += L"  }\n";
            html += L"  const p=row.querySelector&&row.querySelector('[data-gm],[data-genre-main]');\n";
            html += L"  if(p)return String(p.getAttribute('data-gm')||p.getAttribute('data-genre-main')||'');\n";
            html += L"  return'';\n";
            html += L"}\n";

            html += L"function parseCellTimeRange_(el){\n";
            html += L"  let s='';\n";
            html += L"  if(el&&el.getAttribute)s=el.getAttribute('data-time')||'';\n";
            html += L"  if(!s&&el){\n";
            html += L"    const row=el.closest&&el.closest('tr');\n";
            html += L"    const tm=row&&row.querySelector&&row.querySelector('.time,td.time');\n";
            html += L"    if(tm)s=tm.textContent||'';\n";
            html += L"  }\n";
            html += L"  return parseTimeRangeText_(s);\n";
            html += L"}\n";

            html += L"function annotateServiceIdsForListItems_(){\n";
            html += L"  const mount=getMount_()||document;\n";
            html += L"  qsa_('[data-epg-program=\"1\"]',mount).forEach(function(el){\n";
            html += L"    let serviceName=String(el.getAttribute('data-service-name')||el.getAttribute('data-service')||'').trim();\n";
            html += L"    if(!serviceName){\n";
            html += L"      const row=el.closest&&el.closest('tr');\n";
            html += L"      const svc=row&&row.querySelector&&row.querySelector('td.svc,.svc');\n";
            html += L"      if(svc)serviceName=String(svc.textContent||'').trim();\n";
            html += L"    }\n";
            html += L"    if(serviceName){\n";
            html += L"      if(!el.getAttribute('data-service-name'))el.setAttribute('data-service-name',serviceName);\n";
            html += L"      if(!el.getAttribute('data-service'))el.setAttribute('data-service',serviceName);\n";
            html += L"    }\n";
            html += L"    const info=findServiceInfoByName_(serviceName);\n";
            html += L"    if(!info)return;\n";
            html += L"    if(info.serviceKey&&!el.getAttribute('data-service-key'))el.setAttribute('data-service-key',String(info.serviceKey));\n";
            html += L"    if(info.networkId!=null&&!el.getAttribute('data-network-id'))el.setAttribute('data-network-id',String(info.networkId));\n";
            html += L"    if(info.tsId!=null&&!el.getAttribute('data-ts-id'))el.setAttribute('data-ts-id',String(info.tsId));\n";
            html += L"    if(info.serviceId!=null&&!el.getAttribute('data-service-id'))el.setAttribute('data-service-id',String(info.serviceId));\n";
            html += L"  });\n";
            html += L"}\n";

            html += L"function annotateReservationMeta_(){\n";
            html += L"  annotateServiceIdsForListItems_();\n";
            html += L"  const mount=getMount_()||document;\n";
            html += L"  qsa_('[data-epg-program=\"1\"]',mount).forEach(function(el){\n";
            html += L"    const slashDate=normDate_(el.getAttribute('data-date')||activeDay_||firstTemplateDay_());\n";
            html += L"    const startDate=isoDateFromSlash_(slashDate);\n";
            html += L"    const parsed=parseCellTimeRange_(el);\n";
            html += L"    if(startDate&&!el.getAttribute('data-start-date'))el.setAttribute('data-start-date',startDate);\n";
            html += L"    if(parsed.startTime&&!el.getAttribute('data-start-time'))el.setAttribute('data-start-time',parsed.startTime);\n";
            html += L"    if(parsed.endTime&&!el.getAttribute('data-end-time'))el.setAttribute('data-end-time',parsed.endTime);\n";
            html += L"    if(startDate&&!el.getAttribute('data-end-date'))el.setAttribute('data-end-date',parsed.crossesDay?shiftIsoDateByOne_(startDate):startDate);\n";
            html += L"    if(parsed.durationSec>0&&!el.getAttribute('data-duration-sec'))el.setAttribute('data-duration-sec',String(parsed.durationSec));\n";
            html += L"    if(!el.getAttribute('data-space'))el.setAttribute('data-space',String((window.EPG_PROGRAM_PAGE_META&&window.EPG_PROGRAM_PAGE_META.space)||0));\n";
            html += L"  });\n";
            html += L"}\n";
        }

        void AppendListFilterInitScript_(std::wstring& html)
        {
            html += L"function applyListFilter_(){\n";
            html += L"  const mount=getMount_()||document;\n";
            html += L"  qsa_('tr.gitem',mount).forEach(function(row){\n";
            html += L"    const g=readGenre_(row);\n";
            html += L"    const genreOk=(activeGenre_==='all'||!g||g===activeGenre_);\n";
            html += L"    row.hidden=!genreOk;\n";
            html += L"  });\n";
            html += L"  qsa_('tr.genre-sep',mount).forEach(function(sep){\n";
            html += L"    const g=readGenre_(sep);\n";
            html += L"    if(activeGenre_!=='all'&&g&&g!==activeGenre_){sep.hidden=true;return;}\n";
            html += L"    let n=sep.nextElementSibling;\n";
            html += L"    let any=false;\n";
            html += L"    while(n){\n";
            html += L"      if(n.classList&&n.classList.contains('genre-sep'))break;\n";
            html += L"      if(n.classList&&n.classList.contains('gitem')&&!n.hidden){any=true;break;}\n";
            html += L"      n=n.nextElementSibling;\n";
            html += L"    }\n";
            html += L"    sep.hidden=!any;\n";
            html += L"  });\n";
            html += L"}\n";

            html += L"function mountDay_(day){\n";
            html += L"  const mount=getMount_();\n";
            html += L"  if(!mount)return false;\n";
            html += L"  let d=normDate_(day);\n";
            html += L"  if(!d||d==='all')d=firstTemplateDay_();\n";
            html += L"  const tpl=findTemplateByDay_(d);\n";
            html += L"  if(!tpl){try{console.warn('[list-local] template not found',day,templateDays_());}catch(_){ }return false;}\n";
            html += L"  mount.textContent='';\n";
            html += L"  mount.appendChild(tpl.content.cloneNode(true));\n";
            html += L"  activeDay_=d;\n";
            html += L"  annotateReservationMeta_();\n";
            html += L"  applyListFilter_();\n";
            html += L"  const wrap=qs_('.table-wrap',mount)||qs_('.epg-wrap',mount);\n";
            html += L"  if(wrap){wrap.scrollTop=0;wrap.scrollLeft=0;}\n";
            html += L"  requestReservationRender_('mount-day');\n";
            html += L"  return true;\n";
            html += L"}\n";

            html += L"function selectDay_(btn){\n";
            html += L"  if(!btn)return;\n";
            html += L"  const raw=btn.getAttribute('data-day')||'';\n";
            html += L"  const day=(raw==='all')?firstTemplateDay_():normDate_(raw);\n";
            html += L"  if(!day)return;\n";
            html += L"  setActive_(getDayButtons_(),btn);\n";
            html += L"  mountDay_(day);\n";
            html += L"}\n";

            html += L"function selectDayByValue_(day){const btn=findDayButton_(day);if(btn){selectDay_(btn);return;}mountDay_(day);}\n";

            html += L"function selectGenre_(btn){\n";
            html += L"  if(!btn)return;\n";
            html += L"  activeGenre_=String(btn.getAttribute('data-genre')||'all');\n";
            html += L"  setActive_(getGenreButtons_(),btn);\n";
            html += L"  applyListFilter_();\n";
            html += L"}\n";

            html += L"function moveDay_(delta){\n";
            html += L"  const days=templateDays_();\n";
            html += L"  if(days.length===0)return;\n";
            html += L"  let idx=days.indexOf(activeDay_);\n";
            html += L"  if(idx<0)idx=delta>0?-1:days.length;\n";
            html += L"  idx=Math.max(0,Math.min(days.length-1,idx+delta));\n";
            html += L"  selectDayByValue_(days[idx]);\n";
            html += L"}\n";

            html += L"function initDayButtons_(){\n";
            html += L"  getDayButtons_().forEach(function(btn){btn.addEventListener('click',function(){selectDay_(btn);});});\n";
            html += L"  const prev=qs_('#btn-day-prev');\n";
            html += L"  const next=qs_('#btn-day-next');\n";
            html += L"  if(prev)prev.addEventListener('click',function(){moveDay_(-1);});\n";
            html += L"  if(next)next.addEventListener('click',function(){moveDay_(1);});\n";
            html += L"  const first=firstTemplateDay_();\n";
            html += L"  const firstBtn=findDayButton_(first)||getDayButtons_().find(function(b){return (b.getAttribute('data-day')||'')!=='all';});\n";
            html += L"  if(firstBtn)selectDay_(firstBtn);else if(first)mountDay_(first);\n";
            html += L"}\n";

            html += L"function initGenreButtons_(){\n";
            html += L"  const buttons=getGenreButtons_();\n";
            html += L"  buttons.forEach(function(btn){btn.addEventListener('click',function(){selectGenre_(btn);});});\n";
            html += L"  const all=qs_('.genre-btn[data-genre=\"all\"]')||buttons[0];\n";
            html += L"  if(all){activeGenre_=all.getAttribute('data-genre')||'all';setActive_(buttons,all);}\n";
            html += L"}\n";

            html += L"function initKeyboard_(){document.addEventListener('keydown',function(ev){if(ev.key==='ArrowLeft'){moveDay_(-1);ev.preventDefault();}else if(ev.key==='ArrowRight'){moveDay_(1);ev.preventDefault();}});}\n";
            html += L"function init_(){initGenreButtons_();initKeyboard_();initDayButtons_();}\n";
        }

        void AppendListPublicApiScript_(std::wstring& html)
        {
            html += L"window.__epgProgramPageApplyLocalFilters=applyListFilter_;\n";
            html += L"window.__epgProgramPageAnnotateForReservation=annotateReservationMeta_;\n";
            html += L"window.__epgProgramPageRequestReservationRender=requestReservationRender_;\n";
            html += L"window.__epgPortalProgramTableFlushPendingReservationRender=function(reason){requestReservationRender_(reason||'parent-flush');return true;};\n";
            html += L"window.__epgListShowDay=selectDayByValue_;\n";
            html += L"window.__epgListLocalDebug=function(){const mount=getMount_()||document;return{activeDay:activeDay_,activeGenre:activeGenre_,templateDays:templateDays_(),templates:getTemplates_().length,rows:qsa_('tr.gitem',mount).length,visibleRows:qsa_('tr.gitem',mount).filter(function(r){return !r.hidden;}).length,reservationMetaItems:qsa_('[data-epg-program=\"1\"][data-start-date][data-start-time][data-service-name]',mount).length,reservationButtons:qsa_('.epg-inline-switch-btn',mount).length,renderNow:typeof window.__epgPortalProgramTableRenderNow,renderAll:typeof window.__epgPortalProgramTableRenderAll};};\n";
        }

        void AppendListLocalControlsScript_(std::wstring& html)
        {
            AppendLocalScriptOpen_(html);
            AppendLocalDateTimeUtils_(html);
            AppendLocalServiceMapUtils_(html);
            AppendListMountScript_(html);
            AppendListReservationMetaScript_(html);
            AppendLocalReservationRenderUtils_(html);
            AppendLocalButtonUtils_(html);
            AppendListFilterInitScript_(html);
            AppendListPublicApiScript_(html);
            AppendLocalScriptClose_(html);
        }

        void AppendTimeShiftPlaceholderScript_(
            std::wstring& html)
        {
            html += L"<script>\n";
            html += L"(function(){\n";
            html += L"'use strict';\n";

            html += L"function qsa_(s,root){return Array.prototype.slice.call((root||document).querySelectorAll(s));}\n";

            html += L"function findInsertTarget_(program){\n";
            html += L"  if(!program)return null;\n";
            html += L"  const title=program.querySelector&&program.querySelector('.t,.title');\n";
            html += L"  return title||program;\n";
            html += L"}\n";

            html += L"function renderTimeShiftPlaceholders_(){\n";
            html += L"  qsa_('[data-epg-program=\"1\"]').forEach(function(program){\n";
            html += L"    if(program.querySelector&&program.querySelector('.epg-timeshift-switch-host'))return;\n";
            html += L"    const host=document.createElement('div');\n";
            html += L"    host.className='epg-timeshift-switch-host';\n";
            html += L"    const btn=document.createElement('button');\n";
            html += L"    btn.type='button';\n";
            html += L"    btn.className='epg-timeshift-btn';\n";
            html += L"    btn.disabled=true;\n";
            html += L"    btn.setAttribute('data-timeshift-disabled','1');\n";
            html += L"    btn.textContent='タイムシフト未実装';\n";
            html += L"    host.appendChild(btn);\n";
            html += L"    const note=document.createElement('span');\n";
            html += L"    note.className='epg-timeshift-note';\n";
            html += L"    note.textContent='過去番組用の予定枠';\n";
            html += L"    host.appendChild(note);\n";
            html += L"    const target=findInsertTarget_(program);\n";
            html += L"    if(target&&target.parentNode){\n";
            html += L"      target.parentNode.insertBefore(host,target.nextSibling);\n";
            html += L"    }else{\n";
            html += L"      program.appendChild(host);\n";
            html += L"    }\n";
            html += L"  });\n";
            html += L"}\n";

            html += L"window.__epgProgramPageRenderTimeShiftPlaceholders=renderTimeShiftPlaceholders_;\n";

            html += L"if(document.readyState==='loading'){\n";
            html += L"  document.addEventListener('DOMContentLoaded',renderTimeShiftPlaceholders_,{once:true});\n";
            html += L"}else{\n";
            html += L"  renderTimeShiftPlaceholders_();\n";
            html += L"}\n";

            html += L"})();\n";
            html += L"</script>\n";
        }

        void AppendAutoReserveAssistScript_(
            std::wstring& html,
            const ProgramPageRequest& req)
        {
            if (!ShouldEnableAutoReserveAssist_(req)) {
                return;
            }

            html += L"<script>\n";
            html += L"(function(){\n";
            html += L"'use strict';\n";
            html += L"var renderQueued=false;\n";
            html += L"var observer=null;\n";
            html += L"var entryUrl=";
            html += ToJsString_(req.autoReserveEntryUrl);
            html += L";\n";
            html += L"function qsa_(selector,root){return Array.prototype.slice.call((root||document).querySelectorAll(selector));}\n";
            html += L"function titleOf_(program){return String(program&&program.getAttribute?program.getAttribute('data-title')||'':'').trim();}\n";
            html += L"function makeUrl_(title){\n";
            html += L"  try{\n";
            html += L"    var url=new URL(entryUrl,document.baseURI);\n";
            html += L"    url.searchParams.set('mode','new');\n";
            html += L"    url.searchParams.set('keyword',title);\n";
            html += L"    return url.href;\n";
            html += L"  }catch(e){\n";
            html += L"    var sep=entryUrl.indexOf('?')>=0?'&':'?';\n";
            html += L"    return entryUrl+sep+'mode=new&keyword='+encodeURIComponent(title);\n";
            html += L"  }\n";
            html += L"}\n";
            html += L"function insertTarget_(program){\n";
            html += L"  if(!program||!program.querySelector)return program;\n";
            html += L"  return program.querySelector('.t,.title')||program;\n";
            html += L"}\n";
            html += L"function renderLinks_(){\n";
            html += L"  qsa_('[data-epg-program=\"1\"]').forEach(function(program){\n";
            html += L"    if(program.querySelector&&program.querySelector('.epg-auto-reserve-assist-host'))return;\n";
            html += L"    var title=titleOf_(program);\n";
            html += L"    if(!title)return;\n";
            html += L"    var host=document.createElement('div');\n";
            html += L"    host.className='epg-auto-reserve-assist-host';\n";
            html += L"    var link=document.createElement('a');\n";
            html += L"    link.className='epg-auto-reserve-assist-link';\n";
            html += L"    link.href=makeUrl_(title);\n";
            html += L"    link.textContent='この番組名で自動予約';\n";
            html += L"    link.setAttribute('aria-label','「'+title+'」を検索ワードにして自動予約設定を作る');\n";
            html += L"    link.addEventListener('click',function(ev){ev.stopPropagation();});\n";
            html += L"    host.appendChild(link);\n";
            html += L"    var target=insertTarget_(program);\n";
            html += L"    var tag=target&&target.tagName?String(target.tagName).toLowerCase():'';\n";
            html += L"    if(target===program||tag==='td'||tag==='th'){\n";
            html += L"      if(target&&target.appendChild)target.appendChild(host);\n";
            html += L"    }else if(target&&target.parentNode){\n";
            html += L"      target.parentNode.insertBefore(host,target.nextSibling);\n";
            html += L"    }\n";
            html += L"  });\n";
            html += L"}\n";
            html += L"function queueRender_(){\n";
            html += L"  if(renderQueued)return;\n";
            html += L"  renderQueued=true;\n";
            html += L"  window.requestAnimationFrame(function(){renderQueued=false;renderLinks_();});\n";
            html += L"}\n";
            html += L"function init_(){\n";
            html += L"  renderLinks_();\n";
            html += L"  var root=document.getElementById('programTableWrap')||document.body;\n";
            html += L"  if(window.MutationObserver&&root){\n";
            html += L"    observer=new MutationObserver(queueRender_);\n";
            html += L"    observer.observe(root,{childList:true,subtree:true});\n";
            html += L"    window.__epgAutoReserveAssistObserver=observer;\n";
            html += L"  }\n";
            html += L"}\n";
            html += L"window.__epgProgramPageRenderAutoReserveAssist=renderLinks_;\n";
            html += L"if(document.readyState==='loading'){\n";
            html += L"  document.addEventListener('DOMContentLoaded',init_,{once:true});\n";
            html += L"}else{\n";
            html += L"  init_();\n";
            html += L"}\n";
            html += L"})();\n";
            html += L"</script>\n";
        }

        void AppendViewLocalControlsScript_(
            std::wstring& html,
            const ProgramPageRequest& req)
        {
            switch (req.viewKind) {
            case ProgramPageViewKind::Timetable:
                AppendTimetableLocalControlsScript_(html);
                break;
            case ProgramPageViewKind::List:
                AppendListLocalControlsScript_(html);
                break;
            case ProgramPageViewKind::Legacy:
            default:
                break;
            }
        }

        std::wstring BuildHtml_(const ProgramPageRequest& req)
        {
            const std::wstring reservationApiBase = ResolveReservationApiBase_();
            const int reservationPreferredTuner = ResolveReservationPreferredTuner_();

            std::wstring bodyHtml = ResolveBodyHtml_(req);

            if (req.viewKind == ProgramPageViewKind::Timetable) {
                bodyHtml = CanonicalizeTimetableIdentityByServiceOrder_(
                    bodyHtml,
                    req);
            }
            const std::wstring kindText = PageKindToText_(req.kind);
            const std::wstring viewText = ViewKindToText_(req.viewKind);
            const std::wstring pageTitle = ResolvePageTitle_(req);
            const std::wstring bodyClass = ViewKindToBodyClass_(req.viewKind);
            const std::wstring noteText = ViewKindToNote_(req.viewKind);

            std::wstring html;
            html.reserve(bodyHtml.size() + 26000);

            html += L"<!doctype html>\n";
            html += L"<html lang=\"ja\">\n";
            html += L"<head>\n";
            html += L"<meta charset=\"utf-8\">\n";
            html += L"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n";
            html += L"<title>";
            html += EscapeHtml_(pageTitle);
            html += L"</title>\n";

            AppendStyles_(html, req.viewKind);
            AppendRuntimeMetaScript_(html, req, reservationApiBase, reservationPreferredTuner);
            AppendExternalScripts_(html, req);

            html += L"</head>\n";
            html += L"<body class=\"epg-program-page ";
            html += EscapeAttr_(bodyClass);
            html += L"\" data-epg-page-kind=\"";
            html += EscapeAttr_(kindText);
            html += L"\" data-epg-page-view=\"";
            html += EscapeAttr_(viewText);
            html += L"\" data-epg-space=\"";
            html += std::to_wstring(req.space);
            html += L"\">\n";

            html += L"<div class=\"page-shell\">\n";
            html += L"  <header class=\"page-header\">\n";
            html += L"    <h1 class=\"page-title\">";
            html += EscapeHtml_(pageTitle);
            html += L"</h1>\n";
            html += L"    <p class=\"page-meta\">";
            html += EscapeHtml_(
                req.spaceLabel.empty()
                ? (L"Space " + std::to_wstring(req.space))
                : req.spaceLabel);
            html += L" / ";
            html += EscapeHtml_(req.kind == ProgramPageKind::Before ? L"過去番組表" : L"現在・今後番組表");
            html += L" / ";
            html += EscapeHtml_(viewText);
            html += L"</p>\n";
            html += L"  </header>\n";

            html += L"  <main class=\"page-main\">\n";
            html += L"    <div class=\"program-table-wrap\" id=\"programTableWrap\">\n";

            AppendSubChannelViewBarHtml_(html, req.viewKind);

            html += bodyHtml;
            html += L"\n";
            html += L"    </div>\n";
            html += L"    <div class=\"program-table-note\">";
            html += EscapeHtml_(noteText);
            html += L"</div>\n";
            html += L"  </main>\n";
            html += L"</div>\n";

            if (ShouldShowSubChannelSwitch_(req.viewKind)) {
                AppendSubChannelViewSwitchScript_(html);
            }

            AppendViewLocalControlsScript_(html, req);
            AppendAutoReserveAssistScript_(html, req);

            if (ShouldEnableTimeShiftSwitch_(req)) {
                AppendTimeShiftPlaceholderScript_(html);
            }

            if (ShouldEnableReservationSwitch_(req)) {
                AppendProgramPageReservationSyncScript_(html);
            }

            html += L"</body>\n";
            html += L"</html>\n";

            return html;
        }

        bool EnsureParentDirectory_(
            const std::filesystem::path& filePath)
        {
            try {
                const std::filesystem::path parent = filePath.parent_path();
                if (parent.empty()) {
                    return true;
                }

                if (std::filesystem::exists(parent)) {
                    return std::filesystem::is_directory(parent);
                }

                return std::filesystem::create_directories(parent);
            }
            catch (...) {
                return false;
            }
        }

        bool WriteProgramPageInternal_(
            const EpgHtmlPortal::WriteOptions& opt,
            const ProgramPageRequest& req,
            const wchar_t* logPrefix)
        {
            if (opt.outputDir.empty()) {
                DebugLogFormat(L"[%ls][ERROR] outputDir is empty", logPrefix ? logPrefix : L"EPG-PAGE");
                return false;
            }

            if (req.outputFileName.empty()) {
                DebugLogFormat(L"[%ls][ERROR] outputFileName is empty", logPrefix ? logPrefix : L"EPG-PAGE");
                return false;
            }

            if (req.writeServiceMap) {
                if (!WriteServiceMapScript(opt, req)) {
                    DebugLogFormat(L"[%ls][WARN] service map write failed", logPrefix ? logPrefix : L"EPG-PAGE");
                }
            }

            const std::filesystem::path outPath = opt.outputDir / req.outputFileName;
            const std::wstring html = BuildHtml_(req);

            if (!EnsureParentDirectory_(outPath)) {
                DebugLogFormat(
                    L"[%ls][ERROR] failed to create parent dir: %ls",
                    logPrefix ? logPrefix : L"EPG-PAGE",
                    outPath.parent_path().wstring().c_str());
                return false;
            }

            const bool ok = EpgHtmlPortal_Common::WriteUtf8BomTextFile(outPath, html);
            if (ok) {
                DebugLogFormat(
                    L"[%ls] generated: %ls",
                    logPrefix ? logPrefix : L"EPG-PAGE",
                    outPath.wstring().c_str());
            }
            else {
                DebugLogFormat(
                    L"[%ls][ERROR] failed: %ls",
                    logPrefix ? logPrefix : L"EPG-PAGE",
                    outPath.wstring().c_str());
            }

            return ok;
        }


    }//namespace

    bool HasValidServiceIdentity(
        int networkId,
        int tsId,
        int serviceId) noexcept
    {
        return networkId > 0 && tsId > 0 && serviceId > 0;
    }

    bool HasValidProgramCellIdentity(
        const ProgramCellMeta& meta) noexcept
    {
        if (!HasValidServiceIdentity(
            meta.networkId,
            meta.tsId,
            meta.serviceId)) {
            return false;
        }

        if (meta.eventId <= 0) {
            return false;
        }

        if (meta.title.empty()) {
            return false;
        }

        return true;
    }

    std::wstring BuildServiceHeaderDataAttributes(
        const ChannelServiceInfo& service)
    {
        std::wstring attrs;
        attrs.reserve(512);

        const std::wstring key =
            service.serviceKey.empty()
            ? MakeServiceKeySafe_(
                service.space,
                service.networkId,
                service.tsId,
                service.serviceId)
            : service.serviceKey;

        attrs += L" data-epg-service=\"1\"";
        attrs += L" data-space=\"" + std::to_wstring(service.space) + L"\"";
        attrs += L" data-service-key=\"" + EscapeAttr_(key) + L"\"";
        attrs += L" data-network-id=\"" + std::to_wstring(service.networkId) + L"\"";
        attrs += L" data-ts-id=\"" + std::to_wstring(service.tsId) + L"\"";
        attrs += L" data-service-id=\"" + std::to_wstring(service.serviceId) + L"\"";

        if (service.physicalChannel >= 0) {
            attrs += L" data-physical-channel=\"" +
                std::to_wstring(service.physicalChannel) + L"\"";
        }

        if (!service.serviceName.empty()) {
            attrs += L" data-service-name=\"" +
                EscapeAttr_(service.serviceName) + L"\"";
            attrs += L" data-service=\"" +
                EscapeAttr_(service.serviceName) + L"\"";
        }

        if (!service.channelName.empty()) {
            attrs += L" data-channel-name=\"" +
                EscapeAttr_(service.channelName) + L"\"";
        }

        if (!service.remoteControlKeyId.empty()) {
            attrs += L" data-remote-control-key-id=\"" +
                EscapeAttr_(service.remoteControlKeyId) + L"\"";
        }

        attrs += HasValidServiceIdentity(
            service.networkId,
            service.tsId,
            service.serviceId)
            ? L" data-service-identity-valid=\"1\""
            : L" data-service-identity-valid=\"0\"";

        return attrs;
    }

    std::wstring MakeServiceKey(
        int space,
        int networkId,
        int tsId,
        int serviceId)
    {
        return MakeServiceKeySafe_(space, networkId, tsId, serviceId);
    }

    std::wstring BuildProgramCellDataAttributes(const ProgramCellMeta& meta)
    {
        std::wstring attrs;
        attrs.reserve(1024);

        const std::wstring serviceKey = ResolveServiceKey_(meta);

        attrs += L" data-epg-program=\"1\"";

        attrs += L" data-space=\"" + std::to_wstring(meta.space) + L"\"";
        attrs += L" data-service-key=\"" + EscapeAttr_(serviceKey) + L"\"";

        attrs += L" data-network-id=\"" + std::to_wstring(meta.networkId) + L"\"";
        attrs += L" data-ts-id=\"" + std::to_wstring(meta.tsId) + L"\"";
        attrs += L" data-service-id=\"" + std::to_wstring(meta.serviceId) + L"\"";
        attrs += L" data-event-id=\"" + std::to_wstring(meta.eventId) + L"\"";

        attrs += HasValidProgramCellIdentity(meta)
            ? L" data-program-identity-valid=\"1\""
            : L" data-program-identity-valid=\"0\"";

        attrs += HasValidServiceIdentity(meta.networkId, meta.tsId, meta.serviceId)
            ? L" data-service-identity-valid=\"1\""
            : L" data-service-identity-valid=\"0\"";

        attrs += L" data-title=\"" + EscapeAttr_(meta.title) + L"\"";
        attrs += L" data-date=\"" + EscapeAttr_(meta.dateText) + L"\"";
        attrs += L" data-weekday=\"" + EscapeAttr_(meta.weekdayText) + L"\"";
        attrs += L" data-time=\"" + EscapeAttr_(meta.timeText) + L"\"";

        if (!meta.startDate.empty()) {
            attrs += L" data-start-date=\"" + EscapeAttr_(meta.startDate) + L"\"";
        }
        if (!meta.startTime.empty()) {
            attrs += L" data-start-time=\"" + EscapeAttr_(meta.startTime) + L"\"";
        }
        if (!meta.endDate.empty()) {
            attrs += L" data-end-date=\"" + EscapeAttr_(meta.endDate) + L"\"";
        }
        if (!meta.endTime.empty()) {
            attrs += L" data-end-time=\"" + EscapeAttr_(meta.endTime) + L"\"";
        }
        if (meta.durationSec > 0) {
            attrs += L" data-duration-sec=\"" + std::to_wstring(meta.durationSec) + L"\"";
        }

        if (!meta.serviceName.empty()) {
            attrs += L" data-service=\"" + EscapeAttr_(meta.serviceName) + L"\"";
            attrs += L" data-service-name=\"" + EscapeAttr_(meta.serviceName) + L"\"";
        }

        if (!meta.summary.empty()) {
            attrs += L" data-summary=\"" + EscapeAttr_(meta.summary) + L"\"";
        }

        if (!meta.genre.empty()) {
            attrs += L" data-genre=\"" + EscapeAttr_(meta.genre) + L"\"";
        }

        return attrs;
    }

    bool WriteServiceMapScript(
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramPageRequest& req)
    {
        if (opt.outputDir.empty()) {
            DebugLog(L"[EPG-PAGE][SERVICE-MAP][ERROR] outputDir is empty");
            return false;
        }

        const std::wstring fileName =
            req.serviceMapFileName.empty()
            ? L"epg_service_map.js"
            : req.serviceMapFileName;

        const std::filesystem::path outPath = opt.outputDir / fileName;

        std::wstring js;
        js.reserve(4096 + req.services.size() * 256);

        js += L"window.EPG_SERVICE_MAP = window.EPG_SERVICE_MAP || {};\n";
        js += L"(function(){\n";
        js += L"  const map = window.EPG_SERVICE_MAP;\n";

        for (const auto& svc : req.services) {
            const std::wstring key =
                svc.serviceKey.empty()
                ? MakeServiceKeySafe_(svc.space, svc.networkId, svc.tsId, svc.serviceId)
                : svc.serviceKey;

            if (key.empty()) {
                continue;
            }

            js += L"  map[";
            js += ToJsString_(key);
            js += L"] = {\n";

            js += L"    serviceKey: ";
            js += ToJsString_(key);
            js += L",\n";

            js += L"    space: ";
            js += std::to_wstring(svc.space);
            js += L",\n";

            js += L"    physicalChannel: ";
            js += std::to_wstring(svc.physicalChannel);
            js += L",\n";

            js += L"    networkId: ";
            js += std::to_wstring(svc.networkId);
            js += L",\n";

            js += L"    tsId: ";
            js += std::to_wstring(svc.tsId);
            js += L",\n";

            js += L"    serviceId: ";
            js += std::to_wstring(svc.serviceId);
            js += L",\n";

            js += L"    serviceName: ";
            js += ToJsString_(svc.serviceName);
            js += L",\n";

            js += L"    channelName: ";
            js += ToJsString_(svc.channelName.empty() ? svc.serviceName : svc.channelName);
            js += L",\n";

            js += L"    remoteControlKeyId: ";
            js += ToJsString_(svc.remoteControlKeyId);
            js += L"\n";

            js += L"  };\n";
        }

        js += L"})();\n";

        const bool ok = EpgHtmlPortal_Common::WriteUtf8BomTextFile(outPath, js);
        if (ok) {
            DebugLogFormat(
                L"[EPG-PAGE][SERVICE-MAP] generated: %ls services=%zu",
                outPath.wstring().c_str(),
                req.services.size());
        }
        else {
            DebugLogFormat(
                L"[EPG-PAGE][SERVICE-MAP][ERROR] failed: %ls",
                outPath.wstring().c_str());
        }

        return ok;
    }

    bool WriteProgramPage(
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramPageRequest& req)
    {
        ProgramPageRequest local = req;
        if (local.viewKind != ProgramPageViewKind::Timetable &&
            local.viewKind != ProgramPageViewKind::List) {
            local.viewKind = ProgramPageViewKind::Legacy;
        }

        return WriteProgramPageInternal_(opt, local, L"EPG-PAGE");
    }

    bool WritePastProgramTimetablePage(
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramPageRequest& req)
    {
        ProgramPageRequest local = req;

        local.kind = ProgramPageKind::Before;
        local.viewKind = ProgramPageViewKind::Timetable;
        local.enableReservationSwitch = false;
        local.enableTimeShiftSwitch = true;

        if (local.timetableHtml.empty() && !local.tableHtml.empty()) {
            local.timetableHtml = local.tableHtml;
        }

        if (local.htmlTitle.empty()) {
            local.htmlTitle = local.spaceLabel;
            if (!local.archiveDateLabel.empty()) {
                local.htmlTitle += L" ";
                local.htmlTitle += local.archiveDateLabel;
            }
            local.htmlTitle += L" 過去 時間割";
        }

        return WriteProgramPageInternal_(
            opt,
            local,
            L"EPG-PAGE-PAST-TIMETABLE");
    }

    bool WritePastProgramListPage(
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramPageRequest& req)
    {
        ProgramPageRequest local = req;

        local.kind = ProgramPageKind::Before;
        local.viewKind = ProgramPageViewKind::List;
        local.enableReservationSwitch = false;
        local.enableTimeShiftSwitch = true;

        if (local.listHtml.empty() && !local.tableHtml.empty()) {
            local.listHtml = local.tableHtml;
        }

        if (local.htmlTitle.empty()) {
            local.htmlTitle = local.spaceLabel;
            if (!local.archiveDateLabel.empty()) {
                local.htmlTitle += L" ";
                local.htmlTitle += local.archiveDateLabel;
            }
            local.htmlTitle += L" 過去 一覧";
        }

        return WriteProgramPageInternal_(
            opt,
            local,
            L"EPG-PAGE-PAST-LIST");
    }

    bool WriteProgramTimetablePage(
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramPageRequest& req)
    {
        ProgramPageRequest local = req;
        local.viewKind = ProgramPageViewKind::Timetable;

        if (local.timetableHtml.empty() && !local.tableHtml.empty()) {
            local.timetableHtml = local.tableHtml;
        }

        return WriteProgramPageInternal_(opt, local, L"EPG-PAGE-TIMETABLE");
    }

    bool WriteProgramListPage(
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramPageRequest& req)
    {
        ProgramPageRequest local = req;
        local.viewKind = ProgramPageViewKind::List;

        if (local.listHtml.empty() && !local.tableHtml.empty()) {
            local.listHtml = local.tableHtml;
        }

        return WriteProgramPageInternal_(opt, local, L"EPG-PAGE-LIST");
    }
}
