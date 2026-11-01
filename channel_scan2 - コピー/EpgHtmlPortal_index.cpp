#include "EpgHtmlPortal_index.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <system_error>

#include "DebugManager.h"
#include "EpgHtmlPortal_Assets.hpp"
#include "EpgHtmlPortal_Common.hpp"
#include "EpgHtmlPortal_RuntimeData.hpp"
#include "EpgHtmlPortal_Template.hpp"
#include "EpgHtmlPortal_dynamic.hpp"
#include "EpgHtmlPortal_Auto.hpp"
#include "EpgHtmlPortal_SearchData.hpp"

namespace EpgHtmlPortal_index
{
    namespace
    {
        std::wstring BuildFallbackPlaceholderHtml_(
            std::wstring_view title,
            std::wstring_view message)
        {
            std::wstring html;
            html.reserve(1024);

            html += L"<!DOCTYPE html>\n";
            html += L"<html lang=\"ja\">\n";
            html += L"<head>\n";
            html += L"<meta charset=\"UTF-8\">\n";
            html += L"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
            html += L"<title>";
            html += EpgHtmlPortal_Common::EscapeHtmlW(title);
            html += L"</title>\n";
            html += L"<style>";
            html += L"body{margin:0;font-family:\"Yu Gothic UI\",\"Meiryo\",sans-serif;background:#0f1115;color:#e8ecf1;}";
            html += L".main{max-width:960px;margin:0 auto;padding:32px 20px;}";
            html += L".card{background:#171b22;border:1px solid #2a313d;border-radius:14px;padding:24px;}";
            html += L"h1{margin:0 0 12px 0;font-size:24px;}";
            html += L"p{margin:0;line-height:1.8;color:#c3cad4;}";
            html += L"</style>\n";
            html += L"</head>\n";
            html += L"<body>\n";
            html += L"<div class=\"main\"><div class=\"card\"><h1>";
            html += EpgHtmlPortal_Common::EscapeHtmlW(title);
            html += L"</h1><p>";
            html += EpgHtmlPortal_Common::EscapeHtmlW(message);
            html += L"</p></div></div>\n";
            html += L"</body>\n";
            html += L"</html>\n";

            return html;
        }

        static void AppendIndexSwitchLayoutCss_(
            std::wstring& html)
        {
            html += L".topbar,.subbar{";
            html += L"padding:8px 14px;";
            html += L"background:#171b22;";
            html += L"border-bottom:1px solid #2a313d;";
            html += L"}\n";

            html += L".topbar{";
            html += L"position:relative;";
            html += L"z-index:4;";
            html += L"}\n";

            html += L".subbar{";
            html += L"position:relative;";
            html += L"z-index:3;";
            html += L"}\n";

            html += L".menu,.subbar-inner,.quick-search,.detail-actions{";
            html += L"display:flex;";
            html += L"gap:8px;";
            html += L"flex-wrap:wrap;";
            html += L"align-items:center;";
            html += L"}\n";

            html += L".menu-group,.subbar-group{";
            html += L"display:flex;";
            html += L"gap:6px;";
            html += L"flex-wrap:wrap;";
            html += L"align-items:center;";
            html += L"}\n";

            html += L".menu-label,.subbar-label{";
            html += L"font-size:12px;";
            html += L"color:#aeb8c6;";
            html += L"margin-right:2px;";
            html += L"white-space:nowrap;";
            html += L"}\n";

            html += L".subbar-sep{";
            html += L"width:1px;";
            html += L"height:24px;";
            html += L"background:#2f3948;";
            html += L"margin:0 4px;";
            html += L"}\n";

            html += L"#spaceStatusLabel{";
            html += L"margin-left:auto;";
            html += L"color:#aeb8c6;";
            html += L"font-size:12px;";
            html += L"white-space:nowrap;";
            html += L"}\n";
        }

        static void AppendIndexSwitchButtonCss_(
            std::wstring& html)
        {
            html += L".topbar button,.subbar button,.quick-search button,.detail-actions button{";
            html += L"border:1px solid #384252;";
            html += L"background:#232936;";
            html += L"color:#e9eef7;";
            html += L"border-radius:8px;";
            html += L"padding:6px 10px;";
            html += L"font:inherit;";
            html += L"font-size:13px;";
            html += L"line-height:1.2;";
            html += L"cursor:pointer;";
            html += L"}\n";

            html += L".topbar button:hover,.subbar button:hover,.quick-search button:hover,.detail-actions button:hover{";
            html += L"background:#2c3544;";
            html += L"border-color:#4b5a70;";
            html += L"}\n";

            html += L".active,.active-space,.active-page,.active-program-range,.active-view-kind,.toggle-focus-active{";
            html += L"background:#2d6cdf!important;";
            html += L"border-color:#2d6cdf!important;";
            html += L"color:#fff!important;";
            html += L"}\n";

            html += L".topbar button[disabled],.subbar button[disabled]{";
            html += L"opacity:.45;";
            html += L"cursor:not-allowed;";
            html += L"}\n";
        }

        static void AppendIndexQuickSearchCss_(
            std::wstring& html)
        {
            html += L".quick-search input{";
            html += L"min-width:220px;";
            html += L"border:1px solid #384252;";
            html += L"background:#0f131a;";
            html += L"color:#e9eef7;";
            html += L"border-radius:8px;";
            html += L"padding:6px 9px;";
            html += L"font:inherit;";
            html += L"font-size:13px;";
            html += L"}\n";

            html += L".quick-search input::placeholder{";
            html += L"color:#7f8ba0;";
            html += L"}\n";
        }

        static void AppendIndexPastCalendarCss_(
            std::wstring& html)
        {
            html += L"[hidden]{display:none!important;}\n";

            html += L".past-date-group{";
            html += L"display:flex;";
            html += L"gap:6px;";
            html += L"align-items:center;";
            html += L"flex-wrap:wrap;";
            html += L"}\n";

            html += L".past-date-group input[type=\"date\"]{";
            html += L"min-width:150px;";
            html += L"border:1px solid #384252;";
            html += L"background:#0f131a;";
            html += L"color:#e9eef7;";
            html += L"border-radius:8px;";
            html += L"padding:5px 8px;";
            html += L"font:inherit;";
            html += L"font-size:13px;";
            html += L"}\n";

            html += L".past-picker-panel{";
            html += L"border:1px solid #313a4b;";
            html += L"border-radius:10px;";
            html += L"background:#1d2330;";
            html += L"padding:12px;";
            html += L"margin-bottom:10px;";
            html += L"color:#d7dce5;";
            html += L"}\n";

            html += L".past-picker-row{";
            html += L"display:flex;";
            html += L"gap:8px;";
            html += L"align-items:center;";
            html += L"flex-wrap:wrap;";
            html += L"}\n";

            html += L".past-picker-row input[type=\"date\"]{";
            html += L"min-width:170px;";
            html += L"}\n";

            html += L".past-picker-note{";
            html += L"margin-top:8px;";
            html += L"font-size:12px;";
            html += L"color:#aeb8c6;";
            html += L"line-height:1.6;";
            html += L"}\n";
        }

        static void AppendIndexSwitchCss_(
            std::wstring& html)
        {
            AppendIndexSwitchLayoutCss_(html);
            AppendIndexSwitchButtonCss_(html);
            AppendIndexQuickSearchCss_(html);
            AppendIndexPastCalendarCss_(html);
        }

