#include "EpgHtmlPortal_SwitchScript.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <filesystem>
#include <string>

#include "DebugManager.h"
#include "EpgHtmlPortal_Common.hpp"
#include <functional>
#include <windows.h>
#include <atomic>

#ifndef EPG_PORTAL_SWITCH_CPP_DEBUG_DEFAULT
#define EPG_PORTAL_SWITCH_CPP_DEBUG_DEFAULT 1
#endif

inline std::atomic_bool g_EpgPortalSwitchCppDebug{
    EPG_PORTAL_SWITCH_CPP_DEBUG_DEFAULT != 0
};

#define EPG_PORTAL_SWITCH_CPP_LOG(fmt, ...) \
    do { \
        if (g_EpgPortalSwitchCppDebug.load(std::memory_order_relaxed)) { \
            DebugLogFormat(L"[EPG-SWITCH][CPP] " fmt, __VA_ARGS__); \
        } \
    } while (0)

#define EPG_PORTAL_SWITCH_CPP_LOG0(msg) \
    do { \
        if (g_EpgPortalSwitchCppDebug.load(std::memory_order_relaxed)) { \
            DebugLogFormat(L"[EPG-SWITCH][CPP] %ls", msg); \
        } \
    } while (0)

namespace EpgHtmlPortal_SwitchScript
{
    namespace
    {


        void AppendRuntimeSwitchScript_Core1_Text_(std::wstring& js)
        {
            js += LR"JS(
function normalizeText(s){
  return String(s || '').replace(/\u3000/g, ' ').replace(/\s+/g, ' ').trim();
}

function normalizeMode(mode){
  const v = String(mode || '').toLowerCase();
  return (v === 'watch') ? 'watch' : 'record';
}

function toSafeInt(v, defValue){
  const n = Number(v);
  return Number.isFinite(n) ? Math.trunc(n) : defValue;
}

function decodeHtmlEntitiesMinimal(s){
  let v = String(s || '');
  v = v.replace(/&amp;#(\d+);/g, function(_, n){
    const cp = Number(n);
    return Number.isFinite(cp) ? String.fromCodePoint(cp) : _;
  });
  v = v.replace(/&#(\d+);/g, function(_, n){
    const cp = Number(n);
    return Number.isFinite(cp) ? String.fromCodePoint(cp) : _;
  });
  v = v.replace(/&amp;#x([0-9a-fA-F]+);/g, function(_, h){
    const cp = parseInt(h, 16);
    return Number.isFinite(cp) ? String.fromCodePoint(cp) : _;
  });
  v = v.replace(/&#x([0-9a-fA-F]+);/g, function(_, h){
    const cp = parseInt(h, 16);
    return Number.isFinite(cp) ? String.fromCodePoint(cp) : _;
  });

  v = v.replace(/&nbsp;/gi, ' ');
  v = v.replace(/&amp;/gi, '&');
  v = v.replace(/&lt;/gi, '<');
  v = v.replace(/&gt;/gi, '>');
  v = v.replace(/&quot;/gi, '"');
  return v;
}
function getPortalLastResultsSafe_(){
  try {
    if (typeof window.__epgPortalGetLastResults === 'function') {
      const list = window.__epgPortalGetLastResults();
      return Array.isArray(list) ? list : [];
    }
  } catch (_) {
  }

  try {
    if (window.top &&
        window.top !== window &&
        typeof window.top.__epgPortalGetLastResults === 'function') {
      const list = window.top.__epgPortalGetLastResults();
      return Array.isArray(list) ? list : [];
    }
  } catch (_) {
  }

  try {
    if (Array.isArray(window.gLastResults)) {
      return window.gLastResults;
    }
  } catch (_) {
  }

  return [];
}

function getPortalSelectedProgramSafe_(){
  try {
    if (typeof window.__epgPortalGetSelectedProgram === 'function') {
      return window.__epgPortalGetSelectedProgram() || null;
    }
  } catch (_) {
  }

  try {
    if (window.top &&
        window.top !== window &&
        typeof window.top.__epgPortalGetSelectedProgram === 'function') {
      return window.top.__epgPortalGetSelectedProgram() || null;
    }
  } catch (_) {
  }

  return null;
}

