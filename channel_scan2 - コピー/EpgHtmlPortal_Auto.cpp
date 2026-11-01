#include "EpgHtmlPortal_Auto.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <new>
#include <utility>
#include <vector>
#include <chrono>

#include "AutoReserveRuleStore.hpp"
#include "DebugManager.h"
#include "EpgHtmlPortal_Common.hpp"

namespace EpgHtmlPortal_Auto
{
    namespace
    {
        struct ConfigServiceOption_
        {
            int space = 0;
            std::uint16_t serviceId = 0;
            std::wstring serviceName;
        };

        struct IniSection_
        {
            std::wstring name;
            std::map<std::wstring, std::wstring> values;
        };

        static bool FileExistsNoThrow_(
            const std::filesystem::path& path)
        {
            if (path.empty()) {
                return false;
            }

            std::error_code ec;
            return std::filesystem::exists(path, ec) && !ec;
        }

        static std::filesystem::path FindConfigIniPath_()
        {
            const std::filesystem::path exeDir =
                EpgHtmlPortal_Common::GetExeDirectoryPath();

            if (!exeDir.empty()) {
                const std::filesystem::path p = exeDir / L"config.ini";
                if (FileExistsNoThrow_(p)) {
                    return p;
                }
            }

            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            if (!ec && !cwd.empty()) {
                const std::filesystem::path p = cwd / L"config.ini";
                if (FileExistsNoThrow_(p)) {
                    return p;
                }
            }

            return {};
        }

        static std::wstring SpaceLabel_(
            int space)
        {
            switch (space) {
            case -1:
                return L"すべて";
            case 0:
                return L"地上波";
            case 1:
                return L"BS";
            case 2:
                return L"CS";
            default:
                return L"不明";
            }
        }

        static std::wstring MatchModeLabel_(
            AutoReserve::MatchMode mode)
        {
            switch (mode) {
            case AutoReserve::MatchMode::Exact:
                return L"完全一致";
            case AutoReserve::MatchMode::StartsWith:
                return L"前方一致";
            case AutoReserve::MatchMode::EndsWith:
                return L"後方一致";
            case AutoReserve::MatchMode::Contains:
            default:
                return L"部分一致";
            }
        }

        static std::wstring DuplicatePolicyLabel_(
            AutoReserve::DuplicatePolicy policy)
        {
            switch (policy) {
            case AutoReserve::DuplicatePolicy::SkipSameTitleAndTime:
                return L"同一タイトル/時刻を除外";
            case AutoReserve::DuplicatePolicy::Allow:
                return L"重複許可";
            case AutoReserve::DuplicatePolicy::SkipSameEvent:
            default:
                return L"同一イベントを除外";
            }
        }

        static std::wstring TokenJoinModeLabel_(
            AutoReserve::TokenJoinMode mode)
        {
            return mode == AutoReserve::TokenJoinMode::And
                ? L"AND"
                : L"OR";
        }

        static std::wstring ReserveActionModeLabel_(
            AutoReserve::ReserveActionMode mode)
        {
            return mode == AutoReserve::ReserveActionMode::Auto
                ? L"auto"
                : L"candidate";
        }

        static bool ParseBoolLoose_(
            std::wstring value,
            bool defaultValue = false)
        {
            value = EpgHtmlPortal_Common::ToLowerAsciiW(
                EpgHtmlPortal_Common::TrimW(std::move(value)));

            if (value == L"1" ||
                value == L"true" ||
                value == L"yes" ||
                value == L"on" ||
                value == L"enable" ||
                value == L"enabled" ||
                value == L"有効") {
                return true;
            }

            if (value == L"0" ||
                value == L"false" ||
                value == L"no" ||
                value == L"off" ||
                value == L"disable" ||
                value == L"disabled" ||
                value == L"無効") {
                return false;
            }

            return defaultValue;
        }

        static int ParseIntLoose_(
            std::wstring value,
            int defaultValue = 0)
        {
            value = EpgHtmlPortal_Common::TrimW(std::move(value));
            if (value.empty()) {
                return defaultValue;
            }

            try {
                std::size_t idx = 0;
                const int base =
                    value.size() > 2 &&
                    value[0] == L'0' &&
                    (value[1] == L'x' || value[1] == L'X')
                    ? 16
                    : 10;

                return std::stoi(value, &idx, base);
            }
            catch (...) {
                return defaultValue;
            }
        }

        static std::uint16_t ParseUint16Loose_(
            const std::wstring& value,
            std::uint16_t defaultValue = 0)
        {
            const int v = ParseIntLoose_(value, static_cast<int>(defaultValue));
            if (v < 0 || v > 0xFFFF) {
                return defaultValue;
            }
            return static_cast<std::uint16_t>(v);
        }

        static bool LooksNumericOnly_(
            const std::wstring& value)
        {
            const std::wstring s = EpgHtmlPortal_Common::TrimW(value);
            if (s.empty()) {
                return true;
            }

            for (wchar_t ch : s) {
                if (!(std::iswdigit(ch) || ch == L'-' || ch == L'+' || ch == L' ' || ch == L'\t')) {
                    return false;
                }
            }
            return true;
        }

        static std::wstring GetMapValue_(
            const std::map<std::wstring, std::wstring>& values,
            std::initializer_list<const wchar_t*> keys)
        {
            for (const auto* rawKey : keys) {
                std::wstring key = rawKey ? std::wstring(rawKey) : std::wstring{};
                key = EpgHtmlPortal_Common::ToLowerAsciiW(std::move(key));

                const auto it = values.find(key);
                if (it == values.end()) {
                    continue;
                }

                std::wstring value = EpgHtmlPortal_Common::TrimW(it->second);
                if (!value.empty()) {
                    return value;
                }
            }

            return {};
        }

        static std::vector<IniSection_> ParseIniSections_(
            const std::filesystem::path& iniPath)
        {
            std::vector<IniSection_> sections;

            if (iniPath.empty() || !FileExistsNoThrow_(iniPath)) {
                return sections;
            }

            std::wstring text;
            if (!EpgHtmlPortal_Common::ReadTextFileToWide(iniPath, text)) {
                return sections;
            }

            IniSection_* current = nullptr;
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
                        IniSection_ sec{};
                        sec.name = EpgHtmlPortal_Common::TrimW(line.substr(1, line.size() - 2));
                        sections.push_back(std::move(sec));
                        current = &sections.back();
                    }
                    else if (current) {
                        const std::size_t eq = line.find(L'=');
                        if (eq != std::wstring::npos) {
                            std::wstring key = EpgHtmlPortal_Common::TrimW(line.substr(0, eq));
                            std::wstring value = EpgHtmlPortal_Common::TrimW(line.substr(eq + 1));
                            key = EpgHtmlPortal_Common::ToLowerAsciiW(std::move(key));
                            current->values[key] = std::move(value);
                        }
                    }
                }