        static void AppendIndexBaseCss_(
            std::wstring& html)
        {
            html += L"body{margin:0;font-family:\"Yu Gothic UI\",\"Meiryo\",sans-serif;background:#0f1115;color:#e8ecf1;}\n";
            html += L".app{display:grid;grid-template-rows:auto auto 1fr;min-height:100vh;}\n";
            html += L"button,input,select{font:inherit;}\n";
            html += L"input,select{padding:8px 10px;border-radius:8px;border:1px solid #3a4558;background:#ffffff;color:#222;}\n";

            AppendIndexSwitchCss_(html);

            html += L".layout{display:grid;grid-template-columns:260px minmax(0,1fr) 320px;gap:12px;padding:12px;}\n";
            html += L".layout.table-focus-mode{grid-template-columns:minmax(0,1fr);}\n";
            html += L".layout.table-focus-mode > aside{display:none;}\n";
            html += L".layout.table-focus-mode > main{min-width:0;}\n";
            html += L".layout.table-focus-mode .page-card{padding:8px;}\n";
            html += L".layout.table-focus-mode .viewer{height:auto;min-height:720px;}\n";
            html += L".layout.table-focus-mode .result-table-wrap,.layout.table-focus-mode .reservation-table-wrap{min-height:calc(100vh - 170px);}\n";

            html += L".panel,.page-card{background:#171b22;border:1px solid #2a313d;border-radius:12px;padding:12px;}\n";
            html += L".page-section{display:none;}\n";
            html += L".page-section.active{display:block;}\n";

            html += L".viewer{position:relative;min-height:520px;height:auto;border:1px solid #313a4b;border-radius:10px;overflow:visible;background:#fff;}\n";
            html += L".viewer iframe{width:100%;height:520px;border:0;display:block;overflow:hidden;background:#fff;}\n";
            html += L".viewer-empty{position:absolute;inset:0;display:none;align-items:center;justify-content:center;padding:16px;text-align:center;background:#151922;color:#d7dce5;}\n";
            html += L".viewer-empty.open{display:flex;}\n";

            html += L".placeholder,.search-summary,.reservation-summary,.detail-box{border:1px solid #313a4b;border-radius:10px;padding:12px;background:#1d2330;color:#d7dce5;}\n";
            html += L".result-table-wrap{min-height:420px;overflow:auto;border:1px solid #313a4b;border-radius:10px;background:#11161f;}\n";
            html += L".reservation-table-wrap{min-height:280px;}\n";

            html += L".reservation-table th:nth-child(1){width:120px}\n";
            html += L".reservation-table th:nth-child(2){width:100px}\n";
            html += L".reservation-table th:nth-child(3){width:120px}\n";
            html += L".reservation-table th:nth-child(4){width:100px}\n";
            html += L".reservation-table th:nth-child(5){min-width:260px}\n";
            html += L".reservation-table th:nth-child(6){width:180px}\n";
            html += L".reservation-table th:nth-child(7){min-width:260px}\n";
            html += L".reservation-table th:nth-child(8){width:130px}\n";
            html += L".reservation-table th:nth-child(9){width:90px}\n";
            html += L".reservation-table td:nth-child(7){white-space:pre-wrap;word-break:break-word}\n";
            html += L".reservation-row-epg{background:rgba(123,170,255,.10)}\n";
            html += L".reservation-row-watch{background:rgba(255,186,199,.10)}\n";
            html += L".reservation-row-record{background:rgba(255,132,156,.12)}\n";
            html += L".reservation-row-disabled{opacity:.66;filter:saturate(.72)}\n";

            html += L".search-overlay{position:fixed;inset:0;background:rgba(0,0,0,.45);display:none;z-index:50;}\n";
            html += L".search-overlay.open{display:block;}\n";
            html += L".search-dialog{width:min(980px,calc(100vw - 40px));max-height:calc(100vh - 40px);overflow:auto;margin:20px auto;background:#171b22;border:1px solid #2a313d;border-radius:14px;}\n";
            html += L".search-head{padding:12px 14px;border-bottom:1px solid #2a313d;display:flex;justify-content:space-between;align-items:center;}\n";
            html += L".search-body{padding:14px;}\n";
            html += L".search-body label{display:grid;gap:6px;}\n";
            html += L".search-body label > input,.search-body label > select{margin-top:2px;}\n";
            html += L".search-row,.grid-2{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:12px;}\n";
            html += L".listbox{width:100%;min-height:180px;}\n";

            html += L"@media (max-width:980px){.layout{grid-template-columns:1fr;}.search-row,.grid-2{grid-template-columns:1fr;}}\n";
        }

        static void AppendIndexTopbarHtml_(
            std::wstring& html)
        {
            html += L"<header class=\"topbar\"><div class=\"menu\">";

            html += L"<div class=\"menu-group\">";
            html += L"<span class=\"menu-label\">メニュー</span>";
            html += L"<button type=\"button\" class=\"active active-page\" data-page=\"now\" onclick=\"switchProgramRange('now', this)\">番組表</button>";
            html += L"<button type=\"button\" data-page=\"search\" onclick=\"switchPage('search', this)\">検索結果</button>";
            html += L"<button type=\"button\" data-page=\"reservation\" onclick=\"switchPage('reservation', this)\">予約一覧</button>";
            html += L"<button type=\"button\" data-page=\"auto-reserve\" onclick=\"switchPage('auto-reserve', this)\">自動予約</button>";
            html += L"</div>";

            html += L"<div class=\"quick-search\" style=\"margin-left:auto;\">";
            html += L"<input id=\"quickKeyword\" type=\"text\" placeholder=\"キーワード検索\">";
            html += L"<button type=\"button\" onclick=\"runQuickSearch()\">検索</button>";
            html += L"</div>";

            html += L"</div></header>\n";
        }

        static void AppendIndexSubbarHtml_(
            std::wstring& html)
        {
            html += L"<div class=\"subbar\"><div class=\"subbar-inner\">";

            html += L"<div class=\"subbar-group\">";
            html += L"<span class=\"subbar-label\">対象</span>";
            html += L"<button type=\"button\" class=\"active-program-range\" data-program-range=\"now\" onclick=\"switchProgramRange('now', this)\">現在・今後</button>";
            html += L"<button type=\"button\" data-program-range=\"past\" onclick=\"switchProgramRange('past', this)\">過去</button>";
            html += L"</div>";

            html += L"<span class=\"subbar-sep\"></span>";

            html += L"<div class=\"subbar-group\">";
            html += L"<span class=\"subbar-label\">放送波</span>";
            html += L"<button type=\"button\" data-space=\"0\" onclick=\"switchSpace(0, this)\">地上波</button>";
            html += L"<button type=\"button\" data-space=\"1\" onclick=\"switchSpace(1, this)\">BS</button>";
            html += L"<button type=\"button\" data-space=\"2\" onclick=\"switchSpace(2, this)\">CS</button>";
            html += L"</div>";

            html += L"<span class=\"subbar-sep\"></span>";

            html += L"<div class=\"subbar-group\">";
            html += L"<span class=\"subbar-label\">表示</span>";
            html += L"<button type=\"button\" class=\"active-view-kind\" data-program-view=\"timetable\" onclick=\"switchProgramView('timetable', this)\">時間割</button>";
            html += L"<button type=\"button\" data-program-view=\"list\" onclick=\"switchProgramView('list', this)\">一覧</button>";
            html += L"</div>";

            html += L"<span class=\"subbar-sep\"></span>";

            html += L"<div class=\"past-date-group\" id=\"pastDateControlGroup\" hidden>";
            html += L"<span class=\"subbar-label\">過去日付</span>";
            html += L"<input id=\"pastDatePicker\" type=\"date\">";
            html += L"<button type=\"button\" onclick=\"openPastDay()\">表示</button>";
            html += L"</div>";

            html += L"<span class=\"subbar-sep\"></span>";

            html += L"<div class=\"subbar-group\">";
            html += L"<span class=\"subbar-label\">操作</span>";
            html += L"<button type=\"button\" id=\"toggleMainSwitchTableFocusBtn\" onclick=\"toggleMainSwitchTableFocus()\">番組表を拡大</button>";
            html += L"<button type=\"button\" onclick=\"openDetailSearch()\">詳細検索</button>";
            html += L"</div>";

            html += L"<span id=\"spaceStatusLabel\">Space 0</span>";
            html += L"</div></div>\n";
        }

