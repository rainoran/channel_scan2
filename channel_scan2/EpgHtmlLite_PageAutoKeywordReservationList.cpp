#include "EpgHtmlLite_PageAutoKeywordReservationList.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <Windows.h>
#endif

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <cwchar>
#include <system_error>
#include <utility>

#include "AutoReserveRuleStore.hpp"

namespace
{
    using namespace EpgHtmlLite;

    constexpr const wchar_t* kListFilePrefix_ =
        L"epg_lite_auto_keyword_reservation_list_space";

    constexpr const wchar_t* kEditFilePrefix_ =
        L"epg_lite_auto_keyword_reservation_edit_space";

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
            case L'\r':
            case L'\n':
            case L'\t':
                out.push_back(L' ');
                break;
            default:
                out.push_back(ch);
                break;
            }
        }

        return out;
    }

    std::wstring JsString_(
        std::wstring_view s)
    {
        std::wstring out;
        out.reserve(s.size() + 16);

        out.push_back(L'\'');

        for (const wchar_t ch : s) {
            switch (ch) {
            case L'\\':
                out += L"\\\\";
                break;
            case L'\'':
                out += L"\\'";
                break;
            case L'"':
                out += L"\\\"";
                break;
            case L'\r':
            case L'\n':
            case L'\t':
                out.push_back(L' ');
                break;
            case L'<':
                out += L"\\u003C";
                break;
            case L'>':
                out += L"\\u003E";
                break;
            case L'&':
                out += L"\\u0026";
                break;
            default:
                out.push_back(ch);
                break;
            }
        }

        out.push_back(L'\'');
        return out;
    }

    bool WriteUtf8BomTextFile_(
        const std::filesystem::path& path,
        const std::wstring& content)
    {
        const std::filesystem::path parent =
            path.parent_path();

        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(
                parent,
                ec);

            if (ec) {
                return false;
            }
        }

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
        if (!content.empty()) {
            if (content.size() >
                static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
                return false;
            }

            const int srcLen =
                static_cast<int>(content.size());

            const int required =
                ::WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    content.data(),
                    srcLen,
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
                    content.data(),
                    srcLen,
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
        }
#else
        for (const wchar_t wc : content) {
            ofs.put(static_cast<char>(wc & 0xFF));
        }
