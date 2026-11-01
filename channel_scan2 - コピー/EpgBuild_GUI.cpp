// EpgBuild_GUI.cpp (C++20) - 実装（依存をcppに寄せる版）
#include "EpgBuild_GUI.hpp"
#include "EpgHtmlPortal_ProgramPageWriter.hpp"
#include "ConfigManager.h"
#include "EpgHtmlPortal_Common.hpp"
#include "EpgHtmlLite.hpp"
#include "EpgHtmlLite_DateSplit.hpp"
#include "EitEventUpdateUnit.hpp"
#include "EITEventSortUnit.hpp"
#include "AribDecoder.hpp"
#include "EpgHtmlPortal.hpp"
#include "EpgHtmlPortal_ReservationData.hpp"
#include "Reservation_EpgRebuildAction.hpp"
#include "EpgHtmlPortal_SearchData.hpp"

#include "AppSimdPolicy.hpp"
#include "AutoReserveRuleStore.hpp"


#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <fstream>
#include <sstream>
#include <set>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <cwctype>
#include <exception>
#include <new>
#include <type_traits>
#include <cstdint>
#include <ctime>
#include <unordered_map>
#include <initializer_list>
#include <utility>
#include <string_view>

#include "DebugManager.h"
#include "IBonDriver2.h"

#include "FilterManager.hpp"

#include "ChannelListLoader.hpp"
#include "TSParser.h"
#include "FilterManagerRunner.hpp"
#include "MPMCRingBuffer.hpp"
#include "ts_receiver.hpp"
#include "TSStreamUtils.hpp"
#include "TSSectionValidator.hpp"



// ============================================================
// EPG archive save fixed test settings
//
// mode:
//   1 = Month
//   2 = Year
//
// 最初は必ず DRY_RUN=1 で確認する。
// 実ファイルを変更する時だけ EPG_ARCHIVE_SAVE_DRY_RUN を 0 にする。
// ============================================================

#ifndef EPG_ARCHIVE_SAVE_ENABLE
#define EPG_ARCHIVE_SAVE_ENABLE 1
#endif

#ifndef EPG_ARCHIVE_SAVE_DRY_RUN
#define EPG_ARCHIVE_SAVE_DRY_RUN 0
#endif

#ifndef EPG_ARCHIVE_SAVE_MODE
#define EPG_ARCHIVE_SAVE_MODE 1
#endif

#ifndef EPG_ARCHIVE_SAVE_KEEP_RECENT_MONTHS
#define EPG_ARCHIVE_SAVE_KEEP_RECENT_MONTHS 2
#endif

#ifndef EPG_ARCHIVE_SAVE_YEAR_START_MONTH
#define EPG_ARCHIVE_SAVE_YEAR_START_MONTH 2
#endif

#ifndef EPG_ARCHIVE_SAVE_MAKE_BACKUP
#define EPG_ARCHIVE_SAVE_MAKE_BACKUP 1
#endif

#include "Epg_archive_save.hpp"

extern bool WriteEitDumpTextFileFromParsed(const std::wstring& outPath,
    const std::vector<ParsedEITPacket>& events,
    int space, int ch,
    bool enableAribTrace);

extern bool GenerateEITHtmlProgramTable(const std::wstring& mergedEpgPath,
    const std::wstring& channelListPath,
    const std::wstring& outputHtmlPath);