        static void AppendIndexCompatScript_(
            std::wstring& html)
        {
            html += L"<script>\n";
            html += L"(function(){\n";
            html += L"'use strict';\n";

            html += L"function byId_(id){return document.getElementById(id);}\n";
            html += L"function spaceData_(){try{if(typeof SPACE_DATA!=='undefined')return SPACE_DATA;}catch(_){ }return window.SPACE_DATA||{};}\n";
            html += L"function pastManifest_(){try{if(window.EPG_PAST_MANIFEST)return window.EPG_PAST_MANIFEST;}catch(_){ }return {};}\n";
            html += L"function normView_(v){return String(v||'').toLowerCase()==='list'?'list':'timetable';}\n";
            html += L"function normRange_(v){v=String(v||'').toLowerCase();return (v==='past'||v==='before')?'past':'now';}\n";

            html += L"function toYmdCompact_(dateText){\n";
            html += L"const s=String(dateText||'').trim();\n";
            html += L"const m=s.match(/^(\\d{4})-(\\d{2})-(\\d{2})$/);\n";
            html += L"if(m)return m[1]+m[2]+m[3];\n";
            html += L"const n=s.replace(/[^0-9]/g,'');\n";
            html += L"return n.length===8?n:'';\n";
            html += L"}\n";

            html += L"function toDateInput_(ymd){\n";
            html += L"const s=String(ymd||'').replace(/[^0-9]/g,'');\n";
            html += L"if(s.length!==8)return'';\n";
            html += L"return s.slice(0,4)+'-'+s.slice(4,6)+'-'+s.slice(6,8);\n";
            html += L"}\n";

            html += L"function setDateInputs_(ymd){\n";
            html += L"const v=toDateInput_(ymd);\n";
            html += L"const a=byId_('pastDatePicker');\n";
            html += L"const b=byId_('pastDatePickerInPage');\n";
            html += L"if(a&&v)a.value=v;\n";
            html += L"if(b&&v)b.value=v;\n";
            html += L"}\n";

            html += L"function readSelectedPastDate_(){\n";
            html += L"const a=byId_('pastDatePicker');\n";
            html += L"const b=byId_('pastDatePickerInPage');\n";
            html += L"return toYmdCompact_((a&&a.value)||((b&&b.value)||''));\n";
            html += L"}\n";

            html += L"function getLatestPastDate_(space){\n";
            html += L"const m=pastManifest_();\n";
            html += L"const list=m[String(space)]||m[space]||[];\n";
            html += L"if(!Array.isArray(list)||list.length===0)return'';\n";
            html += L"let best='';\n";
            html += L"list.forEach(function(x){const d=toYmdCompact_(x&&x.date);if(d&&d>best)best=d;});\n";
            html += L"return best;\n";
            html += L"}\n";

            html += L"function findPastManifestEntry_(space,date){\n";
            html += L"const m=pastManifest_();\n";
            html += L"const list=m[String(space)]||m[space]||[];\n";
            html += L"if(!Array.isArray(list))return null;\n";
            html += L"const target=toYmdCompact_(date);\n";
            html += L"for(const x of list){if(toYmdCompact_(x&&x.date)===target)return x;}\n";
            html += L"return null;\n";
            html += L"}\n";

            html += L"function buildPastViewerSrc_(space,date,view){\n";
            html += L"const d=toYmdCompact_(date);\n";
            html += L"const v=normView_(view);\n";
            html += L"if(!d)return'';\n";
            html += L"const ent=findPastManifestEntry_(space,d);\n";
            html += L"if(ent){\n";
            html += L"if(v==='list'&&ent.listSrc)return String(ent.listSrc);\n";
            html += L"if(v==='timetable'&&ent.timetableSrc)return String(ent.timetableSrc);\n";
            html += L"}\n";
            html += L"return '';\n";
            html += L"}\n";

            html += L"const gFrameHeightObservers_=new WeakMap();\n";
            html += L"function resizeFrameToContent_(frame){\n";
            html += L"if(!frame||frame.style.display==='none')return;\n";
            html += L"try{\n";
            html += L"const doc=frame.contentDocument||(frame.contentWindow?frame.contentWindow.document:null);\n";
            html += L"if(!doc||!doc.documentElement||!doc.body)return;\n";
            html += L"doc.documentElement.style.height='auto';doc.documentElement.style.overflowY='hidden';\n";
            html += L"doc.body.style.minHeight='0';doc.body.style.height='auto';doc.body.style.overflowY='hidden';\n";
            html += L"const root=doc.documentElement,body=doc.body;\n";
            html += L"const height=Math.max(root.scrollHeight,root.offsetHeight,body.scrollHeight,body.offsetHeight);\n";
            html += L"if(height>0&&Math.abs(frame.getBoundingClientRect().height-height)>2)frame.style.height=Math.ceil(height)+'px';\n";
            html += L"}catch(_e){}\n";
            html += L"}\n";
            html += L"function attachFrameAutoHeight_(frame){\n";
            html += L"if(!frame)return;\n";
            html += L"const old=gFrameHeightObservers_.get(frame);if(old){try{old.disconnect();}catch(_e){}}\n";
            html += L"resizeFrameToContent_(frame);\n";
            html += L"try{\n";
            html += L"const doc=frame.contentDocument||(frame.contentWindow?frame.contentWindow.document:null);\n";
            html += L"if(!doc||typeof ResizeObserver!=='function')return;\n";
            html += L"const observer=new ResizeObserver(function(){window.requestAnimationFrame(function(){resizeFrameToContent_(frame);});});\n";
            html += L"if(doc.documentElement)observer.observe(doc.documentElement);if(doc.body)observer.observe(doc.body);\n";
            html += L"gFrameHeightObservers_.set(frame,observer);\n";
            html += L"}catch(_e){}\n";
            html += L"}\n";
            html += L"function ensureFrameAutoHeight_(frame){\n";
            html += L"if(!frame)return;if(frame.getAttribute('data-auto-height-hooked')!=='1'){frame.addEventListener('load',function(){attachFrameAutoHeight_(frame);});frame.setAttribute('data-auto-height-hooked','1');}\n";
            html += L"window.requestAnimationFrame(function(){attachFrameAutoHeight_(frame);});\n";
            html += L"}\n";

            html += L"function clearFrame_(frameId,emptyId,linkId,message){\n";
            html += L"const frame=byId_(frameId),empty=byId_(emptyId),link=byId_(linkId);\n";
            html += L"if(frame){const prev=frame.getAttribute('data-bound-src')||'';if(prev!==''){frame.src='about:blank';frame.setAttribute('data-bound-src','');}frame.style.display='none';}\n";
            html += L"if(empty){empty.textContent=message||'';if(message)empty.classList.add('open');else empty.classList.remove('open');}\n";
            html += L"if(link){link.href='#';link.style.pointerEvents='none';link.style.opacity='0.45';}\n";
            html += L"}\n";

            html += L"function bindFrame_(src,exists,frameId,emptyId,linkId,msg){\n";
            html += L"const frame=byId_(frameId),empty=byId_(emptyId),link=byId_(linkId);\n";
            html += L"if(!frame||!empty||!link)return;\n";
            html += L"const next=(exists&&src)?String(src):'';\n";
            html += L"const prev=frame.getAttribute('data-bound-src')||'';\n";
            html += L"if(next){\n";
            html += L"if(prev!==next){frame.src=next;frame.setAttribute('data-bound-src',next);}\n";
            html += L"frame.style.display='block';frame.setAttribute('scrolling','no');ensureFrameAutoHeight_(frame);empty.textContent='';empty.classList.remove('open');link.href=next;link.style.pointerEvents='auto';link.style.opacity='1';\n";
            html += L"}else{\n";
            html += L"clearFrame_(frameId,emptyId,linkId,msg||'未生成です。');\n";
            html += L"}\n";
            html += L"}\n";

            html += L"function getState_(){\n";
            html += L"window.__epgIndexState=window.__epgIndexState||{};\n";
            html += L"if(!Number.isFinite(Number(window.__epgIndexState.space)))window.__epgIndexState.space=Number((window.EPG_RUNTIME&&window.EPG_RUNTIME.defaultSpace)||0);\n";
            html += L"window.__epgIndexState.view=normView_(window.__epgIndexState.view||'timetable');\n";
            html += L"window.__epgIndexState.range=normRange_(window.__epgIndexState.range||'now');\n";
            html += L"window.__epgIndexState.pastDate=toYmdCompact_(window.__epgIndexState.pastDate||'');\n";
            html += L"return window.__epgIndexState;\n";
            html += L"}\n";

            html += L"function setPastControlsVisible_(visible){\n";
            html += L"const g=byId_('pastDateControlGroup');\n";
            html += L"if(g)g.hidden=!visible;\n";
            html += L"}\n";

            html += L"function resolveCurrentFrame_(info){\n";
            html += L"const st=getState_();\n";
            html += L"const view=normView_(st.view);\n";
            html += L"if(view==='list'){\n";
            html += L"if(info.hasCurrentList&&info.currentListSrc){return{src:info.currentListSrc,exists:true};}\n";
            html += L"return{src:info.currentSrc||'',exists:!!info.hasCurrent};\n";
            html += L"}\n";
            html += L"if(info.hasCurrentTimetable&&info.currentTimetableSrc){return{src:info.currentTimetableSrc,exists:true};}\n";
            html += L"return{src:info.currentSrc||'',exists:!!info.hasCurrent};\n";
            html += L"}\n";

            html += L"function setActive_(selector,cls,matchAttr,matchValue,btn){\n";
            html += L"document.querySelectorAll(selector).forEach(function(el){el.classList.remove(cls);el.setAttribute('aria-pressed','false');});\n";
            html += L"if(btn&&btn.classList){btn.classList.add(cls);btn.setAttribute('aria-pressed','true');return;}\n";
            html += L"const autoBtn=document.querySelector('['+matchAttr+'=\"'+String(matchValue)+'\"]');\n";
            html += L"if(autoBtn){autoBtn.classList.add(cls);autoBtn.setAttribute('aria-pressed','true');}\n";
            html += L"}\n";

            html += L"function localSwitchPage_(name,btn){\n";
            html += L"const allowed={now:1,past:1,search:1,reservation:1,'auto-reserve':1};\n";
            html += L"const next=allowed[String(name||'')]?String(name):'now';\n";
            html += L"document.querySelectorAll('.page-section').forEach(function(sec){sec.classList.remove('active');sec.setAttribute('aria-hidden','true');});\n";
            html += L"const target=byId_('page-'+next);\n";
            html += L"if(target){target.classList.add('active');target.setAttribute('aria-hidden','false');}\n";
            html += L"document.querySelectorAll('.menu [data-page]').forEach(function(el){el.classList.remove('active');el.classList.remove('active-page');el.setAttribute('aria-pressed','false');});\n";
            html += L"const topPage=(next==='past')?'now':next;\n";
            html += L"const autoBtn=btn&&btn.classList&&btn.hasAttribute('data-page')?btn:document.querySelector('.menu [data-page=\"'+topPage+'\"]');\n";
            html += L"if(autoBtn){autoBtn.classList.add('active');autoBtn.classList.add('active-page');autoBtn.setAttribute('aria-pressed','true');}\n";
            html += L"if(next==='auto-reserve'){\n";
            html += L"setPastControlsVisible_(false);\n";
            html += L"bindFrame_('epg_auto_reserve.html',true,'autoReserveFrame','autoReserveEmpty','autoReserveStandaloneLink','自動予約条件ページを読み込めませんでした。');\n";
            html += L"}\n";
            html += L"}\n";

            html += L"function localSwitchSpace_(space,btn){\n";
            html += L"const st=getState_();\n";
            html += L"const nextSpace=Number(space);\n";
            html += L"const all=spaceData_();\n";
            html += L"const info=all[String(nextSpace)];\n";
            html += L"if(!info)return;\n";
            html += L"st.space=nextSpace;\n";
            html += L"setActive_('[data-space]','active-space','data-space',nextSpace,btn);\n";
            html += L"const label=byId_('spaceStatusLabel');\n";
            html += L"if(label){label.textContent=(info.label||('Space '+nextSpace))+' / Space '+nextSpace+' / '+(normView_(st.view)==='list'?'一覧':'時間割');}\n";
            html += L"if(st.range==='past'){\n";
            html += L"clearFrame_('currentFrame','currentEmpty','currentStandaloneLink','');\n";
            html += L"clearFrame_('beforeFrame','beforeEmpty','beforeStandaloneLink','日付を選択して過去番組表を表示してください。');\n";
            html += L"const latest=getLatestPastDate_(nextSpace);\n";
            html += L"if(latest&&!st.pastDate){st.pastDate=latest;setDateInputs_(latest);}else if(st.pastDate){setDateInputs_(st.pastDate);}\n";
            html += L"return;\n";
            html += L"}\n";
            html += L"clearFrame_('beforeFrame','beforeEmpty','beforeStandaloneLink','');\n";
            html += L"const cur=resolveCurrentFrame_(info);\n";
            html += L"bindFrame_(cur.src,cur.exists,'currentFrame','currentEmpty','currentStandaloneLink',(info.label||('Space '+nextSpace))+' の番組表はまだ生成されていません。');\n";
            html += L"}\n";

            html += L"function switchRange_(range,btn){\n";
            html += L"const st=getState_();\n";
            html += L"st.range=normRange_(range);\n";
            html += L"setActive_('[data-program-range]','active-program-range','data-program-range',st.range,btn);\n";
            html += L"setPastControlsVisible_(st.range==='past');\n";
            html += L"localSwitchPage_(st.range==='past'?'past':'now');\n";
            html += L"localSwitchSpace_(st.space,document.querySelector('[data-space=\"'+st.space+'\"]'));\n";
            html += L"}\n";

            html += L"function switchView_(view,btn){\n";
            html += L"const st=getState_();\n";
            html += L"st.view=normView_(view);\n";
            html += L"setActive_('[data-program-view]','active-view-kind','data-program-view',st.view,btn);\n";
            html += L"if(st.range==='past'&&st.pastDate){openPastDay_();return;}\n";
            html += L"localSwitchSpace_(st.space,document.querySelector('[data-space=\"'+st.space+'\"]'));\n";
            html += L"}\n";

            html += L"function openPastDay_(){\n";
            html += L"const st=getState_();\n";
            html += L"const selected=readSelectedPastDate_()||st.pastDate||getLatestPastDate_(st.space);\n";
            html += L"const d=toYmdCompact_(selected);\n";
            html += L"if(!d){clearFrame_('beforeFrame','beforeEmpty','beforeStandaloneLink','過去番組表の日付を選択してください。');return false;}\n";
            html += L"st.pastDate=d;\n";
            html += L"st.range='past';\n";
            html += L"setDateInputs_(d);\n";
            html += L"setPastControlsVisible_(true);\n";
            html += L"localSwitchPage_('past');\n";
            html += L"const url=buildPastViewerSrc_(st.space,d,st.view);\n";
            html += L"clearFrame_('currentFrame','currentEmpty','currentStandaloneLink','');\n";
            html += L"if(!url){\n";
            html += L"clearFrame_('beforeFrame','beforeEmpty','beforeStandaloneLink','選択した日付の過去番組表データがありません。先に過去アーカイブを生成してください。');\n";
            html += L"return false;\n";
            html += L"}\n";
            html += L"clearFrame_('beforeFrame','beforeEmpty','beforeStandaloneLink','読み込み中です...');\n";
            html += L"window.setTimeout(function(){bindFrame_(url,true,'beforeFrame','beforeEmpty','beforeStandaloneLink','過去番組表を読み込めませんでした。');},0);\n";
            html += L"return true;\n";
            html += L"}\n";

            html += L"function openPastDayFromPage_(){\n";
            html += L"const page=byId_('pastDatePickerInPage');\n";
            html += L"const top=byId_('pastDatePicker');\n";
            html += L"if(page&&top)top.value=page.value;\n";
            html += L"return openPastDay_();\n";
            html += L"}\n";

            html += L"function publishIndexCompatApi_(){\n";
            html += L"window.__epgIndexCompatSwitchSpace=localSwitchSpace_;\n";
            html += L"window.__epgIndexCompatSwitchPage=localSwitchPage_;\n";
            html += L"window.switchPage=localSwitchPage_;\n";
            html += L"window.switchSpace=localSwitchSpace_;\n";
            html += L"window.switchProgramRange=switchRange_;\n";
            html += L"window.switchProgramView=switchView_;\n";
            html += L"window.openPastDay=openPastDay_;\n";
            html += L"window.openPastDayFromPage=openPastDayFromPage_;\n";
            html += L"}\n";

            html += L"publishIndexCompatApi_();\n";

            html += L"window.addEventListener('DOMContentLoaded',function(){\n";
            html += L"publishIndexCompatApi_();\n";
            html += L"const st=getState_();\n";
            html += L"const latest=getLatestPastDate_(st.space);\n";
            html += L"if(latest&&!st.pastDate)st.pastDate=latest;\n";
            html += L"if(st.pastDate)setDateInputs_(st.pastDate);\n";
            html += L"try{switchRange_(st.range,document.querySelector('[data-program-range=\"'+st.range+'\"]'));}catch(_){ }\n";
            html += L"try{switchView_(st.view,document.querySelector('[data-program-view=\"'+st.view+'\"]'));}catch(_){ }\n";
            html += L"try{localSwitchSpace_(st.space,document.querySelector('[data-space=\"'+st.space+'\"]'));}catch(_){ }\n";
            html += L"}, {once:true});\n";

            html += L"})();\n";
            html += L"</script>\n";
        }