#endif

        ofs.flush();
        return ofs.good();
    }

    void AppendCssLine_(
        std::wostringstream& oss,
        const wchar_t* line)
    {
        oss << line << L"\n";
    }

    void AppendJsLine_(
        std::wostringstream& oss,
        const wchar_t* line)
    {
        oss << line << L"\n";
    }

    std::wstring MakeSpaceLabel_(
        int space)
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

    std::wstring ListFileNameFor_(
        int space)
    {
        return std::wstring(kListFilePrefix_) +
            std::to_wstring(space) +
            L".html";
    }

    std::wstring EditFileNameFor_(
        int space)
    {
        return std::wstring(kEditFilePrefix_) +
            std::to_wstring(space) +
            L".html";
    }

    std::wstring MatchModeText_(
        AutoReserve::MatchMode mode)
    {
        switch (mode) {
        case AutoReserve::MatchMode::Exact:
            return L"exact";
        case AutoReserve::MatchMode::StartsWith:
            return L"prefix";
        case AutoReserve::MatchMode::EndsWith:
            return L"suffix";
        case AutoReserve::MatchMode::Contains:
        default:
            return L"contains";
        }
    }

    std::wstring MatchModeLabel_(
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
            return L"含む";
        }
    }

    std::wstring DuplicatePolicyText_(
        AutoReserve::DuplicatePolicy policy)
    {
        switch (policy) {
        case AutoReserve::DuplicatePolicy::SkipSameTitleAndTime:
            return L"titleTime";
        case AutoReserve::DuplicatePolicy::Allow:
            return L"allow";
        case AutoReserve::DuplicatePolicy::SkipSameEvent:
        default:
            return L"event";
        }
    }

    std::wstring DuplicatePolicyLabel_(
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





    std::wstring TimeLabel_(
        int minutes)
    {
        if (minutes < 0) {
            return L"指定なし";
        }

        if (minutes == 1440) {
            return L"24:00";
        }

        const int h =
            minutes / 60;

        const int m =
            minutes % 60;

        wchar_t buf[16]{};
#ifdef _WIN32
        swprintf_s(
            buf,
            L"%02d:%02d",
            h,
            m);
#else
        std::swprintf(
            buf,
            16,
            L"%02d:%02d",
            h,
            m);
#endif

        return std::wstring(buf);
    }

    std::wstring TimeRangeLabel_(
        const AutoReserve::Rule& r)
    {
        if (!r.useSearchTimeRange &&
            r.startMinutes < 0 &&
            r.endMinutes < 0) {
            return L"すべて";
        }

        const int start =
            r.useSearchTimeRange
            ? r.searchStartMinutes
            : r.startMinutes;

        const int end =
            r.useSearchTimeRange
            ? r.searchEndMinutes
            : r.endMinutes;

        std::wstring text =
            TimeLabel_(start) +
            L" - " +
            TimeLabel_(end);

        if (r.searchTimeWrap) {
            text += L" / 日付またぎ";
        }

        return text;
    }

    std::wstring WeekdayLabel_(
        const AutoReserve::Rule& r)
    {
        if (!r.useWeekday) {
            return L"すべて";
        }

        const wchar_t* names[7] = {
            L"月",
            L"火",
            L"水",
            L"木",
            L"金",
            L"土",
            L"日"
        };

        const std::wstring bits =
            r.weekdays.empty()
            ? std::wstring(L"1111111")
            : r.weekdays;

        std::wstring out;

        for (std::size_t i = 0; i < 7; ++i) {
            if (i < bits.size() &&
                bits[i] == L'1') {
                if (!out.empty()) {
                    out += L" ";
                }

                out += names[i];
            }
        }

        return out.empty()
            ? L"未選択"
            : out;
    }

    void AppendCommonCss_(
        std::wostringstream& oss)
    {
        AppendCssLine_(oss, L":root{--bg:#0f131a;--panel:#181f29;--panel2:#1d2632;--line:#2d3848;--text:#e8edf5;--sub:#aab6c8;--accent:#70a7ff;--danger:#ff9b9b;--ok:#8ee6ad;--warn:#f6d36b;}");
        AppendCssLine_(oss, L"*{box-sizing:border-box;}");
        AppendCssLine_(oss, L"body{margin:0;font-family:\"Segoe UI\",\"Yu Gothic UI\",\"Meiryo\",sans-serif;background:var(--bg);color:var(--text);}");
        AppendCssLine_(oss, L"main{padding:12px;}");
        AppendCssLine_(oss, L".panel{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:12px 14px;margin-bottom:12px;}");
        AppendCssLine_(oss, L"h1{margin:0 0 8px;font-size:22px;}");
        AppendCssLine_(oss, L"h2{margin:0 0 8px;font-size:17px;}");
        AppendCssLine_(oss, L".note{color:var(--sub);font-size:12px;line-height:1.65;}");
        AppendCssLine_(oss, L"a{color:#b8d5ff;text-decoration:none;}");
        AppendCssLine_(oss, L"a:hover{text-decoration:underline;}");
        AppendCssLine_(oss, L".actions{display:flex;gap:8px;flex-wrap:wrap;align-items:center;}");
        AppendCssLine_(oss, L"button,.btn{appearance:none;border:1px solid #4b5f7f;background:#1c2b3f;color:#e8f1ff;border-radius:999px;padding:7px 12px;font-size:12px;font-weight:800;cursor:pointer;text-decoration:none;display:inline-block;}");
        AppendCssLine_(oss, L"button:hover,.btn:hover{border-color:#7ebaff;text-decoration:none;}");
        AppendCssLine_(oss, L"button.danger,.btn.danger{background:#5b2424;border-color:#b85b5b;}");
        AppendCssLine_(oss, L".status{margin-top:8px;font-size:12px;color:var(--sub);}");
        AppendCssLine_(oss, L".status.error{color:var(--danger);}");
        AppendCssLine_(oss, L".empty-box{padding:18px;border:1px dashed #425067;border-radius:12px;color:var(--sub);font-size:13px;line-height:1.7;}");
        AppendCssLine_(oss, L"input,select,textarea{background:#101720;color:var(--text);border:1px solid #43516a;border-radius:10px;padding:8px 10px;font-size:13px;}");
        AppendCssLine_(oss, L"input:focus,select:focus,textarea:focus{outline:2px solid rgba(112,167,255,.35);border-color:var(--accent);}");
        AppendCssLine_(oss, L"textarea{width:100%;min-height:150px;font-family:Consolas,\"Yu Gothic UI\",monospace;}");
        AppendCssLine_(oss, L"select[multiple]{min-height:132px;}");
        AppendCssLine_(oss, L".rule-table{width:100%;border-collapse:collapse;font-size:12px;}");
        AppendCssLine_(oss, L".rule-table th,.rule-table td{border:1px solid var(--line);padding:7px 8px;vertical-align:top;}");
        AppendCssLine_(oss, L".rule-table th{background:#202a38;color:#fff;}");
        AppendCssLine_(oss, L".rule-table td{background:#151d27;}");
        AppendCssLine_(oss, L".badge{display:inline-block;border-radius:999px;padding:2px 8px;font-size:11px;font-weight:800;border:1px solid #4b5f7f;}");
        AppendCssLine_(oss, L".badge.on{color:var(--ok);border-color:#3d8b5e;background:#123321;}");
        AppendCssLine_(oss, L".badge.off{color:var(--warn);border-color:#8a7130;background:#332a13;}");
        AppendCssLine_(oss, L".disabled-row{opacity:.62;}");
        AppendCssLine_(oss, L".form-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;align-items:start;}");
        AppendCssLine_(oss, L".field{display:flex;flex-direction:column;gap:4px;font-size:12px;color:var(--sub);}");
        AppendCssLine_(oss, L".field.wide{grid-column:1/-1;}");
        AppendCssLine_(oss, L".check-row{display:flex;gap:10px;flex-wrap:wrap;align-items:center;}");
        AppendCssLine_(oss, L".check-row label{display:inline-flex;gap:4px;align-items:center;color:var(--text);font-size:12px;}");
        AppendCssLine_(oss, L".mini{font-size:11px;color:var(--sub);line-height:1.5;}");
        AppendCssLine_(oss, L"@media(max-width:820px){.form-grid{grid-template-columns:1fr;}.field.wide{grid-column:auto;}.rule-table{font-size:11px;}}");
    }

    void AppendOption_(
        std::wostringstream& oss,
        const std::wstring& value,
        const std::wstring& label)
    {
        oss << L"<option value=\""
            << HtmlEscape_(value)
            << L"\">"
            << HtmlEscape_(label)
            << L"</option>\n";
    }

    void AppendGenreOptions_(
        std::wostringstream& oss)
    {
        AppendOption_(oss, L"ニュース・報道", L"ニュース・報道");
        AppendOption_(oss, L"スポーツ", L"スポーツ");
        AppendOption_(oss, L"ドラマ", L"ドラマ");
        AppendOption_(oss, L"音楽", L"音楽");
        AppendOption_(oss, L"バラエティー", L"バラエティー");
        AppendOption_(oss, L"映画", L"映画");
        AppendOption_(oss, L"アニメ・特撮", L"アニメ・特撮");
        AppendOption_(oss, L"情報・ワイドショー", L"情報・ワイドショー");
        AppendOption_(oss, L"ドキュメンタリー", L"ドキュメンタリー");
        AppendOption_(oss, L"劇場・公演", L"劇場・公演");
        AppendOption_(oss, L"趣味・教育", L"趣味・教育");
        AppendOption_(oss, L"その他", L"その他");
    }


    std::wstring TrimConfigText_(
        std::wstring_view s)
    {
        std::size_t first = 0;

        while (first < s.size()) {
            const wchar_t ch = s[first];

            if (ch != L' ' &&
                ch != L'\t' &&
                ch != L'\r' &&
                ch != L'\n' &&
                ch != L'　') {
                break;
            }

            ++first;
        }

        std::size_t last = s.size();

        while (last > first) {
            const wchar_t ch = s[last - 1];

            if (ch != L' ' &&
                ch != L'\t' &&
                ch != L'\r' &&
                ch != L'\n' &&
                ch != L'　') {
                break;
            }

            --last;
        }

        return std::wstring(
            s.substr(
                first,
                last - first));
    }

    bool ContainsTextValue_(
        const std::vector<std::wstring>& values,
        const std::wstring& value)
    {
        return std::find(
            values.begin(),
            values.end(),
            value) != values.end();
    }

    void AddUniqueTextValue_(
        std::vector<std::wstring>& values,
        std::wstring value)
    {
        value =
            TrimConfigText_(value);

        if (value.empty()) {
            return;
        }

        if (!ContainsTextValue_(
            values,
            value)) {
            values.push_back(
                std::move(value));
        }
    }

    void AddPipeSeparatedTextValues_(
        std::vector<std::wstring>& values,
        std::wstring_view text)
    {
        std::size_t begin = 0;

        while (begin <= text.size()) {
            const std::size_t pos =
                text.find(
                    L'|',
                    begin);

            if (pos == std::wstring_view::npos) {
                AddUniqueTextValue_(
                    values,
                    std::wstring(
                        text.substr(begin)));
                break;
            }

            AddUniqueTextValue_(
                values,
                std::wstring(
                    text.substr(
                        begin,
                        pos - begin)));

            begin = pos + 1;
        }
    }

    std::vector<std::wstring> SplitConfigCsvLine_(
        std::wstring_view line)
    {
        std::vector<std::wstring> fields;
        std::wstring current;
        bool inQuote = false;

        for (const wchar_t ch : line) {
            if (ch == L'"') {
                inQuote = !inQuote;
                continue;
            }

            if (ch == L',' && !inQuote) {
                fields.push_back(
                    TrimConfigText_(current));

                current.clear();
                continue;
            }

            current.push_back(ch);
        }

        fields.push_back(
            TrimConfigText_(current));

        return fields;
    }

    bool TryParseConfigInt_(
        const std::wstring& text,
        int& out) noexcept
    {
        out = 0;

        const std::wstring s =
            TrimConfigText_(text);

        if (s.empty()) {
            return false;
        }

        wchar_t* endPtr = nullptr;

        const long value =
            std::wcstol(
                s.c_str(),
                &endPtr,
                10);

        if (endPtr == s.c_str()) {
            return false;
        }

        while (endPtr && *endPtr != L'\0') {
            const wchar_t ch = *endPtr;

            if (ch != L' ' &&
                ch != L'\t' &&
                ch != L'\r' &&
                ch != L'\n' &&
                ch != L'　') {
                return false;
            }

            ++endPtr;
        }

        out = static_cast<int>(value);
        return true;
    }

    std::vector<std::filesystem::path> BuildConfigIniCandidates_(
        const Context& ctx)
    {
        std::vector<std::filesystem::path> paths;

        if (!ctx.outputDir.empty()) {
            paths.push_back(
                ctx.outputDir /
                L"config.ini");

            paths.push_back(
                ctx.outputDir.parent_path() /
                L"config.ini");
        }

        if (!ctx.mergedEpgPath.empty()) {
            const std::filesystem::path mergedPath(
                ctx.mergedEpgPath);

            if (!mergedPath.parent_path().empty()) {
                paths.push_back(
                    mergedPath.parent_path() /
                    L"config.ini");
            }
        }

        if (!ctx.channelListPath.empty()) {
            const std::filesystem::path chPath(
                ctx.channelListPath);

            if (!chPath.parent_path().empty()) {
                paths.push_back(
                    chPath.parent_path() /
                    L"config.ini");
            }
        }

        std::error_code ec;

        const std::filesystem::path current =
            std::filesystem::current_path(ec);

        if (!ec && !current.empty()) {
            paths.push_back(
                current /
                L"config.ini");
        }

        std::vector<std::filesystem::path> unique;

        for (const auto& path : paths) {
            if (path.empty()) {
                continue;
            }

            bool exists = false;

            for (const auto& item : unique) {
                if (item == path) {
                    exists = true;
                    break;
                }
            }

            if (!exists) {
                unique.push_back(path);
            }
        }

        return unique;
    }

    bool TryLoadEnabledServiceNamesFromConfig_(
        const std::filesystem::path& configPath,
        int space,
        std::vector<std::wstring>& out)
    {
#ifdef _WIN32
        std::error_code ec;

        if (configPath.empty() ||
            !std::filesystem::exists(
                configPath,
                ec) ||
            ec) {
            return false;
        }

        const std::wstring sectionName =
            L"EpgServiceEnable_space" +
            std::to_wstring(space);

        std::vector<wchar_t> buffer(
            32768,
            L'\0');

        DWORD copied = 0;

        for (;;) {
            copied =
                ::GetPrivateProfileSectionW(
                    sectionName.c_str(),
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    configPath.c_str());

            if (copied < buffer.size() - 2) {
                break;
            }

            if (buffer.size() >= 1024 * 1024) {
                return false;
            }

            buffer.assign(
                buffer.size() * 2,
                L'\0');
        }

        if (copied == 0) {
            return false;
        }

        const wchar_t* p =
            buffer.data();

        while (p && *p != L'\0') {
            const std::wstring line(p);

            const std::size_t eq =
                line.find(L'=');

            if (eq != std::wstring::npos) {
                const std::wstring valueText =
                    TrimConfigText_(
                        std::wstring_view(line).substr(eq + 1));

                const std::vector<std::wstring> fields =
                    SplitConfigCsvLine_(valueText);

                if (fields.size() >= 2) {
                    int enabled = 0;

                    if (TryParseConfigInt_(
                        fields[0],
                        enabled) &&
                        enabled != 0) {
                        AddUniqueTextValue_(
                            out,
                            fields[1]);
                    }
                }
            }

            p += std::wcslen(p) + 1;
        }

        return true;
#else
        (void)configPath;
        (void)space;
        (void)out;
        return false;
#endif
    }

    std::vector<std::wstring> BuildEnabledServiceNamesFromConfig_(
        const Context& ctx)
    {
        std::vector<std::wstring> services;

        const std::vector<std::filesystem::path> candidates =
            BuildConfigIniCandidates_(ctx);

        for (const auto& path : candidates) {
            std::vector<std::wstring> loaded;

            if (!TryLoadEnabledServiceNamesFromConfig_(
                path,
                ctx.space,
                loaded)) {
                continue;
            }

            if (!loaded.empty()) {
                return loaded;
            }
        }

        return services;
    }

    void AppendServiceOptionsForEdit_(
        std::wostringstream& oss,
        const Context& ctx,
        const std::vector<AutoReserve::Rule>& rules)
    {
        std::vector<std::wstring> services =
            BuildEnabledServiceNamesFromConfig_(ctx);

        for (const AutoReserve::Rule& rule : rules) {
            AddPipeSeparatedTextValues_(
                services,
                rule.serviceName);
        }

        std::sort(
            services.begin(),
            services.end());

        for (const std::wstring& service : services) {
            AppendOption_(
                oss,
                service,
                service);
        }
    }


    void AppendTimeOptions_(
        std::wostringstream& oss,
        int begin,
        int end,
        int step)
    {
        if (step <= 0) {
            step = 30;
        }

        for (int m = begin; m <= end; m += step) {
            AppendOption_(
                oss,
                std::to_wstring(m),
                TimeLabel_(m));
        }
    }


    void AppendAutoReserveRuleCommonRuntimeScript_(
        std::wostringstream& oss)
    {
        AppendJsLine_(oss, L"function byId(id){return document.getElementById(id);}");
        AppendJsLine_(oss, L"function def(v,d){return v===undefined||v===null?d:v;}");
        AppendJsLine_(oss, L"function esc(s){return String(def(s,'')).replace(/[&<>\\\"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','\\\"':'&quot;',\"'\":'&#39;'}[c];});}");
        AppendJsLine_(oss, L"function encodeForm(payload){var a=[];Object.keys(payload||{}).forEach(function(k){var v=payload[k];if(v===undefined||v===null)return;a.push(encodeURIComponent(k)+'='+encodeURIComponent(String(v)));});return a.join('&');}");
        AppendJsLine_(oss, L"function boolFrom(v,d){if(v===true)return true;if(v===false)return false;if(v===undefined||v===null)return !!d;var s=String(v).toLowerCase().trim();if(s==='1'||s==='true'||s==='yes'||s==='on')return true;if(s==='0'||s==='false'||s==='no'||s==='off')return false;return !!d;}");
        AppendJsLine_(oss, L"function numFrom(v,d){var n=Number(v);return Number.isFinite(n)?Math.trunc(n):d;}");
        AppendJsLine_(oss, L"function textFrom(v,d){return String(def(v,def(d,'')));}");
        AppendJsLine_(oss, L"function splitValues(s){return String(def(s,'')).split('|').map(function(x){return x.trim();}).filter(function(x){return x.length>0;});}");
        AppendJsLine_(oss, L"function joinValues(a){return (Array.isArray(a)?a:[]).map(function(x){return String(x).trim();}).filter(function(x){return x.length>0;}).join('|');}");
        AppendJsLine_(oss, L"function normalizeActionMode(v){v=String(def(v,'candidate')).toLowerCase().trim();return v==='auto'?'auto':'candidate';}");
        AppendJsLine_(oss, L"function normalizeRule(r){");
        AppendJsLine_(oss, L"r=r||{};");
        AppendJsLine_(oss, L"return {");
        AppendJsLine_(oss, L"id:numFrom(r.id,0),");
        AppendJsLine_(oss, L"enabled:boolFrom(r.enabled,true),");
        AppendJsLine_(oss, L"name:textFrom(r.name,''),");
        AppendJsLine_(oss, L"keyword:textFrom(r.keyword,''),");
        AppendJsLine_(oss, L"excludeKeyword:textFrom(r.excludeKeyword,''),");
        AppendJsLine_(oss, L"titleMatchMode:numFrom(r.titleMatchMode,0),");
        AppendJsLine_(oss, L"matchTitle:boolFrom(r.matchTitle,true),");
        AppendJsLine_(oss, L"matchSummary:boolFrom(r.matchSummary,true),");
        AppendJsLine_(oss, L"matchExtended:boolFrom(r.matchExtended,false),");
        AppendJsLine_(oss, L"targetSpace:numFrom(r.targetSpace,0),");
        AppendJsLine_(oss, L"serviceName:textFrom(r.serviceName,''),");
        AppendJsLine_(oss, L"genreName:textFrom(r.genreName,''),");
        AppendJsLine_(oss, L"useKeyword:boolFrom(r.useKeyword,String(def(r.keyword,'')).length>0),");
        AppendJsLine_(oss, L"keywordJoinMode:textFrom(r.keywordJoinMode,'OR'),");
        AppendJsLine_(oss, L"useExclude:boolFrom(r.useExclude,String(def(r.excludeKeyword,'')).length>0),");
        AppendJsLine_(oss, L"excludeJoinMode:textFrom(r.excludeJoinMode,'OR'),");
        AppendJsLine_(oss, L"useService:boolFrom(r.useService,String(def(r.serviceName,'')).length>0),");
        AppendJsLine_(oss, L"useGenre:boolFrom(r.useGenre,String(def(r.genreName,'')).length>0),");
        AppendJsLine_(oss, L"genreJoinMode:textFrom(r.genreJoinMode,'OR'),");
        AppendJsLine_(oss, L"useMinDuration:boolFrom(r.useMinDuration,false),");
        AppendJsLine_(oss, L"minDurationMinutes:numFrom(r.minDurationMinutes,0),");
        AppendJsLine_(oss, L"useSearchTimeRange:boolFrom(r.useSearchTimeRange,false),");
        AppendJsLine_(oss, L"searchStartMinutes:numFrom(r.searchStartMinutes,0),");
        AppendJsLine_(oss, L"searchEndMinutes:numFrom(r.searchEndMinutes,1440),");
        AppendJsLine_(oss, L"searchTimeWrap:boolFrom(r.searchTimeWrap,false),");
        AppendJsLine_(oss, L"startMinutes:numFrom(r.startMinutes,-1),");
        AppendJsLine_(oss, L"endMinutes:numFrom(r.endMinutes,-1),");
        AppendJsLine_(oss, L"useWeekday:boolFrom(r.useWeekday,false),");
        AppendJsLine_(oss, L"weekdays:textFrom(r.weekdays,'1111111'),");
        AppendJsLine_(oss, L"priority:numFrom(r.priority,50),");
        AppendJsLine_(oss, L"preMarginSeconds:numFrom(r.preMarginSeconds,0),");
        AppendJsLine_(oss, L"postMarginSeconds:numFrom(r.postMarginSeconds,0),");
        AppendJsLine_(oss, L"createReservationEnabled:boolFrom(r.createReservationEnabled,true),");
        AppendJsLine_(oss, L"duplicatePolicy:numFrom(r.duplicatePolicy,0),");
        AppendJsLine_(oss, L"skipDuplicate:boolFrom(r.skipDuplicate,true),");
        AppendJsLine_(oss, L"excludeRerun:boolFrom(r.excludeRerun,false),");
        AppendJsLine_(oss, L"actionMode:normalizeActionMode(r.actionMode)");
        AppendJsLine_(oss, L"};");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"function parseIniRules(text){");
        AppendJsLine_(oss, L"var src=String(def(text,'')).replace(/^\\uFEFF/,'');");
        AppendJsLine_(oss, L"var lines=src.split(/\\r?\\n/);");
        AppendJsLine_(oss, L"var sections={};");
        AppendJsLine_(oss, L"var cur='';");
        AppendJsLine_(oss, L"lines.forEach(function(line){");
        AppendJsLine_(oss, L"var s=String(line||'').trim();");
        AppendJsLine_(oss, L"if(!s||s.charAt(0)===';'||s.charAt(0)==='#')return;");
        AppendJsLine_(oss, L"var m=s.match(/^\\[([^\\]]+)\\]$/);");
        AppendJsLine_(oss, L"if(m){cur=m[1];if(!sections[cur])sections[cur]={};return;}");
        AppendJsLine_(oss, L"var eq=s.indexOf('=');");
        AppendJsLine_(oss, L"if(eq<0||!cur)return;");
        AppendJsLine_(oss, L"var k=s.slice(0,eq).trim();");
        AppendJsLine_(oss, L"var v=s.slice(eq+1).trim();");
        AppendJsLine_(oss, L"sections[cur][k]=v;");
        AppendJsLine_(oss, L"});");
        AppendJsLine_(oss, L"var root=sections.AutoReserveRules||{};");
        AppendJsLine_(oss, L"var count=numFrom(root.Count,0);");
        AppendJsLine_(oss, L"var out=[];");
        AppendJsLine_(oss, L"if(count>0){");
        AppendJsLine_(oss, L"for(var i=0;i<count;i++){");
        AppendJsLine_(oss, L"var sec=sections['AutoReserveRule_'+i];");
        AppendJsLine_(oss, L"if(sec)out.push(ruleFromIniSection(sec));");
        AppendJsLine_(oss, L"}");
        AppendJsLine_(oss, L"}else{");
        AppendJsLine_(oss, L"Object.keys(sections).filter(function(k){return /^AutoReserveRule_\\d+$/.test(k);}).sort(function(a,b){return numFrom(a.replace(/\\D/g,''),0)-numFrom(b.replace(/\\D/g,''),0);}).forEach(function(k){out.push(ruleFromIniSection(sections[k]));});");
        AppendJsLine_(oss, L"}");
        AppendJsLine_(oss, L"return out.map(normalizeRule);");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"function ruleFromIniSection(s){");
        AppendJsLine_(oss, L"s=s||{};");
        AppendJsLine_(oss, L"return normalizeRule({");
        AppendJsLine_(oss, L"id:s.Id,enabled:s.Enabled,name:s.Name,keyword:s.Keyword,");
        AppendJsLine_(oss, L"excludeKeyword:s.ExcludeKeyword,titleMatchMode:s.TitleMatchMode,");
        AppendJsLine_(oss, L"matchTitle:s.MatchTitle,matchSummary:s.MatchSummary,matchExtended:s.MatchExtended,");
        AppendJsLine_(oss, L"targetSpace:s.TargetSpace,serviceName:s.ServiceName,genreName:s.GenreName,");
        AppendJsLine_(oss, L"useKeyword:s.UseKeyword,keywordJoinMode:s.KeywordJoinMode,");
        AppendJsLine_(oss, L"useExclude:s.UseExcludeKeyword,excludeJoinMode:s.ExcludeKeywordJoinMode,");
        AppendJsLine_(oss, L"useService:s.UseService,useGenre:s.UseGenre,genreJoinMode:s.GenreMatchMode,");
        AppendJsLine_(oss, L"useMinDuration:s.UseMinDuration,minDurationMinutes:s.MinDurationMinutes,");
        AppendJsLine_(oss, L"useSearchTimeRange:s.UseSearchTimeRange,searchStartMinutes:s.SearchStartMinutes,");
        AppendJsLine_(oss, L"searchEndMinutes:s.SearchEndMinutes,searchTimeWrap:s.SearchTimeWrap,");
        AppendJsLine_(oss, L"startMinutes:s.StartMinutes,endMinutes:s.EndMinutes,");
        AppendJsLine_(oss, L"useWeekday:s.UseWeekday,weekdays:s.Weekdays,priority:s.Priority,");
        AppendJsLine_(oss, L"preMarginSeconds:s.PreMarginSeconds,postMarginSeconds:s.PostMarginSeconds,");
        AppendJsLine_(oss, L"createReservationEnabled:s.CreateReservationEnabled,duplicatePolicy:s.DuplicatePolicy,");
        AppendJsLine_(oss, L"skipDuplicate:s.SkipDuplicate,excludeRerun:s.ExcludeRerunLikeProgram,");
        AppendJsLine_(oss, L"actionMode:s.ActionMode");
        AppendJsLine_(oss, L"});");
        AppendJsLine_(oss, L"}");


        AppendJsLine_(oss, L"var rulesLoadedFromCache=false;");
        AppendJsLine_(oss, L"var rules=[];");
        AppendJsLine_(oss, L"var rulesWriteInProgress=false;");
    }

    void AppendAutoReserveRuleIniRuntimeScript_(
        std::wostringstream& oss)
    {
        AppendJsLine_(oss, L"function nextId(){var m=0;rules.forEach(function(r){m=Math.max(m,Number(r.id)||0);});return m+1;}");
        AppendJsLine_(oss, L"function iniLine(k,v){return k+'='+String(def(v,''))+'\\n';}");
        AppendJsLine_(oss, L"function makeIniText(){");
        AppendJsLine_(oss, L"var out='[AutoReserveRules]\\n';");
        AppendJsLine_(oss, L"out+=iniLine('Count',rules.length);");
        AppendJsLine_(oss, L"out+=iniLine('NextId',nextId());");
        AppendJsLine_(oss, L"out+='\\n';");
        AppendJsLine_(oss, L"rules.forEach(function(r0,i){");
        AppendJsLine_(oss, L"var r=normalizeRule(r0);");
        AppendJsLine_(oss, L"out+='[AutoReserveRule_'+i+']\\n';");
        AppendJsLine_(oss, L"out+=iniLine('Id',r.id);");
        AppendJsLine_(oss, L"out+=iniLine('Enabled',r.enabled?1:0);");
        AppendJsLine_(oss, L"out+=iniLine('Name',r.name);");
        AppendJsLine_(oss, L"out+=iniLine('UseKeyword',r.useKeyword?1:0);");
        AppendJsLine_(oss, L"out+=iniLine('Keyword',r.keyword);");
        AppendJsLine_(oss, L"out+=iniLine('KeywordJoinMode',r.keywordJoinMode);");
        AppendJsLine_(oss, L"out+=iniLine('UseExcludeKeyword',r.useExclude?1:0);");
        AppendJsLine_(oss, L"out+=iniLine('ExcludeKeyword',r.excludeKeyword);");
        AppendJsLine_(oss, L"out+=iniLine('ExcludeKeywordJoinMode',r.excludeJoinMode);");
        AppendJsLine_(oss, L"out+=iniLine('TitleMatchMode',r.titleMatchMode);");
        AppendJsLine_(oss, L"out+=iniLine('MatchTitle',r.matchTitle?1:0);");
        AppendJsLine_(oss, L"out+=iniLine('MatchSummary',r.matchSummary?1:0);");
        AppendJsLine_(oss, L"out+=iniLine('MatchExtended',r.matchExtended?1:0);");
        AppendJsLine_(oss, L"out+=iniLine('TargetSpace',r.targetSpace);");
        AppendJsLine_(oss, L"out+=iniLine('UseService',r.useService?1:0);");
        AppendJsLine_(oss, L"out+=iniLine('ServiceName',r.serviceName);");
        AppendJsLine_(oss, L"out+=iniLine('ServiceMatchMode','OR');");
        AppendJsLine_(oss, L"out+=iniLine('UseGenre',r.useGenre?1:0);");
        AppendJsLine_(oss, L"out+=iniLine('GenreName',r.genreName);");
        AppendJsLine_(oss, L"out+=iniLine('GenreMatchMode',r.genreJoinMode);");
        AppendJsLine_(oss, L"out+=iniLine('UseMinDuration',r.useMinDuration?1:0);");
        AppendJsLine_(oss, L"out+=iniLine('MinDurationMinutes',r.minDurationMinutes);");
        AppendJsLine_(oss, L"out+=iniLine('UseSearchTimeRange',r.useSearchTimeRange?1:0);");
        AppendJsLine_(oss, L"out+=iniLine('SearchStartMinutes',r.searchStartMinutes);");
        AppendJsLine_(oss, L"out+=iniLine('SearchEndMinutes',r.searchEndMinutes);");
        AppendJsLine_(oss, L"out+=iniLine('SearchTimeWrap',r.searchTimeWrap?1:0);");
        AppendJsLine_(oss, L"out+=iniLine('StartMinutes',r.useSearchTimeRange?r.searchStartMinutes:-1);");
        AppendJsLine_(oss, L"out+=iniLine('EndMinutes',r.useSearchTimeRange?r.searchEndMinutes:-1);");
        AppendJsLine_(oss, L"out+=iniLine('UseWeekday',r.useWeekday?1:0);");
        AppendJsLine_(oss, L"out+=iniLine('Weekdays',r.weekdays);");
        AppendJsLine_(oss, L"out+=iniLine('Priority',r.priority);");
        AppendJsLine_(oss, L"out+=iniLine('PreMarginSeconds',r.preMarginSeconds);");
        AppendJsLine_(oss, L"out+=iniLine('PostMarginSeconds',r.postMarginSeconds);");
        AppendJsLine_(oss, L"out+=iniLine('CreateReservationEnabled',r.createReservationEnabled?1:0);");
        AppendJsLine_(oss, L"out+=iniLine('DuplicatePolicy',r.duplicatePolicy);");
        AppendJsLine_(oss, L"out+=iniLine('SkipDuplicate',r.skipDuplicate?1:0);");
        AppendJsLine_(oss, L"out+=iniLine('ExcludeRerunLikeProgram',r.excludeRerun?1:0);");
        AppendJsLine_(oss, L"out+=iniLine('ActionMode',r.actionMode);");
        AppendJsLine_(oss, L"out+='\\n';");
        AppendJsLine_(oss, L"});");
        AppendJsLine_(oss, L"return out;");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"var lastRulesLoadAt=0;");
        AppendJsLine_(oss, L"function requestRulesReload(reason){");
        AppendJsLine_(oss, L"if(rulesWriteInProgress){return Promise.resolve(false);}");
        AppendJsLine_(oss, L"var now=Date.now();");
        AppendJsLine_(oss, L"if(now-lastRulesLoadAt<800){return Promise.resolve(false);}");
        AppendJsLine_(oss, L"lastRulesLoadAt=now;");
        AppendJsLine_(oss, L"return loadRulesFromCache();");
        AppendJsLine_(oss, L"}");
        AppendJsLine_(oss, L"window.addEventListener('pageshow',function(ev){if(ev&&ev.persisted){requestRulesReload('pageshow-cache');}});");
    }

    void AppendAutoReserveRuleApiRuntimeScript_(
        std::wostringstream& oss)
    {
        AppendJsLine_(oss, L"function apiBase(){var base='';try{if(window.EPG_RUNTIME&&window.EPG_RUNTIME.reservationApiBase)base=String(window.EPG_RUNTIME.reservationApiBase);}catch(e){}try{if(!base&&window.top&&window.top!==window&&window.top.EPG_RUNTIME&&window.top.EPG_RUNTIME.reservationApiBase)base=String(window.top.EPG_RUNTIME.reservationApiBase);}catch(e){}return base;}");
        AppendJsLine_(oss, L"function apiSaveUrl(){var base=apiBase();return base?base+'/auto_reserve_rules_save':'/api/reservation/auto_reserve_rules_save';}");
        AppendJsLine_(oss, L"function rulesCacheUrl(){return '/cache/auto_reserve/auto_reserve_rules.cache';}");
        AppendJsLine_(oss, L"function setStatus(msg,err){var el=byId('status');if(!el)return;el.textContent=msg||'';el.className=err?'status error':'status';}");
        AppendJsLine_(oss, L"function refreshIniPreview(){var prev=byId('iniPreview');if(prev)prev.value=makeIniText();}");
        AppendJsLine_(oss, L"function loadRulesFromCache(){");
        AppendJsLine_(oss, L"setStatus('自動予約キャッシュを読み込み中...',false);");
        AppendJsLine_(oss, L"var url=rulesCacheUrl()+'?time='+encodeURIComponent(String(Date.now()));");
        AppendJsLine_(oss, L"return fetch(url,{method:'GET',cache:'no-store',credentials:'same-origin'}).then(function(res){return res.text().then(function(body){return {ok:res.ok,status:res.status,body:body};});}).then(function(r){");
        AppendJsLine_(oss, L"if(!r.ok)throw new Error('cache HTTP '+String(r.status)+(r.body?': '+r.body:''));");
        AppendJsLine_(oss, L"rules=parseIniRules(r.body);");
        AppendJsLine_(oss, L"rulesLoadedFromCache=true;");
        AppendJsLine_(oss, L"if(typeof render==='function')render();");
        AppendJsLine_(oss, L"refreshIniPreview();");
        AppendJsLine_(oss, L"setStatus('自動予約キャッシュを読み込みました。',false);");
        AppendJsLine_(oss, L"return true;");
        AppendJsLine_(oss, L"}).catch(function(e){");
        AppendJsLine_(oss, L"rules=[];");
        AppendJsLine_(oss, L"rulesLoadedFromCache=false;");
        AppendJsLine_(oss, L"if(typeof render==='function')render();");
        AppendJsLine_(oss, L"refreshIniPreview();");
        AppendJsLine_(oss, L"setStatus('キャッシュ読み込み失敗: '+String(e&&e.message?e.message:e)+' / 未取得状態での保存は停止します。',true);");
        AppendJsLine_(oss, L"return false;");
        AppendJsLine_(oss, L"});");
        AppendJsLine_(oss, L"}");
        AppendJsLine_(oss, L"function setRuleWriteBusy(busy){");
        AppendJsLine_(oss, L"rulesWriteInProgress=!!busy;");
        AppendJsLine_(oss, L"Array.prototype.slice.call(document.querySelectorAll('button')).forEach(function(button){button.disabled=rulesWriteInProgress;});");
        AppendJsLine_(oss, L"}");
        AppendJsLine_(oss, L"function saveRules(show){");
        AppendJsLine_(oss, L"if(!rulesLoadedFromCache){");
        AppendJsLine_(oss, L"setStatus('現在の自動予約キャッシュを読み込めていないため保存を中止しました。',true);");
        AppendJsLine_(oss, L"return Promise.resolve(false);");
        AppendJsLine_(oss, L"}");
        AppendJsLine_(oss, L"if(rulesWriteInProgress){");
        AppendJsLine_(oss, L"setStatus('保存処理中です。完了後にもう一度操作してください。',false);");
        AppendJsLine_(oss, L"return Promise.resolve(false);");
        AppendJsLine_(oss, L"}");
        AppendJsLine_(oss, L"var text=makeIniText();");
        AppendJsLine_(oss, L"refreshIniPreview();");
        AppendJsLine_(oss, L"setRuleWriteBusy(true);");
        AppendJsLine_(oss, L"setStatus('保存中...',false);");
        AppendJsLine_(oss, L"return fetch(apiSaveUrl(),{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded; charset=UTF-8'},body:encodeForm({iniText:text}),cache:'no-store'}).then(function(res){return res.text().then(function(body){return {ok:res.ok,body:body};});}).then(function(r){");
        AppendJsLine_(oss, L"var obj=null;try{obj=JSON.parse(r.body||'{}');}catch(e){}");
        AppendJsLine_(oss, L"if(!r.ok||(obj&&obj.ok===false))throw new Error((obj&&(obj.message||obj.reason))||r.body||'save failed');");
        AppendJsLine_(oss, L"setStatus('auto_reserve_rules.ini へ保存しました。',false);");
        AppendJsLine_(oss, L"if(show)alert('保存しました。');");
        AppendJsLine_(oss, L"return true;");
        AppendJsLine_(oss, L"}).catch(function(e){setStatus('保存失敗: '+String(e&&e.message?e.message:e),true);return false;}).then(function(ok){setRuleWriteBusy(false);return ok;});");
        AppendJsLine_(oss, L"}");
    }

    void AppendListScript_(
        std::wostringstream& oss)
    {
        AppendJsLine_(oss, L"<script>");
        AppendJsLine_(oss, L"(function(){");
        AppendJsLine_(oss, L"'use strict';");

        AppendAutoReserveRuleCommonRuntimeScript_(oss);
        AppendAutoReserveRuleIniRuntimeScript_(oss);
        AppendAutoReserveRuleApiRuntimeScript_(oss);

        AppendJsLine_(oss, L"function render(){");
        AppendJsLine_(oss, L"var body=byId('ruleBody');");
        AppendJsLine_(oss, L"var sum=byId('summary');");
        AppendJsLine_(oss, L"refreshIniPreview();");
        AppendJsLine_(oss, L"if(!body)return;");
        AppendJsLine_(oss, L"body.innerHTML='';");
        AppendJsLine_(oss, L"var on=0;");
        AppendJsLine_(oss, L"if(!rules.length){");
        AppendJsLine_(oss, L"body.innerHTML='<tr><td colspan=\"9\"><div class=\"empty-box\">自動予約ルールはまだありません。検索ページから自動登録ルールを追加してください。</div></td></tr>';");
        AppendJsLine_(oss, L"if(sum)sum.textContent='登録 0 件 / '+(rulesLoadedFromCache?'キャッシュ読込済み':'未読込');");
        AppendJsLine_(oss, L"return;");
        AppendJsLine_(oss, L"}");
        AppendJsLine_(oss, L"rules.forEach(function(r0){");
        AppendJsLine_(oss, L"var r=normalizeRule(r0);");
        AppendJsLine_(oss, L"if(r.enabled)on++;");
        AppendJsLine_(oss, L"var tr=document.createElement('tr');");
        AppendJsLine_(oss, L"tr.className=r.enabled?'':'disabled-row';");
        AppendJsLine_(oss, L"var title=r.name||r.keyword||'(無題)';");
        AppendJsLine_(oss, L"tr.innerHTML='<td><span class=\"badge '+(r.enabled?'on':'off')+'\">'+(r.enabled?'有効':'無効')+'</span></td>'+'<td>#'+esc(r.id)+'</td>'+'<td><a href=\"'+esc(window.EPG_LITE_AUTO_RESERVE_EDIT_FILE)+'?id='+encodeURIComponent(String(r.id))+'\">'+esc(title)+'</a></td>'+'<td>'+esc(r.keyword||'-')+'</td>'+'<td>'+esc(r.serviceName||'すべて')+'</td>'+'<td>'+esc(r.genreName||'すべて')+'</td>'+'<td>'+esc(r.useMinDuration?String(r.minDurationMinutes)+'分以上':'なし')+'</td>'+'<td>'+esc(r.actionMode==='auto'?'自動登録':'候補')+'</td>'+'<td><div class=\"actions\"><button type=\"button\" data-toggle=\"'+esc(r.id)+'\">'+(r.enabled?'無効化':'有効化')+'</button><a class=\"btn\" href=\"'+esc(window.EPG_LITE_AUTO_RESERVE_EDIT_FILE)+'?id='+encodeURIComponent(String(r.id))+'\">変更</a><button type=\"button\" class=\"danger\" data-remove=\"'+esc(r.id)+'\">削除</button></div></td>';");
        AppendJsLine_(oss, L"body.appendChild(tr);");
        AppendJsLine_(oss, L"});");
        AppendJsLine_(oss, L"if(sum)sum.textContent='登録 '+rules.length+' 件 / 有効 '+on+' 件 / '+(rulesLoadedFromCache?'キャッシュ読込済み':'未読込');");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"document.addEventListener('click',function(ev){");
        AppendJsLine_(oss, L"var t=ev.target;");
        AppendJsLine_(oss, L"if(!t||!t.closest)return;");
        AppendJsLine_(oss, L"var tog=t.closest('[data-toggle]');");
        AppendJsLine_(oss, L"if(tog){");
        AppendJsLine_(oss, L"ev.preventDefault();");
        AppendJsLine_(oss, L"var id=Number(tog.getAttribute('data-toggle')||0);");
        AppendJsLine_(oss, L"var idx=rules.findIndex(function(r){return Number(r.id)===id;});");
        AppendJsLine_(oss, L"if(idx>=0){");
        AppendJsLine_(oss, L"var before=rules.slice();");
        AppendJsLine_(oss, L"rules[idx]=normalizeRule(Object.assign({},rules[idx],{enabled:!rules[idx].enabled}));");
        AppendJsLine_(oss, L"render();");
        AppendJsLine_(oss, L"saveRules(false).then(function(ok){if(!ok){rules=before;render();}});");
        AppendJsLine_(oss, L"}");
        AppendJsLine_(oss, L"return;");
        AppendJsLine_(oss, L"}");
        AppendJsLine_(oss, L"var rem=t.closest('[data-remove]');");
        AppendJsLine_(oss, L"if(rem){");
        AppendJsLine_(oss, L"ev.preventDefault();");
        AppendJsLine_(oss, L"var rid=Number(rem.getAttribute('data-remove')||0);");
        AppendJsLine_(oss, L"if(!confirm('この自動予約ルールを削除しますか？'))return;");
        AppendJsLine_(oss, L"var before=rules.slice();");
        AppendJsLine_(oss, L"rules=rules.filter(function(r){return Number(r.id)!==rid;});");
        AppendJsLine_(oss, L"render();");
        AppendJsLine_(oss, L"saveRules(false).then(function(ok){if(!ok){rules=before;render();}});");
        AppendJsLine_(oss, L"return;");
        AppendJsLine_(oss, L"}");
        AppendJsLine_(oss, L"});");

        AppendJsLine_(oss, L"document.addEventListener('DOMContentLoaded',function(){render();requestRulesReload('DOMContentLoaded');});");
        AppendJsLine_(oss, L"})();");
        AppendJsLine_(oss, L"</script>");
    }

    void AppendEditScript_(
        std::wostringstream& oss)
    {
        AppendJsLine_(oss, L"<script>");
        AppendJsLine_(oss, L"(function(){");
        AppendJsLine_(oss, L"'use strict';");

        AppendAutoReserveRuleCommonRuntimeScript_(oss);
        AppendAutoReserveRuleIniRuntimeScript_(oss);
        AppendAutoReserveRuleApiRuntimeScript_(oss);

        AppendJsLine_(oss, L"function n(id,d){var el=byId(id);var v=el?Number(el.value):Number.NaN;return Number.isFinite(v)?Math.trunc(v):d;}");
        AppendJsLine_(oss, L"function b(id){var el=byId(id);return !!(el&&el.checked);}");
        AppendJsLine_(oss, L"function s(id){var el=byId(id);return String(el?el.value:'').trim();}");
        AppendJsLine_(oss, L"function setVal(id,v){var el=byId(id);if(el)el.value=String(def(v,''));}");
        AppendJsLine_(oss, L"function setChecked(id,v){var el=byId(id);if(el)el.checked=!!v;}");
        AppendJsLine_(oss, L"function getMulti(id){var el=byId(id);if(!el)return '';var out=[];Array.prototype.slice.call(el.options||[]).forEach(function(o){if(o.selected&&o.value)out.push(o.value);});return joinValues(out);}");

        AppendJsLine_(oss, L"function setMulti(id,text){");
        AppendJsLine_(oss, L"var el=byId(id);");
        AppendJsLine_(oss, L"if(!el)return;");
        AppendJsLine_(oss, L"var values=splitValues(text);");
        AppendJsLine_(oss, L"values.forEach(function(v){");
        AppendJsLine_(oss, L"var exists=false;");
        AppendJsLine_(oss, L"Array.prototype.slice.call(el.options||[]).forEach(function(o){if(o.value===v)exists=true;});");
        AppendJsLine_(oss, L"if(!exists){var opt=document.createElement('option');opt.value=v;opt.textContent=v;el.appendChild(opt);}");
        AppendJsLine_(oss, L"});");
        AppendJsLine_(oss, L"Array.prototype.slice.call(el.options||[]).forEach(function(o){o.selected=values.indexOf(o.value)>=0;});");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"function queryAssist(){");
        AppendJsLine_(oss, L"try{");
        AppendJsLine_(oss, L"var p=new URLSearchParams(location.search);");
        AppendJsLine_(oss, L"var id=Number(p.get('id')||0);");
        AppendJsLine_(oss, L"var mode=String(p.get('mode')||'').toLowerCase().trim();");
        AppendJsLine_(oss, L"var keyword=String(p.get('keyword')||'').trim().slice(0,512);");
        AppendJsLine_(oss, L"return {id:Number.isFinite(id)?Math.trunc(id):0,isNew:mode==='new',keyword:keyword};");
        AppendJsLine_(oss, L"}catch(e){return {id:0,isNew:false,keyword:''};}");
        AppendJsLine_(oss, L"}");
        AppendJsLine_(oss, L"var assist=queryAssist();");
        AppendJsLine_(oss, L"var currentId=assist.isNew?0:assist.id;");
        AppendJsLine_(oss, L"var newDraft=null;");
        AppendJsLine_(oss, L"function currentTargetSpace(){var n=Number(def(window.EPG_LITE_AUTO_RESERVE_TARGET_SPACE,0));return Number.isFinite(n)?Math.trunc(n):0;}");
        AppendJsLine_(oss, L"function findIndex(id){id=Number(id)||0;return rules.findIndex(function(r){return Number(r.id)===id;});}");
        AppendJsLine_(oss, L"function nextRuleId(){var maxId=0;rules.forEach(function(r){var id=Number(r&&r.id)||0;if(id>maxId)maxId=Math.trunc(id);});return maxId+1;}");
        AppendJsLine_(oss, L"function prepareNewDraft(){");
        AppendJsLine_(oss, L"currentId=nextRuleId();");
        AppendJsLine_(oss, L"newDraft=normalizeRule({id:currentId,enabled:true,name:assist.keyword,keyword:assist.keyword,titleMatchMode:0,matchTitle:true,matchSummary:false,matchExtended:false,targetSpace:currentTargetSpace(),useKeyword:assist.keyword.length>0,createReservationEnabled:true,skipDuplicate:true,actionMode:'auto'});");
        AppendJsLine_(oss, L"return newDraft;");
        AppendJsLine_(oss, L"}");
        AppendJsLine_(oss, L"function currentRuleOrDefault(){if(assist.isNew)return newDraft||prepareNewDraft();var idx=findIndex(currentId);return normalizeRule(idx>=0?rules[idx]:{});}");

        AppendJsLine_(oss, L"function updateUi(){");
        AppendJsLine_(oss, L"var useService=b('useService');");
        AppendJsLine_(oss, L"var useGenre=b('useGenre');");
        AppendJsLine_(oss, L"var useMin=b('useMinDuration');");
        AppendJsLine_(oss, L"var useTime=b('useSearchTimeRange');");
        AppendJsLine_(oss, L"['serviceSelect'].forEach(function(id){var el=byId(id);if(el)el.disabled=!useService;});");
        AppendJsLine_(oss, L"['genreSelect','genreJoinMode'].forEach(function(id){var el=byId(id);if(el)el.disabled=!useGenre;});");
        AppendJsLine_(oss, L"var md=byId('minDurationMinutes');");
        AppendJsLine_(oss, L"if(md)md.disabled=!useMin;");
        AppendJsLine_(oss, L"['searchStartMinutes','searchEndMinutes','searchTimeWrap'].forEach(function(id){var el=byId(id);if(el)el.disabled=!useTime;});");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"function buildIniPreview(){refreshIniPreview();return makeIniText();}");

        AppendJsLine_(oss, L"function writeForm(r0){");
        AppendJsLine_(oss, L"var r=normalizeRule(r0);");
        AppendJsLine_(oss, L"setVal('editId',r.id);");
        AppendJsLine_(oss, L"setChecked('enabled',r.enabled);");
        AppendJsLine_(oss, L"setVal('ruleName',r.name);");
        AppendJsLine_(oss, L"setVal('keyword',r.keyword);");
        AppendJsLine_(oss, L"setVal('titleMatchMode',r.titleMatchMode);");
        AppendJsLine_(oss, L"setChecked('matchTitle',r.matchTitle);");
        AppendJsLine_(oss, L"setChecked('matchSummary',r.matchSummary);");
        AppendJsLine_(oss, L"setChecked('matchExtended',r.matchExtended);");
        AppendJsLine_(oss, L"setChecked('useService',r.useService);");
        AppendJsLine_(oss, L"setMulti('serviceSelect',r.serviceName);");
        AppendJsLine_(oss, L"setChecked('useGenre',r.useGenre);");
        AppendJsLine_(oss, L"setMulti('genreSelect',r.genreName);");
        AppendJsLine_(oss, L"setVal('genreJoinMode',r.genreJoinMode);");
        AppendJsLine_(oss, L"setChecked('useMinDuration',r.useMinDuration);");
        AppendJsLine_(oss, L"setVal('minDurationMinutes',r.minDurationMinutes);");
        AppendJsLine_(oss, L"setVal('duplicatePolicy',r.duplicatePolicy);");
        AppendJsLine_(oss, L"setChecked('skipDuplicate',r.skipDuplicate);");
        AppendJsLine_(oss, L"setChecked('excludeRerun',r.excludeRerun);");
        AppendJsLine_(oss, L"setChecked('useSearchTimeRange',r.useSearchTimeRange);");
        AppendJsLine_(oss, L"setVal('searchStartMinutes',r.searchStartMinutes);");
        AppendJsLine_(oss, L"setVal('searchEndMinutes',r.searchEndMinutes);");
        AppendJsLine_(oss, L"setChecked('searchTimeWrap',r.searchTimeWrap);");
        AppendJsLine_(oss, L"updateUi();");
        AppendJsLine_(oss, L"buildIniPreview();");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"function readForm(){");
        AppendJsLine_(oss, L"var base=currentRuleOrDefault();");
        AppendJsLine_(oss, L"var next=Object.assign({},base);");
        AppendJsLine_(oss, L"next.id=Number(currentId)||0;");
        AppendJsLine_(oss, L"next.enabled=b('enabled');");
        AppendJsLine_(oss, L"next.name=s('ruleName');");
        AppendJsLine_(oss, L"next.keyword=s('keyword');");
        AppendJsLine_(oss, L"next.titleMatchMode=n('titleMatchMode',0);");
        AppendJsLine_(oss, L"next.matchTitle=b('matchTitle');");
        AppendJsLine_(oss, L"next.matchSummary=b('matchSummary');");
        AppendJsLine_(oss, L"next.matchExtended=b('matchExtended');");
        AppendJsLine_(oss, L"next.targetSpace=currentTargetSpace();");
        AppendJsLine_(oss, L"next.useKeyword=next.keyword.length>0;");
        AppendJsLine_(oss, L"next.keywordJoinMode=textFrom(base.keywordJoinMode,'OR');");
        AppendJsLine_(oss, L"next.useExclude=boolFrom(base.useExclude,false);");
        AppendJsLine_(oss, L"next.excludeKeyword=textFrom(base.excludeKeyword,'');");
        AppendJsLine_(oss, L"next.excludeJoinMode=textFrom(base.excludeJoinMode,'OR');");
        AppendJsLine_(oss, L"next.useService=b('useService');");
        AppendJsLine_(oss, L"next.serviceName=next.useService?getMulti('serviceSelect'):'';");
        AppendJsLine_(oss, L"next.useGenre=b('useGenre');");
        AppendJsLine_(oss, L"next.genreName=next.useGenre?getMulti('genreSelect'):'';");
        AppendJsLine_(oss, L"next.genreJoinMode=s('genreJoinMode')||textFrom(base.genreJoinMode,'OR');");
        AppendJsLine_(oss, L"next.useMinDuration=b('useMinDuration');");
        AppendJsLine_(oss, L"next.minDurationMinutes=n('minDurationMinutes',0);");
        AppendJsLine_(oss, L"next.useSearchTimeRange=b('useSearchTimeRange');");
        AppendJsLine_(oss, L"next.searchStartMinutes=n('searchStartMinutes',0);");
        AppendJsLine_(oss, L"next.searchEndMinutes=n('searchEndMinutes',1440);");
        AppendJsLine_(oss, L"next.searchTimeWrap=b('searchTimeWrap');");
        AppendJsLine_(oss, L"next.useWeekday=boolFrom(base.useWeekday,false);");
        AppendJsLine_(oss, L"next.weekdays=textFrom(base.weekdays,'1111111');");
        AppendJsLine_(oss, L"next.priority=numFrom(base.priority,50);");
        AppendJsLine_(oss, L"next.preMarginSeconds=numFrom(base.preMarginSeconds,0);");
        AppendJsLine_(oss, L"next.postMarginSeconds=numFrom(base.postMarginSeconds,0);");
        AppendJsLine_(oss, L"next.createReservationEnabled=boolFrom(base.createReservationEnabled,true);");
        AppendJsLine_(oss, L"next.duplicatePolicy=n('duplicatePolicy',0);");
        AppendJsLine_(oss, L"next.skipDuplicate=b('skipDuplicate');");
        AppendJsLine_(oss, L"next.excludeRerun=b('excludeRerun');");
        AppendJsLine_(oss, L"next.actionMode=normalizeActionMode(base.actionMode);");
        AppendJsLine_(oss, L"return normalizeRule(next);");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"function openCurrentRule(){");
        AppendJsLine_(oss, L"if(assist.isNew){writeForm(prepareNewDraft());setStatus(assist.keyword?'番組名を入力しました。条件を確認して登録してください。':'新しい自動予約ルールを入力してください。',false);return;}");
        AppendJsLine_(oss, L"var idx=findIndex(currentId);");
        AppendJsLine_(oss, L"if(idx<0){setStatus('対象ルールが見つかりません。一覧から変更を開いてください。',true);refreshIniPreview();return;}");
        AppendJsLine_(oss, L"writeForm(rules[idx]);");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"window.saveCurrentRule=function(){");
        AppendJsLine_(oss, L"var idx=findIndex(currentId);");
        AppendJsLine_(oss, L"if(!assist.isNew&&idx<0){alert('対象ルールが見つかりません。');return;}");
        AppendJsLine_(oss, L"var before=rules.slice();");
        AppendJsLine_(oss, L"var r=readForm();");
        AppendJsLine_(oss, L"if(!r.keyword&&!r.useService&&!r.useGenre){alert('キーワード、放送局、ジャンルのいずれかを指定してください。');return;}");
        AppendJsLine_(oss, L"if(!r.name)r.name=r.keyword||r.genreName||r.serviceName||'自動予約ルール';");
        AppendJsLine_(oss, L"if(assist.isNew){r.id=currentId;rules.push(normalizeRule(r));}else{rules[idx]=r;}");
        AppendJsLine_(oss, L"buildIniPreview();");
        AppendJsLine_(oss, L"saveRules(true).then(function(ok){if(ok){location.href=window.EPG_LITE_AUTO_RESERVE_LIST_FILE;}else{rules=before;writeForm(r);}});");
        AppendJsLine_(oss, L"};");

        AppendJsLine_(oss, L"window.deleteCurrentRule=function(){");
        AppendJsLine_(oss, L"if(assist.isNew)return;");
        AppendJsLine_(oss, L"var idx=findIndex(currentId);");
        AppendJsLine_(oss, L"if(idx<0){alert('対象ルールが見つかりません。');return;}");
        AppendJsLine_(oss, L"if(!confirm('この自動予約ルールを削除しますか？'))return;");
        AppendJsLine_(oss, L"var before=rules.slice();");
        AppendJsLine_(oss, L"rules=rules.filter(function(r){return Number(r.id)!==Number(currentId);});");
        AppendJsLine_(oss, L"buildIniPreview();");
        AppendJsLine_(oss, L"saveRules(true).then(function(ok){if(ok){location.href=window.EPG_LITE_AUTO_RESERVE_LIST_FILE;}else{rules=before;openCurrentRule();}});");
        AppendJsLine_(oss, L"};");

        AppendJsLine_(oss, L"document.addEventListener('input',function(){updateUi();buildIniPreview();});");
        AppendJsLine_(oss, L"document.addEventListener('change',function(){updateUi();buildIniPreview();});");
        AppendJsLine_(oss, L"document.addEventListener('DOMContentLoaded',function(){");
        AppendJsLine_(oss, L"var deleteButton=byId('deleteRuleButton');");
        AppendJsLine_(oss, L"var saveButton=byId('saveRuleButton');");
        AppendJsLine_(oss, L"var pageHeading=byId('editPageHeading');");
        AppendJsLine_(oss, L"var formHeading=byId('editFormHeading');");
        AppendJsLine_(oss, L"if(assist.isNew){if(deleteButton)deleteButton.hidden=true;if(saveButton)saveButton.textContent='新規登録';if(pageHeading)pageHeading.textContent='自動検索予約 登録';if(formHeading)formHeading.textContent='新しい登録情報';}");
        AppendJsLine_(oss, L"writeForm(currentRuleOrDefault());");
        AppendJsLine_(oss, L"requestRulesReload('DOMContentLoaded').then(function(){openCurrentRule();});");
        AppendJsLine_(oss, L"});");

        AppendJsLine_(oss, L"})();");
        AppendJsLine_(oss, L"</script>");
    }

    std::wstring BuildListHtml_(
        const Context& ctx,
        const std::filesystem::path& iniPath)
    {
        const std::wstring listFile =
            ListFileNameFor_(ctx.space);

        const std::wstring editFile =
            EditFileNameFor_(ctx.space);

        std::wostringstream oss;

        oss << L"<!doctype html>\n";
        oss << L"<html lang=\"ja\">\n";
        oss << L"<head>\n";
        oss << L"<meta charset=\"utf-8\">\n";
        oss << L"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n";
        oss << L"<title>自動検索予約リスト</title>\n";
        oss << L"<style>\n";
        AppendCommonCss_(oss);
        oss << L"</style>\n";
        oss << L"</head>\n";
        oss << L"<body>\n";
        oss << L"<main>\n";

        oss << L"<section class=\"panel\">\n";
        oss << L"<h1>自動検索予約リスト</h1>\n";
        oss << L"<div class=\"note\">Lite / "
            << HtmlEscape_(MakeSpaceLabel_(ctx.space))
            << L" / キャッシュ読込 / INI: "
            << HtmlEscape_(iniPath.wstring())
            << L"</div>\n";

        oss << L"<div class=\"actions\" style=\"margin-top:10px;\">";
        oss << L"<a class=\"btn\" href=\""
            << HtmlEscape_(editFile)
            << L"?mode=new\">新しいルールを登録</a>";
        oss << L"</div>\n";

        oss << L"<div id=\"status\" class=\"status\"></div>\n";
        oss << L"</section>\n";

        oss << L"<section class=\"panel\">\n";
        oss << L"<h2>登録済み自動予約ルール</h2>\n";
        oss << L"<div id=\"summary\" class=\"note\">auto_reserve_rules.ini を読み込み中です。</div>\n";
        oss << L"<table class=\"rule-table\">\n";
        oss << L"<thead><tr>";
        oss << L"<th>状態</th>";
        oss << L"<th>ID</th>";
        oss << L"<th>ルール名</th>";
        oss << L"<th>キーワード</th>";
        oss << L"<th>放送局</th>";
        oss << L"<th>ジャンル</th>";
        oss << L"<th>最小時間</th>";
        oss << L"<th>動作</th>";
        oss << L"<th>操作</th>";
        oss << L"</tr></thead>\n";
        oss << L"<tbody id=\"ruleBody\"></tbody>\n";
        oss << L"</table>\n";
        oss << L"</section>\n";

        oss << L"<section class=\"panel\">\n";
        oss << L"<h2>INIプレビュー</h2>\n";
        oss << L"<div class=\"note\">外部キャッシュから読み込んだ最新データだけを保存対象にします。読み込み失敗時は保存しません。</div>\n";
        oss << L"<textarea id=\"iniPreview\" readonly></textarea>\n";
        oss << L"</section>\n";

        oss << L"<script>\n";
        oss << L"window.EPG_LITE_AUTO_RESERVE_LIST_FILE="
            << JsString_(listFile)
            << L";\n";
        oss << L"window.EPG_LITE_AUTO_RESERVE_EDIT_FILE="
            << JsString_(editFile)
            << L";\n";
        oss << L"window.EPG_LITE_AUTO_RESERVE_TARGET_SPACE="
            << ctx.space
            << L";\n";
        oss << L"</script>\n";

        AppendListScript_(oss);

        oss << L"</main>\n";
        oss << L"</body>\n";
        oss << L"</html>\n";

        return oss.str();
    }

    std::wstring BuildEditHtml_(
        const Context& ctx,
        const std::filesystem::path& iniPath)
    {
        const std::wstring listFile =
            ListFileNameFor_(ctx.space);

        std::wostringstream oss;

        oss << L"<!doctype html>\n";
        oss << L"<html lang=\"ja\">\n";
        oss << L"<head>\n";
        oss << L"<meta charset=\"utf-8\">\n";
        oss << L"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n";
        oss << L"<title>自動検索予約 登録・変更</title>\n";
        oss << L"<style>\n";
        AppendCommonCss_(oss);
        oss << L"</style>\n";
        oss << L"</head>\n";
        oss << L"<body>\n";
        oss << L"<main>\n";

        oss << L"<section class=\"panel\">\n";
        oss << L"<h1 id=\"editPageHeading\">自動検索予約 変更</h1>\n";
        oss << L"<div class=\"note\">Lite / "
            << HtmlEscape_(MakeSpaceLabel_(ctx.space))
            << L" / キャッシュ読込 / INI: "
            << HtmlEscape_(iniPath.wstring())
            << L"</div>\n";
        oss << L"<div class=\"actions\" style=\"margin-top:10px;\">";
        oss << L"<a class=\"btn\" href=\""
            << HtmlEscape_(listFile)
            << L"\">一覧へ戻る</a>";
        oss << L"</div>\n";
        oss << L"<div id=\"status\" class=\"status\"></div>\n";
        oss << L"</section>\n";

        oss << L"<section class=\"panel\">\n";
        oss << L"<h2 id=\"editFormHeading\">登録情報の変更</h2>\n";
        oss << L"<input id=\"editId\" type=\"hidden\" value=\"0\">\n";
        oss << L"<div class=\"form-grid\">\n";

        oss << L"<label class=\"field\"><span>状態</span><span class=\"check-row\"><label><input id=\"enabled\" type=\"checkbox\">有効</label></span></label>\n";

        oss << L"<div class=\"field\"><span>予約動作</span>";
        oss << L"<input type=\"text\" value=\"録画予約を自動登録\" readonly>";
        oss << L"<span class=\"mini\">Lite版の自動予約は録画予約専用です。</span>";
        oss << L"</div>\n";

        oss << L"<label class=\"field wide\"><span>ルール名</span><input id=\"ruleName\" type=\"text\"></label>\n";
        oss << L"<label class=\"field wide\"><span>キーワード</span><input id=\"keyword\" type=\"text\" placeholder=\"例: プリキュア\"></label>\n";

        oss << L"<label class=\"field\"><span>一致方式</span><select id=\"titleMatchMode\">";
        AppendOption_(oss, L"0", L"含む");
        AppendOption_(oss, L"1", L"完全一致");
        AppendOption_(oss, L"2", L"前方一致");
        AppendOption_(oss, L"3", L"後方一致");
        oss << L"</select></label>\n";

        oss << L"<label class=\"field\"><span>重複処理</span><select id=\"duplicatePolicy\">";
        AppendOption_(oss, L"0", L"同一イベントを除外");
        AppendOption_(oss, L"1", L"同一タイトル/時刻を除外");
        AppendOption_(oss, L"2", L"重複許可");
        oss << L"</select></label>\n";

        oss << L"<div class=\"field wide\"><span>検索対象</span><div class=\"check-row\">";
        oss << L"<label><input id=\"matchTitle\" type=\"checkbox\">タイトル</label>";
        oss << L"<label><input id=\"matchSummary\" type=\"checkbox\">概要</label>";
        oss << L"<label><input id=\"matchExtended\" type=\"checkbox\">詳細説明</label>";
        oss << L"</div></div>\n";

        oss << L"<div class=\"field\"><span>放送局</span>";
        oss << L"<div class=\"check-row\"><label><input id=\"useService\" type=\"checkbox\">放送局指定を使う</label></div>";
        oss << L"<select id=\"serviceSelect\" multiple>";
        AppendServiceOptionsForEdit_(
            oss,
            ctx,
            std::vector<AutoReserve::Rule>{});
        oss << L"</select>";
        oss << L"<span class=\"mini\">config.ini の有効放送局を表示します。保存済みルールの局名はキャッシュ読込後に必要なら追加されます。</span>";
        oss << L"</div>\n";

        oss << L"<div class=\"field\"><span>ジャンル</span>";
        oss << L"<div class=\"check-row\"><label><input id=\"useGenre\" type=\"checkbox\">ジャンル指定を使う</label></div>";
        oss << L"<select id=\"genreSelect\" multiple>";
        AppendGenreOptions_(oss);
        oss << L"</select>";
        oss << L"<select id=\"genreJoinMode\">";
        AppendOption_(oss, L"OR", L"いずれかに一致");
        AppendOption_(oss, L"AND", L"すべてに一致");
        oss << L"</select>";
        oss << L"</div>\n";

        oss << L"<label class=\"field\"><span>最小放送時間</span>";
        oss << L"<span class=\"check-row\"><label><input id=\"useMinDuration\" type=\"checkbox\">短い番組を除外</label></span>";
        oss << L"<input id=\"minDurationMinutes\" type=\"number\" min=\"0\" max=\"1440\" value=\"0\">";
        oss << L"</label>\n";

        oss << L"<div class=\"field\"><span>時間帯</span>";
        oss << L"<span class=\"check-row\"><label><input id=\"useSearchTimeRange\" type=\"checkbox\">時間帯指定を使う</label><label><input id=\"searchTimeWrap\" type=\"checkbox\">日付またぎ</label></span>";
        oss << L"<select id=\"searchStartMinutes\">";
        AppendTimeOptions_(oss, 0, 1410, 30);
        oss << L"</select>";
        oss << L"<select id=\"searchEndMinutes\">";
        AppendTimeOptions_(oss, 30, 1440, 30);
        oss << L"</select>";
        oss << L"</div>\n";

        oss << L"<div class=\"field wide\"><span>安全設定</span><div class=\"check-row\">";
        oss << L"<label><input id=\"skipDuplicate\" type=\"checkbox\">既存予約と重複した番組は登録しない</label>";
        oss << L"<label><input id=\"excludeRerun\" type=\"checkbox\">再放送らしき番組を除外</label>";
        oss << L"</div></div>\n";

        oss << L"</div>\n";

        oss << L"<div class=\"actions\" style=\"margin-top:12px;\">";
        oss << L"<button id=\"saveRuleButton\" type=\"button\" onclick=\"saveCurrentRule()\">変更を保存</button>";
        oss << L"<button id=\"deleteRuleButton\" type=\"button\" class=\"danger\" onclick=\"deleteCurrentRule()\">このルールを削除</button>";
        oss << L"</div>\n";

        oss << L"</section>\n";

        oss << L"<section class=\"panel\">\n";
        oss << L"<h2>INIプレビュー</h2>\n";
        oss << L"<textarea id=\"iniPreview\" readonly></textarea>\n";
        oss << L"</section>\n";

        oss << L"<script>\n";
        oss << L"window.EPG_LITE_AUTO_RESERVE_LIST_FILE="
            << JsString_(listFile)
            << L";\n";
        oss << L"window.EPG_LITE_AUTO_RESERVE_TARGET_SPACE="
            << ctx.space
            << L";\n";
        oss << L"</script>\n";

        AppendEditScript_(oss);

        oss << L"</main>\n";
        oss << L"</body>\n";
        oss << L"</html>\n";

        return oss.str();
    }

}

