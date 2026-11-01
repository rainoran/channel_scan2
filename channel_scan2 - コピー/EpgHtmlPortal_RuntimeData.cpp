#include "EpgHtmlPortal_RuntimeData.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "EpgHtmlPortal_Common.hpp"

namespace EpgHtmlPortal_RuntimeData
{
    namespace
    {
        std::wstring FileNameForPortal(const std::filesystem::path& p)
        {
            return p.empty() ? std::wstring{} : p.filename().wstring();
        }

        std::wstring NormalizeReserveButtonMode_(std::wstring value)
        {
            value = EpgHtmlPortal_Common::ToLowerAsciiW(
                EpgHtmlPortal_Common::TrimW(std::move(value)));

            if (value == L"record") {
                return L"record";
            }
            if (value == L"watch") {
                return L"watch";
            }
            return L"select";
        }

        std::filesystem::path FindConfigIniPath_()
        {
            const std::filesystem::path exeDir =
                EpgHtmlPortal_Common::GetExeDirectoryPath();

            if (!exeDir.empty()) {
                const std::filesystem::path p = exeDir / L"config.ini";
                if (std::filesystem::exists(p)) {
                    return p;
                }
            }

            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            if (!ec && !cwd.empty()) {
                const std::filesystem::path p = cwd / L"config.ini";
                if (std::filesystem::exists(p)) {
                    return p;
                }
            }

            return {};
        }

        std::wstring ReadIniValueLike_(
            const std::filesystem::path& iniPath,
            std::wstring_view sectionName,
            std::wstring_view keyName)
        {
            if (iniPath.empty() || !std::filesystem::exists(iniPath)) {
                return {};
            }

            std::wstring text;
            if (!EpgHtmlPortal_Common::ReadTextFileToWide(iniPath, text)) {
                return {};
            }

            const std::wstring targetSection =
                EpgHtmlPortal_Common::ToLowerAsciiW(std::wstring(sectionName));
            const std::wstring targetKey =
                EpgHtmlPortal_Common::ToLowerAsciiW(std::wstring(keyName));

            bool inTargetSection = false;
            std::size_t pos = 0;

            while (pos < text.size()) {
                std::size_t end = text.find(L'\n', pos);
                if (end == std::wstring::npos) {
                    end = text.size();
                }

                std::wstring line = text.substr(pos, end - pos);
                if (!line.empty() && line.back() == L'\r') {
                    line.pop_back();
                }

                line = EpgHtmlPortal_Common::TrimW(std::move(line));

                if (!line.empty() && line.front() != L';' && line.front() != L'#') {
                    if (line.front() == L'[' && line.back() == L']' && line.size() >= 2) {
                        std::wstring sec = EpgHtmlPortal_Common::TrimW(
                            line.substr(1, line.size() - 2));
                        sec = EpgHtmlPortal_Common::ToLowerAsciiW(std::move(sec));
                        inTargetSection = (sec == targetSection);
                    }
                    else if (inTargetSection) {
                        const std::size_t eq = line.find(L'=');
                        if (eq != std::wstring::npos) {
                            std::wstring key = EpgHtmlPortal_Common::TrimW(line.substr(0, eq));
                            key = EpgHtmlPortal_Common::ToLowerAsciiW(std::move(key));

                            if (key == targetKey) {
                                return EpgHtmlPortal_Common::TrimW(line.substr(eq + 1));
                            }
                        }
                    }
                }

                pos = (end < text.size()) ? (end + 1) : text.size();
            }

            return {};
        }

        std::wstring ResolveEpgReserveButtonMode_()
        {
            const std::filesystem::path iniPath = FindConfigIniPath_();
            const std::wstring raw =
                ReadIniValueLike_(iniPath, L"Reservation", L"EpgReserveButtonMode");

            return NormalizeReserveButtonMode_(raw);
        }
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
            (std::clamp)(ReadIniIntLike_(iniPath, L"Network", L"HttpPort", 8080), 1, 65535);