        std::wstring BuildFallbackIndexTemplate_()
        {
            std::wstring html;
            html.reserve(18000);

            html += L"<!DOCTYPE html>\n";
            html += L"<html lang=\"ja\">\n";
            html += L"<head>\n";
            html += L"<meta charset=\"UTF-8\">\n";
            html += L"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
            html += L"<title>{{PORTAL_TITLE}}</title>\n";
            html += L"<link rel=\"stylesheet\" href=\"{{PORTAL_CSS}}\">\n";
            html += L"<style>\n";
            AppendIndexBaseCss_(html);
            html += L"</style>\n";
            html += L"</head>\n";
            html += L"<body>\n";
            html += L"<div class=\"app\">\n";

            AppendIndexTopbarHtml_(html);
            AppendIndexSubbarHtml_(html);

            html += L"<div class=\"layout\" id=\"mainLayout\">\n";

            html += L"<aside class=\"panel\">";
            html += L"<h2>簡易表示設定</h2>";
            html += L"<div style=\"display:grid;gap:10px;\">";
            html += L"<label>表示日<input id=\"displayDate\" type=\"date\"></label>";
            html += L"<label>表示対象<select id=\"displayMode\">";
            html += L"<option value=\"now\">現在・今後</option>";
            html += L"<option value=\"past\">過去</option>";
            html += L"<option value=\"all\">全体</option>";
            html += L"</select></label>";
            html += L"<button type=\"button\" onclick=\"applyQuickView()\">表示を切替</button>";
            html += L"<button type=\"button\" onclick=\"openDetailSearch()\">詳細検索を開く</button>";
            html += L"</div>";
            html += L"<h3 style=\"margin-top:16px;\">検索状態</h3>";
            html += L"<div class=\"placeholder\" id=\"searchStatusBox\">検索前です。</div>";
            html += L"</aside>\n";

            html += L"<main>";

            html += L"<section id=\"page-now\" class=\"page-section active\"><div class=\"page-card\">";
            html += L"<h2>現在・今後の番組表</h2>";
            html += L"<p>選択中の受信枠のHTMLを表示します。</p>";
            html += L"<div style=\"display:flex;justify-content:flex-end;margin-bottom:8px;\">";
            html += L"<a id=\"currentStandaloneLink\" href=\"#\" target=\"_blank\" rel=\"noopener\" ";
            html += L"style=\"display:inline-block;padding:8px 12px;border-radius:8px;text-decoration:none;background:#2d6cdf;color:#fff;\">単独で開く</a>";
            html += L"</div>";
            html += L"<div class=\"viewer\"><iframe id=\"currentFrame\" title=\"現在・今後の番組表\" src=\"about:blank\" scrolling=\"no\"></iframe><div id=\"currentEmpty\" class=\"viewer-empty\"></div></div>";
            html += L"</div></section>";

            html += L"<section id=\"page-past\" class=\"page-section\"><div class=\"page-card\">";
            html += L"<h2>過去番組表</h2>";
            html += L"<p>過去番組表は日付を選択して、1日分だけ読み込みます。</p>";

            html += L"<div class=\"past-picker-panel\">";
            html += L"<div class=\"past-picker-row\">";
            html += L"<label>表示日 ";
            html += L"<input id=\"pastDatePickerInPage\" type=\"date\">";
            html += L"</label>";
            html += L"<button type=\"button\" onclick=\"openPastDayFromPage()\">この日付を表示</button>";
            html += L"</div>";
            html += L"<div class=\"past-picker-note\">";
            html += L"過去番組表は表示専用です。予約スイッチは無効です。";
            html += L"日付を選択すると、選択した1日分の時間割または一覧だけを読み込みます。";
            html += L"</div>";
            html += L"</div>";

            html += L"<div style=\"display:flex;justify-content:flex-end;margin-bottom:8px;\">";
            html += L"<a id=\"beforeStandaloneLink\" href=\"#\" target=\"_blank\" rel=\"noopener\" ";
            html += L"style=\"display:inline-block;padding:8px 12px;border-radius:8px;text-decoration:none;background:#2d6cdf;color:#fff;pointer-events:none;opacity:.45;\">単独で開く</a>";
            html += L"</div>";

            html += L"<div class=\"viewer\">";
            html += L"<iframe id=\"beforeFrame\" title=\"過去番組表\" src=\"about:blank\" scrolling=\"no\"></iframe>";
            html += L"<div id=\"beforeEmpty\" class=\"viewer-empty open\">日付を選択して過去番組表を表示してください。</div>";
            html += L"</div>";

            html += L"</div></section>";

            html += L"<section id=\"page-search\" class=\"page-section\"><div class=\"page-card\">";
            html += L"<h2>検索結果</h2>";
            html += L"<p>EPG検索データから抽出した候補番組を表示します。</p>";
            html += L"<div id=\"searchSummary\" class=\"search-summary\">検索条件を指定して検索実行してください。</div>";
            html += L"<div id=\"searchResultsWrap\" class=\"result-table-wrap\"><div class=\"placeholder\">まだ検索結果はありません。</div></div>";
            html += L"</div></section>";

            html += L"<section id=\"page-reservation\" class=\"page-section\"><div class=\"page-card\">";
            html += L"<h2>予約一覧</h2>";
            html += L"<p>tasks.ini と画面上の即時操作結果をまとめて表示します。</p>";
            html += L"<div id=\"reservationSummary\" class=\"reservation-summary\">予約情報の読み込み待ちです。</div>";
            html += L"<div id=\"reservationListWrap\" class=\"result-table-wrap reservation-table-wrap\"><div class=\"placeholder\" style=\"margin:12px;\">まだ予約はありません。</div></div>";
            html += L"</div></section>";

            html += L"<section id=\"page-auto-reserve\" class=\"page-section\"><div class=\"page-card\">";
            html += L"<h2>自動予約条件</h2>";
            html += L"<p>キーワード・局名・ジャンルなどの条件から自動予約ルールを管理します。</p>";

            html += L"<div style=\"display:flex;justify-content:flex-end;margin-bottom:8px;\">";
            html += L"<a id=\"autoReserveStandaloneLink\" href=\"epg_auto_reserve.html\" target=\"_blank\" rel=\"noopener\" ";
            html += L"style=\"display:inline-block;padding:8px 12px;border-radius:8px;text-decoration:none;background:#2d6cdf;color:#fff;\">単独で開く</a>";
            html += L"</div>";

            html += L"<div class=\"viewer\">";
            html += L"<iframe id=\"autoReserveFrame\" title=\"自動予約条件\" src=\"about:blank\"></iframe>";
            html += L"<div id=\"autoReserveEmpty\" class=\"viewer-empty open\">自動予約条件ページを読み込み待ちです。</div>";
            html += L"</div>";

            html += L"</div></section>";

            html += L"</main>\n";

            html += L"<aside class=\"panel\">";
            html += L"<h2>番組詳細</h2>";
            html += L"<div class=\"detail-box\">";
            html += L"<h3>選択番組</h3>";
            html += L"<p id=\"detailTitle\">選択された番組をここへ表示</p>";
            html += L"<p><strong>日付:</strong> <span id=\"detailDate\">--</span> (<span id=\"detailWeekday\">--</span>)</p>";
            html += L"<p><strong>時刻:</strong> <span id=\"detailTime\">--</span></p>";
            html += L"<p><strong>放送局:</strong> <span id=\"detailService\">--</span></p>";
            html += L"<p><strong>ジャンル:</strong> <span id=\"detailGenre\">--</span></p>";
            html += L"<div id=\"detailSummary\">検索結果から選択した番組を表示します。</div>";
            html += L"<div id=\"detailReservationState\" style=\"margin-top:8px;\">予約状態: 未確認</div>";
            html += L"<div id=\"detailActions\" class=\"detail-actions\" style=\"margin-top:10px;\"></div>";
            html += L"</div>";

            html += L"<div class=\"detail-box\" style=\"margin-top:12px;\">";
            html += L"<h3>検索サマリ</h3>";
            html += L"<p><strong>最新検索件数:</strong> <span id=\"detailSearchCount\">0</span></p>";
            html += L"<p><strong>予約件数:</strong> <span id=\"detailReservationCount\">0</span></p>";
            html += L"<p id=\"detailLastUpdate\">検索データ読み込み待ち</p>";
            html += L"</div>";
            html += L"</aside>\n";

            html += L"</div>\n";

            html += L"<div id=\"detailSearchOverlay\" class=\"search-overlay\" aria-hidden=\"true\">";
            html += L"<div class=\"search-dialog\">";
            html += L"<div class=\"search-head\"><h2 style=\"margin:0;\">詳細検索</h2><button type=\"button\" onclick=\"closeDetailSearch()\">閉じる</button></div>";
            html += L"<div class=\"search-body\">";

            html += L"<div class=\"search-row\">";
            html += L"<label>検索語<input id=\"detailKeyword\" type=\"text\" placeholder=\"番組名 / 概要 / 詳細\"></label>";
            html += L"<label>検索対象<select id=\"detailTarget\">";
            html += L"<option value=\"now\">現在・今後</option>";
            html += L"<option value=\"past\">過去</option>";
            html += L"<option value=\"all\">全体</option>";
            html += L"</select></label>";
            html += L"</div>";

            html += L"<div class=\"search-row\">";
            html += L"<label>開始日<input id=\"detailDateFrom\" type=\"date\"></label>";
            html += L"<label>終了日<input id=\"detailDateTo\" type=\"date\"></label>";
            html += L"</div>";

            html += L"<div class=\"grid-2\">";
            html += L"<div><label>ジャンル選択<select id=\"genreList\" class=\"listbox\" multiple>";
            html += L"<option>ニュース・報道</option>";
            html += L"<option>スポーツ</option>";
            html += L"<option>情報・ワイドショー</option>";
            html += L"<option>ドラマ</option>";
            html += L"<option>音楽</option>";
            html += L"<option>バラエティ</option>";
            html += L"<option>映画</option>";
            html += L"<option>アニメ・特撮</option>";
            html += L"<option>ドキュメンタリー</option>";
            html += L"<option>劇場・公演</option>";
            html += L"<option>趣味・教育</option>";
            html += L"<option>福祉</option>";
            html += L"</select></label></div>";

            html += L"<div><label>放送局選択<select id=\"serviceList\" class=\"listbox\" multiple></select></label></div>";
            html += L"</div>";

            html += L"<div style=\"display:flex;gap:8px;justify-content:flex-end;margin-top:12px;\">";
            html += L"<button type=\"button\" onclick=\"closeDetailSearch()\">キャンセル</button>";
            html += L"<button type=\"button\" onclick=\"runDetailSearch()\">検索実行</button>";
            html += L"</div>";

            html += L"</div></div></div>\n";

            html += L"<script src=\"past_data/manifest.js\"></script>\n";
            html += L"<script src=\"{{SEARCH_DATA_JS}}\"></script>\n";
            html += L"<script src=\"{{RESERVATION_DATA_JS}}\"></script>\n";
            html += L"{{RUNTIME_DATA_BLOCK}}\n";
            html += L"<script defer src=\"{{SWITCH_RUNTIME_JS}}\"></script>\n";
            AppendIndexCompatScript_(html);

            html += L"</body>\n";
            html += L"</html>\n";

            return html;
        }