namespace EpgBuildGUI
{
    void CancelToken::RequestCancel() noexcept { cancelled_.store(true, std::memory_order_release); }
    void CancelToken::Reset() noexcept { cancelled_.store(false, std::memory_order_release); }
    bool CancelToken::IsCancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }

    static void SafeLog(const Callbacks* cb, std::wstring_view s)
    {
        if (cb && cb->onLog) cb->onLog(s);
    }
    static void SafeOverall(const Callbacks* cb, std::size_t done, std::size_t total)
    {
        if (cb && cb->onOverallProgress) cb->onOverallProgress(done, total);
    }
    static void SafeChannelStart(const Callbacks* cb, int space, int ch, std::wstring_view name)
    {
        if (cb && cb->onChannelStart) cb->onChannelStart(space, ch, name);
    }
    static void SafeCycle(const Callbacks* cb, int space, int ch, int cycle,
        std::size_t rawEvents, std::size_t storeSize, std::size_t processedPackets)
    {
        if (cb && cb->onCycleProgress) cb->onCycleProgress(space, ch, cycle, rawEvents, storeSize, processedPackets);
    }

    static void SafeAutoReserveMatchResult_(
        const Callbacks* cb,
        const AutoReserve::RuleMatchResult& matchResult,
        const EpgHtmlPortal::WriteOptions& portalOpt,
        const wchar_t* reason) noexcept
    {
        if (cb == nullptr || !cb->onAutoReserveMatchResult) {
            return;
        }

        try {
            const std::wstring reasonText =
                reason != nullptr ? std::wstring(reason) : std::wstring{};

            cb->onAutoReserveMatchResult(
                matchResult,
                portalOpt,
                reasonText);
        }
        catch (const std::bad_alloc&) {
            DebugLog(L"[AUTO-RESERVE][CALLBACK][ERROR] bad_alloc");
        }
        catch (const std::exception& e) {
            DebugLogFormat(
                L"[AUTO-RESERVE][CALLBACK][ERROR] exception what=%S",
                e.what());
        }
        catch (...) {
            DebugLog(L"[AUTO-RESERVE][CALLBACK][ERROR] unknown exception");
        }
    }

    static bool SafeAutoReserveRegisterRequest_(
        const Callbacks* cb,
        const EpgHtmlPortal::WriteOptions& portalOpt,
        const std::wstring& reason) noexcept
    {
        if (cb == nullptr ||
            !cb->onAutoReserveRegisterRequest) {
            return false;
        }

        try {
            cb->onAutoReserveRegisterRequest(
                portalOpt,
                reason);

            return true;
        }
        catch (const std::bad_alloc&) {
            DebugLog(L"[AUTO-RESERVE][REGISTER-CALLBACK][ERROR] bad_alloc");
        }
        catch (const std::exception& e) {
            DebugLogFormat(
                L"[AUTO-RESERVE][REGISTER-CALLBACK][ERROR] exception what=%S",
                e.what());
        }
        catch (...) {
            DebugLog(L"[AUTO-RESERVE][REGISTER-CALLBACK][ERROR] unknown exception");
        }

        return false;
    }

    static const wchar_t* ToRouteName(HtmlRenderRoute route) noexcept
    {
        switch (route) {
        case HtmlRenderRoute::FixedLegacy:            return L"FixedLegacy";
        case HtmlRenderRoute::FixedProgramPageWriter: return L"FixedProgramPageWriter";
        case HtmlRenderRoute::FixedPlugin:            return L"FixedPlugin";
        case HtmlRenderRoute::AutoPluginThenLegacy:   return L"AutoPluginThenLegacy";
        case HtmlRenderRoute::FixedLite:              return L"FixedLite";
        default:                                      return L"Unknown";
        }
    }

    static long long EpgDiagElapsedMs_(
        const std::chrono::steady_clock::time_point& start) noexcept
    {
        const auto now = std::chrono::steady_clock::now();

        return static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - start).count());
    }

    static std::uintmax_t EpgDiagFileSizeNoThrow_(
        const std::filesystem::path& path) noexcept
    {
        try {
            if (path.empty()) {
                return 0;
            }

            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            return ec ? 0 : size;
        }
        catch (...) {
            return 0;
        }
    }

    static const wchar_t* EpgDiagTunerRoleName_(
        ConfigManager::TunerUseRole role) noexcept
    {
        switch (role) {
        case ConfigManager::TunerUseRole::General:
            return L"General";
        case ConfigManager::TunerUseRole::RecordOnly:
            return L"RecordOnly";
        case ConfigManager::TunerUseRole::EpgOnly:
            return L"EpgOnly";
        case ConfigManager::TunerUseRole::ScanOnly:
            return L"ScanOnly";
        case ConfigManager::TunerUseRole::ViewOnly:
            return L"ViewOnly";
        case ConfigManager::TunerUseRole::TimeShiftOnly:
            return L"TimeShiftOnly";
        default:
            return L"Unknown";
        }
    }

    static void EpgDiagLogPhase_(
        const wchar_t* phase,
        const Options& o,
        IBonDriver2* bon,
        const std::chrono::steady_clock::time_point& start) noexcept
    {
        try {
            DebugLogFormat(
                L"[EPG][DIAG][PHASE] phase=%ls route=%ls elapsedMs=%lld bon=%p space=%d captureMode=%u outputDir=%ls merged=%ls html=%ls beforeMerged=%ls",
                phase ? phase : L"(null)",
                ToRouteName(o.htmlRenderRoute),
                EpgDiagElapsedMs_(start),
                static_cast<void*>(bon),
                o.space,
                static_cast<unsigned int>(o.captureMode),
                o.outputDir.c_str(),
                o.mergedEpgFileName.c_str(),
                o.htmlFileName.c_str(),
                o.beforeMergedEpgFileName.c_str());
        }
        catch (...) {
        }
    }

    static void EpgDiagLogFile_(
        const wchar_t* phase,
        const wchar_t* label,
        const std::filesystem::path& path) noexcept
    {
        try {
            DebugLogFormat(
                L"[EPG][DIAG][FILE] phase=%ls label=%ls exists=%d size=%llu path=%ls",
                phase ? phase : L"(null)",
                label ? label : L"(null)",
                std::filesystem::exists(path) ? 1 : 0,
                static_cast<unsigned long long>(EpgDiagFileSizeNoThrow_(path)),
                path.wstring().c_str());
        }
        catch (...) {
        }
    }

    static void EpgDiagLogTunerConfigSnapshot_(
        const wchar_t* phase) noexcept
    {
        try {
            ConfigManager& cfg = ConfigManager::GetInstance();
            const int count = cfg.GetTunersCount();

            DebugLogFormat(
                L"[EPG][DIAG][TUNER-SNAPSHOT] phase=%ls count=%d",
                phase ? phase : L"(null)",
                count);

            for (int i = 0; i < count; ++i) {
                const auto role = cfg.GetTunerUseRole(i);

                DebugLogFormat(
                    L"[EPG][DIAG][TUNER] phase=%ls tuner=%d enabled=%d role=%d(%ls) allowParallel=%d priority=%d tsSpace=%d tsCh=%d tsSid=%u tsNid=%u tsTsid=%u internalBon=%ls tvtestBon=%ls",
                    phase ? phase : L"(null)",
                    i,
                    cfg.GetTunerEnabled(i) ? 1 : 0,
                    static_cast<int>(role),
                    EpgDiagTunerRoleName_(role),
                    cfg.GetTunerAllowParallel(i) ? 1 : 0,
                    cfg.GetTunerPriority(i),
                    cfg.GetTunerTimeShiftLockSpace(i),
                    cfg.GetTunerTimeShiftLockChannel(i),
                    static_cast<unsigned int>(cfg.GetTunerTimeShiftLockServiceId(i)),
                    static_cast<unsigned int>(cfg.GetTunerTimeShiftLockNetworkId(i)),
                    static_cast<unsigned int>(cfg.GetTunerTimeShiftLockTsId(i)),
                    cfg.GetTunerInternalBonDriverPath(i).c_str(),
                    cfg.GetTunerTvTestBonDriverPath(i).c_str());
            }
        }
        catch (...) {
        }
    }

    static EpgBuildGUI::HtmlRenderRoute ConvertConfigHtmlRenderRoute_(
        ConfigManager::EpgHtmlRenderRoute v) noexcept
    {
        using Src = ConfigManager::EpgHtmlRenderRoute;
        using Dst = EpgBuildGUI::HtmlRenderRoute;

        switch (v) {
        case Src::ProgramPageWriter:
            return Dst::FixedProgramPageWriter;
        case Src::Plugin:
            return Dst::FixedPlugin;
        case Src::Auto:
            return Dst::AutoPluginThenLegacy;
        case Src::Lite:
            return Dst::FixedLite;
        case Src::Legacy:
        default:
            return Dst::FixedLegacy;
        }
    }

    static std::wstring ToLowerCopy_(std::wstring s)
    {
        for (wchar_t& ch : s) {
            ch = static_cast<wchar_t>(::towlower(static_cast<wint_t>(ch)));
        }
        return s;
    }

    static bool ExtractBodyInnerHtml_(const std::wstring& html, std::wstring& outBody)
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

        outBody.assign(html.begin() + static_cast<std::ptrdiff_t>(bodyOpenEnd + 1),
            html.begin() + static_cast<std::ptrdiff_t>(bodyClose));
        return true;
    }

    static std::wstring ExtractAttrValue_(const std::wstring& tag, const std::wstring& attrName)
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

    static std::wstring HtmlEscapeAttr_(std::wstring_view s)
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
                if (ch == L'\r' || ch == L'\n' || ch == L'\t') out.push_back(L' ');
                else out.push_back(ch);
                break;
            }
        }

        return out;
    }

    static std::wstring DeriveBeforePathFromCurrent_(const std::filesystem::path& currentPath)
    {
        const std::filesystem::path dir = currentPath.parent_path();
        const std::wstring stem = currentPath.stem().wstring();
        const std::wstring ext = currentPath.extension().wstring();
        return (dir / (stem + L"_before" + ext)).wstring();
    }

    static int GuessSpaceFromPath_(std::wstring_view s)
    {
        const std::wstring lower = ToLowerCopy_(std::wstring(s));
        const std::wstring needle = L"space";

        const std::size_t pos = lower.find(needle);
        if (pos == std::wstring::npos) {
            return 0;
        }

        std::size_t i = pos + needle.size();
        int value = 0;
        bool found = false;

        while (i < lower.size() && lower[i] >= L'0' && lower[i] <= L'9') {
            found = true;
            value = value * 10 + static_cast<int>(lower[i] - L'0');
            ++i;
        }

        return found ? value : 0;
    }

    static std::wstring MakeSpaceLabel_(int space)
    {
        switch (space) {
        case 0: return L"地上波";
        case 1: return L"BS";
        case 2: return L"CS";
        default: return L"Space " + std::to_wstring(space);
        }
    }

    static std::vector<EpgHtmlPortal_ProgramPageWriter::ChannelServiceInfo>
        LoadProgramPageServiceMapFromChList_(
            const std::wstring& chListPath,
            int targetSpace);

    static void RunReservationEpgRebuildActionAfterPortal_(
        const EpgHtmlPortal::WriteOptions& portalOpt,
        const std::filesystem::path& mergedPath,
        int space,
        const std::vector<std::vector<EpgHtmlPortal::ServiceEntry>>& enabledServicesBySpace);

    static void RunAutoReserveSearchAndRegisterAfterEpgUpdate_(
        const EpgHtmlPortal::WriteOptions& portalOpt,
        const Callbacks* cb,
        const wchar_t* reason) noexcept;

    static std::wstring EnrichLegacyProgramCellsForSwitches_(const std::wstring& bodyHtml)
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

            const std::size_t cellClose = bodyHtml.find(cellEnd, tagEnd + 1);
            if (cellClose == std::wstring::npos) {
                out.append(bodyHtml, tdPos, std::wstring::npos);
                break;
            }

            std::wstring openTag = bodyHtml.substr(tdPos, tagEnd - tdPos + 1);
            const std::wstring cellInner = bodyHtml.substr(tagEnd + 1, cellClose - (tagEnd + 1));

            if (openTag.find(L"data-epg-program=") == std::wstring::npos) {
                const std::wstring title = ExtractAttrValue_(openTag, L"title");
                const std::wstring eid = ExtractInnerByMarkers_(cellInner, L"<span class=\"eid\">", L"</span>");
                const std::wstring tm = ExtractInnerByMarkers_(cellInner, L"<div class=\"tm\">", L"</div>");
                const std::wstring summary = ExtractInnerByMarkers_(cellInner, L"<div class=\"s\">", L"</div>");

                // '>' の直前に属性を差し込む
                openTag.pop_back();
                openTag += L" data-epg-program=\"1\"";

                if (!title.empty()) {
                    openTag += L" data-title=\"" + HtmlEscapeAttr_(title) + L"\"";
                }
                if (!eid.empty()) {
                    openTag += L" data-event-id=\"" + HtmlEscapeAttr_(eid) + L"\"";
                }
                if (!tm.empty()) {
                    openTag += L" data-time=\"" + HtmlEscapeAttr_(tm) + L"\"";
                }
                if (!summary.empty()) {
                    openTag += L" data-summary=\"" + HtmlEscapeAttr_(summary) + L"\"";
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

    static bool TryGenerateWithProgramPageWriter_(
        const HtmlContext& ctx)
    {
        const auto diagStart =
            std::chrono::steady_clock::now();

        DebugLogFormat(
            L"[EPG][DIAG][HTML][PORTAL-BEGIN] route=%ls space=%d outputDir=%ls merged=%ls beforeMerged=%ls chlist=%ls",
            ToRouteName(ctx.renderRoute),
            ctx.space,
            ctx.outputDir.c_str(),
            ctx.mergedEpgPath.c_str(),
            ctx.beforeMergedEpgPath.c_str(),
            ctx.channelListPath.c_str());

        EpgHtmlPortal::GenerateFromEpgRequest req{};

        req.space = ctx.space;
        req.outputDir = std::filesystem::path(ctx.outputDir);
        req.mergedEpgPath = std::filesystem::path(ctx.mergedEpgPath);
        req.beforeMergedEpgPath = std::filesystem::path(ctx.beforeMergedEpgPath);
        req.channelListPath = std::filesystem::path(ctx.channelListPath);
        req.title = ctx.title;

        req.generateCurrentLegacy = true;
        req.generateCurrentTimetable = true;
        req.generateCurrentList = true;

        req.generateBeforeLegacy = true;
        req.generateBeforeTimetable = true;
        req.generateBeforeList = true;

        req.generatePastArchive = !req.beforeMergedEpgPath.empty();

        req.generateIndex = false;
        req.generateSearchData = false;
        req.generateReservationData = false;

        const auto result = EpgHtmlPortal::GenerateFromEpg(req);

        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - diagStart).count();

        DebugLogFormat(
            L"[EPG][DIAG][HTML][PORTAL-END] ok=%d elapsedMs=%lld generatedFiles=%zu index=%ls",
            result.ok ? 1 : 0,
            static_cast<long long>(elapsedMs),
            result.generatedFiles.size(),
            result.indexPath.wstring().c_str());

        return result.ok;
    }

    static std::wstring SanitizeForLog(std::wstring_view src)
    {
        std::wstring out;
        constexpr std::size_t kMaxLen = 512;
        out.reserve((std::min)(src.size(), kMaxLen));
        for (wchar_t ch : src) {
            if (out.size() >= kMaxLen) { out.push_back(L'…'); break; }
            if (ch < 0x20) {
                if (ch == L'\n' || ch == L'\r' || ch == L'\t') out.push_back(ch);
                else out.push_back(L'?');
                continue;
            }
            if (ch >= 0xD800 && ch <= 0xDFFF) { out.push_back(L'?'); continue; }
            out.push_back(ch);
        }
        return out;
    }

    static std::wstring ResolveChannelListPath(int space, std::wstring_view overridePath)
    {
        if (!overridePath.empty()) return std::wstring(overridePath);
        return L"channellist_space" + std::to_wstring(space) + L".ch";
    }

    static void TrimCR(std::string& s)
    {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    }

    static std::vector<std::string> ParseCsvLoose(std::string_view s)
    {
        std::vector<std::string> out;
        std::string cur;
        bool inQ = false;

        for (std::size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (inQ) {
                if (c == '"') {
                    if (i + 1 < s.size() && s[i + 1] == '"') {
                        cur.push_back('"');
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
                if (c == '"') {
                    inQ = true;
                }
                else if (c == ',') {
                    out.push_back(cur);
                    cur.clear();
                }
                else {
                    cur.push_back(c);
                }
            }
        }

        out.push_back(cur);
        return out;
    }

    static int ToIntLoose(const std::string& s, int def = 0)
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

    static std::wstring TrimAsciiWsW(std::wstring s)
    {
        const auto is_ws = [](wchar_t ch) noexcept {
            return ch == L' ' ||
                ch == L'\t' ||
                ch == L'\r' ||
                ch == L'\n' ||
                ch == L'\u3000';
            };

        while (!s.empty() && is_ws(s.front())) {
            s.erase(s.begin());
        }

        while (!s.empty() && is_ws(s.back())) {
            s.pop_back();
        }

        return s;
    }

    static std::vector<std::wstring> ParseCsvLooseW(std::wstring_view s)
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
                    out.push_back(TrimAsciiWsW(cur));
                    cur.clear();
                }
                else {
                    cur.push_back(c);
                }
            }
        }

        out.push_back(TrimAsciiWsW(cur));
        return out;
    }

    static int ToIntLooseW(const std::wstring& s, int def = 0)
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

    static std::wstring TryMakeSiblingChannelListPathForSpace_(
        std::wstring_view basePathText,
        int targetSpace)
    {
        if (basePathText.empty()) {
            return {};
        }

        const std::filesystem::path basePath(basePathText);
        std::wstring fileName = basePath.filename().wstring();
        if (fileName.empty()) {
            return {};
        }

        const std::wstring lower = ToLowerCopy_(fileName);
        const std::wstring marker = L"space";

        std::size_t markerPos = lower.find(marker);
        while (markerPos != std::wstring::npos) {
            const std::size_t digitBegin = markerPos + marker.size();

            if (digitBegin >= lower.size() ||
                lower[digitBegin] < L'0' ||
                lower[digitBegin] > L'9') {
                markerPos = lower.find(marker, markerPos + marker.size());
                continue;
            }

            std::size_t digitEnd = digitBegin;
            while (digitEnd < lower.size() &&
                lower[digitEnd] >= L'0' &&
                lower[digitEnd] <= L'9') {
                ++digitEnd;
            }

            fileName.replace(
                digitBegin,
                digitEnd - digitBegin,
                std::to_wstring(targetSpace));

            return (basePath.parent_path() / fileName).wstring();
        }

        return {};
    }

    static std::wstring ResolveChannelListPathForSearchServices_(
        int targetSpace,
        const EpgBuildGUI::Options& opt)
    {
        const std::wstring defaultPath =
            ResolveChannelListPath(targetSpace, std::wstring_view{});

        if (opt.channelListPathOverride.empty()) {
            return defaultPath;
        }

        // 現在処理中の space は、明示 override をそのまま優先する。
        if (targetSpace == opt.space) {
            return ResolveChannelListPath(targetSpace, opt.channelListPathOverride);
        }

        // override が channellist_space0.ch のような名前なら、
        // channellist_space1.ch / channellist_space2.ch を同じフォルダから推定する。
        const std::wstring siblingPath =
            TryMakeSiblingChannelListPathForSpace_(
                opt.channelListPathOverride,
                targetSpace);

        if (!siblingPath.empty() &&
            std::filesystem::exists(std::filesystem::path(siblingPath))) {
            return siblingPath;
        }

        // BS/CS が未受信環境でもここで安全に空扱いできる。
        return defaultPath;
    }

    static std::wstring Utf8ToWideLocal(std::string_view s)
    {
#ifdef _WIN32
        if (s.empty()) return {};
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (n <= 0) return {};
        std::wstring out(static_cast<std::size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
        return out;
#else
        return std::wstring(s.begin(), s.end());
#endif
    }

    static std::filesystem::path GetExeDirectoryPath()
    {
#ifdef _WIN32
        std::vector<wchar_t> buffer(32768, L'\0');
        const DWORD len = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0 || len >= buffer.size()) {
            return {};
        }
        return std::filesystem::path(buffer.data(), buffer.data() + len).parent_path();
#else
        return {};
#endif
    }

    static std::wstring ResolveConfigIniPath(const std::wstring& rawPath, std::wstring_view outputDir)
    {
        if (rawPath.empty()) return {};

        const std::filesystem::path raw(rawPath);
        if (raw.is_absolute()) {
            return std::filesystem::exists(raw) ? raw.wstring() : std::wstring{};
        }

        if (std::filesystem::exists(raw)) {
            return raw.wstring();
        }

        const std::filesystem::path fileName = raw.filename().empty()
            ? std::filesystem::path(L"config.ini")
            : raw.filename();

        const std::filesystem::path exeDir = GetExeDirectoryPath();
        if (!exeDir.empty()) {
            const std::filesystem::path sameDir = exeDir / fileName;
            if (std::filesystem::exists(sameDir)) {
                return sameDir.wstring();
            }

            const std::filesystem::path oneUp = exeDir.parent_path() / fileName;
            if (std::filesystem::exists(oneUp)) {
                return oneUp.wstring();
            }
        }

        if (!outputDir.empty()) {
            const std::filesystem::path outParent = std::filesystem::path(outputDir).parent_path() / fileName;
            if (std::filesystem::exists(outParent)) {
                return outParent.wstring();
            }
        }

        return {};
    }

    static std::filesystem::path MakeAbsolutePathSafe_(
        const std::filesystem::path& path)
    {
        if (path.empty()) {
            return {};
        }

        std::error_code ec;
        const std::filesystem::path abs = std::filesystem::absolute(path, ec);
        if (!ec && !abs.empty()) {
            return abs;
        }

        return path;
    }

    static std::filesystem::path MakeOutputPathAbsolute_(
        const std::wstring& outputDir,
        const std::wstring& fileName)
    {
        if (fileName.empty()) {
            return {};
        }

        const std::filesystem::path p(fileName);
        if (p.is_absolute()) {
            return MakeAbsolutePathSafe_(p);
        }

        return MakeAbsolutePathSafe_(
            std::filesystem::path(outputDir) / p);
    }

    static EpgHtmlPortal::WriteOptions MakeAutoReserveWriteOptionsAfterEpgBuild_(
        const Options& o,
        int space,
        const std::filesystem::path& mergedPath,
        const std::wstring& currentHtmlPathText,
        const std::vector<std::vector<EpgHtmlPortal::ServiceEntry>>& enabledServicesBySpace)
    {
        EpgHtmlPortal::WriteOptions portalOpt{};

        portalOpt.space = space;

        portalOpt.outputDir =
            MakeAbsolutePathSafe_(
                std::filesystem::path(o.outputDir));

        portalOpt.indexFileName =
            o.topIndexFileName.empty()
            ? std::filesystem::path(L"index.html")
            : std::filesystem::path(o.topIndexFileName);

        if (!currentHtmlPathText.empty()) {
            portalOpt.currentHtmlPath =
                MakeAbsolutePathSafe_(
                    std::filesystem::path(currentHtmlPathText));
        }
        else {
            portalOpt.currentHtmlPath =
                MakeOutputPathAbsolute_(
                    o.outputDir,
                    o.htmlFileName);
        }

        portalOpt.beforeHtmlPath =
            MakeOutputPathAbsolute_(
                o.outputDir,
                o.beforeHtmlFileName);

        portalOpt.portalTitle =
            o.htmlTitle.empty()
            ? std::wstring(L"EPGポータル")
            : (o.htmlTitle + L" ポータル");

        portalOpt.generateBeforeStubIfMissing =
            o.generateBeforeHtmlStubIfMissing;

        portalOpt.generateSpaceStubIfMissing = true;

        portalOpt.enabledServicesBySpace =
            enabledServicesBySpace;

        portalOpt.mergedEpgPathsBySpace.assign(
            3,
            std::filesystem::path{});

        if (space >= 0 && space <= 2) {
            portalOpt.mergedEpgPathsBySpace[static_cast<std::size_t>(space)] =
                MakeAbsolutePathSafe_(mergedPath);
        }

        return portalOpt;
    }

    static void RefreshAutoReserveRulesCacheAfterEpgBuild_() noexcept
    {
        try {
            const AutoReserve::CacheResult cache =
                AutoReserve::RefreshRulesCache(
                    AutoReserve::RulesCacheRefreshMode::ForceRebuild);

            DebugLogFormat(
                L"[AUTO-RESERVE][EPG-BUILD-FINAL][RULE-CACHE] ok=%d updated=%d rules=%zu path=%ls message=%ls",
                cache.ok ? 1 : 0,
                cache.updated ? 1 : 0,
                cache.ruleCount,
                cache.cachePath.c_str(),
                cache.message.c_str());
        }
        catch (const std::bad_alloc&) {
            DebugLog(
                L"[AUTO-RESERVE][EPG-BUILD-FINAL][RULE-CACHE][ERROR] bad_alloc");
        }
        catch (const std::exception& e) {
            DebugLogFormat(
                L"[AUTO-RESERVE][EPG-BUILD-FINAL][RULE-CACHE][ERROR] exception what=%S",
                e.what());
        }
        catch (...) {
            DebugLog(
                L"[AUTO-RESERVE][EPG-BUILD-FINAL][RULE-CACHE][ERROR] unknown exception");
        }
    }

    static void RunAutoReserveRegisterAfterEpgBuildSuccess_(
        const Options& o,
        IBonDriver2* bon,
        const std::chrono::steady_clock::time_point& epgDiagStart,
        const std::filesystem::path& mergedPath,
        const std::wstring& currentHtmlPathText,
        int space,
        const std::vector<std::vector<EpgHtmlPortal::ServiceEntry>>& enabledServicesBySpace,
        const Callbacks* cb) noexcept
    {
        try {
            const EpgHtmlPortal::WriteOptions portalOpt =
                MakeAutoReserveWriteOptionsAfterEpgBuild_(
                    o,
                    space,
                    mergedPath,
                    currentHtmlPathText,
                    enabledServicesBySpace);

            DebugLogFormat(
                L"[AUTO-RESERVE][EPG-BUILD-FINAL] trigger begin route=%ls space=%d outputDir=%ls merged=%ls current=%ls",
                ToRouteName(o.htmlRenderRoute),
                space,
                portalOpt.outputDir.wstring().c_str(),
                mergedPath.wstring().c_str(),
                portalOpt.currentHtmlPath.wstring().c_str());

            EpgDiagLogPhase_(
                L"auto-reserve-register-begin",
                o,
                bon,
                epgDiagStart);

            RunAutoReserveSearchAndRegisterAfterEpgUpdate_(
                portalOpt,
                cb,
                L"epg-build-final");

            EpgDiagLogPhase_(
                L"auto-reserve-rule-cache-begin",
                o,
                bon,
                epgDiagStart);

            RefreshAutoReserveRulesCacheAfterEpgBuild_();

            EpgDiagLogPhase_(
                L"auto-reserve-rule-cache-end",
                o,
                bon,
                epgDiagStart);

            EpgDiagLogPhase_(
                L"auto-reserve-register-end",
                o,
                bon,
                epgDiagStart);

            DebugLogFormat(
                L"[AUTO-RESERVE][EPG-BUILD-FINAL] trigger end route=%ls space=%d",
                ToRouteName(o.htmlRenderRoute),
                space);
        }
        catch (const std::bad_alloc&) {
            DebugLog(L"[AUTO-RESERVE][EPG-BUILD-FINAL][ERROR] bad_alloc");
        }
        catch (const std::exception& e) {
            DebugLogFormat(
                L"[AUTO-RESERVE][EPG-BUILD-FINAL][ERROR] exception what=%S",
                e.what());
        }
        catch (...) {
            DebugLog(L"[AUTO-RESERVE][EPG-BUILD-FINAL][ERROR] unknown exception");
        }
    }

    static std::wstring RsvEpgFormatIsoDate_(int y, int m, int d)
    {
        wchar_t buf[32]{};
        swprintf_s(buf, L"%04d-%02d-%02d", y, m, d);
        return std::wstring(buf);
    }

    static std::wstring RsvEpgFormatIsoTime_(int h, int m, int s)
    {
        wchar_t buf[32]{};
        swprintf_s(buf, L"%02d:%02d:%02d", h, m, s);
        return std::wstring(buf);
    }

    static std::wstring RsvEpgGetWeekdayText_(int y, int m, int d)
    {
        std::tm tmv{};
        tmv.tm_year = y - 1900;
        tmv.tm_mon = m - 1;
        tmv.tm_mday = d;
        tmv.tm_hour = 12;
        tmv.tm_isdst = -1;

        std::mktime(&tmv);

        static constexpr const wchar_t* kWeek[] = {
            L"日", L"月", L"火", L"水", L"木", L"金", L"土"
        };

        if (tmv.tm_wday < 0 || tmv.tm_wday >= 7) {
            return L"--";
        }

        return kWeek[tmv.tm_wday];
    }

    static bool RsvEpgDecodeMjdToDate_(int mjd, int& y, int& m, int& d)
    {
        long j = static_cast<long>(mjd) + 2400001L + 68569L;
        long c = 4L * j / 146097L;
        j = j - (146097L * c + 3L) / 4L;
        long yy = 4000L * (j + 1L) / 1461001L;
        j = j - 1461L * yy / 4L + 31L;
        long mm = 80L * j / 2447L;
        long dd = j - 2447L * mm / 80L;
        j = mm / 11L;
        mm = mm + 2L - 12L * j;
        yy = 100L * (c - 49L) + yy + j;

        y = static_cast<int>(yy);
        m = static_cast<int>(mm);
        d = static_cast<int>(dd);
        return true;
    }

    static int RsvEpgDecodeBcdByte_(std::uint8_t v) noexcept
    {
        return static_cast<int>(((v >> 4) & 0x0F) * 10 + (v & 0x0F));
    }

    static bool RsvEpgParseStartHexLine_(
        std::wstring_view line,
        std::wstring& outDate,
        std::wstring& outWeekday,
        std::wstring& outTime)
    {
        const std::size_t hexPos = line.find(L"0x");
        if (hexPos == std::wstring_view::npos) {
            return false;
        }

        std::wstring hex;
        hex.reserve(16);

        for (std::size_t i = hexPos + 2; i < line.size(); ++i) {
            const wchar_t c = line[i];

            const bool isHex =
                (c >= L'0' && c <= L'9') ||
                (c >= L'A' && c <= L'F') ||
                (c >= L'a' && c <= L'f');

            if (!isHex) {
                break;
            }

            hex.push_back(c);

            if (hex.size() >= 16) {
                break;
            }
        }

        if (hex.size() < 10) {
            return false;
        }

        std::uint64_t raw = 0;

        try {
            raw = static_cast<std::uint64_t>(std::stoull(hex, nullptr, 16));
        }
        catch (...) {
            return false;
        }

        const int mjd = static_cast<int>((raw >> 24) & 0xFFFFULL);

        int y = 0;
        int mo = 0;
        int d = 0;

        if (!RsvEpgDecodeMjdToDate_(mjd, y, mo, d)) {
            return false;
        }

        const int hh = RsvEpgDecodeBcdByte_(static_cast<std::uint8_t>((raw >> 16) & 0xFFULL));
        const int mm = RsvEpgDecodeBcdByte_(static_cast<std::uint8_t>((raw >> 8) & 0xFFULL));
        const int ss = RsvEpgDecodeBcdByte_(static_cast<std::uint8_t>(raw & 0xFFULL));

        outDate = RsvEpgFormatIsoDate_(y, mo, d);
        outWeekday = RsvEpgGetWeekdayText_(y, mo, d);
        outTime = RsvEpgFormatIsoTime_(hh, mm, ss);

        return true;
    }

    static bool RsvEpgParseDurationLine_(
        std::wstring_view line,
        std::uint32_t& outDurationSec)
    {
        const std::size_t pos = line.find(L':');
        if (pos == std::wstring_view::npos) {
            return false;
        }

        const std::wstring s =
            EpgHtmlPortal_Common::TrimW(std::wstring(line.substr(pos + 1)));

        int h = 0;
        int m = 0;
        int sec = 0;

        if (swscanf_s(s.c_str(), L"%d:%d:%d", &h, &m, &sec) != 3) {
            return false;
        }

        if (h < 0 || m < 0 || sec < 0) {
            return false;
        }

        outDurationSec =
            static_cast<std::uint32_t>(h * 3600 + m * 60 + sec);

        return outDurationSec > 0;
    }

    static bool RsvEpgAddSecondsToIsoDateTime_(
        const std::wstring& startDate,
        const std::wstring& startTime,
        std::uint32_t durationSec,
        std::wstring& outDate,
        std::wstring& outTime)
    {
        if (startDate.size() < 10 || startTime.size() < 5 || durationSec == 0) {
            return false;
        }

        int y = 0;
        int mo = 0;
        int d = 0;
        int h = 0;
        int mi = 0;
        int s = 0;

        if (swscanf_s(startDate.c_str(), L"%d-%d-%d", &y, &mo, &d) != 3) {
            return false;
        }

        if (swscanf_s(startTime.c_str(), L"%d:%d:%d", &h, &mi, &s) < 2) {
            return false;
        }

        std::tm tmv{};
        tmv.tm_year = y - 1900;
        tmv.tm_mon = mo - 1;
        tmv.tm_mday = d;
        tmv.tm_hour = h;
        tmv.tm_min = mi;
        tmv.tm_sec = s;
        tmv.tm_isdst = -1;

        std::time_t t = std::mktime(&tmv);
        if (t == static_cast<std::time_t>(-1)) {
            return false;
        }

        t += static_cast<std::time_t>(durationSec);

#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        tmv = *std::localtime(&t);
#endif

        outDate = RsvEpgFormatIsoDate_(
            tmv.tm_year + 1900,
            tmv.tm_mon + 1,
            tmv.tm_mday);

        outTime = RsvEpgFormatIsoTime_(
            tmv.tm_hour,
            tmv.tm_min,
            tmv.tm_sec);

        return true;
    }

    static bool RsvEpgIsNameChar_(wchar_t ch) noexcept
    {
        return
            (ch >= L'0' && ch <= L'9') ||
            (ch >= L'A' && ch <= L'Z') ||
            (ch >= L'a' && ch <= L'z') ||
            ch == L'_';
    }

    static int RsvEpgHexValue_(wchar_t ch) noexcept
    {
        if (ch >= L'0' && ch <= L'9') {
            return static_cast<int>(ch - L'0');
        }
        if (ch >= L'A' && ch <= L'F') {
            return 10 + static_cast<int>(ch - L'A');
        }
        if (ch >= L'a' && ch <= L'f') {
            return 10 + static_cast<int>(ch - L'a');
        }
        return -1;
    }

    static int RsvEpgFindNumberAfterKey_(
        std::wstring_view line,
        std::wstring_view key)
    {
        if (line.empty() || key.empty()) {
            return 0;
        }

        std::size_t searchPos = 0;

        while (searchPos < line.size()) {
            const std::size_t pos = line.find(key, searchPos);
            if (pos == std::wstring_view::npos) {
                return 0;
            }

            const bool leftOk =
                pos == 0 ||
                !RsvEpgIsNameChar_(line[pos - 1]);

            std::size_t cur = pos + key.size();

            const bool rightOk =
                cur >= line.size() ||
                !RsvEpgIsNameChar_(line[cur]);

            if (!leftOk || !rightOk) {
                searchPos = pos + 1;
                continue;
            }

            while (cur < line.size() &&
                (line[cur] == L' ' ||
                    line[cur] == L'\t' ||
                    line[cur] == L'=' ||
                    line[cur] == L':' ||
                    line[cur] == L'(' ||
                    line[cur] == L')')) {
                ++cur;
            }

            if (cur >= line.size()) {
                return 0;
            }

            bool isHex = false;

            if ((cur + 1) < line.size() &&
                line[cur] == L'0' &&
                (line[cur + 1] == L'x' || line[cur + 1] == L'X')) {
                isHex = true;
                cur += 2;
            }

            std::size_t end = cur;

            if (isHex) {
                while (end < line.size() && RsvEpgHexValue_(line[end]) >= 0) {
                    ++end;
                }
            }
            else {
                while (end < line.size() &&
                    line[end] >= L'0' &&
                    line[end] <= L'9') {
                    ++end;
                }
            }

            if (end <= cur) {
                return 0;
            }

            try {
                const std::wstring numberText(line.substr(cur, end - cur));

                if (isHex) {
                    return std::stoi(numberText, nullptr, 16);
                }

                return std::stoi(numberText, nullptr, 10);
            }
            catch (...) {
                return 0;
            }
        }

        return 0;
    }

    static int RsvEpgFindNumberAny_(
        std::wstring_view line,
        std::initializer_list<std::wstring_view> keys)
    {
        for (const auto key : keys) {
            const int v = RsvEpgFindNumberAfterKey_(line, key);
            if (v > 0) {
                return v;
            }
        }

        return 0;
    }

    static std::wstring RsvEpgGenreNameFromPrimary_(int primary)
    {
        switch (primary & 0x0F) {
        case 0x0: return L"ニュース・報道";
        case 0x1: return L"スポーツ";
        case 0x2: return L"情報・ワイドショー";
        case 0x3: return L"ドラマ";
        case 0x4: return L"音楽";
        case 0x5: return L"バラエティ";
        case 0x6: return L"映画";
        case 0x7: return L"アニメ・特撮";
        case 0x8: return L"ドキュメンタリー";
        case 0x9: return L"劇場・公演";
        case 0xA: return L"趣味・教育";
        case 0xB: return L"福祉";
        default:  return L"--";
        }
    }

    static std::wstring RsvEpgExtractAfterPrefix_(
        std::wstring_view line,
        std::wstring_view prefix)
    {
        if (line.rfind(prefix, 0) != 0) {
            return {};
        }

        return EpgHtmlPortal_Common::TrimW(
            std::wstring(line.substr(prefix.size())));
    }

    static std::unordered_map<std::uint16_t, std::wstring>
        RsvEpgBuildServiceNameMap_(
            const std::vector<std::vector<EpgHtmlPortal::ServiceEntry>>& bySpace,
            int space)
    {
        std::unordered_map<std::uint16_t, std::wstring> out;

        if (space < 0 ||
            static_cast<std::size_t>(space) >= bySpace.size()) {
            return out;
        }

        for (const auto& svc : bySpace[static_cast<std::size_t>(space)]) {
            if (svc.serviceId != 0 && !svc.serviceName.empty()) {
                out.emplace(svc.serviceId, svc.serviceName);
            }
        }

        return out;
    }

    static std::vector<ReservationEpgRebuildAction::LatestProgramInfo>
        BuildLatestProgramsForReservationRebuild_(
            const std::filesystem::path& mergedEpgPath,
            int space,
            const std::vector<std::vector<EpgHtmlPortal::ServiceEntry>>& enabledServicesBySpace)
    {
        using LatestProgramInfo =
            ReservationEpgRebuildAction::LatestProgramInfo;

        std::vector<LatestProgramInfo> out;

        if (mergedEpgPath.empty()) {
            return out;
        }

        std::wstring text;
        if (!EpgHtmlPortal_Common::ReadTextFileToWide(mergedEpgPath, text)) {
            DebugLogFormat(
                L"[EPG-REBUILD][RESERVATION][WARN] merged epg read failed: %ls",
                mergedEpgPath.wstring().c_str());
            return out;
        }

        const auto serviceNameMap =
            RsvEpgBuildServiceNameMap_(enabledServicesBySpace, space);

        LatestProgramInfo cur{};
        bool hasEvent = false;

        auto flush = [&]() {
            if (!hasEvent) {
                return;
            }

            if (cur.serviceId != 0 && cur.serviceName.empty()) {
                const auto it = serviceNameMap.find(cur.serviceId);
                if (it != serviceNameMap.end()) {
                    cur.serviceName = it->second;
                }
            }

            if (cur.titleLabel.empty()) {
                cur.titleLabel = cur.title;
            }

            if (cur.durationSec > 0 &&
                !cur.startDate.empty() &&
                !cur.startTime.empty()) {
                std::wstring endDate;
                std::wstring endTime;

                if (RsvEpgAddSecondsToIsoDateTime_(
                    cur.startDate,
                    cur.startTime,
                    cur.durationSec,
                    endDate,
                    endTime)) {
                    if (cur.endDate.empty()) {
                        cur.endDate = endDate;
                    }
                    if (cur.endTime.empty()) {
                        cur.endTime = endTime;
                    }
                }
            }

            if (cur.timeText.empty() && !cur.startTime.empty()) {
                const std::wstring shhmm =
                    cur.startTime.size() >= 5
                    ? cur.startTime.substr(0, 5)
                    : cur.startTime;

                const std::wstring ehhmm =
                    cur.endTime.size() >= 5
                    ? cur.endTime.substr(0, 5)
                    : shhmm;

                cur.timeText = shhmm + L"-" + ehhmm;
            }

            if (cur.eventId != 0 || !cur.title.empty()) {
                out.push_back(std::move(cur));
            }

            cur = LatestProgramInfo{};
            hasEvent = false;
            };

        std::wstringstream ss(text);
        std::wstring line;

        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == L'\r') {
                line.pop_back();
            }

            const std::wstring trimmed =
                EpgHtmlPortal_Common::TrimW(line);

            if (trimmed.empty()) {
                flush();
                continue;
            }

            if (trimmed.rfind(L"TableID:", 0) == 0) {
                flush();

                hasEvent = true;
                cur = LatestProgramInfo{};
                cur.space = space;

                cur.networkId = static_cast<std::uint16_t>(
                    RsvEpgFindNumberAny_(
                        trimmed,
                        { L"OriginalNetworkID", L"NetworkID", L"ONID" }));

                cur.tsId = static_cast<std::uint16_t>(
                    RsvEpgFindNumberAny_(
                        trimmed,
                        { L"TransportStreamID", L"TSID", L"TsId" }));

                cur.serviceId = static_cast<std::uint16_t>(
                    RsvEpgFindNumberAny_(
                        trimmed,
                        { L"ServiceID", L"ServiceId", L"SID" }));

                cur.eventId = static_cast<std::uint16_t>(
                    RsvEpgFindNumberAny_(
                        trimmed,
                        { L"EventID", L"EventId", L"EID" }));

                continue;
            }

            if (!hasEvent) {
                continue;
            }

            const int networkId = RsvEpgFindNumberAny_(
                trimmed,
                { L"OriginalNetworkID", L"NetworkID", L"ONID" });

            if (networkId > 0) {
                cur.networkId = static_cast<std::uint16_t>(networkId);
            }

            const int tsId = RsvEpgFindNumberAny_(
                trimmed,
                { L"TransportStreamID", L"TSID", L"TsId" });

            if (tsId > 0) {
                cur.tsId = static_cast<std::uint16_t>(tsId);
            }

            const int serviceId = RsvEpgFindNumberAny_(
                trimmed,
                { L"ServiceID", L"ServiceId", L"SID" });

            if (serviceId > 0) {
                cur.serviceId = static_cast<std::uint16_t>(serviceId);
            }

            const int eventId = RsvEpgFindNumberAny_(
                trimmed,
                { L"EventID", L"EventId", L"EID" });

            if (eventId > 0) {
                cur.eventId = static_cast<std::uint16_t>(eventId);
            }

            if (trimmed.rfind(L"StartTime(raw MJD+BCD40bit):", 0) == 0) {
                std::wstring startDate;
                std::wstring weekday;
                std::wstring startTime;

                if (RsvEpgParseStartHexLine_(
                    trimmed,
                    startDate,
                    weekday,
                    startTime)) {
                    cur.startDate = startDate;
                    cur.dateText = startDate;
                    cur.weekdayText = weekday;
                    cur.startTime = startTime;
                }

                continue;
            }

            if (trimmed.rfind(L"Duration(HH:MM:SS):", 0) == 0) {
                std::uint32_t durationSec = 0;

                if (RsvEpgParseDurationLine_(trimmed, durationSec)) {
                    cur.durationSec = durationSec;
                }

                continue;
            }

            if (trimmed.rfind(L"[短形式タイトル]", 0) == 0) {
                cur.title =
                    RsvEpgExtractAfterPrefix_(trimmed, L"[短形式タイトル]");
                cur.titleLabel = cur.title;
                continue;
            }

            if (trimmed.rfind(L"[短形式概要]", 0) == 0) {
                cur.summary =
                    RsvEpgExtractAfterPrefix_(trimmed, L"[短形式概要]");
                continue;
            }

            if (trimmed.rfind(L"[ジャンル]", 0) == 0) {
                const std::size_t pos = trimmed.find(L"primary=0x");
                if (pos != std::wstring::npos &&
                    pos + 10 < trimmed.size()) {
                    const wchar_t c = trimmed[pos + 10];
                    int primary = 0;

                    if (c >= L'0' && c <= L'9') {
                        primary = c - L'0';
                    }
                    else if (c >= L'A' && c <= L'F') {
                        primary = 10 + (c - L'A');
                    }
                    else if (c >= L'a' && c <= L'f') {
                        primary = 10 + (c - L'a');
                    }

                    cur.genre = RsvEpgGenreNameFromPrimary_(primary);
                }

                continue;
            }

            if (trimmed.rfind(L"(", 0) == 0) {
                const std::size_t colon = trimmed.find(L':');
                if (colon != std::wstring::npos) {
                    const std::wstring ext =
                        EpgHtmlPortal_Common::TrimW(
                            std::wstring(trimmed.substr(colon + 1)));

                    if (!ext.empty()) {
                        if (cur.summary.empty()) {
                            cur.summary = ext;
                        }
                        else if (cur.summary.find(ext) == std::wstring::npos) {
                            cur.summary += L" / ";
                            cur.summary += ext;
                        }
                    }
                }

                continue;
            }
        }

        flush();

        DebugLogFormat(
            L"[EPG-REBUILD][RESERVATION] latest programs parsed: space=%d count=%zu path=%ls",
            space,
            out.size(),
            mergedEpgPath.wstring().c_str());

        return out;
    }

    static std::vector<int> LoadEnabledPhysicalChannelsFromConfigIni(const std::wstring& iniPath, int space)
    {
        std::ifstream ifs(std::filesystem::path(iniPath), std::ios::binary);
        if (!ifs) return {};

        const std::string targetSec = "[EpgServiceEnable_space" + std::to_string(space) + "]";
        bool inTarget = false;
        std::unordered_set<int> uniq;

        std::string line;
        while (std::getline(ifs, line)) {
            TrimCR(line);
            if (line.empty()) continue;
            if (line[0] == ';' || line[0] == '#') continue;

            if (!line.empty() && line[0] == '[') {
                inTarget = (line == targetSec);
                continue;
            }
            if (!inTarget) continue;

            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string_view rhs(line.c_str() + eq + 1, line.size() - (eq + 1));
            auto cols = ParseCsvLoose(rhs);
            if (cols.size() < 4) continue;

            const int enable = ToIntLoose(cols[0], 0);
            if (enable == 0) continue;

            const int sp = ToIntLoose(cols[2], -1);
            const int ch = ToIntLoose(cols[3], -1);
            if (sp != space || ch < 0) continue;

            uniq.insert(ch);
        }

        std::vector<int> out;
        out.reserve(uniq.size());
        for (int ch : uniq) out.push_back(ch);
        std::sort(out.begin(), out.end());
        return out;
    }


    static int PickServiceIdFromConfigColumns_(
        const std::vector<std::wstring>& cols) noexcept
    {
        const auto tryAt = [&](std::size_t index) noexcept -> int {
            if (index >= cols.size()) {
                return -1;
            }

            const int v = ToIntLooseW(cols[index], -1);
            if (v < 0 || v > 0xFFFF) {
                return -1;
            }

            return v;
            };

        // 既存実装互換: cols[5]
        if (const int v = tryAt(5); v >= 0) {
            return v;
        }

        // config 書式差分対策: serviceId が cols[4] にある場合
        if (const int v = tryAt(4); v >= 0) {
            return v;
        }

        // 念のため拡張書式対策
        if (const int v = tryAt(6); v >= 0) {
            return v;
        }

        return 0;
    }

    static std::vector<EpgHtmlPortal::ServiceEntry>
        LoadEnabledServiceEntriesFromConfigIni(
            const std::wstring& iniPath,
            int space)
    {
#ifdef _WIN32
        if (iniPath.empty()) {
            return {};
        }

        const std::wstring section =
            L"EpgServiceEnable_space" + std::to_wstring(space);

        std::vector<wchar_t> buffer(256 * 1024, L'\0');
        const DWORD len = ::GetPrivateProfileSectionW(
            section.c_str(),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            iniPath.c_str());

        if (len == 0) {
            DebugLogFormat(
                L"[EPG][SEARCH-SERVICE] config section empty: [%ls]",
                section.c_str());
            return {};
        }

        std::vector<EpgHtmlPortal::ServiceEntry> out;
        std::unordered_set<std::uint32_t> seenSid;
        std::unordered_set<std::wstring> seenName;

        const wchar_t* p = buffer.data();
        while (*p != L'\0') {
            std::wstring line(p);
            p += line.size() + 1;

            const auto eq = line.find(L'=');
            if (eq == std::wstring::npos) {
                continue;
            }

            const std::wstring rhs = line.substr(eq + 1);
            const auto cols = ParseCsvLooseW(rhs);

            // 最低限: enable, serviceName があれば候補として扱える。
            if (cols.size() < 2) {
                continue;
            }

            const int enable = ToIntLooseW(cols[0], 0);
            if (enable == 0) {
                continue;
            }

            const std::wstring name = TrimAsciiWsW(cols[1]);
            if (name.empty()) {
                continue;
            }

            // cols[2] に space がある書式ならチェック。
            // 無い場合は、この section の space として扱う。
            const int rowSpace =
                (cols.size() >= 3)
                ? ToIntLooseW(cols[2], space)
                : space;

            if (rowSpace != space) {
                continue;
            }

            const int sid = PickServiceIdFromConfigColumns_(cols);

            if (sid > 0) {
                const std::uint32_t sidKey =
                    static_cast<std::uint32_t>(sid & 0xFFFF);

                if (!seenSid.insert(sidKey).second) {
                    continue;
                }
            }

            // serviceId が取れない場合でも、検索候補として serviceName は使う。
            // SID がある場合も、同名重複はここで抑える。
            if (!seenName.insert(name).second) {
                continue;
            }

            EpgHtmlPortal::ServiceEntry e{};
            e.serviceId = static_cast<std::uint16_t>(sid > 0 ? (sid & 0xFFFF) : 0);
            e.serviceName = name;
            out.push_back(std::move(e));
        }

        std::sort(
            out.begin(),
            out.end(),
            [](const auto& a, const auto& b) {
                if (a.serviceName != b.serviceName) {
                    return a.serviceName < b.serviceName;
                }
                return a.serviceId < b.serviceId;
            });

        DebugLogFormat(
            L"[EPG][SEARCH-SERVICE] config.ini services loaded. space=%d count=%zu",
            space,
            out.size());

        return out;
#else
        (void)iniPath;
        (void)space;
        return {};
#endif
    }

    static std::vector<EpgHtmlPortal::ServiceEntry>
        LoadServiceEntriesFromChList_Strict(
            const std::wstring& chListPath,
            int space)
    {
        if (chListPath.empty()) {
            DebugLogFormat(
                L"[EPG][SEARCH-SERVICE][WARN] channellist path empty. space=%d",
                space);
            return {};
        }

        if (!std::filesystem::exists(std::filesystem::path(chListPath))) {
            DebugLogFormat(
                L"[EPG][SEARCH-SERVICE][WARN] channellist missing for service list. space=%d path=%ls",
                space,
                chListPath.c_str());
            return {};
        }

        ChannelListLoader loader;
        if (!loader.LoadFromChannelListCh(chListPath)) {
            DebugLogFormat(
                L"[EPG][SEARCH-SERVICE][WARN] channellist load failed for service list. space=%d path=%ls",
                space,
                chListPath.c_str());
            return {};
        }

        const auto& list = loader.GetChannelList();
        if (list.empty()) {
            DebugLogFormat(
                L"[EPG][SEARCH-SERVICE][WARN] channellist empty for service list. space=%d path=%ls",
                space,
                chListPath.c_str());
            return {};
        }

        std::vector<EpgHtmlPortal::ServiceEntry> out;
        std::unordered_set<std::uint32_t> seenSid;
        std::unordered_set<std::wstring> seenName;

        bool hasSpace = false;

        for (const auto& ch : list) {
            if (ch.space != space) {
                continue;
            }

            hasSpace = true;

            const std::uint32_t sid =
                static_cast<std::uint32_t>(ch.serviceId & 0xFFFF);

            const std::wstring name = TrimAsciiWsW(ch.serviceName);
            if (name.empty()) {
                continue;
            }

            if (sid > 0) {
                if (!seenSid.insert(sid).second) {
                    continue;
                }
            }

            if (!seenName.insert(name).second) {
                continue;
            }

            EpgHtmlPortal::ServiceEntry e{};
            e.serviceId = static_cast<std::uint16_t>(sid & 0xFFFF);
            e.serviceName = name;
            out.push_back(std::move(e));
        }

        if (!hasSpace) {
            DebugLogFormat(
                L"[EPG][SEARCH-SERVICE][WARN] no entries for target space=%d in channellist: %ls",
                space,
                chListPath.c_str());
            return {};
        }

        std::sort(
            out.begin(),
            out.end(),
            [](const auto& a, const auto& b) {
                if (a.serviceName != b.serviceName) {
                    return a.serviceName < b.serviceName;
                }
                return a.serviceId < b.serviceId;
            });

        DebugLogFormat(
            L"[EPG][SEARCH-SERVICE] channellist services loaded. space=%d count=%zu path=%ls",
            space,
            out.size(),
            chListPath.c_str());

        return out;
    }



    static bool IsReasonableServiceId_(int v) noexcept
    {
        return v > 0 && v <= 0xFFFF;
    }

    static bool IsReasonableTransportId_(int v) noexcept
    {
        return v > 0 && v <= 0xFFFF;
    }

    static bool FillProgramPageServiceInfo_(
        const std::wstring& serviceName,
        int space,
        int physicalChannel,
        const std::wstring& remoteControlKeyId,
        int networkId,
        int tsId,
        int serviceId,
        EpgHtmlPortal_ProgramPageWriter::ChannelServiceInfo& out)
    {
        if (serviceName.empty()) {
            return false;
        }

        if (!IsReasonableServiceId_(serviceId) ||
            !IsReasonableTransportId_(networkId) ||
            !IsReasonableTransportId_(tsId)) {
            return false;
        }

        out = {};
        out.space = space;
        out.physicalChannel = physicalChannel;
        out.networkId = networkId;
        out.tsId = tsId;
        out.serviceId = serviceId;
        out.serviceKey =
            EpgHtmlPortal_ProgramPageWriter::MakeServiceKey(
                space,
                networkId,
                tsId,
                serviceId);
        out.serviceName = serviceName;
        out.channelName = serviceName;
        out.remoteControlKeyId = remoteControlKeyId;

        return !out.serviceKey.empty();
    }

    static bool ParseProgramPageServiceInfoFromChListCols_(
        const std::vector<std::wstring>& cols,
        int targetSpace,
        EpgHtmlPortal_ProgramPageWriter::ChannelServiceInfo& out)
    {
        out = {};

        if (cols.size() < 8) {
            return false;
        }

        const std::wstring serviceName = TrimAsciiWsW(cols[0]);
        if (serviceName.empty()) {
            return false;
        }

        const int space = ToIntLooseW(cols[1], -1);
        if (space != targetSpace) {
            return false;
        }

        const int physicalChannel = ToIntLooseW(cols[2], -1);

        const std::wstring remoteControlKeyId =
            (cols.size() >= 12)
            ? TrimAsciiWsW(cols[11])
            : (cols.size() > 3 ? TrimAsciiWsW(cols[3]) : std::wstring{});

        struct Candidate
        {
            int serviceId = 0;
            int networkId = 0;
            int tsId = 0;
            int enabled = 1;
            const wchar_t* tag = L"";
        };

        std::vector<Candidate> candidates;
        candidates.reserve(5);
        // channel_scan2 channellist_spaceX.ch 形式:
// 0: ServiceName
// 1: Space
// 2: Channel
// 3: ServiceType
// 4: ServiceID
// 5: NetworkID
// 6: TSID
// 7: ONID
// 8: FreeCA
// 9: Provider
// 10: NetworkName
// 11: RemoteControlKeyID
        if (cols.size() >= 12) {
            candidates.push_back(Candidate{
                ToIntLooseW(cols[4], 0),
                ToIntLooseW(cols[5], 0),
                ToIntLooseW(cols[6], 0),
                1,
                L"channel_scan2-12col-4-5-6"
                });
        }
        // 9列想定:
        // name, space, physicalChannel, remoteKey, serviceType, serviceId, networkId, tsId, enabled
        if (cols.size() >= 9) {
            candidates.push_back(Candidate{
                ToIntLooseW(cols[5], 0),
                ToIntLooseW(cols[6], 0),
                ToIntLooseW(cols[7], 0),
                ToIntLooseW(cols[8], 1),
                L"9col-5-6-7"
                });
        }

        // 8列想定:
        // name, space, physicalChannel, remoteKey, serviceId, networkId, tsId, enabled
        if (cols.size() >= 8) {
            candidates.push_back(Candidate{
                ToIntLooseW(cols[4], 0),
                ToIntLooseW(cols[5], 0),
                ToIntLooseW(cols[6], 0),
                ToIntLooseW(cols[7], 1),
                L"8col-4-5-6"
                });
        }

        // 旧実装互換。
        // 以前 epg_service_map.js が空ではなかった時の列読み。
        if (cols.size() >= 8) {
            candidates.push_back(Candidate{
                ToIntLooseW(cols[5], 0),
                ToIntLooseW(cols[6], 0),
                ToIntLooseW(cols[7], 0),
                1,
                L"legacy-5-6-7"
                });
        }

        // 念のため: serviceId が最後尾にある系。
        if (cols.size() >= 9) {
            candidates.push_back(Candidate{
                ToIntLooseW(cols[8], 0),
                ToIntLooseW(cols[6], 0),
                ToIntLooseW(cols[7], 0),
                1,
                L"sid-last-8"
                });
        }

        // 念のため: networkId / tsId / serviceId が 5,6,7 の逆順系。
        if (cols.size() >= 8) {
            candidates.push_back(Candidate{
                ToIntLooseW(cols[7], 0),
                ToIntLooseW(cols[5], 0),
                ToIntLooseW(cols[6], 0),
                1,
                L"sid-7-nid-5-tsid-6"
                });
        }

        for (const auto& c : candidates) {
            // 今は service map の空化を避ける段階なので enabled=0 だけでは捨てない。
            // 実際の有効/無効は後段で別フィールドとして扱う。
            if (FillProgramPageServiceInfo_(
                serviceName,
                space,
                physicalChannel,
                remoteControlKeyId,
                c.networkId,
                c.tsId,
                c.serviceId,
                out)) {
                if (c.enabled == 0) {
                    DebugLogFormat(
                        L"[EPG-PAGE][SERVICE-MAP][INFO] disabled service included. name=%ls tag=%ls",
                        serviceName.c_str(),
                        c.tag);
                }

                DebugLogFormat(
                    L"[EPG-PAGE][SERVICE-MAP][MAP] name=%ls tag=%ls nid=%d tsid=%d sid=%d phys=%d",
                    serviceName.c_str(),
                    c.tag,
                    out.networkId,
                    out.tsId,
                    out.serviceId,
                    out.physicalChannel);

                return true;
            }
        }

        DebugLogFormat(
            L"[EPG-PAGE][SERVICE-MAP][SKIP] no valid column mapping. name=%ls space=%d cols=%zu c0=%ls c1=%ls c2=%ls c3=%ls c4=%ls c5=%ls c6=%ls c7=%ls c8=%ls",
            serviceName.c_str(),
            space,
            cols.size(),
            cols.size() > 0 ? cols[0].c_str() : L"",
            cols.size() > 1 ? cols[1].c_str() : L"",
            cols.size() > 2 ? cols[2].c_str() : L"",
            cols.size() > 3 ? cols[3].c_str() : L"",
            cols.size() > 4 ? cols[4].c_str() : L"",
            cols.size() > 5 ? cols[5].c_str() : L"",
            cols.size() > 6 ? cols[6].c_str() : L"",
            cols.size() > 7 ? cols[7].c_str() : L"",
            cols.size() > 8 ? cols[8].c_str() : L"");

        return false;
    }

    static std::vector<EpgHtmlPortal_ProgramPageWriter::ChannelServiceInfo>
        LoadProgramPageServiceMapFromChList_(
            const std::wstring& chListPath,
            int targetSpace)
    {
        std::vector<EpgHtmlPortal_ProgramPageWriter::ChannelServiceInfo> out;

        if (chListPath.empty()) {
            DebugLogFormat(
                L"[EPG-PAGE][SERVICE-MAP][WARN] channellist path empty. space=%d",
                targetSpace);
            return out;
        }

        const std::filesystem::path path(chListPath);
        if (!std::filesystem::exists(path)) {
            DebugLogFormat(
                L"[EPG-PAGE][SERVICE-MAP][WARN] channellist missing. space=%d path=%ls",
                targetSpace,
                chListPath.c_str());
            return out;
        }

        std::wstring text;
        if (!EpgHtmlPortal_Common::ReadTextFileToWide(path, text)) {
            DebugLogFormat(
                L"[EPG-PAGE][SERVICE-MAP][WARN] channellist read failed. space=%d path=%ls",
                targetSpace,
                chListPath.c_str());
            return out;
        }

        std::unordered_set<std::wstring> seenKeys;

        std::wistringstream iss(text);
        std::wstring line;

        std::size_t parsed = 0;
        std::size_t skipped = 0;
        std::size_t duplicated = 0;

        while (std::getline(iss, line)) {
            line = TrimAsciiWsW(line);

            if (line.empty()) {
                continue;
            }
            if (line[0] == L';' || line[0] == L'#') {
                continue;
            }

            const auto cols = ParseCsvLooseW(line);

            EpgHtmlPortal_ProgramPageWriter::ChannelServiceInfo svc{};
            if (!ParseProgramPageServiceInfoFromChListCols_(
                cols,
                targetSpace,
                svc)) {
                ++skipped;
                continue;
            }

            if (svc.serviceKey.empty()) {
                ++skipped;
                continue;
            }

            if (!seenKeys.insert(svc.serviceKey).second) {
                ++duplicated;
                continue;
            }

            out.push_back(std::move(svc));
            ++parsed;
        }

        std::sort(
            out.begin(),
            out.end(),
            [](const auto& a, const auto& b) {
                if (a.space != b.space) {
                    return a.space < b.space;
                }
                if (a.physicalChannel != b.physicalChannel) {
                    return a.physicalChannel < b.physicalChannel;
                }
                if (a.serviceId != b.serviceId) {
                    return a.serviceId < b.serviceId;
                }
                return a.serviceName < b.serviceName;
            });

        DebugLogFormat(
            L"[EPG-PAGE][SERVICE-MAP] loaded. space=%d count=%zu parsed=%zu skipped=%zu duplicated=%zu path=%ls",
            targetSpace,
            out.size(),
            parsed,
            skipped,
            duplicated,
            chListPath.c_str());

        return out;
    }

    static std::vector<std::vector<EpgHtmlPortal::ServiceEntry>>
        BuildEnabledSearchServicesBySpace_(
            const EpgBuildGUI::Options& o,
            std::wstring_view resolvedConfigIniPath)
    {
        std::vector<std::vector<EpgHtmlPortal::ServiceEntry>> out(3);

        const std::wstring configPath(resolvedConfigIniPath);

        for (int sp = 0; sp <= 2; ++sp) {
            std::vector<EpgHtmlPortal::ServiceEntry> fromIni;

            if (o.useEnabledServicesFromConfig && !configPath.empty()) {
                fromIni = LoadEnabledServiceEntriesFromConfigIni(
                    configPath,
                    sp);
            }

            if (!fromIni.empty()) {
                out[static_cast<std::size_t>(sp)] = std::move(fromIni);

                DebugLogFormat(
                    L"[EPG][SEARCH-SERVICE] use config.ini services. space=%d count=%zu",
                    sp,
                    out[static_cast<std::size_t>(sp)].size());

                continue;
            }

            const std::wstring spChListPath =
                ResolveChannelListPathForSearchServices_(sp, o);

            auto fromChList =
                LoadServiceEntriesFromChList_Strict(spChListPath, sp);

            if (!fromChList.empty()) {
                out[static_cast<std::size_t>(sp)] = std::move(fromChList);

                DebugLogFormat(
                    L"[EPG][SEARCH-SERVICE] use channellist services. space=%d count=%zu path=%ls",
                    sp,
                    out[static_cast<std::size_t>(sp)].size(),
                    spChListPath.c_str());

                continue;
            }

            DebugLogFormat(
                L"[EPG][SEARCH-SERVICE][WARN] no search service candidates. space=%d",
                sp);
        }

        return out;
    }

    static std::vector<int> LoadUniquePhysicalChannelsFromChList_Strict(const std::wstring& chListPath, int space)
    {
        ChannelListLoader loader;
        if (!loader.LoadFromChannelListCh(chListPath)) {
            DebugLogFormat(L"[EPG][ERROR] channellist load failed: %ls", chListPath.c_str());
            return {};
        }

        const auto& list = loader.GetChannelList();
        if (list.empty()) {
            DebugLogFormat(L"[EPG][WARN] channellist empty: %ls", chListPath.c_str());
            return {};
        }

        bool hasSpace = false;
        for (const auto& c : list) {
            if (c.space == space) { hasSpace = true; break; }
        }
        if (!hasSpace) {
            DebugLogFormat(L"[EPG][WARN] channellist has no entries for space=%d: %ls", space, chListPath.c_str());
            return {};
        }

        std::set<int> uniq;
        for (const auto& c : list) {
            if (c.space != space) continue;
            uniq.insert(c.channel);
        }

        std::vector<int> out;
        out.assign(uniq.begin(), uniq.end());
        return out;
    }

    struct EpgMergeEventKey_
    {
        std::uint16_t onid = 0;
        std::uint16_t tsid = 0;
        std::uint16_t sid = 0;
        std::uint16_t eid = 0;
        std::uint64_t startRaw = 0;

        bool operator==(const EpgMergeEventKey_& o) const noexcept
        {
            return onid == o.onid &&
                tsid == o.tsid &&
                sid == o.sid &&
                eid == o.eid &&
                startRaw == o.startRaw;
        }
    };

    struct EpgMergeEventKeyHash_
    {
        std::size_t operator()(const EpgMergeEventKey_& k) const noexcept
        {
            std::uint64_t x = 0;
            x ^= static_cast<std::uint64_t>(k.onid) << 48;
            x ^= static_cast<std::uint64_t>(k.tsid) << 32;
            x ^= static_cast<std::uint64_t>(k.sid) << 16;
            x ^= static_cast<std::uint64_t>(k.eid);
            x ^= k.startRaw + 0x9e3779b97f4a7c15ULL + (x << 6) + (x >> 2);
            x ^= (x >> 33);
            x *= 0xff51afd7ed558ccdULL;
            x ^= (x >> 33);
            return static_cast<std::size_t>(x);
        }
    };

    struct EpgMergeLogicalKey_
    {
        std::uint16_t onid = 0;
        std::uint16_t tsid = 0;
        std::uint16_t sid = 0;
        std::uint16_t eid = 0;

        bool operator==(const EpgMergeLogicalKey_& o) const noexcept
        {
            return onid == o.onid &&
                tsid == o.tsid &&
                sid == o.sid &&
                eid == o.eid;
        }
    };

    struct EpgMergeLogicalKeyHash_
    {
        std::size_t operator()(const EpgMergeLogicalKey_& k) const noexcept
        {
            std::uint64_t x = 0;
            x ^= static_cast<std::uint64_t>(k.onid) << 48;
            x ^= static_cast<std::uint64_t>(k.tsid) << 32;
            x ^= static_cast<std::uint64_t>(k.sid) << 16;
            x ^= static_cast<std::uint64_t>(k.eid);
            x ^= (x >> 33);
            x *= 0xff51afd7ed558ccdULL;
            x ^= (x >> 33);
            return static_cast<std::size_t>(x);
        }
    };

    struct EpgMergeBlock_
    {
        EpgMergeEventKey_ key{};
        std::uint32_t durationSec = 0;
        std::int64_t score = 0;
        std::uint64_t sequence = 0;

        std::int64_t startSec = 0;
        std::int64_t endSec = 0;
        bool hasTimeRange = false;

        std::wstring title;
        bool genericPlaceholder = false;

        std::wstring body;
    };

    static std::wstring TrimMergeLine_(std::wstring s)
    {
        while (!s.empty() &&
            (s.back() == L'\r' ||
                s.back() == L'\n' ||
                s.back() == L' ' ||
                s.back() == L'\t')) {
            s.pop_back();
        }

        std::size_t pos = 0;
        while (pos < s.size() &&
            (s[pos] == L' ' ||
                s[pos] == L'\t' ||
                s[pos] == L'\r' ||
                s[pos] == L'\n')) {
            ++pos;
        }

        if (pos > 0) {
            s.erase(0, pos);
        }

        return s;
    }

    static bool IsMergeNameChar_(wchar_t ch) noexcept
    {
        return
            (ch >= L'0' && ch <= L'9') ||
            (ch >= L'A' && ch <= L'Z') ||
            (ch >= L'a' && ch <= L'z') ||
            ch == L'_';
    }

    static int MergeHexValue_(wchar_t ch) noexcept
    {
        if (ch >= L'0' && ch <= L'9') {
            return static_cast<int>(ch - L'0');
        }
        if (ch >= L'A' && ch <= L'F') {
            return 10 + static_cast<int>(ch - L'A');
        }
        if (ch >= L'a' && ch <= L'f') {
            return 10 + static_cast<int>(ch - L'a');
        }
        return -1;
    }

    static int MergeFindNumberAfterKey_(
        std::wstring_view line,
        std::initializer_list<std::wstring_view> keys) noexcept
    {
        for (const auto key : keys) {
            if (key.empty()) {
                continue;
            }

            std::size_t searchPos = 0;

            while (searchPos < line.size()) {
                const std::size_t pos = line.find(key, searchPos);
                if (pos == std::wstring_view::npos) {
                    break;
                }

                const bool leftOk =
                    pos == 0 ||
                    !IsMergeNameChar_(line[pos - 1]);

                std::size_t cur = pos + key.size();

                const bool rightOk =
                    cur >= line.size() ||
                    !IsMergeNameChar_(line[cur]);

                if (!leftOk || !rightOk) {
                    searchPos = pos + 1;
                    continue;
                }

                while (cur < line.size() &&
                    (line[cur] == L' ' ||
                        line[cur] == L'\t' ||
                        line[cur] == L'=' ||
                        line[cur] == L':' ||
                        line[cur] == L'(' ||
                        line[cur] == L')')) {
                    ++cur;
                }

                if (cur >= line.size()) {
                    break;
                }

                bool isHex = false;

                if ((cur + 1) < line.size() &&
                    line[cur] == L'0' &&
                    (line[cur + 1] == L'x' || line[cur + 1] == L'X')) {
                    isHex = true;
                    cur += 2;
                }

                std::size_t end = cur;

                if (isHex) {
                    while (end < line.size() && MergeHexValue_(line[end]) >= 0) {
                        ++end;
                    }
                }
                else {
                    while (end < line.size() &&
                        line[end] >= L'0' &&
                        line[end] <= L'9') {
                        ++end;
                    }
                }

                if (end <= cur) {
                    break;
                }

                try {
                    const std::wstring numberText(line.substr(cur, end - cur));

                    if (isHex) {
                        return std::stoi(numberText, nullptr, 16);
                    }

                    return std::stoi(numberText, nullptr, 10);
                }
                catch (...) {
                    return 0;
                }
            }
        }

        return 0;
    }

    static std::uint64_t MergeFindFirstHexRaw_(std::wstring_view line) noexcept
    {
        const std::size_t pos = line.find(L"0x");
        if (pos == std::wstring_view::npos) {
            return 0;
        }

        std::size_t cur = pos + 2;
        std::uint64_t value = 0;
        bool found = false;

        while (cur < line.size()) {
            const int hv = MergeHexValue_(line[cur]);
            if (hv < 0) {
                break;
            }

            found = true;
            value = (value << 4) | static_cast<std::uint64_t>(hv & 0x0F);
            ++cur;
        }

        return found ? value : 0;
    }

    static int MergeBcdByteToInt_(std::uint8_t v) noexcept
    {
        return static_cast<int>(((v >> 4) & 0x0F) * 10 + (v & 0x0F));
    }

    static bool MergeStartRawToSeconds_(
        std::uint64_t raw,
        std::int64_t& outSeconds) noexcept
    {
        raw &= 0xFFFFFFFFFFULL;

        if (raw == 0 || raw == 0xFFFFFFFFFFULL) {
            return false;
        }

        const std::uint16_t mjd =
            static_cast<std::uint16_t>((raw >> 24) & 0xFFFFULL);

        const int h = MergeBcdByteToInt_(
            static_cast<std::uint8_t>((raw >> 16) & 0xFFULL));
        const int m = MergeBcdByteToInt_(
            static_cast<std::uint8_t>((raw >> 8) & 0xFFULL));
        const int s = MergeBcdByteToInt_(
            static_cast<std::uint8_t>(raw & 0xFFULL));

        if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) {
            return false;
        }

        outSeconds =
            static_cast<std::int64_t>(mjd) * 86400LL +
            static_cast<std::int64_t>(h) * 3600LL +
            static_cast<std::int64_t>(m) * 60LL +
            static_cast<std::int64_t>(s);

        return true;
    }

    static bool MergeStartRawNear_(
        std::uint64_t a,
        std::uint64_t b,
        std::uint32_t toleranceSec) noexcept
    {
        std::int64_t sa = 0;
        std::int64_t sb = 0;

        if (!MergeStartRawToSeconds_(a, sa) ||
            !MergeStartRawToSeconds_(b, sb)) {
            return false;
        }

        const std::int64_t diff = sa > sb ? (sa - sb) : (sb - sa);
        return diff <= static_cast<std::int64_t>(toleranceSec);
    }

    static bool MergeDurationNear_(
        std::uint32_t a,
        std::uint32_t b,
        std::uint32_t toleranceSec) noexcept
    {
        if (a == 0 || b == 0) {
            return true;
        }

        const std::uint32_t diff = a > b ? (a - b) : (b - a);
        return diff <= toleranceSec;
    }

    static bool MergeStartsWith_(
        std::wstring_view s,
        std::wstring_view prefix) noexcept;

    static std::wstring_view TrimMergeLineView_(
        std::wstring_view s) noexcept;

    static std::wstring MergeExtractValueAfterPrefixView_(
        std::wstring_view line,
        std::wstring_view prefix)
    {
        if (!MergeStartsWith_(line, prefix)) {
            return {};
        }

        line.remove_prefix(prefix.size());

        while (!line.empty() &&
            (line.front() == L' ' ||
                line.front() == L'\t' ||
                line.front() == L':' ||
                line.front() == L'：')) {
            line.remove_prefix(1);
        }

        line =
            TrimMergeLineView_(
                line);

        return std::wstring(line);
    }

    static std::wstring MergeCompactTitleForPlaceholderCheck_(
        std::wstring_view src)
    {
        std::wstring text(src);

        const std::wstring cutChars =
            L"[［(（";

        for (wchar_t cut : cutChars) {
            const std::size_t pos =
                text.find(cut);

            if (pos != std::wstring::npos) {
                text.resize(pos);
            }
        }

        std::wstring out;
        out.reserve(text.size());

        for (wchar_t ch : text) {
            if (ch == L' ' ||
                ch == L'\t' ||
                ch == L'\r' ||
                ch == L'\n' ||
                ch == L'　') {
                continue;
            }

            out.push_back(ch);
        }

        return out;
    }

    static bool MergeIsGenericPlaceholderTitle_(
        std::wstring_view title)
    {
        const std::wstring compact =
            MergeCompactTitleForPlaceholderCheck_(
                title);

        if (compact.empty()) {
            return false;
        }

        // 完全な汎用枠だけを対象にする。
        // 「名曲セレクション」「NHK高校講座 セレクション」などは消さない。
        return compact == L"セレクション" ||
            compact == L"未定" ||
            compact == L"番組未定" ||
            compact == L"放送番組未定" ||
            compact == L"内容未定" ||
            compact == L"詳細未定" ||
            compact == L"調整中";
    }

    static bool MergeBlocksOverlapEnough_(
        const EpgMergeBlock_& a,
        const EpgMergeBlock_& b,
        std::uint32_t minOverlapSec) noexcept
    {
        if (!a.hasTimeRange ||
            !b.hasTimeRange) {
            return false;
        }

        if (a.key.onid != b.key.onid ||
            a.key.tsid != b.key.tsid ||
            a.key.sid != b.key.sid) {
            return false;
        }

        const std::int64_t begin =
            (std::max)(a.startSec, b.startSec);

        const std::int64_t end =
            (std::min)(a.endSec, b.endSec);

        if (end <= begin) {
            return false;
        }

        return static_cast<std::uint32_t>(end - begin) >= minOverlapSec;
    }

    static bool MergeParseDurationLine_(
        std::wstring_view line,
        std::uint32_t& outSec) noexcept
    {
        outSec = 0;

        const std::size_t pos = line.find(L':');
        if (pos == std::wstring_view::npos) {
            return false;
        }

        const std::wstring s =
            TrimMergeLine_(std::wstring(line.substr(pos + 1)));

        int h = 0;
        int m = 0;
        int sec = 0;

        if (swscanf_s(s.c_str(), L"%d:%d:%d", &h, &m, &sec) != 3) {
            return false;
        }

        if (h < 0 || m < 0 || sec < 0 || m > 59 || sec > 59) {
            return false;
        }

        outSec =
            static_cast<std::uint32_t>(h * 3600 + m * 60 + sec);

        return outSec > 0;
    }

    struct EpgMergeBlockRange_
    {
        std::size_t begin = 0;
        std::size_t end = 0;
    };

    static bool MergeStartsWith_(
        std::wstring_view s,
        std::wstring_view prefix) noexcept
    {
        return s.size() >= prefix.size() &&
            s.substr(0, prefix.size()) == prefix;
    }

    static std::wstring_view TrimMergeLineView_(
        std::wstring_view s) noexcept
    {
        while (!s.empty()) {
            const wchar_t ch = s.front();

            if (ch != L' ' &&
                ch != L'\t' &&
                ch != L'\r' &&
                ch != L'\n' &&
                ch != L'\ufeff') {
                break;
            }

            s.remove_prefix(1);
        }

        while (!s.empty()) {
            const wchar_t ch = s.back();

            if (ch != L' ' &&
                ch != L'\t' &&
                ch != L'\r' &&
                ch != L'\n') {
                break;
            }

            s.remove_suffix(1);
        }

        return s;
    }

    static bool MergeParseIntRange_(
        std::wstring_view s,
        bool isHex,
        int& outValue) noexcept
    {
        outValue = 0;

        if (s.empty()) {
            return false;
        }

        int value = 0;
        bool found = false;

        for (const wchar_t ch : s) {
            int digit = -1;

            if (isHex) {
                digit = MergeHexValue_(ch);
            }
            else if (ch >= L'0' && ch <= L'9') {
                digit = static_cast<int>(ch - L'0');
            }

            if (digit < 0) {
                break;
            }

            found = true;

            const int base = isHex ? 16 : 10;

            if (value > ((0x7fffffff - digit) / base)) {
                return false;
            }

            value = value * base + digit;
        }

        if (!found) {
            return false;
        }

        outValue = value;
        return true;
    }

    static int MergeFindNumberAfterKeyView_(
        std::wstring_view line,
        std::initializer_list<std::wstring_view> keys) noexcept
    {
        for (const auto key : keys) {
            if (key.empty()) {
                continue;
            }

            std::size_t searchPos = 0;

            while (searchPos < line.size()) {
                const std::size_t pos = line.find(key, searchPos);

                if (pos == std::wstring_view::npos) {
                    break;
                }

                const bool leftOk =
                    pos == 0 ||
                    !IsMergeNameChar_(line[pos - 1u]);

                std::size_t cur = pos + key.size();

                const bool rightOk =
                    cur >= line.size() ||
                    !IsMergeNameChar_(line[cur]);

                if (!leftOk || !rightOk) {
                    searchPos = pos + 1u;
                    continue;
                }

                while (cur < line.size() &&
                    (line[cur] == L' ' ||
                        line[cur] == L'\t' ||
                        line[cur] == L'=' ||
                        line[cur] == L':' ||
                        line[cur] == L'(' ||
                        line[cur] == L')')) {
                    ++cur;
                }

                if (cur >= line.size()) {
                    break;
                }

                bool isHex = false;

                if ((cur + 1u) < line.size() &&
                    line[cur] == L'0' &&
                    (line[cur + 1u] == L'x' || line[cur + 1u] == L'X')) {
                    isHex = true;
                    cur += 2u;
                }

                std::size_t end = cur;

                if (isHex) {
                    while (end < line.size() &&
                        MergeHexValue_(line[end]) >= 0) {
                        ++end;
                    }
                }
                else {
                    while (end < line.size() &&
                        line[end] >= L'0' &&
                        line[end] <= L'9') {
                        ++end;
                    }
                }

                if (end <= cur) {
                    break;
                }

                int value = 0;

                if (MergeParseIntRange_(
                    line.substr(cur, end - cur),
                    isHex,
                    value)) {
                    return value;
                }

                return 0;
            }
        }

        return 0;
    }

    static bool MergeParseDurationLineView_(
        std::wstring_view line,
        std::uint32_t& outSec) noexcept
    {
        outSec = 0;

        const std::size_t pos = line.find(L':');

        if (pos == std::wstring_view::npos) {
            return false;
        }

        std::wstring_view s =
            TrimMergeLineView_(
                line.substr(pos + 1u));

        auto readNumber =
            [&s](int& outValue) noexcept -> bool
            {
                outValue = 0;

                if (s.empty() ||
                    s.front() < L'0' ||
                    s.front() > L'9') {
                    return false;
                }

                int value = 0;

                while (!s.empty() &&
                    s.front() >= L'0' &&
                    s.front() <= L'9') {
                    const int digit =
                        static_cast<int>(s.front() - L'0');

                    if (value > ((0x7fffffff - digit) / 10)) {
                        return false;
                    }

                    value = value * 10 + digit;
                    s.remove_prefix(1);
                }

                outValue = value;
                return true;
            };

        auto consumeColon =
            [&s]() noexcept -> bool
            {
                if (s.empty() || s.front() != L':') {
                    return false;
                }

                s.remove_prefix(1);
                return true;
            };

        int h = 0;
        int m = 0;
        int sec = 0;

        if (!readNumber(h) ||
            !consumeColon() ||
            !readNumber(m) ||
            !consumeColon() ||
            !readNumber(sec)) {
            return false;
        }

        if (h < 0 || m < 0 || sec < 0 || m > 59 || sec > 59) {
            return false;
        }

        outSec =
            static_cast<std::uint32_t>(
                h * 3600 + m * 60 + sec);

        return outSec > 0;
    }

    static std::int64_t MergeCalcBlockScoreView_(
        std::wstring_view block) noexcept
    {
        // 現在の MergeEpgFiles() は「後から読んだ新しいEPGブロック優先」で
        // 重複更新しており、score は採用判定に使っていない。
        //
        // 以前の実装では、ここで各ブロックに対して
        //   [短形式タイトル]
        //   [短形式概要]
        //   [ジャンル]
        //   Duration(HH:MM:SS):
        //   StartTime(raw MJD+BCD40bit):
        //   [拡張形式] / [詳細]
        // を block.find() で確認していた。
        //
        // 100万件級のEITブロックではこの全文検索が重くなるため、
        // 出力互換を維持したまま軽量化する。
        //
        // b.body は別途そのまま保持して merged EPG に出力するため、
        // ここを block.size() のみにしても EpgMerged_spaceX_All_EIT.epg の
        // 出力形式は変わらない。
        return static_cast<std::int64_t>(block.size());
    }

    static std::int64_t MergeCalcBlockScore_(
        const std::wstring& block) noexcept
    {
        return MergeCalcBlockScoreView_(
            std::wstring_view(block));
    }

    static bool MergeParseEventBlockView_(
        std::wstring_view block,
        std::uint64_t sequence,
        EpgMergeBlock_& out)
    {
        out = {};
        out.sequence = sequence;
        out.score = MergeCalcBlockScoreView_(block);

        std::size_t lineBegin = 0;

        while (lineBegin < block.size()) {
            std::size_t lineEnd = lineBegin;

            while (lineEnd < block.size() &&
                block[lineEnd] != L'\r' &&
                block[lineEnd] != L'\n') {
                ++lineEnd;
            }

            std::wstring_view line =
                TrimMergeLineView_(
                    block.substr(
                        lineBegin,
                        lineEnd - lineBegin));

            if (!line.empty()) {
                if (MergeStartsWith_(line, L"TableID:")) {
                    const int onid =
                        MergeFindNumberAfterKeyView_(
                            line,
                            {
                                L"OriginalNetworkID",
                                L"OriginalNetworkId",
                                L"ONID",
                                L"NetworkID",
                                L"NetworkId"
                            });

                    const int tsid =
                        MergeFindNumberAfterKeyView_(
                            line,
                            {
                                L"TransportStreamID",
                                L"TransportStreamId",
                                L"TSID",
                                L"TsId"
                            });

                    const int sid =
                        MergeFindNumberAfterKeyView_(
                            line,
                            {
                                L"ServiceID",
                                L"ServiceId",
                                L"SID"
                            });

                    const int eid =
                        MergeFindNumberAfterKeyView_(
                            line,
                            {
                                L"EventID",
                                L"EventId",
                                L"EID"
                            });

                    if (onid > 0 && onid <= 0xFFFF) {
                        out.key.onid =
                            static_cast<std::uint16_t>(onid);
                    }

                    if (tsid > 0 && tsid <= 0xFFFF) {
                        out.key.tsid =
                            static_cast<std::uint16_t>(tsid);
                    }

                    if (sid > 0 && sid <= 0xFFFF) {
                        out.key.sid =
                            static_cast<std::uint16_t>(sid);
                    }

                    if (eid > 0 && eid <= 0xFFFF) {
                        out.key.eid =
                            static_cast<std::uint16_t>(eid);
                    }
                }
                else if (MergeStartsWith_(line, L"StartTime(raw MJD+BCD40bit):")) {
                    out.key.startRaw =
                        MergeFindFirstHexRaw_(line) &
                        0xFFFFFFFFFFULL;
                }
                else if (MergeStartsWith_(line, L"Duration(HH:MM:SS):")) {
                    std::uint32_t sec = 0;

                    if (MergeParseDurationLineView_(
                        line,
                        sec)) {
                        out.durationSec = sec;
                    }
                }
                else if (MergeStartsWith_(line, L"[短形式タイトル]")) {
                    out.title =
                        MergeExtractValueAfterPrefixView_(
                            line,
                            L"[短形式タイトル]");

                    out.genericPlaceholder =
                        MergeIsGenericPlaceholderTitle_(
                            out.title);
                }
            }

            if (lineEnd >= block.size()) {
                break;
            }

            if (block[lineEnd] == L'\r' &&
                lineEnd + 1u < block.size() &&
                block[lineEnd + 1u] == L'\n') {
                lineBegin = lineEnd + 2u;
            }
            else {
                lineBegin = lineEnd + 1u;
            }
        }

        {
            std::int64_t startSec = 0;

            if (MergeStartRawToSeconds_(
                out.key.startRaw,
                startSec)) {
                out.startSec = startSec;

                const std::uint32_t durationSec =
                    out.durationSec > 0
                    ? out.durationSec
                    : 1u;

                out.endSec =
                    out.startSec +
                    static_cast<std::int64_t>(durationSec);

                out.hasTimeRange =
                    out.endSec > out.startSec;
            }
        }

        return out.key.onid != 0 &&
            out.key.tsid != 0 &&
            out.key.sid != 0 &&
            out.key.eid != 0 &&
            out.key.startRaw != 0;
    }

    static bool MergeParseEventBlock_(
        const std::wstring& block,
        std::uint64_t sequence,
        EpgMergeBlock_& out)
    {
        if (!MergeParseEventBlockView_(
            std::wstring_view(block),
            sequence,
            out)) {
            return false;
        }

        out.body = block;
        return true;
    }

    static std::size_t MergeLineBeginAt_(
        const std::wstring& text,
        std::size_t pos) noexcept
    {
        if (pos > text.size()) {
            pos = text.size();
        }

        while (pos > 0u) {
            const wchar_t prev =
                text[pos - 1u];

            if (prev == L'\n' || prev == L'\r') {
                break;
            }

            --pos;
        }

        return pos;
    }

    static bool MergeLineStartsWithTableIdAt_(
        const std::wstring& text,
        std::size_t markerPos,
        std::size_t& outBlockBegin) noexcept
    {
        outBlockBegin = 0;

        if (markerPos >= text.size()) {
            return false;
        }

        const std::size_t lineBegin =
            MergeLineBeginAt_(text, markerPos);

        std::size_t p = lineBegin;

        while (p < text.size() &&
            (text[p] == L' ' ||
                text[p] == L'\t' ||
                text[p] == L'\ufeff')) {
            ++p;
        }

        if (p != markerPos) {
            return false;
        }

        constexpr std::wstring_view kPrefix = L"TableID:";

        if (p + kPrefix.size() > text.size()) {
            return false;
        }

        if (std::wstring_view(
            text.data() + p,
            kPrefix.size()) != kPrefix) {
            return false;
        }

        outBlockBegin = p;
        return true;
    }

    static bool MergeIsBlankLineRange_(
        const std::wstring& text,
        std::size_t begin,
        std::size_t end) noexcept
    {
        if (begin > text.size()) {
            begin = text.size();
        }

        if (end > text.size()) {
            end = text.size();
        }

        if (begin > end) {
            return false;
        }

        for (std::size_t i = begin; i < end; ++i) {
            const wchar_t ch = text[i];

            if (ch != L' ' &&
                ch != L'\t' &&
                ch != L'\r' &&
                ch != L'\n') {
                return false;
            }
        }

        return true;
    }

    static std::size_t MergeNextLineBegin_(
        const std::wstring& text,
        std::size_t lineEnd,
        std::size_t limit) noexcept
    {
        if (lineEnd >= limit) {
            return limit;
        }

        if (text[lineEnd] == L'\r' &&
            lineEnd + 1u < limit &&
            text[lineEnd + 1u] == L'\n') {
            return lineEnd + 2u;
        }

        return lineEnd + 1u;
    }

    static std::size_t MergeFindBlockEndBeforeNext_(
        const std::wstring& text,
        std::size_t begin,
        std::size_t limit) noexcept
    {
        if (begin > text.size()) {
            begin = text.size();
        }

        if (limit > text.size()) {
            limit = text.size();
        }

        if (begin >= limit) {
            return begin;
        }

        std::size_t lineBegin = begin;
        bool firstLine = true;

        while (lineBegin < limit) {
            std::size_t lineEnd = lineBegin;

            while (lineEnd < limit &&
                text[lineEnd] != L'\r' &&
                text[lineEnd] != L'\n') {
                ++lineEnd;
            }

            if (!firstLine &&
                MergeIsBlankLineRange_(
                    text,
                    lineBegin,
                    lineEnd)) {
                return lineBegin;
            }

            firstLine = false;

            if (lineEnd >= limit) {
                break;
            }

            lineBegin =
                MergeNextLineBegin_(
                    text,
                    lineEnd,
                    limit);
        }

        return limit;
    }

    static std::wstring MergeMakeBlockText_(
        const std::wstring& text,
        std::size_t begin,
        std::size_t end)
    {
        if (begin > text.size()) {
            begin = text.size();
        }

        if (end > text.size()) {
            end = text.size();
        }

        if (begin > end) {
            begin = end;
        }

        while (begin < end &&
            (text[begin] == L'\r' ||
                text[begin] == L'\n' ||
                text[begin] == L'\ufeff')) {
            ++begin;
        }

        while (end > begin &&
            (text[end - 1u] == L'\r' ||
                text[end - 1u] == L'\n')) {
            --end;
        }

        std::wstring block =
            text.substr(begin, end - begin);

        if (!block.empty() && block.back() != L'\n') {
            block += L'\n';
        }

        return block;
    }

    static std::vector<std::wstring> MergeSplitEventBlocksLegacy_(
        const std::wstring& text)
    {
        std::vector<std::wstring> blocks;
        std::wstring cur;
        bool inEvent = false;

        std::wistringstream iss(text);
        std::wstring line;

        auto flush = [&]() {
            if (!cur.empty()) {
                blocks.push_back(std::move(cur));
                cur.clear();
            }

            inEvent = false;
            };

        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == L'\r') {
                line.pop_back();
            }

            const std::wstring trimmed =
                TrimMergeLine_(line);

            if (trimmed.rfind(L"TableID:", 0) == 0) {
                flush();

                inEvent = true;
                cur += line;
                cur += L'\n';
                continue;
            }

            if (!inEvent) {
                continue;
            }

            if (trimmed.empty()) {
                flush();
                continue;
            }

            cur += line;
            cur += L'\n';
        }

        flush();
        return blocks;
    }

    static bool MergeCollectEventBlockRanges_(
        const std::wstring& text,
        std::vector<EpgMergeBlockRange_>& outRanges)
    {
        outRanges.clear();

        if (text.empty()) {
            return true;
        }

        std::vector<std::size_t> markerOffsets;
        AppSimdPolicy::FixedTextScanResult scanStats{};

        const bool scanOk =
            AppSimdPolicy::CollectFixedTextOffsetsFromCurrentConfig(
                AppSimdPolicy::BlockKind::EpgBlockOffsetScan,
                text,
                L"TableID",
                markerOffsets,
                &scanStats);

        AppSimdPolicy::LogFixedTextScanResult(
            scanStats,
            L"MergeCollectEventBlockRanges.TableID");

        if (!scanOk || markerOffsets.empty()) {
            DebugLogFormat(
                L"[EPG][MERGE][RANGE][WARN] TableID scan unavailable. chars=%zu markers=%zu",
                text.size(),
                markerOffsets.size());

            return false;
        }

        std::vector<std::size_t> starts;
        starts.reserve(markerOffsets.size());

        for (const std::size_t markerPos : markerOffsets) {
            std::size_t blockBegin = 0;

            if (!MergeLineStartsWithTableIdAt_(
                text,
                markerPos,
                blockBegin)) {
                continue;
            }

            if (!starts.empty() &&
                starts.back() == blockBegin) {
                continue;
            }

            starts.push_back(blockBegin);
        }

        if (starts.empty()) {
            DebugLogFormat(
                L"[EPG][MERGE][RANGE][WARN] no line-head TableID markers. chars=%zu rawMarkers=%zu",
                text.size(),
                markerOffsets.size());

            return false;
        }

        std::sort(
            starts.begin(),
            starts.end());

        starts.erase(
            std::unique(
                starts.begin(),
                starts.end()),
            starts.end());

        outRanges.reserve(starts.size());

        for (std::size_t i = 0; i < starts.size(); ++i) {
            const std::size_t begin =
                starts[i];

            const std::size_t nextBegin =
                (i + 1u < starts.size())
                ? starts[i + 1u]
                : text.size();

            const std::size_t end =
                MergeFindBlockEndBeforeNext_(
                    text,
                    begin,
                    nextBegin);

            if (end > begin) {
                outRanges.push_back(
                    EpgMergeBlockRange_{
                        begin,
                        end
                    });
            }
        }

        DebugLogFormat(
            L"[EPG][MERGE][RANGE] collected: chars=%zu rawMarkers=%zu lineHead=%zu ranges=%zu",
            text.size(),
            markerOffsets.size(),
            starts.size(),
            outRanges.size());

        return !outRanges.empty();
    }

    static std::vector<std::wstring> MergeSplitEventBlocks_(
        const std::wstring& text)
    {
        std::vector<EpgMergeBlockRange_> ranges;

        if (!MergeCollectEventBlockRanges_(
            text,
            ranges)) {
            return MergeSplitEventBlocksLegacy_(text);
        }

        std::vector<std::wstring> blocks;
        blocks.reserve(ranges.size());

        for (const auto& r : ranges) {
            std::wstring block =
                MergeMakeBlockText_(
                    text,
                    r.begin,
                    r.end);

            if (!block.empty()) {
                blocks.push_back(std::move(block));
            }
        }

        return blocks;
    }

    struct EpgUpdateTextKey_
    {
        std::uint16_t onid = 0;
        std::uint16_t tsid = 0;
        std::uint16_t sid = 0;
        std::uint16_t eid = 0;
        std::uint64_t startRaw = 0;

        bool operator==(const EpgUpdateTextKey_& o) const noexcept
        {
            return onid == o.onid &&
                tsid == o.tsid &&
                sid == o.sid &&
                eid == o.eid &&
                startRaw == o.startRaw;
        }
    };

    struct EpgUpdateTextKeyHash_
    {
        std::size_t operator()(const EpgUpdateTextKey_& k) const noexcept
        {
            std::uint64_t x = 0;
            x ^= static_cast<std::uint64_t>(k.onid) << 48;
            x ^= static_cast<std::uint64_t>(k.tsid) << 32;
            x ^= static_cast<std::uint64_t>(k.sid) << 16;
            x ^= static_cast<std::uint64_t>(k.eid);
            x ^= k.startRaw + 0x9e3779b97f4a7c15ULL + (x << 6) + (x >> 2);
            x ^= (x >> 33);
            x *= 0xff51afd7ed558ccdULL;
            x ^= (x >> 33);
            return static_cast<std::size_t>(x);
        }
    };

    struct EpgUpdateBlockLoadStats_
    {
        std::size_t rawBlocks = 0;
        std::size_t parsedBlocks = 0;
        std::size_t duplicatedKeys = 0;
    };

    using EpgUpdateBlockMap_ =
        std::unordered_map<
        EpgUpdateTextKey_,
        std::wstring,
        EpgUpdateTextKeyHash_>;

    using EpgUpdateSeedMap_ =
        std::unordered_map<
        EpgUpdateTextKey_,
        ParsedEITPacket,
        EpgUpdateTextKeyHash_>;

    static std::uint32_t EpgUpdateHmsFromSeconds_(
        std::uint32_t sec) noexcept
    {
        const std::uint32_t h = sec / 3600u;
        sec %= 3600u;

        const std::uint32_t m = sec / 60u;
        const std::uint32_t s = sec % 60u;

        return h * 10000u + m * 100u + s;
    }

    static std::wstring EpgUpdateExtractAfterPrefix_(
        std::wstring_view line,
        std::wstring_view prefix)
    {
        if (!MergeStartsWith_(line, prefix)) {
            return {};
        }

        line.remove_prefix(prefix.size());

        while (!line.empty() &&
            (line.front() == L' ' ||
                line.front() == L'\t' ||
                line.front() == L':' ||
                line.front() == L'：')) {
            line.remove_prefix(1);
        }

        line =
            TrimMergeLineView_(
                line);

        return std::wstring(line);
    }

    static EpgUpdateTextKey_ EpgUpdateMakeKeyFromParsed_(
        const ParsedEITPacket& e) noexcept
    {
        return EpgUpdateTextKey_{
            e.original_network_id,
            e.transport_stream_id,
            e.service_id,
            e.event_id,
            e.start_time & 0xFFFFFFFFFFULL
        };
    }

    static bool EpgUpdateParseBlockForSeed_(
        std::wstring_view block,
        ParsedEITPacket& out)
    {
        out = {};

        std::size_t lineBegin = 0;

        while (lineBegin < block.size()) {
            std::size_t lineEnd = lineBegin;

            while (lineEnd < block.size() &&
                block[lineEnd] != L'\r' &&
                block[lineEnd] != L'\n') {
                ++lineEnd;
            }

            const std::wstring_view line =
                TrimMergeLineView_(
                    block.substr(
                        lineBegin,
                        lineEnd - lineBegin));

            if (!line.empty()) {
                if (MergeStartsWith_(line, L"TableID:")) {
                    out.table_id =
                        static_cast<std::uint8_t>(
                            MergeFindFirstHexRaw_(line) & 0xFFu);

                    const int onid =
                        MergeFindNumberAfterKeyView_(
                            line,
                            {
                                L"OriginalNetworkID",
                                L"OriginalNetworkId",
                                L"ONID",
                                L"NetworkID",
                                L"NetworkId"
                            });

                    const int tsid =
                        MergeFindNumberAfterKeyView_(
                            line,
                            {
                                L"TransportStreamID",
                                L"TransportStreamId",
                                L"TSID",
                                L"TsId"
                            });

                    const int sid =
                        MergeFindNumberAfterKeyView_(
                            line,
                            {
                                L"ServiceID",
                                L"ServiceId",
                                L"SID"
                            });

                    const int eid =
                        MergeFindNumberAfterKeyView_(
                            line,
                            {
                                L"EventID",
                                L"EventId",
                                L"EID"
                            });

                    if (onid > 0 && onid <= 0xFFFF) {
                        out.original_network_id =
                            static_cast<std::uint16_t>(onid);
                    }

                    if (tsid > 0 && tsid <= 0xFFFF) {
                        out.transport_stream_id =
                            static_cast<std::uint16_t>(tsid);
                    }

                    if (sid > 0 && sid <= 0xFFFF) {
                        out.service_id =
                            static_cast<std::uint16_t>(sid);
                    }

                    if (eid > 0 && eid <= 0xFFFF) {
                        out.event_id =
                            static_cast<std::uint16_t>(eid);
                    }
                }
                else if (MergeStartsWith_(line, L"Version:")) {
                    const int version =
                        MergeFindNumberAfterKeyView_(
                            line,
                            {
                                L"Version"
                            });

                    if (version >= 0 && version <= 31) {
                        out.version =
                            static_cast<std::uint8_t>(version);
                    }
                }
                else if (MergeStartsWith_(line, L"StartTime(raw MJD+BCD40bit):")) {
                    out.start_time =
                        MergeFindFirstHexRaw_(line) &
                        0xFFFFFFFFFFULL;
                }
                else if (MergeStartsWith_(line, L"Duration(HH:MM:SS):")) {
                    std::uint32_t durationSec = 0;

                    if (MergeParseDurationLineView_(
                        line,
                        durationSec)) {
                        out.duration = durationSec;
                        out.duration_hhmmss =
                            EpgUpdateHmsFromSeconds_(durationSec);
                    }
                }
                else if (MergeStartsWith_(line, L"[短形式タイトル]")) {
                    out.title_utf16 =
                        EpgUpdateExtractAfterPrefix_(
                            line,
                            L"[短形式タイトル]");
                }
            }

            if (lineEnd >= block.size()) {
                break;
            }

            if (block[lineEnd] == L'\r' &&
                lineEnd + 1u < block.size() &&
                block[lineEnd + 1u] == L'\n') {
                lineBegin = lineEnd + 2u;
            }
            else {
                lineBegin = lineEnd + 1u;
            }
        }

        return out.original_network_id != 0 &&
            out.transport_stream_id != 0 &&
            out.service_id != 0 &&
            out.event_id != 0 &&
            out.start_time != 0 &&
            out.start_time != 0xFFFFFFFFFFULL;
    }

    static bool LoadEpgUpdateBlocksAndSeeds_(
        const std::filesystem::path& path,
        EpgUpdateBlockMap_& outBlocks,
        std::vector<ParsedEITPacket>* outSeeds,
        EpgUpdateBlockLoadStats_& outStats)
    {
        outBlocks.clear();
        outStats = {};

        if (outSeeds) {
            outSeeds->clear();
        }

        std::wstring text;
        if (!EpgHtmlPortal_Common::ReadTextFileToWide(path, text)) {
            return false;
        }

        EpgUpdateSeedMap_ seedMap;
        seedMap.max_load_factor(0.70f);

        auto addParsedBlock =
            [&](
                const EpgUpdateTextKey_& key,
                ParsedEITPacket&& seed,
                std::wstring&& body)
            {
                if (outBlocks.find(key) != outBlocks.end()) {
                    ++outStats.duplicatedKeys;
                }

                // 追記運用の既存EPGは、後ろのブロックほど新しい。
                // seed も body も後勝ちにする。
                outBlocks[key] = std::move(body);
                seedMap[key] = std::move(seed);
                ++outStats.parsedBlocks;
            };

        std::vector<EpgMergeBlockRange_> ranges;

        if (MergeCollectEventBlockRanges_(
            text,
            ranges)) {
            outStats.rawBlocks = ranges.size();

            outBlocks.reserve(ranges.size());
            seedMap.reserve(ranges.size());

            for (const EpgMergeBlockRange_& range : ranges) {
                if (range.begin >= text.size() ||
                    range.end > text.size() ||
                    range.end <= range.begin) {
                    continue;
                }

                const std::wstring_view blockView(
                    text.data() + range.begin,
                    range.end - range.begin);

                ParsedEITPacket seed{};

                if (!EpgUpdateParseBlockForSeed_(
                    blockView,
                    seed)) {
                    continue;
                }

                const EpgUpdateTextKey_ key =
                    EpgUpdateMakeKeyFromParsed_(seed);

                std::wstring body =
                    MergeMakeBlockText_(
                        text,
                        range.begin,
                        range.end);

                if (body.empty()) {
                    continue;
                }

                addParsedBlock(
                    key,
                    std::move(seed),
                    std::move(body));
            }
        }
        else {
            const std::vector<std::wstring> blocks =
                MergeSplitEventBlocksLegacy_(text);

            outStats.rawBlocks = blocks.size();

            outBlocks.reserve(blocks.size());
            seedMap.reserve(blocks.size());

            for (const std::wstring& bodySrc : blocks) {
                ParsedEITPacket seed{};

                if (!EpgUpdateParseBlockForSeed_(
                    std::wstring_view(bodySrc),
                    seed)) {
                    continue;
                }

                const EpgUpdateTextKey_ key =
                    EpgUpdateMakeKeyFromParsed_(seed);

                std::wstring body = bodySrc;

                addParsedBlock(
                    key,
                    std::move(seed),
                    std::move(body));
            }
        }

        if (outSeeds) {
            outSeeds->reserve(seedMap.size());

            for (auto& kv : seedMap) {
                outSeeds->push_back(std::move(kv.second));
            }
        }

        return !outBlocks.empty();
    }

    static bool BuildUpdatedEpgTextFromSnapshot_(
        const std::vector<ParsedEITPacket>& snapshot,
        const EpgUpdateBlockMap_& existingBlocks,
        const EpgUpdateBlockMap_& newBlocks,
        std::wstring& outText,
        std::size_t& outWritten,
        std::size_t& outMissing)
    {
        outText.clear();
        outWritten = 0;
        outMissing = 0;

        std::unordered_set<EpgUpdateTextKey_, EpgUpdateTextKeyHash_> used;
        used.reserve(snapshot.size());

        outText.reserve(snapshot.size() * 512u);

        for (const ParsedEITPacket& e : snapshot) {
            const EpgUpdateTextKey_ key =
                EpgUpdateMakeKeyFromParsed_(e);

            if (key.onid == 0 ||
                key.tsid == 0 ||
                key.sid == 0 ||
                key.eid == 0 ||
                key.startRaw == 0 ||
                key.startRaw == 0xFFFFFFFFFFULL) {
                ++outMissing;
                continue;
            }

            if (!used.insert(key).second) {
                continue;
            }

            const std::wstring* body = nullptr;

            auto itNew =
                newBlocks.find(key);

            if (itNew != newBlocks.end()) {
                body = &itNew->second;
            }
            else {
                auto itOld =
                    existingBlocks.find(key);

                if (itOld != existingBlocks.end()) {
                    body = &itOld->second;
                }
            }

            if (!body || body->empty()) {
                ++outMissing;
                continue;
            }

            outText += *body;

            if (outText.empty() || outText.back() != L'\n') {
                outText += L'\n';
            }

            outText += L'\n';
            ++outWritten;
        }

        return outWritten > 0 && !outText.empty();
    }

    static bool WriteWideUtf8File_(
        const std::filesystem::path& path,
        const std::wstring& text)
    {
        try {
            std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                return false;
            }

            const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
            ofs.write(reinterpret_cast<const char*>(bom), 3);

#ifdef _WIN32
            if (!text.empty()) {
                const int bytes = ::WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    text.data(),
                    static_cast<int>(text.size()),
                    nullptr,
                    0,
                    nullptr,
                    nullptr);

                if (bytes <= 0) {
                    return false;
                }

                std::string utf8(static_cast<std::size_t>(bytes), '\0');

                const int written = ::WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    text.data(),
                    static_cast<int>(text.size()),
                    utf8.data(),
                    bytes,
                    nullptr,
                    nullptr);

                if (written <= 0) {
                    return false;
                }

                ofs.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
            }