        if (bindAddress.empty()) {
            bindAddress = allowRemote ? L"0.0.0.0" : L"127.0.0.1";
        }

        bindAddress = EpgHtmlPortal_Common::TrimW(std::move(bindAddress));

        // ブラウザから 0.0.0.0 へはアクセスしないので、UI向けURLは localhost に寄せる
        if (bindAddress == L"0.0.0.0" || bindAddress == L"::" || bindAddress == L"[::]") {
            bindAddress = L"127.0.0.1";
        }

        std::wstring base = L"http://";
        base += bindAddress;
        base += L":";
        base += std::to_wstring(httpPort);
        base += L"/api/reservation";
        return base;
    }

    std::wstring MakeStandardHtmlFileName(int space)
    {
        return L"epg_space" + std::to_wstring(space) + L".html";
    }

    std::wstring MakeStandardBeforeHtmlFileName(int space)
    {
        return L"epg_space" + std::to_wstring(space) + L"_before.html";
    }

    std::wstring MakeStandardTimetableHtmlFileName(int space)
    {
        return L"epg_space" + std::to_wstring(space) + L"_timetable.html";
    }

    std::wstring MakeStandardListHtmlFileName(int space)
    {
        return L"epg_space" + std::to_wstring(space) + L"_list.html";
    }

    std::wstring MakeStandardBeforeTimetableHtmlFileName(int space)
    {
        return L"epg_space" + std::to_wstring(space) + L"_before_timetable.html";
    }

    std::wstring MakeStandardBeforeListHtmlFileName(int space)
    {
        return L"epg_space" + std::to_wstring(space) + L"_before_list.html";
    }

    std::wstring MakeSpaceLabel(int space)
    {
        switch (space) {
        case 0:
            return L"地上波";
        case 1:
            return L"BS";
        case 2:
            return L"CS";
        default:
            return L"Space " + std::to_wstring(space);
        }
    }

    std::wstring MakeMissingMessage(int space, bool before)
    {
        std::wstring msg = MakeSpaceLabel(space);
        msg += before
            ? L" の過去番組表はまだ生成されていません。"
            : L" の番組表はまだ生成されていません。";
        return msg;
    }

    SpaceEntry BuildSpaceEntry(
        const EpgHtmlPortal::WriteOptions& opt,
        int space)
    {
        SpaceEntry e{};
        e.space = space;
        e.label = MakeSpaceLabel(space);

        if (space == opt.space) {
            e.currentPath =
                opt.currentHtmlPath.empty()
                ? (opt.outputDir / MakeStandardHtmlFileName(space))
                : opt.currentHtmlPath;

            e.beforePath =
                opt.beforeHtmlPath.empty()
                ? (opt.outputDir / MakeStandardBeforeHtmlFileName(space))
                : opt.beforeHtmlPath;
        }
        else {
            e.currentPath = opt.outputDir / MakeStandardHtmlFileName(space);
            e.beforePath = opt.outputDir / MakeStandardBeforeHtmlFileName(space);
        }

        // 分離HTMLは固定命名で見る。
        // index側では currentTimetableSrc / currentListSrc を優先する。
        e.currentTimetablePath =
            opt.outputDir / MakeStandardTimetableHtmlFileName(space);

        e.currentListPath =
            opt.outputDir / MakeStandardListHtmlFileName(space);

        e.beforeTimetablePath =
            opt.outputDir / MakeStandardBeforeTimetableHtmlFileName(space);

        e.beforeListPath =
            opt.outputDir / MakeStandardBeforeListHtmlFileName(space);

        e.hasCurrent =
            !e.currentPath.empty() &&
            std::filesystem::exists(e.currentPath);

        e.hasBefore =
            !e.beforePath.empty() &&
            std::filesystem::exists(e.beforePath);

        e.hasCurrentTimetable =
            !e.currentTimetablePath.empty() &&
            std::filesystem::exists(e.currentTimetablePath);

        e.hasCurrentList =
            !e.currentListPath.empty() &&
            std::filesystem::exists(e.currentListPath);

        e.hasBeforeTimetable =
            !e.beforeTimetablePath.empty() &&
            std::filesystem::exists(e.beforeTimetablePath);

        e.hasBeforeList =
            !e.beforeListPath.empty() &&
            std::filesystem::exists(e.beforeListPath);

        return e;
    }