        static std::wstring MakeAssetUrlWithVersion_(
            const std::filesystem::path& outputDir,
            std::wstring_view fileName);

        static bool ContainsWideText_(
            const std::wstring& text,
            std::wstring_view needle)
        {
            if (needle.empty()) {
                return false;
            }

            return text.find(std::wstring(needle)) != std::wstring::npos;
        }

        static std::size_t FindScriptTagStartContaining_(
            const std::wstring& html,
            std::wstring_view needle)
        {
            if (needle.empty()) {
                return std::wstring::npos;
            }

            const std::size_t hit = html.find(std::wstring(needle));
            if (hit == std::wstring::npos) {
                return std::wstring::npos;
            }

            const std::size_t tagStart = html.rfind(L"<script", hit);
            if (tagStart == std::wstring::npos) {
                return hit;
            }

            const std::size_t tagEnd = html.find(L'>', tagStart);
            if (tagEnd != std::wstring::npos && tagEnd < hit) {
                return hit;
            }

            return tagStart;
        }

        static void InsertBlockBeforeRuntimeOrBodyClose_(
            std::wstring& html,
            const std::wstring& block)
        {
            if (block.empty()) {
                return;
            }

            const std::size_t runtimePos =
                FindScriptTagStartContaining_(
                    html,
                    L"epg_portal_runtime.js");

            if (runtimePos != std::wstring::npos) {
                html.insert(runtimePos, block);
                return;
            }

            const std::size_t bodyClose = html.rfind(L"</body>");
            if (bodyClose != std::wstring::npos) {
                html.insert(bodyClose, block);
                return;
            }

            html += L"\n";
            html += block;
        }