                pos = (end < text.size()) ? (end + 1) : text.size();
            }

            return sections;
        }

        static bool IsLikelyConfigServiceSection_(
            const IniSection_& sec)
        {
            const std::wstring lowerSection =
                EpgHtmlPortal_Common::ToLowerAsciiW(sec.name);

            if (lowerSection.find(L"channel") != std::wstring::npos ||
                lowerSection.find(L"service") != std::wstring::npos ||
                lowerSection.find(L"station") != std::wstring::npos ||
                lowerSection.find(L"bon") != std::wstring::npos ||
                lowerSection.rfind(L"ch", 0) == 0) {
                return true;
            }

            const bool hasServiceName =
                !GetMapValue_(sec.values,
                    {
                        L"servicename",
                        L"service_name",
                        L"service",
                        L"channelname",
                        L"channel_name",
                        L"displayname",
                        L"display_name",
                        L"name",
                        L"title"
                    }).empty();

            const bool hasServiceId =
                !GetMapValue_(sec.values,
                    {
                        L"serviceid",
                        L"service_id",
                        L"sid"
                    }).empty();

            return hasServiceName && hasServiceId;
        }

        static bool IsConfigServiceSectionEnabled_(
            const IniSection_& sec)
        {
            const std::wstring enabledText =
                GetMapValue_(sec.values,
                    {
                        L"enabled",
                        L"enable",
                        L"isenabled",
                        L"use",
                        L"usechannel",
                        L"active",
                        L"checked",
                        L"visible"
                    });

            if (!enabledText.empty()) {
                return ParseBoolLoose_(enabledText, false);
            }

            const std::wstring disabledText =
                GetMapValue_(sec.values,
                    {
                        L"disabled",
                        L"disable",
                        L"hidden"
                    });

            if (!disabledText.empty()) {
                return !ParseBoolLoose_(disabledText, false);
            }

            return true;
        }

        static std::wstring ResolveServiceNameFromConfigSection_(
            const IniSection_& sec)
        {
            std::wstring name =
                GetMapValue_(sec.values,
                    {
                        L"servicename",
                        L"service_name",
                        L"service",
                        L"channelname",
                        L"channel_name",
                        L"displayname",
                        L"display_name",
                        L"name",
                        L"title"
                    });

            name = EpgHtmlPortal_Common::TrimW(std::move(name));
            name = EpgHtmlPortal_Common::RepairUtf8ByteWidenedMojibake(name);

            if (name.empty() || LooksNumericOnly_(name)) {
                return {};
            }

            return name;
        }

        static int ResolveSpaceFromConfigSection_(
            const IniSection_& sec)
        {
            return ParseIntLoose_(
                GetMapValue_(sec.values,
                    {
                        L"space",
                        L"targetspace",
                        L"currentspace",
                        L"tuningspace",
                        L"tuning_space"
                    }),
                0);
        }

        static std::uint16_t ResolveServiceIdFromConfigSection_(
            const IniSection_& sec)
        {
            return ParseUint16Loose_(
                GetMapValue_(sec.values,
                    {
                        L"serviceid",
                        L"service_id",
                        L"sid"
                    }),
                0);
        }

        static void AddConfigServiceOption_(
            std::vector<ConfigServiceOption_>& out,
            std::set<std::wstring>& seen,
            int space,
            std::uint16_t serviceId,
            std::wstring serviceName)
        {
            serviceName = EpgHtmlPortal_Common::TrimW(std::move(serviceName));
            serviceName = EpgHtmlPortal_Common::RepairUtf8ByteWidenedMojibake(serviceName);

            if (serviceName.empty()) {
                return;
            }

            std::wstring key =
                std::to_wstring(space) +
                L"|" +
                std::to_wstring(serviceId) +
                L"|" +
                EpgHtmlPortal_Common::ToLowerAsciiW(serviceName);

            if (!seen.insert(key).second) {
                return;
            }

            ConfigServiceOption_ option{};
            option.space = space;
            option.serviceId = serviceId;
            option.serviceName = std::move(serviceName);
            out.push_back(std::move(option));
        }

        static std::vector<ConfigServiceOption_> LoadEnabledConfigServicesFromOptions_(
            const EpgHtmlPortal::WriteOptions& opt)
        {
            std::vector<ConfigServiceOption_> out;
            std::set<std::wstring> seen;

            for (int space = 0; space <= 2; ++space) {
                const std::size_t index = static_cast<std::size_t>(space);
                if (index >= opt.enabledServicesBySpace.size()) {
                    continue;
                }

                const auto& list = opt.enabledServicesBySpace[index];
                for (const auto& service : list) {
                    AddConfigServiceOption_(
                        out,
                        seen,
                        space,
                        service.serviceId,
                        service.serviceName);
                }
            }

            return out;
        }

        static std::vector<ConfigServiceOption_> LoadEnabledConfigServicesFromIni_()
        {
            std::vector<ConfigServiceOption_> out;

            const std::filesystem::path iniPath = FindConfigIniPath_();
            if (iniPath.empty()) {
                DebugLog(L"[EPG-PORTAL][AUTO][WARN] config.ini が見つからないため放送局候補を生成できません。");
                return out;
            }

            const std::vector<IniSection_> sections = ParseIniSections_(iniPath);
            std::set<std::wstring> seen;

            for (const auto& sec : sections) {
                if (!IsLikelyConfigServiceSection_(sec)) {
                    continue;
                }
                if (!IsConfigServiceSectionEnabled_(sec)) {
                    continue;
                }

                std::wstring serviceName = ResolveServiceNameFromConfigSection_(sec);
                if (serviceName.empty()) {
                    continue;
                }

                AddConfigServiceOption_(
                    out,
                    seen,
                    ResolveSpaceFromConfigSection_(sec),
                    ResolveServiceIdFromConfigSection_(sec),
                    std::move(serviceName));
            }

            return out;
        }

        static std::vector<ConfigServiceOption_> LoadEnabledConfigServices_(
            const EpgHtmlPortal::WriteOptions& opt)
        {
            std::vector<ConfigServiceOption_> out = LoadEnabledConfigServicesFromOptions_(opt);

            if (out.empty()) {
                out = LoadEnabledConfigServicesFromIni_();
            }

            std::sort(
                out.begin(),
                out.end(),
                [](const ConfigServiceOption_& a, const ConfigServiceOption_& b) {
                    if (a.space != b.space) {
                        return a.space < b.space;
                    }
                    if (a.serviceName != b.serviceName) {
                        return a.serviceName < b.serviceName;
                    }
                    return a.serviceId < b.serviceId;
                });

            DebugLogFormat(
                L"[EPG-PORTAL][AUTO] 有効放送局候補: %zu 件",
                out.size());

            return out;
        }

        static void AppendConfigServiceOptionsHtml_(
            std::wstring& html,
            const std::vector<ConfigServiceOption_>& services)
        {
            if (services.empty()) {
                html += L"<option value=\"\" disabled>config.ini に有効な放送局候補がありません</option>";
                return;
            }

            int lastSpace = -9999;

            for (const auto& service : services) {
                if (service.serviceName.empty()) {
                    continue;
                }

                if (service.space != lastSpace) {
                    if (lastSpace != -9999) {
                        html += L"</optgroup>";
                    }

                    html += L"<optgroup label=\"";
                    html += EpgHtmlPortal_Common::EscapeHtmlW(SpaceLabel_(service.space));
                    html += L"\">";
                    lastSpace = service.space;
                }

                html += L"<option value=\"";
                html += EpgHtmlPortal_Common::EscapeHtmlW(service.serviceName);
                html += L"\">";
                html += EpgHtmlPortal_Common::EscapeHtmlW(service.serviceName);

                if (service.serviceId != 0) {
                    html += L" / SID ";
                    html += std::to_wstring(service.serviceId);
                }

                html += L"</option>";
            }

            if (lastSpace != -9999) {
                html += L"</optgroup>";
            }
        }

        static void AppendGenreOptionsHtml_(
            std::wstring& html)
        {
            html += L"<option value=\"ニュース/報道\">ニュース/報道</option>";
            html += L"<option value=\"スポーツ\">スポーツ</option>";
            html += L"<option value=\"情報/ワイドショー\">情報/ワイドショー</option>";
            html += L"<option value=\"ドラマ\">ドラマ</option>";
            html += L"<option value=\"音楽\">音楽</option>";
            html += L"<option value=\"バラエティ\">バラエティ</option>";
            html += L"<option value=\"映画\">映画</option>";
            html += L"<option value=\"アニメ/特撮\">アニメ/特撮</option>";
            html += L"<option value=\"ドキュメンタリー/教養\">ドキュメンタリー/教養</option>";
            html += L"<option value=\"劇場/公演\">劇場/公演</option>";
            html += L"<option value=\"趣味/教育\">趣味/教育</option>";
            html += L"<option value=\"福祉\">福祉</option>";
        }

        static std::wstring FormatTimeLabel_(
            int minutes)
        {
            if (minutes == 1440) {
                return L"24:00";
            }
            if (minutes < 0) {
                return L"--:--";
            }

            const int h = minutes / 60;
            const int m = minutes % 60;
            wchar_t buf[16]{};
            swprintf_s(buf, L"%02d:%02d", h, m);
            return std::wstring(buf);
        }

        static void AppendTimeOptionsHtml_(
            std::wstring& html,
            int beginMinutes,
            int endMinutes,
            int stepMinutes)
        {
            if (stepMinutes <= 0) {
                stepMinutes = 30;
            }

            for (int m = beginMinutes; m <= endMinutes; m += stepMinutes) {
                html += L"<option value=\"";
                html += std::to_wstring(m);
                html += L"\">";
                html += FormatTimeLabel_(m);
                html += L"</option>";
            }
        }

        static void AppendRuleJsObject_(
            std::wstring& js,
            const AutoReserve::Rule& r)
        {
            js += L"{";
            js += L"id:";
            js += std::to_wstring(r.id);
            js += L",";
            js += L"enabled:";
            js += EpgHtmlPortal_Common::ToJsBool(r.enabled);
            js += L",";
            js += L"name:";
            js += EpgHtmlPortal_Common::ToJsStringLiteral(r.name);
            js += L",";
            js += L"keyword:";
            js += EpgHtmlPortal_Common::ToJsStringLiteral(r.keyword);
            js += L",";
            js += L"excludeKeyword:";
            js += EpgHtmlPortal_Common::ToJsStringLiteral(r.excludeKeyword);
            js += L",";
            js += L"titleMatchMode:";
            js += std::to_wstring(static_cast<int>(r.titleMatchMode));
            js += L",";
            js += L"matchTitle:";
            js += EpgHtmlPortal_Common::ToJsBool(r.matchTitle);
            js += L",";
            js += L"matchSummary:";
            js += EpgHtmlPortal_Common::ToJsBool(r.matchSummary);
            js += L",";
            js += L"matchExtended:";
            js += EpgHtmlPortal_Common::ToJsBool(r.matchExtended);
            js += L",";
            js += L"targetSpace:";
            js += std::to_wstring(r.targetSpace);
            js += L",";
            js += L"serviceName:";
            js += EpgHtmlPortal_Common::ToJsStringLiteral(r.serviceName);
            js += L",";
            js += L"genreName:";
            js += EpgHtmlPortal_Common::ToJsStringLiteral(r.genreName);
            js += L",";
            js += L"startMinutes:";
            js += std::to_wstring(r.startMinutes);
            js += L",";
            js += L"endMinutes:";
            js += std::to_wstring(r.endMinutes);
            js += L",";
            js += L"priority:";
            js += std::to_wstring(r.priority);
            js += L",";
            js += L"preMarginSeconds:";
            js += std::to_wstring(r.preMarginSeconds);
            js += L",";
            js += L"postMarginSeconds:";
            js += std::to_wstring(r.postMarginSeconds);
            js += L",";
            js += L"createReservationEnabled:";
            js += EpgHtmlPortal_Common::ToJsBool(r.createReservationEnabled);
            js += L",";
            js += L"duplicatePolicy:";
            js += std::to_wstring(static_cast<int>(r.duplicatePolicy));
            js += L",";
            js += L"useKeyword:";
            js += EpgHtmlPortal_Common::ToJsBool(r.useKeyword);
            js += L",";
            js += L"keywordJoinMode:";
            js += EpgHtmlPortal_Common::ToJsStringLiteral(TokenJoinModeLabel_(r.keywordJoinMode));
            js += L",";
            js += L"useExclude:";
            js += EpgHtmlPortal_Common::ToJsBool(r.useExcludeKeyword);
            js += L",";
            js += L"excludeJoinMode:";
            js += EpgHtmlPortal_Common::ToJsStringLiteral(TokenJoinModeLabel_(r.excludeKeywordJoinMode));
            js += L",";
            js += L"useService:";
            js += EpgHtmlPortal_Common::ToJsBool(r.useService);
            js += L",";
            js += L"useGenre:";
            js += EpgHtmlPortal_Common::ToJsBool(r.useGenre);
            js += L",";
            js += L"genreJoinMode:";
            js += EpgHtmlPortal_Common::ToJsStringLiteral(TokenJoinModeLabel_(r.genreMatchMode));
            js += L",";
            js += L"useMinDuration:";
            js += EpgHtmlPortal_Common::ToJsBool(r.useMinDuration);
            js += L",minDurationMinutes:";
            js += std::to_wstring(r.minDurationMinutes);
            js += L",";
            js += L"timeRangeMode:";
            js += r.useSearchTimeRange ? L"\"custom\"" : L"\"all\"";
            js += L",";
            js += L"searchStartMinutes:";
            js += std::to_wstring(r.useSearchTimeRange ? r.searchStartMinutes : 0);
            js += L",";
            js += L"searchEndMinutes:";
            js += std::to_wstring(r.useSearchTimeRange ? r.searchEndMinutes : 1440);
            js += L",";
            js += L"searchTimeWrap:";
            js += EpgHtmlPortal_Common::ToJsBool(r.searchTimeWrap);
            js += L",weekdayMode:";
            js += r.useWeekday ? L"\"custom\"" : L"\"all\"";
            js += L",weekdays:";
            js += EpgHtmlPortal_Common::ToJsStringLiteral(r.weekdays.empty() ? std::wstring(L"1111111") : r.weekdays);
            js += L",skipDuplicate:";
            js += EpgHtmlPortal_Common::ToJsBool(r.skipDuplicate);
            js += L",excludeRerun:";
            js += EpgHtmlPortal_Common::ToJsBool(r.excludeRerunLikeProgram);
            js += L",actionMode:";
            js += EpgHtmlPortal_Common::ToJsStringLiteral(ReserveActionModeLabel_(r.actionMode));
            js += L"}";
        }

        static std::wstring BuildInitialRulesScript_(
            const std::vector<AutoReserve::Rule>& rules)
        {
            std::wstring js;
            js.reserve(4096 + rules.size() * 640);

            js += L"<script>\n";
            js += L"window.EPG_AUTO_RESERVE_INITIAL_RULES=[\n";

            for (std::size_t i = 0; i < rules.size(); ++i) {
                js += L"  ";
                AppendRuleJsObject_(js, rules[i]);

                if (i + 1 < rules.size()) {
                    js += L",";
                }

                js += L"\n";
            }

            js += L"];\n";
            js += L"</script>\n";

            return js;
        }

        static void AppendPageStyle_(
            std::wstring& html)
        {
            html += L"<style>\n";
            html += L"*{box-sizing:border-box;}\n";
            html += L"html,body{margin:0;padding:0;}\n";
            html += L"body{font-family:\"Yu Gothic UI\",\"Meiryo\",sans-serif;background:#0f1115;color:#e8ecf1;}\n";
            html += L".shell{max-width:1580px;margin:0 auto;padding:16px;}\n";
            html += L".header,.card{background:#171b22;border:1px solid #2a313d;border-radius:14px;padding:14px;margin-bottom:12px;box-shadow:0 8px 22px rgba(0,0,0,.18);}\n";
            html += L".header{display:flex;justify-content:space-between;gap:12px;align-items:flex-start;flex-wrap:wrap;}\n";
            html += L".headerMain{min-width:320px;flex:1;}\n";
            html += L".headerMeta{font-size:12px;color:#aeb8c6;background:#11161f;border:1px solid #2d3646;border-radius:12px;padding:10px 12px;max-width:620px;line-height:1.55;}\n";
            html += L"h1{font-size:24px;margin:0 0 8px 0;}\n";
            html += L"h2{font-size:18px;margin:0 0 10px 0;}\n";
            html += L"h3{font-size:15px;margin:0;color:#eef3fb;}\n";
            html += L"p{line-height:1.65;color:#c3cad4;margin:6px 0;}\n";
            html += L".note{font-size:12px;color:#aeb8c6;}\n";
            html += L".assistNotice{display:none;margin:9px 0 0;padding:9px 11px;border:1px solid #355f96;border-radius:10px;background:#13243a;color:#d9e8fb;font-size:13px;line-height:1.5;}\n";
            html += L".assistNotice.visible{display:block;}\n";
            html += L".editorCard{padding:12px 14px 14px;}\n";
            html += L".editorTop{display:grid;grid-template-columns:170px minmax(260px,1fr) minmax(260px,.9fr);gap:10px;align-items:end;margin-top:10px;}\n";
            html += L".switchBox{background:#11161f;border:1px solid #2d3646;border-radius:12px;padding:9px;display:grid;gap:7px;}\n";
            html += L".consoleGrid{display:grid;grid-template-columns:minmax(300px,.95fr) minmax(430px,1.35fr) minmax(340px,1.05fr);gap:12px;margin-top:12px;align-items:stretch;}\n";
            html += L".conditionConsole{background:#11161f;border:1px solid #2d3646;border-radius:14px;padding:11px;min-width:0;display:flex;flex-direction:column;}\n";
            html += L".conditionConsole.text{border-top:3px solid #4b8dff;}\n";
            html += L".conditionConsole.target{border-top:3px solid #58c087;}\n";
            html += L".conditionConsole.limit{border-top:3px solid #d6a756;}\n";
            html += L".consoleHead{display:flex;justify-content:space-between;gap:8px;align-items:flex-start;margin-bottom:10px;border-bottom:1px solid #283241;padding-bottom:8px;}\n";
            html += L".consoleSub{font-size:11px;color:#9aa8bc;text-align:right;line-height:1.35;}\n";
            html += L".consoleBody{display:grid;gap:10px;align-content:start;}\n";
            html += L".twoCols{display:grid;grid-template-columns:1fr 1fr;gap:9px;}\n";
            html += L".selectPair{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:10px;align-items:start;}\n";
            html += L".wide{grid-column:1/-1;}\n";
            html += L"label{display:grid;gap:5px;font-size:13px;color:#d7dce5;min-width:0;}\n";
            html += L"input,select{width:100%;border:1px solid #384252;border-radius:8px;background:#0f131a;color:#e9eef7;padding:8px 9px;font:inherit;min-width:0;}\n";
            html += L"select[multiple]{min-height:174px;}\n";
            html += L"input[type=\"checkbox\"],input[type=\"radio\"]{width:auto;}\n";
            html += L"input:disabled,select:disabled{opacity:.55;cursor:not-allowed;}\n";
            html += L".inlineCheck{display:flex;gap:7px;align-items:center;white-space:nowrap;}\n";
            html += L".statusSwitch{display:inline-flex;align-items:center;gap:6px;width:max-content;max-width:100%;cursor:pointer;user-select:none;white-space:nowrap;}\n";
            html += L".statusSwitch input{position:absolute;inline-size:1px;block-size:1px;opacity:0;pointer-events:none;}\n";
            html += L".statusSwitchTrack{position:relative;display:inline-block;flex:0 0 auto;width:36px;height:20px;border:1px solid #515b69;border-radius:999px;background:#303641;transition:background-color .14s ease,border-color .14s ease;}\n";
            html += L".statusSwitchTrack::after{content:'';position:absolute;left:2px;top:2px;width:14px;height:14px;border-radius:50%;background:#c9d0da;transition:transform .14s ease,background-color .14s ease;}\n";
            html += L".statusSwitch input:checked+.statusSwitchTrack{background:#216b48;border-color:#3b9266;}\n";
            html += L".statusSwitch input:checked+.statusSwitchTrack::after{transform:translateX(16px);background:#effbf4;}\n";
            html += L".statusSwitch input:focus-visible+.statusSwitchTrack{outline:2px solid #8fb7ff;outline-offset:2px;}\n";
            html += L".statusSwitch input:disabled+.statusSwitchTrack{opacity:.62;cursor:wait;}\n";
            html += L".statusSwitchText{min-width:34px;font-size:12px;font-weight:600;color:#9fa9b7;}\n";
            html += L".statusSwitch input:checked~.statusSwitchText{color:#75c99a;}\n";
            html += L".statusSwitch.saving .statusSwitchText{color:#f0c978;}\n";
            html += L".statusSwitchField{align-self:center;padding:3px 0;}\n";
            html += L".radioLine,.checkLine{display:flex;gap:10px 14px;flex-wrap:wrap;align-items:center;background:#0f131a;border:1px solid #283241;border-radius:10px;padding:8px;}\n";
            html += L".radioLine label,.checkLine label{display:flex;gap:6px;align-items:center;}\n";
            html += L".groupTitle{font-size:12px;color:#aeb8c6;margin-bottom:4px;}\n";
            html += L".helpText{font-size:12px;color:#97a4b7;line-height:1.45;}\n";
            html += L".selectedText{display:block;font-size:12px;color:#cbd6e6;line-height:1.45;max-height:42px;overflow:auto;border-top:1px solid #283241;margin-top:6px;padding-top:5px;}\n";
            html += L".compactRow{display:grid;grid-template-columns:1fr 1fr;gap:9px;}\n";
            html += L".previewConsole{margin-top:12px;background:#101720;border:1px solid #314156;border-radius:14px;padding:11px;}\n";
            html += L".previewHead{display:flex;justify-content:space-between;gap:10px;align-items:center;border-bottom:1px solid #29384b;padding-bottom:8px;margin-bottom:8px;}\n";
            html += L".conditionPreviewDetails{padding:0;overflow:hidden;}\n";
            html += L".conditionPreviewDetails>summary{cursor:pointer;list-style:none;padding:11px 13px;font-size:15px;font-weight:700;color:#eef3fb;user-select:none;}\n";
            html += L".conditionPreviewDetails>summary::-webkit-details-marker{display:none;}\n";
            html += L".conditionPreviewDetails>summary::before{content:'▶';display:inline-block;margin-right:8px;color:#8fb7ff;transition:transform .15s ease;}\n";
            html += L".conditionPreviewDetails[open]>summary::before{transform:rotate(90deg);}\n";
            html += L".conditionPreviewDetails[open]>summary{border-bottom:1px solid #29384b;}\n";
            html += L".conditionPreviewSummaryNote{font-size:12px;font-weight:400;color:#9aa8bc;margin-left:8px;}\n";
            html += L".conditionPreviewBody{padding:11px;}\n";
            html += L".previewList{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px;}\n";
            html += L".previewItem{background:#0f131a;border:1px solid #263245;border-radius:10px;padding:8px;min-width:0;}\n";
            html += L".previewLabel{display:block;font-size:11px;color:#97a4b7;margin-bottom:3px;}\n";
            html += L".previewValue{font-size:13px;color:#e8ecf1;line-height:1.45;word-break:break-word;}\n";
            html += L".actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px;}\n";
            html += L"button{border:1px solid #384252;background:#232936;color:#e9eef7;border-radius:8px;padding:8px 12px;font:inherit;cursor:pointer;}\n";
            html += L"button:hover{background:#2c3544;border-color:#4b5a70;}\n";
            html += L".primary{background:#2d6cdf;border-color:#2d6cdf;color:#fff;}\n";
            html += L".danger{background:#68303b;border-color:#884656;}\n";
            html += L".ruleGrid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;}\n";
            html += L".listHead{display:flex;justify-content:space-between;gap:10px;align-items:flex-start;flex-wrap:wrap;}\n";
            html += L".manageLink{display:inline-block;color:#e8ecf1;text-decoration:none;border:1px solid #384252;background:#232936;border-radius:8px;padding:8px 11px;font-size:13px;}\n";
            html += L".manageLink:hover{background:#2c3544;border-color:#4b5a70;text-decoration:none;}\n";
            html += L".simpleRuleList{display:grid;gap:8px;}\n";
            html += L".simpleRuleRow{display:flex;justify-content:space-between;gap:12px;align-items:center;background:#11161f;border:1px solid #2d3646;border-radius:10px;padding:10px 12px;min-width:0;}\n";
            html += L".simpleRuleRow.disabled .simpleRuleTitle{opacity:.58;}\n";
            html += L".simpleRuleTitle{color:#e8ecf1;text-decoration:none;font-weight:700;word-break:break-word;min-width:0;}\n";
            html += L".simpleRuleTitle:hover{text-decoration:underline;}\n";
            html += L".simpleRuleSwitch{flex:0 0 auto;}\n";
            html += L".ruleCard{background:#11161f;border:1px solid #2d3646;border-radius:14px;padding:11px;display:grid;gap:10px;min-width:0;}\n";
            html += L".ruleCard.disabled{opacity:.62;}\n";
            html += L".ruleTop{display:flex;justify-content:space-between;gap:8px;align-items:flex-start;border-bottom:1px solid #283241;padding-bottom:8px;}\n";
            html += L".ruleTitle{font-weight:700;color:#eef3fb;word-break:break-word;}\n";
            html += L".ruleMeta{display:flex;gap:5px;flex-wrap:wrap;justify-content:flex-end;}\n";
            html += L".ruleBody{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:7px;}\n";
            html += L".ruleField{background:#0f131a;border:1px solid #263245;border-radius:10px;padding:7px;min-width:0;}\n";
            html += L".ruleLabel{display:block;font-size:11px;color:#97a4b7;margin-bottom:3px;}\n";
            html += L".ruleValue{font-size:13px;color:#e8ecf1;line-height:1.4;word-break:break-word;}\n";
            html += L".ruleActions{display:flex;gap:7px;flex-wrap:wrap;}\n";
            html += L".pill{display:inline-block;border:1px solid #3d4a5d;border-radius:999px;padding:2px 7px;font-size:12px;color:#cbd6e6;margin:1px;}\n";
            html += L".pillOn{background:#173d29;border-color:#2d7a4f;color:#dff8ea;}\n";
            html += L".pillOff{background:#3b2730;border-color:#7a4655;color:#ffdce5;}\n";
            html += L"textarea{width:100%;min-height:220px;border:1px solid #384252;border-radius:10px;background:#0f131a;color:#e9eef7;padding:10px;font-family:Consolas,\"Yu Gothic UI\",monospace;}\n";
            html += L"@media(max-width:1240px){.consoleGrid{grid-template-columns:1fr 1fr;}.conditionConsole.limit{grid-column:1/-1;}.editorTop{grid-template-columns:150px minmax(240px,1fr);}.editorTop .switchBox{grid-column:1/-1;}.previewList{grid-template-columns:1fr;}}\n";
            html += L"@media(max-width:860px){.consoleGrid,.selectPair,.twoCols,.compactRow,.ruleGrid,.ruleBody{grid-template-columns:1fr;}.shell{padding:10px;}.header{display:block;}.headerMeta{max-width:none;margin-top:10px;}.editorTop{grid-template-columns:1fr;}select[multiple]{min-height:150px;}}\n";
            html += L"</style>\n";
        }

        static void AppendFormHtml_(
            std::wstring& html,
            const std::vector<ConfigServiceOption_>& services)
        {
            html += L"<section class=\"card editorCard\">";
            html += L"<h2>自動予約設定を作る / 編集する</h2>";
            html += L"<p class=\"note\">探す言葉・探す範囲・絞り込みを組み合わせて、番組表から予約候補を探す設定を作ります。</p>";
            html += L"<p id=\"autoReserveAssistNotice\" class=\"assistNotice\" role=\"status\" aria-live=\"polite\"></p>";
            html += L"<input id=\"editId\" type=\"hidden\" value=\"0\">";

            html += L"<div class=\"editorTop\">";
            html += L"<label class=\"statusSwitch statusSwitchField\"><input id=\"enabled\" type=\"checkbox\" checked role=\"switch\" aria-label=\"この自動予約設定の有効状態\"><span class=\"statusSwitchTrack\" aria-hidden=\"true\"></span><span id=\"enabledStatusText\" class=\"statusSwitchText\">有効</span></label>";
            html += L"<label>この自動予約設定の名前<input id=\"ruleName\" type=\"text\" placeholder=\"例: 深夜アニメを探す\"></label>";
            html += L"<div class=\"switchBox\"><div class=\"groupTitle\">登録動作</div>";
            html += L"<div class=\"radioLine\">";
            html += L"<label><input type=\"radio\" name=\"actionMode\" value=\"candidate\" checked>候補として表示</label>";
            html += L"<label><input type=\"radio\" name=\"actionMode\" value=\"auto\">自動で予約登録</label>";
            html += L"</div></div>";
            html += L"</div>";

            html += L"<div class=\"consoleGrid\">";

            html += L"<section class=\"conditionConsole text\">";
            html += L"<div class=\"consoleHead\"><h3>① 探す言葉</h3><div class=\"consoleSub\">番組名・概要から探す</div></div>";
            html += L"<div class=\"consoleBody\">";
            html += L"<label class=\"inlineCheck\"><input id=\"useKeyword\" type=\"checkbox\" checked>番組名や説明に含まれる言葉で探す</label>";
            html += L"<label>含まれる言葉<input id=\"keyword\" type=\"text\" placeholder=\"例: ガンダム|映画|特番\"><span class=\"helpText\">| 区切りで複数指定できます。</span></label>";
            html += L"<div><div class=\"groupTitle\">含まれる言葉のつなぎ方</div><div class=\"radioLine\">";
            html += L"<label><input type=\"radio\" name=\"keywordJoinMode\" value=\"OR\" checked>いずれかに一致</label>";
            html += L"<label><input type=\"radio\" name=\"keywordJoinMode\" value=\"AND\">すべてに一致</label>";
            html += L"</div></div>";
            html += L"<label class=\"inlineCheck\"><input id=\"useExclude\" type=\"checkbox\">除外する言葉を使う</label>";
            html += L"<label>除外する言葉<input id=\"excludeKeyword\" type=\"text\" placeholder=\"例: 再放送|通販|番宣\"></label>";
            html += L"<div><div class=\"groupTitle\">除外の判定</div><div class=\"radioLine\">";
            html += L"<label><input type=\"radio\" name=\"excludeJoinMode\" value=\"OR\" checked>いずれかを含んだら除外</label>";
            html += L"<label><input type=\"radio\" name=\"excludeJoinMode\" value=\"AND\">すべて含んだら除外</label>";
            html += L"</div></div>";
            html += L"<label>言葉の一致方法<select id=\"titleMatchMode\">";
            html += L"<option value=\"0\">部分一致</option><option value=\"1\">完全一致</option><option value=\"2\">前方一致</option><option value=\"3\">後方一致</option>";
            html += L"</select></label>";
            html += L"<div><div class=\"groupTitle\">検索する場所</div><div class=\"checkLine\">";
            html += L"<label><input id=\"matchTitle\" type=\"checkbox\" checked>番組タイトル</label>";
            html += L"<label><input id=\"matchSummary\" type=\"checkbox\" checked>番組概要</label>";
            html += L"<label><input id=\"matchExtended\" type=\"checkbox\">詳細説明</label>";
            html += L"</div></div>";
            html += L"</div></section>";

            html += L"<section class=\"conditionConsole target\">";
            html += L"<div class=\"consoleHead\"><h3>② 探す範囲</h3><div class=\"consoleSub\">放送波・放送局・ジャンル</div></div>";
            html += L"<div class=\"consoleBody\">";
            html += L"<label>対象の放送波<select id=\"targetSpace\"><option value=\"0\">地上波</option><option value=\"1\">BS</option><option value=\"2\">CS</option><option value=\"-1\">すべて</option></select><span class=\"helpText\">現在の環境では基本的に地上波用です。</span></label>";
            html += L"<div class=\"selectPair\">";
            html += L"<label>放送局<label class=\"inlineCheck\"><input id=\"useService\" type=\"checkbox\">選んだ放送局だけ</label><select id=\"serviceSelect\" multiple>";
            AppendConfigServiceOptionsHtml_(html, services);
            html += L"</select><span class=\"helpText\">放送局は複数選択時も、いずれかの局に一致すれば対象です。</span><span id=\"serviceSelectedText\" class=\"selectedText\"></span></label>";
            html += L"<label>ジャンル<label class=\"inlineCheck\"><input id=\"useGenre\" type=\"checkbox\">選んだジャンルだけ</label><select id=\"genreSelect\" multiple>";
            AppendGenreOptionsHtml_(html);
            html += L"</select><span id=\"genreSelectedText\" class=\"selectedText\"></span></label>";
            html += L"</div>";
            html += L"<div><div class=\"groupTitle\">ジャンルの判定</div><div class=\"radioLine\">";
            html += L"<label><input type=\"radio\" name=\"genreJoinMode\" value=\"OR\" checked>いずれかに一致</label>";
            html += L"<label><input type=\"radio\" name=\"genreJoinMode\" value=\"AND\">すべてに一致</label>";
            html += L"</div></div>";
            html += L"</div></section>";

            html += L"<section class=\"conditionConsole limit\">";
            html += L"<div class=\"consoleHead\"><h3>③ 絞り込み</h3><div class=\"consoleSub\">短い番組・時間帯・曜日</div></div>";
            html += L"<div class=\"consoleBody\">";
            html += L"<label class=\"inlineCheck\"><input id=\"useMinDuration\" type=\"checkbox\">短い番組を除外する</label>";
            html += L"<label>最低放送時間<input id=\"minDurationMinutes\" type=\"number\" value=\"30\" min=\"0\" max=\"1440\"><span class=\"helpText\">チェック時だけ、この分数以上の番組を対象にします。</span></label>";
            html += L"<div><div class=\"groupTitle\">検索する時間帯</div><div class=\"radioLine\">";
            html += L"<label><input type=\"radio\" name=\"timeRangeMode\" value=\"all\" checked>すべての時間帯</label>";
            html += L"<label><input type=\"radio\" name=\"timeRangeMode\" value=\"custom\">時間帯を指定</label>";
            html += L"</div></div>";
            html += L"<div class=\"compactRow\">";
            html += L"<label>開始<select id=\"searchStartMinutes\">";
            AppendTimeOptionsHtml_(html, 0, 1410, 30);
            html += L"</select></label>";
            html += L"<label>終了<select id=\"searchEndMinutes\">";
            AppendTimeOptionsHtml_(html, 30, 1440, 30);
            html += L"</select></label>";
            html += L"</div>";
            html += L"<label class=\"inlineCheck\"><input id=\"searchTimeWrap\" type=\"checkbox\">日付をまたぐ時間帯として扱う</label>";
            html += L"<div><div class=\"groupTitle\">曜日</div><div class=\"radioLine\">";
            html += L"<label><input type=\"radio\" name=\"weekdayMode\" value=\"all\" checked>すべての曜日</label>";
            html += L"<label><input type=\"radio\" name=\"weekdayMode\" value=\"custom\">曜日を指定</label>";
            html += L"</div></div>";
            html += L"<div class=\"checkLine\">";
            html += L"<label><input id=\"wdMon\" class=\"weekdayCheck\" type=\"checkbox\" checked>月</label>";
            html += L"<label><input id=\"wdTue\" class=\"weekdayCheck\" type=\"checkbox\" checked>火</label>";
            html += L"<label><input id=\"wdWed\" class=\"weekdayCheck\" type=\"checkbox\" checked>水</label>";
            html += L"<label><input id=\"wdThu\" class=\"weekdayCheck\" type=\"checkbox\" checked>木</label>";
            html += L"<label><input id=\"wdFri\" class=\"weekdayCheck\" type=\"checkbox\" checked>金</label>";
            html += L"<label><input id=\"wdSat\" class=\"weekdayCheck\" type=\"checkbox\" checked>土</label>";
            html += L"<label><input id=\"wdSun\" class=\"weekdayCheck\" type=\"checkbox\" checked>日</label>";
            html += L"</div>";
            html += L"<div class=\"twoCols\">";
            html += L"<label>優先度<input id=\"priority\" type=\"number\" value=\"50\" min=\"0\" max=\"100\"></label>";
            html += L"<label>重複処理<select id=\"duplicatePolicy\"><option value=\"0\">同一イベントを除外</option><option value=\"1\">同一タイトル/時刻を除外</option><option value=\"2\">重複許可</option></select></label>";
            html += L"</div>";
            html += L"<div class=\"twoCols\">";
            html += L"<label>前マージン秒<input id=\"preMarginSeconds\" type=\"number\" value=\"0\" min=\"0\" max=\"3600\"></label>";
            html += L"<label>後マージン秒<input id=\"postMarginSeconds\" type=\"number\" value=\"0\" min=\"0\" max=\"3600\"></label>";
            html += L"</div>";
            html += L"<div><div class=\"groupTitle\">安全設定</div><div class=\"checkLine\">";
            html += L"<label><input id=\"skipDuplicate\" type=\"checkbox\" checked>既存予約と重複した番組は登録しない</label>";
            html += L"<label><input id=\"excludeRerun\" type=\"checkbox\">再放送らしい番組を除外</label>";
            html += L"<label><input id=\"createReservationEnabled\" type=\"checkbox\" checked>作成する予約を有効状態にする</label>";
            html += L"</div></div>";
            html += L"</div></section>";

            html += L"</div>";

            html += L"<details id=\"conditionPreviewDetails\" class=\"previewConsole conditionPreviewDetails\">";
            html += L"<summary>この設定で探す番組<span class=\"conditionPreviewSummaryNote\">入力中の条件を表示</span></summary>";
            html += L"<div class=\"conditionPreviewBody\">";
            html += L"<div id=\"conditionPreview\" class=\"previewList\"></div>";
            html += L"</div>";
            html += L"</details>";

            html += L"<div class=\"actions\">";
            html += L"<button type=\"button\" class=\"primary\" onclick=\"addOrUpdateRule()\">この設定を auto_reserve_rules.ini へ保存</button>";
            html += L"<button type=\"button\" onclick=\"resetForm()\">入力をクリア</button>";
            html += L"<button type=\"button\" onclick=\"reloadInitialRules()\">INI読込状態に戻す</button>";
            html += L"<button type=\"button\" onclick=\"buildIniPreview()\">INIプレビュー作成</button>";
            html += L"<button type=\"button\" onclick=\"downloadIniFile()\">専用INIを書き出し</button>";
            html += L"<button type=\"button\" onclick=\"document.getElementById('iniFileInput').click()\">専用INIを読み込み</button>";
            html += L"<input id=\"iniFileInput\" type=\"file\" accept=\".ini,text/plain\" style=\"display:none\">";
            html += L"</div>";
            html += L"</section>";
        }

        static void AppendListHtml_(
            std::wstring& html)
        {
            html += L"<section class=\"card\">";
            html += L"<div class=\"listHead\">";
            html += L"<div>";
            html += L"<h2>登録済み自動予約設定</h2>";
            html += L"<p id=\"summary\" class=\"note\">読み込み中です。</p>";
            html += L"</div>";
            html += L"</div>";
            html += L"<p class=\"note\">ここではタイトルと有効/無効だけを表示します。タイトルをクリックすると、そのルールだけの詳細編集ページを開きます。</p>";
            html += L"<div id=\"ruleRows\" class=\"simpleRuleList\"></div>";
            html += L"</section>";

            html += L"<section class=\"card\">";
            html += L"<h2>INIプレビュー</h2>";
            html += L"<p class=\"note\">専用の auto_reserve_rules.ini として書き出し/読み込みできます。保存ボタンはローカルAPI経由で auto_reserve_rules.ini へ直接保存します。API未接続時は専用INIを書き出してください。</p>";
            html += L"<textarea id=\"iniPreview\" readonly></textarea>";
            html += L"</section>";
        }

        static void AppendPageScript_(
            std::wstring& html)
        {
            html += L"<script>\n";
            html += L"(function(){\n";
            html += L"'use strict';\n";
            html += L"var hostResizeQueued=false;\n";
            html += L"var lastHostFrameHeight=0;\n";
            html += L"function getPageContentHeight(){var shell=document.querySelector('.shell');if(shell){var rect=shell.getBoundingClientRect();return Math.max(1,Math.ceil(rect.bottom+window.scrollY+4));}var body=document.body;return Math.max(1,Math.ceil(body?body.scrollHeight:1));}\n";
            html += L"function syncHostFrameHeight(){if(hostResizeQueued)return;hostResizeQueued=true;window.requestAnimationFrame(function(){hostResizeQueued=false;var h=getPageContentHeight();if(Math.abs(h-lastHostFrameHeight)<2)return;lastHostFrameHeight=h;try{if(window.parent&&window.parent!==window){var frames=window.parent.document.getElementsByTagName('iframe');for(var i=0;i<frames.length;i++){if(frames[i].contentWindow===window){var frame=frames[i];var current=parseInt(frame.style.height||frame.getAttribute('height')||'0',10)||0;if(Math.abs(current-h)>=2){frame.style.height=String(h)+'px';frame.style.minHeight='0';frame.setAttribute('height',String(h));}break;}}}}catch(e){}try{if(window.parent&&window.parent!==window){window.parent.postMessage({type:'epg-portal-content-height',page:'auto-reserve',height:h},location.origin);}}catch(e2){}});}\n";
            html += L"function installHostFrameAutoResize(){syncHostFrameHeight();var shell=document.querySelector('.shell');if(window.ResizeObserver&&shell){var ro=new ResizeObserver(function(){syncHostFrameHeight();});ro.observe(shell);window.__epgAutoResizeObserver=ro;}else if(window.MutationObserver&&shell){var mo=new MutationObserver(function(){syncHostFrameHeight();});mo.observe(shell,{childList:true,subtree:true,attributes:true,characterData:true});window.__epgAutoMutationObserver=mo;}window.addEventListener('resize',syncHostFrameHeight);}\n";
            html += L"var LEGACY_KEYS=['epg.autoReserve.rules.v1','epg.autoReserve.rules.v2','epg.autoReserve.rules.v3'];\n";
            html += L"function deleteLegacyBrowserSave(){try{LEGACY_KEYS.forEach(function(k){localStorage.removeItem(k);});}catch(e){}}\n";
            html += L"function byId(id){return document.getElementById(id);}\n";
            html += L"function def(v,d){return (v===undefined||v===null)?d:v;}\n";
            html += L"function esc(s){return String(def(s,'')).replace(/[&<>\"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c];});}\n";
            html += L"function n(id,d){var el=byId(id);var v=el?Number(el.value):Number.NaN;return Number.isFinite(v)?v:d;}\n";
            html += L"function b(id){var el=byId(id);return !!(el&&el.checked);}\n";
            html += L"function s(id){var el=byId(id);return String(el?el.value:'').trim();}\n";
            html += L"function syncEnabledSwitchText(){var sw=byId('enabled');var text=byId('enabledStatusText');if(sw)sw.setAttribute('aria-checked',sw.checked?'true':'false');if(text)text.textContent=sw&&sw.checked?'有効':'無効';}\n";
            html += L"function getRadio(name,d){var el=document.querySelector('input[name=\"'+name+'\"]:checked');return el?el.value:d;}\n";
            html += L"function setRadio(name,value){var list=document.querySelectorAll('input[name=\"'+name+'\"]');list.forEach(function(el){el.checked=String(el.value)===String(value);});}\n";
            html += L"function splitValues(text){return String(def(text,'')).split('|').map(function(x){return x.trim();}).filter(function(x){return x.length>0;});}\n";
            html += L"function joinValues(list){return list.map(function(x){return String(x).trim();}).filter(function(x){return x.length>0;}).join('|');}\n";
            html += L"function displayMulti(text,emptyText,joinMode){var a=splitValues(text);if(!a.length)return emptyText;var sep=joinMode==='AND'?' AND ':' OR ';return a.join(sep);}\n";
            html += L"function getMulti(id){var el=byId(id);if(!el)return '';var a=[];for(var i=0;i<el.options.length;i++){if(el.options[i].selected&&el.options[i].value){a.push(el.options[i].value);}}return joinValues(a);}\n";
            html += L"function setMulti(id,pipeText){var el=byId(id);if(!el)return;var values=splitValues(pipeText);for(var i=0;i<el.options.length;i++){el.options[i].selected=values.indexOf(el.options[i].value)>=0;}}\n";
            html += L"function load(){deleteLegacyBrowserSave();return [];}\n";
            html += L"function cloneRules(list){return JSON.parse(JSON.stringify((Array.isArray(list)?list:[]).map(normalizeRule)));}\n";
            html += L"function restoreRules(before){rules=Array.isArray(before)?before.map(normalizeRule):[];render();buildIniPreview();}\n";
            html += L"function save(){return saveIniToApp(false,false);}\n";
            html += L"var rules=load();\n";
            html += L"function nextId(){var m=0;for(var i=0;i<rules.length;i++){m=Math.max(m,Number(rules[i].id)||0);}return m+1;}\n";
            html += L"function spaceLabel(v){v=Number(v);if(v===-1)return 'すべて';if(v===0)return '地上波';if(v===1)return 'BS';if(v===2)return 'CS';return '不明';}\n";
            html += L"function matchLabel(v){v=Number(v);if(v===1)return '完全一致';if(v===2)return '前方一致';if(v===3)return '後方一致';return '部分一致';}\n";
            html += L"function dupLabel(v){v=Number(v);if(v===1)return '同一タイトル/時刻を除外';if(v===2)return '重複を許可';return '同一イベントを除外';}\n";
            html += L"function timeLabel(m){m=Number(m);if(m===1440)return '24:00';if(!Number.isFinite(m)||m<0)return '--:--';var h=Math.floor(m/60);var mm=m%60;return String(h).padStart(2,'0')+':'+String(mm).padStart(2,'0');}\n";
            html += L"function timeText(r){if(def(r.timeRangeMode,'all')!=='custom')return 'すべての時間帯';var t=timeLabel(def(r.searchStartMinutes,0))+' ～ '+timeLabel(def(r.searchEndMinutes,1440));if(r.searchTimeWrap)t+='（日付またぎ）';return t;}\n";
            html += L"function weekdayText(r){if(def(r.weekdayMode,'all')!=='custom')return 'すべての曜日';var names=['月','火','水','木','金','土','日'];var bits=String(def(r.weekdays,'1111111'));var out=[];for(var i=0;i<7;i++){if(bits.charAt(i)==='1')out.push(names[i]);}return out.length?out.join(' '):'未選択';}\n";
            html += L"function normalizeRule(r){r=r||{};var nr={};nr.id=Number(def(r.id,0))||0;nr.enabled=!!def(r.enabled,true);nr.name=String(def(r.name,''));nr.keyword=String(def(r.keyword,''));nr.excludeKeyword=String(def(r.excludeKeyword,''));nr.titleMatchMode=Number(def(r.titleMatchMode,0))||0;nr.matchTitle=!!def(r.matchTitle,true);nr.matchSummary=!!def(r.matchSummary,false);nr.matchExtended=!!def(r.matchExtended,false);nr.targetSpace=Number(def(r.targetSpace,0));nr.serviceName=String(def(r.serviceName,''));nr.genreName=String(def(r.genreName,''));nr.priority=Number(def(r.priority,50))||50;nr.preMarginSeconds=Number(def(r.preMarginSeconds,0))||0;nr.postMarginSeconds=Number(def(r.postMarginSeconds,0))||0;nr.createReservationEnabled=!!def(r.createReservationEnabled,true);nr.duplicatePolicy=Number(def(r.duplicatePolicy,0))||0;nr.useKeyword=!!def(r.useKeyword,nr.keyword.length>0);nr.keywordJoinMode=String(def(r.keywordJoinMode,'OR'));nr.useExclude=!!def(r.useExclude,nr.excludeKeyword.length>0);nr.excludeJoinMode=String(def(r.excludeJoinMode,'OR'));nr.useService=!!def(r.useService,nr.serviceName.length>0);nr.useGenre=!!def(r.useGenre,nr.genreName.length>0);nr.genreJoinMode=String(def(r.genreJoinMode,'OR'));nr.useMinDuration=!!def(r.useMinDuration,false);nr.minDurationMinutes=Number(def(r.minDurationMinutes,0))||0;var hasOldTime=Number(def(r.startMinutes,-1))>=0&&Number(def(r.endMinutes,-1))>=0;nr.timeRangeMode=String(def(r.timeRangeMode,hasOldTime?'custom':'all'));nr.searchStartMinutes=Number(def(r.searchStartMinutes,hasOldTime?r.startMinutes:0));nr.searchEndMinutes=Number(def(r.searchEndMinutes,hasOldTime?r.endMinutes:1440));nr.searchTimeWrap=!!def(r.searchTimeWrap,false);nr.startMinutes=nr.timeRangeMode==='custom'?nr.searchStartMinutes:-1;nr.endMinutes=nr.timeRangeMode==='custom'?nr.searchEndMinutes:-1;nr.weekdayMode=String(def(r.weekdayMode,'all'));nr.weekdays=String(def(r.weekdays,'1111111'));nr.skipDuplicate=!!def(r.skipDuplicate,true);nr.excludeRerun=!!def(r.excludeRerun,false);nr.actionMode=String(def(r.actionMode,'candidate'));return nr;}\n";
            html += L"function updateSelectedTexts(){var st=byId('serviceSelectedText');var gt=byId('genreSelectedText');if(st)st.textContent='選択中: '+displayMulti(getMulti('serviceSelect'),'すべての放送局','OR');if(gt)gt.textContent='選択中: '+displayMulti(getMulti('genreSelect'),'すべてのジャンル',getRadio('genreJoinMode','OR'));}\n";
            html += L"function setGroupDisabled(selector,disabled){document.querySelectorAll(selector).forEach(function(el){el.disabled=disabled;});}\n";
            html += L"function updateUiState(){setGroupDisabled('#keyword,#titleMatchMode,input[name=\"keywordJoinMode\"],#matchTitle,#matchSummary,#matchExtended',!b('useKeyword'));setGroupDisabled('#excludeKeyword,input[name=\"excludeJoinMode\"]',!b('useExclude'));setGroupDisabled('#serviceSelect',!b('useService'));setGroupDisabled('#genreSelect,input[name=\"genreJoinMode\"]',!b('useGenre'));setGroupDisabled('#minDurationMinutes',!b('useMinDuration'));var timeCustom=getRadio('timeRangeMode','all')==='custom';setGroupDisabled('#searchStartMinutes,#searchEndMinutes,#searchTimeWrap',!timeCustom);var weekdayCustom=getRadio('weekdayMode','all')==='custom';setGroupDisabled('.weekdayCheck',!weekdayCustom);}\n";
            html += L"function readWeekdays(){var ids=['wdMon','wdTue','wdWed','wdThu','wdFri','wdSat','wdSun'];return ids.map(function(id){return b(id)?'1':'0';}).join('');}\n";
            html += L"function setWeekdays(bits){bits=String(def(bits,'1111111'));var ids=['wdMon','wdTue','wdWed','wdThu','wdFri','wdSat','wdSun'];for(var i=0;i<ids.length;i++){var el=byId(ids[i]);if(el)el.checked=bits.charAt(i)!=='0';}}\n";
            html += L"function readForm(){var timeMode=getRadio('timeRangeMode','all');var start=timeMode==='custom'?n('searchStartMinutes',0):-1;var end=timeMode==='custom'?n('searchEndMinutes',1440):-1;return normalizeRule({id:n('editId',0),enabled:b('enabled'),name:s('ruleName'),keyword:s('keyword'),excludeKeyword:s('excludeKeyword'),titleMatchMode:n('titleMatchMode',0),matchTitle:b('matchTitle'),matchSummary:b('matchSummary'),matchExtended:b('matchExtended'),targetSpace:n('targetSpace',0),serviceName:b('useService')?getMulti('serviceSelect'):'',genreName:b('useGenre')?getMulti('genreSelect'):'',startMinutes:start,endMinutes:end,priority:n('priority',50),preMarginSeconds:n('preMarginSeconds',0),postMarginSeconds:n('postMarginSeconds',0),createReservationEnabled:b('createReservationEnabled'),duplicatePolicy:n('duplicatePolicy',0),useKeyword:b('useKeyword'),keywordJoinMode:getRadio('keywordJoinMode','OR'),useExclude:b('useExclude'),excludeJoinMode:getRadio('excludeJoinMode','OR'),useService:b('useService'),useGenre:b('useGenre'),genreJoinMode:getRadio('genreJoinMode','OR'),useMinDuration:b('useMinDuration'),minDurationMinutes:n('minDurationMinutes',0),timeRangeMode:timeMode,searchStartMinutes:n('searchStartMinutes',0),searchEndMinutes:n('searchEndMinutes',1440),searchTimeWrap:b('searchTimeWrap'),weekdayMode:getRadio('weekdayMode','all'),weekdays:readWeekdays(),skipDuplicate:b('skipDuplicate'),excludeRerun:b('excludeRerun'),actionMode:getRadio('actionMode','candidate')});}\n";
            html += L"function writeForm(r0){var r=normalizeRule(r0);byId('editId').value=String(r.id);byId('enabled').checked=!!r.enabled;byId('ruleName').value=r.name;byId('useKeyword').checked=!!r.useKeyword;byId('keyword').value=r.keyword;setRadio('keywordJoinMode',r.keywordJoinMode);byId('useExclude').checked=!!r.useExclude;byId('excludeKeyword').value=r.excludeKeyword;setRadio('excludeJoinMode',r.excludeJoinMode);byId('titleMatchMode').value=String(r.titleMatchMode);byId('matchTitle').checked=!!r.matchTitle;byId('matchSummary').checked=!!r.matchSummary;byId('matchExtended').checked=!!r.matchExtended;byId('targetSpace').value=String(r.targetSpace);byId('useService').checked=!!r.useService;setMulti('serviceSelect',r.serviceName);byId('useGenre').checked=!!r.useGenre;setMulti('genreSelect',r.genreName);setRadio('genreJoinMode',r.genreJoinMode);byId('useMinDuration').checked=!!r.useMinDuration;byId('minDurationMinutes').value=String(r.minDurationMinutes);setRadio('timeRangeMode',r.timeRangeMode);byId('searchStartMinutes').value=String(r.searchStartMinutes);byId('searchEndMinutes').value=String(r.searchEndMinutes);byId('searchTimeWrap').checked=!!r.searchTimeWrap;setRadio('weekdayMode',r.weekdayMode);setWeekdays(r.weekdays);byId('priority').value=String(r.priority);byId('preMarginSeconds').value=String(r.preMarginSeconds);byId('postMarginSeconds').value=String(r.postMarginSeconds);byId('createReservationEnabled').checked=!!r.createReservationEnabled;byId('duplicatePolicy').value=String(r.duplicatePolicy);byId('skipDuplicate').checked=!!r.skipDuplicate;byId('excludeRerun').checked=!!r.excludeRerun;setRadio('actionMode',r.actionMode);syncEnabledSwitchText();updateUiState();updateSelectedTexts();renderConditionPreview();}\n";
            html += L"function previewItem(label,value){return '<div class=\"previewItem\"><span class=\"previewLabel\">'+esc(label)+'</span><div class=\"previewValue\">'+esc(value)+'</div></div>';}\n";
            html += L"function renderConditionPreview(){var box=byId('conditionPreview');if(!box)return;var r=readForm();var targets=[];if(r.matchTitle)targets.push('番組タイトル');if(r.matchSummary)targets.push('番組概要');if(r.matchExtended)targets.push('詳細説明');var html='';html+=previewItem('この設定',r.enabled?'使用する':'一時停止');html+=previewItem('探す言葉',r.useKeyword?displayMulti(r.keyword,'未入力',r.keywordJoinMode):'使わない');html+=previewItem('除外する言葉',r.useExclude?displayMulti(r.excludeKeyword,'未入力',r.excludeJoinMode):'使わない');html+=previewItem('検索する場所',r.useKeyword?(targets.join(' / ')||'未選択'):'言葉検索なし');html+=previewItem('対象の放送波',spaceLabel(r.targetSpace));html+=previewItem('放送局',r.useService?displayMulti(r.serviceName,'未選択','OR'):'すべての放送局');html+=previewItem('ジャンル',r.useGenre?displayMulti(r.genreName,'未選択',r.genreJoinMode):'すべてのジャンル');html+=previewItem('短い番組',r.useMinDuration?(String(r.minDurationMinutes||0)+'分未満を除外'):'除外しない');html+=previewItem('検索する時間帯',timeText(r));html+=previewItem('曜日',weekdayText(r));html+=previewItem('登録動作',r.actionMode==='auto'?'条件に合う番組を自動で予約登録':'条件に合う番組を候補として表示');html+=previewItem('安全設定',(r.skipDuplicate?'重複回避':'重複回避なし')+(r.excludeRerun?' / 再放送らしい番組を除外':''));box.innerHTML=html;}\n";
            html += L"function renderPills(pipeText,emptyText){var a=splitValues(pipeText);if(!a.length)return '<span class=\"pill\">'+esc(emptyText)+'</span>';return a.map(function(x){return '<span class=\"pill\">'+esc(x)+'</span>';}).join('');}\n";
            html += L"function renderField(label,value){return '<div class=\"ruleField\"><span class=\"ruleLabel\">'+esc(label)+'</span><div class=\"ruleValue\">'+value+'</div></div>';}\n";
            html += L"function detailUrlForRule(r){var id=Number(r&&r.id)||0;return 'epg_auto_reserve_rule_detail.html?id='+encodeURIComponent(String(id));}\n";
            html += L"function render(){";
            html += L"var wrap=byId('ruleRows');";
            html += L"if(!wrap)return;";
            html += L"wrap.innerHTML='';";
            html += L"var enabledCount=0;";
            html += L"for(var i=0;i<rules.length;i++){";
            html += L"rules[i]=normalizeRule(rules[i]);";
            html += L"var r=rules[i];";
            html += L"if(r.enabled)enabledCount++;";
            html += L"var row=document.createElement('div');";
            html += L"row.className='simpleRuleRow'+(r.enabled?'':' disabled');";
            html += L"var title=r.name||r.keyword||'(無題)';";
            html += L"row.innerHTML='<a class=\"simpleRuleTitle\" href=\"'+esc(detailUrlForRule(r))+'\">#'+esc(r.id)+' '+esc(title)+'</a><label class=\"statusSwitch simpleRuleSwitch\"><input class=\"ruleEnabledSwitch\" type=\"checkbox\" role=\"switch\" aria-label=\"'+esc(title)+' の有効状態\" aria-checked=\"'+(r.enabled?'true':'false')+'\" '+(r.enabled?'checked':'')+'><span class=\"statusSwitchTrack\" aria-hidden=\"true\"></span><span class=\"statusSwitchText\">'+(r.enabled?'有効':'無効')+'</span></label>';";
            html += L"(function(index,el){";
            html += L"var sw=el.querySelector('.ruleEnabledSwitch');";
            html += L"var switchBox=el.querySelector('.simpleRuleSwitch');";
            html += L"var label=el.querySelector('.statusSwitchText');";
            html += L"if(sw){sw.addEventListener('change',function(){";
            html += L"var before=cloneRules(rules);";
            html += L"rules[index].enabled=!!sw.checked;";
            html += L"sw.setAttribute('aria-checked',sw.checked?'true':'false');";
            html += L"sw.disabled=true;";
            html += L"if(switchBox)switchBox.classList.add('saving');";
            html += L"if(label)label.textContent='保存中';";
            html += L"el.classList.toggle('disabled',!rules[index].enabled);";
            html += L"buildIniPreview();";
            html += L"save().then(function(ok){";
            html += L"if(ok){";
            html += L"sw.disabled=false;";
            html += L"if(switchBox)switchBox.classList.remove('saving');";
            html += L"if(label)label.textContent=rules[index].enabled?'有効':'無効';";
            html += L"var enabledCount=0;for(var j=0;j<rules.length;j++){if(rules[j].enabled)enabledCount++;}";
            html += L"var sum=byId('summary');if(sum)sum.textContent='登録設定 '+rules.length+' 件 / 使用中 '+enabledCount+' 件';";
            html += L"syncHostFrameHeight();";
            html += L"}else{restoreRules(before);}";
            html += L"});";
            html += L"});}";
            html += L"})(i,row);";
            html += L"wrap.appendChild(row);";
            html += L"}";
            html += L"var sum=byId('summary');";
            html += L"if(sum)sum.textContent='登録設定 '+rules.length+' 件 / 使用中 '+enabledCount+' 件';";
            html += L"syncHostFrameHeight();";
            html += L"}\n";
            html += L"function addOrUpdateRule(){var before=cloneRules(rules);var r=readForm();var hasCondition=(r.useKeyword&&r.keyword)||(r.useService&&r.serviceName)||(r.useGenre&&r.genreName);if(!hasCondition&&!r.name){alert('探す言葉、放送局、ジャンルのどれかを設定してください。');return;}if(!r.name){if(r.keyword)r.name=r.keyword;else if(r.genreName)r.name='ジャンル: '+splitValues(r.genreName).join('/');else if(r.serviceName)r.name='放送局: '+splitValues(r.serviceName).join('/');else r.name='自動予約設定';}if(!r.id){r.id=nextId();rules.push(r);}else{var found=-1;for(var i=0;i<rules.length;i++){if(Number(rules[i].id)===Number(r.id)){found=i;break;}}if(found>=0)rules[found]=r;else rules.push(r);}render();buildIniPreview();saveIniToApp(true).then(function(ok){if(ok){resetForm();render();buildIniPreview();}else{restoreRules(before);}});}\n";
            html += L"function defaultRule(){return {id:0,enabled:true,name:'',keyword:'',excludeKeyword:'',titleMatchMode:0,matchTitle:true,matchSummary:true,matchExtended:false,targetSpace:0,serviceName:'',genreName:'',priority:50,preMarginSeconds:0,postMarginSeconds:0,createReservationEnabled:true,duplicatePolicy:0,useKeyword:true,keywordJoinMode:'OR',useExclude:false,excludeJoinMode:'OR',useService:false,useGenre:false,genreJoinMode:'OR',useMinDuration:false,minDurationMinutes:30,timeRangeMode:'all',searchStartMinutes:0,searchEndMinutes:1440,searchTimeWrap:false,weekdayMode:'all',weekdays:'1111111',skipDuplicate:true,excludeRerun:false,actionMode:'candidate'};}\n";
            html += L"function resetForm(){writeForm(defaultRule());}\n";
            html += L"function applyAutoReserveAssistInput(){";
            html += L"var params=null;";
            html += L"try{params=new URLSearchParams(window.location.search||'');}catch(e){return false;}";
            html += L"if(String(params.get('mode')||'').toLowerCase()!=='new')return false;";
            html += L"var keyword=String(params.get('keyword')||'').trim();";
            html += L"if(!keyword)return false;";
            html += L"if(keyword.length>512)keyword=keyword.substring(0,512);";
            html += L"var keywordInput=byId('keyword');";
            html += L"if(!keywordInput)return false;";
            html += L"var useKeyword=byId('useKeyword');";
            html += L"var matchTitle=byId('matchTitle');";
            html += L"if(useKeyword)useKeyword.checked=true;";
            html += L"if(matchTitle)matchTitle.checked=true;";
            html += L"keywordInput.value=keyword;";
            html += L"keywordInput.dispatchEvent(new Event('input',{bubbles:true}));";
            html += L"var notice=byId('autoReserveAssistNotice');";
            html += L"if(notice){notice.textContent='番組表から「'+keyword+'」を検索ワードへ入力しました。条件を確認してから保存してください。';notice.classList.add('visible');}";
            html += L"updateUiState();updateSelectedTexts();renderConditionPreview();";
            html += L"try{keywordInput.focus({preventScroll:true});}catch(e2){try{keywordInput.focus();}catch(e3){}}";
            html += L"return true;";
            html += L"}\n";
            html += L"function reloadInitialRules(){return loadRulesCacheFromApp().then(function(){resetForm();render();buildIniPreview();return rules;});}\n";
            html += L"function reloadInitialRulesFromText(text){rules=rulesFromIniText(String(def(text,''))).map(normalizeRule);return rules;}\n";
            html += L"function iniLine(k,v){return k+'='+String(def(v,''))+'\\n';}\n";
            html += L"function makeIniText(){var out='[AutoReserveRules]\\n';out+=iniLine('Count',rules.length);out+=iniLine('NextId',nextId());out+='\\n';rules.forEach(function(r0,i){var r=normalizeRule(r0);out+='[AutoReserveRule_'+i+']\\n';out+=iniLine('Id',r.id);out+=iniLine('Enabled',r.enabled?1:0);out+=iniLine('Name',r.name);out+=iniLine('UseKeyword',r.useKeyword?1:0);out+=iniLine('Keyword',r.keyword);out+=iniLine('KeywordJoinMode',r.keywordJoinMode);out+=iniLine('UseExcludeKeyword',r.useExclude?1:0);out+=iniLine('ExcludeKeyword',r.excludeKeyword);out+=iniLine('ExcludeKeywordJoinMode',r.excludeJoinMode);out+=iniLine('TitleMatchMode',r.titleMatchMode);out+=iniLine('MatchTitle',r.matchTitle?1:0);out+=iniLine('MatchSummary',r.matchSummary?1:0);out+=iniLine('MatchExtended',r.matchExtended?1:0);out+=iniLine('TargetSpace',r.targetSpace);out+=iniLine('UseService',r.useService?1:0);out+=iniLine('ServiceName',r.serviceName);out+=iniLine('ServiceMatchMode','OR');out+=iniLine('UseGenre',r.useGenre?1:0);out+=iniLine('GenreName',r.genreName);out+=iniLine('GenreMatchMode',r.genreJoinMode);out+=iniLine('UseMinDuration',r.useMinDuration?1:0);out+=iniLine('MinDurationMinutes',r.minDurationMinutes);out+=iniLine('UseSearchTimeRange',r.timeRangeMode==='custom'?1:0);out+=iniLine('SearchStartMinutes',r.searchStartMinutes);out+=iniLine('SearchEndMinutes',r.searchEndMinutes);out+=iniLine('SearchTimeWrap',r.searchTimeWrap?1:0);out+=iniLine('StartMinutes',r.timeRangeMode==='custom'?r.searchStartMinutes:-1);out+=iniLine('EndMinutes',r.timeRangeMode==='custom'?r.searchEndMinutes:-1);out+=iniLine('UseWeekday',r.weekdayMode==='custom'?1:0);out+=iniLine('Weekdays',r.weekdays);out+=iniLine('Priority',r.priority);out+=iniLine('PreMarginSeconds',r.preMarginSeconds);out+=iniLine('PostMarginSeconds',r.postMarginSeconds);out+=iniLine('CreateReservationEnabled',r.createReservationEnabled?1:0);out+=iniLine('DuplicatePolicy',r.duplicatePolicy);out+=iniLine('SkipDuplicate',r.skipDuplicate?1:0);out+=iniLine('ExcludeRerunLikeProgram',r.excludeRerun?1:0);out+=iniLine('ActionMode',r.actionMode);out+='\\n';});return out;}\n";
            html += L"function buildIniPreview(){var out=makeIniText();var box=byId('iniPreview');if(box)box.value=out;renderConditionPreview();return out;}\n";
            html += L"function downloadIniFile(){var text=buildIniPreview();var bom=new Uint8Array([0xEF,0xBB,0xBF]);var blob=new Blob([bom,text],{type:'text/plain;charset=utf-8'});var url=URL.createObjectURL(blob);var a=document.createElement('a');a.href=url;a.download='auto_reserve_rules.ini';document.body.appendChild(a);a.click();document.body.removeChild(a);setTimeout(function(){URL.revokeObjectURL(url);},1000);}\n";
            html += L"function encodeForm(payload){";
            html += L"var pairs=[];";
            html += L"Object.keys(payload||{}).forEach(function(key){";
            html += L"var value=payload[key];";
            html += L"if(value===undefined||value===null)return;";
            html += L"pairs.push(encodeURIComponent(key)+'='+encodeURIComponent(String(value)));";
            html += L"});";
            html += L"return pairs.join('&');";
            html += L"}\n";

            html += L"function autoReserveSaveApiUrl(){";
            html += L"var base='';";
            html += L"try{";
            html += L"if(window.EPG_RUNTIME&&window.EPG_RUNTIME.reservationApiBase){";
            html += L"base=String(window.EPG_RUNTIME.reservationApiBase);";
            html += L"}";
            html += L"}catch(e){}";
            html += L"try{";
            html += L"if(!base&&window.top&&window.top!==window&&window.top.EPG_RUNTIME&&window.top.EPG_RUNTIME.reservationApiBase){";
            html += L"base=String(window.top.EPG_RUNTIME.reservationApiBase);";
            html += L"}";
            html += L"}catch(e){}";
            html += L"if(base){return base+'/auto_reserve_rules_save';}";
            html += L"return '/api/reservation/auto_reserve_rules_save';";
            html += L"}\n";

            html += L"function saveIniToApp(showSuccess,reloadAfterSave){";
            html += L"var text=buildIniPreview();";
            html += L"if(!window.fetch){";
            html += L"alert('このブラウザでは本体保存APIを呼び出せません。専用INIを書き出してください。');";
            html += L"return Promise.resolve(false);";
            html += L"}";
            html += L"return fetch(autoReserveSaveApiUrl(),{";
            html += L"method:'POST',";
            html += L"headers:{'Content-Type':'application/x-www-form-urlencoded; charset=UTF-8'},";
            html += L"body:encodeForm({iniText:text}),";
            html += L"cache:'no-store'";
            html += L"}).then(function(res){";
            html += L"return res.text().then(function(body){return {ok:res.ok,body:body};});";
            html += L"}).then(function(r){";
            html += L"var obj=null;";
            html += L"try{obj=JSON.parse(r.body||'{}');}catch(e){}";
            html += L"if(!r.ok||(obj&&obj.ok===false)){";
            html += L"throw new Error((obj&&obj.message)?String(obj.message):(r.body||'save failed'));";
            html += L"}";
            html += L"if(reloadAfterSave===false){return true;}";
            html += L"return loadRulesCacheFromApp().then(function(){";
            html += L"if(showSuccess!==false){alert('auto_reserve_rules.ini へ保存し、最新キャッシュを再読込しました。');}";
            html += L"return true;";
            html += L"});";
            html += L"}).catch(function(e){";
            html += L"alert('auto_reserve_rules.ini へ直接保存できませんでした。保存APIが未接続、または保存処理が失敗しています。\\n専用INIを書き出すか、ローカルAPIを接続してください。\\n'+String(e&&e.message?e.message:e));";
            html += L"return false;";
            html += L"});";
            html += L"}\n";
            html += L"function parseIniText(text){text=String(def(text,'')).replace(/^\\uFEFF/,'');var current='';var sec={};text.split(/\\r?\\n/).forEach(function(line){line=String(line).trim();if(!line||line.charAt(0)===';'||line.charAt(0)==='#')return;if(line.charAt(0)==='['&&line.charAt(line.length-1)===']'){current=line.substring(1,line.length-1).trim();if(!sec[current])sec[current]={};return;}var eq=line.indexOf('=');if(eq<0||!current)return;var k=line.substring(0,eq).trim().toLowerCase();var v=line.substring(eq+1).trim();sec[current][k]=v;});return sec;}\n";
            html += L"function importIniBool(obj,key,d){var v=obj&&obj[key.toLowerCase()];if(v===undefined)return d;v=String(v).trim().toLowerCase();if(v==='1'||v==='true'||v==='yes'||v==='on'||v==='enabled'||v==='有効')return true;if(v==='0'||v==='false'||v==='no'||v==='off'||v==='disabled'||v==='無効')return false;return d;}\n";
            html += L"function importIniInt(obj,key,d){var v=obj&&obj[key.toLowerCase()];var n=Number(v);return Number.isFinite(n)?n:d;}\n";
            html += L"function importIniStr(obj,key,d){var v=obj&&obj[key.toLowerCase()];return v===undefined?d:String(v);}\n";
            html += L"function importJoin(v){v=String(def(v,'OR')).toUpperCase();return v==='AND'?'AND':'OR';}\n";
            html += L"function rulesFromIniText(text){var sec=parseIniText(text);var root=sec.AutoReserveRules||sec.autoreserverules||{};var count=importIniInt(root,'Count',-1);var names=Object.keys(sec).filter(function(name){return /^AutoReserveRule_\\d+$/i.test(name);}).sort(function(a,b){return Number(a.replace(/\\D/g,''))-Number(b.replace(/\\D/g,''));});if(count>=0){names=names.filter(function(name){var n=Number(name.replace(/\\D/g,''));return n<count;});}var out=[];names.forEach(function(name){var o=sec[name]||{};var useTime=importIniBool(o,'UseSearchTimeRange',importIniInt(o,'StartMinutes',-1)>=0&&importIniInt(o,'EndMinutes',-1)>=0);var useWeekday=importIniBool(o,'UseWeekday',false);out.push(normalizeRule({id:importIniInt(o,'Id',0),enabled:importIniBool(o,'Enabled',true),name:importIniStr(o,'Name',''),useKeyword:importIniBool(o,'UseKeyword',!!importIniStr(o,'Keyword','')),keyword:importIniStr(o,'Keyword',''),keywordJoinMode:importJoin(importIniStr(o,'KeywordJoinMode','OR')),useExclude:importIniBool(o,'UseExcludeKeyword',!!importIniStr(o,'ExcludeKeyword','')),excludeKeyword:importIniStr(o,'ExcludeKeyword',''),excludeJoinMode:importJoin(importIniStr(o,'ExcludeKeywordJoinMode','OR')),titleMatchMode:importIniInt(o,'TitleMatchMode',0),matchTitle:importIniBool(o,'MatchTitle',true),matchSummary:importIniBool(o,'MatchSummary',false),matchExtended:importIniBool(o,'MatchExtended',false),targetSpace:importIniInt(o,'TargetSpace',0),useService:importIniBool(o,'UseService',!!importIniStr(o,'ServiceName','')),serviceName:importIniStr(o,'ServiceName',''),useGenre:importIniBool(o,'UseGenre',!!importIniStr(o,'GenreName','')),genreName:importIniStr(o,'GenreName',''),genreJoinMode:importJoin(importIniStr(o,'GenreMatchMode','OR')),useMinDuration:importIniBool(o,'UseMinDuration',false),minDurationMinutes:importIniInt(o,'MinDurationMinutes',0),timeRangeMode:useTime?'custom':'all',searchStartMinutes:importIniInt(o,'SearchStartMinutes',importIniInt(o,'StartMinutes',0)),searchEndMinutes:importIniInt(o,'SearchEndMinutes',importIniInt(o,'EndMinutes',1440)),searchTimeWrap:importIniBool(o,'SearchTimeWrap',false),weekdayMode:useWeekday?'custom':'all',weekdays:importIniStr(o,'Weekdays','1111111'),priority:importIniInt(o,'Priority',50),preMarginSeconds:importIniInt(o,'PreMarginSeconds',0),postMarginSeconds:importIniInt(o,'PostMarginSeconds',0),createReservationEnabled:importIniBool(o,'CreateReservationEnabled',true),duplicatePolicy:importIniInt(o,'DuplicatePolicy',0),skipDuplicate:importIniBool(o,'SkipDuplicate',true),excludeRerun:importIniBool(o,'ExcludeRerunLikeProgram',false),actionMode:importIniStr(o,'ActionMode','candidate')}));});return out;}\n";
            html += L"function reservationApiBase(){var base='';try{if(window.EPG_RUNTIME&&window.EPG_RUNTIME.reservationApiBase)base=String(window.EPG_RUNTIME.reservationApiBase);}catch(e){}try{if(!base&&window.top&&window.top!==window&&window.top.EPG_RUNTIME&&window.top.EPG_RUNTIME.reservationApiBase)base=String(window.top.EPG_RUNTIME.reservationApiBase);}catch(e){}return base;}\n";
            html += L"function autoReserveCacheApiUrl(){return '/cache/auto_reserve/auto_reserve_rules.cache';}\n";
            html += L"function loadRulesCacheFromApp(){if(!window.fetch)return Promise.reject(new Error('このブラウザーはキャッシュ読込に対応していません。'));var url=autoReserveCacheApiUrl()+'?time='+encodeURIComponent(String(Date.now()));return fetch(url,{method:'GET',cache:'no-store',credentials:'same-origin'}).then(function(res){return res.text().then(function(body){return {ok:res.ok,status:res.status,body:body};});}).then(function(r){if(!r.ok)throw new Error('cache HTTP '+String(r.status)+(r.body?': '+r.body:''));rules=rulesFromIniText(r.body).map(normalizeRule);return rules;});}\n";
            html += L"function loadIniFile(file){if(!file)return;var reader=new FileReader();reader.onload=function(){var list=rulesFromIniText(String(reader.result||''));if(!list.length){alert('自動予約設定を読み込めませんでした。');return;}if(!confirm('専用INIから '+list.length+' 件読み込み、画面上の登録内容を置き換えますか？'))return;rules=list;resetForm();render();buildIniPreview();alert('専用INIを読み込みました。画面上へ反映しました。保存する場合は専用INIを書き出してください。');};reader.onerror=function(){alert('専用INIの読み込みに失敗しました。');};reader.readAsText(file,'utf-8');}\n";
            html += L"window.addOrUpdateRule=addOrUpdateRule;window.resetForm=resetForm;window.reloadInitialRules=reloadInitialRules;window.buildIniPreview=buildIniPreview;window.downloadIniFile=downloadIniFile;window.saveIniToApp=saveIniToApp;window.applyAutoReserveAssistInput=applyAutoReserveAssistInput;\n";
            html += L"document.querySelectorAll('.editorCard input,.editorCard select').forEach(function(el){el.addEventListener('input',function(){syncEnabledSwitchText();updateUiState();updateSelectedTexts();renderConditionPreview();});el.addEventListener('change',function(){syncEnabledSwitchText();updateUiState();updateSelectedTexts();renderConditionPreview();});});\n";
            html += L"var conditionDetails=byId('conditionPreviewDetails');if(conditionDetails){conditionDetails.addEventListener('toggle',syncHostFrameHeight);}\n";
            html += L"var iniInput=byId('iniFileInput');if(iniInput){iniInput.addEventListener('change',function(){var f=iniInput.files&&iniInput.files.length?iniInput.files[0]:null;loadIniFile(f);iniInput.value='';});}\n";
            html += L"installHostFrameAutoResize();\n";
            html += L"resetForm();applyAutoReserveAssistInput();";
            html += L"loadRulesCacheFromApp().then(function(){";
            html += L"render();buildIniPreview();syncHostFrameHeight();";
            html += L"}).catch(function(e){";
            html += L"var sum=byId('summary');";
            html += L"if(sum){sum.textContent='最新の自動予約キャッシュを読み込めませんでした: '+String(e&&e.message?e.message:e);}";
            html += L"syncHostFrameHeight();";
            html += L"try{console.error('[auto-reserve] init failed',e);}catch(_e){}";
            html += L"});\n";
            html += L"})();\n";
            html += L"</script>\n";
        }

        static std::wstring BuildAutoReservePageHtml_(
            const EpgHtmlPortal::WriteOptions& opt,
            const std::filesystem::path& rulesIniPath,
            const std::vector<AutoReserve::Rule>& rules)
        {
            const std::vector<ConfigServiceOption_> services = LoadEnabledConfigServices_(opt);

            std::wstring html;
            html.reserve(96 * 1024);

            html += L"<!DOCTYPE html>\n";
            html += L"<html lang=\"ja\">\n";
            html += L"<head>\n";
            html += L"<meta charset=\"UTF-8\">\n";
            html += L"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
            html += L"<title>自動予約設定</title>\n";
            AppendPageStyle_(html);
            html += L"</head>\n";
            html += L"<body>\n";
            html += L"<div class=\"shell\">\n";
            html += L"<section class=\"header\">";
            html += L"<div class=\"headerMain\">";
            html += L"<h1>自動予約設定</h1>";
            html += L"<p>番組表から条件に合う番組を探すための設定を作成します。ワード検索、ジャンル検索、放送局指定、時間帯指定を組み合わせられます。</p>";
            html += L"</div>";
            html += L"<div class=\"headerMeta\">";
            html += L"INI: ";
            html += EpgHtmlPortal_Common::EscapeHtmlW(rulesIniPath.wstring());
            html += L"<br>初期読込 ";
            html += std::to_wstring(rules.size());
            html += L" 件 / 有効放送局候補 ";
            html += std::to_wstring(services.size());
            html += L" 件<br>保存形式は auto_reserve_rules.ini に一本化しています。旧ブラウザ保存はこのページを開いた時点で削除します。";
            html += L"</div>";
            html += L"</section>";

            AppendFormHtml_(html, services);
            AppendListHtml_(html);
            AppendPageScript_(html);

            html += L"</div>\n";
            html += L"</body>\n";
            html += L"</html>\n";

            return html;
        }



        static void AppendRuleDetailPageStyle_(
            std::wstring& html)
        {
            html += L"<style>\n";
            html += L".detailFormGrid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;}\n";
            html += L".detailFormGrid .wide{grid-column:1/-1;}\n";
            html += L".detailActions{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px;}\n";
            html += L".detailMini{font-size:12px;color:#97a4b7;line-height:1.45;}\n";
            html += L".detailPreview{padding:0;overflow:hidden;}\n";
            html += L".detailPreview>summary{cursor:pointer;list-style:none;padding:12px 14px;font-size:15px;font-weight:700;color:#e8ecf1;user-select:none;}\n";
            html += L".detailPreview>summary::-webkit-details-marker{display:none;}\n";
            html += L".detailPreview>summary::before{content:'▶';display:inline-block;margin-right:8px;color:#8fb7ff;transition:transform .15s ease;}\n";
            html += L".detailPreview[open]>summary::before{transform:rotate(90deg);}\n";
            html += L".detailPreview[open]>summary{border-bottom:1px solid #2a313d;}\n";
            html += L".detailPreviewBody{padding:12px 14px 14px;}\n";
            html += L".detailPreview textarea{height:140px;min-height:100px;max-height:260px;resize:vertical;}\n";
            html += L"@media(max-width:980px){.detailFormGrid{grid-template-columns:1fr;}}\n";
            html += L"</style>\n";
        }

        static void AppendRuleDetailEditorHtml_(
            std::wstring& html,
            const std::vector<ConfigServiceOption_>& services)
        {
            html += L"<section class=\"card editorCard\">";
            html += L"<h2 id=\"detailTitle\">自動予約ルール詳細</h2>";
            html += L"<p id=\"detailStatus\" class=\"note\">読み込み中です。</p>";
            html += L"<input id=\"editId\" type=\"hidden\" value=\"0\">";

            html += L"<div class=\"detailFormGrid\">";

            html += L"<label class=\"statusSwitch statusSwitchField\"><input id=\"enabled\" type=\"checkbox\" role=\"switch\" aria-label=\"この自動予約設定の有効状態\"><span class=\"statusSwitchTrack\" aria-hidden=\"true\"></span><span id=\"enabledStatusText\" class=\"statusSwitchText\">無効</span></label>";
            html += L"<label>登録動作<select id=\"actionMode\"><option value=\"candidate\">候補として表示</option><option value=\"auto\">自動で予約登録</option></select></label>";
            html += L"<label class=\"wide\">この自動予約設定の名前<input id=\"ruleName\" type=\"text\"></label>";

            html += L"<label class=\"inlineCheck wide\"><input id=\"useKeyword\" type=\"checkbox\">番組名や説明に含まれる言葉で探す</label>";
            html += L"<label class=\"wide\">含まれる言葉<input id=\"keyword\" type=\"text\"><span class=\"detailMini\">| 区切りで複数指定できます。</span></label>";
            html += L"<label>含まれる言葉のつなぎ方<select id=\"keywordJoinMode\"><option value=\"OR\">いずれかに一致</option><option value=\"AND\">すべてに一致</option></select></label>";
            html += L"<label>言葉の一致方法<select id=\"titleMatchMode\"><option value=\"0\">部分一致</option><option value=\"1\">完全一致</option><option value=\"2\">前方一致</option><option value=\"3\">後方一致</option></select></label>";

            html += L"<div class=\"checkLine wide\">";
            html += L"<label><input id=\"matchTitle\" type=\"checkbox\">番組タイトル</label>";
            html += L"<label><input id=\"matchSummary\" type=\"checkbox\">番組概要</label>";
            html += L"<label><input id=\"matchExtended\" type=\"checkbox\">詳細説明</label>";
            html += L"</div>";

            html += L"<label class=\"inlineCheck wide\"><input id=\"useExclude\" type=\"checkbox\">除外する言葉を使う</label>";
            html += L"<label class=\"wide\">除外する言葉<input id=\"excludeKeyword\" type=\"text\"></label>";
            html += L"<label>除外の判定<select id=\"excludeJoinMode\"><option value=\"OR\">いずれかを含んだら除外</option><option value=\"AND\">すべて含んだら除外</option></select></label>";

            html += L"<label>対象の放送波<select id=\"targetSpace\"><option value=\"0\">地上波</option><option value=\"1\">BS</option><option value=\"2\">CS</option><option value=\"-1\">すべて</option></select></label>";

            html += L"<label class=\"inlineCheck\"><input id=\"useService\" type=\"checkbox\">放送局を指定する</label>";
            html += L"<label class=\"wide\">放送局<select id=\"serviceSelect\" multiple>";
            AppendConfigServiceOptionsHtml_(html, services);
            html += L"</select></label>";

            html += L"<label class=\"inlineCheck\"><input id=\"useGenre\" type=\"checkbox\">ジャンルを指定する</label>";
            html += L"<label>ジャンルの判定<select id=\"genreJoinMode\"><option value=\"OR\">いずれかに一致</option><option value=\"AND\">すべてに一致</option></select></label>";
            html += L"<label class=\"wide\">ジャンル<select id=\"genreSelect\" multiple>";
            AppendGenreOptionsHtml_(html);
            html += L"</select></label>";

            html += L"<label class=\"inlineCheck\"><input id=\"useMinDuration\" type=\"checkbox\">短い番組を除外する</label>";
            html += L"<label>最低放送時間<input id=\"minDurationMinutes\" type=\"number\" min=\"0\" max=\"1440\"></label>";

            html += L"<label>検索する時間帯<select id=\"timeRangeMode\"><option value=\"all\">すべての時間帯</option><option value=\"custom\">時間帯を指定</option></select></label>";
            html += L"<label class=\"inlineCheck\"><input id=\"searchTimeWrap\" type=\"checkbox\">日付をまたぐ</label>";
            html += L"<label>開始<select id=\"searchStartMinutes\">";
            AppendTimeOptionsHtml_(html, 0, 1410, 30);
            html += L"</select></label>";
            html += L"<label>終了<select id=\"searchEndMinutes\">";
            AppendTimeOptionsHtml_(html, 30, 1440, 30);
            html += L"</select></label>";

            html += L"<label>曜日<select id=\"weekdayMode\"><option value=\"all\">すべての曜日</option><option value=\"custom\">曜日を指定</option></select></label>";
            html += L"<div class=\"checkLine wide\">";
            html += L"<label><input id=\"wdMon\" class=\"weekdayCheck\" type=\"checkbox\">月</label>";
            html += L"<label><input id=\"wdTue\" class=\"weekdayCheck\" type=\"checkbox\">火</label>";
            html += L"<label><input id=\"wdWed\" class=\"weekdayCheck\" type=\"checkbox\">水</label>";
            html += L"<label><input id=\"wdThu\" class=\"weekdayCheck\" type=\"checkbox\">木</label>";
            html += L"<label><input id=\"wdFri\" class=\"weekdayCheck\" type=\"checkbox\">金</label>";
            html += L"<label><input id=\"wdSat\" class=\"weekdayCheck\" type=\"checkbox\">土</label>";
            html += L"<label><input id=\"wdSun\" class=\"weekdayCheck\" type=\"checkbox\">日</label>";
            html += L"</div>";

            html += L"<label>優先度<input id=\"priority\" type=\"number\" min=\"0\" max=\"100\"></label>";
            html += L"<label>重複処理<select id=\"duplicatePolicy\"><option value=\"0\">同一イベントを除外</option><option value=\"1\">同一タイトル/時刻を除外</option><option value=\"2\">重複許可</option></select></label>";
            html += L"<label>前マージン秒<input id=\"preMarginSeconds\" type=\"number\" min=\"0\" max=\"3600\"></label>";
            html += L"<label>後マージン秒<input id=\"postMarginSeconds\" type=\"number\" min=\"0\" max=\"3600\"></label>";

            html += L"<div class=\"checkLine wide\">";
            html += L"<label><input id=\"skipDuplicate\" type=\"checkbox\">既存予約と重複した番組は登録しない</label>";
            html += L"<label><input id=\"excludeRerun\" type=\"checkbox\">再放送らしい番組を除外</label>";
            html += L"<label><input id=\"createReservationEnabled\" type=\"checkbox\">作成する予約を有効状態にする</label>";
            html += L"</div>";

            html += L"</div>";

            html += L"<div class=\"detailActions\">";
            html += L"<button type=\"button\" class=\"primary\" onclick=\"saveCurrentRule()\">変更を保存</button>";
            html += L"<button type=\"button\" class=\"danger\" onclick=\"deleteCurrentRule()\">このルールを削除</button>";
            html += L"<button type=\"button\" onclick=\"location.href='epg_auto_reserve.html'\">作成ページへ戻る</button>";
            html += L"</div>";

            html += L"</section>";

            html += L"<details class=\"card detailPreview\">";
            html += L"<summary>INIプレビュー（確認用）</summary>";
            html += L"<div class=\"detailPreviewBody\">";
            html += L"<p class=\"note\">必要な場合だけ開いて、保存されるINI内容を確認できます。</p>";
            html += L"<textarea id=\"iniPreview\" readonly></textarea>";
            html += L"</div>";
            html += L"</details>";
        }

        static void AppendRuleDetailPageScript_(
            std::wstring& html)
        {
            html += L"<script>\n";
            html += L"(function(){\n";
            html += L"'use strict';\n";
            html += L"function scrollPageTop(){try{window.scrollTo(0,0);document.documentElement.scrollTop=0;document.body.scrollTop=0;}catch(e){}try{if(window.parent&&window.parent!==window){window.parent.scrollTo(0,0);window.parent.document.documentElement.scrollTop=0;window.parent.document.body.scrollTop=0;}}catch(e2){}}\n";
            html += L"var hostResizeQueued=false;\n";
            html += L"var lastHostFrameHeight=0;\n";
            html += L"function getPageContentHeight(){var shell=document.querySelector('.shell');if(shell){var rect=shell.getBoundingClientRect();return Math.max(1,Math.ceil(rect.bottom+window.scrollY+4));}var body=document.body;return Math.max(1,Math.ceil(body?body.scrollHeight:1));}\n";
            html += L"function syncHostFrameHeight(){if(hostResizeQueued)return;hostResizeQueued=true;window.requestAnimationFrame(function(){hostResizeQueued=false;var h=getPageContentHeight();if(Math.abs(h-lastHostFrameHeight)<2)return;lastHostFrameHeight=h;try{if(window.parent&&window.parent!==window){var frames=window.parent.document.getElementsByTagName('iframe');for(var i=0;i<frames.length;i++){if(frames[i].contentWindow===window){var frame=frames[i];var current=parseInt(frame.style.height||frame.getAttribute('height')||'0',10)||0;if(Math.abs(current-h)>=2){frame.style.height=String(h)+'px';frame.style.minHeight='0';frame.setAttribute('height',String(h));}break;}}}}catch(e){}try{if(window.parent&&window.parent!==window){window.parent.postMessage({type:'epg-portal-content-height',page:'auto-reserve-detail',height:h},location.origin);}}catch(e2){}});}\n";
            html += L"function installHostFrameAutoResize(){syncHostFrameHeight();var shell=document.querySelector('.shell');if(window.ResizeObserver&&shell){var ro=new ResizeObserver(function(){syncHostFrameHeight();});ro.observe(shell);window.__epgAutoDetailResizeObserver=ro;}else if(window.MutationObserver&&shell){var mo=new MutationObserver(function(){syncHostFrameHeight();});mo.observe(shell,{childList:true,subtree:true,attributes:true,characterData:true});window.__epgAutoDetailMutationObserver=mo;}window.addEventListener('resize',syncHostFrameHeight);}\n";
            html += L"scrollPageTop();\n";

            html += L"function byId(id){return document.getElementById(id);}\n";
            html += L"function def(v,d){return (v===undefined||v===null)?d:v;}\n";
            html += L"function n(id,d){var el=byId(id);var v=el?Number(el.value):Number.NaN;return Number.isFinite(v)?v:d;}\n";
            html += L"function b(id){var el=byId(id);return !!(el&&el.checked);}\n";
            html += L"function s(id){var el=byId(id);return String(el?el.value:'').trim();}\n";
            html += L"function setVal(id,v){var el=byId(id);if(el)el.value=String(def(v,''));}\n";
            html += L"function setChecked(id,v){var el=byId(id);if(el)el.checked=!!v;}\n";
            html += L"function syncEnabledSwitchText(){var sw=byId('enabled');var text=byId('enabledStatusText');if(sw)sw.setAttribute('aria-checked',sw.checked?'true':'false');if(text)text.textContent=sw&&sw.checked?'有効':'無効';}\n";

            html += L"function splitValues(text){return String(def(text,'')).split('|').map(function(x){return x.trim();}).filter(function(x){return x.length>0;});}\n";
            html += L"function joinValues(list){return list.map(function(x){return String(x).trim();}).filter(function(x){return x.length>0;}).join('|');}\n";
            html += L"function getMulti(id){var el=byId(id);if(!el)return '';var a=[];for(var i=0;i<el.options.length;i++){if(el.options[i].selected&&el.options[i].value){a.push(el.options[i].value);}}return joinValues(a);}\n";
            html += L"function setMulti(id,pipeText){var el=byId(id);if(!el)return;var values=splitValues(pipeText);for(var i=0;i<el.options.length;i++){el.options[i].selected=values.indexOf(el.options[i].value)>=0;}}\n";

            html += L"function readWeekdays(){var ids=['wdMon','wdTue','wdWed','wdThu','wdFri','wdSat','wdSun'];return ids.map(function(id){return b(id)?'1':'0';}).join('');}\n";
            html += L"function setWeekdays(bits){bits=String(def(bits,'1111111'));var ids=['wdMon','wdTue','wdWed','wdThu','wdFri','wdSat','wdSun'];for(var i=0;i<ids.length;i++){setChecked(ids[i],bits.charAt(i)!=='0');}}\n";

            html += L"function normalizeRule(r){r=r||{};var nr={};nr.id=Number(def(r.id,0))||0;nr.enabled=!!def(r.enabled,true);nr.name=String(def(r.name,''));nr.keyword=String(def(r.keyword,''));nr.excludeKeyword=String(def(r.excludeKeyword,''));nr.titleMatchMode=Number(def(r.titleMatchMode,0))||0;nr.matchTitle=!!def(r.matchTitle,true);nr.matchSummary=!!def(r.matchSummary,false);nr.matchExtended=!!def(r.matchExtended,false);nr.targetSpace=Number(def(r.targetSpace,0));nr.serviceName=String(def(r.serviceName,''));nr.genreName=String(def(r.genreName,''));nr.priority=Number(def(r.priority,50))||50;nr.preMarginSeconds=Number(def(r.preMarginSeconds,0))||0;nr.postMarginSeconds=Number(def(r.postMarginSeconds,0))||0;nr.createReservationEnabled=!!def(r.createReservationEnabled,true);nr.duplicatePolicy=Number(def(r.duplicatePolicy,0))||0;nr.useKeyword=!!def(r.useKeyword,nr.keyword.length>0);nr.keywordJoinMode=String(def(r.keywordJoinMode,'OR'));nr.useExclude=!!def(r.useExclude,nr.excludeKeyword.length>0);nr.excludeJoinMode=String(def(r.excludeJoinMode,'OR'));nr.useService=!!def(r.useService,nr.serviceName.length>0);nr.useGenre=!!def(r.useGenre,nr.genreName.length>0);nr.genreJoinMode=String(def(r.genreJoinMode,'OR'));nr.useMinDuration=!!def(r.useMinDuration,false);nr.minDurationMinutes=Number(def(r.minDurationMinutes,0))||0;var hasOldTime=Number(def(r.startMinutes,-1))>=0&&Number(def(r.endMinutes,-1))>=0;nr.timeRangeMode=String(def(r.timeRangeMode,hasOldTime?'custom':'all'));nr.searchStartMinutes=Number(def(r.searchStartMinutes,hasOldTime?r.startMinutes:0));nr.searchEndMinutes=Number(def(r.searchEndMinutes,hasOldTime?r.endMinutes:1440));nr.searchTimeWrap=!!def(r.searchTimeWrap,false);nr.startMinutes=nr.timeRangeMode==='custom'?nr.searchStartMinutes:-1;nr.endMinutes=nr.timeRangeMode==='custom'?nr.searchEndMinutes:-1;nr.weekdayMode=String(def(r.weekdayMode,'all'));nr.weekdays=String(def(r.weekdays,'1111111'));nr.skipDuplicate=!!def(r.skipDuplicate,true);nr.excludeRerun=!!def(r.excludeRerun,false);nr.actionMode=String(def(r.actionMode,'candidate'));return nr;}\n";

            html += L"var rules=[];\n";
            html += L"function getQueryId(){try{var p=new URLSearchParams(location.search);var v=Number(p.get('id')||0);return Number.isFinite(v)?v:0;}catch(e){return 0;}}\n";
            html += L"var currentId=getQueryId();\n";
            html += L"function findIndexById(id){id=Number(id)||0;for(var i=0;i<rules.length;i++){if(Number(rules[i].id)===id)return i;}return -1;}\n";

            html += L"function setGroupDisabled(selector,disabled){document.querySelectorAll(selector).forEach(function(el){el.disabled=disabled;});}\n";
            html += L"function updateUiState(){setGroupDisabled('#keyword,#titleMatchMode,#keywordJoinMode,#matchTitle,#matchSummary,#matchExtended',!b('useKeyword'));setGroupDisabled('#excludeKeyword,#excludeJoinMode',!b('useExclude'));setGroupDisabled('#serviceSelect',!b('useService'));setGroupDisabled('#genreSelect,#genreJoinMode',!b('useGenre'));setGroupDisabled('#minDurationMinutes',!b('useMinDuration'));var timeCustom=s('timeRangeMode')==='custom';setGroupDisabled('#searchStartMinutes,#searchEndMinutes,#searchTimeWrap',!timeCustom);setGroupDisabled('.weekdayCheck',s('weekdayMode')!=='custom');}\n";

            html += L"function writeForm(r0){var r=normalizeRule(r0);setVal('editId',r.id);setChecked('enabled',r.enabled);setVal('actionMode',r.actionMode);setVal('ruleName',r.name);setChecked('useKeyword',r.useKeyword);setVal('keyword',r.keyword);setVal('keywordJoinMode',r.keywordJoinMode);setVal('titleMatchMode',r.titleMatchMode);setChecked('matchTitle',r.matchTitle);setChecked('matchSummary',r.matchSummary);setChecked('matchExtended',r.matchExtended);setChecked('useExclude',r.useExclude);setVal('excludeKeyword',r.excludeKeyword);setVal('excludeJoinMode',r.excludeJoinMode);setVal('targetSpace',r.targetSpace);setChecked('useService',r.useService);setMulti('serviceSelect',r.serviceName);setChecked('useGenre',r.useGenre);setVal('genreJoinMode',r.genreJoinMode);setMulti('genreSelect',r.genreName);setChecked('useMinDuration',r.useMinDuration);setVal('minDurationMinutes',r.minDurationMinutes);setVal('timeRangeMode',r.timeRangeMode);setChecked('searchTimeWrap',r.searchTimeWrap);setVal('searchStartMinutes',r.searchStartMinutes);setVal('searchEndMinutes',r.searchEndMinutes);setVal('weekdayMode',r.weekdayMode);setWeekdays(r.weekdays);setVal('priority',r.priority);setVal('duplicatePolicy',r.duplicatePolicy);setVal('preMarginSeconds',r.preMarginSeconds);setVal('postMarginSeconds',r.postMarginSeconds);setChecked('skipDuplicate',r.skipDuplicate);setChecked('excludeRerun',r.excludeRerun);setChecked('createReservationEnabled',r.createReservationEnabled);syncEnabledSwitchText();var title=byId('detailTitle');if(title)title.textContent='#'+r.id+' '+(r.name||r.keyword||'(無題)');var st=byId('detailStatus');if(st)st.textContent='一覧から呼び出した自動予約ルールを表示しています。変更後は保存してください。';updateUiState();buildIniPreview();syncHostFrameHeight();}\n";

            html += L"function readForm(){var timeMode=s('timeRangeMode')||'all';var start=timeMode==='custom'?n('searchStartMinutes',0):-1;var end=timeMode==='custom'?n('searchEndMinutes',1440):-1;return normalizeRule({id:n('editId',0),enabled:b('enabled'),actionMode:s('actionMode')||'candidate',name:s('ruleName'),useKeyword:b('useKeyword'),keyword:s('keyword'),keywordJoinMode:s('keywordJoinMode')||'OR',titleMatchMode:n('titleMatchMode',0),matchTitle:b('matchTitle'),matchSummary:b('matchSummary'),matchExtended:b('matchExtended'),useExclude:b('useExclude'),excludeKeyword:s('excludeKeyword'),excludeJoinMode:s('excludeJoinMode')||'OR',targetSpace:n('targetSpace',0),useService:b('useService'),serviceName:b('useService')?getMulti('serviceSelect'):'',useGenre:b('useGenre'),genreName:b('useGenre')?getMulti('genreSelect'):'',genreJoinMode:s('genreJoinMode')||'OR',useMinDuration:b('useMinDuration'),minDurationMinutes:n('minDurationMinutes',0),timeRangeMode:timeMode,searchStartMinutes:n('searchStartMinutes',0),searchEndMinutes:n('searchEndMinutes',1440),searchTimeWrap:b('searchTimeWrap'),startMinutes:start,endMinutes:end,weekdayMode:s('weekdayMode')||'all',weekdays:readWeekdays(),priority:n('priority',50),duplicatePolicy:n('duplicatePolicy',0),preMarginSeconds:n('preMarginSeconds',0),postMarginSeconds:n('postMarginSeconds',0),skipDuplicate:b('skipDuplicate'),excludeRerun:b('excludeRerun'),createReservationEnabled:b('createReservationEnabled')});}\n";

            html += L"function iniLine(k,v){return k+'='+String(def(v,''))+'\\n';}\n";
            html += L"function nextId(){var m=0;for(var i=0;i<rules.length;i++){m=Math.max(m,Number(rules[i].id)||0);}return m+1;}\n";
            html += L"function makeIniText(){var out='[AutoReserveRules]\\n';out+=iniLine('Count',rules.length);out+=iniLine('NextId',nextId());out+='\\n';rules.forEach(function(r0,i){var r=normalizeRule(r0);out+='[AutoReserveRule_'+i+']\\n';out+=iniLine('Id',r.id);out+=iniLine('Enabled',r.enabled?1:0);out+=iniLine('Name',r.name);out+=iniLine('UseKeyword',r.useKeyword?1:0);out+=iniLine('Keyword',r.keyword);out+=iniLine('KeywordJoinMode',r.keywordJoinMode);out+=iniLine('UseExcludeKeyword',r.useExclude?1:0);out+=iniLine('ExcludeKeyword',r.excludeKeyword);out+=iniLine('ExcludeKeywordJoinMode',r.excludeJoinMode);out+=iniLine('TitleMatchMode',r.titleMatchMode);out+=iniLine('MatchTitle',r.matchTitle?1:0);out+=iniLine('MatchSummary',r.matchSummary?1:0);out+=iniLine('MatchExtended',r.matchExtended?1:0);out+=iniLine('TargetSpace',r.targetSpace);out+=iniLine('UseService',r.useService?1:0);out+=iniLine('ServiceName',r.serviceName);out+=iniLine('ServiceMatchMode','OR');out+=iniLine('UseGenre',r.useGenre?1:0);out+=iniLine('GenreName',r.genreName);out+=iniLine('GenreMatchMode',r.genreJoinMode);out+=iniLine('UseMinDuration',r.useMinDuration?1:0);out+=iniLine('MinDurationMinutes',r.minDurationMinutes);out+=iniLine('UseSearchTimeRange',r.timeRangeMode==='custom'?1:0);out+=iniLine('SearchStartMinutes',r.searchStartMinutes);out+=iniLine('SearchEndMinutes',r.searchEndMinutes);out+=iniLine('SearchTimeWrap',r.searchTimeWrap?1:0);out+=iniLine('StartMinutes',r.timeRangeMode==='custom'?r.searchStartMinutes:-1);out+=iniLine('EndMinutes',r.timeRangeMode==='custom'?r.searchEndMinutes:-1);out+=iniLine('UseWeekday',r.weekdayMode==='custom'?1:0);out+=iniLine('Weekdays',r.weekdays);out+=iniLine('Priority',r.priority);out+=iniLine('PreMarginSeconds',r.preMarginSeconds);out+=iniLine('PostMarginSeconds',r.postMarginSeconds);out+=iniLine('CreateReservationEnabled',r.createReservationEnabled?1:0);out+=iniLine('DuplicatePolicy',r.duplicatePolicy);out+=iniLine('SkipDuplicate',r.skipDuplicate?1:0);out+=iniLine('ExcludeRerunLikeProgram',r.excludeRerun?1:0);out+=iniLine('ActionMode',r.actionMode);out+='\\n';});return out;}\n";

            html += L"function buildIniPreview(){var out=makeIniText();var box=byId('iniPreview');if(box)box.value=out;return out;}\n";
            html += L"function encodeForm(payload){var pairs=[];Object.keys(payload||{}).forEach(function(key){var value=payload[key];if(value===undefined||value===null)return;pairs.push(encodeURIComponent(key)+'='+encodeURIComponent(String(value)));});return pairs.join('&');}\n";
            html += L"function parseIniText(text){text=String(def(text,'')).replace(/^\\uFEFF/,'');var current='';var sec={};text.split(/\\r?\\n/).forEach(function(line){line=String(line).trim();if(!line||line.charAt(0)===';'||line.charAt(0)==='#')return;if(line.charAt(0)==='['&&line.charAt(line.length-1)===']'){current=line.substring(1,line.length-1).trim();if(!sec[current])sec[current]={};return;}var eq=line.indexOf('=');if(eq<0||!current)return;var k=line.substring(0,eq).trim().toLowerCase();var v=line.substring(eq+1).trim();sec[current][k]=v;});return sec;}\n";
            html += L"function importIniBool(obj,key,d){var v=obj&&obj[key.toLowerCase()];if(v===undefined)return d;v=String(v).toLowerCase();return v==='1'||v==='true'||v==='yes'||v==='on';}\n";
            html += L"function importIniInt(obj,key,d){var v=obj&&obj[key.toLowerCase()];var x=Number(v);return Number.isFinite(x)?x:d;}\n";
            html += L"function importIniStr(obj,key,d){var v=obj&&obj[key.toLowerCase()];return v===undefined?d:String(v);}\n";
            html += L"function importJoin(v){v=String(def(v,'OR')).toUpperCase();return v==='AND'?'AND':'OR';}\n";
            html += L"function rulesFromIniText(text){var sec=parseIniText(text);var root=sec.AutoReserveRules||sec.autoreserverules||{};var count=importIniInt(root,'Count',-1);var names=Object.keys(sec).filter(function(name){return /^AutoReserveRule_\\d+$/i.test(name);}).sort(function(a,b){return Number(a.replace(/\\D/g,''))-Number(b.replace(/\\D/g,''));});if(count>=0){names=names.filter(function(name){return Number(name.replace(/\\D/g,''))<count;});}var out=[];names.forEach(function(name){var o=sec[name]||{};var useTime=importIniBool(o,'UseSearchTimeRange',importIniInt(o,'StartMinutes',-1)>=0&&importIniInt(o,'EndMinutes',-1)>=0);var useWeekday=importIniBool(o,'UseWeekday',false);out.push(normalizeRule({id:importIniInt(o,'Id',0),enabled:importIniBool(o,'Enabled',true),name:importIniStr(o,'Name',''),useKeyword:importIniBool(o,'UseKeyword',!!importIniStr(o,'Keyword','')),keyword:importIniStr(o,'Keyword',''),keywordJoinMode:importJoin(importIniStr(o,'KeywordJoinMode','OR')),useExclude:importIniBool(o,'UseExcludeKeyword',!!importIniStr(o,'ExcludeKeyword','')),excludeKeyword:importIniStr(o,'ExcludeKeyword',''),excludeJoinMode:importJoin(importIniStr(o,'ExcludeKeywordJoinMode','OR')),titleMatchMode:importIniInt(o,'TitleMatchMode',0),matchTitle:importIniBool(o,'MatchTitle',true),matchSummary:importIniBool(o,'MatchSummary',false),matchExtended:importIniBool(o,'MatchExtended',false),targetSpace:importIniInt(o,'TargetSpace',0),useService:importIniBool(o,'UseService',!!importIniStr(o,'ServiceName','')),serviceName:importIniStr(o,'ServiceName',''),useGenre:importIniBool(o,'UseGenre',!!importIniStr(o,'GenreName','')),genreName:importIniStr(o,'GenreName',''),genreJoinMode:importJoin(importIniStr(o,'GenreMatchMode','OR')),useMinDuration:importIniBool(o,'UseMinDuration',false),minDurationMinutes:importIniInt(o,'MinDurationMinutes',0),timeRangeMode:useTime?'custom':'all',searchStartMinutes:importIniInt(o,'SearchStartMinutes',importIniInt(o,'StartMinutes',0)),searchEndMinutes:importIniInt(o,'SearchEndMinutes',importIniInt(o,'EndMinutes',1440)),searchTimeWrap:importIniBool(o,'SearchTimeWrap',false),weekdayMode:useWeekday?'custom':'all',weekdays:importIniStr(o,'Weekdays','1111111'),priority:importIniInt(o,'Priority',50),preMarginSeconds:importIniInt(o,'PreMarginSeconds',0),postMarginSeconds:importIniInt(o,'PostMarginSeconds',0),createReservationEnabled:importIniBool(o,'CreateReservationEnabled',true),duplicatePolicy:importIniInt(o,'DuplicatePolicy',0),skipDuplicate:importIniBool(o,'SkipDuplicate',true),excludeRerun:importIniBool(o,'ExcludeRerunLikeProgram',false),actionMode:importIniStr(o,'ActionMode','candidate')}));});return out;}\n";
            html += L"function reservationApiBase(){var base='';try{if(window.EPG_RUNTIME&&window.EPG_RUNTIME.reservationApiBase)base=String(window.EPG_RUNTIME.reservationApiBase);}catch(e){}try{if(!base&&window.top&&window.top!==window&&window.top.EPG_RUNTIME&&window.top.EPG_RUNTIME.reservationApiBase)base=String(window.top.EPG_RUNTIME.reservationApiBase);}catch(e){}return base;}\n";
            html += L"function autoReserveSaveApiUrl(){var base=reservationApiBase();return base?base+'/auto_reserve_rules_save':'/api/reservation/auto_reserve_rules_save';}\n";
            html += L"function autoReserveCacheApiUrl(){return '/cache/auto_reserve/auto_reserve_rules.cache';}\n";
            html += L"function loadRulesCacheFromApp(){if(!window.fetch)return Promise.reject(new Error('このブラウザーはキャッシュ読込に対応していません。'));var url=autoReserveCacheApiUrl()+'?time='+encodeURIComponent(String(Date.now()));return fetch(url,{method:'GET',cache:'no-store',credentials:'same-origin'}).then(function(res){return res.text().then(function(body){return {ok:res.ok,status:res.status,body:body};});}).then(function(r){if(!r.ok)throw new Error('cache HTTP '+String(r.status)+(r.body?': '+r.body:''));rules=rulesFromIniText(r.body).map(normalizeRule);return rules;});}\n";
            html += L"function saveRulesToApp(showSuccess){var text=buildIniPreview();if(!window.fetch){alert('このブラウザでは本体保存APIを呼び出せません。');return Promise.resolve(false);}return fetch(autoReserveSaveApiUrl(),{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded; charset=UTF-8'},body:encodeForm({iniText:text}),cache:'no-store'}).then(function(res){return res.text().then(function(body){return {ok:res.ok,body:body};});}).then(function(r){var obj=null;try{obj=JSON.parse(r.body||'{}');}catch(e){}if(!r.ok||(obj&&obj.ok===false)){throw new Error((obj&&obj.message)?String(obj.message):(r.body||'save failed'));}return loadRulesCacheFromApp();}).then(function(){if(showSuccess!==false){alert('auto_reserve_rules.ini へ保存し、最新キャッシュを再読込しました。');}return true;}).catch(function(e){alert('auto_reserve_rules.ini へ保存できませんでした。\\n'+String(e&&e.message?e.message:e));return false;});}\n";

            html += L"window.saveCurrentRule=function(){var idx=findIndexById(currentId);if(idx<0){alert('対象ルールが見つかりません。');return;}var beforeId=currentId;var r=readForm();if(!r.name&&r.keyword)r.name=r.keyword;r.id=beforeId;rules[idx]=r;buildIniPreview();saveRulesToApp(true).then(function(ok){if(ok){location.href='epg_auto_reserve.html';}});};\n";
            html += L"window.deleteCurrentRule=function(){var idx=findIndexById(currentId);if(idx<0){alert('対象ルールが見つかりません。');return;}if(!confirm('この自動予約ルールを削除しますか？'))return;var removeId=currentId;rules=rules.filter(function(x){return Number(x.id)!==Number(removeId);});buildIniPreview();saveRulesToApp(true).then(function(ok){if(ok){location.href='epg_auto_reserve.html';}});};\n";

            html += L"document.querySelectorAll('input,select').forEach(function(el){el.addEventListener('input',function(){syncEnabledSwitchText();updateUiState();buildIniPreview();});el.addEventListener('change',function(){syncEnabledSwitchText();updateUiState();buildIniPreview();});});\n";
            html += L"var detailPreview=document.querySelector('.detailPreview');if(detailPreview){detailPreview.addEventListener('toggle',syncHostFrameHeight);}\n";
            html += L"installHostFrameAutoResize();\n";

            html += L"loadRulesCacheFromApp().then(function(){";
            html += L"var idx=findIndexById(currentId);";
            html += L"if(idx<0){var st=byId('detailStatus');if(st)st.textContent='対象ルールは削除済み、または存在しません。作成ページへ戻って一覧を更新してください。';buildIniPreview();syncHostFrameHeight();return;}";
            html += L"writeForm(rules[idx]);";
            html += L"scrollPageTop();syncHostFrameHeight();";
            html += L"}).catch(function(e){";
            html += L"var st2=byId('detailStatus');";
            html += L"if(st2)st2.textContent='最新の自動予約キャッシュを読み込めませんでした: '+String(e&&e.message?e.message:e);";
            html += L"syncHostFrameHeight();";
            html += L"try{console.error('[auto-reserve-detail] init failed',e);}catch(e2){}";
            html += L"});\n";

            html += L"})();\n";
            html += L"</script>\n";
        }

        static std::wstring BuildAutoReserveRuleDetailPageHtml_(
            const EpgHtmlPortal::WriteOptions& opt,
            const std::filesystem::path& rulesIniPath,
            const std::vector<AutoReserve::Rule>& rules)
        {
            const std::vector<ConfigServiceOption_> services =
                LoadEnabledConfigServices_(opt);

            std::wstring html;
            html.reserve(112 * 1024);

            html += L"<!DOCTYPE html>\n";
            html += L"<html lang=\"ja\">\n";
            html += L"<head>\n";
            html += L"<meta charset=\"UTF-8\">\n";
            html += L"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
            html += L"<title>自動予約ルール詳細</title>\n";
            AppendPageStyle_(html);
            AppendRuleDetailPageStyle_(html);
            html += L"</head>\n";
            html += L"<body>\n";
            html += L"<div class=\"shell\">\n";

            html += L"<section class=\"header\">";
            html += L"<div class=\"headerMain\">";
            html += L"<h1>自動予約ルール詳細</h1>";
            html += L"<p>作成ページの一覧から呼び出した自動予約ルールを表示し、登録情報を変更・削除します。</p>";
            html += L"</div>";
            html += L"<div class=\"headerMeta\">";
            html += L"INI: ";
            html += EpgHtmlPortal_Common::EscapeHtmlW(rulesIniPath.wstring());
            html += L"<br>読込 ";
            html += std::to_wstring(rules.size());
            html += L" 件 / 有効放送局候補 ";
            html += std::to_wstring(services.size());
            html += L" 件<br><a class=\"manageLink\" href=\"epg_auto_reserve.html\">作成ページへ戻る</a>";
            html += L"</div>";
            html += L"</section>";

            AppendRuleDetailEditorHtml_(html, services);
            AppendRuleDetailPageScript_(html);

            html += L"</div>\n";
            html += L"</body>\n";
            html += L"</html>\n";

            return html;
        }

    }

    std::filesystem::path MakeAutoReservePagePath(
        const EpgHtmlPortal::WriteOptions& opt)
    {
        if (opt.outputDir.empty()) {
            return {};
        }

        return opt.outputDir / L"epg_auto_reserve.html";
    }

    std::filesystem::path MakeAutoReserveRuleDetailPagePath(
        const EpgHtmlPortal::WriteOptions& opt)
    {
        if (opt.outputDir.empty()) {
            return {};
        }

        return opt.outputDir / L"epg_auto_reserve_rule_detail.html";
    }

    bool WriteAutoReservePage(
        const EpgHtmlPortal::WriteOptions& opt)
    {
        const auto pageBegin =
            std::chrono::steady_clock::now();

        if (opt.outputDir.empty()) {
            DebugLog(L"[EPG-PORTAL][AUTO][ERROR] outputDir is empty");
            return false;
        }

        const std::filesystem::path outPath =
            MakeAutoReservePagePath(opt);

        if (outPath.empty()) {
            DebugLog(L"[EPG-PORTAL][AUTO][ERROR] output path is empty");
            return false;
        }

        if (!EpgHtmlPortal_Common::EnsureParentDirectory(outPath)) {
            DebugLogFormat(
                L"[EPG-PORTAL][AUTO][ERROR] directory create failed: %ls",
                outPath.parent_path().wstring().c_str());
            return false;
        }

        const std::filesystem::path rulesIniPath =
            AutoReserve::GetDefaultRulesIniPath();

        AutoReserve::RuleStore store;
        const auto loadBegin =
            std::chrono::steady_clock::now();

        const auto load =
            store.LoadFromIni(rulesIniPath.wstring());

        const auto loadMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - loadBegin).count();

        DebugLogFormat(
            L"[EPG-PORTAL][AUTO] rule load result ok=%d loaded=%zu ms=%lld path=%ls",
            load.ok ? 1 : 0,
            load.loadedCount,
            static_cast<long long>(loadMs),
            rulesIniPath.wstring().c_str());

        if (!load.ok) {
            DebugLogFormat(
                L"[EPG-PORTAL][AUTO][WARN] rule load failed: %ls path=%ls",
                load.message.c_str(),
                rulesIniPath.wstring().c_str());
        }

        if (!FileExistsNoThrow_(rulesIniPath)) {
            const auto initSave =
                store.SaveToIni(rulesIniPath.wstring());

            DebugLogFormat(
                L"[EPG-PORTAL][AUTO] create default auto_reserve_rules.ini ok=%d path=%ls",
                initSave.ok ? 1 : 0,
                rulesIniPath.wstring().c_str());
        }

        try {
            const std::wstring html =
                BuildAutoReservePageHtml_(
                    opt,
                    rulesIniPath,
                    store.GetRules());

            DebugLogFormat(
                L"[EPG-PORTAL][AUTO] html size=%zu chars rules=%zu",
                html.size(),
                store.GetRules().size());

            if (html.size() > 4u * 1024u * 1024u) {
                DebugLogFormat(
                    L"[EPG-PORTAL][AUTO][WARN] 自動予約HTMLサイズが大きめです: %zu chars",
                    html.size());
            }

            const bool ok =
                EpgHtmlPortal_Common::WriteUtf8BomTextFile(
                    outPath,
                    html);

            DebugLogFormat(
                L"[EPG-PORTAL][AUTO] write %ls path=%ls rules=%zu",
                ok ? L"OK" : L"NG",
                outPath.wstring().c_str(),
                store.GetRules().size());

            const auto totalMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - pageBegin).count();

            DebugLogFormat(
                L"[EPG-PORTAL][AUTO] page total ms=%lld rules=%zu htmlChars=%zu path=%ls",
                static_cast<long long>(totalMs),
                store.GetRules().size(),
                html.size(),
                outPath.wstring().c_str());

            return ok;
        }
        catch (const std::bad_alloc&) {
            DebugLog(L"[EPG-PORTAL][AUTO][ERROR] std::bad_alloc: 自動予約ページ生成中にメモリ確保失敗");
            return false;
        }
        catch (const std::exception& e) {
            DebugLogFormat(
                L"[EPG-PORTAL][AUTO][ERROR] exception: %hs",
                e.what());
            return false;
        }
        catch (...) {
            DebugLog(L"[EPG-PORTAL][AUTO][ERROR] unknown exception");
            return false;
        }
    }





    bool WriteAutoReserveRuleDetailPage(
        const EpgHtmlPortal::WriteOptions& opt)
    {
        const auto pageBegin =
            std::chrono::steady_clock::now();

        if (opt.outputDir.empty()) {
            DebugLog(L"[EPG-PORTAL][AUTO-DETAIL][ERROR] outputDir is empty");
            return false;
        }

        const std::filesystem::path outPath =
            MakeAutoReserveRuleDetailPagePath(opt);

        if (outPath.empty()) {
            DebugLog(L"[EPG-PORTAL][AUTO-DETAIL][ERROR] output path is empty");
            return false;
        }

        if (!EpgHtmlPortal_Common::EnsureParentDirectory(outPath)) {
            DebugLogFormat(
                L"[EPG-PORTAL][AUTO-DETAIL][ERROR] directory create failed: %ls",
                outPath.parent_path().wstring().c_str());
            return false;
        }

        const std::filesystem::path rulesIniPath =
            AutoReserve::GetDefaultRulesIniPath();

        AutoReserve::RuleStore store;

        const auto loadBegin =
            std::chrono::steady_clock::now();

        const auto load =
            store.LoadFromIni(rulesIniPath.wstring());

        const auto loadMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - loadBegin).count();

        DebugLogFormat(
            L"[EPG-PORTAL][AUTO-DETAIL] rule load result ok=%d loaded=%zu ms=%lld path=%ls",
            load.ok ? 1 : 0,
            load.loadedCount,
            static_cast<long long>(loadMs),
            rulesIniPath.wstring().c_str());

        if (!load.ok) {
            DebugLogFormat(
                L"[EPG-PORTAL][AUTO-DETAIL][WARN] rule load failed: %ls path=%ls",
                load.message.c_str(),
                rulesIniPath.wstring().c_str());
        }

        if (!FileExistsNoThrow_(rulesIniPath)) {
            const auto initSave =
                store.SaveToIni(rulesIniPath.wstring());

            DebugLogFormat(
                L"[EPG-PORTAL][AUTO-DETAIL] create default auto_reserve_rules.ini ok=%d path=%ls",
                initSave.ok ? 1 : 0,
                rulesIniPath.wstring().c_str());
        }

        try {
            const std::wstring html =
                BuildAutoReserveRuleDetailPageHtml_(
                    opt,
                    rulesIniPath,
                    store.GetRules());

            DebugLogFormat(
                L"[EPG-PORTAL][AUTO-DETAIL] html size=%zu chars rules=%zu",
                html.size(),
                store.GetRules().size());

            if (html.size() > 4u * 1024u * 1024u) {
                DebugLogFormat(
                    L"[EPG-PORTAL][AUTO-DETAIL][WARN] 自動予約詳細HTMLサイズが大きめです: %zu chars",
                    html.size());
            }

            const bool ok =
                EpgHtmlPortal_Common::WriteUtf8BomTextFile(
                    outPath,
                    html);

            const auto totalMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - pageBegin).count();

            DebugLogFormat(
                L"[EPG-PORTAL][AUTO-DETAIL] write %ls path=%ls rules=%zu htmlChars=%zu ms=%lld",
                ok ? L"OK" : L"NG",
                outPath.wstring().c_str(),
                store.GetRules().size(),
                html.size(),
                static_cast<long long>(totalMs));

            return ok;
        }
        catch (const std::bad_alloc&) {
            DebugLog(L"[EPG-PORTAL][AUTO-DETAIL][ERROR] std::bad_alloc: 自動予約詳細ページ生成中にメモリ確保失敗");
            return false;
        }
        catch (const std::exception& e) {
            DebugLogFormat(
                L"[EPG-PORTAL][AUTO-DETAIL][ERROR] exception: %hs",
                e.what());
            return false;
        }
        catch (...) {
            DebugLog(L"[EPG-PORTAL][AUTO-DETAIL][ERROR] unknown exception");
            return false;
        }
    }

}