#else
            std::string utf8(text.begin(), text.end());
            ofs.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
#endif

            ofs.flush();
            return ofs.good();
        }
        catch (...) {
            return false;
        }
    }

    static bool ShouldReplaceMergeBlock_(
        const EpgMergeBlock_& oldBlock,
        const EpgMergeBlock_& newBlock) noexcept
    {
        (void)oldBlock;

        // EPG更新では、新しく取得したイベント情報を優先する。
        // チャンネル別EPGは append 保存なので、後から読まれる block ほど新しい。
        // score は診断用に残し、採用判定には使わない。
        return newBlock.sequence >= oldBlock.sequence;
    }

    static bool MergeEpgFiles(
        const std::vector<std::filesystem::path>& inputs,
        const std::filesystem::path& mergedOutPath)
    {
        std::unordered_map<EpgMergeEventKey_, EpgMergeBlock_, EpgMergeEventKeyHash_> byStrong;
        std::unordered_map<EpgMergeLogicalKey_, std::vector<EpgMergeEventKey_>, EpgMergeLogicalKeyHash_> byLogical;

        // EPGマージでは rawBlocks は100万件級でも、
// unique は10万件前後になりやすい。
//
// unordered_map の rehash を抑えて、重複整理時のメモリ再配置と
// 処理時間のブレを減らす。
        byStrong.max_load_factor(0.70f);
        byLogical.max_load_factor(0.70f);

        byStrong.reserve(262144);
        byLogical.reserve(262144);

        std::size_t inputFiles = 0;
        std::size_t rawBlocks = 0;
        std::size_t parsedBlocks = 0;
        std::size_t duplicateStrong = 0;
        std::size_t replacedNearStart = 0;
        std::size_t droppedStalePlaceholder = 0;
        std::uint64_t sequence = 0;

        constexpr std::uint32_t kStartReplaceToleranceSec = 10 * 60;
        constexpr std::uint32_t kDurationToleranceSec = 60;

        auto removeLogicalKey = [&](
            const EpgMergeLogicalKey_& lk,
            const EpgMergeEventKey_& sk)
            {
                auto it = byLogical.find(lk);
                if (it == byLogical.end()) {
                    return;
                }

                auto& v = it->second;
                v.erase(
                    std::remove_if(
                        v.begin(),
                        v.end(),
                        [&](const EpgMergeEventKey_& x) {
                            return x == sk;
                        }),
                    v.end());

                if (v.empty()) {
                    byLogical.erase(it);
                }
            };

        auto addBlock = [&](EpgMergeBlock_&& block)
            {
                const EpgMergeEventKey_ newStrongKey =
                    block.key;

                const EpgMergeLogicalKey_ lk{
                    block.key.onid,
                    block.key.tsid,
                    block.key.sid,
                    block.key.eid
                };

                auto itStrong =
                    byStrong.find(block.key);

                if (itStrong != byStrong.end()) {
                    ++duplicateStrong;

                    // 完全一致は同一時刻の同一イベントなので、新規データを優先する。
                    // body も含めて move 置換し、中間コピーを避ける。
                    itStrong->second =
                        std::move(block);
                    return;
                }

                auto itLogical =
                    byLogical.find(lk);

                if (itLogical != byLogical.end()) {
                    for (const auto& oldKey : itLogical->second) {
                        auto itOld =
                            byStrong.find(oldKey);

                        if (itOld == byStrong.end()) {
                            continue;
                        }

                        const auto& oldBlock =
                            itOld->second;

                        if (!MergeStartRawNear_(
                            oldBlock.key.startRaw,
                            block.key.startRaw,
                            kStartReplaceToleranceSec)) {
                            continue;
                        }

                        if (!MergeDurationNear_(
                            oldBlock.durationSec,
                            block.durationSec,
                            kDurationToleranceSec)) {
                            continue;
                        }

                        // 同一イベントIDで開始時刻・duration が近い場合は、
                        // 番組情報の更新と判断し、新規データで置換する。
                        const EpgMergeEventKey_ replaceKey =
                            oldKey;

                        byStrong.erase(replaceKey);
                        removeLogicalKey(lk, replaceKey);

                        byStrong.emplace(
                            newStrongKey,
                            std::move(block));

                        byLogical[lk].push_back(newStrongKey);

                        ++replacedNearStart;
                        return;
                    }
                }

                byStrong.emplace(
                    newStrongKey,
                    std::move(block));

                byLogical[lk].push_back(newStrongKey);
            };

        for (const auto& p : inputs) {
            if (!std::filesystem::exists(p)) {
                continue;
            }

            std::wstring text;
            if (!EpgHtmlPortal_Common::ReadTextFileToWide(p, text)) {
                DebugLogFormat(
                    L"[EPG][MERGE][WARN] read failed: %ls",
                    p.wstring().c_str());
                continue;
            }

            ++inputFiles;

            std::vector<EpgMergeBlockRange_> ranges;

            if (MergeCollectEventBlockRanges_(
                text,
                ranges)) {
                rawBlocks += ranges.size();

                for (const auto& range : ranges) {
                    const std::uint64_t blockSequence =
                        sequence++;

                    if (range.begin >= text.size() ||
                        range.end > text.size() ||
                        range.end <= range.begin) {
                        continue;
                    }

                    const std::wstring_view bodyView(
                        text.data() + range.begin,
                        range.end - range.begin);

                    EpgMergeBlock_ block{};

                    if (!MergeParseEventBlockView_(
                        bodyView,
                        blockSequence,
                        block)) {
                        continue;
                    }

                    // 出力形式維持用。
                    // ここでだけ従来と同じイベントブロック本文を作る。
                    block.body =
                        MergeMakeBlockText_(
                            text,
                            range.begin,
                            range.end);

                    if (block.body.empty()) {
                        continue;
                    }

                    ++parsedBlocks;

                    addBlock(std::move(block));
                }
            }
            else {
                // 保険ルート。
                // SIMD/range split が失敗した場合だけ従来方式へ戻す。
                auto blocks =
                    MergeSplitEventBlocksLegacy_(text);

                rawBlocks += blocks.size();

                for (const auto& body : blocks) {
                    const std::uint64_t blockSequence =
                        sequence++;

                    EpgMergeBlock_ block{};

                    if (!MergeParseEventBlock_(
                        body,
                        blockSequence,
                        block)) {
                        continue;
                    }

                    ++parsedBlocks;

                    addBlock(std::move(block));
                }
            }
        }

        if (byStrong.empty()) {
            DebugLogFormat(
                L"[EPG][MERGE][ERROR] no parsed events. inputs=%zu rawBlocks=%zu parsed=%zu out=%ls",
                inputFiles,
                rawBlocks,
                parsedBlocks,
                mergedOutPath.wstring().c_str());
            return false;
        }

        {
            constexpr std::uint32_t kPlaceholderOverlapMinSec = 60;

            std::vector<EpgMergeEventKey_> removeKeys;

            for (const auto& placeholderPair : byStrong) {
                const EpgMergeBlock_& placeholder =
                    placeholderPair.second;

                if (!placeholder.genericPlaceholder) {
                    continue;
                }

                for (const auto& realPair : byStrong) {
                    const EpgMergeBlock_& real =
                        realPair.second;

                    if (placeholderPair.first == realPair.first) {
                        continue;
                    }

                    if (real.genericPlaceholder) {
                        continue;
                    }

                    if (!MergeBlocksOverlapEnough_(
                        placeholder,
                        real,
                        kPlaceholderOverlapMinSec)) {
                        continue;
                    }

                    removeKeys.push_back(
                        placeholderPair.first);

                    break;
                }
            }

            if (!removeKeys.empty()) {
                std::sort(
                    removeKeys.begin(),
                    removeKeys.end(),
                    [](const EpgMergeEventKey_& a, const EpgMergeEventKey_& b) {
                        if (a.onid != b.onid) return a.onid < b.onid;
                        if (a.tsid != b.tsid) return a.tsid < b.tsid;
                        if (a.sid != b.sid) return a.sid < b.sid;
                        if (a.eid != b.eid) return a.eid < b.eid;
                        return a.startRaw < b.startRaw;
                    });

                removeKeys.erase(
                    std::unique(
                        removeKeys.begin(),
                        removeKeys.end(),
                        [](const EpgMergeEventKey_& a, const EpgMergeEventKey_& b) {
                            return a == b;
                        }),
                    removeKeys.end());

                for (const EpgMergeEventKey_& key : removeKeys) {
                    auto it =
                        byStrong.find(key);

                    if (it == byStrong.end()) {
                        continue;
                    }

                    const EpgMergeLogicalKey_ lk{
                        it->second.key.onid,
                        it->second.key.tsid,
                        it->second.key.sid,
                        it->second.key.eid
                    };

                    byStrong.erase(it);
                    removeLogicalKey(lk, key);
                    ++droppedStalePlaceholder;
                }
            }
        }

        std::vector<EpgMergeBlock_> out;
        out.reserve(byStrong.size());

        for (auto& kv : byStrong) {
            out.push_back(std::move(kv.second));
        }

        std::sort(
            out.begin(),
            out.end(),
            [](const EpgMergeBlock_& a, const EpgMergeBlock_& b) {
                if (a.key.onid != b.key.onid) return a.key.onid < b.key.onid;
                if (a.key.tsid != b.key.tsid) return a.key.tsid < b.key.tsid;
                if (a.key.sid != b.key.sid) return a.key.sid < b.key.sid;
                if (a.key.startRaw != b.key.startRaw) return a.key.startRaw < b.key.startRaw;
                if (a.key.eid != b.key.eid) return a.key.eid < b.key.eid;
                return a.sequence < b.sequence;
            });

        std::wstring mergedText;
        mergedText.reserve(out.size() * 512);

        for (const auto& b : out) {
            mergedText += b.body;

            if (mergedText.empty() || mergedText.back() != L'\n') {
                mergedText += L'\n';
            }

            mergedText += L'\n';
        }

        if (!WriteWideUtf8File_(mergedOutPath, mergedText)) {
            DebugLogFormat(
                L"[EPG][MERGE][ERROR] write failed: %ls",
                mergedOutPath.wstring().c_str());
            return false;
        }

        DebugLogFormat(
            L"[EPG] merge ok: %ls inputs=%zu rawBlocks=%zu parsed=%zu unique=%zu dupStrong=%zu replacedNearStart=%zu droppedStalePlaceholder=%zu",
            mergedOutPath.wstring().c_str(),
            inputFiles,
            rawBlocks,
            parsedBlocks,
            out.size(),
            duplicateStrong,
            replacedNearStart,
            droppedStalePlaceholder);

        return true;
    }

    static EpgHtmlLite::SharedData* g_LiteSharedData_ = nullptr;

    struct LiteSharedDataGuard_
    {
        ~LiteSharedDataGuard_() noexcept
        {
            g_LiteSharedData_ = nullptr;
        }
    };

    static std::wstring Utf8OrAnsiToWide_(std::string_view s)
    {
#ifdef _WIN32
        if (s.empty()) return {};

        int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (n > 0) {
            std::wstring out(static_cast<std::size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
            return out;
        }

        n = ::MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (n > 0) {
            std::wstring out(static_cast<std::size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
            return out;
        }
#endif
        return std::wstring(s.begin(), s.end());
    }

    template <class T>
    static std::wstring ToWStringLoose_(const T& v)
    {
        using U = std::remove_cvref_t<T>;

        if constexpr (std::is_same_v<U, std::wstring>) {
            return v;
        }
        else if constexpr (std::is_same_v<U, std::wstring_view>) {
            return std::wstring(v);
        }
        else if constexpr (std::is_same_v<U, const wchar_t*> || std::is_same_v<U, wchar_t*>) {
            return v ? std::wstring(v) : std::wstring{};
        }
        else if constexpr (std::is_same_v<U, std::string>) {
            return Utf8OrAnsiToWide_(v);
        }
        else if constexpr (std::is_same_v<U, std::string_view>) {
            return Utf8OrAnsiToWide_(v);
        }
        else if constexpr (std::is_same_v<U, const char*> || std::is_same_v<U, char*>) {
            return v ? Utf8OrAnsiToWide_(std::string_view(v)) : std::wstring{};
        }
        else if constexpr (std::is_integral_v<U>) {
            return std::to_wstring(static_cast<long long>(v));
        }
        else {
            return {};
        }
    }

    template <class TimeLike>
    static bool TryFormatHHMMFromTimeLike_(const TimeLike& t, std::wstring& out)
    {
        int h = -1;
        int m = -1;

        if constexpr (requires { t.hour; t.minute; }) {
            h = static_cast<int>(t.hour);
            m = static_cast<int>(t.minute);
        }
        else if constexpr (requires { t.Hour; t.Minute; }) {
            h = static_cast<int>(t.Hour);
            m = static_cast<int>(t.Minute);
        }
        else {
            return false;
        }

        if (h < 0 || h > 23 || m < 0 || m > 59) {
            return false;
        }

        wchar_t buf[16]{};
        swprintf_s(buf, L"%02d:%02d", h, m);
        out.assign(buf);
        return true;
    }

    static bool ParseHHMM_(std::wstring_view s, int& h, int& m) noexcept
    {
        h = 0;
        m = 0;
        if (s.size() < 4) return false;

        wchar_t c1 = 0;
        std::wstringstream ss;
        ss << s;
        ss >> h >> c1 >> m;
        if (!ss || c1 != L':') return false;
        if (h < 0 || h > 23 || m < 0 || m > 59) return false;
        return true;
    }

    static std::wstring AddMinutesToHHMM_(std::wstring_view hhmm, int addMinutes)
    {
        int h = 0;
        int m = 0;
        if (!ParseHHMM_(hhmm, h, m)) {
            return L"--:--";
        }

        int total = h * 60 + m + addMinutes;
        total %= (24 * 60);
        if (total < 0) total += (24 * 60);

        wchar_t buf[16]{};
        swprintf_s(buf, L"%02d:%02d", total / 60, total % 60);
        return std::wstring(buf);
    }

    template <class Ev>
    static std::wstring ExtractTitleFromEvent_(const Ev& ev)
    {
        if constexpr (requires { ev.shortTitle; }) {
            const auto s = ToWStringLoose_(ev.shortTitle);
            if (!s.empty()) return s;
        }
        if constexpr (requires { ev.eventName; }) {
            const auto s = ToWStringLoose_(ev.eventName);
            if (!s.empty()) return s;
        }
        if constexpr (requires { ev.title; }) {
            const auto s = ToWStringLoose_(ev.title);
            if (!s.empty()) return s;
        }
        if constexpr (requires { ev.name; }) {
            const auto s = ToWStringLoose_(ev.name);
            if (!s.empty()) return s;
        }
        return {};
    }

    template <class Ev>
    static std::wstring ExtractDescriptionFromEvent_(const Ev& ev)
    {
        if constexpr (requires { ev.extendedText; }) {
            const auto s = ToWStringLoose_(ev.extendedText);
            if (!s.empty()) return s;
        }
        if constexpr (requires { ev.description; }) {
            const auto s = ToWStringLoose_(ev.description);
            if (!s.empty()) return s;
        }
        if constexpr (requires { ev.eventText; }) {
            const auto s = ToWStringLoose_(ev.eventText);
            if (!s.empty()) return s;
        }
        if constexpr (requires { ev.shortText; }) {
            const auto s = ToWStringLoose_(ev.shortText);
            if (!s.empty()) return s;
        }
        if constexpr (requires { ev.summary; }) {
            const auto s = ToWStringLoose_(ev.summary);
            if (!s.empty()) return s;
        }
        return {};
    }

    template <class Ev>
    static std::wstring ExtractServiceNameFromEvent_(const Ev& ev)
    {
        if constexpr (requires { ev.serviceName; }) {
            const auto s = ToWStringLoose_(ev.serviceName);
            if (!s.empty()) return s;
        }
        if constexpr (requires { ev.service_name; }) {
            const auto s = ToWStringLoose_(ev.service_name);
            if (!s.empty()) return s;
        }
        if constexpr (requires { ev.channelName; }) {
            const auto s = ToWStringLoose_(ev.channelName);
            if (!s.empty()) return s;
        }
        if constexpr (requires { ev.channel_name; }) {
            const auto s = ToWStringLoose_(ev.channel_name);
            if (!s.empty()) return s;
        }
        return {};
    }

    template <class Ev>
    static std::wstring ExtractStartTextFromEvent_(const Ev& ev)
    {
        if constexpr (requires { ev.startTimeText; }) {
            const auto s = ToWStringLoose_(ev.startTimeText);
            if (!s.empty()) return s;
        }
        if constexpr (requires { ev.startText; }) {
            const auto s = ToWStringLoose_(ev.startText);
            if (!s.empty()) return s;
        }
        if constexpr (requires { ev.start; }) {
            std::wstring out;
            if (TryFormatHHMMFromTimeLike_(ev.start, out)) return out;
        }
        if constexpr (requires { ev.startTime; }) {
            std::wstring out;
            if (TryFormatHHMMFromTimeLike_(ev.startTime, out)) return out;
        }
        return L"--:--";
    }

    template <class Ev>
    static int ExtractDurationMinutesFromEvent_(const Ev& ev)
    {
        if constexpr (requires { ev.durationMinutes; }) {
            return static_cast<int>(ev.durationMinutes);
        }
        if constexpr (requires { ev.durationMin; }) {
            return static_cast<int>(ev.durationMin);
        }
        if constexpr (requires { ev.durationSeconds; }) {
            const int sec = static_cast<int>(ev.durationSeconds);
            return sec > 0 ? ((sec + 59) / 60) : -1;
        }
        return -1;
    }

    template <class Ev>
    static std::wstring ExtractEndTextFromEvent_(const Ev& ev)
    {
        if constexpr (requires { ev.endTimeText; }) {
            const auto s = ToWStringLoose_(ev.endTimeText);
            if (!s.empty()) return s;
        }
        if constexpr (requires { ev.endText; }) {
            const auto s = ToWStringLoose_(ev.endText);
            if (!s.empty()) return s;
        }
        if constexpr (requires { ev.end; }) {
            std::wstring out;
            if (TryFormatHHMMFromTimeLike_(ev.end, out)) return out;
        }
        if constexpr (requires { ev.endTime; }) {
            std::wstring out;
            if (TryFormatHHMMFromTimeLike_(ev.endTime, out)) return out;
        }

        const std::wstring start = ExtractStartTextFromEvent_(ev);
        const int durMin = ExtractDurationMinutesFromEvent_(ev);
        if (durMin > 0 && start != L"--:--") {
            return AddMinutesToHHMM_(start, durMin);
        }

        return L"--:--";
    }

    template <class Ev>
    static std::uint16_t ExtractServiceIdFromEvent_(const Ev& ev) noexcept
    {
        if constexpr (requires { ev.serviceId; }) {
            return static_cast<std::uint16_t>(ev.serviceId);
        }
        if constexpr (requires { ev.service_id; }) {
            return static_cast<std::uint16_t>(ev.service_id);
        }
        if constexpr (requires { ev.sid; }) {
            return static_cast<std::uint16_t>(ev.sid);
        }
        return 0;
    }

    template <class Ev>
    static std::uint16_t ExtractEventIdFromEvent_(const Ev& ev) noexcept
    {
        if constexpr (requires { ev.eventId; }) {
            return static_cast<std::uint16_t>(ev.eventId);
        }
        if constexpr (requires { ev.event_id; }) {
            return static_cast<std::uint16_t>(ev.event_id);
        }
        if constexpr (requires { ev.eid; }) {
            return static_cast<std::uint16_t>(ev.eid);
        }
        return 0;
    }

    static std::wstring GenreKeyFromMajor_(
        std::uint8_t major)
    {
        switch (major & 0x0F) {
        case 0x0:
            return L"news";
        case 0x1:
            return L"sports";
        case 0x2:
            return L"info";
        case 0x3:
            return L"drama";
        case 0x4:
            return L"music";
        case 0x5:
            return L"variety";
        case 0x6:
            return L"movie";
        case 0x7:
            return L"anime";
        case 0x8:
            return L"documentary";
        case 0x9:
            return L"stage";
        case 0xA:
            return L"hobby";
        default:
            return L"other";
        }
    }

    static std::wstring GuessGenreKeyFromText_(
        std::wstring s)
    {
        for (wchar_t& ch : s) {
            ch = static_cast<wchar_t>(
                ::towlower(
                    static_cast<wint_t>(ch)));
        }

        if (s.find(L"ニュース") != std::wstring::npos ||
            s.find(L"報道") != std::wstring::npos ||
            s.find(L"news") != std::wstring::npos ||
            s.find(L"report") != std::wstring::npos) {
            return L"news";
        }

        if (s.find(L"スポーツ") != std::wstring::npos ||
            s.find(L"sports") != std::wstring::npos ||
            s.find(L"sport") != std::wstring::npos) {
            return L"sports";
        }

        if (s.find(L"情報") != std::wstring::npos ||
            s.find(L"ワイド") != std::wstring::npos ||
            s.find(L"info") != std::wstring::npos ||
            s.find(L"information") != std::wstring::npos) {
            return L"info";
        }

        if (s.find(L"ドラマ") != std::wstring::npos ||
            s.find(L"drama") != std::wstring::npos) {
            return L"drama";
        }

        if (s.find(L"音楽") != std::wstring::npos ||
            s.find(L"music") != std::wstring::npos) {
            return L"music";
        }

        if (s.find(L"バラエティ") != std::wstring::npos ||
            s.find(L"バラエティー") != std::wstring::npos ||
            s.find(L"variety") != std::wstring::npos) {
            return L"variety";
        }

        if (s.find(L"映画") != std::wstring::npos ||
            s.find(L"movie") != std::wstring::npos ||
            s.find(L"cinema") != std::wstring::npos ||
            s.find(L"film") != std::wstring::npos) {
            return L"movie";
        }

        if (s.find(L"アニメ") != std::wstring::npos ||
            s.find(L"特撮") != std::wstring::npos ||
            s.find(L"anime") != std::wstring::npos ||
            s.find(L"animation") != std::wstring::npos ||
            s.find(L"tokusatsu") != std::wstring::npos) {
            return L"anime";
        }

        if (s.find(L"ドキュメンタリー") != std::wstring::npos ||
            s.find(L"教養") != std::wstring::npos ||
            s.find(L"documentary") != std::wstring::npos ||
            s.find(L"culture") != std::wstring::npos) {
            return L"documentary";
        }

        if (s.find(L"劇場") != std::wstring::npos ||
            s.find(L"公演") != std::wstring::npos ||
            s.find(L"stage") != std::wstring::npos ||
            s.find(L"theater") != std::wstring::npos ||
            s.find(L"theatre") != std::wstring::npos ||
            s.find(L"performance") != std::wstring::npos) {
            return L"stage";
        }

        if (s.find(L"趣味") != std::wstring::npos ||
            s.find(L"教育") != std::wstring::npos ||
            s.find(L"hobby") != std::wstring::npos ||
            s.find(L"education") != std::wstring::npos ||
            s.find(L"edu") != std::wstring::npos) {
            return L"hobby";
        }

        return L"other";
    }

    template <class Ev>
    static std::wstring ExtractGenreKeyFromEvent_(const Ev& ev)
    {
        if constexpr (requires { ev.genreMajor; }) {
            return GenreKeyFromMajor_(static_cast<std::uint8_t>(ev.genreMajor));
        }
        if constexpr (requires { ev.contentNibbleLevel1; }) {
            return GenreKeyFromMajor_(static_cast<std::uint8_t>(ev.contentNibbleLevel1));
        }
        if constexpr (requires { ev.genreText; }) {
            const auto s = ToWStringLoose_(ev.genreText);
            if (!s.empty()) return GuessGenreKeyFromText_(s);
        }
        if constexpr (requires { ev.genre; }) {
            const auto s = ToWStringLoose_(ev.genre);
            if (!s.empty()) return GuessGenreKeyFromText_(s);
        }
        return L"all";
    }

    static std::wstring MakeBroadcastTypeLabel_(int space)
    {
        switch (space) {
        case 0: return L"地上波";
        case 1: return L"BS";
        case 2: return L"CS";
        default: return L"その他";
        }
    }

    static std::wstring MakeReserveKey_(const EpgHtmlLite::ProgramEntry& e)
    {
        if (e.serviceId != 0 || e.eventId != 0) {
            return L"svc" + std::to_wstring(e.serviceId) + L"_evt" + std::to_wstring(e.eventId);
        }

        std::wstring key = e.serviceName + L"_" + e.startText + L"_" + e.title;
        for (wchar_t& ch : key) {
            if (ch == L' ' || ch == L':' || ch == L'/' || ch == L'\\' || ch == L'\t') {
                ch = L'_';
            }
        }
        return key;
    }


    static std::wstring LiteTrimHHMM_(const std::wstring& s)
    {
        if (s.size() >= 5) {
            return s.substr(0, 5);
        }
        return s.empty() ? L"--:--" : s;
    }

    static bool LiteParseIsoDateTime_(
        const std::wstring& date,
        const std::wstring& time,
        std::tm& outTm) noexcept
    {
        if (date.size() < 10 || time.size() < 5) {
            return false;
        }

        int y = 0;
        int mo = 0;
        int d = 0;
        int h = 0;
        int mi = 0;
        int sec = 0;

        if (swscanf_s(date.c_str(), L"%d-%d-%d", &y, &mo, &d) != 3) {
            return false;
        }
        if (swscanf_s(time.c_str(), L"%d:%d:%d", &h, &mi, &sec) < 2) {
            return false;
        }

        outTm = {};
        outTm.tm_year = y - 1900;
        outTm.tm_mon = mo - 1;
        outTm.tm_mday = d;
        outTm.tm_hour = h;
        outTm.tm_min = mi;
        outTm.tm_sec = sec;
        outTm.tm_isdst = -1;
        return true;
    }

    static bool LiteIsPastProgram_(
        const std::wstring& endDate,
        const std::wstring& endTime) noexcept
    {
        std::tm tmv{};
        if (!LiteParseIsoDateTime_(endDate, endTime, tmv)) {
            return false;
        }

        const std::time_t endEpoch = std::mktime(&tmv);
        if (endEpoch == static_cast<std::time_t>(-1)) {
            return false;
        }

        const std::time_t nowEpoch = std::time(nullptr);
        return endEpoch < nowEpoch;
    }


    static std::wstring MakeLiteBaseBroadcastDateToday_()
    {
        std::time_t now = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &now);
#else
        tmv = *std::localtime(&now);
#endif

        wchar_t buf[16]{};
        swprintf_s(
            buf,
            L"%04d-%02d-%02d",
            tmv.tm_year + 1900,
            tmv.tm_mon + 1,
            tmv.tm_mday);
        return std::wstring(buf);
    }

    static void NormalizeLiteProgramDate_(
        EpgHtmlLite::ProgramEntry& row,
        const std::wstring& baseBroadcastDate)
    {
        EpgHtmlLite::LiteDateYmd startDate{};
        EpgHtmlLite::LiteDateYmd baseDate{};

        int startMinutes = -1;
        int endMinutes = -1;

        EpgHtmlLite::ParseLiteDate(row.startDate, startDate);
        EpgHtmlLite::ParseLiteDate(baseBroadcastDate, baseDate);

        EpgHtmlLite::ParseLiteTimeMinutes(row.startTime, startMinutes);
        EpgHtmlLite::ParseLiteTimeMinutes(row.endTime, endMinutes);

        const EpgHtmlLite::LiteDateYmd broadcastDate =
            EpgHtmlLite::CalcLiteBroadcastDate(startDate, startMinutes);

        row.startMinutesOfDay = startMinutes;
        row.endMinutesOfDay = endMinutes;
        row.timeBlock = EpgHtmlLite::CalcLiteTimeBlock(startMinutes);

        row.broadcastDate = EpgHtmlLite::FormatLiteDateYmd(broadcastDate);
        row.broadcastDateKey = EpgHtmlLite::FormatLiteDateCompact(broadcastDate);

        row.dayIndex = EpgHtmlLite::LiteDaysBetween(baseDate, broadcastDate);

        row.isPast = (row.dayIndex < 0);
    }

    static void AppendLiteEntriesFromMergedEpg_(
        EpgHtmlLite::SharedData& dst,
        const std::filesystem::path& mergedEpgPath,
        int space,
        const std::vector<std::vector<EpgHtmlPortal::ServiceEntry>>& enabledServicesBySpace,
        const std::wstring& baseBroadcastDate,
        int futureDays,
        int pastKeepDays)
    {
        dst.entries.clear();

        if (mergedEpgPath.empty() || !std::filesystem::exists(mergedEpgPath)) {
            DebugLogFormat(L"[EPG][LITE][WARN] merged epg missing: %ls", mergedEpgPath.wstring().c_str());
            return;
        }

        std::wstring text;
        if (!EpgHtmlPortal_Common::ReadTextFileToWide(mergedEpgPath, text)) {
            DebugLogFormat(L"[EPG][LITE][WARN] merged epg read failed: %ls", mergedEpgPath.wstring().c_str());
            return;
        }

        const auto serviceNameMap = RsvEpgBuildServiceNameMap_(enabledServicesBySpace, space);

        struct LiteMergedEvent_
        {
            std::uint16_t networkId = 0;
            std::uint16_t tsId = 0;
            std::uint16_t serviceId = 0;
            std::uint16_t eventId = 0;
            std::wstring startDate;
            std::wstring startTime;
            std::wstring endDate;
            std::wstring endTime;
            std::uint32_t durationSec = 0;
            std::wstring title;
            std::wstring summary;
            std::wstring genreKey = L"all";
        };

        std::unordered_set<std::wstring> seen;
        seen.reserve(dst.entries.size() + 256);
        for (const auto& e : dst.entries) {
            seen.insert(e.reserveKey);
        }

        std::size_t appended = 0;
        std::size_t skippedNoTitle = 0;
        int sortOrder = static_cast<int>(dst.entries.size());

        LiteMergedEvent_ cur{};
        bool hasEvent = false;

        auto flush = [&]() {
            if (!hasEvent) {
                return;
            }

            if (cur.title.empty()) {
                ++skippedNoTitle;
                cur = LiteMergedEvent_{};
                hasEvent = false;
                return;
            }

            if ((cur.endDate.empty() || cur.endTime.empty()) &&
                cur.durationSec > 0 &&
                !cur.startDate.empty() &&
                !cur.startTime.empty()) {
                std::wstring endDate;
                std::wstring endTime;
                if (RsvEpgAddSecondsToIsoDateTime_(cur.startDate, cur.startTime, cur.durationSec, endDate, endTime)) {
                    cur.endDate = endDate;
                    cur.endTime = endTime;
                }
            }

            EpgHtmlLite::ProgramEntry row{};
            row.title = cur.title;
            row.description = cur.summary;
            row.startDate = cur.startDate;
            row.startTime = cur.startTime;
            row.endDate = cur.endDate;
            row.endTime = cur.endTime;
            row.startText = LiteTrimHHMM_(cur.startTime);
            row.endText = LiteTrimHHMM_(cur.endTime);
            row.genreKey = cur.genreKey.empty() ? L"all" : cur.genreKey;
            row.broadcastType = MakeBroadcastTypeLabel_(space);
            row.reserveState = L"none";
            row.serviceId = cur.serviceId;
            row.eventId = cur.eventId;
            row.space = space;
            row.sortOrder = sortOrder++;

            NormalizeLiteProgramDate_(row, baseBroadcastDate);

            // Lite表示に使わない未来データ・古すぎる過去データはHTML生成前に落とす。
            if (row.dayIndex >= futureDays) {
                cur = LiteMergedEvent_{};
                hasEvent = false;
                return;
            }
            if (row.dayIndex < -pastKeepDays) {
                cur = LiteMergedEvent_{};
                hasEvent = false;
                return;
            }

            const auto serviceIt = serviceNameMap.find(cur.serviceId);
            if (serviceIt != serviceNameMap.end() && !serviceIt->second.empty()) {
                row.serviceName = serviceIt->second;
            }
            else if (cur.serviceId != 0) {
                row.serviceName = L"SID " + std::to_wstring(cur.serviceId);
            }
            else {
                row.serviceName = L"不明サービス";
            }

            row.reserveKey = MakeReserveKey_(row);
            if (seen.insert(row.reserveKey).second) {
                dst.entries.push_back(std::move(row));
                ++appended;
            }

            cur = LiteMergedEvent_{};
            hasEvent = false;
            };

        std::wstringstream ss(text);
        std::wstring line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == L'\r') {
                line.pop_back();
            }

            const std::wstring trimmed = EpgHtmlPortal_Common::TrimW(line);
            if (trimmed.empty()) {
                flush();
                continue;
            }

            if (trimmed.rfind(L"TableID:", 0) == 0) {
                flush();
                hasEvent = true;
                cur = LiteMergedEvent_{};
                cur.networkId = static_cast<std::uint16_t>(RsvEpgFindNumberAny_(trimmed, { L"OriginalNetworkID", L"NetworkID", L"ONID" }));
                cur.tsId = static_cast<std::uint16_t>(RsvEpgFindNumberAny_(trimmed, { L"TransportStreamID", L"TSID", L"TsId" }));
                cur.serviceId = static_cast<std::uint16_t>(RsvEpgFindNumberAny_(trimmed, { L"ServiceID", L"ServiceId", L"SID" }));
                cur.eventId = static_cast<std::uint16_t>(RsvEpgFindNumberAny_(trimmed, { L"EventID", L"EventId", L"EID" }));
                continue;
            }

            if (!hasEvent) {
                continue;
            }

            const int serviceId = RsvEpgFindNumberAny_(trimmed, { L"ServiceID", L"ServiceId", L"SID" });
            if (serviceId > 0) {
                cur.serviceId = static_cast<std::uint16_t>(serviceId);
            }

            const int eventId = RsvEpgFindNumberAny_(trimmed, { L"EventID", L"EventId", L"EID" });
            if (eventId > 0) {
                cur.eventId = static_cast<std::uint16_t>(eventId);
            }

            if (trimmed.rfind(L"StartTime(raw MJD+BCD40bit):", 0) == 0) {
                std::wstring startDate;
                std::wstring weekday;
                std::wstring startTime;
                if (RsvEpgParseStartHexLine_(trimmed, startDate, weekday, startTime)) {
                    cur.startDate = startDate;
                    cur.startTime = startTime;
                }
                continue;
            }

            if (trimmed.rfind(L"Duration(HH:MM:SS):", 0) == 0) {
                std::uint32_t durationSec = 0;
                if (RsvEpgParseDurationLine_(trimmed, durationSec)) {
                    cur.durationSec = durationSec;
                }
                continue;
            }

            if (trimmed.rfind(L"[短形式タイトル]", 0) == 0) {
                cur.title = RsvEpgExtractAfterPrefix_(trimmed, L"[短形式タイトル]");
                continue;
            }

            if (trimmed.rfind(L"[短形式概要]", 0) == 0) {
                cur.summary = RsvEpgExtractAfterPrefix_(trimmed, L"[短形式概要]");
                continue;
            }

            if (trimmed.rfind(L"[ジャンル]", 0) == 0) {
                const std::size_t pos = trimmed.find(L"primary=0x");
                if (pos != std::wstring::npos && pos + 10 < trimmed.size()) {
                    const wchar_t c = trimmed[pos + 10];
                    int primary = 0;
                    if (c >= L'0' && c <= L'9') primary = c - L'0';
                    else if (c >= L'A' && c <= L'F') primary = 10 + (c - L'A');
                    else if (c >= L'a' && c <= L'f') primary = 10 + (c - L'a');
                    cur.genreKey = GenreKeyFromMajor_(static_cast<std::uint8_t>(primary));
                }
                continue;
            }

            if (trimmed.rfind(L"(", 0) == 0) {
                const std::size_t colon = trimmed.find(L':');
                if (colon != std::wstring::npos) {
                    const std::wstring ext = EpgHtmlPortal_Common::TrimW(std::wstring(trimmed.substr(colon + 1)));
                    if (!ext.empty()) {
                        if (cur.summary.empty()) cur.summary = ext;
                        else if (cur.summary.find(ext) == std::wstring::npos) cur.summary += L" / " + ext;
                    }
                }
                continue;
            }
        }
        flush();

        DebugLogFormat(
            L"[EPG][LITE] merged epg parsed: appended=%zu total=%zu skippedNoTitle=%zu path=%ls",
            appended,
            dst.entries.size(),
            skippedNoTitle,
            mergedEpgPath.wstring().c_str());
    }

    template <class Ev>
    static void AppendLiteEntriesFromEvents_(
        EpgHtmlLite::SharedData& dst,
        const std::vector<Ev>& events,
        int space,
        const std::wstring& fallbackServiceName)
    {
        std::unordered_set<std::wstring> seen;
        seen.reserve(dst.entries.size() + events.size() + 8);

        for (const auto& e : dst.entries) {
            seen.insert(e.reserveKey);
        }

        int sortOrder = static_cast<int>(dst.entries.size());

        for (const auto& ev : events) {
            EpgHtmlLite::ProgramEntry row{};

            row.title = ExtractTitleFromEvent_(ev);
            if (row.title.empty()) {
                continue;
            }

            row.serviceName = ExtractServiceNameFromEvent_(ev);
            if (row.serviceName.empty()) {
                row.serviceName = fallbackServiceName;
            }

            row.description = ExtractDescriptionFromEvent_(ev);
            row.startText = ExtractStartTextFromEvent_(ev);
            row.endText = ExtractEndTextFromEvent_(ev);

            row.genreKey = ExtractGenreKeyFromEvent_(ev);
            row.broadcastType = MakeBroadcastTypeLabel_(space);
            row.reserveState = L"none";

            row.serviceId = ExtractServiceIdFromEvent_(ev);
            row.eventId = ExtractEventIdFromEvent_(ev);
            row.space = space;
            row.isPast = false;
            row.sortOrder = sortOrder++;

            row.reserveKey = MakeReserveKey_(row);

            if (!seen.insert(row.reserveKey).second) {
                continue;
            }

            dst.entries.push_back(std::move(row));
        }
    }

    static bool TryGenerateWithLite_(
        const Options& o,
        const std::wstring& mergedEpgPath,
        const std::wstring& channelListPath,
        const EpgHtmlLite::SharedData* sharedData,
        std::wstring& outIndexPath)
    {
        const auto diagStart =
            std::chrono::steady_clock::now();

        const std::size_t entryCount =
            sharedData != nullptr
            ? sharedData->entries.size()
            : 0;

        DebugLogFormat(
            L"[EPG][DIAG][HTML][LITE-BEGIN] route=%ls space=%d entries=%zu outputDir=%ls merged=%ls chlist=%ls",
            ToRouteName(o.htmlRenderRoute),
            o.space,
            entryCount,
            o.outputDir.c_str(),
            mergedEpgPath.c_str(),
            channelListPath.c_str());

        EpgHtmlLite::Context liteCtx{};
        liteCtx.mergedEpgPath = mergedEpgPath;
        liteCtx.channelListPath = channelListPath;
        liteCtx.outputDir = std::filesystem::path(o.outputDir);
        liteCtx.title = o.htmlTitle.empty() ? L"EPG" : o.htmlTitle;
        liteCtx.space = o.space;
        liteCtx.indexFileName =
            o.topIndexFileName.empty()
            ? L"index.html"
            : o.topIndexFileName;
        liteCtx.baseBroadcastDate = MakeLiteBaseBroadcastDateToday_();
        liteCtx.futureDays = 8;
        liteCtx.pastKeepDays = 30;
        liteCtx.sharedData = sharedData;

        EpgHtmlLite::Result liteRes{};
        const bool ok =
            EpgHtmlLite::Generate(liteCtx, &liteRes);

        if (!ok) {
            const auto elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - diagStart).count();

            DebugLogFormat(
                L"[EPG][DIAG][HTML][LITE-END] ok=0 elapsedMs=%lld files=%zu index=%ls",
                static_cast<long long>(elapsedMs),
                liteRes.generatedFiles.size(),
                liteRes.indexPath.c_str());

            return false;
        }

        if (!liteRes.indexPath.empty()) {
            outIndexPath = liteRes.indexPath;
        }
        else {
            outIndexPath =
                (std::filesystem::path(o.outputDir) /
                    std::filesystem::path(liteCtx.indexFileName)).wstring();
        }

        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - diagStart).count();

        DebugLogFormat(
            L"[EPG][DIAG][HTML][LITE-END] ok=1 elapsedMs=%lld entries=%zu files=%zu index=%ls",
            static_cast<long long>(elapsedMs),
            entryCount,
            liteRes.generatedFiles.size(),
            outIndexPath.c_str());

        return true;
    }



    void HtmlRendererHost::Reset() noexcept { Unload(); }

    bool HtmlRendererHost::Load(const std::wstring& dllPath)
    {
        Unload();

        HMODULE hm = ::LoadLibraryW(dllPath.c_str());
        if (!hm) {
            DebugLogFormat(L"[EPG-HTML][WARN] LoadLibraryW failed: %ls", dllPath.c_str());
            return false;
        }
        hmod_ = reinterpret_cast<void*>(hm);

        auto p = reinterpret_cast<CreateFn>(::GetProcAddress(hm, "CreateEpgHtmlRendererPlugin"));
        if (!p) {
            DebugLogFormat(L"[EPG-HTML][WARN] GetProcAddress failed: %ls", dllPath.c_str());
            Unload();
            return false;
        }

        plugin_ = p();
        if (!plugin_) {
            DebugLogFormat(L"[EPG-HTML][WARN] Create returned null: %ls", dllPath.c_str());
            Unload();
            return false;
        }

        DebugLogFormat(L"[EPG-HTML] plugin loaded: %ls", plugin_->GetPluginName());
        return true;
    }

    void HtmlRendererHost::Unload() noexcept
    {
        if (plugin_) {
            plugin_->Release();
            plugin_ = nullptr;
        }
        if (hmod_) {
            ::FreeLibrary(reinterpret_cast<HMODULE>(hmod_));
            hmod_ = nullptr;
        }
    }

    bool HtmlRendererHost::Generate(const HtmlContext& ctx)
    {
        DebugLogFormat(
            L"[EPG-HTML] route=%ls plugin=%d out=%ls",
            ToRouteName(ctx.renderRoute),
            plugin_ ? 1 : 0,
            ctx.outputHtmlPath.c_str());

        switch (ctx.renderRoute) {
        case HtmlRenderRoute::FixedLegacy:
            return GenerateEITHtmlProgramTable(
                ctx.mergedEpgPath,
                ctx.channelListPath,
                ctx.outputHtmlPath);

        case HtmlRenderRoute::FixedProgramPageWriter:
            return TryGenerateWithProgramPageWriter_(ctx);

        case HtmlRenderRoute::FixedPlugin:
            if (plugin_) {
                return plugin_->GenerateHtml(ctx);
            }
            DebugLog(L"[EPG-HTML][WARN] FixedPlugin selected but plugin is not loaded");
            return false;

        case HtmlRenderRoute::AutoPluginThenLegacy:
            if (plugin_) {
                return plugin_->GenerateHtml(ctx);
            }
            return GenerateEITHtmlProgramTable(
                ctx.mergedEpgPath,
                ctx.channelListPath,
                ctx.outputHtmlPath);

        default:
            return GenerateEITHtmlProgramTable(
                ctx.mergedEpgPath,
                ctx.channelListPath,
                ctx.outputHtmlPath);
        }
    }

    bool HtmlRendererHost::HasPlugin() const noexcept { return plugin_ != nullptr; }

    void EpgBuild_GUI::RequestCancel() noexcept { cancel_.RequestCancel(); }
    void EpgBuild_GUI::ResetCancel() noexcept { cancel_.Reset(); }
    bool EpgBuild_GUI::IsCancelled() const noexcept { return cancel_.IsCancelled(); }
    void EpgBuild_GUI::Reset() noexcept { cancel_.Reset(); }

    struct EpgCapturePlan_
    {
        int captureSecondsPerCycle = 120;
        int maxCycles = 2;

        bool existingEpgUsable = false;
        bool shortUpdate = false;
        bool manualMode = false;
        bool reuseExistingOnFailure = false;
    };

    static const wchar_t* ToEpgCaptureModeName_(EpgCaptureMode mode) noexcept
    {
        switch (mode) {
        case EpgCaptureMode::AutoByExistingEpg:
            return L"AutoByExistingEpg";
        case EpgCaptureMode::FixedInitial:
            return L"FixedInitial";
        case EpgCaptureMode::FixedRefresh:
            return L"FixedRefresh";
        case EpgCaptureMode::FixedManual:
            return L"FixedManual";
        default:
            return L"Unknown";
        }
    }

    static const wchar_t* ToStatusName_(Status status) noexcept
    {
        switch (status) {
        case Status::Ok:
            return L"Ok";
        case Status::Cancelled:
            return L"Cancelled";
        case Status::InvalidArgument:
            return L"InvalidArgument";
        case Status::ChannelListMissing:
            return L"ChannelListMissing";
        case Status::NoTargets:
            return L"NoTargets";
        case Status::NoEpgGenerated:
            return L"NoEpgGenerated";
        case Status::MergeFailed:
            return L"MergeFailed";
        case Status::HtmlFailed:
            return L"HtmlFailed";
        default:
            return L"Unknown";
        }
    }

    struct EpgRunEvalLog_
    {
        std::chrono::steady_clock::time_point start{};
        bool active = false;

        void Start(
            const Options& o,
            int space,
            std::size_t targetCount,
            const std::wstring& chListPath)
        {
            start = std::chrono::steady_clock::now();
            active = true;

            DebugLogFormat(
                L"[EPG][EVAL][START] space=%d targets=%zu mode=%ls route=%ls outputDir=%ls merged=%ls html=%ls chlist=%ls",
                space,
                targetCount,
                ToEpgCaptureModeName_(o.captureMode),
                ToRouteName(o.htmlRenderRoute),
                o.outputDir.c_str(),
                o.mergedEpgFileName.c_str(),
                o.htmlFileName.c_str(),
                chListPath.c_str());
        }

        void End(
            const Result& res,
            std::size_t targetCount)
        {
            if (!active) {
                return;
            }

            const auto now = std::chrono::steady_clock::now();
            const auto elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

            DebugLogFormat(
                L"[EPG][EVAL][END] status=%ls processed=%zu/%zu elapsedMs=%lld merged=%ls html=%ls",
                ToStatusName_(res.status),
                res.processedChannels,
                targetCount,
                static_cast<long long>(elapsedMs),
                res.mergedEpgPath.c_str(),
                res.htmlPath.c_str());

            active = false;
        }
    };

    static std::filesystem::path MakePerChannelEpgPath_(
        const Options& o,
        int space,
        int ch)
    {
        std::wostringstream oss;
        oss << o.basePrefix << L"_space" << space << L"_ch" << ch << L"_EIT.epg";
        return std::filesystem::path(o.outputDir) / std::filesystem::path(oss.str());
    }

    static void RunEpgArchiveSaveAfterCapture_(
        const Options& o,
        const std::vector<std::filesystem::path>& perChannelEpg) noexcept
    {
#if EPG_ARCHIVE_SAVE_ENABLE != 0
        try {
            if (perChannelEpg.empty()) {
                DebugLog(L"[EPG][ARCHIVE][SKIP] perChannelEpg empty");
                return;
            }

            const std::filesystem::path oldRootDir =
                std::filesystem::path(o.outputDir) / std::filesystem::path(L"old");

            const auto archiveResult =
                EpgArchiveSave::RunForFiles(
                    perChannelEpg,
                    oldRootDir);

            DebugLogFormat(
                L"[EPG][ARCHIVE][CALL] ok=%d dryRun=%d files=%zu changed=%zu failed=%zu blocks=%zu keep=%zu old=%zu invalidDate=%zu",
                archiveResult.ok ? 1 : 0,
                archiveResult.dryRun ? 1 : 0,
                archiveResult.files,
                archiveResult.changedFiles,
                archiveResult.failedFiles,
                archiveResult.totalBlocks,
                archiveResult.keepBlocks,
                archiveResult.archiveBlocks,
                archiveResult.invalidDateBlocks);
        }
        catch (const std::bad_alloc&) {
            DebugLog(L"[EPG][ARCHIVE][ERROR] bad_alloc");
        }
        catch (const std::exception&) {
            DebugLog(L"[EPG][ARCHIVE][ERROR] std::exception");
        }
        catch (...) {
            DebugLog(L"[EPG][ARCHIVE][ERROR] unknown exception");
        }
#else
        (void)o;
        (void)perChannelEpg;
#endif
    }

    static bool ExistingEpgHasMinimumContent_(
        const std::filesystem::path& epgPath) noexcept;

    static bool IsExistingEpgUsable_(
        const std::filesystem::path& epgPath,
        std::uintmax_t minBytes) noexcept
    {
        try {
            std::error_code ec;

            if (!std::filesystem::exists(epgPath, ec) || ec) {
                return false;
            }

            ec.clear();
            if (!std::filesystem::is_regular_file(epgPath, ec) || ec) {
                return false;
            }

            ec.clear();
            const std::uintmax_t size =
                std::filesystem::file_size(epgPath, ec);

            if (ec) {
                return false;
            }

            if (size < minBytes) {
                return false;
            }

            // サイズだけでは壊れたEPGも更新扱いになるため、
            // 最低限 TableID / ServiceID / EventID を含むか確認する。
            if (!ExistingEpgHasMinimumContent_(epgPath)) {
                return false;
            }

            return true;
        }
        catch (...) {
            return false;
        }
    }

    static int PositiveOrDefault_(int value, int fallback) noexcept
    {
        return value > 0 ? value : fallback;
    }

    static int PositiveOrDefaultMin_(
        int value,
        int fallback,
        int minValue) noexcept
    {
        int v = value > 0 ? value : fallback;

        if (v < minValue) {
            v = minValue;
        }

        return v;
    }

    static bool ExistingEpgHasMinimumContent_(
        const std::filesystem::path& epgPath) noexcept
    {
        try {
            std::ifstream ifs(epgPath, std::ios::binary);
            if (!ifs) {
                return false;
            }

            constexpr std::size_t kProbeBytes = 512 * 1024;
            std::string buf;
            buf.resize(kProbeBytes);

            ifs.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const std::streamsize got = ifs.gcount();

            if (got <= 0) {
                return false;
            }

            buf.resize(static_cast<std::size_t>(got));

            // EPG dump の最低限の構造確認。
            // UTF-8 BOM付きでも ASCII 部分はそのまま検索できる。
            if (buf.find("TableID:") == std::string::npos) {
                return false;
            }

            if (buf.find("ServiceID") == std::string::npos &&
                buf.find("SID") == std::string::npos) {
                return false;
            }

            if (buf.find("EventID") == std::string::npos &&
                buf.find("EID") == std::string::npos) {
                return false;
            }

            return true;
        }
        catch (...) {
            return false;
        }
    }

    static bool PathLooksSpaceEpg_(
        const std::filesystem::path& path,
        int space)
    {
        const std::wstring fileNameLower =
            ToLowerCopy_(path.filename().wstring());

        const std::wstring extLower =
            ToLowerCopy_(path.extension().wstring());

        if (extLower != L".epg") {
            return false;
        }

        const std::wstring marker =
            L"space" + std::to_wstring(space);

        return fileNameLower.find(marker) != std::wstring::npos;
    }

    static std::vector<std::filesystem::path> CollectBeforeArchiveEpgFiles_(
        const Options& o)
    {
        std::vector<std::filesystem::path> files;

        const std::filesystem::path oldRoot =
            std::filesystem::path(o.outputDir) / std::filesystem::path(L"old");

        std::error_code ec;

        if (!std::filesystem::exists(oldRoot, ec) || ec) {
            DebugLogFormat(
                L"[EPG][BEFORE] old root not found: %ls",
                oldRoot.wstring().c_str());
            return files;
        }

        ec.clear();
        if (!std::filesystem::is_directory(oldRoot, ec) || ec) {
            DebugLogFormat(
                L"[EPG][BEFORE][WARN] old root is not directory: %ls",
                oldRoot.wstring().c_str());
            return files;
        }

        std::filesystem::recursive_directory_iterator it(
            oldRoot,
            std::filesystem::directory_options::skip_permission_denied,
            ec);

        const std::filesystem::recursive_directory_iterator end{};

        if (ec) {
            DebugLogFormat(
                L"[EPG][BEFORE][WARN] old root enumerate failed: %ls",
                oldRoot.wstring().c_str());
            return files;
        }

        for (; it != end; it.increment(ec)) {
            if (ec) {
                DebugLogFormat(
                    L"[EPG][BEFORE][WARN] old enumerate skipped by error: %ls",
                    oldRoot.wstring().c_str());
                ec.clear();
                continue;
            }

            const std::filesystem::directory_entry& entry = *it;

            ec.clear();
            if (!entry.is_regular_file(ec) || ec) {
                ec.clear();
                continue;
            }

            const std::filesystem::path p = entry.path();

            if (!PathLooksSpaceEpg_(p, o.space)) {
                continue;
            }

            if (!IsExistingEpgUsable_(p, o.existingEpgMinBytes)) {
                continue;
            }

            files.push_back(p);
        }

        std::sort(files.begin(), files.end());

        DebugLogFormat(
            L"[EPG][BEFORE] old epg files collected: space=%d count=%zu oldRoot=%ls",
            o.space,
            files.size(),
            oldRoot.wstring().c_str());

        return files;
    }

    static EpgCapturePlan_ MakeEpgCapturePlan_(
        const Options& o,
        const std::filesystem::path& epgPath)
    {
        EpgCapturePlan_ plan{};

        plan.existingEpgUsable =
            IsExistingEpgUsable_(epgPath, o.existingEpgMinBytes);

        // 初回取得:
        //   既存EPGが無い/壊れている場合。従来通り 120秒 x 2 を基本。
        //
        // 短縮更新:
        //   既存EPGが使える場合。60秒は短すぎるため、最低120秒 x 1 に丸める。
        //
        // 90秒にしたい場合は kRefreshCaptureSeconds を 90 に変更する。
        constexpr int kInitialCaptureSeconds = 120;
        constexpr int kInitialMaxCycles = 2;

        constexpr int kRefreshCaptureSeconds = 120;
        constexpr int kRefreshMaxCycles = 1;

        constexpr int kManualCaptureSeconds = 120;
        constexpr int kManualMaxCycles = 1;

        switch (o.captureMode) {
        case EpgCaptureMode::AutoByExistingEpg:
            if (plan.existingEpgUsable) {
                plan.captureSecondsPerCycle =
                    PositiveOrDefaultMin_(
                        o.refreshCaptureSecondsPerCycle,
                        kRefreshCaptureSeconds,
                        kRefreshCaptureSeconds);

                plan.maxCycles =
                    PositiveOrDefaultMin_(
                        o.refreshMaxCycles,
                        kRefreshMaxCycles,
                        kRefreshMaxCycles);

                plan.shortUpdate = true;
            }
            else {
                plan.captureSecondsPerCycle =
                    PositiveOrDefault_(
                        o.captureSecondsPerCycle,
                        kInitialCaptureSeconds);

                plan.maxCycles =
                    PositiveOrDefault_(
                        o.maxCycles,
                        kInitialMaxCycles);

                plan.shortUpdate = false;
            }
            break;

        case EpgCaptureMode::FixedInitial:
            plan.captureSecondsPerCycle =
                PositiveOrDefault_(
                    o.captureSecondsPerCycle,
                    kInitialCaptureSeconds);

            plan.maxCycles =
                PositiveOrDefault_(
                    o.maxCycles,
                    kInitialMaxCycles);

            plan.shortUpdate = false;
            break;

        case EpgCaptureMode::FixedRefresh:
            plan.captureSecondsPerCycle =
                PositiveOrDefaultMin_(
                    o.refreshCaptureSecondsPerCycle,
                    kRefreshCaptureSeconds,
                    kRefreshCaptureSeconds);

            plan.maxCycles =
                PositiveOrDefaultMin_(
                    o.refreshMaxCycles,
                    kRefreshMaxCycles,
                    kRefreshMaxCycles);

            plan.shortUpdate = true;
            break;

        case EpgCaptureMode::FixedManual:
            plan.captureSecondsPerCycle =
                PositiveOrDefault_(
                    o.manualCaptureSecondsPerCycle,
                    kManualCaptureSeconds);

            plan.maxCycles =
                PositiveOrDefault_(
                    o.manualMaxCycles,
                    kManualMaxCycles);

            plan.manualMode = true;
            plan.shortUpdate = false;
            break;

        default:
            plan.captureSecondsPerCycle =
                PositiveOrDefault_(
                    o.captureSecondsPerCycle,
                    kInitialCaptureSeconds);

            plan.maxCycles =
                PositiveOrDefault_(
                    o.maxCycles,
                    kInitialMaxCycles);

            plan.shortUpdate = false;
            break;
        }

        plan.reuseExistingOnFailure =
            o.reuseExistingEpgOnShortUpdateFailure &&
            plan.shortUpdate &&
            plan.existingEpgUsable;

        return plan;
    }

    enum class EpgCommitResult_
    {
        Created,
        Appended
    };

    static bool MoveTempToFinalEpg_(
        const std::filesystem::path& tempPath,
        const std::filesystem::path& finalPath) noexcept
    {
        try {
            std::error_code ec;
            std::filesystem::create_directories(finalPath.parent_path(), ec);

#ifdef _WIN32
            const std::wstring temp = tempPath.wstring();
            const std::wstring final = finalPath.wstring();

            const BOOL ok = ::MoveFileExW(
                temp.c_str(),
                final.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

            return ok != FALSE;
#else
            std::filesystem::rename(tempPath, finalPath, ec);
            return !ec;
#endif
        }
        catch (...) {
            return false;
        }
    }

    static bool GetFileSizeSafe_(
        const std::filesystem::path& path,
        std::uintmax_t& outSize) noexcept
    {
        outSize = 0;

        try {
            std::error_code ec;

            if (!std::filesystem::exists(path, ec) || ec) {
                return false;
            }

            ec.clear();
            if (!std::filesystem::is_regular_file(path, ec) || ec) {
                return false;
            }

            ec.clear();
            outSize = std::filesystem::file_size(path, ec);
            return !ec;
        }
        catch (...) {
            outSize = 0;
            return false;
        }
    }

    static bool AppendTempEpgToFinalEpg_(
        const std::filesystem::path& tempPath,
        const std::filesystem::path& finalPath) noexcept
    {
        try {
            std::ifstream ifs(tempPath, std::ios::binary);
            if (!ifs) {
                return false;
            }

            std::uintmax_t finalSize = 0;
            (void)GetFileSizeSafe_(finalPath, finalSize);

            std::ofstream ofs(finalPath, std::ios::binary | std::ios::app);
            if (!ofs) {
                return false;
            }

            // 既存EPGとの境界を必ず作る。
            if (finalSize > 0) {
                ofs.write("\n\n", 2);
                if (!ofs) {
                    return false;
                }
            }

            // .tmp 側に UTF-8 BOM がある場合、EPG途中に BOM が混ざらないように飛ばす。
            unsigned char bom[3]{};
            ifs.read(reinterpret_cast<char*>(bom), 3);

            const bool hasBom =
                ifs.gcount() == 3 &&
                bom[0] == 0xEF &&
                bom[1] == 0xBB &&
                bom[2] == 0xBF;

            if (!hasBom) {
                ifs.clear();
                ifs.seekg(0, std::ios::beg);
                if (!ifs) {
                    return false;
                }
            }

            // C6262対策:
            // 64KBのローカル配列をスタックに置かず、ヒープ側に逃がす。
            constexpr std::size_t kCopyBufferBytes = 64 * 1024;
            std::vector<char> buffer(kCopyBufferBytes);

            while (ifs) {
                ifs.read(
                    buffer.data(),
                    static_cast<std::streamsize>(buffer.size()));

                const std::streamsize got = ifs.gcount();

                if (got > 0) {
                    ofs.write(buffer.data(), got);
                    if (!ofs) {
                        return false;
                    }
                }
            }

            ofs.write("\n", 1);
            ofs.flush();

            return ofs.good();
        }
        catch (...) {
            return false;
        }
    }

    static bool CommitTempEpgByAppendRule_(
        const std::filesystem::path& tempPath,
        const std::filesystem::path& finalPath,
        std::uintmax_t existingEpgMinBytes,
        EpgCommitResult_& outResult) noexcept
    {
        outResult = EpgCommitResult_::Created;

        std::uintmax_t tempSize = 0;
        if (!GetFileSizeSafe_(tempPath, tempSize) || tempSize == 0) {
            return false;
        }

        const bool existingUsable =
            IsExistingEpgUsable_(finalPath, existingEpgMinBytes);

        if (!existingUsable) {
            // 初回、または壊れた小さいEPGは生成扱い。
            std::error_code ec;
            std::filesystem::remove(finalPath, ec);

            if (!MoveTempToFinalEpg_(tempPath, finalPath)) {
                return false;
            }

            outResult = EpgCommitResult_::Created;
            return true;
        }

        // 既存EPGがある場合は、成功した取得データを必ず追記する。
        if (!AppendTempEpgToFinalEpg_(tempPath, finalPath)) {
            return false;
        }

        {
            std::error_code ec;
            std::filesystem::remove(tempPath, ec);
        }

        outResult = EpgCommitResult_::Appended;
        return true;
    }

    Result EpgBuild_GUI::Run(IBonDriver2* bon,
        FilterManagerMode mode,
        const Options& opt,
        const Callbacks* cb)
    {
        Result res{};
        if (!bon) {
            SafeLog(cb, L"[ERROR][EPG] BonDriverが無効");
            DebugLog(L"[ERROR][EPG] BonDriver null");
            res.status = Status::InvalidArgument;
            return res;
        }

        Options o = opt;
        const int space = o.space;

        {
            std::wstring bonLog =
                L"[EPG][BonDriver] run space=" +
                std::to_wstring(space) +
                L" ";

            bonLog += MakeSpaceLabel_(space);
            bonLog += L" path=";

            if (o.bonDriverPathForLog.empty()) {
                bonLog += L"(not supplied by caller)";
            }
            else {
                bonLog += o.bonDriverPathForLog;
            }

            DebugLog(bonLog.c_str());
            SafeLog(cb, bonLog);
        }

        const std::wstring resolvedConfigIniPath = o.useEnabledServicesFromConfig
            ? ResolveConfigIniPath(o.configIniPath, o.outputDir)
            : std::wstring{};

        if (o.useEnabledServicesFromConfig) {
            if (!resolvedConfigIniPath.empty()) {
                DebugLogFormat(L"[EPG] resolved config.ini: %ls", resolvedConfigIniPath.c_str());
            }
            else {
                DebugLogFormat(L"[EPG][WARN] config.ini not found: raw=%ls", o.configIniPath.c_str());
            }
        }

        if (o.useEnabledServicesFromConfig && !resolvedConfigIniPath.empty()) {
            ConfigManager& cfg = ConfigManager::GetInstance();

            // 既に外側から明示指定していない場合は config.ini の値を採用
            if (o.htmlRenderRoute == HtmlRenderRoute::FixedLegacy) {
                o.htmlRenderRoute = ConvertConfigHtmlRenderRoute_(cfg.GetEpgHtmlRenderRoute());

                DebugLogFormat(
                    L"[EPG] htmlRenderRoute <- config.ini : %ls",
                    ToRouteName(o.htmlRenderRoute));
            }
        }

        const std::wstring chListPath = ResolveChannelListPath(space, o.channelListPathOverride);
        res.channelListPath = chListPath;

        if (!std::filesystem::exists(std::filesystem::path(chListPath))) {
            SafeLog(cb, L"[ERROR][EPG] channellistが存在しません（先にチャンネルスキャン実行が必要）");
            DebugLogFormat(L"[ERROR][EPG] channellist missing: %ls", chListPath.c_str());
            res.status = Status::ChannelListMissing;
            return res;
        }

        if (!o.enableEitDump) {
            SafeLog(cb, L"[ERROR][EPG] enableEitDump=0 のままでは HTML 生成を継続できません");
            DebugLog(L"[ERROR][EPG] enableEitDump is false");
            res.status = Status::InvalidArgument;
            return res;
        }

        std::filesystem::create_directories(std::filesystem::path(o.outputDir));

        std::vector<int> targets = o.selectedPhysicalChannels;

        if (targets.empty() && o.useEnabledServicesFromConfig && !resolvedConfigIniPath.empty()) {
            auto fromIni = LoadEnabledPhysicalChannelsFromConfigIni(resolvedConfigIniPath, space);
            if (!fromIni.empty()) {
                targets = std::move(fromIni);
                DebugLogFormat(L"[EPG] targets from config.ini: %zu", targets.size());
                SafeLog(cb, L"[EPG] 対象chは config.ini の有効サービスから抽出");
            }
        }

        if (targets.empty()) {
            targets = LoadUniquePhysicalChannelsFromChList_Strict(chListPath, space);
        }

        if (targets.empty()) {
            SafeLog(cb, L"[WARN][EPG] 対象チャンネルが0件");
            DebugLog(L"[WARN][EPG] no targets");
            res.status = Status::NoTargets;
            return res;
        }

        auto enabledServicesBySpace =
            BuildEnabledSearchServicesBySpace_(o, resolvedConfigIniPath);

        for (int sp = 0; sp <= 2; ++sp) {
            DebugLogFormat(
                L"[EPG] final search services for space=%d : %zu",
                sp,
                enabledServicesBySpace[static_cast<std::size_t>(sp)].size());
        }

        res.targetChannels = targets.size();

        if (o.mergedEpgFileName.empty()) {
            o.mergedEpgFileName = L"EpgMerged_space" + std::to_wstring(space) + L"_All_EIT.epg";
        }
        if (o.htmlFileName.empty()) {
            o.htmlFileName = L"epg_space" + std::to_wstring(space) + L".html";
        }
        if (o.beforeHtmlFileName.empty()) {
            o.beforeHtmlFileName = L"epg_space" + std::to_wstring(space) + L"_before.html";
        }

        SafeLog(cb, L"[EPG] start");
        DebugLogFormat(L"[EPG] start: space=%d targets=%zu chlist=%ls", space, targets.size(), chListPath.c_str());

        EpgRunEvalLog_ evalLog;
        evalLog.Start(o, space, targets.size(), chListPath);

        const auto epgDiagStart =
            std::chrono::steady_clock::now();

        EpgDiagLogPhase_(
            L"run-start",
            o,
            bon,
            epgDiagStart);

        EpgDiagLogTunerConfigSnapshot_(
            L"run-start");

        std::vector<std::filesystem::path> perChannelEpg;
        perChannelEpg.reserve(targets.size());
        EpgHtmlLite::SharedData liteSharedData{};

        // Lite版は EIT の生イベントから直接 sharedData を作らない。
        // 必ず、各chの .epg をマージした EpgMerged_spaceX_All_EIT.epg を読み直して構築する。
        // これにより、重いJS版と同じ「EPGファイル由来」のデータを参照でき、
        // ParsedEITPacket のフィールド差異で Lite だけ空になる事故を避ける。
        g_LiteSharedData_ = nullptr;
        LiteSharedDataGuard_ liteSharedGuard{};

        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (cancel_.IsCancelled()) {
                res.status = Status::Cancelled;
                SafeLog(cb, L"[EPG] cancelled");
                DebugLog(L"[EPG] cancelled");
                return res;
            }

            const int ch = targets[i];
            const auto nm = bon->EnumChannelName(space, ch);
            const std::wstring chNameSafe = nm ? SanitizeForLog(nm)
                : (L"(ch=" + std::to_wstring(ch) + L")");

            SafeChannelStart(cb, space, ch, chNameSafe);
            DebugLogFormat(L"[EPG] channel start: space=%d ch=%d %ls", space, ch, chNameSafe.c_str());

            const std::filesystem::path existingEpgPath =
                MakePerChannelEpgPath_(o, space, ch);

            const EpgCapturePlan_ capturePlanForFallback =
                MakeEpgCapturePlan_(o, existingEpgPath);

            std::filesystem::path outEpgPath;
            const bool ok = ProcessOneChannel(bon, mode, o, ch, chNameSafe, cb, outEpgPath);

            if (ok && !outEpgPath.empty()) {
                perChannelEpg.push_back(outEpgPath);
                ++res.processedChannels;
            }
            else if (capturePlanForFallback.reuseExistingOnFailure) {
                perChannelEpg.push_back(existingEpgPath);
                ++res.processedChannels;

                DebugLogFormat(
                    L"[EPG][CAPTURE][WARN] short update failed, reuse existing epg: mode=%ls space=%d ch=%d path=%ls",
                    ToEpgCaptureModeName_(o.captureMode),
                    space,
                    ch,
                    existingEpgPath.wstring().c_str());

                SafeLog(cb, L"[EPG] 短縮更新に失敗したため既存EPGを再利用");
            }

            SafeOverall(cb, i + 1, targets.size());
        }

        if (perChannelEpg.empty()) {
            SafeLog(cb, L"[ERROR][EPG] 生成された.epgが0件");
            DebugLog(L"[ERROR][EPG] no epg generated");
            res.status = Status::NoEpgGenerated;
            return res;
        }

        EpgDiagLogPhase_(
            L"capture-loop-end-before-archive",
            o,
            bon,
            epgDiagStart);

        DebugLogFormat(
            L"[EPG][DIAG][CAPTURE] route=%ls perChannelEpg=%zu processed=%zu target=%zu bon=%p",
            ToRouteName(o.htmlRenderRoute),
            perChannelEpg.size(),
            res.processedChannels,
            targets.size(),
            static_cast<void*>(bon));

        for (const auto& epgPath : perChannelEpg) {
            EpgDiagLogFile_(
                L"capture-loop-end",
                L"per-channel-epg",
                epgPath);
        }

        // ------------------------------------------------------------
        // EPG archive maintenance
        //
        // チャンネル別EPGが揃った後、merged EPG を作る前に評価する。
        // DRY_RUN=1 の間はログ評価のみで、ファイルは変更されない。
        //
        // DRY_RUN=0 の場合:
        //   チャンネル別EPGが compact される
        //   old 対象は epg_out\old\... に分離される
        //   その後の MergeEpgFiles は compact 後の現役EPGを読む
        // ------------------------------------------------------------
        EpgDiagLogPhase_(
            L"archive-begin",
            o,
            bon,
            epgDiagStart);

        RunEpgArchiveSaveAfterCapture_(o, perChannelEpg);

        EpgDiagLogPhase_(
            L"archive-end",
            o,
            bon,
            epgDiagStart);

        const std::filesystem::path mergedPath =
            std::filesystem::path(o.outputDir) / std::filesystem::path(o.mergedEpgFileName);

        if (o.beforeMergedEpgFileName.empty()) {
            o.beforeMergedEpgFileName =
                L"EpgMerged_space" + std::to_wstring(space) + L"_Before_EIT.epg";
        }

        const std::filesystem::path beforeMergedPath =
            std::filesystem::path(o.outputDir) /
            std::filesystem::path(o.beforeMergedEpgFileName);

        

        if (!MergeEpgFiles(perChannelEpg, mergedPath)) {
            SafeLog(cb, L"[ERROR][EPG] マージ失敗");
            DebugLogFormat(
                L"[ERROR][EPG] merge failed: %ls",
                mergedPath.wstring().c_str());

            res.status = Status::MergeFailed;
            evalLog.End(res, targets.size());
            return res;
        }

        EpgDiagLogPhase_(
            L"merge-current-end",
            o,
            bon,
            epgDiagStart);

        EpgDiagLogFile_(
            L"merge-current-end",
            L"merged-current",
            mergedPath);

        res.mergedEpgPath = mergedPath.wstring();

        {
            const auto beforeInputs =
                CollectBeforeArchiveEpgFiles_(o);

            if (!beforeInputs.empty()) {
                if (MergeEpgFiles(beforeInputs, beforeMergedPath)) {
                    DebugLogFormat(
                        L"[EPG][BEFORE] before merged epg generated: %ls inputs=%zu",
                        beforeMergedPath.wstring().c_str(),
                        beforeInputs.size());

                    EpgDiagLogFile_(
                        L"merge-before-end",
                        L"merged-before",
                        beforeMergedPath);
                }
                else {
                    DebugLogFormat(
                        L"[EPG][BEFORE][WARN] before merged epg generation failed: %ls inputs=%zu",
                        beforeMergedPath.wstring().c_str(),
                        beforeInputs.size());

                    EpgDiagLogFile_(
                        L"merge-before-failed",
                        L"merged-before",
                        beforeMergedPath);
                }
            }
            else {
                DebugLogFormat(
                    L"[EPG][BEFORE] old epg files not found. before split skipped: %ls",
                    (std::filesystem::path(o.outputDir) / L"old").wstring().c_str());

                EpgDiagLogFile_(
                    L"merge-before-skipped",
                    L"merged-before",
                    beforeMergedPath);
            }
        }

        if (cancel_.IsCancelled()) {
            res.status = Status::Cancelled;
            return res;
        }

        const std::filesystem::path htmlPath =
            std::filesystem::path(o.outputDir) / std::filesystem::path(o.htmlFileName);

        EpgDiagLogPhase_(
            L"html-dispatch-begin",
            o,
            bon,
            epgDiagStart);

        if (o.htmlRenderRoute == HtmlRenderRoute::FixedLite) {
            const std::wstring liteBaseBroadcastDate =
                MakeLiteBaseBroadcastDateToday_();

            constexpr int kLiteFutureDays = 8;
            constexpr int kLitePastKeepDays = 30;

            EpgDiagLogPhase_(
                L"lite-shareddata-begin",
                o,
                bon,
                epgDiagStart);

            DebugLogFormat(
                L"[EPG][DIAG][LITE-SHARED][BEGIN] route=%ls bon=%p merged=%ls baseDate=%ls futureDays=%d pastKeepDays=%d",
                ToRouteName(o.htmlRenderRoute),
                static_cast<void*>(bon),
                mergedPath.wstring().c_str(),
                liteBaseBroadcastDate.c_str(),
                kLiteFutureDays,
                kLitePastKeepDays);

            AppendLiteEntriesFromMergedEpg_(
                liteSharedData,
                mergedPath,
                space,
                enabledServicesBySpace,
                liteBaseBroadcastDate,
                kLiteFutureDays,
                kLitePastKeepDays);

            EpgDiagLogPhase_(
                L"lite-shareddata-end",
                o,
                bon,
                epgDiagStart);

            DebugLogFormat(
                L"[EPG][DIAG][LITE-SHARED][END] route=%ls bon=%p entries=%zu source=merged-epg-only",
                ToRouteName(o.htmlRenderRoute),
                static_cast<void*>(bon),
                liteSharedData.entries.size());

            DebugLogFormat(
                L"[EPG][LITE] final shared entries before html: total=%zu source=merged-epg-only",
                liteSharedData.entries.size());

            EpgDiagLogPhase_(
                L"html-lite-begin",
                o,
                bon,
                epgDiagStart);

            std::wstring liteIndexPath;
            if (!TryGenerateWithLite_(
                o,
                res.mergedEpgPath,
                chListPath,
                &liteSharedData,
                liteIndexPath)) {
                EpgDiagLogPhase_(
                    L"html-lite-failed",
                    o,
                    bon,
                    epgDiagStart);

                SafeLog(cb, L"[ERROR][EPG] 軽量版HTML生成失敗");
                DebugLog(L"[ERROR][EPG] lite html failed");

                res.status = Status::HtmlFailed;
                evalLog.End(res, targets.size());
                return res;
            }

            res.htmlPath = liteIndexPath;

            EpgDiagLogPhase_(
                L"html-lite-end",
                o,
                bon,
                epgDiagStart);

            DebugLogFormat(
                L"[EPG][DIAG][HTML][LITE-RESULT] ok=1 bon=%p index=%ls entries=%zu",
                static_cast<void*>(bon),
                res.htmlPath.c_str(),
                liteSharedData.entries.size());
        }
        else {
            EpgDiagLogPhase_(
                L"html-portal-host-begin",
                o,
                bon,
                epgDiagStart);

            HtmlRendererHost host;
            if (!o.htmlPluginDllPath.empty()) {
                host.Load(o.htmlPluginDllPath);
            }

            HtmlContext ctx{};
            ctx.mergedEpgPath = res.mergedEpgPath;

            if (IsExistingEpgUsable_(beforeMergedPath, o.existingEpgMinBytes)) {
                ctx.beforeMergedEpgPath = beforeMergedPath.wstring();
            }

            ctx.channelListPath = chListPath;

            // 互換用。旧Legacy / Plugin 用に残す。
            ctx.outputHtmlPath = htmlPath.wstring();
            ctx.beforeHtmlPath =
                (std::filesystem::path(o.outputDir) /
                    std::filesystem::path(o.beforeHtmlFileName)).wstring();

            // Portal親生成ユニット用。
            ctx.space = space;
            ctx.outputDir = o.outputDir;

            ctx.title = o.htmlTitle;
            ctx.pluginSettings = o.htmlPluginSettings;
            ctx.renderRoute = o.htmlRenderRoute;

            EpgDiagLogPhase_(
                L"html-portal-generate-begin",
                o,
                bon,
                epgDiagStart);

            if (!host.Generate(ctx)) {
                EpgDiagLogPhase_(
                    L"html-portal-generate-failed",
                    o,
                    bon,
                    epgDiagStart);

                host.Unload();
                SafeLog(cb, L"[ERROR][EPG] HTML生成失敗");
                DebugLogFormat(
                    L"[ERROR][EPG] html failed: %ls",
                    ctx.outputHtmlPath.c_str());

                res.status = Status::HtmlFailed;
                evalLog.End(res, targets.size());
                return res;
            }

            EpgDiagLogPhase_(
                L"html-portal-generate-end",
                o,
                bon,
                epgDiagStart);

            host.Unload();

            EpgDiagLogPhase_(
                L"html-portal-host-unloaded",
                o,
                bon,
                epgDiagStart);

            res.htmlPath = htmlPath.wstring();

            if (o.enableTopIndexHtml) {
                EpgDiagLogPhase_(
                    L"portal-index-begin",
                    o,
                    bon,
                    epgDiagStart);

                EpgHtmlPortal::WriteOptions portalOpt{};

                const std::filesystem::path portalOutputDir =
                    MakeAbsolutePathSafe_(std::filesystem::path(o.outputDir));

                const std::filesystem::path portalCurrentHtmlPath =
                    MakeAbsolutePathSafe_(htmlPath);

                const std::filesystem::path portalBeforeHtmlPath =
                    MakeOutputPathAbsolute_(o.outputDir, o.beforeHtmlFileName);

                const std::filesystem::path portalMergedPath =
                    MakeAbsolutePathSafe_(mergedPath);

                portalOpt.space = space;
                portalOpt.outputDir = portalOutputDir;

                portalOpt.indexFileName =
                    o.topIndexFileName.empty()
                    ? std::filesystem::path(L"index.html")
                    : std::filesystem::path(o.topIndexFileName);

                portalOpt.currentHtmlPath = portalCurrentHtmlPath;
                portalOpt.beforeHtmlPath = portalBeforeHtmlPath;

                portalOpt.portalTitle =
                    o.htmlTitle.empty()
                    ? std::wstring(L"EPGポータル")
                    : (o.htmlTitle + L" ポータル");

                portalOpt.generateBeforeStubIfMissing =
                    o.generateBeforeHtmlStubIfMissing;

                portalOpt.generateSpaceStubIfMissing = true;

                // 詳細検索の放送局候補用
                portalOpt.enabledServicesBySpace = enabledServicesBySpace;

                // 詳細検索DB用。
                // SearchData 側へ absolute path を渡す。
                // これで epg_out\epg_out\EpgMerged... の二重結合を防ぐ。
                portalOpt.mergedEpgPathsBySpace.assign(
                    3,
                    std::filesystem::path{});

                if (space >= 0 && space <= 2) {
                    portalOpt.mergedEpgPathsBySpace[static_cast<std::size_t>(space)] =
                        portalMergedPath;
                }

                DebugLogFormat(
                    L"[EPG-PORTAL][PATH] outputDir=%ls current=%ls before=%ls merged=%ls",
                    portalOpt.outputDir.wstring().c_str(),
                    portalOpt.currentHtmlPath.wstring().c_str(),
                    portalOpt.beforeHtmlPath.wstring().c_str(),
                    portalMergedPath.wstring().c_str());

                const bool portalOk =
                    EpgHtmlPortal::WritePortalIndexHtml(portalOpt);

                EpgDiagLogPhase_(
                    portalOk ? L"portal-index-end" : L"portal-index-failed",
                    o,
                    bon,
                    epgDiagStart);

                if (!portalOk) {
                    DebugLog(L"[EPG][WARN] index.html の生成に失敗しました。");
                }
                else {
                    EpgDiagLogPhase_(
                        L"reservation-rebuild-begin",
                        o,
                        bon,
                        epgDiagStart);

                    RunReservationEpgRebuildActionAfterPortal_(
                        portalOpt,
                        portalMergedPath,
                        space,
                        enabledServicesBySpace);

                    EpgDiagLogPhase_(
                        L"reservation-rebuild-end",
                        o,
                        bon,
                        epgDiagStart);
                }
            }
        }

        EpgDiagLogPhase_(
            L"html-dispatch-end",
            o,
            bon,
            epgDiagStart);

        RunAutoReserveRegisterAfterEpgBuildSuccess_(
            o,
            bon,
            epgDiagStart,
            mergedPath,
            res.htmlPath,
            space,
            enabledServicesBySpace,
            cb);

        EpgDiagLogPhase_(
            L"run-before-decode-counter-dump",
            o,
            bon,
            epgDiagStart);

        decoder::AribDecoder::DumpDecodeCounters();

        res.status = Status::Ok;
        evalLog.End(res, targets.size());

        EpgDiagLogPhase_(
            L"run-end-ok-before-return",
            o,
            bon,
            epgDiagStart);

        FlushDebugLog();

        SafeLog(cb, L"[EPG] done");
        DebugLog(L"[EPG] done");
        return res;
    }

    bool EpgBuild_GUI::ProcessOneChannel(IBonDriver2* bon,
        FilterManagerMode mode,
        const Options& o,
        int ch,
        const std::wstring& chNameSafe,
        const Callbacks* cb,
        std::filesystem::path& outEpgPath)
    {
        const int space = o.space;

        const std::filesystem::path plannedEpgPath =
            MakePerChannelEpgPath_(o, space, ch);

        const EpgCapturePlan_ capturePlan =
            MakeEpgCapturePlan_(o, plannedEpgPath);

        TSStreamUtils::ResetTsStreamStats();
        TSValidator::ResetStats();

        if (cancel_.IsCancelled()) return false;

        if (!bon->SetChannel(space, ch)) {
            DebugLogFormat(L"[WARN][EPG] SetChannel failed: space=%d ch=%d", space, ch);
            return false;
        }

        if (!TSStreamUtils::WaitForSignalLock(bon)) {
            DebugLogFormat(L"[WARN][EPG] SignalLock failed: space=%d ch=%d", space, ch);
            return false;
        }

        const float level = bon->GetSignalLevel();
        DebugLogFormat(L"[EPG] recv start: space=%d ch=%d (%ls) level=%.1f dB",
            space, ch, chNameSafe.c_str(), level);

        EitEventUpdateUnit updateStore;
        EitEventUpdateUnit::UpdateOptions upOpt{};
        upOpt.enablePreSort = true;
        upOpt.relaxKeyWhenInvalidStartTime = true;

        EpgUpdateBlockMap_ existingEpgBlocks;
        std::vector<ParsedEITPacket> existingSeedEvents;
        bool updateRewriteAvailable = false;

        if (o.enableUpdateMerge &&
            IsExistingEpgUsable_(plannedEpgPath, o.existingEpgMinBytes)) {
            EpgUpdateBlockLoadStats_ seedLoadStats{};

            if (LoadEpgUpdateBlocksAndSeeds_(
                plannedEpgPath,
                existingEpgBlocks,
                &existingSeedEvents,
                seedLoadStats)) {
                EitEventUpdateUnit::UpdateOptions seedOpt = upOpt;

                // 既存EPGはテキストブロックから後勝ち整理済みなので、
                // seed投入時は内部preSortを切る。
                seedOpt.enablePreSort = false;

                const auto seedStats =
                    updateStore.Apply(
                        existingSeedEvents,
                        seedOpt);

                updateRewriteAvailable =
                    seedStats.storeSize > 0 &&
                    !existingEpgBlocks.empty();

                DebugLogFormat(
                    L"[EPG][UPD][SEED] space=%d ch=%d path=%ls rawBlocks=%zu parsedBlocks=%zu duplicatedKeys=%zu seedEvents=%zu added=%llu updated=%llu replaced=%llu serviceTimeReplaced=%llu serviceTimeDropped=%llu store=%llu rewrite=%d",
                    space,
                    ch,
                    plannedEpgPath.wstring().c_str(),
                    seedLoadStats.rawBlocks,
                    seedLoadStats.parsedBlocks,
                    seedLoadStats.duplicatedKeys,
                    existingSeedEvents.size(),
                    static_cast<unsigned long long>(seedStats.added),
                    static_cast<unsigned long long>(seedStats.updated),
                    static_cast<unsigned long long>(seedStats.replaced),
                    static_cast<unsigned long long>(seedStats.serviceTimeReplaced),
                    static_cast<unsigned long long>(seedStats.serviceTimeDroppedIncoming),
                    static_cast<unsigned long long>(seedStats.storeSize),
                    updateRewriteAvailable ? 1 : 0);
            }
            else {
                DebugLogFormat(
                    L"[EPG][UPD][SEED][WARN] existing epg seed load failed: space=%d ch=%d path=%ls",
                    space,
                    ch,
                    plannedEpgPath.wstring().c_str());
            }
        }

        std::vector<ParsedEITPacket> rawEventsAccum;
        std::vector<ParsedEITPacket> updateNewRawEventsForText;
        std::size_t writeOkTotal = 0;
        std::size_t writeFailTotal = 0;
        std::size_t processedTotal = 0;

        constexpr std::size_t kMinEpgRingPackets = 32768;     // 約6MB。短時間の解析遅れを吸収する最低値
        constexpr std::size_t kMaxEpgRingPackets = 262144;    // 約49MB。暴走的な確保を防ぐ上限
        constexpr int kPushStallLimitMs = 30000;              // ring が30秒詰まるなら異常終了
        constexpr int kDrainTotalLimitMs = 30000;             // producer後の吸い切り最大待ち
        constexpr int kDrainIdleLimitMs = 5000;               // 処理数が5秒進まないなら異常終了
        constexpr int kNoTsDataLimitMs = 15000;               // 信号ロック後15秒TSが来ないなら異常終了

        auto NormalizeRingCapacity = [](std::size_t requested) noexcept -> std::size_t {
            std::size_t cap = (std::max)(requested, kMinEpgRingPackets);
            cap = (std::min)(cap, kMaxEpgRingPackets);

            std::size_t pow2 = 1;
            while (pow2 < cap && pow2 < kMaxEpgRingPackets) {
                pow2 <<= 1;
            }
            return (std::min)(pow2, kMaxEpgRingPackets);
            };

        const std::size_t ringCapacity = NormalizeRingCapacity(o.ringBufferSize);
        const int pushSleepUs = (std::max)(o.pushRetrySleepUs, 50);

        DebugLogFormat(
            L"[EPG] stream safety: ringCapacity=%zu requested=%zu pushSleepUs=%d pushStallLimitMs=%d drainLimitMs=%d",
            ringCapacity,
            o.ringBufferSize,
            pushSleepUs,
            kPushStallLimitMs,
            kDrainTotalLimitMs);

        DebugLogFormat(
            L"[EPG][CAPTURE] plan: mode=%ls space=%d ch=%d existing=%d short=%d manual=%d reuseOnFail=%d seconds=%d cycles=%d path=%ls",
            ToEpgCaptureModeName_(o.captureMode),
            space,
            ch,
            capturePlan.existingEpgUsable ? 1 : 0,
            capturePlan.shortUpdate ? 1 : 0,
            capturePlan.manualMode ? 1 : 0,
            capturePlan.reuseExistingOnFailure ? 1 : 0,
            capturePlan.captureSecondsPerCycle,
            capturePlan.maxCycles,
            plannedEpgPath.wstring().c_str());

        for (int cycle = 1; cycle <= capturePlan.maxCycles; ++cycle)
        {
            if (cancel_.IsCancelled()) return false;

            // cycle ごとに作り直して、生EITを cycle 境界で解放する
            TSParser parser;
            FilterManager filterManager(&parser, nullptr);
            filterManager.ApplyMode(mode);

            MPMCRingBuffer<TsPacket> ringBuffer(ringCapacity);

            std::atomic<std::size_t> writeOk(0);
            std::atomic<std::size_t> writeFail(0);
            std::atomic<std::size_t> receiveOkBlocks(0);
            std::atomic<std::size_t> receiveEmptyPolls(0);
            std::atomic<int> producerError(0); // 0=OK, 1=push stall, 2=no TS data, 3=bad_alloc, 4=exception

            auto SetProducerError = [&](int code) noexcept {
                int expected = 0;
                producerError.compare_exchange_strong(expected, code, std::memory_order_acq_rel);
                };

            FilterManagerRunner runner(&filterManager, &ringBuffer);
            runner.Start();

            std::thread producer([&]() {
                try {
                    std::vector<TsPacket> packets;
                    packets.reserve(4096);

                    using clock = std::chrono::steady_clock;
                    const auto captureStart = clock::now();
                    auto lastTsData = captureStart;

                    while (!cancel_.IsCancelled())
                    {
                        const auto now = clock::now();
                        if (std::chrono::duration_cast<std::chrono::seconds>(now - captureStart).count()
                            >= static_cast<long long>((std::max)(capturePlan.captureSecondsPerCycle, 1))) {
                            break;
                        }

                        packets.clear();

                        if (!TSStreamUtils::ReceiveTsStreamBlock(bon, packets)) {
                            receiveEmptyPolls.fetch_add(1, std::memory_order_relaxed);

                            if (std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - lastTsData).count()
                                >= kNoTsDataLimitMs) {
                                SetProducerError(2);
                                DebugLogFormat(
                                    L"[EPG][ERROR] no TS data timeout: space=%d ch=%d cycle=%d emptyPolls=%zu tsFail=%zu",
                                    space,
                                    ch,
                                    cycle,
                                    receiveEmptyPolls.load(std::memory_order_relaxed),
                                    TSStreamUtils::GetTsStreamFailureCount());
                                return;
                            }

                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            continue;
                        }

                        if (packets.empty()) {
                            receiveEmptyPolls.fetch_add(1, std::memory_order_relaxed);
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            continue;
                        }

                        receiveOkBlocks.fetch_add(1, std::memory_order_relaxed);
                        lastTsData = clock::now();

                        for (const auto& pkt : packets) {
                            if (cancel_.IsCancelled()) {
                                return;
                            }

                            const auto pushStart = clock::now();
                            bool pushed = false;

                            while (!cancel_.IsCancelled()) {
                                if (ringBuffer.Push(pkt)) {
                                    pushed = true;
                                    break;
                                }

                                if (std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - pushStart).count()
                                    >= kPushStallLimitMs) {
                                    writeFail.fetch_add(1, std::memory_order_relaxed);
                                    SetProducerError(1);
                                    DebugLogFormat(
                                        L"[EPG][ERROR] ring push stall: space=%d ch=%d cycle=%d pushed=%zu processed=%zu capacity=%zu",
                                        space,
                                        ch,
                                        cycle,
                                        writeOk.load(std::memory_order_relaxed),
                                        filterManager.GetProcessedPacketCount(),
                                        ringCapacity);
                                    return;
                                }

                                std::this_thread::sleep_for(std::chrono::microseconds(pushSleepUs));
                            }

                            if (pushed) {
                                writeOk.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }
                catch (const std::bad_alloc&) {
                    SetProducerError(3);
                    DebugLogFormat(L"[EPG][ERROR] producer bad_alloc: space=%d ch=%d cycle=%d", space, ch, cycle);
                }
                catch (const std::exception&) {
                    SetProducerError(4);
                    DebugLogFormat(L"[EPG][ERROR] producer std::exception: space=%d ch=%d cycle=%d", space, ch, cycle);
                }
                catch (...) {
                    SetProducerError(4);
                    DebugLogFormat(L"[EPG][ERROR] producer unknown exception: space=%d ch=%d cycle=%d", space, ch, cycle);
                }
                });

            producer.join();

            const int producerErrorCode = producerError.load(std::memory_order_acquire);

            bool drainOk = true;
            if (producerErrorCode == 0 && !cancel_.IsCancelled()) {
                using clock = std::chrono::steady_clock;

                const std::size_t expectedQueued = writeOk.load(std::memory_order_relaxed);
                const auto drainStart = clock::now();
                auto lastProgressTime = drainStart;
                std::size_t lastProcessed = filterManager.GetProcessedPacketCount();

                while (!cancel_.IsCancelled()) {
                    const std::size_t curProcessed = filterManager.GetProcessedPacketCount();
                    if (curProcessed >= expectedQueued) {
                        break;
                    }

                    if (curProcessed != lastProcessed) {
                        lastProcessed = curProcessed;
                        lastProgressTime = clock::now();
                    }

                    const auto now = clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - drainStart).count()
                        >= kDrainTotalLimitMs) {
                        drainOk = false;
                        DebugLogFormat(
                            L"[EPG][ERROR] ring drain total timeout: space=%d ch=%d cycle=%d queued=%zu processed=%zu remain=%zu",
                            space,
                            ch,
                            cycle,
                            expectedQueued,
                            curProcessed,
                            expectedQueued > curProcessed ? (expectedQueued - curProcessed) : 0);
                        break;
                    }

                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProgressTime).count()
                        >= kDrainIdleLimitMs) {
                        drainOk = false;
                        DebugLogFormat(
                            L"[EPG][ERROR] ring drain idle timeout: space=%d ch=%d cycle=%d queued=%zu processed=%zu remain=%zu",
                            space,
                            ch,
                            cycle,
                            expectedQueued,
                            curProcessed,
                            expectedQueued > curProcessed ? (expectedQueued - curProcessed) : 0);
                        break;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }

            runner.Stop();

            const std::size_t cyclePushed = writeOk.load(std::memory_order_relaxed);
            const std::size_t cycleWriteFail = writeFail.load(std::memory_order_relaxed);
            const std::size_t cycleProcessed = filterManager.GetProcessedPacketCount();

            writeOkTotal += cyclePushed;
            writeFailTotal += cycleWriteFail;
            processedTotal += cycleProcessed;

            DebugLogFormat(
                L"[EPG][CYCLE] space=%d ch=%d cycle=%d blocks=%zu emptyPolls=%zu pushed=%zu fail=%zu processed=%zu eit=%zu pat=%zu pmt=%zu sdt=%zu nit=%zu null=%zu unmatched=%zu scrambled=%zu",
                space,
                ch,
                cycle,
                receiveOkBlocks.load(std::memory_order_relaxed),
                receiveEmptyPolls.load(std::memory_order_relaxed),
                cyclePushed,
                cycleWriteFail,
                cycleProcessed,
                filterManager.GetEITCount(),
                filterManager.GetPATCount(),
                filterManager.GetPMTCount(),
                filterManager.GetSDTCount(),
                filterManager.GetNITCount(),
                filterManager.GetNullCount(),
                filterManager.GetUnmatchedCount(),
                filterManager.GetScrambledCount());

            if (cancel_.IsCancelled()) {
                return false;
            }

            if (producerErrorCode != 0) {
                DebugLogFormat(
                    L"[EPG][ERROR] producer failed: space=%d ch=%d cycle=%d code=%d pushed=%zu processed=%zu",
                    space,
                    ch,
                    cycle,
                    producerErrorCode,
                    cyclePushed,
                    cycleProcessed);
                return false;
            }

            if (!drainOk) {
                DebugLogFormat(
                    L"[EPG][ERROR] drain failed: space=%d ch=%d cycle=%d pushed=%zu processed=%zu",
                    space,
                    ch,
                    cycle,
                    cyclePushed,
                    cycleProcessed);
                return false;
            }

            if (cyclePushed == 0) {
                DebugLogFormat(L"[EPG][ERROR] no TS packets pushed: space=%d ch=%d cycle=%d", space, ch, cycle);
                return false;
            }

            if (cycleProcessed < cyclePushed) {
                DebugLogFormat(
                    L"[EPG][ERROR] processed packet count shortage: space=%d ch=%d cycle=%d pushed=%zu processed=%zu remain=%zu",
                    space,
                    ch,
                    cycle,
                    cyclePushed,
                    cycleProcessed,
                    cyclePushed - cycleProcessed);
                return false;
            }

            const auto& rawAll = filterManager.GetEITEvents();
            const std::size_t cycleRawCount = rawAll.size();

            if (o.enableUpdateMerge) {
                if (!rawAll.empty()) {
                    try {
                        updateNewRawEventsForText.insert(
                            updateNewRawEventsForText.end(),
                            rawAll.begin(),
                            rawAll.end());

                        const auto ust =
                            updateStore.Apply(
                                rawAll,
                                upOpt);

                        DebugLogFormat(
                            L"[EPG][UPD] space=%d ch=%d cycle=%d delta=%zu added=%llu updated=%llu replaced=%llu serviceTimeReplaced=%llu serviceTimeDropped=%llu filled=%llu store=%llu",
                            space,
                            ch,
                            cycle,
                            cycleRawCount,
                            static_cast<unsigned long long>(ust.added),
                            static_cast<unsigned long long>(ust.updated),
                            static_cast<unsigned long long>(ust.replaced),
                            static_cast<unsigned long long>(ust.serviceTimeReplaced),
                            static_cast<unsigned long long>(ust.serviceTimeDroppedIncoming),
                            static_cast<unsigned long long>(ust.filledFields),
                            static_cast<unsigned long long>(ust.storeSize));
                    }
                    catch (const std::bad_alloc&) {
                        DebugLogFormat(L"[EPG][ERROR] updateStore.Apply bad_alloc: space=%d ch=%d cycle=%d raw=%zu",
                            space, ch, cycle, cycleRawCount);
                        return false;
                    }
                    catch (const std::exception&) {
                        DebugLogFormat(L"[EPG][ERROR] updateStore.Apply exception: space=%d ch=%d cycle=%d raw=%zu",
                            space, ch, cycle, cycleRawCount);
                        return false;
                    }
                    catch (...) {
                        DebugLogFormat(L"[EPG][ERROR] updateStore.Apply unknown exception: space=%d ch=%d cycle=%d raw=%zu",
                            space, ch, cycle, cycleRawCount);
                        return false;
                    }
                }
                else {
                    DebugLogFormat(
                        L"[EPG][UPD] space=%d ch=%d cycle=%d delta=0 added=0 updated=0 replaced=0 serviceTimeReplaced=0 serviceTimeDropped=0 filled=0 store=%llu",
                        space,
                        ch,
                        cycle,
                        static_cast<unsigned long long>(updateStore.Size()));
                }
            }
            else {
                if (!rawAll.empty()) {
                    try {
                        rawEventsAccum.insert(rawEventsAccum.end(), rawAll.begin(), rawAll.end());
                    }
                    catch (const std::bad_alloc&) {
                        DebugLogFormat(L"[EPG][ERROR] rawEventsAccum bad_alloc: space=%d ch=%d cycle=%d raw=%zu accum=%zu",
                            space, ch, cycle, rawAll.size(), rawEventsAccum.size());
                        return false;
                    }
                    catch (...) {
                        DebugLogFormat(L"[EPG][ERROR] rawEventsAccum exception: space=%d ch=%d cycle=%d raw=%zu accum=%zu",
                            space, ch, cycle, rawAll.size(), rawEventsAccum.size());
                        return false;
                    }
                }
            }

            SafeCycle(cb, space, ch, cycle,
                cycleRawCount,
                o.enableUpdateMerge ? updateStore.Size() : rawEventsAccum.size(),
                processedTotal);
        }

        const auto tsStats = TSStreamUtils::GetTsStreamStats();
        DebugLogFormat(
            L"[EPG] push ok=%zu fail=%zu processed=%zu tsCall=%zu tsSuccess=%zu tsFail=%zu tsPackets=%zu failRate=%.4f avgPkt=%.2f",
            writeOkTotal,
            writeFailTotal,
            processedTotal,
            tsStats.callCount,
            tsStats.successCount,
            tsStats.failureCount,
            tsStats.packetCount,
            tsStats.GetFailureRate(),
            tsStats.GetAvgPacketsPerSuccess());

        if (writeFailTotal != 0) {
            DebugLogFormat(L"[EPG][ERROR] TS push failure detected: space=%d ch=%d fail=%zu", space, ch, writeFailTotal);
            return false;
        }

        if (!o.enableEitDump) {
            return true;
        }

        if (o.enableUpdateMerge &&
            updateNewRawEventsForText.empty()) {
            if (updateRewriteAvailable &&
                IsExistingEpgUsable_(plannedEpgPath, o.existingEpgMinBytes)) {
                outEpgPath = plannedEpgPath;

                DebugLogFormat(
                    L"[EPG][UPD][KEEP] no new EIT events. keep existing epg: space=%d ch=%d path=%ls store=%zu",
                    space,
                    ch,
                    plannedEpgPath.wstring().c_str(),
                    updateStore.Size());

                return true;
            }

            DebugLogFormat(
                L"[EPG][UPD][WARN] no new EIT events and no reusable existing epg: space=%d ch=%d",
                space,
                ch);

            return false;
        }

        std::vector<ParsedEITPacket> snapshot;
        try {
            snapshot = o.enableUpdateMerge
                ? updateStore.ExportSnapshotSorted()
                : std::move(rawEventsAccum);
        }
        catch (const std::bad_alloc&) {
            DebugLogFormat(L"[EPG][ERROR] ExportSnapshotSorted bad_alloc: space=%d ch=%d", space, ch);
            return false;
        }
        catch (...) {
            DebugLogFormat(L"[EPG][ERROR] ExportSnapshotSorted exception: space=%d ch=%d", space, ch);
            return false;
        }

        if (snapshot.empty()) {
            DebugLogFormat(L"[WARN][EPG] no events: space=%d ch=%d", space, ch);
            return false;
        }

        std::vector<ParsedEITPacket> events;
        try {
            events = EITEventSortUnit::BuildUniqueSortedEvents(snapshot);
        }
        catch (const std::bad_alloc&) {
            DebugLogFormat(L"[EPG][ERROR] BuildUniqueSortedEvents bad_alloc: space=%d ch=%d snapshot=%zu",
                space, ch, snapshot.size());
            return false;
        }
        catch (...) {
            DebugLogFormat(L"[EPG][ERROR] BuildUniqueSortedEvents exception: space=%d ch=%d snapshot=%zu",
                space, ch, snapshot.size());
            return false;
        }

        // ここで元 snapshot はもう不要なので即解放
        snapshot.clear();
        snapshot.shrink_to_fit();

        if (events.empty()) {
            DebugLogFormat(L"[WARN][EPG] unique=0: space=%d ch=%d", space, ch);
            return false;
        }
        /*{
        const std::filesystem::path outDir(o.outputDir);
        std::wostringstream oss;
        oss << o.basePrefix << L"_space" << space << L"_ch" << ch << L"_EIT.epg";
        const std::filesystem::path epgPath = outDir / std::filesystem::path(oss.str());

        bool writeOk = false;
        try {
            writeOk = WriteEitDumpTextFileFromParsed(epgPath.wstring(), events, space, ch, o.enableAribTrace);
        }
        catch (const std::bad_alloc&) {
            DebugLogFormat(L"[EPG][ERROR] WriteEitDumpTextFileFromParsed bad_alloc: %ls", epgPath.wstring().c_str());
            return false;
        }
        catch (...) {
            DebugLogFormat(L"[EPG][ERROR] WriteEitDumpTextFileFromParsed exception: %ls", epgPath.wstring().c_str());
            return false;
        }

        if (!writeOk) {
            DebugLogFormat(L"[ERROR][EPG] write failed: %ls", epgPath.wstring().c_str());
            return false;
        }

        outEpgPath = epgPath;
        }
*/
        
        const std::filesystem::path epgPath = plannedEpgPath;
        const std::filesystem::path tempEpgPath =
            std::filesystem::path(epgPath.wstring() + L".tmp");

        {
            std::error_code ec;
            std::filesystem::remove(tempEpgPath, ec);
        }

        bool writeOk = false;

        if (o.enableUpdateMerge &&
            updateRewriteAvailable &&
            !updateNewRawEventsForText.empty()) {
            std::vector<ParsedEITPacket> newUniqueEvents;

            try {
                newUniqueEvents =
                    EITEventSortUnit::BuildUniqueSortedEvents(
                        updateNewRawEventsForText);
            }
            catch (const std::bad_alloc&) {
                DebugLogFormat(
                    L"[EPG][ERROR] update new BuildUniqueSortedEvents bad_alloc: space=%d ch=%d raw=%zu",
                    space,
                    ch,
                    updateNewRawEventsForText.size());
                return false;
            }
            catch (...) {
                DebugLogFormat(
                    L"[EPG][ERROR] update new BuildUniqueSortedEvents exception: space=%d ch=%d raw=%zu",
                    space,
                    ch,
                    updateNewRawEventsForText.size());
                return false;
            }

            const std::filesystem::path newOnlyTempPath =
                std::filesystem::path(epgPath.wstring() + L".new.tmp");

            {
                std::error_code ec;
                std::filesystem::remove(newOnlyTempPath, ec);
            }

            bool newWriteOk = false;

            try {
                newWriteOk =
                    WriteEitDumpTextFileFromParsed(
                        newOnlyTempPath.wstring(),
                        newUniqueEvents,
                        space,
                        ch,
                        o.enableAribTrace);
            }
            catch (const std::bad_alloc&) {
                DebugLogFormat(
                    L"[EPG][ERROR] WriteEitDumpTextFileFromParsed new bad_alloc: %ls",
                    newOnlyTempPath.wstring().c_str());
                return false;
            }
            catch (...) {
                DebugLogFormat(
                    L"[EPG][ERROR] WriteEitDumpTextFileFromParsed new exception: %ls",
                    newOnlyTempPath.wstring().c_str());
                return false;
            }

            if (!newWriteOk) {
                DebugLogFormat(
                    L"[ERROR][EPG] new temp write failed: %ls",
                    newOnlyTempPath.wstring().c_str());

                std::error_code ec;
                std::filesystem::remove(newOnlyTempPath, ec);
                return false;
            }

            EpgUpdateBlockMap_ newEpgBlocks;
            EpgUpdateBlockLoadStats_ newLoadStats{};

            if (!LoadEpgUpdateBlocksAndSeeds_(
                newOnlyTempPath,
                newEpgBlocks,
                nullptr,
                newLoadStats)) {
                DebugLogFormat(
                    L"[EPG][ERROR] new temp block load failed: %ls",
                    newOnlyTempPath.wstring().c_str());

                std::error_code ec;
                std::filesystem::remove(newOnlyTempPath, ec);
                return false;
            }

            std::wstring updatedText;
            std::size_t updatedWritten = 0;
            std::size_t updatedMissing = 0;

            if (!BuildUpdatedEpgTextFromSnapshot_(
                events,
                existingEpgBlocks,
                newEpgBlocks,
                updatedText,
                updatedWritten,
                updatedMissing)) {
                DebugLogFormat(
                    L"[EPG][ERROR] updated epg text build failed: space=%d ch=%d finalEvents=%zu existingBlocks=%zu newBlocks=%zu written=%zu missing=%zu",
                    space,
                    ch,
                    events.size(),
                    existingEpgBlocks.size(),
                    newEpgBlocks.size(),
                    updatedWritten,
                    updatedMissing);

                std::error_code ec;
                std::filesystem::remove(newOnlyTempPath, ec);
                return false;
            }

            writeOk =
                WriteWideUtf8File_(
                    tempEpgPath,
                    updatedText);

            {
                std::error_code ec;
                std::filesystem::remove(newOnlyTempPath, ec);
            }

            if (!writeOk) {
                DebugLogFormat(
                    L"[ERROR][EPG] updated rewrite temp write failed: %ls",
                    tempEpgPath.wstring().c_str());

                std::error_code ec;
                std::filesystem::remove(tempEpgPath, ec);
                return false;
            }

            if (!MoveTempToFinalEpg_(
                tempEpgPath,
                epgPath)) {
                DebugLogFormat(
                    L"[ERROR][EPG] updated rewrite commit failed: temp=%ls final=%ls",
                    tempEpgPath.wstring().c_str(),
                    epgPath.wstring().c_str());

                std::error_code ec;
                std::filesystem::remove(tempEpgPath, ec);
                return false;
            }

            DebugLogFormat(
                L"[EPG][COMMIT] rewrite-update: %ls finalEvents=%zu existingBlocks=%zu newBlocks=%zu written=%zu missing=%zu seedRewrite=%d",
                epgPath.wstring().c_str(),
                events.size(),
                existingEpgBlocks.size(),
                newEpgBlocks.size(),
                updatedWritten,
                updatedMissing,
                updateRewriteAvailable ? 1 : 0);
        }
        else {
            try {
                writeOk =
                    WriteEitDumpTextFileFromParsed(
                        tempEpgPath.wstring(),
                        events,
                        space,
                        ch,
                        o.enableAribTrace);
            }
            catch (const std::bad_alloc&) {
                DebugLogFormat(
                    L"[EPG][ERROR] WriteEitDumpTextFileFromParsed bad_alloc: %ls",
                    tempEpgPath.wstring().c_str());
                return false;
            }
            catch (...) {
                DebugLogFormat(
                    L"[EPG][ERROR] WriteEitDumpTextFileFromParsed exception: %ls",
                    tempEpgPath.wstring().c_str());
                return false;
            }

            if (!writeOk) {
                DebugLogFormat(
                    L"[ERROR][EPG] write failed: %ls",
                    tempEpgPath.wstring().c_str());

                std::error_code ec;
                std::filesystem::remove(tempEpgPath, ec);
                return false;
            }

            EpgCommitResult_ commitResult = EpgCommitResult_::Created;

            if (!CommitTempEpgByAppendRule_(
                tempEpgPath,
                epgPath,
                o.existingEpgMinBytes,
                commitResult)) {
                DebugLogFormat(
                    L"[ERROR][EPG] commit failed: temp=%ls final=%ls",
                    tempEpgPath.wstring().c_str(),
                    epgPath.wstring().c_str());

                std::error_code ec;
                std::filesystem::remove(tempEpgPath, ec);
                return false;
            }

            DebugLogFormat(
                L"[EPG][COMMIT] %ls: %ls",
                commitResult == EpgCommitResult_::Appended ? L"append" : L"create",
                epgPath.wstring().c_str());
        }

        outEpgPath = epgPath;
        
        // Lite版の ProgramEntry はここでは作らない。
        // ここで直接 ParsedEITPacket から作ると、重いJS版とデータ経路が分岐して
        // Lite版だけ空になる原因になるため、Run() 側で MergeEpgFiles() 完了後に
        // EpgMerged_spaceX_All_EIT.epg を読み直して構築する。

        DebugLogFormat(L"[EPG] wrote: %ls (unique=%zu)", epgPath.wstring().c_str(), events.size());
        return true;
    }

    static void RunReservationEpgRebuildActionAfterPortal_(
        const EpgHtmlPortal::WriteOptions& portalOpt,
        const std::filesystem::path& mergedPath,
        int space,
        const std::vector<std::vector<EpgHtmlPortal::ServiceEntry>>& enabledServicesBySpace)
    {
        const std::filesystem::path taskIniPath =
            EpgHtmlPortal_ReservationData::ResolveTaskIniPath(portalOpt);

        if (taskIniPath.empty()) {
            DebugLog(L"[EPG-REBUILD][RESERVATION][WARN] tasks.ini path is empty");
            return;
        }

        std::error_code ec;
        if (!std::filesystem::exists(taskIniPath, ec) || ec) {
            DebugLogFormat(
                L"[EPG-REBUILD][RESERVATION][WARN] tasks.ini not found. skip reconcile: %ls",
                taskIniPath.wstring().c_str());
            return;
        }

        auto latestPrograms =
            BuildLatestProgramsForReservationRebuild_(
                mergedPath,
                space,
                enabledServicesBySpace);

        ReservationEpgRebuildAction::Options rsvOpt{};
        rsvOpt.tasksIniPath = taskIniPath.wstring();
        rsvOpt.latestPrograms = std::move(latestPrograms);

        // EIT再構築時は、予約情報補正後の表示ズレを避けるためキャッシュは必ず再生成。
        rsvOpt.forceCacheRebuild = true;
        rsvOpt.saveTasksWhenChanged = true;
        rsvOpt.refreshReservationDataJs = true;

        const EpgHtmlPortal::WriteOptions portalOptCopy = portalOpt;

        rsvOpt.rebuildReservationDataJs =
            [portalOptCopy](const std::wstring&) -> bool {
            return EpgHtmlPortal_ReservationData::WriteReservationDataJs(
                portalOptCopy);
            };

        const auto rr = ReservationEpgRebuildAction::Run(rsvOpt);

        DebugLogFormat(
            L"[EPG-REBUILD][RESERVATION] result ok=%d tasks=%zu programs=%zu matched=%zu updated=%zu saved=%d cache=%d dataJs=%d msg=%ls",
            rr.ok ? 1 : 0,
            rr.taskCount,
            rr.programCount,
            rr.matchedCount,
            rr.updatedCount,
            rr.tasksSaved ? 1 : 0,
            rr.reservationCacheUpdated ? 1 : 0,
            rr.reservationDataJsUpdated ? 1 : 0,
            rr.message.c_str());
    }

    static void RunAutoReserveSearchAndRegisterAfterEpgUpdate_(
        const EpgHtmlPortal::WriteOptions& portalOpt,
        const Callbacks* cb,
        const wchar_t* reason) noexcept
    {
        const std::wstring reasonText =
            reason != nullptr
            ? std::wstring(reason)
            : std::wstring(L"epg-update");

        try {
            DebugLogFormat(
                L"[AUTO-RESERVE][TRIGGER] register begin reason=%ls",
                reasonText.c_str());

            if (SafeAutoReserveRegisterRequest_(
                cb,
                portalOpt,
                reasonText)) {
                DebugLogFormat(
                    L"[AUTO-RESERVE][TRIGGER] register requested reason=%ls",
                    reasonText.c_str());

                return;
            }

            DebugLogFormat(
                L"[AUTO-RESERVE][TRIGGER][FALLBACK] register callback missing. dry-run only. reason=%ls",
                reasonText.c_str());

            const AutoReserve::RuleMatchResult matchResult =
                EpgHtmlPortal_SearchData::RunAutoReserveMatchDryRunFromCurrentSearchData(
                    portalOpt,
                    1000);

            DebugLogFormat(
                L"[AUTO-RESERVE][TRIGGER] dry-run done reason=%ls ok=%d rules=%zu programs=%zu checked=%zu matched=%zu candidate=%zu auto=%zu message=%ls",
                reasonText.c_str(),
                matchResult.ok ? 1 : 0,
                matchResult.ruleCount,
                matchResult.programCount,
                matchResult.checkedCount,
                matchResult.matchedCount,
                matchResult.candidateCount,
                matchResult.autoRegisterCount,
                matchResult.message.c_str());

            SafeAutoReserveMatchResult_(
                cb,
                matchResult,
                portalOpt,
                reasonText.c_str());
        }
        catch (const std::bad_alloc&) {
            DebugLogFormat(
                L"[AUTO-RESERVE][TRIGGER][ERROR] bad_alloc reason=%ls",
                reasonText.c_str());
        }
        catch (const std::exception& e) {
            DebugLogFormat(
                L"[AUTO-RESERVE][TRIGGER][ERROR] exception reason=%ls what=%S",
                reasonText.c_str(),
                e.what());
        }
        catch (...) {
            DebugLogFormat(
                L"[AUTO-RESERVE][TRIGGER][ERROR] unknown reason=%ls",
                reasonText.c_str());
        }
    }

} // namespace EpgBuildGUI