namespace EpgHtmlLite::PageAutoKeywordReservationList
{
    bool Generate(
        const Context& ctx,
        Result& ioResult)
    {
        try {
            const std::filesystem::path listPath =
                ctx.outputDir /
                std::filesystem::path(ListFileNameFor_(ctx.space));

            const std::filesystem::path editPath =
                ctx.outputDir /
                std::filesystem::path(EditFileNameFor_(ctx.space));

            const std::filesystem::path rulesIniPath =
                AutoReserve::GetDefaultRulesIniPath();

            // INIが無い初回だけ作成する。
            // ただしHTMLにはルール内容を埋め込まない。
            AutoReserve::RuleStore store;

            const auto load =
                store.LoadFromIni(
                    rulesIniPath.wstring());

            if (!load.ok) {
                const auto initSave =
                    store.SaveToIni(
                        rulesIniPath.wstring());

                (void)initSave;
            }

            if (!WriteUtf8BomTextFile_(
                listPath,
                BuildListHtml_(
                    ctx,
                    rulesIniPath))) {
                return false;
            }

            if (!WriteUtf8BomTextFile_(
                editPath,
                BuildEditHtml_(
                    ctx,
                    rulesIniPath))) {
                return false;
            }

            ioResult.generatedFiles.push_back(
                listPath.wstring());

            ioResult.generatedFiles.push_back(
                editPath.wstring());

            return true;
        }
        catch (...) {
            return false;
        }
    }
}