    std::vector<SpaceEntry> BuildKnownSpaces(
        const EpgHtmlPortal::WriteOptions& opt)
    {
        std::vector<SpaceEntry> spaces;
        spaces.reserve(3);

        spaces.push_back(BuildSpaceEntry(opt, 0));
        spaces.push_back(BuildSpaceEntry(opt, 1));
        spaces.push_back(BuildSpaceEntry(opt, 2));

        return spaces;
    }

    std::wstring BuildSpaceDatasetJs(
        const std::vector<SpaceEntry>& spaces)
    {
        std::wstring js;
        js.reserve(4096 + spaces.size() * 768);

        js += L"const SPACE_DATA = {\n";

        for (const auto& e : spaces) {
            const bool compatHasCurrent =
                e.hasCurrent || e.hasCurrentTimetable || e.hasCurrentList;

            const bool compatHasBefore =
                e.hasBefore || e.hasBeforeTimetable || e.hasBeforeList;

            // 旧 runtime / 旧 index 向け互換。
            // 分離HTMLがある場合は時間割を currentSrc として返す。
            const std::filesystem::path compatCurrentPath =
                e.hasCurrentTimetable
                ? e.currentTimetablePath
                : (e.hasCurrent ? e.currentPath : e.currentListPath);

            const std::filesystem::path compatBeforePath =
                e.hasBeforeTimetable
                ? e.beforeTimetablePath
                : (e.hasBefore ? e.beforePath : e.beforeListPath);

            js += L"  '";
            js += std::to_wstring(e.space);
            js += L"': {\n";

            js += L"    label: ";
            js += EpgHtmlPortal_Common::ToJsStringLiteral(e.label);
            js += L",\n";

            // 既存互換。
            js += L"    currentSrc: ";
            js += EpgHtmlPortal_Common::ToJsStringLiteral(
                FileNameForPortal(compatCurrentPath));
            js += L",\n";

            js += L"    beforeSrc: ";
            js += EpgHtmlPortal_Common::ToJsStringLiteral(
                FileNameForPortal(compatBeforePath));
            js += L",\n";

            js += L"    hasCurrent: ";
            js += EpgHtmlPortal_Common::ToJsBool(compatHasCurrent);
            js += L",\n";

            js += L"    hasBefore: ";
            js += EpgHtmlPortal_Common::ToJsBool(compatHasBefore);
            js += L",\n";

            // 分離HTML用。
            js += L"    currentTimetableSrc: ";
            js += EpgHtmlPortal_Common::ToJsStringLiteral(
                FileNameForPortal(e.currentTimetablePath));
            js += L",\n";

            js += L"    currentListSrc: ";
            js += EpgHtmlPortal_Common::ToJsStringLiteral(
                FileNameForPortal(e.currentListPath));
            js += L",\n";

            js += L"    beforeTimetableSrc: ";
            js += EpgHtmlPortal_Common::ToJsStringLiteral(
                FileNameForPortal(e.beforeTimetablePath));
            js += L",\n";

            js += L"    beforeListSrc: ";
            js += EpgHtmlPortal_Common::ToJsStringLiteral(
                FileNameForPortal(e.beforeListPath));
            js += L",\n";

            js += L"    hasCurrentTimetable: ";
            js += EpgHtmlPortal_Common::ToJsBool(e.hasCurrentTimetable);
            js += L",\n";

            js += L"    hasCurrentList: ";
            js += EpgHtmlPortal_Common::ToJsBool(e.hasCurrentList);
            js += L",\n";

            js += L"    hasBeforeTimetable: ";
            js += EpgHtmlPortal_Common::ToJsBool(e.hasBeforeTimetable);
            js += L",\n";

            js += L"    hasBeforeList: ";
            js += EpgHtmlPortal_Common::ToJsBool(e.hasBeforeList);
            js += L"\n";

            js += L"  },\n";
        }

        js += L"};\n";
        js += L"window.SPACE_DATA = SPACE_DATA;\n";

        return js;
    }