        static std::wstring GetOutputFileNameText_(
            const std::filesystem::path& p,
            const wchar_t* fallbackName)
        {
            std::wstring name = p.filename().wstring();
            if (name.empty()) {
                name = p.wstring();
            }
            if (name.empty() && fallbackName != nullptr) {
                name = fallbackName;
            }
            return name;
        }

        static std::wstring BuildVersionedScriptTag_(
            const EpgHtmlPortal::WriteOptions& opt,
            const std::filesystem::path& fileNamePath,
            const wchar_t* fallbackName,
            bool defer)
        {
            const std::wstring fileName =
                GetOutputFileNameText_(fileNamePath, fallbackName);

            if (fileName.empty()) {
                return {};
            }

            const std::wstring src =
                MakeAssetUrlWithVersion_(
                    opt.outputDir,
                    fileName);

            if (src.empty()) {
                return {};
            }

            std::wstring tag;
            tag.reserve(src.size() + 48);
            tag += L"<script ";
            if (defer) {
                tag += L"defer ";
            }
            tag += L"src=\"";
            tag += EpgHtmlPortal_Common::EscapeHtmlW(src);
            tag += L"\"></script>\n";
            return tag;
        }

        static std::wstring ExtractRuntimeBootstrapBlock_(
            std::wstring_view runtimeBlock)
        {
            std::wstring block(runtimeBlock);
            if (block.empty()) {
                return {};
            }

            std::size_t scriptPos = block.find(L"<script src=\"");
            if (scriptPos == std::wstring::npos) {
                scriptPos = block.find(L"<script defer src=\"");
            }

            if (scriptPos == std::wstring::npos) {
                return block;
            }

            return block.substr(0, scriptPos);
        }

        static bool HasRuntimeBootstrapData_(
            const std::wstring& rendered)
        {
            return ContainsWideText_(rendered, L"const DEFAULT_SPACE") ||
                ContainsWideText_(rendered, L"window.EPG_RUNTIME") ||
                ContainsWideText_(rendered, L"const SPACE_DATA") ||
                ContainsWideText_(rendered, L"window.CONFIG_SERVICE_DATA");
        }

