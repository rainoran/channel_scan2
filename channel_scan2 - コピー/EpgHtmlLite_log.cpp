#include "EpgHtmlLite_log.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <Windows.h>
#endif

#include <climits>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "EpgHtmlLite_Common.hpp"

namespace
{
    using namespace EpgHtmlLite;

    constexpr const wchar_t* kLogScriptFile =
        L"epg_lite_log.js";

    constexpr int kInitialLineLimit = 300;
    constexpr int kPollingIntervalMs = 5000;
    constexpr int kClientLineLimit = 2000;

    std::wstring HtmlEscape_(
        const std::wstring_view src)
    {
        std::wstring out;
        out.reserve(src.size() + 16);

        for (const wchar_t ch : src) {
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

    bool WriteUtf8BomTextFile_(
        const std::filesystem::path& path,
        const std::wstring& content)
    {
        std::error_code ec;
        const std::filesystem::path parent =
            path.parent_path();

        if (!parent.empty()) {
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

        constexpr unsigned char bom[3] = {
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
                static_cast<std::size_t>(INT_MAX)) {
                return false;
            }

            const int sourceLength =
                static_cast<int>(content.size());

            const int required =
                ::WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    content.data(),
                    sourceLength,
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
                    sourceLength,
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
        for (const wchar_t ch : content) {
            if (ch <= 0x7F) {
                ofs.put(static_cast<char>(ch));
            }
            else {
                ofs.put('?');
            }
        }
#endif

        ofs.flush();
        return ofs.good();
    }

    std::wstring MakeSpaceLabel_(
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

    std::wstring BuildLogPageFileName_(
        const int space)
    {
        return L"epg_lite_log_space" +
            std::to_wstring(space) +
            L".html";
    }

    void AppendLogPageCss_(
        std::wostringstream& oss)
    {
        oss << L".system-log-page{display:flex;flex-direction:column;gap:10px;min-height:calc(100vh - 70px);}\n";
        oss << L".system-log-toolbar{display:flex;flex-wrap:wrap;align-items:center;gap:8px;padding:10px;background:var(--lite-panel2);border:1px solid var(--lite-line);border-radius:10px;}\n";
        oss << L".system-log-toolbar button,.system-log-toolbar select,.system-log-toolbar input{min-height:34px;border:1px solid var(--lite-line);border-radius:7px;background:#111722;color:var(--lite-text);font:inherit;}\n";
        oss << L".system-log-toolbar button{padding:6px 12px;cursor:pointer;font-weight:700;}\n";
        oss << L".system-log-toolbar button:hover{border-color:#6384b8;}\n";
        oss << L".system-log-toolbar button.active{background:#244a73;border-color:#4d8fd1;}\n";
        oss << L".system-log-toolbar select{padding:6px 30px 6px 9px;}\n";
        oss << L".system-log-search{flex:1 1 220px;padding:6px 10px;}\n";
        oss << L".system-log-status{display:flex;flex-wrap:wrap;justify-content:space-between;gap:8px;padding:0 3px;color:var(--lite-sub);font-size:12px;}\n";
        oss << L".system-log-status.error{color:#ff8989;}\n";
        oss << L".system-log-view{flex:1 1 auto;min-height:360px;max-height:calc(100vh - 205px);overflow:auto;background:#090c11;border:1px solid var(--lite-line);border-radius:10px;padding:8px 0;font-family:Consolas,'Yu Gothic UI',monospace;font-size:12px;line-height:1.55;}\n";
        oss << L".system-log-line{padding:2px 10px;white-space:pre-wrap;overflow-wrap:anywhere;border-left:3px solid transparent;}\n";
        oss << L".system-log-line:hover{background:rgba(255,255,255,0.045);}\n";
        oss << L".system-log-line.level-info{color:#c8d3e1;border-left-color:#4b84bd;}\n";
        oss << L".system-log-line.level-warn{color:#ffd889;background:rgba(255,185,55,0.055);border-left-color:#e2a62f;}\n";
        oss << L".system-log-line.level-error{color:#ff9c9c;background:rgba(255,75,75,0.075);border-left-color:#e35050;}\n";
        oss << L".system-log-empty{padding:32px 12px;text-align:center;color:var(--lite-sub);}\n";
        oss << L"@media(max-width:720px){.system-log-page{min-height:calc(100vh - 40px);}.system-log-view{max-height:calc(100vh - 245px);}.system-log-toolbar>*{flex:1 1 auto;}}\n";
    }

    std::wstring BuildLogPageHtml_(
        const Context& ctx)
    {
        Common::HeaderContext header{};
        header.title = L"システムログ";
        header.subtitle =
            L"Lite System Log / " +
            MakeSpaceLabel_(ctx.space) +
            L" / 随時読み込み";
        header.mainLinks =
            Common::BuildMainMenuLinks(
                ctx.space,
                MenuId::Log);
        header.showMainMenuRow = false;
        header.showGuideControlRow = false;
        header.showGenreRow = false;

        std::wostringstream oss;

        oss << L"<!DOCTYPE html>\n";
        oss << L"<html lang=\"ja\">\n";
        oss << L"<head>\n";
        oss << L"<meta charset=\"UTF-8\">\n";
        oss << L"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">\n";
        oss << L"<title>システムログ</title>\n";
        oss << L"<style>\n";
        oss << Common::BuildCommonCss();
        AppendLogPageCss_(oss);
        oss << L"</style>\n";
        oss << L"<script src=\"";
        oss << HtmlEscape_(kLogScriptFile);
        oss << L"\" defer></script>\n";
        oss << L"</head>\n";
        oss << L"<body>\n";
        oss << Common::BuildLayoutBegin(header);

        oss << L"<div class=\"system-log-page\" id=\"systemLogPage\"";
        oss << L" data-initial-limit=\"";
        oss << kInitialLineLimit;
        oss << L"\" data-poll-ms=\"";
        oss << kPollingIntervalMs;
        oss << L"\" data-client-limit=\"";
        oss << kClientLineLimit;
        oss << L"\">\n";

        oss << L"<div class=\"system-log-toolbar\">\n";
        oss << L"<select id=\"systemLogLevel\" aria-label=\"ログレベル\">\n";
        oss << L"<option value=\"all\">全レベル</option>\n";
        oss << L"<option value=\"info\">INFO</option>\n";
        oss << L"<option value=\"warn\">WARN</option>\n";
        oss << L"<option value=\"error\">ERROR</option>\n";
        oss << L"</select>\n";

        oss << L"<select id=\"systemLogCategory\" aria-label=\"ログ分類\">\n";
        oss << L"<option value=\"all\">すべての分類</option>\n";
        oss << L"<option value=\"record\">録画</option>\n";
        oss << L"<option value=\"reservation\">予約</option>\n";
        oss << L"<option value=\"epg\">EPG</option>\n";
        oss << L"<option value=\"tuner\">チューナー</option>\n";
        oss << L"</select>\n";

        oss << L"<input class=\"system-log-search\" id=\"systemLogSearch\" type=\"search\"";
        oss << L" placeholder=\"ログ本文を検索\" autocomplete=\"off\">\n";
        oss << L"<button type=\"button\" id=\"systemLogReload\">再読込</button>\n";
        oss << L"<button type=\"button\" id=\"systemLogAutoUpdate\" class=\"active\" aria-pressed=\"true\">自動更新 ON</button>\n";
        oss << L"<button type=\"button\" id=\"systemLogAutoScroll\" class=\"active\" aria-pressed=\"true\">自動スクロール ON</button>\n";
        oss << L"</div>\n";

        oss << L"<div class=\"system-log-status\" id=\"systemLogStatus\">\n";
        oss << L"<span id=\"systemLogStatusText\">システムログを読み込んでいます...</span>\n";
        oss << L"<span id=\"systemLogCount\">表示 0 / 取得 0</span>\n";
        oss << L"</div>\n";

        oss << L"<div class=\"system-log-view\" id=\"systemLogView\" role=\"log\" aria-live=\"polite\">\n";
        oss << L"<div class=\"system-log-empty\" id=\"systemLogEmpty\">読み込み中...</div>\n";
        oss << L"</div>\n";
        oss << L"</div>\n";

        oss << Common::BuildLayoutEnd();
        oss << L"</body>\n";
        oss << L"</html>\n";

        return oss.str();
    }

    void AppendJsLine_(
        std::wostringstream& oss,
        const std::wstring_view line)
    {
        oss << line << L'\n';
    }

    std::wstring BuildLogPageScript_()
    {
        std::wostringstream oss;

        AppendJsLine_(oss, L"(function(){");
        AppendJsLine_(oss, L"'use strict';");
        AppendJsLine_(oss, L"var page=document.getElementById('systemLogPage');");
        AppendJsLine_(oss, L"if(!page){return;}");
        AppendJsLine_(oss, L"var view=document.getElementById('systemLogView');");
        AppendJsLine_(oss, L"var empty=document.getElementById('systemLogEmpty');");
        AppendJsLine_(oss, L"var statusBox=document.getElementById('systemLogStatus');");
        AppendJsLine_(oss, L"var statusText=document.getElementById('systemLogStatusText');");
        AppendJsLine_(oss, L"var countText=document.getElementById('systemLogCount');");
        AppendJsLine_(oss, L"var levelSelect=document.getElementById('systemLogLevel');");
        AppendJsLine_(oss, L"var categorySelect=document.getElementById('systemLogCategory');");
        AppendJsLine_(oss, L"var searchInput=document.getElementById('systemLogSearch');");
        AppendJsLine_(oss, L"var reloadButton=document.getElementById('systemLogReload');");
        AppendJsLine_(oss, L"var updateButton=document.getElementById('systemLogAutoUpdate');");
        AppendJsLine_(oss, L"var scrollButton=document.getElementById('systemLogAutoScroll');");
        AppendJsLine_(oss, L"var initialLimit=Math.max(1,Number(page.getAttribute('data-initial-limit'))||300);");
        AppendJsLine_(oss, L"var pollMs=Math.max(1000,Number(page.getAttribute('data-poll-ms'))||5000);");
        AppendJsLine_(oss, L"var clientLimit=Math.max(initialLimit,Number(page.getAttribute('data-client-limit'))||2000);");
        AppendJsLine_(oss, L"var cursor='0';");
        AppendJsLine_(oss, L"var entries=[];");
        AppendJsLine_(oss, L"var polling=true;");
        AppendJsLine_(oss, L"var autoScroll=true;");
        AppendJsLine_(oss, L"var loading=false;");
        AppendJsLine_(oss, L"var timer=0;");
        AppendJsLine_(oss, L"var searchTimer=0;");

        AppendJsLine_(oss, L"function runtimeValue(name){");
        AppendJsLine_(oss, L" try{if(window.EPG_RUNTIME&&window.EPG_RUNTIME[name]){return String(window.EPG_RUNTIME[name]);}}catch(e){}");
        AppendJsLine_(oss, L" try{if(window.parent&&window.parent!==window&&window.parent.EPG_RUNTIME&&window.parent.EPG_RUNTIME[name]){return String(window.parent.EPG_RUNTIME[name]);}}catch(e){}");
        AppendJsLine_(oss, L" try{if(window.top&&window.top!==window&&window.top.EPG_RUNTIME&&window.top.EPG_RUNTIME[name]){return String(window.top.EPG_RUNTIME[name]);}}catch(e){}");
        AppendJsLine_(oss, L" return '';");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"function trimSlash(value){return String(value||'').replace(/\\/+$/,'');}");
        AppendJsLine_(oss, L"function apiUrl(){");
        AppendJsLine_(oss, L" var direct=runtimeValue('systemLogApiUrl');");
        AppendJsLine_(oss, L" if(direct){return direct;}");
        AppendJsLine_(oss, L" var systemBase=trimSlash(runtimeValue('systemApiBase'));");
        AppendJsLine_(oss, L" if(systemBase){return systemBase+'/log';}");
        AppendJsLine_(oss, L" var reservationBase=trimSlash(runtimeValue('reservationApiBase'));");
        AppendJsLine_(oss, L" if(reservationBase){");
        AppendJsLine_(oss, L"   if(/\\/api\\/reservation$/i.test(reservationBase)){return reservationBase.replace(/\\/reservation$/i,'/system')+'/log';}");
        AppendJsLine_(oss, L"   return reservationBase+'/system/log';");
        AppendJsLine_(oss, L" }");
        AppendJsLine_(oss, L" return '/api/system/log';");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"function setStatus(message,isError){");
        AppendJsLine_(oss, L" statusText.textContent=message;");
        AppendJsLine_(oss, L" statusBox.classList.toggle('error',Boolean(isError));");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"function detectLevel(text){");
        AppendJsLine_(oss, L" var value=String(text||'');");
        AppendJsLine_(oss, L" if(/\\[(ERROR|FATAL)\\]/i.test(value)){return 'error';}");
        AppendJsLine_(oss, L" if(/\\[(WARN|WARNING)\\]/i.test(value)){return 'warn';}");
        AppendJsLine_(oss, L" return 'info';");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"function detectCategory(text){");
        AppendJsLine_(oss, L" var value=String(text||'').toLowerCase();");
        AppendJsLine_(oss, L" if(value.indexOf('録画')>=0||value.indexOf('record')>=0){return 'record';}");
        AppendJsLine_(oss, L" if(value.indexOf('予約')>=0||value.indexOf('reserve')>=0){return 'reservation';}");
        AppendJsLine_(oss, L" if(value.indexOf('epg')>=0||value.indexOf('eit')>=0||value.indexOf('番組表')>=0){return 'epg';}");
        AppendJsLine_(oss, L" if(value.indexOf('チューナー')>=0||value.indexOf('tuner')>=0||value.indexOf('bondriver')>=0){return 'tuner';}");
        AppendJsLine_(oss, L" return 'other';");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"function normalizeLine(item){");
        AppendJsLine_(oss, L" var text='';");
        AppendJsLine_(oss, L" if(typeof item==='string'){text=item;}");
        AppendJsLine_(oss, L" else if(item&&typeof item==='object'){text=String(item.text||item.line||item.message||'');}");
        AppendJsLine_(oss, L" return {text:text,level:detectLevel(text),category:detectCategory(text)};");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"function matches(entry){");
        AppendJsLine_(oss, L" var level=levelSelect.value;");
        AppendJsLine_(oss, L" var category=categorySelect.value;");
        AppendJsLine_(oss, L" var keyword=searchInput.value.trim().toLowerCase();");
        AppendJsLine_(oss, L" if(level!=='all'&&entry.level!==level){return false;}");
        AppendJsLine_(oss, L" if(category!=='all'&&entry.category!==category){return false;}");
        AppendJsLine_(oss, L" return !keyword||entry.text.toLowerCase().indexOf(keyword)>=0;");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"function render(){");
        AppendJsLine_(oss, L" var fragment=document.createDocumentFragment();");
        AppendJsLine_(oss, L" var shown=0;");
        AppendJsLine_(oss, L" for(var i=0;i<entries.length;i+=1){");
        AppendJsLine_(oss, L"   var entry=entries[i];");
        AppendJsLine_(oss, L"   if(!matches(entry)){continue;}");
        AppendJsLine_(oss, L"   var row=document.createElement('div');");
        AppendJsLine_(oss, L"   row.className='system-log-line level-'+entry.level;");
        AppendJsLine_(oss, L"   row.textContent=entry.text;");
        AppendJsLine_(oss, L"   fragment.appendChild(row);");
        AppendJsLine_(oss, L"   shown+=1;");
        AppendJsLine_(oss, L" }");
        AppendJsLine_(oss, L" while(view.firstChild){view.removeChild(view.firstChild);}");
        AppendJsLine_(oss, L" if(shown===0){");
        AppendJsLine_(oss, L"   empty=document.createElement('div');");
        AppendJsLine_(oss, L"   empty.className='system-log-empty';");
        AppendJsLine_(oss, L"   empty.textContent=entries.length===0?'表示できるログはありません。':'絞り込み条件に一致するログはありません。';");
        AppendJsLine_(oss, L"   view.appendChild(empty);");
        AppendJsLine_(oss, L" }else{view.appendChild(fragment);}");
        AppendJsLine_(oss, L" countText.textContent='表示 '+String(shown)+' / 取得 '+String(entries.length);");
        AppendJsLine_(oss, L" if(autoScroll){view.scrollTop=view.scrollHeight;}");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"function applyResponse(data,forceReset){");
        AppendJsLine_(oss, L" var shouldReset=Boolean(forceReset||data.reset);");
        AppendJsLine_(oss, L" if(shouldReset){entries=[];}");
        AppendJsLine_(oss, L" var lines=Array.isArray(data.lines)?data.lines:[];");
        AppendJsLine_(oss, L" for(var i=0;i<lines.length;i+=1){entries.push(normalizeLine(lines[i]));}");
        AppendJsLine_(oss, L" if(entries.length>clientLimit){entries.splice(0,entries.length-clientLimit);}");
        AppendJsLine_(oss, L" if(data.cursor!==undefined&&data.cursor!==null){cursor=String(data.cursor);}");
        AppendJsLine_(oss, L" render();");
        AppendJsLine_(oss, L" setStatus(lines.length>0?String(lines.length)+'件の新しいログを取得しました。':'最新の状態です。',false);");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"function loadLog(forceReset){");
        AppendJsLine_(oss, L" if(loading){return Promise.resolve(false);}");
        AppendJsLine_(oss, L" loading=true;");
        AppendJsLine_(oss, L" var requestCursor=forceReset?'0':cursor;");
        AppendJsLine_(oss, L" var url=apiUrl()+'?cursor='+encodeURIComponent(requestCursor)+'&limit='+encodeURIComponent(String(initialLimit))+'&v='+encodeURIComponent(String(Date.now()));");
        AppendJsLine_(oss, L" if(forceReset){setStatus('システムログを再読み込みしています...',false);}");
        AppendJsLine_(oss, L" return fetch(url,{method:'GET',cache:'no-store',headers:{'Accept':'application/json'}})");
        AppendJsLine_(oss, L" .then(function(response){return response.text().then(function(body){return {response:response,body:body};});})");
        AppendJsLine_(oss, L" .then(function(result){");
        AppendJsLine_(oss, L"   var data=null;");
        AppendJsLine_(oss, L"   try{data=JSON.parse(result.body||'{}');}catch(e){throw new Error('応答JSONを解析できません。');}");
        AppendJsLine_(oss, L"   if(!result.response.ok||!data||data.ok===false){throw new Error((data&&(data.message||data.reason))||('HTTP '+String(result.response.status)));}");
        AppendJsLine_(oss, L"   applyResponse(data,forceReset);");
        AppendJsLine_(oss, L"   return true;");
        AppendJsLine_(oss, L" })");
        AppendJsLine_(oss, L" .catch(function(error){setStatus('ログ読込失敗: '+String(error&&error.message?error.message:error),true);return false;})");
        AppendJsLine_(oss, L" .finally(function(){loading=false;});");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"function schedule(){");
        AppendJsLine_(oss, L" if(timer){window.clearTimeout(timer);timer=0;}");
        AppendJsLine_(oss, L" if(!polling){return;}");
        AppendJsLine_(oss, L" timer=window.setTimeout(function(){loadLog(false).finally(schedule);},pollMs);");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"function updateToggle(button,enabled,onText,offText){");
        AppendJsLine_(oss, L" button.classList.toggle('active',enabled);");
        AppendJsLine_(oss, L" button.setAttribute('aria-pressed',enabled?'true':'false');");
        AppendJsLine_(oss, L" button.textContent=enabled?onText:offText;");
        AppendJsLine_(oss, L"}");

        AppendJsLine_(oss, L"levelSelect.addEventListener('change',render);");
        AppendJsLine_(oss, L"categorySelect.addEventListener('change',render);");
        AppendJsLine_(oss, L"searchInput.addEventListener('input',function(){if(searchTimer){window.clearTimeout(searchTimer);}searchTimer=window.setTimeout(render,120);});");
        AppendJsLine_(oss, L"reloadButton.addEventListener('click',function(){cursor='0';loadLog(true).finally(schedule);});");
        AppendJsLine_(oss, L"updateButton.addEventListener('click',function(){polling=!polling;updateToggle(updateButton,polling,'自動更新 ON','自動更新 OFF');schedule();});");
        AppendJsLine_(oss, L"scrollButton.addEventListener('click',function(){autoScroll=!autoScroll;updateToggle(scrollButton,autoScroll,'自動スクロール ON','自動スクロール OFF');if(autoScroll){view.scrollTop=view.scrollHeight;}});");
        AppendJsLine_(oss, L"document.addEventListener('visibilitychange',function(){if(!document.hidden&&polling){loadLog(false).finally(schedule);}});");
        AppendJsLine_(oss, L"window.addEventListener('beforeunload',function(){if(timer){window.clearTimeout(timer);}});");
        AppendJsLine_(oss, L"loadLog(true).finally(schedule);");
        AppendJsLine_(oss, L"})();");

        return oss.str();
    }
}

namespace EpgHtmlLite::Log
{
    bool Generate(
        const Context& ctx,
        Result& ioResult)
    {
        try {
            if (ctx.outputDir.empty()) {
                return false;
            }

            const std::filesystem::path htmlPath =
                ctx.outputDir /
                std::filesystem::path(
                    BuildLogPageFileName_(ctx.space));

            const std::filesystem::path scriptPath =
                ctx.outputDir /
                std::filesystem::path(
                    kLogScriptFile);

            if (!WriteUtf8BomTextFile_(
                htmlPath,
                BuildLogPageHtml_(ctx))) {
                return false;
            }

            if (!WriteUtf8BomTextFile_(
                scriptPath,
                BuildLogPageScript_())) {
                return false;
            }

            ioResult.generatedFiles.push_back(
                htmlPath.wstring());

            ioResult.generatedFiles.push_back(
                scriptPath.wstring());

            return true;
        }
        catch (...) {
            return false;
        }
    }
}