    std::wstring BuildConfigServiceDatasetJs(
        const EpgHtmlPortal::WriteOptions& opt)
    {
        std::wstring js;
        js.reserve(1024);
        js += L"const CONFIG_SERVICE_DATA = {\n";

        for (int space = 0; space <= 2; ++space) {
            js += L"  '";
            js += std::to_wstring(space);
            js += L"': [";

            if (static_cast<std::size_t>(space) < opt.enabledServicesBySpace.size()) {
                const auto& list = opt.enabledServicesBySpace[static_cast<std::size_t>(space)];
                bool first = true;

                for (const auto& entry : list) {
                    if (entry.serviceName.empty()) {
                        continue;
                    }

                    if (!first) {
                        js += L", ";
                    }

                    js += EpgHtmlPortal_Common::ToJsStringLiteral(entry.serviceName);
                    first = false;
                }
            }

            js += L"],\n";
        }

        js += L"};\n";
        js += L"window.CONFIG_SERVICE_DATA = CONFIG_SERVICE_DATA;\n";
        return js;
    }

    std::wstring BuildRuntimeScriptBlock(
        std::wstring_view jsSpaceData,
        std::wstring_view jsConfigServiceData,
        int initialSpace,
        std::wstring_view runtimeScriptFileName)
    {
        const std::wstring epgReserveButtonMode =
            ResolveEpgReserveButtonMode_();

        const std::wstring reservationApiBase =
            ResolveReservationApiBase_();

        std::wstring reservationApiHost;
        int reservationApiPort = 0;
        const bool reservationApiEnabled = !reservationApiBase.empty();

        if (!reservationApiBase.empty()) {
            const std::wstring prefix = L"http://";

            if (reservationApiBase.rfind(prefix, 0) == 0) {
                const std::wstring rest =
                    reservationApiBase.substr(prefix.size());

                const std::size_t colon = rest.find(L':');
                const std::size_t slash = rest.find(L'/');

                if (colon != std::wstring::npos) {
                    reservationApiHost = rest.substr(0, colon);

                    const std::size_t portEnd =
                        (slash != std::wstring::npos && slash > colon)
                        ? slash
                        : rest.size();

                    try {
                        reservationApiPort =
                            std::stoi(
                                rest.substr(
                                    colon + 1,
                                    portEnd - (colon + 1)));
                    }
                    catch (...) {
                        reservationApiPort = 0;
                    }
                }
            }
        }

        std::wstring out;
        out.reserve(
            jsSpaceData.size() +
            jsConfigServiceData.size() +
            runtimeScriptFileName.size() +
            epgReserveButtonMode.size() +
            reservationApiBase.size() +
            reservationApiHost.size() +
            4096);

        out += L"<script>\n";

        out.append(jsSpaceData.data(), jsSpaceData.size());
        out.append(jsConfigServiceData.data(), jsConfigServiceData.size());

        out += L"const DEFAULT_SPACE = ";
        out += std::to_wstring(initialSpace);
        out += L";\n";

        out += L"window.EPG_RESERVE_BUTTON_MODE = ";
        out += EpgHtmlPortal_Common::ToJsStringLiteral(epgReserveButtonMode);
        out += L";\n";

        out += L"window.EPG_RUNTIME = Object.assign({}, window.EPG_RUNTIME || {}, {\n";

        out += L"  reservationApiBase: ";
        out += EpgHtmlPortal_Common::ToJsStringLiteral(reservationApiBase);
        out += L",\n";

        out += L"  reservationApiEnabled: ";
        out += EpgHtmlPortal_Common::ToJsBool(reservationApiEnabled);
        out += L",\n";

        out += L"  reservationApiHost: ";
        out += EpgHtmlPortal_Common::ToJsStringLiteral(reservationApiHost);
        out += L",\n";

        out += L"  reservationApiPort: ";
        out += std::to_wstring(reservationApiPort);
        out += L",\n";

        out += L"  defaultSpace: ";
        out += std::to_wstring(initialSpace);
        out += L"\n";

        out += L"});\n";
        out += L"</script>\n";

        out += L"<script src=\"";
        out += EpgHtmlPortal_Common::EscapeHtmlW(runtimeScriptFileName);
        out += L"\"></script>\n";

        // ここでは iframe の src を変更しない。
        // 番組表 iframe の読み込みは EpgHtmlPortal_index.cpp 側だけで管理する。
        out += L"<script>\n";
        out += L"(function(){\n";
        out += L"'use strict';\n";

        out += L"function byId(id){return document.getElementById(id);}\n";

        out += L"function fallbackOpenDetailSearch(){\n";
        out += L"  const ov = byId('detailSearchOverlay');\n";
        out += L"  if (!ov) return;\n";
        out += L"  ov.classList.add('open');\n";
        out += L"  ov.setAttribute('aria-hidden', 'false');\n";
        out += L"}\n";

        out += L"function fallbackCloseDetailSearch(){\n";
        out += L"  const ov = byId('detailSearchOverlay');\n";
        out += L"  if (!ov) return;\n";
        out += L"  ov.classList.remove('open');\n";
        out += L"  ov.setAttribute('aria-hidden', 'true');\n";
        out += L"}\n";

        out += L"function fallbackRunQuickSearch(){\n";
        out += L"  const quick = byId('quickKeyword');\n";
        out += L"  const detail = byId('detailKeyword');\n";
        out += L"  if (quick && detail) {\n";
        out += L"    detail.value = quick.value || '';\n";
        out += L"  }\n";
        out += L"  fallbackOpenDetailSearch();\n";
        out += L"  try {\n";
        out += L"    if (typeof window.switchPage === 'function') {\n";
        out += L"      window.switchPage('search');\n";
        out += L"    }\n";
        out += L"  } catch (_) {\n";
        out += L"  }\n";
        out += L"}\n";

        out += L"function fallbackApplyQuickView(){\n";
        out += L"  const mode = byId('displayMode');\n";
        out += L"  if (!mode) return;\n";
        out += L"\n";
        out += L"  if (mode.value === 'past') {\n";
        out += L"    try {\n";
        out += L"      if (typeof window.switchProgramRange === 'function') {\n";
        out += L"        window.switchProgramRange('past');\n";
        out += L"      } else if (typeof window.switchPage === 'function') {\n";
        out += L"        window.switchPage('past');\n";
        out += L"      }\n";
        out += L"    } catch (_) {\n";
        out += L"    }\n";
        out += L"    return;\n";
        out += L"  }\n";
        out += L"\n";
        out += L"  try {\n";
        out += L"    if (typeof window.switchProgramRange === 'function') {\n";
        out += L"      window.switchProgramRange('now');\n";
        out += L"    } else if (typeof window.switchPage === 'function') {\n";
        out += L"      window.switchPage('now');\n";
        out += L"    }\n";
        out += L"  } catch (_) {\n";
        out += L"  }\n";
        out += L"}\n";

        out += L"if (typeof window.runQuickSearch !== 'function') {\n";
        out += L"  window.runQuickSearch = fallbackRunQuickSearch;\n";
        out += L"}\n";

        out += L"if (typeof window.openDetailSearch !== 'function') {\n";
        out += L"  window.openDetailSearch = fallbackOpenDetailSearch;\n";
        out += L"}\n";

        out += L"if (typeof window.closeDetailSearch !== 'function') {\n";
        out += L"  window.closeDetailSearch = fallbackCloseDetailSearch;\n";
        out += L"}\n";

        out += L"if (typeof window.applyQuickView !== 'function') {\n";
        out += L"  window.applyQuickView = fallbackApplyQuickView;\n";
        out += L"}\n";

        out += L"})();\n";
        out += L"</script>\n";

        return out;
    }
}