        static void EnsureRequiredIndexDataScripts_(
            const EpgHtmlPortal::WriteOptions& opt,
            std::wstring& rendered)
        {
            std::wstring insertBlock;

            const std::wstring searchDataFile =
                GetOutputFileNameText_(
                    opt.searchDataJsFileName,
                    L"epg_search_data.js");

            const bool hasSearchData =
                ContainsWideText_(rendered, searchDataFile) ||
                ContainsWideText_(rendered, L"EPG_SEARCH_DATA");

            if (!hasSearchData) {
                insertBlock += BuildVersionedScriptTag_(
                    opt,
                    opt.searchDataJsFileName,
                    L"epg_search_data.js",
                    false);

                DebugLogFormat(
                    L"[EPG-PORTAL][INDEX] search data script injected: %ls",
                    searchDataFile.c_str());
            }

            const std::wstring reservationDataFile =
                GetOutputFileNameText_(
                    opt.reservationDataJsFileName,
                    L"reservation_data.js");

            const bool hasReservationData =
                ContainsWideText_(rendered, reservationDataFile) ||
                ContainsWideText_(rendered, L"EPG_RESERVATION_DATA");

            if (!hasReservationData) {
                insertBlock += BuildVersionedScriptTag_(
                    opt,
                    opt.reservationDataJsFileName,
                    L"reservation_data.js",
                    false);

                DebugLogFormat(
                    L"[EPG-PORTAL][INDEX] reservation data script injected: %ls",
                    reservationDataFile.c_str());
            }

            InsertBlockBeforeRuntimeOrBodyClose_(rendered, insertBlock);
        }

        static void EnsureRuntimeBootstrapBlock_(
            std::wstring& rendered,
            std::wstring_view runtimeBlock)
        {
            if (HasRuntimeBootstrapData_(rendered)) {
                return;
            }

            const std::wstring bootstrapBlock =
                ExtractRuntimeBootstrapBlock_(runtimeBlock);

            InsertBlockBeforeRuntimeOrBodyClose_(rendered, bootstrapBlock);

            DebugLog(
                L"[EPG-PORTAL][INDEX] runtime bootstrap data injected because template lacked RUNTIME_DATA_BLOCK");
        }

        static std::wstring MakeAssetUrlWithVersion_(
            const std::filesystem::path& outputDir,
            std::wstring_view fileName)
        {
            std::wstring name(fileName);
            if (name.empty()) {
                return {};
            }

            std::uintmax_t version = 0;

            try {
                const std::filesystem::path p =
                    outputDir / std::filesystem::path(name);
                std::error_code ec;

                if (std::filesystem::exists(p, ec) && !ec) {
                    const std::uintmax_t size =
                        std::filesystem::file_size(p, ec);
                    if (!ec) {
                        version ^= size;
                    }

                    const auto wt = std::filesystem::last_write_time(p, ec);
                    if (!ec) {
                        const auto raw = wt.time_since_epoch().count();
                        const auto mixed =
                            raw < 0
                            ? static_cast<std::uintmax_t>(-raw)
                            : static_cast<std::uintmax_t>(raw);

                        version ^= mixed;
                    }
                }
            }
            catch (...) {
                version = 0;
            }

            if (version == 0) {
                version = 1;
            }

            return name + L"?v=" + std::to_wstring(version);
        }

        std::vector<EpgHtmlPortal_Template::ReplaceEntry> BuildIndexReplacements_(
            const EpgHtmlPortal::WriteOptions& opt,
            std::wstring_view runtimeBlock)
        {
            std::vector<EpgHtmlPortal_Template::ReplaceEntry> repls;
            repls.reserve(6);

            repls.push_back({
                EpgHtmlPortal_Template::MakePlaceholder(L"PORTAL_TITLE"),
                EpgHtmlPortal_Common::EscapeHtmlW(opt.portalTitle)
                });

            repls.push_back({
                EpgHtmlPortal_Template::MakePlaceholder(L"PORTAL_CSS"),
                EpgHtmlPortal_Common::EscapeHtmlW(
                    MakeAssetUrlWithVersion_(opt.outputDir, L"portal.css"))
                });

            repls.push_back({
                EpgHtmlPortal_Template::MakePlaceholder(L"SEARCH_DATA_JS"),
                EpgHtmlPortal_Common::EscapeHtmlW(
                    MakeAssetUrlWithVersion_(
                        opt.outputDir,
                        opt.searchDataJsFileName.filename().wstring()))
                });

            repls.push_back({
                EpgHtmlPortal_Template::MakePlaceholder(L"RESERVATION_DATA_JS"),
                EpgHtmlPortal_Common::EscapeHtmlW(
                    MakeAssetUrlWithVersion_(
                        opt.outputDir,
                        opt.reservationDataJsFileName.filename().wstring()))
                });

            repls.push_back({
                EpgHtmlPortal_Template::MakePlaceholder(L"SWITCH_RUNTIME_JS"),
                EpgHtmlPortal_Common::EscapeHtmlW(
                    MakeAssetUrlWithVersion_(opt.outputDir, L"portal_runtime_switch.js"))
                });

            repls.push_back({
                EpgHtmlPortal_Template::MakePlaceholder(L"RUNTIME_DATA_BLOCK"),
                std::wstring(runtimeBlock)
                });

            return repls;
        }

        bool TryRenderIndexByTemplate_(
            const EpgHtmlPortal::WriteOptions& opt,
            std::wstring_view runtimeBlock,
            const std::filesystem::path& indexPath)
        {
            const auto searchRoots =
                EpgHtmlPortal_Assets::BuildDefaultTemplateSearchRoots();

            const auto templatePath =
                EpgHtmlPortal_Assets::FindTemplateAssetPath(
                    L"portal_index_template.html",
                    searchRoots);

            const auto repls = BuildIndexReplacements_(opt, runtimeBlock);

            if (!templatePath.empty()) {
                std::wstring rendered;
                if (!EpgHtmlPortal_Template::RenderTemplateFile(
                    templatePath,
                    repls,
                    rendered)) {
                    return false;
                }

                const std::wstring runtimeScriptName = L"epg_portal_runtime.js";
                const std::wstring switchScriptName = L"portal_runtime_switch.js";

                EnsureRequiredIndexDataScripts_(opt, rendered);
                EnsureRuntimeBootstrapBlock_(rendered, runtimeBlock);

                const bool hasRuntimeScriptRef =
                    rendered.find(runtimeScriptName) != std::wstring::npos;
                const bool hasSwitchScriptRef =
                    rendered.find(switchScriptName) != std::wstring::npos;

                if (!hasRuntimeScriptRef) {
                    const std::wstring block(runtimeBlock);
                    const std::size_t bodyClose = rendered.rfind(L"</body>");
                    if (bodyClose != std::wstring::npos) {
                        rendered.insert(bodyClose, block + L"\n");
                    }
                    else {
                        rendered += L"\n";
                        rendered += block;
                        rendered += L"\n";
                    }

                    DebugLogFormat(
                        L"[EPG-PORTAL][INDEX] runtime block injected because template lacked runtime script: %ls",
                        templatePath.wstring().c_str());
                }

                if (!hasSwitchScriptRef) {
                    const std::wstring switchTag =
                        BuildVersionedScriptTag_(
                            opt,
                            std::filesystem::path(L"portal_runtime_switch.js"),
                            L"portal_runtime_switch.js",
                            true);

                    const std::size_t bodyClose = rendered.rfind(L"</body>");
                    if (bodyClose != std::wstring::npos) {
                        rendered.insert(bodyClose, switchTag);
                    }
                    else {
                        rendered += L"\n";
                        rendered += switchTag;
                    }

                    DebugLogFormat(
                        L"[EPG-PORTAL][INDEX] switch runtime script injected because template lacked switch script ref: %ls",
                        templatePath.wstring().c_str());
                }

                return EpgHtmlPortal_Template::SaveRenderedText(indexPath, rendered);
            }

            std::wstring rendered;
            if (!EpgHtmlPortal_Template::RenderTemplateText(
                BuildFallbackIndexTemplate_(),
                repls,
                rendered)) {
                return false;
            }

            EnsureRequiredIndexDataScripts_(opt, rendered);
            EnsureRuntimeBootstrapBlock_(rendered, runtimeBlock);

            return EpgHtmlPortal_Template::SaveRenderedText(indexPath, rendered);
        }