function getPortalLastResultAtIndex_(index){
  const results = getPortalLastResultsSafe_();
  const n = Number(index);

  if (!Number.isFinite(n) || n < 0 || n >= results.length) {
    return null;
  }

  return results[Math.trunc(n)] || null;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_Time_(std::wstring& js)
        {
            js += LR"JS(
function normalizeTimeTextForReservation(timeText){
  return decodeHtmlEntitiesMinimal(timeText)
    .replace(/\u301C/g, '〜')
    .replace(/\uFF5E/g, '〜')
    .replace(/\u223C/g, '〜')
    .replace(/[‐-‒–—―ー－]/g, '-')
    .trim();
}

function normalizeTimeRangeForDisplay_(timeText){
  let s = normalizeTimeTextForReservation(timeText);

  s = s
    .replace(/？/g, '?')
    .replace(/～/g, '〜')
    .replace(/\s+/g, ' ')
    .trim();

  s = s.replace(
    /(\d{1,2}:\d{2})\s*[?~〜\-]\s*(\d{1,2}:\d{2})/g,
    '$1～$2'
  );

  return s;
}

function normalizeTimeRangeForStorage_(timeText){
  let s = normalizeTimeTextForReservation(timeText);

  s = s
    .replace(/？/g, '?')
    .replace(/[～〜?]/g, '-')
    .replace(/\s+/g, ' ')
    .trim();

  s = s.replace(
    /(\d{1,2}:\d{2})\s*-\s*(\d{1,2}:\d{2})/g,
    '$1-$2'
  );

  return s;
}

function formatIsoTime(hh, mm, ss){
  return String(hh).padStart(2, '0')
    + ':' + String(mm).padStart(2, '0')
    + ':' + String(ss || 0).padStart(2, '0');
}

function parseTimeTextParts(timeText){
  const s = normalizeTimeTextForReservation(timeText);
  const m = /^(\d{1,2}):(\d{2})(?:\s*[～〜\-]\s*(\d{1,2}):(\d{2}))?/.exec(s);
  if (!m) {
    return {
      startTime:'',
      endTime:'',
      durationSec:0,
      crossesDay:false
    };
  }

  const sh = Number(m[1]);
  const sm = Number(m[2]);
  const eh = (m[3] != null) ? Number(m[3]) : sh;
  const em = (m[4] != null) ? Number(m[4]) : sm;

  let durationMin = 0;
  let crossesDay = false;

  if (m[3] != null) {
    durationMin = (eh * 60 + em) - (sh * 60 + sm);
    if (durationMin < 0) {
      durationMin += 24 * 60;
      crossesDay = true;
    }
  }

  return {
    startTime: formatIsoTime(sh, sm, 0),
    endTime: formatIsoTime(eh, em, 0),
    durationSec: durationMin > 0 ? durationMin * 60 : 0,
    crossesDay: crossesDay
  };
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_Date_(std::wstring& js)
        {
            js += LR"JS(
function normalizeDateTextForReservation(dateText){
  return decodeHtmlEntitiesMinimal(dateText).trim();
}

function formatIsoDate(y, m, d){
  const yy = String(y).padStart(4, '0');
  const mm = String(m).padStart(2, '0');
  const dd = String(d).padStart(2, '0');
  return yy + '-' + mm + '-' + dd;
}

function shiftIsoDate(isoDate, deltaDays){
  const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(String(isoDate || ''));
  if (!m) return '';
  const dt = new Date(Number(m[1]), Number(m[2]) - 1, Number(m[3]));
  if (!Number.isFinite(dt.getTime())) return '';
  dt.setDate(dt.getDate() + deltaDays);
  return formatIsoDate(dt.getFullYear(), dt.getMonth() + 1, dt.getDate());
}

function parseDateTextToIso(dateText){
  const s = normalizeDateTextForReservation(dateText);
  const m = /^(\d{4})[\/\-](\d{1,2})[\/\-](\d{1,2})/.exec(s);
  if (!m) return '';
  return formatIsoDate(Number(m[1]), Number(m[2]), Number(m[3]));
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_Config_(std::wstring& js)
        {
            js += LR"JS(
function resolveCurrentSpaceForReservation_(payload){
  try {
    if (payload && payload.space != null) {
      const n = Number(payload.space);
      if (Number.isFinite(n)) return Math.trunc(n);
    }
  } catch (_) {
  }

  try {
    if (typeof window !== 'undefined' && typeof window.gCurrentSpace !== 'undefined') {
      const n = Number(window.gCurrentSpace);
      if (Number.isFinite(n)) return Math.trunc(n);
    }
  } catch (_) {
  }

  try {
    if (window.EPG_RUNTIME && window.EPG_RUNTIME.defaultSpace != null) {
      const n = Number(window.EPG_RUNTIME.defaultSpace);
      if (Number.isFinite(n)) return Math.trunc(n);
    }
  } catch (_) {
  }

  try {
    if (window.EPG_PROGRAM_PAGE_META && window.EPG_PROGRAM_PAGE_META.space != null) {
      const n = Number(window.EPG_PROGRAM_PAGE_META.space);
      if (Number.isFinite(n)) return Math.trunc(n);
    }
  } catch (_) {
  }

  try {
    if (document.body && document.body.dataset && document.body.dataset.epgSpace != null) {
      const n = Number(document.body.dataset.epgSpace);
      if (Number.isFinite(n)) return Math.trunc(n);
    }
  } catch (_) {
  }

  try {
    if (window.top && window.top !== window) {
      if (typeof window.top.gCurrentSpace !== 'undefined') {
        const n = Number(window.top.gCurrentSpace);
        if (Number.isFinite(n)) return Math.trunc(n);
      }

      if (window.top.EPG_RUNTIME && window.top.EPG_RUNTIME.defaultSpace != null) {
        const n = Number(window.top.EPG_RUNTIME.defaultSpace);
        if (Number.isFinite(n)) return Math.trunc(n);
      }
    }
  } catch (_) {
  }

  return 0;
}

function getConfiguredPreferredTunerIndex(){
  const raw =
    (window.EPG_RUNTIME && window.EPG_RUNTIME.reservationPreferredTuner != null)
      ? Number(window.EPG_RUNTIME.reservationPreferredTuner)
      : -1;

  return Number.isFinite(raw) ? Math.trunc(raw) : -1;
}

function askPreferredTunerIndexForReservation(mode){
  const configured = getConfiguredPreferredTunerIndex();

  if (configured === -2) {
    const label = (mode === 'record') ? '録画' : '視聴';
    const raw = window.prompt(
      label + '予約の優先チューナー番号を入力してください。\n-1 または空欄で自動にします。',
      '-1'
    );

    if (raw === null) return null;

    const text = String(raw).trim();
    if (!text || text === '-1') return -1;

    const v = Number(text);
    if (!Number.isFinite(v) || v < 0) return -1;

    return Math.floor(v);
  }

  if (configured >= 0) {
    return configured;
  }

  return -1;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_BackendAccess_(std::wstring& js)
        {
            js += LR"JS(
function getNativeReservationBackend(){
  try {
    if (window.__epgPortalNativeReservationBackend) {
      return window.__epgPortalNativeReservationBackend;
    }
  } catch (_) {
  }

  try {
    if (window.top && window.top !== window &&
        window.top.__epgPortalNativeReservationBackend) {
      return window.top.__epgPortalNativeReservationBackend;
    }
  } catch (_) {
  }

  return null;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_Debug_(std::wstring& js)
        {
            js += LR"JS(
let gReservationSwitchDebugEnabled_ = false;
let gReservationSwitchFindDebugEnabled_ = false;

try {
  if (window.EPG_RUNTIME &&
      window.EPG_RUNTIME.reservationSwitchDebug === true) {
    gReservationSwitchDebugEnabled_ = true;
  }
} catch (_) {
}

function reservationSwitchCompactEntry_(entry){
  if (!entry || typeof entry !== 'object') {
    return null;
  }

  return {
    id: String(entry.id || ''),
    label: String(entry.label || entry.title || entry.titleLabel || ''),
    mode: String(entry.mode || entry.reserveMode || entry.taskType || ''),
    enabled: entry.enabled,
    dedupeKey: String(entry.dedupeKey || ''),
    reserveKey: String(entry.reserveKey || ''),
    fallbackKey: String(entry.fallbackKey || ''),
    serviceId: entry.serviceId,
    eventId: entry.eventId,
    startDate: entry.startDate,
    startTime: entry.startTime
  };
}

function reservationSwitchCompactPayload_(payload){
  if (!payload || typeof payload !== 'object') {
    return null;
  }

  return {
    title: String(payload.title || payload.titleLabel || ''),
    service: String(payload.serviceName || payload.service || ''),
    serviceId: payload.serviceId,
    eventId: payload.eventId,
    startDate: payload.startDate,
    startTime: payload.startTime,
    time: payload.time || payload.timeText || payload.displayTimeText || '',
    mode: payload.mode || payload.reserveMode || ''
  };
}

function reservationSwitchCompactResult_(result){
  if (!result || typeof result !== 'object') {
    return null;
  }

  return {
    ok: result.ok,
    source: result.source,
    reason: result.reason,
    status: result.status,
    path: result.path,
    items: Array.isArray(result.items) ? result.items.length : -1,
    reloadOk: result.reload ? result.reload.ok : undefined,
    reloadItems: result.reload && Array.isArray(result.reload.items)
      ? result.reload.items.length
      : undefined
  };
}

function reservationSwitchStateSummary_(){
  let viewCache = null;
  try {
    if (typeof getReservationViewCacheDebugInfo_ === 'function') {
      viewCache = getReservationViewCacheDebugInfo_();
    }
  } catch (_) {
  }

  let listLength = -1;
  try {
    if (typeof getSharedReservationList_ === 'function') {
      const list = getSharedReservationList_();
      listLength = Array.isArray(list) ? list.length : -1;
    }
  } catch (_) {
  }

  let overlaySize = -1;
  try {
    overlaySize = gReservationMutationOverlay
      ? gReservationMutationOverlay.size
      : -1;
  } catch (_) {
  }

  return {
    localHolding: (typeof isReservationLocalMutationHolding_ === 'function')
      ? isReservationLocalMutationHolding_()
      : false,
    overlaySize: overlaySize,
    listLength: listLength,
    viewCache: viewCache
  };
}

function reservationSwitchDebug_(tag, data){
  if (!gReservationSwitchDebugEnabled_) {
    return;
  }

  try {
    console.log('[EPG-SWITCH]', Object.assign({
      tag: String(tag || ''),
      stamp: Date.now()
    }, data || {}, {
      state: reservationSwitchStateSummary_()
    }));
  } catch (_) {
  }
}

function reservationSwitchFindDebug_(tag, data){
  if (!gReservationSwitchDebugEnabled_ ||
      !gReservationSwitchFindDebugEnabled_) {
    return;
  }

  reservationSwitchDebug_(tag, data);
}

window.__epgPortalSetReservationSwitchDebug = function(enabled){
  gReservationSwitchDebugEnabled_ = !!enabled;
  return gReservationSwitchDebugEnabled_;
};

window.__epgPortalSetReservationSwitchFindDebug = function(enabled){
  gReservationSwitchFindDebugEnabled_ = !!enabled;
  return gReservationSwitchFindDebugEnabled_;
};

window.__epgPortalGetReservationSwitchDebug = function(){
  return {
    debugEnabled: !!gReservationSwitchDebugEnabled_,
    findDebugEnabled: !!gReservationSwitchFindDebugEnabled_
  };
};

window.__epgPortalDumpReservationSwitchState = function(){
  return Object.assign({
    debugEnabled: !!gReservationSwitchDebugEnabled_,
    findDebugEnabled: !!gReservationSwitchFindDebugEnabled_
  }, reservationSwitchStateSummary_());
};
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_TraceBase_(std::wstring& js)
        {
            js += LR"JS(
let gEpgPortalTraceEnabled_ = false;
let gEpgPortalTraceSeq_ = 0;
let gEpgPortalTraceLog_ = [];
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_TraceCompact_(std::wstring& js)
        {
            js += LR"JS(
function epgPortalTraceCompact_(data){
  if (!data || typeof data !== 'object') {
    return data;
  }

  const out = {};

  Object.keys(data).forEach(key => {
    const value = data[key];

    if (value === undefined) {
      return;
    }

    if (value === null ||
        typeof value === 'string' ||
        typeof value === 'number' ||
        typeof value === 'boolean') {
      out[key] = value;
      return;
    }

    if (Array.isArray(value)) {
      out[key] = {
        type:'array',
        length:value.length
      };
      return;
    }

    if (typeof value === 'object') {
      const compact = reservationSwitchCompactEntry_(value);
      out[key] = compact || String(value);
    }
  });

  return out;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_TracePush_(std::wstring& js)
        {
            js += LR"JS(
function epgPortalTracePush_(area, tag, data){
  if (!gEpgPortalTraceEnabled_) {
    return null;
  }

  const item = {
    seq: ++gEpgPortalTraceSeq_,
    t: Math.round(performance.now() * 100) / 100,
    stamp: Date.now(),
    area: String(area || ''),
    tag: String(tag || ''),
    data: epgPortalTraceCompact_(data || {})
  };

  gEpgPortalTraceLog_.push(item);

  if (gEpgPortalTraceLog_.length > 1000) {
    gEpgPortalTraceLog_.splice(0, gEpgPortalTraceLog_.length - 1000);
  }

  try {
    console.log('[EPG-TRACE]', item);
  } catch (_) {
  }

  return item;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_TracePublic_(std::wstring& js)
        {
            js += LR"JS(
window.__epgPortalTraceEnable = function(enabled){
  gEpgPortalTraceEnabled_ = !!enabled;
  return gEpgPortalTraceEnabled_;
};

window.__epgPortalTraceClear = function(){
  gEpgPortalTraceSeq_ = 0;
  gEpgPortalTraceLog_ = [];
  return true;
};

window.__epgPortalTraceDump = function(){
  return gEpgPortalTraceLog_.slice();
};

window.__epgPortalTraceTable = function(){
  const rows = gEpgPortalTraceLog_.map(x => ({
    seq:x.seq,
    t:x.t,
    area:x.area,
    tag:x.tag,
    data:JSON.stringify(x.data)
  }));

  console.table(rows);
  return gEpgPortalTraceLog_.slice();
};

window.__epgPortalTracePush = epgPortalTracePush_;
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_Trace_(std::wstring& js)
        {
            AppendRuntimeSwitchScript_Core1_TraceBase_(js);
            AppendRuntimeSwitchScript_Core1_TraceCompact_(js);
            AppendRuntimeSwitchScript_Core1_TracePush_(js);
            AppendRuntimeSwitchScript_Core1_TracePublic_(js);
        }

        void AppendRuntimeSwitchScript_Core1_ListState_Base_(std::wstring& js)
        {
            js += LR"JS(
function getSharedReservationList_(){
  if (typeof window.__epgPortalGetReservationList === 'function') {
    const list = window.__epgPortalGetReservationList();
    return Array.isArray(list) ? list : [];
  }

  if (!Array.isArray(window.gReservationList)) {
    window.gReservationList = [];
  }
  return window.gReservationList;
}

function normalizeReservationEntryForDisplay_(entry){
  if (!entry || typeof entry !== 'object') {
    return entry;
  }

  const next = Object.assign({}, entry);

  const rawTime =
    next.displayTimeText ||
    next.timeText ||
    next.time ||
    '';

  const displayTime = normalizeTimeRangeForDisplay_(rawTime);

  if (displayTime) {
    next.displayTimeText = displayTime;
    next.timeText = displayTime;
    next.time = displayTime;
  }

  return next;
}

function setSharedReservationList_(list){
  const next = Array.isArray(list)
    ? list.map(normalizeReservationEntryForDisplay_)
    : [];

  if (typeof window.__epgPortalSetReservationList === 'function') {
    window.__epgPortalSetReservationList(next, Date.now());
  } else {
    window.gReservationList = next;
  }

  return next;
}

function getReservationListRef(){
  return getSharedReservationList_();
}

let gReservationStateCache = null;

function invalidateReservationStateCache(){
  gReservationStateCache = null;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ListState_KeyNormalize_(std::wstring& js)
        {
            js += LR"JS(
function normalizeReservationProgramMode_(mode){
  const v = String(mode || '').toLowerCase().trim();
  if (v === 'watch') return 'watch';
  if (v === 'record') return 'record';
  return '';
}

function makeReservationStateKey(dedupeKey, mode){
  const k = String(dedupeKey || '');
  const m = normalizeReservationProgramMode_(mode);
  if (!k || !m) return '';
  return k + '|' + m;
}

function pushReservationStateKeyUnique_(keys, key){
  const k = String(key || '');
  if (!k) return;
  if (keys.indexOf(k) >= 0) return;
  keys.push(k);
}

function normalizeReservationCompareText_(s){
  let v = String(s || '');

  try {
    v = v.normalize('NFKC');
  } catch (_) {
  }

  return normalizeText(v)
    .toLowerCase()
    .replace(/\u3000/g, ' ')
    .replace(/\s+/g, '')
    .trim();
}

function normalizeReservationDateKey_(value){
  const raw = normalizeDateTextForReservation(value || '');
  const iso = parseDateTextToIso(raw);

  if (iso) {
    return iso;
  }

  const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(raw);
  if (m) {
    return raw;
  }

  return raw.slice(0, 10);
}

function normalizeReservationStartTimeKey_(value){
  const raw = normalizeTimeTextForReservation(value || '');
  const m = /(\d{1,2}):(\d{2})(?::(\d{2}))?/.exec(raw);

  if (!m) {
    return '';
  }

  return String(Number(m[1])).padStart(2, '0')
    + ':'
    + String(Number(m[2])).padStart(2, '0');
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ListState_EventKeys_(std::wstring& js)
        {
            js += LR"JS(
function makeReservationEventStateKey_(space, networkId, tsId, serviceId, eventId, mode){
  const m = normalizeReservationProgramMode_(mode);
  const sp = toSafeInt(space, 0);
  const nid = toSafeInt(networkId, 0);
  const tid = toSafeInt(tsId, 0);
  const sid = toSafeInt(serviceId, 0);
  const eid = toSafeInt(eventId, 0);

  if (!m || nid <= 0 || tid <= 0 || sid <= 0 || eid <= 0) {
    return '';
  }

  return 'event|' + sp + '-' + nid + '-' + tid + '-' + sid + '-' + eid + '|' + m;
}

function makeReservationEventLooseStateKey_(space, serviceId, eventId, mode){
  const m = normalizeReservationProgramMode_(mode);
  const sp = toSafeInt(space, 0);
  const sid = toSafeInt(serviceId, 0);
  const eid = toSafeInt(eventId, 0);

  if (!m || sid <= 0 || eid <= 0) {
    return '';
  }

  return 'event-loose|' + sp + '-' + sid + '-' + eid + '|' + m;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ListState_TimeKeys_(std::wstring& js)
        {
            js += LR"JS(
function makeReservationTimeStateKey_(serviceKey, startDate, startTime, title, mode){
  const m = normalizeReservationProgramMode_(mode);
  const sk = String(serviceKey || '');
  const sd = String(startDate || '');
  const st = String(startTime || '');
  const tt = normalizeText(title || '');

  if (!m || !sk || !sd || !st || !tt) {
    return '';
  }

  return 'time|' + sk + '|' + sd + '|' + st + '|' + tt + '|' + m;
}

function makeReservationSoftTimeStateKey_(serviceText, startDate, startTime, title, mode){
  const m = normalizeReservationProgramMode_(mode);
  const service = normalizeReservationCompareText_(serviceText || '');
  const date = normalizeReservationDateKey_(startDate || '');
  const time = normalizeReservationStartTimeKey_(startTime || '');
  const titleKey = normalizeReservationCompareText_(title || '');

  if (!m || !service || !date || !time || !titleKey) {
    return '';
  }

  return 'time-soft|' + service + '|' + date + '|' + time + '|' + titleKey + '|' + m;
}

function collectReservationServiceTexts_(entry){
  const out = [];

  function add(v){
    const s = String(v || '');
    if (!s) return;
    if (out.indexOf(s) >= 0) return;
    out.push(s);
  }

  if (entry) {
    add(entry.serviceName);
    add(entry.service);
    add(entry.target);
    add(entry.channelName);
  }

  return out;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ListState_EntryKeys_(std::wstring& js)
        {
            js += LR"JS(
function makeReservationEntryStateKeys_(entry){
  const keys = [];

  if (!entry) {
    return keys;
  }

  const mode = normalizeReservationProgramMode_(
    entry.mode || entry.reserveMode || entry.taskType || ''
  );

  if (!mode) {
    return keys;
  }

  const dedupeKey = String(entry.dedupeKey || entry.id || '');

  if (dedupeKey) {
    pushReservationStateKeyUnique_(
      keys,
      makeReservationStateKey(dedupeKey, mode)
    );
  }

  pushReservationStateKeyUnique_(
    keys,
    makeReservationEventStateKey_(
      entry.space,
      entry.networkId,
      entry.tsId,
      entry.serviceId,
      entry.eventId,
      mode
    )
  );

  pushReservationStateKeyUnique_(
    keys,
    makeReservationEventLooseStateKey_(
      entry.space,
      entry.serviceId,
      entry.eventId,
      mode
    )
  );

  pushReservationStateKeyUnique_(
    keys,
    makeReservationTimeStateKey_(
      entry.serviceKey,
      entry.startDate || entry.date || entry.dateText,
      entry.startTime,
      entry.label || entry.title || entry.titleLabel,
      mode
    )
  );
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ListState_EntryKeysSoft_(std::wstring& js)
        {
            js += LR"JS(
  const serviceTexts = collectReservationServiceTexts_(entry);
  const dateText = entry.startDate || entry.date || entry.dateText;
  const timeText =
    entry.startTime ||
    entry.time ||
    entry.timeText ||
    entry.displayTimeText;

  const titleText = entry.label || entry.title || entry.titleLabel;

  serviceTexts.forEach(serviceText => {
    pushReservationStateKeyUnique_(
      keys,
      makeReservationSoftTimeStateKey_(
        serviceText,
        dateText,
        timeText,
        titleText,
        mode
      )
    );
  });

  return keys.filter(Boolean);
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ListState_PayloadKeys_(std::wstring& js)
        {
            js += LR"JS(
function makeReservationPayloadStateKeys_(payload, mode){
  const keys = [];

  if (!payload) {
    return keys;
  }

  const fixedMode = normalizeReservationProgramMode_(mode);
  if (!fixedMode) {
    return keys;
  }

  const p = buildProgramReservationPayload(payload, fixedMode, -1, null);

  if (p.dedupeKey) {
    pushReservationStateKeyUnique_(
      keys,
      makeReservationStateKey(p.dedupeKey, fixedMode)
    );
  }

  pushReservationStateKeyUnique_(
    keys,
    makeReservationEventStateKey_(
      p.space,
      p.networkId,
      p.tsId,
      p.serviceId,
      p.eventId,
      fixedMode
    )
  );

  pushReservationStateKeyUnique_(
    keys,
    makeReservationEventLooseStateKey_(
      p.space,
      p.serviceId,
      p.eventId,
      fixedMode
    )
  );
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ListState_PayloadKeysTime_(std::wstring& js)
        {
            js += LR"JS(
  pushReservationStateKeyUnique_(
    keys,
    makeReservationTimeStateKey_(
      p.serviceKey,
      p.startDate,
      p.startTime,
      p.title || p.titleLabel,
      fixedMode
    )
  );

  const serviceTexts = collectReservationServiceTexts_(p);
  const dateText = p.startDate || p.dateText || p.date;
  const timeText =
    p.startTime ||
    p.displayTimeText ||
    p.time ||
    p.timeText;

  const titleText = p.title || p.titleLabel;

  serviceTexts.forEach(serviceText => {
    pushReservationStateKeyUnique_(
      keys,
      makeReservationSoftTimeStateKey_(
        serviceText,
        dateText,
        timeText,
        titleText,
        fixedMode
      )
    );
  });

  return keys.filter(Boolean);
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ListState_StateCache_(std::wstring& js)
        {
            js += LR"JS(
function rebuildReservationStateCache(){
  const map = new Map();
  const list = getSharedReservationList_();

  list.forEach(entry => {
    const keys = makeReservationEntryStateKeys_(entry);
    keys.forEach(key => {
      if (key && !map.has(key)) {
        map.set(key, entry);
      }
    });
  });

  gReservationStateCache = map;
  return map;
}

function getReservationStateCache(){
  return gReservationStateCache || rebuildReservationStateCache();
}

let gReservationMutationOverlay = new Map();

let gReservationLocalMutationStamp_ = 0;
let gReservationLocalMutationHoldMs_ = 15000;

function markReservationLocalMutation_(){
  gReservationLocalMutationStamp_ = Date.now();
}

function isReservationLocalMutationHolding_(){
  if (!gReservationLocalMutationStamp_) {
    return false;
  }

  return (Date.now() - gReservationLocalMutationStamp_) < gReservationLocalMutationHoldMs_;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ListState_OverlayBase_(std::wstring& js)
        {
            js += LR"JS(
function cleanupReservationMutationOverlay_(){
  const now = Date.now();

  gReservationMutationOverlay.forEach((value, key) => {
    if (!value || value.expiresAt <= now) {
      gReservationMutationOverlay.delete(key);
    }
  });
}

function rememberReservationMutationEntry_(entry){
  if (!entry) {
    return;
  }

  cleanupReservationMutationOverlay_();

  const normalized = normalizeReservationEntryForDisplay_(entry);
  const expiresAt = Date.now() + 30000;
  const keys = makeReservationEntryStateKeys_(normalized);

  keys.forEach(key => {
    gReservationMutationOverlay.set(key, {
      kind:'entry',
      entry:normalized,
      expiresAt:expiresAt
    });
  });

  invalidateReservationStateCache();
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ListState_OverlayRemove_(std::wstring& js)
        {
            js += LR"JS(
function rememberReservationMutationRemove_(payload, mode, existingEntry){
  cleanupReservationMutationOverlay_();

  const expiresAt = Date.now() + 30000;
  const keys = [];

  makeReservationPayloadStateKeys_(payload, mode).forEach(key => {
    pushReservationStateKeyUnique_(keys, key);
  });

  if (existingEntry) {
    makeReservationEntryStateKeys_(existingEntry).forEach(key => {
      pushReservationStateKeyUnique_(keys, key);
    });
  }

  keys.filter(Boolean).forEach(key => {
    gReservationMutationOverlay.set(key, {
      kind:'removed',
      entry:null,
      expiresAt:expiresAt
    });
  });

  invalidateReservationStateCache();
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ListState_OverlayFind_(std::wstring& js)
        {
            js += LR"JS(
function getReservationMutationOverlayForPayload_(payload, mode){
  cleanupReservationMutationOverlay_();

  const keys = makeReservationPayloadStateKeys_(payload, mode);

  for (let i = 0; i < keys.length; ++i) {
    const hit = gReservationMutationOverlay.get(keys[i]);

    if (!hit) {
      continue;
    }

    if (hit.kind === 'removed') {
      return {
        hit:true,
        entry:null
      };
    }

    if (hit.kind === 'entry') {
      return {
        hit:true,
        entry:hit.entry || null
      };
    }
  }

  return {
    hit:false,
    entry:null
  };
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ListState_Result_(std::wstring& js)
        {
            js += LR"JS(
function applyReservationListResult(result){
  if (!result || !Array.isArray(result.items)) {
    return false;
  }

  setSharedReservationList_(result.items);
  invalidateReservationViewCache_();
  invalidateReservationStateCache();
  return true;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ListState_MirrorBase_(std::wstring& js)
        {
            js += LR"JS(
let gReservationListMirror_ = [];
let gReservationListMirrorStamp_ = 0;

function normalizeReservationListMirrorArray_(list){
  return Array.isArray(list)
    ? list.map(normalizeReservationEntryForDisplay_).filter(Boolean)
    : [];
}

function getOriginalReservationListGetter_(){
  const fn = window.__epgPortalGetReservationList;
  if (typeof fn !== 'function') {
    return null;
  }

  if (fn.__epgSwitchMirrorPatched && typeof fn.__epgSwitchOriginal === 'function') {
    return fn.__epgSwitchOriginal;
  }

  return fn;
}

function getOriginalReservationListSetter_(){
  const fn = window.__epgPortalSetReservationList;
  if (typeof fn !== 'function') {
    return null;
  }

  if (fn.__epgSwitchMirrorPatched && typeof fn.__epgSwitchOriginal === 'function') {
    return fn.__epgSwitchOriginal;
  }

  return fn;
}

function makeReservationListMergeKeys_(entry){
  const keys = [];
  if (!entry || typeof entry !== 'object') {
    return keys;
  }

  const mode = normalizeReservationListMode_(
    entry.mode || entry.reserveMode || entry.taskType || ''
  );

  const id = String(entry.id || '');
  const dedupeKey = String(entry.dedupeKey || '');
  const reserveKey = String(entry.reserveKey || '');
  const fallbackKey = String(entry.fallbackKey || '');

  if (id) keys.push('id|' + id);
  if (dedupeKey) keys.push('dedupe|' + mode + '|' + dedupeKey);
  if (reserveKey) keys.push('reserve|' + mode + '|' + reserveKey);
  if (fallbackKey) keys.push('fallback|' + mode + '|' + fallbackKey);

  const title = normalizeText(entry.label || entry.title || entry.titleLabel || '');
  const service = normalizeText(entry.serviceName || entry.service || entry.target || '');
  const startDate = String(entry.startDate || entry.date || entry.dateText || '');
  const startTime = String(entry.startTime || '');

  if (mode && title && service && startDate && startTime) {
    keys.push('time|' + mode + '|' + service + '|' + startDate + '|' + startTime + '|' + title);
  }

  return keys.filter(Boolean);
}

function mergeReservationEntryPreserve_(base, rich){
  const out = Object.assign({}, base || {});
  const src = rich || {};

  Object.keys(src).forEach(key => {
    const value = src[key];

    if (value === undefined || value === null) {
      return;
    }

    if (String(value) === '' &&
        out[key] !== undefined &&
        out[key] !== null &&
        String(out[key]) !== '') {
      return;
    }

    out[key] = value;
  });

  return normalizeReservationEntryForDisplay_(out);
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ListState_MirrorBridge_(std::wstring& js)
        {
            js += LR"JS(
function mergeReservationListPreserve_(baseList, richList){
  const base = Array.isArray(baseList) ? baseList : [];
  const rich = Array.isArray(richList) ? richList : [];

  const richMap = new Map();
  rich.forEach(item => {
    makeReservationListMergeKeys_(item).forEach(key => {
      if (key && !richMap.has(key)) {
        richMap.set(key, item);
      }
    });
  });

  const used = new Set();

  const out = base.map(item => {
    let hit = null;

    const keys = makeReservationListMergeKeys_(item);
    for (let i = 0; i < keys.length; ++i) {
      if (richMap.has(keys[i])) {
        hit = richMap.get(keys[i]);
        break;
      }
    }

    if (hit) {
      makeReservationListMergeKeys_(hit).forEach(key => used.add(key));
      return mergeReservationEntryPreserve_(item, hit);
    }

    return normalizeReservationEntryForDisplay_(item);
  });

  rich.forEach(item => {
    const keys = makeReservationListMergeKeys_(item);
    const alreadyUsed = keys.some(key => used.has(key));

    if (!alreadyUsed) {
      out.push(normalizeReservationEntryForDisplay_(item));
      keys.forEach(key => used.add(key));
    }
  });

  return out;
}

function getSharedReservationList_(){
  const getter = getOriginalReservationListGetter_();

  let base = [];
  if (getter) {
    try {
      const got = getter.call(window);
      base = Array.isArray(got) ? got : [];
    } catch (_) {
      base = [];
    }
  } else if (Array.isArray(window.gReservationList)) {
    base = window.gReservationList;
  }

  if (Array.isArray(gReservationListMirror_) && gReservationListMirror_.length > 0) {
  if (typeof isReservationLocalMutationHolding_ === 'function' &&
      isReservationLocalMutationHolding_()) {
    return gReservationListMirror_.slice();
  }

  return mergeReservationListPreserve_(base, gReservationListMirror_);
}

return base;
}

function setSharedReservationList_(list){
  const next = normalizeReservationListMirrorArray_(list);

  gReservationListMirror_ = next.slice();
  gReservationListMirrorStamp_ = Date.now();

  const setter = getOriginalReservationListSetter_();

  if (setter) {
    try {
      setter.call(window, next, gReservationListMirrorStamp_);
    } catch (e) {
      console.warn('[reservation-switch] original set reservation list failed', e);
      window.gReservationList = next;
    }
  } else {
    window.gReservationList = next;
  }

  installReservationListMirrorBridge_();
  return next;
}

function installReservationListMirrorBridge_(){
  const currentGet = window.__epgPortalGetReservationList;

  if (typeof currentGet === 'function' && !currentGet.__epgSwitchMirrorPatched) {
    const originalGet = currentGet;

    const wrappedGet = function(){
      let base = [];

      try {
        const got = originalGet.apply(this, arguments);
        base = Array.isArray(got) ? got : [];
      } catch (_) {
        base = [];
      }

      if (Array.isArray(gReservationListMirror_) && gReservationListMirror_.length > 0) {
  if (typeof isReservationLocalMutationHolding_ === 'function' &&
      isReservationLocalMutationHolding_()) {
    return gReservationListMirror_.slice();
  }

  return mergeReservationListPreserve_(base, gReservationListMirror_);
}

      return base;
    };

    wrappedGet.__epgSwitchMirrorPatched = true;
    wrappedGet.__epgSwitchOriginal = originalGet;
    window.__epgPortalGetReservationList = wrappedGet;
  }

  const currentSet = window.__epgPortalSetReservationList;

  if (typeof currentSet === 'function' && !currentSet.__epgSwitchMirrorPatched) {
    const originalSet = currentSet;

    const wrappedSet = function(list, stamp){
      const next = normalizeReservationListMirrorArray_(list);

      gReservationListMirror_ = next.slice();
gReservationListMirrorStamp_ = Number(stamp || Date.now());

      return originalSet.apply(this, arguments);
    };

    wrappedSet.__epgSwitchMirrorPatched = true;
    wrappedSet.__epgSwitchOriginal = originalSet;
    window.__epgPortalSetReservationList = wrappedSet;
  }
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ViewCache_Parse_(std::wstring& js)
        {
            js += LR"JS(
function unescapeReservationCacheField_(s){
  return String(s || '')
    .replace(/\\t/g, '\t')
    .replace(/\\r/g, '\r')
    .replace(/\\n/g, '\n')
    .replace(/\\\\/g, '\\');
}

function splitReservationCacheTsvLine_(line){
  return String(line || '').split('\t').map(unescapeReservationCacheField_);
}

function parseReservationViewCacheTsv_(text){
  const rows = [];
  const lines = String(text || '').replace(/^\uFEFF/, '').split(/\r?\n/);

  let header = null;

  for (let i = 0; i < lines.length; ++i) {
    const line = lines[i];
    if (!line || line.trim() === '') continue;
    if (line.charAt(0) === '#') continue;

    const cols = splitReservationCacheTsvLine_(line);

    if (!header) {
      header = cols.map(v => String(v || '').trim());
      continue;
    }

    const row = {};
    for (let c = 0; c < header.length; ++c) {
      row[header[c]] = (c < cols.length) ? cols[c] : '';
    }

    rows.push(row);
  }

  return rows;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ViewCache_Map_(std::wstring& js)
        {
            js += LR"JS(
let gReservationViewCache = {
  loaded:false,
  loading:false,
  rows:[],
  map:new Map(),
  stamp:0,
  lastError:'',
  failStamp:0,
  failCooldownMs:5000
};

function boolFromCacheField_(v){
  const s = String(v || '').toLowerCase();
  return s === '1' || s === 'true' || s === 'yes' || s === 'on';
}

function normalizeProgramReserveModeStrict_(mode){
  const v = String(mode || '').toLowerCase().trim();

  if (v === 'watch') return 'watch';
  if (v === 'record') return 'record';

  return '';
}

function makeReservationEntryFromCacheRow_(row){
  if (!row || typeof row !== 'object') return null;

  const mode = normalizeProgramReserveModeStrict_(row.mode || row.reserveMode || '');
if (!mode) {
  return null;
}

  const enabled = boolFromCacheField_(row.enabled);

  return normalizeReservationEntryForDisplay_({
    id: String(row.dedupeKey || row.reserveKey || ''),
    label: String(row.title || ''),
    title: String(row.title || ''),
    titleLabel: String(row.title || ''),
    mode: mode,
    reserveMode: mode,

    enabled: enabled,
    disabledByUser: boolFromCacheField_(row.disabledByUser),
    excludeFromAuto: boolFromCacheField_(row.excludeFromAuto),
    autoReserved: boolFromCacheField_(row.autoReserved),

    dedupeKey: String(row.dedupeKey || ''),
    reserveKey: String(row.reserveKey || ''),
    fallbackKey: String(row.fallbackKey || ''),

    space: toSafeInt(row.space, 0),
    networkId: toSafeInt(row.networkId, 0),
    tsId: toSafeInt(row.tsId, 0),
    serviceId: toSafeInt(row.serviceId, 0),
    eventId: toSafeInt(row.eventId, 0),

    serviceKey: String(row.serviceKey || ''),
    service: String(row.serviceName || ''),
    serviceName: String(row.serviceName || ''),
    target: String(row.serviceName || ''),

    startDate: String(row.startDate || ''),
    startTime: String(row.startTime || ''),
    endDate: String(row.endDate || ''),
    endTime: String(row.endTime || ''),
    durationSec: toSafeInt(row.durationSec, 0),

    time: String(row.startTime || ''),
    displayTimeText: String(row.displayTimeText || row.time || row.startTime || ''),
    timeText: String(row.timeText || row.time || row.startTime || '')
  });
}

function addReservationViewCacheKey_(map, key, entry){
  if (!key || !entry) return;
  if (!map.has(key)) {
    map.set(key, entry);
  }
}

function rebuildReservationViewCacheMap_(rows){
  const map = new Map();

  (Array.isArray(rows) ? rows : []).forEach(row => {
    const entry = makeReservationEntryFromCacheRow_(row);
    if (!entry) return;

    const mode = normalizeMode(entry.mode || entry.reserveMode || '');

    addReservationViewCacheKey_(
      map,
      makeReservationStateKey(entry.dedupeKey || entry.id, mode),
      entry
    );

    addReservationViewCacheKey_(
      map,
      makeReservationEventStateKey_(
        entry.space,
        entry.networkId,
        entry.tsId,
        entry.serviceId,
        entry.eventId,
        mode
      ),
      entry
    );

    addReservationViewCacheKey_(
      map,
      makeReservationTimeStateKey_(
        entry.serviceKey,
        entry.startDate,
        entry.startTime,
        entry.label || entry.title || entry.titleLabel,
        mode
      ),
      entry
    );
  });

  return map;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ViewCache_Url_(std::wstring& js)
        {
            js += LR"JS(
function getReservationViewCacheUrl_(){
  try {
    if (window.EPG_RUNTIME && window.EPG_RUNTIME.reservationViewCacheUrl) {
      return String(window.EPG_RUNTIME.reservationViewCacheUrl);
    }
  } catch (_) {
  }

  return '/cache/reservation/view/tasks_view_index.tsv';
}

function invalidateReservationViewCache_(){
  gReservationViewCache.loaded = false;
  gReservationViewCache.rows = [];
  gReservationViewCache.map = new Map();
  gReservationViewCache.stamp = 0;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ViewCache_LoadSync_(std::wstring& js)
        {
            js += LR"JS(
function loadReservationViewCacheSync_(force){
  if (gReservationViewCache.loading) {
    return gReservationViewCache.loaded;
  }

  if (gReservationViewCache.loaded && !force) {
    return true;
  }

  const now = Date.now();

  if (!force &&
      gReservationViewCache.failStamp > 0 &&
      now - gReservationViewCache.failStamp < gReservationViewCache.failCooldownMs) {
    return false;
  }

  const url = getReservationViewCacheUrl_();
  if (!url) {
    gReservationViewCache.lastError = 'cache-url-empty';
    gReservationViewCache.failStamp = Date.now();
    return false;
  }

  gReservationViewCache.loading = true;

  try {
    const xhr = new XMLHttpRequest();
    const sep = url.indexOf('?') >= 0 ? '&' : '?';
    xhr.open('GET', url + sep + 'v=' + String(Date.now()), false);
    xhr.send(null);

    if (xhr.status < 200 || xhr.status >= 300) {
      gReservationViewCache.lastError = 'http-status-' + String(xhr.status);
      gReservationViewCache.loaded = false;
      gReservationViewCache.rows = [];
      gReservationViewCache.map = new Map();
      gReservationViewCache.failStamp = Date.now();
      return false;
    }

    const rows = parseReservationViewCacheTsv_(xhr.responseText || '');

    gReservationViewCache.rows = rows;
    gReservationViewCache.map = rebuildReservationViewCacheMap_(rows);
    gReservationViewCache.loaded = true;
    gReservationViewCache.stamp = Date.now();
    gReservationViewCache.lastError = '';
    gReservationViewCache.failStamp = 0;

    return true;
  } catch (e) {
    gReservationViewCache.loaded = false;
    gReservationViewCache.rows = [];
    gReservationViewCache.map = new Map();
    gReservationViewCache.lastError = (e && e.message) ? String(e.message) : String(e);
    gReservationViewCache.failStamp = Date.now();
    return false;
  } finally {
    gReservationViewCache.loading = false;
  }
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ViewCache_DebugInfo_(std::wstring& js)
        {
            js += LR"JS(
function getReservationViewCacheDebugInfo_(){
  return {
    loaded: !!gReservationViewCache.loaded,
    loading: !!gReservationViewCache.loading,
    rows: Array.isArray(gReservationViewCache.rows) ? gReservationViewCache.rows.length : 0,
    mapSize: gReservationViewCache.map ? gReservationViewCache.map.size : 0,
    stamp: gReservationViewCache.stamp || 0,
    failStamp: gReservationViewCache.failStamp || 0,
    lastError: gReservationViewCache.lastError || '',
    url: getReservationViewCacheUrl_()
  };
}

window.__epgPortalGetReservationViewCacheDebugInfo = getReservationViewCacheDebugInfo_;
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ViewCache_PublicDump_(std::wstring& js)
        {
            js += LR"JS(
window.__epgPortalDumpReservationViewCacheRows = function(){
  loadReservationViewCacheSync_(false);
  return Array.isArray(gReservationViewCache.rows)
    ? gReservationViewCache.rows.slice()
    : [];
};

window.__epgPortalDumpReservationViewCacheKeys = function(){
  loadReservationViewCacheSync_(false);
  return gReservationViewCache.map
    ? Array.from(gReservationViewCache.map.keys())
    : [];
};
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ViewCache_ReloadPublic_(std::wstring& js)
        {
            js += LR"JS(
window.__epgPortalReloadReservationViewCache = function(){
  invalidateReservationViewCache_();

  const ok = loadReservationViewCacheSync_(true);

  if (ok && typeof syncSharedReservationListFromViewCache_ === 'function') {
    try {
      syncSharedReservationListFromViewCache_();
      invalidateReservationStateCache();

      if (typeof refreshReservationViewsAfterSwitchOnce_ === 'function') {
        refreshReservationViewsAfterSwitchOnce_({ syncCache:false });
      }
    } catch (e) {
      console.warn('[reservation-switch] reload view cache sync failed', e);
    }
  }

  return ok;
};

function reloadReservationViewCacheAfterMutation_(){
  invalidateReservationViewCache_();
  return loadReservationViewCacheSync_(true);
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ViewCache_ListEntry_(std::wstring& js)
        {
            js += LR"JS(
function makeReservationListEntryFromViewCacheRow_(row){
  if (!row || typeof row !== 'object') {
    return null;
  }

  const rawMode = String(row.mode || row.reserveMode || '').toLowerCase().trim();
  if (!rawMode) {
    return null;
  }

  const listMode = normalizeReservationListMode_(rawMode);
  const isProgram = listMode === 'watch' || listMode === 'record';

  const title = String(row.title || row.label || row.name || '');
  const serviceName = String(row.serviceName || row.service || row.target || '');

  return normalizeReservationEntryForDisplay_({
    id: String(row.id || row.dedupeKey || row.fallbackKey || row.reserveKey || title),
    label: title || (listMode === 'epg' ? 'EPG予約' : '予約項目'),

    mode: listMode,
    reserveMode: listMode,
    taskType: rawMode,

    category: isProgram
      ? (listMode === 'record' ? 'RECORD' : 'WATCH')
      : 'EPG',

    enabled: boolFromCacheField_(row.enabled),
    disabledByUser: boolFromCacheField_(row.disabledByUser),
    excludeFromAuto: boolFromCacheField_(row.excludeFromAuto),
    autoReserved: boolFromCacheField_(row.autoReserved),

    dedupeKey: String(row.dedupeKey || ''),
    excludeKey: String(row.excludeKey || row.dedupeKey || ''),
    reserveKey: String(row.reserveKey || ''),
    fallbackKey: String(row.fallbackKey || ''),

    service: serviceName,
    serviceName: serviceName,
    target: serviceName,

    serviceKey: String(row.serviceKey || ''),
    physicalChannel: toSafeInt(row.physicalChannel, -1),
    remoteControlKeyId: String(row.remoteControlKeyId || ''),

    space: toSafeInt(row.space, 0),
    networkId: toSafeInt(row.networkId, 0),
    tsId: toSafeInt(row.tsId, 0),
    serviceId: toSafeInt(row.serviceId, 0),
    eventId: toSafeInt(row.eventId, 0),

    date: String(row.date || row.dateText || row.startDate || ''),
    weekday: String(row.weekday || row.weekdayText || ''),

    time: String(row.displayTimeText || row.time || row.timeText || row.startTime || ''),
    displayTimeText: String(row.displayTimeText || row.time || row.timeText || row.startTime || ''),
    timeText: String(row.timeText || row.time || row.startTime || ''),

    startDate: String(row.startDate || ''),
    startTime: String(row.startTime || ''),
    endDate: String(row.endDate || ''),
    endTime: String(row.endTime || ''),
    durationSec: toSafeInt(row.durationSec, 0),

    summary: String(row.summary || ''),
    genre: String(row.genre || ''),
    note: String(row.note || ''),
    reserveSource: String(row.reserveSource || 'view_cache'),

    kind: toSafeInt(row.kind, 0),
    instance: toSafeInt(row.instance, 0),
    requiredTuners: toSafeInt(row.requiredTuners, 1),
    preferredTunerIndex: toSafeInt(row.preferredTunerIndex, -1)
  });
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ViewCache_SyncList_(std::wstring& js)
        {
            js += LR"JS(
function syncSharedReservationListFromViewCache_(){
  reservationSwitchDebug_('view-cache-sync-begin', {
    cache: getReservationViewCacheDebugInfo_()
  });

  if (!loadReservationViewCacheSync_(false)) {
    reservationSwitchDebug_('view-cache-sync-failed', {
      cache: getReservationViewCacheDebugInfo_()
    });
    return false;
  }

  const rows = Array.isArray(gReservationViewCache.rows)
    ? gReservationViewCache.rows
    : [];

  const entries = rows
    .map(makeReservationListEntryFromViewCacheRow_)
    .filter(Boolean);

  setSharedReservationList_(entries);
  invalidateReservationStateCache();

  reservationSwitchDebug_('view-cache-sync-end', {
    rows: rows.length,
    entries: entries.length,
    cache: getReservationViewCacheDebugInfo_()
  });

  return true;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ViewCache_Find_(std::wstring& js)
        {
            js += LR"JS(
function findProgramReservationFromViewCache_(payload, mode){
  const fixedMode = normalizeMode(mode);

  if (!loadReservationViewCacheSync_(false)) {
    return null;
  }

  const keys = makeReservationPayloadStateKeys_(payload, fixedMode);
  const map = gReservationViewCache.map;

  for (let i = 0; i < keys.length; ++i) {
    const hit = map.get(keys[i]);
    if (hit) {
      return hit;
    }
  }

  return null;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ViewCache_Load_(std::wstring& js)
        {
            AppendRuntimeSwitchScript_Core1_ViewCache_Url_(js);
            AppendRuntimeSwitchScript_Core1_ViewCache_LoadSync_(js);
            AppendRuntimeSwitchScript_Core1_ViewCache_DebugInfo_(js);
            AppendRuntimeSwitchScript_Core1_ViewCache_PublicDump_(js);
            AppendRuntimeSwitchScript_Core1_ViewCache_ReloadPublic_(js);
            AppendRuntimeSwitchScript_Core1_ViewCache_ListEntry_(js);
            AppendRuntimeSwitchScript_Core1_ViewCache_SyncList_(js);
            AppendRuntimeSwitchScript_Core1_ViewCache_Find_(js);
        }

        void AppendRuntimeSwitchScript_Core1_ViewCache_(std::wstring& js)
        {
            AppendRuntimeSwitchScript_Core1_ViewCache_Parse_(js);
            AppendRuntimeSwitchScript_Core1_ViewCache_Map_(js);
            AppendRuntimeSwitchScript_Core1_ViewCache_Load_(js);
        }

        void AppendRuntimeSwitchScript_Core1_Api_Fetch_(std::wstring& js)
        {
            js += LR"JS(
function fetchReservationListSync(){
  const apiBase =
    (window.EPG_RUNTIME && window.EPG_RUNTIME.reservationApiBase)
      ? String(window.EPG_RUNTIME.reservationApiBase)
      : '';

  if (!apiBase) {
    return { ok:false, reason:'reservation-api-disabled' };
  }

  try {
    const xhr = new XMLHttpRequest();
    xhr.open('GET', apiBase + '/list', false);
    xhr.send(null);

    let obj = null;
    try {
      obj = JSON.parse(xhr.responseText || '{}');
    } catch (_) {
      obj = null;
    }

    if (obj && typeof obj === 'object') {
      return obj;
    }

    return { ok:false, reason:'invalid-json-response' };
  } catch (e) {
    return {
      ok:false,
      reason:'http-list-error',
      message:(e && e.message) ? String(e.message) : String(e)
    };
  }
}

function reloadReservationStateSync(){
  const result = fetchReservationListSync();
  if (!result || result.ok === false) {
    return result || { ok:false, reason:'list-reload-failed' };
  }

  applyReservationListResult(result);
  refreshAllReservationVisuals();
  return { ok:true };
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_Api_MutationIdentity_(std::wstring& js)
        {
            js += LR"JS(
function getMutationEntryIdentity_(entry){
  if (!entry) return '';

  const mode = normalizeMode(entry.mode || entry.reserveMode || '');
  const key = String(entry.dedupeKey || entry.id || '');

  if (!mode || !key) {
    return '';
  }

  return mode + '|' + key;
}

function sameMutationEntry_(entry, probe){
  const a = getMutationEntryIdentity_(entry);
  const b = getMutationEntryIdentity_(probe);

  if (a && b && a === b) {
    return true;
  }

  const aid = String(entry && entry.id ? entry.id : '');
  const bid = String(probe && probe.id ? probe.id : '');

  if (aid && bid && aid === bid) {
    return true;
  }

  return false;
}

function listHasMutationEntry_(list, probe){
  if (!Array.isArray(list) || !probe) {
    return false;
  }

  return list.some(v => sameMutationEntry_(v, probe));
}

function reservationEntryMatchesRemoveFallback_(entry, fallback){
  if (!entry || !fallback) {
    return false;
  }

  const removeId = String(fallback.removeId || '');
  const removeKey = String(fallback.removeKey || '');

  const id = String(entry.id || '');
  const dedupeKey = String(entry.dedupeKey || '');
  const reserveKey = String(entry.reserveKey || '');
  const fallbackKey = String(entry.fallbackKey || '');

  if (removeId) {
    if (id === removeId ||
        dedupeKey === removeId ||
        reserveKey === removeId ||
        fallbackKey === removeId) {
      return true;
    }
  }

  if (removeKey) {
    if (id === removeKey ||
        dedupeKey === removeKey ||
        reserveKey === removeKey ||
        fallbackKey === removeKey) {
      return true;
    }
  }

  const probe = fallback.removeEntry;
  if (!probe) {
    return false;
  }

  const probeId = String(probe.id || '');
  const probeDedupe = String(probe.dedupeKey || '');
  const probeReserve = String(probe.reserveKey || '');
  const probeFallback = String(probe.fallbackKey || '');

  const probeKeys = [
    probeId,
    probeDedupe,
    probeReserve,
    probeFallback
  ].filter(Boolean);

  for (let i = 0; i < probeKeys.length; ++i) {
    const key = probeKeys[i];

    if (id === key ||
        dedupeKey === key ||
        reserveKey === key ||
        fallbackKey === key) {
      return true;
    }
  }

  const entryMode = normalizeReservationListMode_(
    entry.mode || entry.reserveMode || entry.taskType || ''
  );

  const probeMode = normalizeReservationListMode_(
    probe.mode || probe.reserveMode || probe.taskType || ''
  );

  if (entryMode && probeMode && entryMode !== probeMode) {
    return false;
  }

  const entryTitle = normalizeText(entry.label || entry.title || entry.titleLabel || '');
  const probeTitle = normalizeText(probe.label || probe.title || probe.titleLabel || '');

  const entryService = normalizeText(entry.serviceName || entry.service || entry.target || '');
  const probeService = normalizeText(probe.serviceName || probe.service || probe.target || '');

  const entryDate = String(entry.startDate || entry.date || entry.dateText || '');
  const probeDate = String(probe.startDate || probe.date || probe.dateText || '');

  const entryTime = String(entry.startTime || '');
  const probeTime = String(probe.startTime || '');

  if (entryTitle && probeTitle &&
      entryTitle === probeTitle &&
      entryService && probeService &&
      entryService === probeService &&
      entryDate && probeDate &&
      entryDate === probeDate) {
    if (!entryTime || !probeTime || entryTime === probeTime) {
      return true;
    }
  }

  return false;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_Api_FallbackApply_(std::wstring& js)
        {
            js += LR"JS(
function applyMutationFallbackToList_(baseList, fallback){
  let next = Array.isArray(baseList) ? baseList.slice() : [];

  if (!fallback || typeof fallback !== 'object') {
    return next;
  }

  if (fallback.removeId || fallback.removeKey || fallback.removeEntry) {
  next = next.filter(v => !reservationEntryMatchesRemoveFallback_(v, fallback));
}

  if (fallback.upsertEntry) {
    const entry = normalizeReservationEntryForDisplay_(fallback.upsertEntry);
    let replaced = false;

    next = next.map(v => {
      if (sameMutationEntry_(v, entry)) {
        replaced = true;
        return entry;
      }
      return v;
    });

    if (!replaced) {
      next.push(entry);
    }
  }

  if (fallback.patchId && fallback.patch) {
    const patchId = String(fallback.patchId || '');

    next = next.map(v => {
      const id = String(v && v.id ? v.id : '');
      const key = String(v && (v.dedupeKey || v.id) ? (v.dedupeKey || v.id) : '');

      if (id !== patchId && key !== patchId) {
        return v;
      }

      const patched = cloneReservationEntry(v);
      Object.assign(patched, fallback.patch);
      return normalizeReservationEntryForDisplay_(patched);
    });
  }

  return next;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_Api_FallbackCheck_(std::wstring& js)
        {
            js += LR"JS(
function shouldApplyMutationFallback_(items, fallback){
  if (!fallback || typeof fallback !== 'object') {
    return false;
  }

  if (fallback.upsertEntry && !listHasMutationEntry_(items, fallback.upsertEntry)) {
    return true;
  }

  if (fallback.removeId || fallback.removeKey || fallback.removeEntry) {
  const stillExists = Array.isArray(items) &&
    items.some(v => reservationEntryMatchesRemoveFallback_(v, fallback));

  if (stillExists) {
    reservationSwitchDebug_('mutation-fallback-remove-still-exists', {
      removeId: String(fallback.removeId || ''),
      removeKey: String(fallback.removeKey || ''),
      removeEntry: reservationSwitchCompactEntry_(fallback.removeEntry),
      items: Array.isArray(items) ? items.length : -1
    });

    return true;
  }
}

  if (fallback.patchId && fallback.patch && typeof fallback.patch === 'object') {
    const patchId = String(fallback.patchId || '');

    if (!patchId || !Array.isArray(items)) {
      return true;
    }

    const hit = items.find(v => {
      const id = String(v && v.id ? v.id : '');
      const key = String(v && (v.dedupeKey || v.id) ? (v.dedupeKey || v.id) : '');
      return id === patchId || key === patchId;
    });

    if (!hit) {
      return true;
    }

    const patch = fallback.patch;

    if (Object.prototype.hasOwnProperty.call(patch, 'enabled') &&
        !!hit.enabled !== !!patch.enabled) {
      return true;
    }

    if (Object.prototype.hasOwnProperty.call(patch, 'disabledByUser') &&
        !!hit.disabledByUser !== !!patch.disabledByUser) {
      return true;
    }

    if (Object.prototype.hasOwnProperty.call(patch, 'excludeFromAuto') &&
        !!hit.excludeFromAuto !== !!patch.excludeFromAuto) {
      return true;
    }
  }

  return false;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_Api_ViewCacheMutation_(std::wstring& js)
        {
            js += LR"JS(
function buildReservationEntriesFromViewCacheRows_(){
  const rows = Array.isArray(gReservationViewCache.rows)
    ? gReservationViewCache.rows
    : [];

  return rows
    .map(makeReservationListEntryFromViewCacheRow_)
    .filter(Boolean);
}

function tryApplyReservationViewCacheForMutation_(fallback, force){
  reservationSwitchDebug_('mutation-view-cache-try-begin', {
    force: !!force,
    fallback: fallback || null,
    cache: getReservationViewCacheDebugInfo_()
  });

  const loaded = force
    ? reloadReservationViewCacheAfterMutation_()
    : loadReservationViewCacheSync_(false);

  if (!loaded) {
    reservationSwitchDebug_('mutation-view-cache-try-load-failed', {
      force: !!force,
      cache: getReservationViewCacheDebugInfo_()
    });

    return {
      ok:false,
      reason:'view-cache-load-failed'
    };
  }

  const entries = buildReservationEntriesFromViewCacheRows_();

  if (shouldApplyMutationFallback_(entries, fallback)) {
    reservationSwitchDebug_('mutation-view-cache-stale', {
      entries: entries.length,
      fallback: fallback || null,
      cache: getReservationViewCacheDebugInfo_()
    });

    return {
      ok:false,
      reason:'view-cache-stale',
      items:entries
    };
  }

  setSharedReservationList_(entries);
  invalidateReservationStateCache();

  reservationSwitchDebug_('mutation-view-cache-apply-ok', {
    entries: entries.length,
    cache: getReservationViewCacheDebugInfo_()
  });

  return {
    ok:true,
    source:'view-cache',
    items:entries
  };
}

let gReservationMutationResyncSeq_ = 0;

function scheduleReservationCacheResyncAfterMutation_(fallback){
  const seq = ++gReservationMutationResyncSeq_;
  const delays = [160, 480, 1200];
  let applied = false;

  delays.forEach(delayMs => {
    try {
      window.setTimeout(function(){
        if (seq !== gReservationMutationResyncSeq_) {
          return;
        }

        if (applied) {
          return;
        }

        const synced = tryApplyReservationViewCacheForMutation_(fallback, true);
        if (synced && synced.ok) {
  applied = true;

  queueReservationUiSync_(
    'cache-resync-after-mutation',
    true
  );

  try {
    flushVisibleProgramTableFramePendingRenders_(
      'cache-resync-after-mutation',
      true
    );
  } catch (_) {
  }
}
      }, delayMs);
    } catch (_) {
    }
  });
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_Api_MutationRefresh_(std::wstring& js)
        {
            js += LR"JS(
function refreshReservationViewsAfterMutation_(reason){
  refreshReservationViewsAfterSwitch({
    syncCache:false,
    scheduleRetry:true,
    skipCacheSync:true,
    preferLocalState:true,
    notifyFrames:true,
    refreshProgramFrames:false
  });

  try {
    queueReservationUiSync_(
      String(reason || 'mutation-refresh'),
      true
    );
  } catch (_) {
  }
}

function finishReservationMutationRefresh_(source, reason, items, reload, fallback){
  reservationSwitchDebug_('mutation-finish-begin', {
    source: String(source || ''),
    reason: String(reason || ''),
    items: Array.isArray(items) ? items.length : -1,
    reload: reservationSwitchCompactResult_(reload),
    fallback: fallback || null
  });

  setSharedReservationList_(Array.isArray(items) ? items : []);
invalidateReservationStateCache();
markReservationLocalMutation_();

  refreshReservationViewsAfterMutation_(reason);
  scheduleReservationCacheResyncAfterMutation_(fallback);

  const ret = {
    ok:true,
    source:String(source || ''),
    reason:String(reason || ''),
    items:getSharedReservationList_(),
    reload:reload || null
  };

  reservationSwitchDebug_('mutation-finish-end', {
    result: reservationSwitchCompactResult_(ret)
  });
epgPortalTracePush_('parent', 'mutation-finish-end', {
  source: String(source || ''),
  reason: String(reason || ''),
  items: Array.isArray(items) ? items.length : -1
});

  return ret;
}

function refreshReservationListAfterMutation_(mutationResult, reason, fallback){
  reservationSwitchDebug_('mutation-refresh-begin', {
    reason: String(reason || ''),
    mutationResult: reservationSwitchCompactResult_(mutationResult),
    fallback: fallback || null
  });

  const current = getSharedReservationList_().slice();

  const cacheResult = tryApplyReservationViewCacheForMutation_(fallback, true);
  if (cacheResult && cacheResult.ok) {
   refreshReservationViewsAfterMutation_(reason);

    const ret = {
      ok:true,
      source:'view-cache',
      reason:String(reason || ''),
      items:cacheResult.items || [],
      reload:null
    };

    reservationSwitchDebug_('mutation-refresh-end-view-cache', {
      result: reservationSwitchCompactResult_(ret)
    });

    return ret;
  }

  reservationSwitchDebug_('mutation-refresh-cache-miss', {
    cacheResult: reservationSwitchCompactResult_(cacheResult)
  });

  const reload = fetchReservationListSync();

  reservationSwitchDebug_('mutation-refresh-list-reload', {
    reload: reservationSwitchCompactResult_(reload)
  });

  if (reload && reload.ok !== false && Array.isArray(reload.items)) {
    let next = reload.items;
    let source = 'list-reload';

    if (shouldApplyMutationFallback_(next, fallback)) {
      next = applyMutationFallbackToList_(current.length > 0 ? current : next, fallback);
      source = 'list-reload-plus-local-fallback';

      console.warn(
        '[reservation-switch] reload looks stale, apply mutation fallback',
        reason,
        reload,
        fallback
      );

      reservationSwitchDebug_('mutation-refresh-list-reload-stale-apply-fallback', {
        reason: String(reason || ''),
        reloadItems: Array.isArray(reload.items) ? reload.items.length : -1,
        fallback: fallback || null
      });
    }

    return finishReservationMutationRefresh_(
      source,
      reason,
      next,
      reload,
      fallback
    );
  }

  if (mutationResult && Array.isArray(mutationResult.items)) {
    let next = mutationResult.items;
    let source = 'mutation-items';

    if (shouldApplyMutationFallback_(next, fallback)) {
      next = applyMutationFallbackToList_(current.length > 0 ? current : next, fallback);
      source = 'mutation-items-plus-local-fallback';

      console.warn(
        '[reservation-switch] mutation items look stale, apply mutation fallback',
        reason,
        mutationResult,
        fallback
      );

      reservationSwitchDebug_('mutation-refresh-mutation-items-stale-apply-fallback', {
        reason: String(reason || ''),
        mutationItems: Array.isArray(mutationResult.items) ? mutationResult.items.length : -1,
        fallback: fallback || null
      });
    }

    return finishReservationMutationRefresh_(
      source,
      reason,
      next,
      reload || null,
      fallback
    );
  }

  if (fallback) {
    const next = applyMutationFallbackToList_(current, fallback);

    return finishReservationMutationRefresh_(
      'local-fallback-only',
      reason,
      next,
      reload || null,
      fallback
    );
  }

  refreshReservationViewsAfterMutation_(reason);

  scheduleReservationCacheResyncAfterMutation_(fallback);

  const ret = {
    ok:false,
    source:'fallback-redraw-only',
    reason:String(reason || ''),
    reload:reload || null
  };

  reservationSwitchDebug_('mutation-refresh-end-fallback-redraw-only', {
    result: reservationSwitchCompactResult_(ret)
  });

  return ret;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_Api_BackendData_(std::wstring& js)
        {
            js += LR"JS(
function applyBackendReservationData(result){
  if (!result) return;

  if (Array.isArray(result.reservationData)) {
    setSharedReservationList_(result.reservationData);
    invalidateReservationViewCache_();
    invalidateReservationStateCache();
    return;
  }

  if (Array.isArray(result.items)) {
    setSharedReservationList_(result.items);
    invalidateReservationViewCache_();
    invalidateReservationStateCache();
  }
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_Api_(std::wstring& js)
        {
            AppendRuntimeSwitchScript_Core1_Api_Fetch_(js);
            AppendRuntimeSwitchScript_Core1_Api_MutationIdentity_(js);
            AppendRuntimeSwitchScript_Core1_Api_FallbackApply_(js);
            AppendRuntimeSwitchScript_Core1_Api_FallbackCheck_(js);
            AppendRuntimeSwitchScript_Core1_Api_ViewCacheMutation_(js);
            AppendRuntimeSwitchScript_Core1_Api_MutationRefresh_(js);
            AppendRuntimeSwitchScript_Core1_Api_BackendData_(js);
        }

        void AppendRuntimeSwitchScript_Core1_BackendRegister_(std::wstring& js)
        {
            js += LR"JS(
(function registerHttpReservationBackend(){
  const apiBase =
    (window.EPG_RUNTIME && window.EPG_RUNTIME.reservationApiBase)
      ? String(window.EPG_RUNTIME.reservationApiBase)
      : '';

  function encodeForm(payload){
    const pairs = [];
    Object.keys(payload || {}).forEach(function(key){
      const value = payload[key];
      if (value === undefined || value === null) return;
      pairs.push(
        encodeURIComponent(key) + '=' + encodeURIComponent(String(value))
      );
    });
    return pairs.join('&');
  }

  function postSync(path, payload){
    if (!apiBase) {
      return {
        ok:false,
        reason:'reservation-api-disabled',
        path:path || '',
        apiBase:apiBase || ''
      };
    }

    try {
      const xhr = new XMLHttpRequest();
      xhr.open('POST', apiBase + path, false);
      xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded; charset=UTF-8');
      xhr.send(encodeForm(payload));

      let obj = null;
      try {
        obj = JSON.parse(xhr.responseText || '{}');
      } catch (_) {
        obj = null;
      }

      if (obj && typeof obj === 'object') {
        if (obj.ok === undefined) {
          obj.ok = (xhr.status >= 200 && xhr.status < 300);
        }
        if (obj.status === undefined) {
          obj.status = xhr.status;
        }
        if (obj.path === undefined) {
          obj.path = path || '';
        }
        return obj;
      }

      return {
        ok: xhr.status >= 200 && xhr.status < 300,
        reason: 'invalid-json-response',
        status: xhr.status,
        path: path || '',
        responseText: xhr.responseText || ''
      };
    } catch (e) {
      return {
        ok:false,
        reason:'http-backend-error',
        path:path || '',
        message:(e && e.message) ? String(e.message) : String(e)
      };
    }
  }

  const backend = window.__epgPortalNativeReservationBackend || {};

  if (typeof backend.upsertProgramReservation !== 'function') {
    backend.upsertProgramReservation = function(payload){
      return postSync('/upsert', payload);
    };
  }

  if (typeof backend.removeProgramReservation !== 'function') {
    backend.removeProgramReservation = function(payload){
      return postSync('/remove', payload);
    };
  }

  if (typeof backend.setProgramReservationEnabled !== 'function') {
    backend.setProgramReservationEnabled = function(payload, enabled, disabledByUser, excludeFromAuto){
      const req = Object.assign({}, payload || {}, {
        enabled: !!enabled,
        disabledByUser: !!disabledByUser,
        excludeFromAuto: !!excludeFromAuto
      });
      return postSync('/set_enabled', req);
    };
  }

  backend.removeReservationById = function(payload){
    const req = (typeof payload === 'string')
      ? { id: payload }
      : Object.assign({}, payload || {});

    return postSync('/remove_by_id', req);
  };

  backend.setReservationEnabledById = function(payload, enabled, disabledByUser, excludeFromAuto){
    const req = (typeof payload === 'string')
      ? { id: payload }
      : Object.assign({}, payload || {});

    req.enabled = !!enabled;
    req.disabledByUser = !!disabledByUser;
    req.excludeFromAuto = !!excludeFromAuto;

    return postSync('/set_enabled_by_id', req);
  };

  window.__epgPortalNativeReservationBackend = backend;
})();
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_Payload_(std::wstring& js)
        {
            js += LR"JS(
function buildProgramReservationPayload(payload, mode, preferredTunerIndex, stateOverride){
  const fixedMode = normalizeMode(mode);

  const dateText = normalizeDateTextForReservation(
    payload && (payload.dateText || payload.date) ? (payload.dateText || payload.date) : ''
  );
  const weekdayText = String(payload && payload.weekdayText ? payload.weekdayText : '');

  const rawTimeText = String(
    payload && (payload.displayTimeText || payload.time || payload.timeText)
      ? (payload.displayTimeText || payload.time || payload.timeText)
      : ''
  );

  const displayTimeText = normalizeTimeRangeForDisplay_(rawTimeText);
  const timeText = normalizeTimeRangeForStorage_(displayTimeText || rawTimeText);

  const startDateText = String(payload && payload.startDate ? payload.startDate : '');
  const startTimeText = String(payload && payload.startTime ? payload.startTime : '');
  const endDateText = String(payload && payload.endDate ? payload.endDate : '');
  const endTimeText = String(payload && payload.endTime ? payload.endTime : '');

  const parsedDate = parseDateTextToIso(dateText);
  const parsedTime = parseTimeTextParts(displayTimeText || timeText);

  const startDate = startDateText || parsedDate;
  const startTime = startTimeText || parsedTime.startTime;

  let endDate = endDateText || parsedDate;
  if (!endDate && startDate) {
    endDate = startDate;
  }
  if (!endDateText && parsedTime.crossesDay && startDate) {
    endDate = shiftIsoDate(startDate, 1) || startDate;
  }

  const endTime = endTimeText || parsedTime.endTime || startTime;

  const space = toSafeInt(
    resolveCurrentSpaceForReservation_(payload),
    0
  );

  const networkId = toSafeInt(payload && payload.networkId, 0);
  const tsId = toSafeInt(payload && payload.tsId, 0);
  const serviceId = toSafeInt(payload && payload.serviceId, 0);
  const eventId = toSafeInt(payload && payload.eventId, 0);

  const result = {
    space: space,
    networkId: networkId,
    tsId: tsId,
    serviceId: serviceId,
    eventId: eventId,

    serviceKey: String(payload && payload.serviceKey ? payload.serviceKey : ''),
    physicalChannel: toSafeInt(payload && payload.physicalChannel, -1),
    remoteControlKeyId: String(payload && payload.remoteControlKeyId ? payload.remoteControlKeyId : ''),

    displayTimeText: displayTimeText,

    title: String(payload && payload.title ? payload.title : ''),
    titleLabel: String(
      payload && (payload.titleLabel || payload.title)
        ? (payload.titleLabel || payload.title)
        : ''
    ),
    serviceName: String(
      payload && (payload.serviceName || payload.service)
        ? (payload.serviceName || payload.service)
        : ''
    ),
    service: String(
      payload && (payload.serviceName || payload.service)
        ? (payload.serviceName || payload.service)
        : ''
    ),
    summary: String(payload && payload.summary ? payload.summary : ''),
    genre: String(payload && payload.genre ? payload.genre : ''),

    dateText: dateText,
    weekdayText: weekdayText,
    timeText: timeText,
    time: displayTimeText || timeText,

    startDate: startDate,
    startTime: startTime,
    endDate: endDate,
    endTime: endTime,
    durationSec: toSafeInt(
      payload && payload.durationSec != null ? payload.durationSec : parsedTime.durationSec,
      parsedTime.durationSec
    ),

    reserveMode: fixedMode,
    reserveSource: String(
      payload && payload.reserveSource ? payload.reserveSource : 'epg_portal_switch'
    ),
    dedupeKey: '',
    excludeKey: '',

    enabled: !(stateOverride && stateOverride.enabled === false),
    disabledByUser: !!(stateOverride && stateOverride.disabledByUser),
    excludeFromAuto: !!(stateOverride && stateOverride.excludeFromAuto),
    autoReserved: !!(stateOverride && stateOverride.autoReserved),

    requiredTuners: toSafeInt(
      payload && payload.requiredTuners != null ? payload.requiredTuners : 1,
      1
    ),
    preferredTunerIndex: Number.isFinite(Number(preferredTunerIndex))
      ? Number(preferredTunerIndex)
      : toSafeInt(payload && payload.preferredTunerIndex, -1)
  };

  result.dedupeKey =
    String(result.space)
    + '-' + String(result.networkId)
    + '-' + String(result.tsId)
    + '-' + String(result.serviceId)
    + '-' + String(result.eventId)
    + '-' + String(result.startDate || '')
    + '-' + String(result.startTime || '')
    + '-' + fixedMode;

  result.excludeKey = result.dedupeKey;

  return result;
}

function buildProgramReservationKey(payload, mode){
  return buildProgramReservationPayload(payload, mode, -1, null).dedupeKey;
}

function buildProgramCandidateFromPayload(payload){
  return buildProgramReservationPayload(payload, 'record', -1, null);
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core1_ListState_(std::wstring& js)
        {
            AppendRuntimeSwitchScript_Core1_ListState_Base_(js);
            AppendRuntimeSwitchScript_Core1_ListState_KeyNormalize_(js);
            AppendRuntimeSwitchScript_Core1_ListState_EventKeys_(js);
            AppendRuntimeSwitchScript_Core1_ListState_TimeKeys_(js);
            AppendRuntimeSwitchScript_Core1_ListState_EntryKeys_(js);
            AppendRuntimeSwitchScript_Core1_ListState_EntryKeysSoft_(js);
            AppendRuntimeSwitchScript_Core1_ListState_PayloadKeys_(js);
            AppendRuntimeSwitchScript_Core1_ListState_PayloadKeysTime_(js);
            AppendRuntimeSwitchScript_Core1_ListState_StateCache_(js);
            AppendRuntimeSwitchScript_Core1_ListState_OverlayBase_(js);
            AppendRuntimeSwitchScript_Core1_ListState_OverlayRemove_(js);
            AppendRuntimeSwitchScript_Core1_ListState_OverlayFind_(js);
            AppendRuntimeSwitchScript_Core1_ListState_Result_(js);
        }

        void AppendRuntimeSwitchScript_Core1_(std::wstring& js)
        {
            AppendRuntimeSwitchScript_Core1_Text_(js);
            AppendRuntimeSwitchScript_Core1_Time_(js);
            AppendRuntimeSwitchScript_Core1_Date_(js);
            AppendRuntimeSwitchScript_Core1_Config_(js);
            AppendRuntimeSwitchScript_Core1_BackendAccess_(js);
            AppendRuntimeSwitchScript_Core1_Debug_(js);
            AppendRuntimeSwitchScript_Core1_Trace_(js);
            AppendRuntimeSwitchScript_Core1_ListState_(js);
            AppendRuntimeSwitchScript_Core1_ListState_MirrorBase_(js);
            AppendRuntimeSwitchScript_Core1_ListState_MirrorBridge_(js);
            AppendRuntimeSwitchScript_Core1_ViewCache_(js);
            AppendRuntimeSwitchScript_Core1_Api_(js);
            AppendRuntimeSwitchScript_Core1_BackendRegister_(js);
            AppendRuntimeSwitchScript_Core1_Payload_(js);
        }

        void AppendRuntimeSwitchScript_Core2_Entry_(std::wstring& js)
        {
            js += LR"JS(
function makeProgramReservationEntry(item, mode, preferredTunerIndex){
  const payload = buildProgramReservationPayload(item, mode, preferredTunerIndex, null);

  return {
    id: payload.dedupeKey,
    label: String(payload.titleLabel || payload.title || '予約項目'),
    category: payload.reserveMode === 'record' ? 'RECORD' : 'WATCH',
    mode: payload.reserveMode,
    enabled: payload.enabled,
    disabledByUser: payload.disabledByUser,
    excludeFromAuto: payload.excludeFromAuto,
    autoReserved: payload.autoReserved,

    date: payload.dateText,
weekday: payload.weekdayText,
time: payload.displayTimeText || payload.time || payload.timeText,
displayTimeText: payload.displayTimeText || payload.time || payload.timeText,
timeText: payload.timeText,

startDate: payload.startDate,
startTime: payload.startTime,
endDate: payload.endDate,
endTime: payload.endTime,

    service: payload.serviceName,
    target: payload.serviceName,
    schedule: (payload.displayTimeText || payload.time || payload.timeText) || (payload.startDate + ' ' + payload.startTime),
    note: '番組表から登録',
    summary: payload.summary,
    genre: payload.genre,
    reserveSource: payload.reserveSource,
    dedupeKey: payload.dedupeKey,
    excludeKey: payload.excludeKey,

    space: payload.space,
    channel: -1,
    networkId: payload.networkId,
    serviceKey: payload.serviceKey,
    physicalChannel: payload.physicalChannel,
    remoteControlKeyId: payload.remoteControlKeyId,
    tsId: payload.tsId,
    serviceId: payload.serviceId,
    eventId: payload.eventId,
    kind: 0,
    instance: 0,

    preferredTunerIndex: payload.preferredTunerIndex,
    requiredTuners: payload.requiredTuners
  };
}

function cloneReservationEntry(entry){
  return Object.assign({}, entry || {});
}

function upsertProgramReservationEntry(entry){
  if (!entry) {
    return;
  }

  const list = getSharedReservationList_().slice();

  const fixedMode = normalizeMode(entry && entry.mode ? entry.mode : '');
  const fixedKey = String(
    entry && (entry.dedupeKey || entry.id)
      ? (entry.dedupeKey || entry.id)
      : ''
  );

  const idx = list.findIndex(v => {
    const vm = normalizeMode(
      v && (v.mode || v.reserveMode)
        ? (v.mode || v.reserveMode)
        : ''
    );
    const vk = String(
      v && (v.dedupeKey || v.id)
        ? (v.dedupeKey || v.id)
        : ''
    );
    return vm === fixedMode && vk === fixedKey;
  });

  if (idx >= 0) {
    list[idx] = normalizeReservationEntryForDisplay_(entry);
  } else {
    list.push(normalizeReservationEntryForDisplay_(entry));
  }

  setSharedReservationList_(list);
  invalidateReservationStateCache();
}

function removeProgramReservation(item, mode){
  if (!item) return false;

  const list = getReservationListRef();
  const fixedMode = normalizeMode(mode);
  const key = buildProgramReservationKey(item, fixedMode);
  const before = list.length;

  const nextList = list.filter(v => {
    const vm = normalizeMode(v && (v.mode || v.reserveMode) ? (v.mode || v.reserveMode) : '');
    const vk = String(v && (v.dedupeKey || v.id) ? (v.dedupeKey || v.id) : '');
    return !(vm === fixedMode && vk === key);
  });

  setSharedReservationList_(nextList);
  invalidateReservationStateCache();
  return nextList.length !== before;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core2_Find_(std::wstring& js)
        {
            js += LR"JS(
function sameProgramReservationFallback_(entry, payload, fixedMode){
  if (!entry || !payload) return false;

  const em = String(entry.mode || entry.reserveMode || '').toLowerCase();
  if (em !== fixedMode) return false;

  const entryServiceId = toSafeInt(entry.serviceId, -1);
  const entryEventId = toSafeInt(entry.eventId, -1);
  const payloadServiceId = toSafeInt(payload.serviceId, -2);
  const payloadEventId = toSafeInt(payload.eventId, -2);

  if (entryServiceId >= 0 &&
      entryEventId >= 0 &&
      entryServiceId === payloadServiceId &&
      entryEventId === payloadEventId) {
    return true;
  }

  const entryTitle = normalizeText(entry.label || entry.title || entry.titleLabel || '');
  const payloadTitle = normalizeText(payload.title || payload.titleLabel || '');

  const entryService = normalizeText(entry.service || entry.target || entry.serviceName || '');
  const payloadService = normalizeText(payload.service || payload.serviceName || '');

  const entryDate = normalizeDateTextForReservation(entry.startDate || entry.date || entry.dateText || '');
  const payloadDate = normalizeDateTextForReservation(payload.startDate || payload.date || payload.dateText || '');

  const entryTime = normalizeTimeTextForReservation(entry.startTime || entry.time || entry.timeText || '');
  const payloadTime = normalizeTimeTextForReservation(payload.startTime || payload.time || payload.timeText || '');

  if (entryTitle && payloadTitle &&
      entryTitle === payloadTitle &&
      entryService && payloadService &&
      entryService === payloadService &&
      entryDate && payloadDate &&
      entryDate === payloadDate) {
    if (!entryTime || !payloadTime || entryTime === payloadTime) {
      return true;
    }
  }

  return false;
}

function findProgramReservation(item, mode){
  if (!item) return null;

  const fixedMode = normalizeMode(mode);

  // 1. 直前操作 overlay を最優先
  const overlay = getReservationMutationOverlayForPayload_(item, fixedMode);
  if (overlay && overlay.hit) {
    reservationSwitchFindDebug_('find-hit-overlay', {
      mode: fixedMode,
      removed: !overlay.entry,
      payload: reservationSwitchCompactPayload_(item),
      entry: reservationSwitchCompactEntry_(overlay.entry)
    });

    return overlay.entry || null;
  }

  // 2. ローカル変更直後は view cache を見ない。
  //    削除直後に古い tasks_view_index.tsv を拾って予約表示が復活するのを防ぐ。
  if (isReservationLocalMutationHolding_()) {
    const localHit = findProgramReservationFromSharedList_(item, fixedMode);

    if (localHit) {
      reservationSwitchFindDebug_('find-hit-local-hold-shared', {
        mode: fixedMode,
        payload: reservationSwitchCompactPayload_(item),
        entry: reservationSwitchCompactEntry_(localHit)
      });

      return localHit;
    }

    reservationSwitchFindDebug_('find-miss-local-hold-no-cache', {
      mode: fixedMode,
      payload: reservationSwitchCompactPayload_(item)
    });

    return null;
  }

  // 3. 通常時だけ view cache を見る
  const cacheHit = findProgramReservationFromViewCache_(item, fixedMode);
  if (cacheHit) {
    reservationSwitchFindDebug_('find-hit-view-cache', {
      mode: fixedMode,
      localHolding: isReservationLocalMutationHolding_(),
      payload: reservationSwitchCompactPayload_(item),
      entry: reservationSwitchCompactEntry_(cacheHit)
    });

    return cacheHit;
  }

  // 4. 最後に shared list
  const sharedHit = findProgramReservationFromSharedList_(item, fixedMode);
  if (sharedHit) {
    return sharedHit;
  }

  reservationSwitchFindDebug_('find-miss', {
    mode: fixedMode,
    payload: reservationSwitchCompactPayload_(item)
  });

  return null;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core2_FindShared_(std::wstring& js)
        {
            js += LR"JS(
function findProgramReservationFromSharedList_(item, fixedMode){
  if (!item) return null;

  const dedupeKey = buildProgramReservationKey(item, fixedMode);
  const key = makeReservationStateKey(dedupeKey, fixedMode);

  if (key) {
    const hit = getReservationStateCache().get(key);
    if (hit) {
      reservationSwitchFindDebug_('find-hit-shared-state-cache', {
        mode: fixedMode,
        key: key,
        payload: reservationSwitchCompactPayload_(item),
        entry: reservationSwitchCompactEntry_(hit)
      });

      return hit;
    }
  }

  const list = getSharedReservationList_();

  for (let i = 0; i < list.length; ++i) {
    if (sameProgramReservationFallback_(list[i], item, fixedMode)) {
      reservationSwitchFindDebug_('find-hit-shared-fallback', {
        mode: fixedMode,
        payload: reservationSwitchCompactPayload_(item),
        entry: reservationSwitchCompactEntry_(list[i])
      });

      return list[i];
    }
  }

  return null;
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core2_State_(std::wstring& js)
        {
            js += LR"JS(
function getReservationEnabled(entry){
  return !(entry && entry.enabled === false);
}

function getReservationDisabledByUser(entry){
  return !!(entry && entry.disabledByUser);
}

function getReservationExcludeFromAuto(entry){
  return !!(entry && entry.excludeFromAuto);
}

function formatReserveModeLabel(mode){
  return normalizeMode(mode) === 'watch' ? '視聴' : '録画';
}

function formatReserveStateLabel(entry){
  if (!entry) return '';
  if (entry.enabled === false) {
    return entry.disabledByUser ? '手動無効' : '無効';
  }
  return '有効';
}

function getProgramButtonStateFromPayload(payload){
  const watchRes = findProgramReservation(payload, 'watch');
  const recordRes = findProgramReservation(payload, 'record');

  return {
    watchReserved: !!watchRes,
    recordReserved: !!recordRes,

    watchEnabled: getReservationEnabled(watchRes),
    recordEnabled: getReservationEnabled(recordRes),

    watchDisabledByUser: getReservationDisabledByUser(watchRes),
    recordDisabledByUser: getReservationDisabledByUser(recordRes),

    watchExcludeFromAuto: getReservationExcludeFromAuto(watchRes),
    recordExcludeFromAuto: getReservationExcludeFromAuto(recordRes)
  };
}

function makeProgramButtonStateOverride_(payload, mode, reserved, enabled){
  const fixedMode = normalizeMode(mode);
  const state = getProgramButtonStateFromPayload(payload);

  if (fixedMode === 'watch') {
    state.watchReserved = !!reserved;
    state.watchEnabled = enabled !== false;
    state.watchDisabledByUser = reserved && enabled === false;
    state.watchExcludeFromAuto = reserved && enabled === false;
  } else {
    state.recordReserved = !!reserved;
    state.recordEnabled = enabled !== false;
    state.recordDisabledByUser = reserved && enabled === false;
    state.recordExcludeFromAuto = reserved && enabled === false;
  }

  return state;
}

function refreshAllReservationVisuals(){
  refreshReservationViewsAfterSwitch();
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core2_ToggleProgram_(std::wstring& js)
        {
            js += LR"JS(
function toggleProgramReservationByPayload(payload, mode){
  const fixedMode = normalizeMode(mode);
  const existing = findProgramReservation(payload, fixedMode);
  if (!existing && getProgramBroadcastState_(payload) === 'finished') {
    return { ok:false, reason:'program-already-finished' };
  }

  reservationSwitchDebug_('program-toggle-begin', {
    mode: fixedMode,
    existing: reservationSwitchCompactEntry_(existing),
    payload: reservationSwitchCompactPayload_(payload)
  });
epgPortalTracePush_('parent', 'program-toggle-begin', {
  mode: fixedMode,
  existing: reservationSwitchCompactEntry_(existing),
  payload: reservationSwitchCompactPayload_(payload)
});

  if (existing) {
    const nativeBackend = getNativeReservationBackend();
    const removePayload = buildProgramReservationPayload(
      payload,
      fixedMode,
      existing && Number.isFinite(Number(existing.preferredTunerIndex))
        ? Number(existing.preferredTunerIndex)
        : -1,
      {
        enabled: getReservationEnabled(existing),
        disabledByUser: getReservationDisabledByUser(existing),
        excludeFromAuto: getReservationExcludeFromAuto(existing),
        autoReserved: !!(existing && existing.autoReserved)
      }
    );

    if (nativeBackend &&
        typeof nativeBackend.removeProgramReservation === 'function') {
      const result = nativeBackend.removeProgramReservation(removePayload);
      if (!result || result.ok === false) {
        console.error('[reservation-switch] remove failed', result, removePayload);
        return result || { ok:false, reason:'native-remove-failed' };
      }

      removeProgramReservation(payload, fixedMode);
      rememberReservationMutationRemove_(payload, fixedMode, existing);

const refreshed = refreshReservationListAfterMutation_(
  result,
  'program_remove_' + fixedMode,
  {
    removeId:String(existing.id || existing.dedupeKey || removePayload.dedupeKey || ''),
    removeKey:String(existing.dedupeKey || removePayload.dedupeKey || ''),
    removeEntry:cloneReservationEntry(existing)
  }
);

reservationSwitchDebug_('program-toggle-remove-end', {
  mode: fixedMode,
  result: reservationSwitchCompactResult_(refreshed),
  payload: reservationSwitchCompactPayload_(payload),
  existing: reservationSwitchCompactEntry_(existing)
});
epgPortalTracePush_('parent', 'program-toggle-remove-end', {
  mode: fixedMode,
  result: reservationSwitchCompactResult_(refreshed),
  items: refreshed && Array.isArray(refreshed.items) ? refreshed.items.length : -1
});
completeReservationUiSyncAfterMutation_(
  'program-remove-' + fixedMode,
  refreshed
);

      refreshed.state = makeProgramButtonStateOverride_(
        payload,
        fixedMode,
        false,
        true
      );

      return refreshed;
    }

    removeProgramReservation(payload, fixedMode);
    refreshReservationViewsAfterSwitch();

    return {
      ok:true,
      source:'local-remove',
      state:getProgramButtonStateFromPayload(payload)
    };
  }

  const preferredTunerIndex = askPreferredTunerIndexForReservation(fixedMode);
  if (preferredTunerIndex === null) {
    return { ok:false, reason:'cancelled' };
  }

  const addPayload = buildProgramReservationPayload(
    payload,
    fixedMode,
    preferredTunerIndex,
    {
      enabled:true,
      disabledByUser:false,
      excludeFromAuto:false,
      autoReserved:false
    }
  );

  const nativeBackend = getNativeReservationBackend();
  if (nativeBackend &&
      typeof nativeBackend.upsertProgramReservation === 'function') {
    const result = nativeBackend.upsertProgramReservation(addPayload);
    if (!result || result.ok === false) {
      console.error('[reservation-switch] upsert failed', result, addPayload);
      return result || { ok:false, reason:'native-upsert-failed' };
    }

    const optimisticEntry = makeProgramReservationEntry(
  payload,
  fixedMode,
  preferredTunerIndex
);

upsertProgramReservationEntry(optimisticEntry);
rememberReservationMutationEntry_(optimisticEntry);

const refreshed = refreshReservationListAfterMutation_(
  result,
  'program_upsert_' + fixedMode,
  {
    upsertEntry: optimisticEntry
  }
);
reservationSwitchDebug_('program-toggle-upsert-end', {
  mode: fixedMode,
  result: reservationSwitchCompactResult_(refreshed),
  optimisticEntry: reservationSwitchCompactEntry_(optimisticEntry),
  payload: reservationSwitchCompactPayload_(payload)
});
epgPortalTracePush_('parent', 'program-toggle-upsert-end', {
  mode: fixedMode,
  result: reservationSwitchCompactResult_(refreshed),
  items: refreshed && Array.isArray(refreshed.items) ? refreshed.items.length : -1
});
completeReservationUiSyncAfterMutation_(
  'program-upsert-' + fixedMode,
  refreshed
);

    refreshed.state = makeProgramButtonStateOverride_(
      payload,
      fixedMode,
      true,
      true
    );

    return refreshed;
  }

  upsertProgramReservationEntry(
    makeProgramReservationEntry(payload, fixedMode, preferredTunerIndex)
  );
  refreshReservationViewsAfterSwitch();

  return {
    ok:true,
    source:'local-upsert',
    state:getProgramButtonStateFromPayload(payload)
  };
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core2_ToggleDisable_(std::wstring& js)
        {
            js += LR"JS(
function toggleProgramReservationDisableByPayload(payload, mode){
  const fixedMode = normalizeMode(mode);

  const existing = findProgramReservation(payload, fixedMode);

  reservationSwitchDebug_('program-disable-toggle-begin', {
    mode: fixedMode,
    existing: reservationSwitchCompactEntry_(existing),
    payload: reservationSwitchCompactPayload_(payload)
  });

  if (!existing) {
    return { ok:false, reason:'not-found' };
  }

  const nextEnabled = !getReservationEnabled(existing);
  const nextDisabledByUser = nextEnabled ? false : true;
  const nextExcludeFromAuto = nextEnabled ? false : true;

  const changePayload = buildProgramReservationPayload(
    payload,
    fixedMode,
    existing && Number.isFinite(Number(existing.preferredTunerIndex))
      ? Number(existing.preferredTunerIndex)
      : -1,
    {
      enabled: nextEnabled,
      disabledByUser: nextDisabledByUser,
      excludeFromAuto: nextExcludeFromAuto,
      autoReserved: !!(existing && existing.autoReserved)
    });

  const nativeBackend = getNativeReservationBackend();
  if (nativeBackend &&
      typeof nativeBackend.setProgramReservationEnabled === 'function') {
    const result = nativeBackend.setProgramReservationEnabled(
      changePayload,
      nextEnabled,
      nextDisabledByUser,
      nextExcludeFromAuto
    );
    if (!result || result.ok === false) {
      console.error('[reservation-switch] set_enabled failed', result, changePayload);
      return result || { ok:false, reason:'native-set-enabled-failed' };
    }

    const optimisticEntry = cloneReservationEntry(existing);
optimisticEntry.enabled = nextEnabled;
optimisticEntry.disabledByUser = nextDisabledByUser;
optimisticEntry.excludeFromAuto = nextExcludeFromAuto;

upsertProgramReservationEntry(optimisticEntry);
rememberReservationMutationEntry_(optimisticEntry);

const refreshed = refreshReservationListAfterMutation_(
  result,
  'program_set_enabled_' + fixedMode,
  {
    patchId:String(existing.id || existing.dedupeKey || changePayload.dedupeKey || ''),
    patch:{
      enabled:nextEnabled,
      disabledByUser:nextDisabledByUser,
      excludeFromAuto:nextExcludeFromAuto
    }
  }
);
reservationSwitchDebug_('program-disable-toggle-end', {
  mode: fixedMode,
  nextEnabled: nextEnabled,
  result: reservationSwitchCompactResult_(refreshed),
  existing: reservationSwitchCompactEntry_(existing),
  payload: reservationSwitchCompactPayload_(payload)
});
completeReservationUiSyncAfterMutation_(
  'program-toggle-disable-' + fixedMode,
  refreshed
);
    refreshed.state = makeProgramButtonStateOverride_(
      payload,
      fixedMode,
      true,
      nextEnabled
    );

    return refreshed;
  }

  const next = cloneReservationEntry(existing);
  next.enabled = nextEnabled;
  next.disabledByUser = nextDisabledByUser;
  next.excludeFromAuto = nextExcludeFromAuto;
  upsertProgramReservationEntry(next);
  refreshAllReservationVisuals();

  return {
    ok:true,
    state:getProgramButtonStateFromPayload(payload)
  };
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core2_Bridge_(std::wstring& js)
        {
            js += LR"JS(
function publishSwitchProgramTableBridge_(){
  try {
    if (window.top && window.top !== window) {
      console.warn('[reservation-switch] skip bridge publish from iframe runtime', {
        href:String(location.href || ''),
        pathname:String(location.pathname || '')
      });
      return;
    }
  } catch (_) {
    return;
  }

  const bridge = {
    __epgSwitchBridge: true,

    getState: function(payload){
      epgPortalTracePush_('parent', 'switch-bridge-get-state-begin', {
        payload: reservationSwitchCompactPayload_(payload)
      });

      const state = getProgramButtonStateFromPayload(payload);

      epgPortalTracePush_('parent', 'switch-bridge-get-state-end', {
        watchReserved: !!(state && state.watchReserved),
        recordReserved: !!(state && state.recordReserved),
        watchEnabled: state && state.watchEnabled,
        recordEnabled: state && state.recordEnabled
      });

      return state;
    },

    toggle: function(payload, mode){
      const fixedMode = normalizeMode(mode);

      try {
        console.log('[EPG-SWITCH-BRIDGE] toggle enter', {
          mode: fixedMode,
          payload: reservationSwitchCompactPayload_(payload)
        });
      } catch (_) {
      }

      const result = toggleProgramReservationByPayload(payload, fixedMode);

      try {
        console.log('[EPG-SWITCH-BRIDGE] toggle leave', {
          mode: fixedMode,
          result: reservationSwitchCompactResult_(result)
        });
      } catch (_) {
      }

      return result;
    },

    toggleDisable: function(payload, mode){
      const fixedMode = normalizeMode(mode);
      return toggleProgramReservationDisableByPayload(payload, fixedMode);
    }
  };

  window.__epgPortalSwitchProgramTableBridge = bridge;
  window.__epgPortalProgramTableBridge = bridge;
}

function publishProgramTableBridge(){
  publishSwitchProgramTableBridge_();
}
)JS";
        }

        void AppendRuntimeSwitchScript_Core2_(std::wstring& js)
        {
            AppendRuntimeSwitchScript_Core2_Entry_(js);
            AppendRuntimeSwitchScript_Core2_Find_(js);
            AppendRuntimeSwitchScript_Core2_FindShared_(js);
            AppendRuntimeSwitchScript_Core2_State_(js);
            AppendRuntimeSwitchScript_Core2_ToggleProgram_(js);
            AppendRuntimeSwitchScript_Core2_ToggleDisable_(js);
            AppendRuntimeSwitchScript_Core2_Bridge_(js);
        }

        void AppendRuntimeSwitchScript_UI1_(std::wstring& js)
        {
            js += LR"JS(

function getProgramBroadcastState_(payload){
  if (!payload) return 'unknown';
  const parse = (dateText, timeText) => {
    const d = /^(\d{4})[-\/](\d{1,2})[-\/](\d{1,2})$/.exec(String(dateText || '').trim());
    const t = /^(\d{1,2}):(\d{2})(?::(\d{2}))?/.exec(String(timeText || '').trim());
    if (!d || !t) return NaN;
    const y = +d[1], mo = +d[2], day = +d[3], h = +t[1], mi = +t[2], sec = +(t[3] || 0);
    if (mo < 1 || mo > 12 || day < 1 || day > 31 || h > 23 || mi > 59 || sec > 59) return NaN;
    const dt = new Date(y, mo - 1, day, h, mi, sec);
    if (dt.getFullYear() !== y || dt.getMonth() !== mo - 1 || dt.getDate() !== day) return NaN;
    return dt.getTime();
  };
  const date = payload.startDate || parseDateTextToIso(payload.dateText || payload.date || '');
  const range = parseTimeTextParts(payload.displayTimeText || payload.timeText || payload.time || '');
  const start = parse(date, payload.startTime || range.startTime);
  let endDate = payload.endDate || date;
  let endTime = payload.endTime || range.endTime;
  if (!payload.endDate && range.crossesDay) endDate = shiftIsoDate(date, 1) || date;
  let end = parse(endDate, endTime);
  if (Number.isFinite(start) && Number.isFinite(end) && end <= start && !payload.endDate && endTime) {
    end = parse(shiftIsoDate(date, 1), endTime);
  }
  if ((!Number.isFinite(end) || end <= start) && Number(payload.durationSec) > 0 && Number.isFinite(start)) {
    end = start + Number(payload.durationSec) * 1000;
  }
  if (!Number.isFinite(start) || !Number.isFinite(end) || end <= start) return 'unknown';
  const now = Date.now();
  return now < start ? 'before' : (now < end ? 'onair' : 'finished');
}

function buildReservationSwitchButtonsHtml(index, item){
  const watchRes = findProgramReservation(item, 'watch');
  const recordRes = findProgramReservation(item, 'record');

  const watchEnabled = getReservationEnabled(watchRes);
  const recordEnabled = getReservationEnabled(recordRes);

  const broadcastState = getProgramBroadcastState_(item);
  const watchBlocked = !watchRes && broadcastState === 'finished';
  const recordBlocked = !recordRes && broadcastState === 'finished';
  const watchLabel = watchRes ? '視聴取消' : (watchBlocked ? '放送終了' : (broadcastState === 'onair' ? '途中から視聴' : '視聴予約'));
  const recordLabel = recordRes ? '録画取消' : (recordBlocked ? '放送終了' : (broadcastState === 'onair' ? '途中から録画' : '録画予約'));

  let html = ''
    + '<div class="reservation-switch-group">'
    + '<button type="button" class="reservation-switch-btn reservation-switch-watch'
    + (watchRes ? ' on' : '')
    + (watchRes && !watchEnabled ? ' disabled-state' : '')
    + '"' + (watchBlocked ? ' disabled' : '') + ' onclick="requestWatchReservation(' + index + ');event.stopPropagation();">'
    + escapeHtml(watchLabel)
    + '</button>'
    + '<button type="button" class="reservation-switch-btn reservation-switch-record'
    + (recordRes ? ' on' : '')
    + (recordRes && !recordEnabled ? ' disabled-state' : '')
    + '"' + (recordBlocked ? ' disabled' : '') + ' onclick="requestRecordReservation(' + index + ');event.stopPropagation();">'
    + escapeHtml(recordLabel)
    + '</button>';

  if (watchRes) {
    html += ''
      + '<button type="button" class="reservation-switch-btn reservation-switch-disable'
      + (watchEnabled ? '' : ' on')
      + '" onclick="toggleWatchReservationDisabled(' + index + ');event.stopPropagation();">'
      + escapeHtml(watchEnabled ? '視聴無効化' : '視聴有効化')
      + '</button>';
  }

  if (recordRes) {
    html += ''
      + '<button type="button" class="reservation-switch-btn reservation-switch-disable'
      + (recordEnabled ? '' : ' on')
      + '" onclick="toggleRecordReservationDisabled(' + index + ');event.stopPropagation();">'
      + escapeHtml(recordEnabled ? '録画無効化' : '録画有効化')
      + '</button>';
  }

  html += '</div>';
  return html;
}

function applyReservationRowMarker(row, item){
  if (!row) return;

  row.classList.remove('reserved-watch');
  row.classList.remove('reserved-record');
  row.classList.remove('reserved-watch-disabled');
  row.classList.remove('reserved-record-disabled');

  const watchRes = findProgramReservation(item, 'watch');
  const recordRes = findProgramReservation(item, 'record');

  if (recordRes) {
    if (getReservationEnabled(recordRes)) {
      row.classList.add('reserved-record');
    } else {
      row.classList.add('reserved-record-disabled');
    }
    return;
  }

  if (watchRes) {
    if (getReservationEnabled(watchRes)) {
      row.classList.add('reserved-watch');
    } else {
      row.classList.add('reserved-watch-disabled');
    }
  }
}

function syncProgramReservationButtons(){
  const wrap = document.getElementById('searchResultsWrap');
  if (!wrap) return;

  const table = wrap.querySelector('table.result-table');
  if (!table) return;

  const headRow = table.querySelector('thead tr');
  if (headRow) {
    const ths = headRow.querySelectorAll('th');
    if (ths.length < 8) {
      const th = document.createElement('th');
      th.textContent = '予約';
      headRow.appendChild(th);
    } else {
      const lastTh = ths[ths.length - 1];
      if (lastTh && lastTh.textContent !== '予約') {
        const th = document.createElement('th');
        th.textContent = '予約';
        headRow.appendChild(th);
      }
    }
  }

  const lastResults = getPortalLastResultsSafe_();

table.querySelectorAll('tbody tr.result-row').forEach(row => {
  const index = Number(row.getAttribute('data-result-index'));
  const item = (Number.isFinite(index) && index >= 0 && index < lastResults.length)
    ? lastResults[index]
    : null;
  if (!item) return;

    let cell = row.querySelector('td.reservation-switch-cell');
    if (!cell) {
      cell = document.createElement('td');
      cell.className = 'reservation-switch-cell';
      row.appendChild(cell);
    }

    cell.innerHTML = buildReservationSwitchButtonsHtml(index, item);
    applyReservationRowMarker(row, item);
  });
}
)JS";
        }

        void AppendRuntimeSwitchScript_UI2_Detail_(std::wstring& js)
        {
            js += LR"JS(
function syncDetailReservationButtons(){
  const actions = document.getElementById('detailActions');
  if (!actions) return;

  const selectedProgram = getPortalSelectedProgramSafe_();

  if (!selectedProgram) {
    actions.innerHTML = '';
    return;
  }

  const results = getPortalLastResultsSafe_();
  const index = results.indexOf(selectedProgram);

  if (index < 0) {
    actions.innerHTML = '';
    return;
  }

  actions.innerHTML = buildReservationSwitchButtonsHtml(index, selectedProgram);

  const states = [];
  const watchRes = findProgramReservation(selectedProgram, 'watch');
  const recordRes = findProgramReservation(selectedProgram, 'record');

  if (watchRes) {
    states.push(
      getReservationEnabled(watchRes)
        ? ('視聴予約 ' + formatReservationTunerLabel(watchRes))
        : ('視聴予約(無効) ' + formatReservationTunerLabel(watchRes))
    );
  }

  if (recordRes) {
    states.push(
      getReservationEnabled(recordRes)
        ? ('録画予約 ' + formatReservationTunerLabel(recordRes))
        : ('録画予約(無効) ' + formatReservationTunerLabel(recordRes))
    );
  }

  setText(
    'detailReservationState',
    states.length > 0
      ? ('予約状態: ' + states.join(' / '))
      : '予約状態: 未確認'
  );
}
)JS";
        }

        void AppendRuntimeSwitchScript_UI2_PageDetect_(std::wstring& js)
        {
            js += LR"JS(
function isElementVisibleForReservationRefresh_(el){
  if (!el) return false;

  try {
    const style = window.getComputedStyle(el);
    if (!style) return false;
    if (style.display === 'none' || style.visibility === 'hidden') return false;

    const rect = el.getBoundingClientRect();
    if (rect.width <= 0 && rect.height <= 0) return false;

    return true;
  } catch (_) {
    return false;
  }
}

function isReservationPageActive_(){
  const selectors = [
    '#reservationPage',
    '#reservationListPage',
    '#reservationListView',
    '#reservationListWrap',
    '#reservationPageWrap',
    '#pageReservation',
    '#page-reservation',
    '[data-page="reservation"]',
    '[data-view="reservation"]'
  ];

  for (const sel of selectors) {
    try {
      const el = document.querySelector(sel);
      if (isElementVisibleForReservationRefresh_(el)) {
        return true;
      }
    } catch (_) {
    }
  }

  try {
    const keys = [
      'gCurrentPage',
      'gActivePage',
      'gCurrentView',
      'gActiveView',
      'currentPage',
      'activePage'
    ];

    for (const key of keys) {
      if (typeof window[key] !== 'undefined') {
        const v = String(window[key] || '').toLowerCase();
        if (v.indexOf('reservation') >= 0 ||
            v.indexOf('reserve') >= 0 ||
            v.indexOf('予約') >= 0) {
          return true;
        }
      }
    }
  } catch (_) {
  }

  return false;
}

function isSearchPageActive_(){
  try {
    const wrap = document.getElementById('searchResultsWrap');
    if (isElementVisibleForReservationRefresh_(wrap)) {
      return true;
    }
  } catch (_) {
  }

  return false;
}
)JS";
        }

        void AppendRuntimeSwitchScript_UI2_RefreshParts_(std::wstring& js)
        {
            js += LR"JS(
function refreshReservationListViewOnly_(options){
  const opt = options || {};

  if (!isReservationPageActive_()) {
    return false;
  }

  const skipCacheSync =
    opt.skipCacheSync === true ||
    opt.preferLocalState === true ||
    isReservationLocalMutationHolding_();

  if (!skipCacheSync) {
    try {
      syncSharedReservationListFromViewCache_();
    } catch (e) {
      console.warn('[reservation-switch] sync view cache for reservation list failed', e);
    }
  }

  try {
    if (typeof renderReservationPage === 'function') {
      renderReservationPage();
      return true;
    }
  } catch (e) {
    console.error('[reservation-switch] renderReservationPage failed', e);
  }

  return false;
}

function refreshSearchReservationButtonsOnly_(){
  if (!isSearchPageActive_()) {
    return false;
  }

  try {
    if (typeof syncProgramReservationButtons === 'function') {
      syncProgramReservationButtons();
      return true;
    }
  } catch (e) {
    console.error('[reservation-switch] syncProgramReservationButtons failed', e);
  }

  return false;
}

function refreshDetailReservationButtonsOnly_(){
  try {
    if (typeof syncDetailReservationButtons === 'function') {
      syncDetailReservationButtons();
      return true;
    }
  } catch (e) {
    console.error('[reservation-switch] syncDetailReservationButtons failed', e);
  }

  return false;
}
)JS";
        }

        void AppendRuntimeSwitchScript_UI2_FrameRefresh_(std::wstring& js)
        {
            js += LR"JS(
let gProgramTableFramesRefreshQueued_ = false;

function refreshProgramTableFramesOnly_(){
  if (gProgramTableFramesRefreshQueued_) {
    return;
  }

  gProgramTableFramesRefreshQueued_ = true;

  const runner = function(){
    gProgramTableFramesRefreshQueued_ = false;

    try {
      document.querySelectorAll('iframe').forEach(frame => {
        try {
          const cw = frame.contentWindow;
          if (!cw) return;

          if (typeof cw.__epgPortalProgramTableRenderAll === 'function') {
            cw.__epgPortalProgramTableRenderAll();
          } else if (typeof cw.__epgPortalProgramTableRenderNow === 'function') {
            cw.__epgPortalProgramTableRenderNow();
          }
        } catch (_) {
        }
      });
    } catch (_) {
    }
  };

  if (typeof window.requestAnimationFrame === 'function') {
    window.requestAnimationFrame(runner);
  } else {
    window.setTimeout(runner, 0);
  }
}
)JS";
        }

        void AppendRuntimeSwitchScript_UI2_VisibleFrameSync_(std::wstring& js)
        {
            js += LR"JS(
function flushVisibleProgramTableFramePendingRenders_(reason, forceRender){
  let flushed = 0;

  try {
    document.querySelectorAll('iframe').forEach(frame => {
      try {
        if (!isElementVisibleForReservationRefresh_(frame)) {
          return;
        }

        const cw = frame.contentWindow;
        if (!cw) {
          return;
        }

        if (typeof cw.__epgPortalProgramTableFlushPendingReservationRender === 'function') {
          if (cw.__epgPortalProgramTableFlushPendingReservationRender(reason || 'parent-flush')) {
            ++flushed;
            return;
          }
        }

        if (forceRender === true &&
            typeof cw.__epgPortalProgramTableRenderNow === 'function') {
          cw.__epgPortalProgramTableRenderNow();
          ++flushed;
        }
      } catch (_) {
      }
    });
  } catch (_) {
  }

  return flushed;
}

let gReservationUiSyncQueued_ = false;
let gReservationUiSyncNeedNotifyFrames_ = false;
let gReservationUiSyncReason_ = '';

function makeReservationUiSyncOptions_(notifyFrames){
  return {
    syncCache:false,
    scheduleRetry:false,
    skipCacheSync:true,
    preferLocalState:true,
    refreshProgramFrames:false,
    notifyFrames:notifyFrames === true
  };
}

function queueReservationUiSync_(reason, notifyFrames){
  gReservationUiSyncNeedNotifyFrames_ =
    gReservationUiSyncNeedNotifyFrames_ || notifyFrames === true;

  gReservationUiSyncReason_ = String(reason || gReservationUiSyncReason_ || 'ui-sync');

  if (gReservationUiSyncQueued_) {
    return;
  }

  gReservationUiSyncQueued_ = true;

  const runner = function(){
    const useNotifyFrames = gReservationUiSyncNeedNotifyFrames_;
    const useReason = gReservationUiSyncReason_;

    gReservationUiSyncQueued_ = false;
    gReservationUiSyncNeedNotifyFrames_ = false;
    gReservationUiSyncReason_ = '';

    epgPortalTracePush_('parent', 'ui-sync-run', {
      reason:useReason,
      notifyFrames:useNotifyFrames
    });

    try {
      refreshReservationViewsAfterSwitchOnce_(
        makeReservationUiSyncOptions_(useNotifyFrames)
      );
    } catch (e) {
      console.warn('[reservation-switch] ui sync failed', e);
      epgPortalTracePush_('parent', 'ui-sync-error', {
        reason:useReason,
        message:e && e.message ? e.message : String(e)
      });
    }
  };

  try {
    if (typeof window.requestAnimationFrame === 'function') {
      window.requestAnimationFrame(runner);
    } else {
      window.setTimeout(runner, 0);
    }
  } catch (_) {
    runner();
  }
}

let gReservationUiConfirmTimer_ = 0;

function scheduleReservationUiConfirm_(reason){
  try {
    if (gReservationUiConfirmTimer_) {
      window.clearTimeout(gReservationUiConfirmTimer_);
      gReservationUiConfirmTimer_ = 0;
    }

    gReservationUiConfirmTimer_ = window.setTimeout(function(){
      gReservationUiConfirmTimer_ = 0;

      epgPortalTracePush_('parent', 'ui-sync-confirm', {
        reason:String(reason || '')
      });

      queueReservationUiSync_(
  String(reason || 'ui-sync') + '-confirm',
  true
);

flushVisibleProgramTableFramePendingRenders_(
  String(reason || 'ui-sync') + '-confirm',
  true
);
    }, 360);
  } catch (_) {
  }
}

function completeReservationUiSyncAfterMutation_(reason, result){
  epgPortalTracePush_('parent', 'complete-ui-sync-begin', {
    reason:String(reason || ''),
    result:reservationSwitchCompactResult_(result)
  });

  try {
    markReservationLocalMutation_();
  } catch (_) {
  }

  try {
    invalidateReservationStateCache();
  } catch (_) {
  }

  queueReservationUiSync_(reason || 'mutation', true);
  scheduleReservationUiConfirm_(reason || 'mutation');

  epgPortalTracePush_('parent', 'complete-ui-sync-end', {
    reason:String(reason || '')
  });
}

window.__epgPortalQueueReservationUiSync = function(reason, notifyFrames){
  queueReservationUiSync_(reason || 'external', notifyFrames === true);
  return true;
};

window.__epgPortalCompleteReservationUiSyncAfterMutation =
  completeReservationUiSyncAfterMutation_;

window.__epgPortalFlushVisibleProgramTableFramePendingRenders =
  flushVisibleProgramTableFramePendingRenders_;
)JS";
        }

        void AppendRuntimeSwitchScript_UI2_NavigationSync_(std::wstring& js)
        {
            js += LR"JS(
let gReservationSwitchNavigationRefreshBound_ = false;

function bindReservationSwitchNavigationRefresh_(){
  if (gReservationSwitchNavigationRefreshBound_) {
    return;
  }

  gReservationSwitchNavigationRefreshBound_ = true;

  document.addEventListener('click', function(ev){
    try {
      const target = ev && ev.target ? ev.target : null;
      if (!target || !target.closest) {
        return;
      }

      const hit = target.closest(
        'a,button,[data-page],[data-view],[data-tab],[data-target],[data-epg-page]'
      );

      if (!hit) {
        return;
      }

      window.setTimeout(function(){
        try {
          queueReservationUiSync_('navigation-click', false);
          flushVisibleProgramTableFramePendingRenders_('navigation-click', false);
        } catch (_) {
        }
      }, 80);
    } catch (_) {
    }
  }, true);
}
)JS";
        }

        void AppendRuntimeSwitchScript_UI2_Notify_(std::wstring& js)
        {
            js += LR"JS(
function notifyReservationListChanged_(options){
  const opt = options || {};
  const notifyFrames = opt.notifyFrames === true;

  const detail = {
    stamp: Date.now(),
    items: getSharedReservationList_(),
    source: String(opt.source || 'reservation-switch'),
    notifyFrames: notifyFrames
  };
epgPortalTracePush_('parent', 'notify-reservation-list-changed', {
  notifyFrames: notifyFrames,
  items: Array.isArray(detail.items) ? detail.items.length : -1,
  iframeCount: document.querySelectorAll('iframe').length
});

  try {
    document.dispatchEvent(new CustomEvent('epg-reservation-list-changed', {
      detail: detail
    }));
  } catch (_) {
  }

  try {
    window.dispatchEvent(new CustomEvent('epg-reservation-list-changed', {
      detail: detail
    }));
  } catch (_) {
  }

  // 重要:
  // 既定では iframe へ直接イベントを投げない。
  // 番組表 iframe 側の全セル再描画が重いため、
  // 必要な場合だけ notifyFrames:true で明示する。
  if (!notifyFrames) {
    return;
  }

  try {
    document.querySelectorAll('iframe').forEach(frame => {
      try {
        const cw = frame.contentWindow;
        const cd = frame.contentDocument;

        if (!cw || !cd) {
          return;
        }

        try {
          cd.dispatchEvent(new CustomEvent('epg-reservation-list-changed', {
            detail: detail
          }));
        } catch (_) {
        }

        try {
          cw.dispatchEvent(new CustomEvent('epg-reservation-list-changed', {
            detail: detail
          }));
        } catch (_) {
        }
      } catch (_) {
      }
    });
  } catch (_) {
  }
}
)JS";
        }

        void AppendRuntimeSwitchScript_UI2_RefreshQueue_(std::wstring& js)
        {
            js += LR"JS(
let gReservationViewsRefreshQueued_ = false;
let gReservationViewsRefreshTimer_ = 0;

function refreshReservationViewsAfterSwitchOnce_(options){
  const opt = options || {};

  epgPortalTracePush_('parent', 'refresh-views-once-begin', {
    syncCache: opt.syncCache === true,
    notifyFrames: opt.notifyFrames === true,
    refreshProgramFrames: opt.refreshProgramFrames === true,
    skipCacheSync: opt.skipCacheSync === true,
    preferLocalState: opt.preferLocalState === true
  });

  if (opt.syncCache === true) {
    try {
      epgPortalTracePush_('parent', 'refresh-sync-cache-begin', {});
      if (typeof syncSharedReservationListFromViewCache_ === 'function') {
        syncSharedReservationListFromViewCache_();
      }
      epgPortalTracePush_('parent', 'refresh-sync-cache-end', {});
    } catch (e) {
      console.warn('[reservation-switch] sync cache before refresh failed', e);
      epgPortalTracePush_('parent', 'refresh-sync-cache-error', {
        message: e && e.message ? e.message : String(e)
      });
    }
  }

  try {
    epgPortalTracePush_('parent', 'refresh-reservation-list-begin', {});
    refreshReservationListViewOnly_(opt);
    epgPortalTracePush_('parent', 'refresh-reservation-list-end', {});
  } catch (e) {
    console.warn('[reservation-switch] refreshReservationListViewOnly_ failed', e);
    epgPortalTracePush_('parent', 'refresh-reservation-list-error', {
      message: e && e.message ? e.message : String(e)
    });
  }

  try {
    epgPortalTracePush_('parent', 'refresh-search-buttons-begin', {});
    refreshSearchReservationButtonsOnly_();
    epgPortalTracePush_('parent', 'refresh-search-buttons-end', {});
  } catch (e) {
    console.warn('[reservation-switch] refreshSearchReservationButtonsOnly_ failed', e);
    epgPortalTracePush_('parent', 'refresh-search-buttons-error', {
      message: e && e.message ? e.message : String(e)
    });
  }

  try {
    epgPortalTracePush_('parent', 'refresh-detail-buttons-begin', {});
    refreshDetailReservationButtonsOnly_();
    epgPortalTracePush_('parent', 'refresh-detail-buttons-end', {});
  } catch (e) {
    console.warn('[reservation-switch] refreshDetailReservationButtonsOnly_ failed', e);
    epgPortalTracePush_('parent', 'refresh-detail-buttons-error', {
      message: e && e.message ? e.message : String(e)
    });
  }

  try {
    epgPortalTracePush_('parent', 'notify-reservation-list-begin', {
      notifyFrames: opt.notifyFrames === true || opt.refreshProgramFrames === true
    });

    notifyReservationListChanged_({
      source: 'reservation-switch',
      notifyFrames: opt.notifyFrames === true || opt.refreshProgramFrames === true
    });

    epgPortalTracePush_('parent', 'notify-reservation-list-end', {});
  } catch (e) {
    console.warn('[reservation-switch] notifyReservationListChanged_ failed', e);
    epgPortalTracePush_('parent', 'notify-reservation-list-error', {
      message: e && e.message ? e.message : String(e)
    });
  }

  if (opt.refreshProgramFrames === true) {
    try {
      epgPortalTracePush_('parent', 'refresh-program-frames-begin', {});
      refreshProgramTableFramesOnly_();
      epgPortalTracePush_('parent', 'refresh-program-frames-end', {});
    } catch (e) {
      console.warn('[reservation-switch] refreshProgramTableFramesOnly_ failed', e);
      epgPortalTracePush_('parent', 'refresh-program-frames-error', {
        message: e && e.message ? e.message : String(e)
      });
    }
  }

  epgPortalTracePush_('parent', 'refresh-views-once-end', {});
}

function scheduleReservationViewsRefresh_(delayMs, options){
  try {
    if (gReservationViewsRefreshTimer_) {
      window.clearTimeout(gReservationViewsRefreshTimer_);
      gReservationViewsRefreshTimer_ = 0;
    }

    gReservationViewsRefreshTimer_ = window.setTimeout(function(){
      gReservationViewsRefreshTimer_ = 0;

      try {
        refreshReservationViewsAfterSwitchOnce_(options || {});
      } catch (e) {
        console.warn('[reservation-switch] scheduled refresh failed', e);
      }
    }, delayMs);
  } catch (_) {
  }
}

function refreshReservationViewsAfterSwitch(options){
  const opt = options || {};

  if (gReservationViewsRefreshQueued_) {
    if (opt.scheduleRetry !== false) {
      scheduleReservationViewsRefresh_(120, {
  syncCache:false,
  skipCacheSync: opt.skipCacheSync === true || opt.preferLocalState === true,
  preferLocalState: opt.preferLocalState === true,
  notifyFrames: opt.notifyFrames === true,
  refreshProgramFrames: opt.refreshProgramFrames === true
});
    }
    return;
  }

  gReservationViewsRefreshQueued_ = true;

  const runner = function(){
    gReservationViewsRefreshQueued_ = false;

    try {
      refreshReservationViewsAfterSwitchOnce_(opt);
    } catch (e) {
      console.warn('[reservation-switch] refresh failed', e);
    }

    if (opt.scheduleRetry !== false) {
      scheduleReservationViewsRefresh_(120, {
  syncCache:false,
  skipCacheSync: opt.skipCacheSync === true || opt.preferLocalState === true,
  preferLocalState: opt.preferLocalState === true,
  notifyFrames: opt.notifyFrames === true,
  refreshProgramFrames: opt.refreshProgramFrames === true
});
    }
  };

  try {
    if (typeof window.requestAnimationFrame === 'function') {
      window.requestAnimationFrame(runner);
    } else {
      window.setTimeout(runner, 0);
    }
  } catch (_) {
    runner();
  }
}
)JS";
        }

        void AppendRuntimeSwitchScript_UI2_PostOperation_(std::wstring& js)
        {
            js += LR"JS(
function refreshReservationViewsAfterOperationComplete_(reason){
  epgPortalTracePush_('parent', 'operation-complete-refresh-request', {
    reason:String(reason || '')
  });

  refreshReservationViewsAfterSwitch({
    syncCache:false,
    scheduleRetry:false,
    skipCacheSync:true,
    preferLocalState:true,
    refreshProgramFrames:false,
    notifyFrames:true
  });

  try {
    window.setTimeout(function(){
      try {
        refreshReservationViewsAfterSwitch({
          syncCache:false,
          scheduleRetry:false,
          skipCacheSync:true,
          preferLocalState:true,
          refreshProgramFrames:false,
          notifyFrames:true
        });

        flushVisibleProgramTableFramePendingRenders_('operation-complete-delay');
      } catch (_) {
      }
    }, 360);
  } catch (_) {
  }
}
)JS";
        }

        void AppendRuntimeSwitchScript_UI2_RequestFns_(std::wstring& js)
        {
            js += LR"JS(
function requestWatchReservation(index){
  const item = getPortalLastResultAtIndex_(index);
  if (!item) return;

  toggleProgramReservationByPayload(item, 'watch');
}

function requestRecordReservation(index){
  const item = getPortalLastResultAtIndex_(index);
  if (!item) return;

  toggleProgramReservationByPayload(item, 'record');
}

function toggleWatchReservationDisabled(index){
  const item = getPortalLastResultAtIndex_(index);
  if (!item) return;

  toggleProgramReservationDisableByPayload(item, 'watch');
}

function toggleRecordReservationDisabled(index){
  const item = getPortalLastResultAtIndex_(index);
  if (!item) return;

  toggleProgramReservationDisableByPayload(item, 'record');
}
)JS";
        }

        void AppendRuntimeSwitchScript_UI2_(std::wstring& js)
        {
            AppendRuntimeSwitchScript_UI2_Detail_(js);
            AppendRuntimeSwitchScript_UI2_PageDetect_(js);
            AppendRuntimeSwitchScript_UI2_RefreshParts_(js);
            AppendRuntimeSwitchScript_UI2_FrameRefresh_(js);
            AppendRuntimeSwitchScript_UI2_VisibleFrameSync_(js);
            AppendRuntimeSwitchScript_UI2_NavigationSync_(js);
            AppendRuntimeSwitchScript_UI2_Notify_(js);
            AppendRuntimeSwitchScript_UI2_RefreshQueue_(js);
            AppendRuntimeSwitchScript_UI2_PostOperation_(js);
            AppendRuntimeSwitchScript_UI2_RequestFns_(js);
        }

        void AppendRuntimeSwitchScript_ListOps_Mode_(std::wstring& js)
        {
            js += LR"JS(
function normalizeReservationListMode_(mode){
  const v = String(mode || '').toLowerCase().trim();

  if (v === 'watch') return 'watch';
  if (v === 'record') return 'record';

  if (v === 'epg' ||
      v === 'epg_full' ||
      v === 'epg_light' ||
      v.indexOf('epg_') === 0) {
    return 'epg';
  }

  return v || 'task';
}

function isFixedEpgConfigTask_(entry){
  if (!entry) return false;

  const mode = normalizeReservationListMode_(
    entry.mode || entry.reserveMode || entry.taskType || ''
  );

  const kind = toSafeInt(entry.kind, 0);
  const instance = toSafeInt(entry.instance, 0);

  if (mode !== 'epg') {
    return false;
  }

  return (kind === 1 || kind === 2) && instance <= 0;
}

function canMutateReservationListEntry_(entry){
  if (!entry) return false;

  if (isFixedEpgConfigTask_(entry)) {
    return false;
  }

  const id = String(entry.id || '');
  if (!id) {
    return false;
  }

  return true;
}

function findReservationListEntryById(reservationId){
  const id = String(reservationId || '');
  if (!id) return null;

  const list = getSharedReservationList_();
  for (let i = 0; i < list.length; ++i) {
    const item = list[i];
    if (String(item && item.id ? item.id : '') === id) {
      return item;
    }
  }

  return null;
}
)JS";
        }

        void AppendRuntimeSwitchScript_ListOps_Payload_(std::wstring& js)
        {
            js += LR"JS(
function buildReservationListPayload(entry){
  if (!entry) return null;

  const displayTimeText = normalizeTimeRangeForDisplay_(
    entry.displayTimeText || entry.timeText || entry.time || ''
  );

  const storageTimeText = normalizeTimeRangeForStorage_(
    displayTimeText || entry.timeText || entry.time || ''
  );

  const rawMode = normalizeReservationListMode_(
    entry.mode || entry.reserveMode || entry.taskType || ''
  );

  return {
    id: String(entry.id || ''),
    kind: toSafeInt(entry.kind, 0),
    instance: toSafeInt(entry.instance, 0),

    mode: rawMode,
    reserveMode: rawMode,

    dedupeKey: String(entry.dedupeKey || ''),
    excludeKey: String(entry.excludeKey || entry.dedupeKey || ''),

    reserveSource: String(entry.reserveSource || 'epg_portal_reservation_list'),

    title: String(entry.label || ''),
    titleLabel: String(entry.label || ''),
    serviceName: String(entry.service || entry.target || ''),
    service: String(entry.service || entry.target || ''),

    serviceKey: String(entry.serviceKey || ''),
    physicalChannel: toSafeInt(entry.physicalChannel, -1),
    remoteControlKeyId: String(entry.remoteControlKeyId || ''),

    summary: String(entry.summary || ''),
    genre: String(entry.genre || ''),

    dateText: normalizeDateTextForReservation(entry.date || ''),
    weekdayText: String(entry.weekday || ''),

    displayTimeText: displayTimeText,
    timeText: storageTimeText,
    time: displayTimeText || storageTimeText,

    startDate: String(entry.startDate || ''),
    startTime: String(entry.startTime || ''),
    endDate: normalizeDateTextForReservation(entry.endDate || ''),
    endTime: normalizeTimeTextForReservation(entry.endTime || ''),

    space: toSafeInt(entry.space, 0),
    networkId: toSafeInt(entry.networkId, 0),
    tsId: toSafeInt(entry.tsId, 0),
    serviceId: toSafeInt(entry.serviceId, 0),
    eventId: toSafeInt(entry.eventId, 0),

    enabled: !(entry && entry.enabled === false),
    disabledByUser: !!(entry && entry.disabledByUser),
    excludeFromAuto: !!(entry && entry.excludeFromAuto),
    autoReserved: !!(entry && entry.autoReserved),

    requiredTuners: toSafeInt(entry.requiredTuners, 1),
    preferredTunerIndex: toSafeInt(entry.preferredTunerIndex, -1)
  };
}
)JS";
        }

        void AppendRuntimeSwitchScript_ListOps_ProgramTableSync_(std::wstring& js)
        {
            js += LR"JS(
function getProgramModeFromReservationListEntry_(entry){
  const mode = normalizeReservationListMode_(
    entry && (entry.mode || entry.reserveMode || entry.taskType)
      ? (entry.mode || entry.reserveMode || entry.taskType)
      : ''
  );

  return (mode === 'watch' || mode === 'record') ? mode : '';
}

function rememberReservationListRemoveForProgramTable_(entry, payload){
  const mode = getProgramModeFromReservationListEntry_(entry);
  if (!mode) {
    return;
  }

  try {
    rememberReservationMutationRemove_(payload, mode, entry);
  } catch (e) {
    console.warn('[reservation-list] remember remove overlay failed', e, entry, payload);
  }
}

function rememberReservationListPatchForProgramTable_(entry, patch){
  const mode = getProgramModeFromReservationListEntry_(entry);
  if (!mode) {
    return null;
  }

  const next = cloneReservationEntry(entry);
  Object.assign(next, patch || {});

  try {
    rememberReservationMutationEntry_(next);
  } catch (e) {
    console.warn('[reservation-list] remember patch overlay failed', e, next);
  }

  return next;
}

function notifyProgramTableFromReservationList_(){
  markReservationLocalMutation_();

  refreshReservationViewsAfterSwitch({
    syncCache:false,
    scheduleRetry:false,
    skipCacheSync:true,
    preferLocalState:true,
    refreshProgramFrames:false,
    notifyFrames:true
  });
}
)JS";
        }

        void AppendRuntimeSwitchScript_ListOps_Remove_(std::wstring& js)
        {
            js += LR"JS(
function makeFixedTaskRemoveRejected_(entry, reservationId){
  return {
    ok:false,
    reason:'fixed-epg-config-task-not-removable',
    id:String(entry && entry.id ? entry.id : reservationId || ''),
    kind:toSafeInt(entry && entry.kind, 0),
    instance:toSafeInt(entry && entry.instance, 0),
    mode:normalizeReservationListMode_(
      entry && (entry.mode || entry.reserveMode || entry.taskType)
        ? (entry.mode || entry.reserveMode || entry.taskType)
        : ''
    )
  };
}

function removeReservationListEntryLocal_(entry){
  const targetId = String(entry && entry.id ? entry.id : '');

  const nextList = getSharedReservationList_().filter(v =>
    String(v && v.id ? v.id : '') !== targetId
  );

  setSharedReservationList_(nextList);
  invalidateReservationStateCache();

  return nextList;
}

function requestReservationListRemoveById(reservationId){
  const entry = findReservationListEntryById(reservationId);

  reservationSwitchDebug_('list-remove-begin', {
    reservationId: String(reservationId || ''),
    found: !!entry,
    entry: reservationSwitchCompactEntry_(entry)
  });

  if (!entry) {
    return { ok:false, reason:'not-found' };
  }

  if (!canMutateReservationListEntry_(entry)) {
    console.warn('[reservation-list] skip remove fixed/config task', entry);
    refreshReservationViewsAfterSwitch();
    return makeFixedTaskRemoveRejected_(entry, reservationId);
  }

  const payload = buildReservationListPayload(entry);
  const nativeBackend = getNativeReservationBackend();

  if (nativeBackend &&
      typeof nativeBackend.removeReservationById === 'function') {
    const result = nativeBackend.removeReservationById(payload);

    if (!result || result.ok === false) {
      console.error('[reservation-list] remove_by_id failed', result, payload);
      return result || { ok:false, reason:'native-remove-by-id-failed' };
    }

    removeReservationListEntryLocal_(entry);
rememberReservationListRemoveForProgramTable_(entry, payload);
markReservationLocalMutation_();

    const refreshed = refreshReservationListAfterMutation_(
  result,
  'remove_by_id',
  {
    removeId:String(entry.id || reservationId || ''),
    removeKey:String(entry.dedupeKey || payload.dedupeKey || entry.id || ''),
    removeEntry:cloneReservationEntry(entry)
  }
);
reservationSwitchDebug_('list-remove-end', {
  reservationId: String(reservationId || ''),
  result: reservationSwitchCompactResult_(refreshed),
  entry: reservationSwitchCompactEntry_(entry)
});

    completeReservationUiSyncAfterMutation_(
  'list-remove-by-id',
  refreshed
);

return refreshed;
  }

  removeReservationListEntryLocal_(entry);
  rememberReservationListRemoveForProgramTable_(entry, payload);
  notifyProgramTableFromReservationList_();
queueReservationUiSync_('list-remove-local', true);

return { ok:true };
}
)JS";
        }

        void AppendRuntimeSwitchScript_ListOps_ToggleBase_(std::wstring& js)
        {
            js += LR"JS(
function makeFixedTaskToggleRejected_(entry, reservationId){
  return {
    ok:false,
    reason:'fixed-epg-config-task-not-toggleable',
    id:String(entry && entry.id ? entry.id : reservationId || ''),
    kind:toSafeInt(entry && entry.kind, 0),
    instance:toSafeInt(entry && entry.instance, 0),
    mode:normalizeReservationListMode_(
      entry && (entry.mode || entry.reserveMode || entry.taskType)
        ? (entry.mode || entry.reserveMode || entry.taskType)
        : ''
    )
  };
}

function makeReservationListEnabledPatch_(entry){
  const currentEnabled = !(entry && entry.enabled === false);
  const nextEnabled = !currentEnabled;

  return {
    enabled:nextEnabled,
    disabledByUser:nextEnabled ? false : true,
    excludeFromAuto:nextEnabled ? false : true
  };
}

function applyReservationListPatchLocal_(entry, patch){
  const list = getSharedReservationList_().slice();
  const targetId = String(entry && entry.id ? entry.id : '');

  const idx = list.findIndex(v =>
    String(v && v.id ? v.id : '') === targetId
  );

  if (idx < 0) {
    return null;
  }

  const patchedEntry = cloneReservationEntry(list[idx]);
  Object.assign(patchedEntry, patch || {});
  list[idx] = patchedEntry;

  setSharedReservationList_(list);
  invalidateReservationStateCache();

  return patchedEntry;
}

function rememberReservationListPatchOrEntry_(entry, patch){
  const patched = rememberReservationListPatchForProgramTable_(entry, patch);

  if (patched) {
    rememberReservationMutationEntry_(patched);
  }

  return patched;
}
)JS";
        }

        void AppendRuntimeSwitchScript_ListOps_ToggleNative_(std::wstring& js)
        {
            js += LR"JS(
function toggleReservationListEntryWithNative_(entry, reservationId, payload, patch){
  reservationSwitchDebug_('list-toggle-native-begin', {
    reservationId: String(reservationId || ''),
    entry: reservationSwitchCompactEntry_(entry),
    patch: patch || null
  });

  const nativeBackend = getNativeReservationBackend();

  if (!nativeBackend ||
      typeof nativeBackend.setReservationEnabledById !== 'function') {
    return null;
  }

  const result = nativeBackend.setReservationEnabledById(
    payload,
    patch.enabled,
    patch.disabledByUser,
    patch.excludeFromAuto
  );

  if (!result || result.ok === false) {
    console.error('[reservation-list] set_enabled_by_id failed', result, payload);
    return result || { ok:false, reason:'native-set-enabled-by-id-failed' };
  }

  const patchedEntry = applyReservationListPatchLocal_(entry, patch);

  if (patchedEntry) {
    rememberReservationMutationEntry_(patchedEntry);
  } else {
    rememberReservationListPatchOrEntry_(entry, patch);
  }

  const refreshed = refreshReservationListAfterMutation_(
    result,
    'set_enabled_by_id',
    {
      patchId:String(entry.id || reservationId || ''),
      patch:patch
    }
  );
reservationSwitchDebug_('list-toggle-native-end', {
  reservationId: String(reservationId || ''),
  result: reservationSwitchCompactResult_(refreshed),
  entry: reservationSwitchCompactEntry_(entry),
  patch: patch || null
});

  completeReservationUiSyncAfterMutation_(
  'list-toggle-enabled-by-id',
  refreshed
);

return refreshed;
}
)JS";
        }

        void AppendRuntimeSwitchScript_ListOps_ToggleMain_(std::wstring& js)
        {
            js += LR"JS(
function requestReservationListToggleEnabledById(reservationId){
  const entry = findReservationListEntryById(reservationId);

  reservationSwitchDebug_('list-toggle-enabled-begin', {
    reservationId: String(reservationId || ''),
    found: !!entry,
    entry: reservationSwitchCompactEntry_(entry)
  });

  if (!entry) {
    return { ok:false, reason:'not-found' };
  }

  if (!canMutateReservationListEntry_(entry)) {
    console.warn('[reservation-list] skip toggle fixed/config task', entry);
    refreshReservationViewsAfterSwitch();
    return makeFixedTaskToggleRejected_(entry, reservationId);
  }

  const patch = makeReservationListEnabledPatch_(entry);

  const payload = buildReservationListPayload(entry);
  payload.enabled = patch.enabled;
  payload.disabledByUser = patch.disabledByUser;
  payload.excludeFromAuto = patch.excludeFromAuto;

  const nativeResult = toggleReservationListEntryWithNative_(
    entry,
    reservationId,
    payload,
    patch
  );

  if (nativeResult) {
    return nativeResult;
  }

  const patchedEntry = applyReservationListPatchLocal_(entry, patch);

  if (patchedEntry) {
  rememberReservationMutationEntry_(patchedEntry);

  reservationSwitchDebug_('list-toggle-enabled-local-end', {
    reservationId: String(reservationId || ''),
    patchedEntry: reservationSwitchCompactEntry_(patchedEntry),
    patch: patch || null
  });

  notifyProgramTableFromReservationList_();
}

  return { ok:true };
}
)JS";
        }

        void AppendRuntimeSwitchScript_ListOps_Exports_(std::wstring& js)
        {
            js += LR"JS(
// dynamic.cpp 側の既存 onclick 名をここで上書きする
// dynamic.cpp 側の既存 onclick 名をここで上書きする。
// 予約一覧操作は必ず ListOps 経由へ流す。
function removeReservationById(reservationId){
  return requestReservationListRemoveById(reservationId);
}

function toggleReservationEnabledById(reservationId){
  return requestReservationListToggleEnabledById(reservationId);
}

function installReservationListOperationExports_(){
  window.removeReservationById = removeReservationById;
  window.toggleReservationEnabledById = toggleReservationEnabledById;

  window.requestReservationListRemoveById = requestReservationListRemoveById;
  window.requestReservationListToggleEnabledById = requestReservationListToggleEnabledById;

  return true;
}

installReservationListOperationExports_();
)JS";
        }

        void AppendRuntimeSwitchScript_ListOps_(std::wstring& js)
        {
            AppendRuntimeSwitchScript_ListOps_Mode_(js);
            AppendRuntimeSwitchScript_ListOps_Payload_(js);
            AppendRuntimeSwitchScript_ListOps_ProgramTableSync_(js);
            AppendRuntimeSwitchScript_ListOps_Remove_(js);
            AppendRuntimeSwitchScript_ListOps_ToggleBase_(js);
            AppendRuntimeSwitchScript_ListOps_ToggleNative_(js);
            AppendRuntimeSwitchScript_ListOps_ToggleMain_(js);
            AppendRuntimeSwitchScript_ListOps_Exports_(js);
        }

        void AppendRuntimeSwitchScript_Patch_(std::wstring& js)
        {
            js += LR"JS(
(function patchReservationHooks(){
  function runLater_(fn){
    try {
      if (typeof window.requestAnimationFrame === 'function') {
        window.requestAnimationFrame(function(){
          try {
            fn();
          } catch (e) {
            console.warn('[reservation-switch] delayed hook failed', e);
          }
        });
      } else {
        window.setTimeout(function(){
          try {
            fn();
          } catch (e) {
            console.warn('[reservation-switch] delayed hook failed', e);
          }
        }, 0);
      }
    } catch (_) {
    }
  }

  function patchAfter_(name, after){
    try {
      const orig = window[name];
      if (typeof orig !== 'function') {
        return false;
      }

      if (orig.__epgReservationPatched) {
        return true;
      }

      const wrapped = function(){
        let ret = orig.apply(this, arguments);
        runLater_(after);
        return ret;
      };

      wrapped.__epgReservationPatched = true;
      wrapped.__epgReservationOriginal = orig;
      window[name] = wrapped;
      return true;
    } catch (e) {
      console.warn('[reservation-switch] patch failed: ' + name, e);
      return false;
    }
  }

  function patchBeforeAfter_(name, before, after){
    try {
      const orig = window[name];
      if (typeof orig !== 'function') {
        return false;
      }

      if (orig.__epgReservationPatchedBeforeAfter) {
        return true;
      }

      const wrapped = function(){
        let ret;

        try {
          if (typeof before === 'function') {
            before();
          }
        } catch (e) {
          console.warn('[reservation-switch] before hook failed: ' + name, e);
        }

        ret = orig.apply(this, arguments);

        runLater_(after);
        return ret;
      };

      wrapped.__epgReservationPatchedBeforeAfter = true;
      wrapped.__epgReservationOriginal = orig;
      window[name] = wrapped;
      return true;
    } catch (e) {
      console.warn('[reservation-switch] before/after patch failed: ' + name, e);
      return false;
    }
  }

  patchAfter_('renderSearchResults', function(){
    if (typeof syncProgramReservationButtons === 'function') {
      syncProgramReservationButtons();
    }
  });

  patchAfter_('showProgramDetail', function(){
    if (typeof syncDetailReservationButtons === 'function') {
      syncDetailReservationButtons();
    }
  });

    patchBeforeAfter_(
    'renderReservationPage',
    function(){
      const localHolding =
        (typeof isReservationLocalMutationHolding_ === 'function')
          ? isReservationLocalMutationHolding_()
          : false;

      reservationSwitchDebug_('renderReservationPage-before', {
        localHolding: localHolding,
        willSyncViewCache: !localHolding
      });
epgPortalTracePush_('parent', 'renderReservationPage-before', {
  localHolding: localHolding,
  willSyncViewCache: !localHolding
});

      if (localHolding) {
        reservationSwitchDebug_('renderReservationPage-before-sync-skip-local-hold', {
          state: window.__epgPortalDumpReservationSwitchState
            ? window.__epgPortalDumpReservationSwitchState()
            : null
        });
        return;
      }

      if (typeof syncSharedReservationListFromViewCache_ === 'function') {
        const ok = syncSharedReservationListFromViewCache_();

        reservationSwitchDebug_('renderReservationPage-before-sync-done', {
          ok: !!ok,
          cache: getReservationViewCacheDebugInfo_()
        });
      }
    },
    function(){
      if (typeof syncDetailReservationButtons === 'function') {
        syncDetailReservationButtons();
      }
    }
  );

    publishProgramTableBridge();

  if (typeof installReservationListOperationExports_ === 'function') {
  installReservationListOperationExports_();

  // dynamic runtime 側が後から同名 export を上書きした場合の保険。
  try {
    window.setTimeout(installReservationListOperationExports_, 0);
    window.setTimeout(installReservationListOperationExports_, 250);
    window.setTimeout(installReservationListOperationExports_, 1000);
  } catch (_) {
  }
}

  window.__epgPortalRefreshReservationViews = refreshReservationViewsAfterSwitch;
  window.__epgPortalReloadReservationStateSync = reloadReservationStateSync;

  installReservationListMirrorBridge_();

  if (typeof bindReservationSwitchNavigationRefresh_ === 'function') {
    bindReservationSwitchNavigationRefresh_();
  }

  window.__epgPortalSyncReservationListFromViewCache = function(){
    if (typeof syncSharedReservationListFromViewCache_ !== 'function') {
      return false;
    }

    const ok = syncSharedReservationListFromViewCache_();
    if (ok) {
      refreshReservationViewsAfterSwitch({
        syncCache:false,
        scheduleRetry:false
      });
    }

    return ok;
  };
})();
)JS";
        }

        std::wstring BuildRuntimeSwitchScript_()
        {
            std::wstring js;
            js.reserve(static_cast<std::size_t>(128) * 1024U);

            AppendRuntimeSwitchScript_Core1_(js);
            AppendRuntimeSwitchScript_Core2_(js);
            AppendRuntimeSwitchScript_UI1_(js);
            AppendRuntimeSwitchScript_UI2_(js);
            AppendRuntimeSwitchScript_ListOps_(js);
            AppendRuntimeSwitchScript_Patch_(js);

            return js;
        }

        void AppendProgramTableSwitchScript_Core1_Open_(std::wstring& js)
        {
            js += LR"JS((function(){
'use strict';

)JS";
        }

        void AppendProgramTableSwitchScript_Core1_Text_(std::wstring& js)
        {
            js += LR"JS(
function normalizeText(s){
  return String(s || '').replace(/\u3000/g, ' ').replace(/\s+/g, ' ').trim();
}

function decodeHtmlEntitiesMinimal(s){
  let v = String(s || '');
  v = v.replace(/&amp;#(\d+);/g, function(_, n){
    const cp = Number(n);
    return Number.isFinite(cp) ? String.fromCodePoint(cp) : _;
  });
  v = v.replace(/&#(\d+);/g, function(_, n){
    const cp = Number(n);
    return Number.isFinite(cp) ? String.fromCodePoint(cp) : _;
  });
  v = v.replace(/&amp;#x([0-9a-fA-F]+);/g, function(_, h){
    const cp = parseInt(h, 16);
    return Number.isFinite(cp) ? String.fromCodePoint(cp) : _;
  });
  v = v.replace(/&#x([0-9a-fA-F]+);/g, function(_, h){
    const cp = parseInt(h, 16);
    return Number.isFinite(cp) ? String.fromCodePoint(cp) : _;
  });

  v = v.replace(/&nbsp;/gi, ' ');
  v = v.replace(/&amp;/gi, '&');
  v = v.replace(/&lt;/gi, '<');
  v = v.replace(/&gt;/gi, '>');
  v = v.replace(/&quot;/gi, '"');

  return v;
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core1_Time_(std::wstring& js)
        {
            js += LR"JS(
function normalizeTimeTextForReservation(timeText){
  return decodeHtmlEntitiesMinimal(timeText)
    .replace(/\u301C/g, '〜')
    .replace(/\uFF5E/g, '〜')
    .replace(/\u223C/g, '〜')
    .replace(/[‐-‒–—―ー－]/g, '-')
    .trim();
}

function normalizeTimeRangeForDisplay_(timeText){
  let s = normalizeTimeTextForReservation(timeText);

  s = s
    .replace(/？/g, '?')
    .replace(/～/g, '〜')
    .replace(/\s+/g, ' ')
    .trim();

  s = s.replace(
    /(\d{1,2}:\d{2})\s*[?~〜\-]\s*(\d{1,2}:\d{2})/g,
    '$1～$2'
  );

  return s;
}

function normalizeTimeRangeForStorage_(timeText){
  let s = normalizeTimeTextForReservation(timeText);

  s = s
    .replace(/？/g, '?')
    .replace(/[～〜?]/g, '-')
    .replace(/\s+/g, ' ')
    .trim();

  s = s.replace(
    /(\d{1,2}:\d{2})\s*-\s*(\d{1,2}:\d{2})/g,
    '$1-$2'
  );

  return s;
}

function formatIsoTime(hh, mm, ss){
  return String(hh).padStart(2, '0')
    + ':' + String(mm).padStart(2, '0')
    + ':' + String(ss || 0).padStart(2, '0');
}

function parseTimeTextParts(timeText){
  const s = normalizeTimeTextForReservation(timeText);
  const m = /^(\d{1,2}):(\d{2})(?:\s*[～〜\-]\s*(\d{1,2}):(\d{2}))?/.exec(s);

  if (!m) {
    return {
      startTime:'',
      endTime:'',
      durationSec:0,
      crossesDay:false
    };
  }

  const sh = Number(m[1]);
  const sm = Number(m[2]);
  const eh = (m[3] != null) ? Number(m[3]) : sh;
  const em = (m[4] != null) ? Number(m[4]) : sm;

  let durationMin = 0;
  let crossesDay = false;

  if (m[3] != null) {
    durationMin = (eh * 60 + em) - (sh * 60 + sm);
    if (durationMin < 0) {
      durationMin += 24 * 60;
      crossesDay = true;
    }
  }

  return {
    startTime: formatIsoTime(sh, sm, 0),
    endTime: formatIsoTime(eh, em, 0),
    durationSec: durationMin > 0 ? durationMin * 60 : 0,
    crossesDay: crossesDay
  };
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core1_Date_(std::wstring& js)
        {
            js += LR"JS(
function normalizeDateTextForReservation(dateText){
  return decodeHtmlEntitiesMinimal(dateText).trim();
}

function formatIsoDate(y, m, d){
  const yy = String(y).padStart(4, '0');
  const mm = String(m).padStart(2, '0');
  const dd = String(d).padStart(2, '0');
  return yy + '-' + mm + '-' + dd;
}

function shiftIsoDate(isoDate, deltaDays){
  const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(String(isoDate || ''));
  if (!m) return '';

  const dt = new Date(Number(m[1]), Number(m[2]) - 1, Number(m[3]));
  if (!Number.isFinite(dt.getTime())) return '';

  dt.setDate(dt.getDate() + deltaDays);
  return formatIsoDate(dt.getFullYear(), dt.getMonth() + 1, dt.getDate());
}

function parseDateTextToIso(dateText){
  const s = normalizeDateTextForReservation(dateText);
  const m = /^(\d{4})[\/\-](\d{1,2})[\/\-](\d{1,2})/.exec(s);
  if (!m) return '';

  return formatIsoDate(Number(m[1]), Number(m[2]), Number(m[3]));
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core1_BackendAccess_(std::wstring& js)
        {
            js += LR"JS(
function getBridge(){
  try {
    if (window.top && window.top !== window) {
      if (window.top.__epgPortalSwitchProgramTableBridge) {
        return window.top.__epgPortalSwitchProgramTableBridge;
      }

      if (window.top.__epgPortalProgramTableBridge) {
        return window.top.__epgPortalProgramTableBridge;
      }
    }
  } catch (_) {
  }

  try {
    if (window.__epgPortalSwitchProgramTableBridge) {
      return window.__epgPortalSwitchProgramTableBridge;
    }

    if (window.__epgPortalProgramTableBridge) {
      return window.__epgPortalProgramTableBridge;
    }
  } catch (_) {
  }

  return null;
}

function getNativeBackend(){
  try {
    if (window.__epgPortalNativeReservationBackend) {
      return window.__epgPortalNativeReservationBackend;
    }
  } catch (_) {
  }

  try {
    if (window.top && window.top !== window &&
        window.top.__epgPortalNativeReservationBackend) {
      return window.top.__epgPortalNativeReservationBackend;
    }
  } catch (_) {
  }

  return null;
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core1_LocalStore_(std::wstring& js)
        {
            js += LR"JS(
const localStore = [];

function buildLocalKey(payload, mode){
  return String(payload && payload.serviceId ? payload.serviceId : 0)
    + ':' + String(payload && payload.eventId ? payload.eventId : 0)
    + ':' + normalizeText(payload && payload.title ? payload.title : '')
    + ':' + normalizeText(payload && (payload.dateText || payload.date) ? (payload.dateText || payload.date) : '')
    + ':' + normalizeText(payload && (payload.timeText || payload.time) ? (payload.timeText || payload.time) : '')
    + ':' + String(mode || '');
}

function localFind(payload, mode){
  const key = buildLocalKey(payload, mode);
  return localStore.find(v => v && v.key === key) || null;
}

function localGetState(payload){
  const watch = localFind(payload, 'watch');
  const record = localFind(payload, 'record');

  return {
    watchReserved: !!watch,
    recordReserved: !!record,
    watchEnabled: watch ? watch.enabled !== false : true,
    recordEnabled: record ? record.enabled !== false : true
  };
}

function localUpsert(payload, mode, enabled){
  const key = buildLocalKey(payload, mode);
  const item = localFind(payload, mode);

  if (item) {
    item.enabled = (enabled !== false);
    return;
  }

  localStore.push({
    key:key,
    enabled:(enabled !== false),
    excludeFromAuto:false,
    disabledByUser:false
  });
}

function localRemove(payload, mode){
  const key = buildLocalKey(payload, mode);
  const idx = localStore.findIndex(v => v && v.key === key);

  if (idx >= 0) {
    localStore.splice(idx, 1);
    return true;
  }

  return false;
}

function localToggle(payload, mode){
  if (localFind(payload, mode)) {
    localRemove(payload, mode);
  } else {
    localUpsert(payload, mode, true);
  }

  return {
    ok:true,
    state:localGetState(payload)
  };
}

function localToggleDisable(payload, mode){
  const item = localFind(payload, mode);

  if (!item) {
    return { ok:false, reason:'not-found' };
  }

  const nowEnabled = item.enabled !== false;
  item.enabled = !nowEnabled;
  item.disabledByUser = nowEnabled ? true : false;
  item.excludeFromAuto = nowEnabled ? true : false;

  return {
    ok:true,
    state:localGetState(payload)
  };
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core1_Trace_(std::wstring& js)
        {
            js += LR"JS(
function programTableTrace_(tag, data){
  try {
    if (window.top &&
        window.top !== window &&
        typeof window.top.__epgPortalTracePush === 'function') {
      return window.top.__epgPortalTracePush(
        'frame',
        tag,
        Object.assign({
          frameUrl:String(location.pathname || location.href || '')
        }, data || {})
      );
    }
  } catch (_) {
  }

  try {
    console.log('[EPG-FRAME-TRACE]', tag, data || {});
  } catch (_) {
  }

  return null;
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core1_(std::wstring& js)
        {
            AppendProgramTableSwitchScript_Core1_Open_(js);
            AppendProgramTableSwitchScript_Core1_Text_(js);
            AppendProgramTableSwitchScript_Core1_Time_(js);
            AppendProgramTableSwitchScript_Core1_Date_(js);
            AppendProgramTableSwitchScript_Core1_BackendAccess_(js);
            AppendProgramTableSwitchScript_Core1_LocalStore_(js);
            AppendProgramTableSwitchScript_Core1_Trace_(js);
        }

        void AppendProgramTableSwitchScript_Core2_Request_(std::wstring& js)
        {
            js += LR"JS(
function makeNativeRequest(payload, mode){
  const req = Object.assign({}, payload || {});
  req.reserveMode = String(mode || '');
  req.dedupeKey = buildLocalKey(payload, mode);
  req.excludeKey = req.dedupeKey;
  req.enabled = true;
  req.disabledByUser = false;
  req.excludeFromAuto = false;
  req.autoReserved = false;
  req.requiredTuners = Number(req.requiredTuners || 1);
  req.preferredTunerIndex =
    Number.isFinite(Number(req.preferredTunerIndex))
      ? Number(req.preferredTunerIndex)
      : -1;

  if (!req.serviceName && req.service) {
    req.serviceName = req.service;
  }
  if (!req.titleLabel && req.title) {
    req.titleLabel = req.title;
  }

  return req;
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core2_NativeToggle_(std::wstring& js)
        {
            js += LR"JS(
function nativeToggle(payload, mode){
  const nativeBackend = getNativeBackend();
  if (!nativeBackend ||
      typeof nativeBackend.upsertProgramReservation !== 'function' ||
      typeof nativeBackend.removeProgramReservation !== 'function') {
    return localToggle(payload, mode);
  }

  const existing = localFind(payload, mode);

  if (existing) {
    const req = makeNativeRequest(payload, mode);
    const result = nativeBackend.removeProgramReservation(req);
    if (!result || result.ok === false) {
      return result || { ok:false, reason:'native-remove-failed' };
    }

    localRemove(payload, mode);
    return {
      ok:true,
      state:localGetState(payload)
    };
  }

  const req = makeNativeRequest(payload, mode);
  const result = nativeBackend.upsertProgramReservation(req);
  if (!result || result.ok === false) {
    return result || { ok:false, reason:'native-upsert-failed' };
  }

  localUpsert(payload, mode, true);
  return {
    ok:true,
    state:localGetState(payload)
  };
}

function nativeToggleDisable(payload, mode){
  const nativeBackend = getNativeBackend();
  if (!nativeBackend ||
      typeof nativeBackend.setProgramReservationEnabled !== 'function') {
    return localToggleDisable(payload, mode);
  }

  const existing = localFind(payload, mode);
  if (!existing) {
    return { ok:false, reason:'not-found' };
  }

  const nextEnabled = existing.enabled === false ? true : false;
  const req = makeNativeRequest(payload, mode);

  const result = nativeBackend.setProgramReservationEnabled(
    req,
    nextEnabled,
    !nextEnabled,
    !nextEnabled
  );

  if (!result || result.ok === false) {
    return result || { ok:false, reason:'native-set-enabled-failed' };
  }

  existing.enabled = nextEnabled;
  existing.disabledByUser = !nextEnabled;
  existing.excludeFromAuto = !nextEnabled;

  return {
    ok:true,
    state:localGetState(payload)
  };
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core2_BackendResolve_(std::wstring& js)
        {
            js += LR"JS(
function makeUnavailableProgramTableBackend_(){
  return {
    getState: function(){
      return {
        watchReserved:false,
        recordReserved:false,
        watchEnabled:true,
        recordEnabled:true,
        watchDisabledByUser:false,
        recordDisabledByUser:false,
        watchExcludeFromAuto:false,
        recordExcludeFromAuto:false
      };
    },
    toggle: function(payload, mode){
      console.warn(
        '[program-table-switch] parent bridge unavailable. toggle blocked.',
        payload,
        mode
      );
      return {
        ok:false,
        reason:'parent-bridge-unavailable'
      };
    },
    toggleDisable: function(payload, mode){
      console.warn(
        '[program-table-switch] parent bridge unavailable. toggleDisable blocked.',
        payload,
        mode
      );
      return {
        ok:false,
        reason:'parent-bridge-unavailable'
      };
    }
  };
}

function resolveBackend(){
  const bridge = getBridge();

  if (bridge &&
      typeof bridge.getState === 'function' &&
      typeof bridge.toggle === 'function') {

    return {
      getState: bridge.getState,
      toggle: bridge.toggle,
      toggleDisable: (typeof bridge.toggleDisable === 'function')
        ? bridge.toggleDisable
        : function(){
            return {
              ok:false,
              reason:'not-supported'
            };
          }
    };
  }

  // 重要:
  // iframe側では nativeBackend / localStore へ落とさない。
  // 予約状態の正本は親ページ Runtime 側の __epgPortalProgramTableBridge に一本化する。
  return makeUnavailableProgramTableBackend_();
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core2_(std::wstring& js)
        {
            AppendProgramTableSwitchScript_Core2_Request_(js);
            AppendProgramTableSwitchScript_Core2_NativeToggle_(js);
            AppendProgramTableSwitchScript_Core2_BackendResolve_(js);
        }

        void AppendProgramTableSwitchScript_Core3_Cells_(std::wstring& js)
        {
            js += LR"JS(
function getCandidateCells(){
  const out = [];
  const seen = new Set();

  const push = (node) => {
    if (!node || seen.has(node)) return;
    seen.add(node);
    out.push(node);
  };

  document.querySelectorAll('[data-epg-program="1"]').forEach(push);
  document.querySelectorAll('.epg-program-cell[data-title]').forEach(push);
  document.querySelectorAll('td[data-title]').forEach(push);
  document.querySelectorAll('td[data-event-id]').forEach(push);

  return out;
}

function inferDateTextFromCell(cell){
  try {
    const sec = cell.closest('section[data-day]');
    if (sec && sec.dataset && sec.dataset.day) {
      return normalizeDateTextForReservation(sec.dataset.day);
    }
  } catch (_) {
  }
  return '';
}

function getEpgServiceMap_(){
  try {
    if (window.EPG_SERVICE_MAP) {
      return window.EPG_SERVICE_MAP;
    }
  } catch (_) {
  }

  try {
    if (window.top && window.top !== window && window.top.EPG_SERVICE_MAP) {
      return window.top.EPG_SERVICE_MAP;
    }
  } catch (_) {
  }

  return {};
}

function normalizeServiceNameForMap_(s){
  return normalizeText(decodeHtmlEntitiesMinimal(s || ''))
    .replace(/\s+/g, '')
    .trim();
}

function attachServiceKeyToInfo_(key, info){
  if (!info || typeof info !== 'object') {
    return null;
  }

  const out = Object.assign({}, info);
  out.serviceKey = String(out.serviceKey || key || '');
  return out;
}

function resolveServiceInfoByKey_(serviceKey){
  const key = String(serviceKey || '');
  if (!key) return null;

  const map = getEpgServiceMap_();
  if (map && map[key]) {
    return attachServiceKeyToInfo_(key, map[key]);
  }

  return null;
}

function resolveServiceInfoByIds_(space, networkId, tsId, serviceId){
  const sp = Number(space);
  const nid = Number(networkId);
  const tid = Number(tsId);
  const sid = Number(serviceId);

  if (!Number.isFinite(nid) || !Number.isFinite(tid) || !Number.isFinite(sid) ||
      nid <= 0 || tid <= 0 || sid <= 0) {
    return null;
  }

  const key = 's' + String(Number.isFinite(sp) ? Math.trunc(sp) : 0)
    + '_n' + String(Math.trunc(nid))
    + '_t' + String(Math.trunc(tid))
    + '_sid' + String(Math.trunc(sid));

  return resolveServiceInfoByKey_(key);
}

function resolveServiceInfoByName_(serviceName, space, physicalChannel){
  const targetName = normalizeServiceNameForMap_(serviceName);
  if (!targetName) return null;

  const sp = Number(space);
  const phys = Number(physicalChannel);
  const usePhys = Number.isFinite(phys) && phys >= 0;

  const map = getEpgServiceMap_();
  const candidates = [];

  Object.keys(map || {}).forEach(key => {
    const item = map[key];
    if (!item) return;

    const itemSpace = Number(item.space);
    if (Number.isFinite(sp) && Number.isFinite(itemSpace) && itemSpace !== sp) {
      return;
    }

    if (usePhys) {
      const itemPhys = Number(item.physicalChannel);
      if (Number.isFinite(itemPhys) && itemPhys !== phys) {
        return;
      }
    }

    const n1 = normalizeServiceNameForMap_(item.serviceName || '');
    const n2 = normalizeServiceNameForMap_(item.channelName || '');

    if (n1 === targetName || n2 === targetName) {
      candidates.push(attachServiceKeyToInfo_(key, item));
    }
  });

  if (candidates.length <= 0) {
    return null;
  }

  candidates.sort((a, b) => {
    const asid = Number(a && a.serviceId);
    const bsid = Number(b && b.serviceId);
    if (Number.isFinite(asid) && Number.isFinite(bsid) && asid !== bsid) {
      return asid - bsid;
    }
    return String(a.serviceName || '').localeCompare(String(b.serviceName || ''), 'ja');
  });

  return candidates[0];
}

function buildTableCellVisualColumnMap_(table){
  const map = new WeakMap();
  const rows = Array.from(table ? table.rows || [] : []);
  const occupied = [];

  rows.forEach(row => {
    let col = 0;

    for (const cell of Array.from(row.cells || [])) {
      while ((occupied[col] || 0) > 0) {
        ++col;
      }

      map.set(cell, col);

      const colSpan = Math.max(1, Number(cell.colSpan || 1));
      const rowSpan = Math.max(1, Number(cell.rowSpan || 1));

      for (let i = 0; i < colSpan; ++i) {
        occupied[col + i] = Math.max(occupied[col + i] || 0, rowSpan);
      }

      col += colSpan;
    }

    for (let i = 0; i < occupied.length; ++i) {
      if ((occupied[i] || 0) > 0) {
        occupied[i] -= 1;
      }
    }
  });

  return map;
}

const gTableVisualColumnCache_ = new WeakMap();

function getCellVisualColumn_(cell){
  const table = cell ? cell.closest('table') : null;
  if (!table) return -1;

  let map = gTableVisualColumnCache_.get(table);
  if (!map) {
    map = buildTableCellVisualColumnMap_(table);
    gTableVisualColumnCache_.set(table, map);
  }

  const v = map.get(cell);
  return Number.isFinite(Number(v)) ? Number(v) : -1;
}

function getHeaderTextForVisualColumn_(table, visualCol){
  if (!table || !Number.isFinite(visualCol) || visualCol < 0) {
    return '';
  }

  const headRow = table.querySelector('thead tr');
  if (!headRow) return '';

  let col = 0;
  for (const th of Array.from(headRow.cells || [])) {
    const span = Math.max(1, Number(th.colSpan || 1));
    if (visualCol >= col && visualCol < col + span) {
      return normalizeText(decodeHtmlEntitiesMinimal(th.textContent || ''));
    }
    col += span;
  }

  return '';
}

function inferServiceNameFromCell(cell){
  try {
    const table = cell ? cell.closest('table') : null;
    const visualCol = getCellVisualColumn_(cell);
    const name = getHeaderTextForVisualColumn_(table, visualCol);
    return normalizeText(name);
  } catch (_) {
  }
  return '';
}


)JS";
        }

        void AppendProgramTableSwitchScript_Core3_Payload_(std::wstring& js)
        {
            js += LR"JS(
function extractPayload(cell){
  if (!cell) return null;

  const d = cell.dataset || {};
  const pageSpace = Number(
    (document.body && document.body.dataset && document.body.dataset.epgSpace)
      ? document.body.dataset.epgSpace
      : (window.EPG_PROGRAM_PAGE_META && window.EPG_PROGRAM_PAGE_META.space != null
          ? window.EPG_PROGRAM_PAGE_META.space
          : 0)
  );

  const dateText = normalizeDateTextForReservation(
    d.date || cell.getAttribute('data-date') || inferDateTextFromCell(cell) || ''
  );

  const rawTimeText =
    d.time ||
    cell.getAttribute('data-time') ||
    '';

  const displayTimeText = normalizeTimeRangeForDisplay_(rawTimeText);
  const timeTextForStorage = normalizeTimeRangeForStorage_(displayTimeText || rawTimeText);

  const explicitServiceName = String(
    d.service || d.serviceName ||
    cell.getAttribute('data-service') ||
    cell.getAttribute('data-service-name') ||
    ''
  );

  const inferredServiceName = inferServiceNameFromCell(cell);
  const baseServiceName = explicitServiceName || inferredServiceName;

  const attrNetworkId = Number(d.networkId || cell.getAttribute('data-network-id') || 0);
  const attrTsId = Number(d.tsId || cell.getAttribute('data-ts-id') || 0);
  const attrServiceId = Number(d.serviceId || cell.getAttribute('data-service-id') || 0);
  const attrPhysicalChannel = Number(d.physicalChannel || cell.getAttribute('data-physical-channel') || -1);

  let serviceInfo =
    resolveServiceInfoByKey_(d.serviceKey || cell.getAttribute('data-service-key') || '');

  if (!serviceInfo) {
    serviceInfo = resolveServiceInfoByIds_(
      Number.isFinite(pageSpace) ? pageSpace : 0,
      attrNetworkId,
      attrTsId,
      attrServiceId
    );
  }

  if (!serviceInfo) {
    serviceInfo = resolveServiceInfoByName_(
      baseServiceName,
      Number.isFinite(pageSpace) ? pageSpace : 0,
      attrPhysicalChannel
    );
  }

  const resolvedServiceName = serviceInfo
    ? String(serviceInfo.serviceName || serviceInfo.channelName || baseServiceName || '')
    : baseServiceName;

  const parsedTime = parseTimeTextParts(displayTimeText || rawTimeText);

  const startDate =
    d.startDate ||
    cell.getAttribute('data-start-date') ||
    parseDateTextToIso(dateText) ||
    '';

  let endDate =
    d.endDate ||
    cell.getAttribute('data-end-date') ||
    startDate;

  if (!d.endDate &&
      !cell.getAttribute('data-end-date') &&
      parsedTime.crossesDay &&
      startDate) {
    endDate = shiftIsoDate(startDate, 1) || startDate;
  }

  const payload = {
    space: Number.isFinite(pageSpace) ? pageSpace : 0,

    serviceKey: serviceInfo
      ? String(serviceInfo.serviceKey || '')
      : String(d.serviceKey || cell.getAttribute('data-service-key') || ''),

    title: decodeHtmlEntitiesMinimal(
      d.title ||
      d.epgTitle ||
      cell.getAttribute('data-title') ||
      cell.getAttribute('title') ||
      ''
    ),
    titleLabel: decodeHtmlEntitiesMinimal(
      d.titleLabel ||
      d.title ||
      cell.getAttribute('data-title') ||
      cell.getAttribute('title') ||
      ''
    ),

    dateText: dateText,
    weekdayText: d.weekday || cell.getAttribute('data-weekday') || '',

    displayTimeText: displayTimeText,
    timeText: timeTextForStorage,
    time: displayTimeText || timeTextForStorage,

    startDate: startDate,
    startTime: d.startTime || cell.getAttribute('data-start-time') || parsedTime.startTime || '',
    endDate: endDate,
    endTime: d.endTime || cell.getAttribute('data-end-time') || parsedTime.endTime || '',
    durationSec: Number(d.durationSec || cell.getAttribute('data-duration-sec') || parsedTime.durationSec || 0),

    serviceName: resolvedServiceName,
    service: resolvedServiceName,

    inferredServiceName: inferredServiceName,
    serviceNameSource: serviceInfo
      ? 'service-map'
      : (explicitServiceName ? 'explicit' : 'inferred'),

    summary: decodeHtmlEntitiesMinimal(d.summary || cell.getAttribute('data-summary') || ''),
    genre: decodeHtmlEntitiesMinimal(d.genre || cell.getAttribute('data-genre') || ''),

    physicalChannel: serviceInfo
      ? Number(serviceInfo.physicalChannel || -1)
      : attrPhysicalChannel,

    remoteControlKeyId: serviceInfo
      ? String(serviceInfo.remoteControlKeyId || '')
      : String(d.remoteControlKeyId || cell.getAttribute('data-remote-control-key-id') || ''),

    networkId: serviceInfo
      ? Number(serviceInfo.networkId || 0)
      : attrNetworkId,

    tsId: serviceInfo
      ? Number(serviceInfo.tsId || 0)
      : attrTsId,

    serviceId: serviceInfo
      ? Number(serviceInfo.serviceId || 0)
      : attrServiceId,

    eventId: Number(d.eventId || cell.getAttribute('data-event-id') || 0)
  };

  if (!payload.title && payload.eventId <= 0) {
    return null;
  }

  return payload;
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core3_Host_(std::wstring& js)
        {
            js += LR"JS(
function ensureHost(cell){
  if (!cell) return null;

  let host = null;
  for (const child of Array.from(cell.children || [])) {
    if (child && child.classList && child.classList.contains('epg-inline-switch-host')) {
      host = child;
      break;
    }
  }

  if (host) return host;

  host = document.createElement('div');
  host.className = 'epg-inline-switch-host';
  cell.appendChild(host);
  return host;
}

function applyCellMarker(cell, state){
  if (!cell) return;

  cell.classList.toggle(
    'epg-inline-watch-mark',
    !!(state && state.watchReserved && state.watchEnabled !== false)
  );

  cell.classList.toggle(
    'epg-inline-record-mark',
    !!(state && state.recordReserved && state.recordEnabled !== false)
  );

  cell.classList.toggle(
    'epg-inline-watch-disabled',
    !!(state && state.watchReserved && state.watchEnabled === false)
  );

  cell.classList.toggle(
    'epg-inline-record-disabled',
    !!(state && state.recordReserved && state.recordEnabled === false)
  );
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core3_State_(std::wstring& js)
        {
            js += LR"JS(
function makeDefaultProgramSwitchState_(){
  return {
    watchReserved:false,
    recordReserved:false,
    watchEnabled:true,
    recordEnabled:true,
    watchDisabledByUser:false,
    recordDisabledByUser:false,
    watchExcludeFromAuto:false,
    recordExcludeFromAuto:false
  };
}

function resolveBackendSafeForProgramTable_(){
  try {
    const backend = resolveBackend();
    if (backend) {
      return backend;
    }
  } catch (e) {
    console.warn('[program-table-switch] resolveBackend failed', e);
  }

  return {
    getState: function(){
      return makeDefaultProgramSwitchState_();
    },
    toggle: function(){
      return { ok:false, reason:'backend-unavailable' };
    },
    toggleDisable: function(){
      return { ok:false, reason:'backend-unavailable' };
    }
  };
}

function getProgramSwitchStateSafe_(backend, payload){
  const def = makeDefaultProgramSwitchState_();

  try {
    if (backend && typeof backend.getState === 'function') {
      const state = backend.getState(payload);
      if (state && typeof state === 'object') {
        return Object.assign(def, state);
      }
    }
  } catch (e) {
    console.warn('[program-table-switch] getState failed. fallback to default state', e, payload);
  }

  return def;
}

function makeExpectedToggleState_(beforeState, mode, willReserve){
  const state = Object.assign(
    makeDefaultProgramSwitchState_(),
    beforeState || {}
  );

  const fixedMode = String(mode || '');

  if (fixedMode === 'watch') {
    state.watchReserved = !!willReserve;
    state.watchEnabled = true;
    state.watchDisabledByUser = false;
    state.watchExcludeFromAuto = false;
  } else if (fixedMode === 'record') {
    state.recordReserved = !!willReserve;
    state.recordEnabled = true;
    state.recordDisabledByUser = false;
    state.recordExcludeFromAuto = false;
  }

  return state;
}

function stateMatchesExpectedToggle_(state, mode, willReserve){
  if (!state) return false;

  const fixedMode = String(mode || '');
  if (fixedMode === 'watch') {
    return !!state.watchReserved === !!willReserve;
  }

  if (fixedMode === 'record') {
    return !!state.recordReserved === !!willReserve;
  }

  return false;
}

function makeExpectedDisableState_(beforeState, mode){
  const state = Object.assign(
    makeDefaultProgramSwitchState_(),
    beforeState || {}
  );

  const fixedMode = String(mode || '');

  if (fixedMode === 'watch' && state.watchReserved) {
    const nextEnabled = state.watchEnabled === false ? true : false;
    state.watchEnabled = nextEnabled;
    state.watchDisabledByUser = !nextEnabled;
    state.watchExcludeFromAuto = !nextEnabled;
  } else if (fixedMode === 'record' && state.recordReserved) {
    const nextEnabled = state.recordEnabled === false ? true : false;
    state.recordEnabled = nextEnabled;
    state.recordDisabledByUser = !nextEnabled;
    state.recordExcludeFromAuto = !nextEnabled;
  }

  return state;
}

function chooseStateAfterToggle_(payload, mode, beforeState, willReserve, result){
  const expected = makeExpectedToggleState_(beforeState, mode, willReserve);

  if (result && result.state && stateMatchesExpectedToggle_(result.state, mode, willReserve)) {
    return Object.assign(makeDefaultProgramSwitchState_(), result.state);
  }

  try {
    const latestBackend = resolveBackendSafeForProgramTable_();
    const latestState = getProgramSwitchStateSafe_(latestBackend, payload);
    if (stateMatchesExpectedToggle_(latestState, mode, willReserve)) {
      return latestState;
    }
  } catch (_) {
  }

  return expected;
}

function chooseStateAfterDisable_(payload, mode, beforeState, result){
  const expected = makeExpectedDisableState_(beforeState, mode);

  if (result && result.state) {
    return Object.assign(
      makeDefaultProgramSwitchState_(),
      expected,
      result.state
    );
  }

  try {
    const latestBackend = resolveBackendSafeForProgramTable_();
    const latestState = getProgramSwitchStateSafe_(latestBackend, payload);

    const fixedMode = String(mode || '');

    if (fixedMode === 'watch') {
      if (!!latestState.watchReserved === !!expected.watchReserved &&
          !!latestState.watchEnabled === !!expected.watchEnabled) {
        return latestState;
      }
    } else if (fixedMode === 'record') {
      if (!!latestState.recordReserved === !!expected.recordReserved &&
          !!latestState.recordEnabled === !!expected.recordEnabled) {
        return latestState;
      }
    }
  } catch (_) {
  }

  return expected;
}

function schedulePostMutationConfirm_(cell, expectedState){
  try {
    window.setTimeout(function(){
      try {
        if (!cell) {
          return;
        }

        const payload = extractPayload(cell);
        if (!payload) {
          renderCell(cell);
          return;
        }

        const backend = resolveBackendSafeForProgramTable_();
        const latestState = getProgramSwitchStateSafe_(backend, payload);

        const expected = Object.assign(
          makeDefaultProgramSwitchState_(),
          expectedState || {}
        );

        // 最新状態が期待状態と同じ方向なら最新状態で描画。
        // staleで未登録に戻りそうなら、直前の期待状態を維持する。
        const latestLooksSame =
          (!!latestState.watchReserved === !!expected.watchReserved) &&
          (!!latestState.recordReserved === !!expected.recordReserved) &&
          (!!latestState.watchEnabled === !!expected.watchEnabled) &&
          (!!latestState.recordEnabled === !!expected.recordEnabled);

        if (latestLooksSame) {
          renderCellWithKnownState_(cell, latestState);
        } else {
          renderCellWithKnownState_(cell, expected);
        }
      } catch (_) {
      }
    }, 120);
  } catch (_) {
  }
}

function requestParentReservationViewsRefresh_(){
  programTableTrace_('request-parent-refresh', {
    from:'program-table'
  });

  try {
    if (window.top &&
        window.top !== window &&
        typeof window.top.__epgPortalQueueReservationUiSync === 'function') {
      // 番組表 iframe からの予約操作は、
      // 時間割 iframe / 一覧 iframe の両方へ状態変更を伝える必要がある。
      // false にすると、親の予約一覧だけ更新されて別番組表側が stale になりやすい。
      window.top.__epgPortalQueueReservationUiSync(
        'program-table-request',
        true
      );
      return;
    }
  } catch (_) {
  }

  try {
    if (window.top &&
        window.top !== window &&
        typeof window.top.__epgPortalRefreshReservationViews === 'function') {
      window.top.__epgPortalRefreshReservationViews({
        syncCache:false,
        scheduleRetry:false,
        skipCacheSync:true,
        preferLocalState:true,
        refreshProgramFrames:false,
        notifyFrames:true
      });
      return;
    }
  } catch (_) {
  }

  const detail = {
    stamp: Date.now(),
    source: 'program-table',
    notifyFrames:true
  };

  try {
    document.dispatchEvent(new CustomEvent('epg-reservation-list-changed', {
      detail: detail
    }));
  } catch (_) {
  }

  try {
    window.dispatchEvent(new CustomEvent('epg-reservation-list-changed', {
      detail: detail
    }));
  } catch (_) {
  }
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core3_Html_(std::wstring& js)
        {
            js += LR"JS(

function getProgramBroadcastState_(payload){
  if (!payload) return 'unknown';
  const parse = (dateText, timeText) => {
    const d = /^(\d{4})[-\/](\d{1,2})[-\/](\d{1,2})$/.exec(String(dateText || '').trim());
    const t = /^(\d{1,2}):(\d{2})(?::(\d{2}))?/.exec(String(timeText || '').trim());
    if (!d || !t) return NaN;
    const y = +d[1], mo = +d[2], day = +d[3], h = +t[1], mi = +t[2], sec = +(t[3] || 0);
    if (mo < 1 || mo > 12 || day < 1 || day > 31 || h > 23 || mi > 59 || sec > 59) return NaN;
    const dt = new Date(y, mo - 1, day, h, mi, sec);
    if (dt.getFullYear() !== y || dt.getMonth() !== mo - 1 || dt.getDate() !== day) return NaN;
    return dt.getTime();
  };
  const date = payload.startDate || parseDateTextToIso(payload.dateText || payload.date || '');
  const range = parseTimeTextParts(payload.displayTimeText || payload.timeText || payload.time || '');
  const start = parse(date, payload.startTime || range.startTime);
  let endDate = payload.endDate || date;
  let endTime = payload.endTime || range.endTime;
  if (!payload.endDate && range.crossesDay) endDate = shiftIsoDate(date, 1) || date;
  let end = parse(endDate, endTime);
  if (Number.isFinite(start) && Number.isFinite(end) && end <= start && !payload.endDate && endTime) {
    end = parse(shiftIsoDate(date, 1), endTime);
  }
  if ((!Number.isFinite(end) || end <= start) && Number(payload.durationSec) > 0 && Number.isFinite(start)) {
    end = start + Number(payload.durationSec) * 1000;
  }
  if (!Number.isFinite(start) || !Number.isFinite(end) || end <= start) return 'unknown';
  const now = Date.now();
  return now < start ? 'before' : (now < end ? 'onair' : 'finished');
}

function buildProgramSwitchHtml_(state, payload){
  const broadcastState = getProgramBroadcastState_(payload);
  const watchBlocked = !state.watchReserved && broadcastState === 'finished';
  const recordBlocked = !state.recordReserved && broadcastState === 'finished';
  let html = ''
    + '<button type="button" class="epg-inline-switch-btn watch'
    + (state.watchReserved ? ' active' : '')
    + (state.watchReserved && state.watchEnabled === false ? ' disabled-state' : '')
    + '" data-mode="watch"' + (watchBlocked ? ' disabled' : '') + '>'
    + (state.watchReserved ? '視聴取消' : (watchBlocked ? '放送終了' : (broadcastState === 'onair' ? '途中から視聴' : '視聴予約')))
    + '</button>'
    + '<button type="button" class="epg-inline-switch-btn record'
    + (state.recordReserved ? ' active' : '')
    + (state.recordReserved && state.recordEnabled === false ? ' disabled-state' : '')
    + '" data-mode="record"' + (recordBlocked ? ' disabled' : '') + '>'
    + (state.recordReserved ? '録画取消' : (recordBlocked ? '放送終了' : (broadcastState === 'onair' ? '途中から録画' : '録画予約')))
    + '</button>';

  if (state.watchReserved) {
    html += ''
      + '<button type="button" class="epg-inline-switch-btn disable'
      + (state.watchEnabled === false ? ' active' : '')
      + '" data-disable-mode="watch">'
      + (state.watchEnabled === false ? '視聴有効化' : '視聴無効化')
      + '</button>';
  }

  if (state.recordReserved) {
    html += ''
      + '<button type="button" class="epg-inline-switch-btn disable'
      + (state.recordEnabled === false ? ' active' : '')
      + '" data-disable-mode="record">'
      + (state.recordEnabled === false ? '録画有効化' : '録画無効化')
      + '</button>';
  }

  return html;
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core3_Pending_(std::wstring& js)
        {
            js += LR"JS(
function makePendingProgramSwitchState_(currentState, mode, action){
  const state = Object.assign(
    makeDefaultProgramSwitchState_(),
    currentState || {}
  );

  state.__pending = true;
  state.__pendingMode = String(mode || '');
  state.__pendingAction = String(action || '');

  return state;
}

function buildProgramSwitchHtmlWithPending_(state){
  if (!state || !state.__pending) {
    return buildProgramSwitchHtml_(state);
  }

  const mode = String(state.__pendingMode || '');
  const action = String(state.__pendingAction || '');

  let watchText = state.watchReserved ? '視聴取消' : '視聴予約';
  let recordText = state.recordReserved ? '録画取消' : '録画予約';

  if (mode === 'watch') {
    watchText = action === 'remove' ? '視聴取消中...' : '視聴登録中...';
  } else if (mode === 'record') {
    recordText = action === 'remove' ? '録画取消中...' : '録画登録中...';
  }

  return ''
    + '<button type="button" class="epg-inline-switch-btn watch pending'
    + (state.watchReserved ? ' active' : '')
    + '" data-mode="watch" disabled>'
    + watchText
    + '</button>'
    + '<button type="button" class="epg-inline-switch-btn record pending'
    + (state.recordReserved ? ' active' : '')
    + '" data-mode="record" disabled>'
    + recordText
    + '</button>';
}

function renderCellPending_(cell, payload, backend, mode){
  if (!cell || !payload) return;

  let host = null;

  try {
    host = ensureHost(cell);
    if (!host) return;

    const currentState = getProgramSwitchStateSafe_(backend, payload);
    const fixedMode = String(mode || '');

    const alreadyReserved =
      fixedMode === 'watch'
        ? !!currentState.watchReserved
        : !!currentState.recordReserved;

    const pendingState = makePendingProgramSwitchState_(
      currentState,
      fixedMode,
      alreadyReserved ? 'remove' : 'add'
    );

    host.innerHTML = buildProgramSwitchHtmlWithPending_(pendingState);
    applyCellMarker(cell, currentState);
  } catch (e) {
    console.warn('[program-table-switch] render pending failed', e, cell);
  }
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core3_ClickMode_(std::wstring& js)
        {
            js += LR"JS(
function attachProgramModeHandlers_(host, cell, payload, backend){
  host.querySelectorAll('button[data-mode]').forEach(btn => {
    btn.addEventListener('click', (ev) => {
      ev.preventDefault();
      ev.stopPropagation();

      const mode = String(btn.getAttribute('data-mode') || '');
programTableTrace_('click-mode-begin', {
  mode: mode,
  text: btn.textContent ? btn.textContent.trim() : '',
  title: payload && payload.title,
  service: payload && payload.serviceName,
  eventId: payload && payload.eventId,
  startDate: payload && payload.startDate,
  startTime: payload && payload.startTime
});

      try {
        const liveBackend = resolveBackendSafeForProgramTable_();

        if (!liveBackend || typeof liveBackend.toggle !== 'function') {
          console.error('[program-table-switch] toggle backend unavailable', payload, mode);
          renderCell(cell);
          return;
        }

        const beforeState = getProgramSwitchStateSafe_(liveBackend, payload);
        const alreadyReserved =
          mode === 'watch'
            ? !!beforeState.watchReserved
            : !!beforeState.recordReserved;

        const willReserve = !alreadyReserved;
        if (willReserve && getProgramBroadcastState_(payload) === 'finished') {
          renderCell(cell);
          return;
        }

        renderCellPending_(cell, payload, liveBackend, mode);

        window.setTimeout(function(){
          try {
            const latestBackend = resolveBackendSafeForProgramTable_();

programTableTrace_('backend-before-toggle', {
  hasBackend: !!latestBackend,
  hasToggle: !!(latestBackend && typeof latestBackend.toggle === 'function'),
  toggleHasEnter: !!(
    latestBackend &&
    typeof latestBackend.toggle === 'function' &&
    String(latestBackend.toggle || '').indexOf('bridge-toggle-enter') >= 0
  ),
  toggleHead: latestBackend && latestBackend.toggle
    ? String(latestBackend.toggle || '').slice(0, 160)
    : ''
});

            if (!latestBackend || typeof latestBackend.toggle !== 'function') {
              console.error('[program-table-switch] toggle backend unavailable after pending', payload, mode);
              renderCell(cell);
              return;
            }

programTableTrace_('bridge-toggle-call', {
  mode: mode,
  title: payload && payload.title,
  eventId: payload && payload.eventId
});

            const result = latestBackend.toggle(payload, mode);

programTableTrace_('bridge-toggle-return', {
  mode: mode,
  ok: result && result.ok,
  reason: result && result.reason,
  source: result && result.source
});
            if (!result || result.ok === false) {
              console.error('[program-table-switch] toggle failed', result, payload, mode);
              renderCell(cell);
              return;
            }

            const finalState = chooseStateAfterToggle_(
              payload,
              mode,
              beforeState,
              willReserve,
              result
            );

            renderCellWithKnownState_(cell, finalState);
requestParentReservationViewsRefresh_();
schedulePostMutationConfirm_(cell, finalState);
          } catch (e) {
            console.error('[program-table-switch] toggle exception', e, payload, mode);
            renderCell(cell);
          }
        }, 0);
      } catch (e) {
        console.error('[program-table-switch] toggle prepare exception', e, payload, mode);
        renderCell(cell);
      }
    }, { passive:false });
  });
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core3_ClickDisable_(std::wstring& js)
        {
            js += LR"JS(
function attachProgramDisableHandlers_(host, cell, payload, backend){
  host.querySelectorAll('button[data-disable-mode]').forEach(btn => {
    btn.addEventListener('click', (ev) => {
      ev.preventDefault();
      ev.stopPropagation();

      const mode = String(btn.getAttribute('data-disable-mode') || '');

      try {
        const liveBackend = resolveBackendSafeForProgramTable_();

        if (!liveBackend || typeof liveBackend.toggleDisable !== 'function') {
          console.error('[program-table-switch] toggleDisable backend unavailable', payload, mode);
          renderCell(cell);
          return;
        }

        const beforeState = getProgramSwitchStateSafe_(liveBackend, payload);

        renderCellPending_(cell, payload, liveBackend, mode);

        window.setTimeout(function(){
          try {
            const latestBackend = resolveBackendSafeForProgramTable_();

            if (!latestBackend || typeof latestBackend.toggleDisable !== 'function') {
              console.error('[program-table-switch] toggleDisable backend unavailable after pending', payload, mode);
              renderCell(cell);
              return;
            }

            const result = latestBackend.toggleDisable(payload, mode);
            if (!result || result.ok === false) {
              console.error('[program-table-switch] toggleDisable failed', result, payload, mode);
              renderCell(cell);
              return;
            }

            const finalState = chooseStateAfterDisable_(
              payload,
              mode,
              beforeState,
              result
            );

            renderCellWithKnownState_(cell, finalState);
requestParentReservationViewsRefresh_();
schedulePostMutationConfirm_(cell, finalState);
          } catch (e) {
            console.error('[program-table-switch] toggleDisable exception', e, payload, mode);
            renderCell(cell);
          }
        }, 0);
      } catch (e) {
        console.error('[program-table-switch] toggleDisable prepare exception', e, payload, mode);
        renderCell(cell);
      }
    }, { passive:false });
  });
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core3_Handlers_(std::wstring& js)
        {
            js += LR"JS(
function attachProgramSwitchHandlers_(host, cell, payload, backend){
  if (!host || !payload) {
    return;
  }

  attachProgramModeHandlers_(host, cell, payload, backend);
  attachProgramDisableHandlers_(host, cell, payload, backend);
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core3_RenderKnown_(std::wstring& js)
        {
            js += LR"JS(
function renderCellWithKnownState_(cell, knownState){
  if (!cell) return;

  let host = null;
  let payload = null;

  try {
    host = ensureHost(cell);
    if (!host) return;

    payload = extractPayload(cell);
    if (!payload) return;

    const backend = resolveBackendSafeForProgramTable_();
    const state = Object.assign(
      makeDefaultProgramSwitchState_(),
      knownState || {}
    );

    host.innerHTML = state.__pending ? buildProgramSwitchHtmlWithPending_(state) : buildProgramSwitchHtml_(state, payload);

    try {
      applyCellMarker(cell, state);
    } catch (e) {
      console.warn('[program-table-switch] applyCellMarker known state failed', e, cell);
    }

    attachProgramSwitchHandlers_(host, cell, payload, backend);
  } catch (e) {
    console.error('[program-table-switch] renderCellWithKnownState failed', e, cell, knownState);

    try {
      if (host) {
        host.innerHTML =
          '<span class="epg-inline-switch-error" title="予約スイッチ即時描画エラー">予約</span>';
      }
    } catch (_) {
    }
  }
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core3_RenderCell_(std::wstring& js)
        {
            js += LR"JS(
function renderCell(cell){
  if (!cell) {
    return;
  }

  let host = null;
  let payload = null;

  try {
    host = ensureHost(cell);
    if (!host) {
      return;
    }

    payload = extractPayload(cell);
    if (!payload) {
      host.innerHTML =
        '<span class="epg-inline-switch-error" title="番組情報を取得できません">予約情報なし</span>';
      return;
    }

    const backend = resolveBackendSafeForProgramTable_();
    const state = getProgramSwitchStateSafe_(backend, payload);

    host.innerHTML = buildProgramSwitchHtml_(state, payload);

    try {
      applyCellMarker(cell, state);
    } catch (e) {
      console.warn('[program-table-switch] applyCellMarker failed', e, cell);
    }

    attachProgramSwitchHandlers_(host, cell, payload, backend);
  } catch (e) {
    console.error('[program-table-switch] renderCell failed', e, cell);

    try {
      if (host) {
        host.innerHTML =
          '<span class="epg-inline-switch-error" title="予約スイッチ描画エラー">予約</span>';
      }
    } catch (_) {
    }
  }
}
)JS";
        }

        void AppendProgramTableSwitchScript_Core3_(std::wstring& js)
        {
            AppendProgramTableSwitchScript_Core3_Cells_(js);
            AppendProgramTableSwitchScript_Core3_Payload_(js);
            AppendProgramTableSwitchScript_Core3_Host_(js);
            AppendProgramTableSwitchScript_Core3_State_(js);
            AppendProgramTableSwitchScript_Core3_Html_(js);
            AppendProgramTableSwitchScript_Core3_Pending_(js);
            AppendProgramTableSwitchScript_Core3_ClickMode_(js);
            AppendProgramTableSwitchScript_Core3_ClickDisable_(js);
            AppendProgramTableSwitchScript_Core3_Handlers_(js);
            AppendProgramTableSwitchScript_Core3_RenderKnown_(js);
            AppendProgramTableSwitchScript_Core3_RenderCell_(js);
        }

        void AppendProgramTableSwitchScript_Core4_RenderQueue_(std::wstring& js)
        {
            js += LR"JS(
let gRenderQueued = false;

function queueRenderAll(){
  if (gRenderQueued) return;
  gRenderQueued = true;

  const runner = function(){
    gRenderQueued = false;
    renderAll();
  };

  if (typeof window.requestAnimationFrame === 'function') {
    window.requestAnimationFrame(runner);
  } else {
    window.setTimeout(runner, 0);
  }
}

function renderAll(){
  const cells = getCandidateCells();

  programTableTrace_('render-all-begin', {
    cells: cells.length
  });

  cells.forEach(renderCell);

  programTableTrace_('render-all-end', {
    cells: cells.length
  });
}

window.__epgPortalProgramTableRenderAll = queueRenderAll;
window.__epgPortalProgramTableRenderNow = renderAll;

window.__epgPortalProgramTableDebug = {
  getCells: function(){
    return getCandidateCells();
  },

  dump: function(limit){
    const cells = getCandidateCells();
    const max = Number(limit || 100);

    return cells.slice(0, max).map(cell => {
      const payload = extractPayload(cell);
      const backend = resolveBackendSafeForProgramTable_();
      const state = payload ? getProgramSwitchStateSafe_(backend, payload) : null;

      return {
        title: payload && payload.title,
        service: payload && payload.serviceName,
        serviceKey: payload && payload.serviceKey,
        networkId: payload && payload.networkId,
        tsId: payload && payload.tsId,
        serviceId: payload && payload.serviceId,
        eventId: payload && payload.eventId,
        startDate: payload && payload.startDate,
        startTime: payload && payload.startTime,
        time: payload && payload.time,
        state: state
      };
    });
  },

  dumpReserved: function(limit){
    return this.dump(limit || 1000).filter(x =>
      x.state && (x.state.watchReserved || x.state.recordReserved)
    );
  }
};
)JS";
        }

        void AppendProgramTableSwitchScript_Core4_Bootstrap_(std::wstring& js)
        {
            js += LR"JS(
function bootstrapProgramTableSwitch_(){
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', queueRenderAll, { once:true });
  } else {
    queueRenderAll();
  }

  window.addEventListener('pageshow', queueRenderAll);

  // focus は重いので停止
  // window.addEventListener('focus', queueRenderAll);
}

bootstrapProgramTableSwitch_();
)JS";
        }

        void AppendProgramTableSwitchScript_Core4_EventBridge_(std::wstring& js)
        {
            js += LR"JS(
let gLastReservationEventKey_ = '';
let gReservationEventRenderPending_ = false;
let gReservationEventPendingKey_ = '';
let gReservationEventPendingTimer_ = 0;
let gReservationEventPendingWatchUntil_ = 0;

function makeReservationEventKeyForProgramTable_(detail){
  return String(detail.stamp || '') + '|'
    + String(detail.source || '') + '|'
    + String(Array.isArray(detail.items) ? detail.items.length : -1) + '|'
    + String(detail.notifyFrames);
}

function isCurrentProgramFrameVisible_(){
  try {
    const fe = window.frameElement;
    if (!fe) {
      return true;
    }

    const style = window.top.getComputedStyle(fe);
    if (!style ||
        style.display === 'none' ||
        style.visibility === 'hidden') {
      return false;
    }

    const rect = fe.getBoundingClientRect();
    return rect.width > 0 && rect.height > 0;
  } catch (_) {
    return true;
  }
}

function clearPendingReservationRender_(){
  gReservationEventRenderPending_ = false;
  gReservationEventPendingKey_ = '';

  if (gReservationEventPendingTimer_) {
    try {
      window.clearTimeout(gReservationEventPendingTimer_);
    } catch (_) {
    }
    gReservationEventPendingTimer_ = 0;
  }
}

function flushPendingReservationRenderIfVisible_(reason){
  if (!gReservationEventRenderPending_) {
    return false;
  }

  if (!isCurrentProgramFrameVisible_()) {
    return false;
  }

  programTableTrace_('reservation-event-pending-flush', {
    reason:String(reason || ''),
    eventKey:gReservationEventPendingKey_
  });

  clearPendingReservationRender_();
  queueRenderAll();
  return true;
}

function schedulePendingReservationRenderCheck_(eventKey){
  gReservationEventRenderPending_ = true;
  gReservationEventPendingKey_ = String(eventKey || '');
  gReservationEventPendingWatchUntil_ = Date.now() + 60000;

  if (gReservationEventPendingTimer_) {
    return;
  }

  const tick = function(){
    gReservationEventPendingTimer_ = 0;

    if (!gReservationEventRenderPending_) {
      return;
    }

    if (flushPendingReservationRenderIfVisible_('visibility-watch')) {
      return;
    }

    if (Date.now() >= gReservationEventPendingWatchUntil_) {
      programTableTrace_('reservation-event-pending-watch-expired', {
        eventKey:gReservationEventPendingKey_
      });
      return;
    }

    gReservationEventPendingTimer_ = window.setTimeout(tick, 250);
  };

  gReservationEventPendingTimer_ = window.setTimeout(tick, 250);
}

function handleProgramTableReservationChanged_(ev){
  try {
    const detail = ev && ev.detail ? ev.detail : {};
    const eventKey = makeReservationEventKeyForProgramTable_(detail);

    if (eventKey && eventKey === gLastReservationEventKey_) {
      programTableTrace_('reservation-event-duplicate-skip', {
        notifyFrames: !!detail.notifyFrames,
        source: detail.source || '',
        items: Array.isArray(detail.items) ? detail.items.length : -1
      });
      return;
    }

    gLastReservationEventKey_ = eventKey;

    programTableTrace_('reservation-event-received', {
      notifyFrames: !!detail.notifyFrames,
      source: detail.source || '',
      items: Array.isArray(detail.items) ? detail.items.length : -1
    });

    if (detail && detail.notifyFrames === false) {
      return;
    }

    if (!isCurrentProgramFrameVisible_()) {
      programTableTrace_('reservation-event-hidden-frame-skip', {
        notifyFrames: !!detail.notifyFrames,
        source: detail.source || '',
        items: Array.isArray(detail.items) ? detail.items.length : -1
      });

      schedulePendingReservationRenderCheck_(eventKey);
      return;
    }

    clearPendingReservationRender_();
    queueRenderAll();
  } catch (e) {
    programTableTrace_('reservation-event-error', {
      message: e && e.message ? e.message : String(e)
    });

    queueRenderAll();
  }
}

function bindProgramTableReservationEvents_(){
  document.addEventListener(
    'epg-reservation-list-changed',
    handleProgramTableReservationChanged_
  );

  window.addEventListener(
    'epg-reservation-list-changed',
    handleProgramTableReservationChanged_
  );

  window.addEventListener('pageshow', function(){
    flushPendingReservationRenderIfVisible_('pageshow');
  });

  document.addEventListener('visibilitychange', function(){
    flushPendingReservationRenderIfVisible_('visibilitychange');
  });

  window.__epgPortalProgramTableFlushPendingReservationRender = function(reason){
    if (flushPendingReservationRenderIfVisible_(reason || 'external')) {
      return true;
    }

    if (isCurrentProgramFrameVisible_()) {
      queueRenderAll();
      return true;
    }

    return false;
  };
}

bindProgramTableReservationEvents_();
)JS";
        }

        void AppendProgramTableSwitchScript_Core4_Close_(std::wstring& js)
        {
            js += LR"JS(
})();
)JS";
        }

        void AppendProgramTableSwitchScript_Core4_(std::wstring& js)
        {
            AppendProgramTableSwitchScript_Core4_RenderQueue_(js);
            AppendProgramTableSwitchScript_Core4_Bootstrap_(js);
            AppendProgramTableSwitchScript_Core4_EventBridge_(js);
            AppendProgramTableSwitchScript_Core4_Close_(js);
        }


        std::wstring BuildProgramTableSwitchScript_()
        {
            std::wstring js;
            js.reserve(static_cast<std::size_t>(96) * 1024U);

            AppendProgramTableSwitchScript_Core1_(js);
            AppendProgramTableSwitchScript_Core2_(js);
            AppendProgramTableSwitchScript_Core3_(js);
            AppendProgramTableSwitchScript_Core4_(js);

            return js;
        }

        bool WritePortalCssIfMissing_(const std::filesystem::path& outputDir)
        {
            if (outputDir.empty()) {
                DebugLog(L"[EPG-PORTAL][CSS][ERROR] outputDir is empty");
                return false;
            }

            const std::filesystem::path cssPath = outputDir / L"portal.css";

            std::error_code ec;
            if (std::filesystem::exists(cssPath, ec) && !ec) {
                return true;
            }

            std::wstring css;
            css.reserve(1024);

            css += L"/* EPG Portal external CSS placeholder. */\n";
            css += L"/* This file is generated to satisfy index.html stylesheet loading. */\n";
            css += L"html{box-sizing:border-box;}\n";
            css += L"*,*::before,*::after{box-sizing:inherit;}\n";

            const bool ok = EpgHtmlPortal_Common::WriteUtf8BomTextFile(
                cssPath,
                css);

            if (ok) {
                DebugLogFormat(
                    L"[EPG-PORTAL][CSS] generated: %ls",
                    cssPath.wstring().c_str());
            }
            else {
                DebugLogFormat(
                    L"[EPG-PORTAL][CSS][ERROR] failed to generate: %ls",
                    cssPath.wstring().c_str());
            }

            return ok;
        }

        bool WriteRuntimeSwitchScript_(const std::filesystem::path& path)
        {
            EPG_PORTAL_SWITCH_CPP_LOG(
                L"BEGIN WriteRuntimeSwitchScript path=%ls",
                path.wstring().c_str());

            const std::wstring js = BuildRuntimeSwitchScript_();

            EPG_PORTAL_SWITCH_CPP_LOG(
                L"BUILD RuntimeSwitchScript chars=%zu path=%ls",
                js.size(),
                path.wstring().c_str());

            const bool ok = EpgHtmlPortal_Common::WriteUtf8BomTextFile(
                path,
                js);

            DebugLogFormat(
                L"[EPG-PORTAL][SWITCH] runtime script write %ls path=%ls chars=%zu",
                ok ? L"OK" : L"NG",
                path.wstring().c_str(),
                js.size());

            return ok;
        }

        bool WriteProgramTableSwitchScript_(const std::filesystem::path& path)
        {
            EPG_PORTAL_SWITCH_CPP_LOG(
                L"BEGIN WriteProgramTableSwitchScript path=%ls",
                path.wstring().c_str());

            const std::wstring js = BuildProgramTableSwitchScript_();

            EPG_PORTAL_SWITCH_CPP_LOG(
                L"BUILD ProgramTableSwitchScript chars=%zu path=%ls",
                js.size(),
                path.wstring().c_str());

            const bool ok = EpgHtmlPortal_Common::WriteUtf8BomTextFile(
                path,
                js);

            DebugLogFormat(
                L"[EPG-PORTAL][PROGRAM-TABLE-SWITCH] write %ls path=%ls chars=%zu",
                ok ? L"OK" : L"NG",
                path.wstring().c_str(),
                js.size());

            return ok;
        }


    }

    void AppendRuntimeSwitchScript(std::wstring& js)
    {
        js += L"\n";
        js += BuildRuntimeSwitchScript_();
        js += L"\n";
    }

    bool WriteRuntimeSwitchFragment(const EpgHtmlPortal::WriteOptions& opt)
    {
        if (opt.outputDir.empty()) {
            DebugLog(L"[EPG-PORTAL][SWITCH][ERROR] outputDir is empty");
            return false;
        }

        const bool cssOk = WritePortalCssIfMissing_(opt.outputDir);

        const std::filesystem::path out = opt.outputDir / L"portal_runtime_switch.js";
        const bool jsOk = WriteRuntimeSwitchScript_(out);

        return cssOk && jsOk;
    }

    bool WriteProgramTableSwitchFragment(const EpgHtmlPortal::WriteOptions& opt)
    {
        if (opt.outputDir.empty()) {
            DebugLog(L"[EPG-PORTAL][PROGRAM-TABLE-SWITCH][ERROR] outputDir is empty");
            return false;
        }

        const std::filesystem::path out = opt.outputDir / L"portal_program_table_switch.js";
        return WriteProgramTableSwitchScript_(out);
    }
}