        static void EnsurePastManifestStub_(
            const EpgHtmlPortal::WriteOptions& opt)
        {
            if (opt.outputDir.empty()) {
                return;
            }

            const std::filesystem::path manifestPath =
                opt.outputDir / L"past_data" / L"manifest.js";

            std::error_code ec;
            if (std::filesystem::exists(manifestPath, ec) && !ec) {
                return;
            }

            if (!EpgHtmlPortal_Common::EnsureParentDirectory(manifestPath)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][PAST][WARN] manifest directory create failed: %ls",
                    manifestPath.parent_path().wstring().c_str());
                return;
            }

            std::wstring js;
            js += L"window.EPG_PAST_MANIFEST = window.EPG_PAST_MANIFEST || {};\n";
            js += L"window.EPG_PAST_MANIFEST_READY = true;\n";

            if (EpgHtmlPortal_Common::WriteUtf8BomTextFile(manifestPath, js)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][PAST] empty manifest generated: %ls",
                    manifestPath.wstring().c_str());
            }
        }

        void EnsurePerSpacePlaceholders_(
            const EpgHtmlPortal::WriteOptions& opt)
        {
            for (int space = 0; space <= 2; ++space) {
                const auto entry =
                    EpgHtmlPortal_RuntimeData::BuildSpaceEntry(opt, space);

                if (!entry.beforePath.empty() &&
                    !std::filesystem::exists(entry.beforePath) &&
                    opt.generateBeforeStubIfMissing) {
                    EnsurePlaceholderHtml(
                        entry.beforePath,
                        entry.label + L" 過去番組表",
                        EpgHtmlPortal_RuntimeData::MakeMissingMessage(space, true));
                }

                if (!entry.currentPath.empty() &&
                    !std::filesystem::exists(entry.currentPath) &&
                    opt.generateSpaceStubIfMissing) {
                    EnsurePlaceholderHtml(
                        entry.currentPath,
                        entry.label + L" 番組表",
                        EpgHtmlPortal_RuntimeData::MakeMissingMessage(space, false));
                }
            }
        }
    }

    bool EnsurePlaceholderHtml(
        const std::filesystem::path& htmlPath,
        std::wstring_view title,
        std::wstring_view message)
    {
        if (htmlPath.empty()) {
            DebugLog(L"[EPG-PORTAL][ERROR] placeholder path is empty");
            return false;
        }

        if (!EpgHtmlPortal_Common::EnsureParentDirectory(htmlPath)) {
            DebugLogFormat(
                L"[EPG-PORTAL][ERROR] placeholder directory create failed: %ls",
                htmlPath.parent_path().wstring().c_str());
            return false;
        }

        const auto searchRoots =
            EpgHtmlPortal_Assets::BuildDefaultTemplateSearchRoots();

        const auto templatePath =
            EpgHtmlPortal_Assets::FindTemplateAssetPath(
                L"portal_placeholder_template.html",
                searchRoots);

        if (!templatePath.empty()) {
            std::vector<EpgHtmlPortal_Template::ReplaceEntry> repls;
            repls.reserve(2);

            repls.push_back({
                EpgHtmlPortal_Template::MakePlaceholder(L"PAGE_TITLE"),
                EpgHtmlPortal_Common::EscapeHtmlW(title)
                });

            repls.push_back({
                EpgHtmlPortal_Template::MakePlaceholder(L"PAGE_MESSAGE"),
                EpgHtmlPortal_Common::EscapeHtmlW(message)
                });

            const bool ok =
                EpgHtmlPortal_Template::RenderTemplateFileToOutput(
                    templatePath,
                    htmlPath,
                    repls);

            if (ok) {
                DebugLogFormat(
                    L"[EPG-PORTAL] placeholder generated(template): %ls",
                    htmlPath.wstring().c_str());
            }

            return ok;
        }

        const bool ok =
            EpgHtmlPortal_Common::WriteUtf8BomTextFile(
                htmlPath,
                BuildFallbackPlaceholderHtml_(title, message));

        if (ok) {
            DebugLogFormat(
                L"[EPG-PORTAL] placeholder generated(fallback): %ls",
                htmlPath.wstring().c_str());
        }

        return ok;
    }

    bool WritePortalIndexHtml(
        const EpgHtmlPortal::WriteOptions& opt)
    {
        if (opt.outputDir.empty()) {
            DebugLog(L"[EPG-PORTAL][ERROR] outputDir is empty");
            return false;
        }

        if (opt.currentHtmlPath.empty()) {
            DebugLog(L"[EPG-PORTAL][ERROR] currentHtmlPath is empty");
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(opt.outputDir, ec);
        if (ec) {
            DebugLogFormat(
                L"[EPG-PORTAL][ERROR] outputDir create failed: %ls",
                opt.outputDir.wstring().c_str());
            return false;
        }

        if (!EpgHtmlPortal_Assets::EnsureDefaultPortalAssets(opt)) {
            DebugLog(L"[EPG-PORTAL][WARN] portal asset の配置に失敗しました。");
        }

        EnsurePerSpacePlaceholders_(opt);
        EnsurePastManifestStub_(opt);

        const auto spaces =
            EpgHtmlPortal_RuntimeData::BuildKnownSpaces(opt);

        if (!EpgHtmlPortal_dynamic::WriteDynamicAssets(opt)) {
            DebugLog(L"[EPG-PORTAL][WARN] dynamic assets の生成に失敗しました。");
        }

        {
            const AutoReserve::RuleMatchResult autoMatch =
                EpgHtmlPortal_SearchData::RunAutoReserveMatchDryRunFromCurrentSearchData(
                    opt,
                    1000);

            DebugLogFormat(
                L"[EPG-PORTAL][AUTO-SEARCH][INDEX-DRYRUN] ok=%d rules=%zu programs=%zu matched=%zu candidate=%zu auto=%zu message=%ls",
                autoMatch.ok ? 1 : 0,
                autoMatch.ruleCount,
                autoMatch.programCount,
                autoMatch.matchedCount,
                autoMatch.candidateCount,
                autoMatch.autoRegisterCount,
                autoMatch.message.c_str());
        }

        bool autoReservePageOk = true;

        if (!EpgHtmlPortal_Auto::WriteAutoReservePage(opt)) {
            DebugLog(L"[EPG-PORTAL][WARN] 自動予約条件ページの生成に失敗しました。");
            autoReservePageOk = false;
        }

        if (!EpgHtmlPortal_Auto::WriteAutoReserveRuleDetailPage(opt)) {
            DebugLog(L"[EPG-PORTAL][WARN] 自動予約詳細ページの生成に失敗しました。");
            autoReservePageOk = false;
        }

        const std::wstring jsSpaceData =
            EpgHtmlPortal_RuntimeData::BuildSpaceDatasetJs(spaces);

        const std::wstring jsConfigServiceData =
            EpgHtmlPortal_RuntimeData::BuildConfigServiceDatasetJs(opt);

        const std::wstring runtimeBlock =
            EpgHtmlPortal_RuntimeData::BuildRuntimeScriptBlock(
                jsSpaceData,
                jsConfigServiceData,
                opt.space,
                MakeAssetUrlWithVersion_(opt.outputDir, L"epg_portal_runtime.js"));

        const std::filesystem::path indexPath =
            opt.outputDir / opt.indexFileName;

        const bool ok =
            TryRenderIndexByTemplate_(
                opt,
                runtimeBlock,
                indexPath);

        if (ok) {
            DebugLogFormat(
                L"[EPG-PORTAL] index generated: %ls",
                indexPath.wstring().c_str());
        }
        else {
            DebugLogFormat(
                L"[EPG-PORTAL][ERROR] index generate failed: %ls",
                indexPath.wstring().c_str());
        }

        return ok && autoReservePageOk;
    }
}
