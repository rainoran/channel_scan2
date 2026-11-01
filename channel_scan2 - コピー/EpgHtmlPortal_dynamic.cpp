#include "EpgHtmlPortal_dynamic.hpp"
#include "EpgHtmlPortal_SwitchScript.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "DebugManager.h"
#include "EpgHtmlPortal_Assets.hpp"
#include "EpgHtmlPortal_Common.hpp"
#include "EpgHtmlPortal_ReservationData.hpp"
#include "EpgHtmlPortal_RuntimeData.hpp"
#include "EpgHtmlPortal_SearchData.hpp"

namespace EpgHtmlPortal_dynamic
{
    namespace
    {
        void AddUniqueRoot_(
            std::vector<std::filesystem::path>& roots,
            const std::filesystem::path& p)
        {
            if (p.empty()) {
                return;
            }

            if (std::find(roots.begin(), roots.end(), p) == roots.end()) {
                roots.push_back(p);
            }
        }

        std::vector<std::filesystem::path> BuildRuntimeSearchRoots_(
            const EpgHtmlPortal::WriteOptions& opt)
        {
            std::vector<std::filesystem::path> roots =
                EpgHtmlPortal_Assets::BuildDefaultTemplateSearchRoots();

            if (!opt.outputDir.empty()) {
                AddUniqueRoot_(roots, opt.outputDir / L"templates" / L"EpgHtmlPortal");
                AddUniqueRoot_(roots, opt.outputDir / L"templates");
                AddUniqueRoot_(roots, opt.outputDir);
            }

            return roots;
        }

        std::vector<std::filesystem::path> BuildRuntimeFragmentList_()
        {
            return {
                std::filesystem::path(L"portal_runtime_core.js"),
                std::filesystem::path(L"portal_runtime_search.js"),
                std::filesystem::path(L"portal_runtime_reservation.js"),
                std::filesystem::path(L"portal_runtime_switch.js"),
                std::filesystem::path(L"portal_runtime_page.js")
            };
        }

        bool PathsEquivalentSafe_(
            const std::filesystem::path& lhs,
            const std::filesystem::path& rhs)
        {
            std::error_code ec;

            if (std::filesystem::exists(lhs, ec) && !ec &&
                std::filesystem::exists(rhs, ec) && !ec) {
                std::error_code ecEq;
                const bool eq = std::filesystem::equivalent(lhs, rhs, ecEq);
                if (!ecEq) {
                    return eq;
                }
            }

            return false;
        }

        bool IsFallbackRuntimeScriptText_(std::wstring_view text)
        {
            return
                text.find(L"runtime テンプレートが見つからないため簡易モードで表示中です。") != std::wstring::npos ||
                text.find(L"window.switchPage = noop;") != std::wstring::npos;
        }

        bool IsFallbackRuntimeScript_(const std::filesystem::path& jsPath)
        {
            std::wstring text;
            if (!EpgHtmlPortal_Common::ReadTextFileToWide(jsPath, text)) {
                return false;
            }

            return IsFallbackRuntimeScriptText_(text);
        }

        static bool HasUsableRuntimeScriptInOutput_(const EpgHtmlPortal::WriteOptions& opt)
        {
            const std::filesystem::path runtimePath =
                opt.outputDir / L"epg_portal_runtime.js";

            if (std::filesystem::exists(runtimePath)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][DYNAMIC] force rebuild runtime js: %ls",
                    runtimePath.wstring().c_str());
            }

            // 詳細検索・予約スイッチなど runtime 側の更新を確実に反映するため、
            // 既存 epg_portal_runtime.js は保持せず毎回再生成する。
            return false;
        }

        bool TryBuildRuntimeFromFullScript_(
            const std::vector<std::filesystem::path>& roots,
            const std::filesystem::path& outputPath)
        {
            const std::filesystem::path found =
                EpgHtmlPortal_Assets::FindTemplateAssetPath(
                    L"epg_portal_runtime.js",
                    roots);

            if (found.empty()) {
                return false;
            }

            if (PathsEquivalentSafe_(found, outputPath)) {
                return false;
            }

            std::wstring text;
            if (!EpgHtmlPortal_Common::ReadTextFileToWide(found, text)) {
                return false;
            }

            if (text.empty() || IsFallbackRuntimeScriptText_(text)) {
                return false;
            }

            const bool ok =
                EpgHtmlPortal_Common::WriteUtf8BomTextFile(outputPath, text);

            if (ok) {
                DebugLogFormat(
                    L"[EPG-PORTAL][DYNAMIC] runtime built from full source: %ls",
                    found.wstring().c_str());
            }

            return ok;
        }

        bool TryBuildRuntimeFromFragments_(
            const std::vector<std::filesystem::path>& roots,
            const std::filesystem::path& outputPath)
        {
            std::wstring merged;
            bool any = false;

            for (const auto& rel : BuildRuntimeFragmentList_()) {
                const std::filesystem::path found =
                    EpgHtmlPortal_Assets::FindTemplateAssetPath(rel, roots);

                if (found.empty()) {
                    continue;
                }

                if (PathsEquivalentSafe_(found, outputPath)) {
                    continue;
                }

                std::wstring text;
                if (!EpgHtmlPortal_Common::ReadTextFileToWide(found, text)) {
                    continue;
                }

                if (text.empty()) {
                    continue;
                }

                if (any) {
                    merged += L"\n\n";
                }

                merged += text;
                any = true;

                DebugLogFormat(
                    L"[EPG-PORTAL][DYNAMIC] runtime fragment found: %ls",
                    found.wstring().c_str());
            }

            if (!any) {
                return false;
            }

            const bool ok =
                EpgHtmlPortal_Common::WriteUtf8BomTextFile(outputPath, merged);

            if (ok) {
                DebugLogFormat(
                    L"[EPG-PORTAL][DYNAMIC] runtime built from fragments: %ls",
                    outputPath.wstring().c_str());
            }

            return ok;
        }

        void AppendRuntimeGeneratedScript_Header_Base_(std::wstring& js)
        {
            js += LR"JS((function(){
'use strict';

let gCurrentSpace = (typeof DEFAULT_SPACE === 'number') ? DEFAULT_SPACE : 0;
let gCurrentPage = 'now';
let gLastBrowsePage = 'now';
let gLastResults = [];
let gSelectedProgram = null;
let gProgramCache = { now: [], past: [] };
let gProgramCacheBySpace = {};
let gSwitchRefreshToken = 0;

const EPG_RESERVE_BUTTON_MODE =
  String(window.EPG_RESERVE_BUTTON_MODE || 'select').toLowerCase();

function normalizeText(s){
  return String(s || '')
    .replace(/\u3000/g, ' ')
    .replace(/\u00A0/g, ' ')
    .replace(/\r/g, '\n')
    .replace(/\n{3,}/g, '\n\n')
    .replace(/[ \t]+/g, ' ')
    .replace(/\n +/g, '\n')
    .trim();
}

function normalizeServiceKey(s){
  return normalizeText(s || '').normalize('NFKC');
}

function normalizeSearchText(s){
  return normalizeText(s || '')
    .toLowerCase()
    .normalize('NFKC')
    .replace(/[\u30a1-\u30f6]/g, ch => String.fromCharCode(ch.charCodeAt(0) - 0x60))
    .replace(/[\s\-‐-‒–—―ー~～・\/]+/g, '');
}

function escapeHtml(s){
  return String(s || '')
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

function setText(id, value){
  const el = document.getElementById(id);
  if (el) {
    el.textContent = String(value || '');
  }
}

function hasPastSearchData(){
  return Array.isArray(gProgramCache.past) && gProgramCache.past.length > 0;
}

function resolveEffectiveSearchTarget(target){
  if (target === 'past' && !hasPastSearchData()) return 'now';
  if (target === 'all' && !hasPastSearchData()) return 'now';
  return target;
}

function getSelectedValues(selectId){
  const select = document.getElementById(selectId);
  if (!select) return [];
  return Array.from(select.selectedOptions || []).map(o => o.value).filter(Boolean);
}

function normalizeReservationMode(mode){
  const v = String(mode || '').toLowerCase();
  if (v.includes('record') || v.includes('録画')) return 'record';
  if (v.includes('watch') || v.includes('view') || v.includes('視聴')) return 'watch';
  if (v.includes('epg')) return 'epg';
  return 'reserve';
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Header_NormalizeReservation_(std::wstring& js)
        {
            js += LR"JS(
function normalizeReservationEntry(item, idx){
  return {
    id: String(item && item.id ? item.id : ('resv-' + idx)),
    label: normalizeText(item && item.label ? item.label : '予約項目'),
    category: normalizeText(item && item.category ? item.category : ''),
    mode: normalizeReservationMode(item && item.mode),
    enabled: !(item && item.enabled === false),
    date: normalizeText(item && item.date ? item.date : ''),
    weekday: normalizeText(item && item.weekday ? item.weekday : ''),
    time: normalizeText(item && item.time ? item.time : ''),
    endDate: normalizeText(item && item.endDate ? item.endDate : ''),
    endTime: normalizeText(item && item.endTime ? item.endTime : ''),
    service: normalizeText(item && item.service ? item.service : ''),
    target: normalizeText(item && item.target ? item.target : ''),
    schedule: normalizeText(item && item.schedule ? item.schedule : ''),
    note: normalizeText(item && item.note ? item.note : ''),
    summary: normalizeText(item && item.summary ? item.summary : ''),
    genre: normalizeText(item && item.genre ? item.genre : ''),
    reserveSource: normalizeText(item && item.reserveSource ? item.reserveSource : ''),
    dedupeKey: normalizeText(item && item.dedupeKey ? item.dedupeKey : ''),
    space: Number.isFinite(Number(item && item.space)) ? Number(item.space) : -1,
    channel: Number.isFinite(Number(item && item.channel)) ? Number(item.channel) : -1,
    networkId: Number(item && item.networkId ? item.networkId : 0),
    tsId: Number(item && item.tsId ? item.tsId : 0),
    serviceId: Number(item && item.serviceId ? item.serviceId : 0),
    eventId: Number(item && item.eventId ? item.eventId : 0),
    kind: Number(item && item.kind ? item.kind : 0),
    instance: Number(item && item.instance ? item.instance : 0),
    preferredTunerIndex: Number.isFinite(Number(item && item.preferredTunerIndex))
      ? Number(item.preferredTunerIndex)
      : -1,
    requiredTuners: Number.isFinite(Number(item && item.requiredTuners))
      ? Math.max(1, Number(item.requiredTuners))
      : 1
  };
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Header_ReservationMirror_(std::wstring& js)
        {
            js += LR"JS(
let gReservationRevision = 0;
let gReservationCommitBusy = false;

function normalizeReservationListForMirror_(list){
  if (!Array.isArray(list)) {
    return [];
  }

  return list
    .map((item, idx) => normalizeReservationEntry(item, idx))
    .filter(Boolean);
}

let gReservationList = normalizeReservationListForMirror_(window.EPG_RESERVATION_DATA);

window.gReservationList = gReservationList;

let gUiProgramReservations = [];
let gSuppressedProgramEventKeys = new Set();

function publishReservationMirror_(){
  window.gReservationList = gReservationList;
}

function makeReservationMirrorFingerprint_(list){
  if (!Array.isArray(list)) {
    return '';
  }

  return list.map(item => {
    if (!item) return '';
    return String(item.id || '')
      + '|'
      + String(item.kind || '')
      + '|'
      + String(item.instance || '')
      + '|'
      + String(item.mode || item.reserveMode || '')
      + '|'
      + String(item.dedupeKey || item.excludeKey || '');
  }).join('\n');
}

function setReservationMirrorList_(list, revision){
  gReservationList = normalizeReservationListForMirror_(list);
  gUiProgramReservations = [];
  publishReservationMirror_();

  if (Number.isFinite(Number(revision))) {
    gReservationRevision = Number(revision);
  } else {
    gReservationRevision += 1;
  }

  if (typeof invalidateReservationStateCache === 'function') {
    invalidateReservationStateCache();
  }

  if (typeof refreshReservationVisuals === 'function') {
    refreshReservationVisuals();
  }

  return gReservationList;
}

function syncReservationMirrorFromStaticData_(reason){
  if (!Array.isArray(window.EPG_RESERVATION_DATA)) {
    return false;
  }

  const staticList = normalizeReservationListForMirror_(window.EPG_RESERVATION_DATA);

  const currentFp = makeReservationMirrorFingerprint_(gReservationList);
  const staticFp = makeReservationMirrorFingerprint_(staticList);

  if (currentFp === staticFp) {
    return false;
  }

  // 初期ロード時に runtime が先行して空配列を掴んだ場合の復旧。
  // 既に画面操作で一時予約を追加している場合でも、静的データ側が更新されていれば反映する。
  gReservationList = staticList;
  gUiProgramReservations = [];
  publishReservationMirror_();
  gReservationRevision += 1;

  if (typeof invalidateReservationStateCache === 'function') {
    invalidateReservationStateCache();
  }

  if (typeof refreshReservationVisuals === 'function') {
    refreshReservationVisuals();
  }

  return true;
}

window.__epgPortalSetReservationList = function(list, revision){
  return setReservationMirrorList_(list, revision);
};

window.__epgPortalGetReservationList = function(){
  return Array.isArray(gReservationList) ? gReservationList : [];
};

window.__epgPortalSyncReservationDataFromStatic = function(reason){
  return syncReservationMirrorFromStaticData_(reason || 'manual');
};

function getNativeReservationBackend_(){
  try {
    if (window.__epgPortalNativeReservationBackend &&
        typeof window.__epgPortalNativeReservationBackend === 'object') {
      return window.__epgPortalNativeReservationBackend;
    }
  } catch (_) {
  }
  return null;
}

function callReservationBackend_(methodName, payload){
  const backend = getNativeReservationBackend_();

  if (!backend || typeof backend[methodName] !== 'function') {
    return Promise.resolve({
      ok: false,
      changed: false,
      reason: 'native-backend-missing:' + methodName,
      revision: gReservationRevision
    });
  }

  try {
    return Promise.resolve(backend[methodName](payload));
  } catch (e) {
    return Promise.resolve({
      ok: false,
      changed: false,
      reason: 'native-backend-exception:' + String(e && e.message ? e.message : e),
      revision: gReservationRevision
    });
  }
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Header_ReservationPayload_(std::wstring& js)
        {
            js += LR"JS(
function buildProgramReservationPayload_(program, mode){
  const fixedMode = normalizeReservationMode(mode);

  const payload = {};
  payload.id = String(program && program.id ? program.id : '');

  payload.space = Number.isFinite(Number(program && program.space))
    ? Number(program.space)
    : Number(gCurrentSpace);

  payload.networkId = Number(program && program.networkId ? program.networkId : 0);
  payload.tsId = Number(program && program.tsId ? program.tsId : 0);
  payload.serviceId = Number(program && program.serviceId ? program.serviceId : 0);
  payload.eventId = Number(program && program.eventId ? program.eventId : 0);

  payload.title = String(program && program.title ? program.title : '');
  payload.titleLabel = payload.title;
  payload.label = payload.title;

  payload.serviceName = String(program && program.service ? program.service : '');
  payload.service = payload.serviceName;
  payload.target = payload.serviceName;

  payload.summary = String(program && program.summary ? program.summary : '');
  payload.genre = String(program && program.genre ? program.genre : '');

  payload.dateText = String(program && program.dateText ? program.dateText : '');
  payload.date = payload.dateText;
  payload.weekdayText = String(program && program.weekdayText ? program.weekdayText : '');
  payload.weekday = payload.weekdayText;
  payload.timeText = String(program && program.time ? program.time : '');
  payload.time = payload.timeText;

  payload.reserveMode = fixedMode;
  payload.mode = fixedMode;
  payload.reserveSource = 'epg_portal_store';

  payload.enabled = !(program && program.enabled === false);
  payload.disabledByUser = !!(program && program.disabledByUser);
  payload.excludeFromAuto = !!(program && program.excludeFromAuto);

  payload.autoReserved = false;
  payload.requiredTuners = 1;
  payload.preferredTunerIndex = -1;

  return payload;
}

function makeReservationEntryFromProgram_(program, mode){
  const payload = buildProgramReservationPayload_(program, mode);

  return normalizeReservationEntry({
    id: payload.id || ('ui-saved-' + Date.now().toString(36) + '-' + Math.random().toString(36).slice(2, 8)),
    label: payload.label || '番組予約',
    category: payload.mode === 'record' ? '録画予約' : '視聴予約',
    mode: payload.mode,
    enabled: payload.enabled,
    date: payload.date,
    weekday: payload.weekday,
    time: payload.time,
    service: payload.service,
    target: payload.target,
    schedule: payload.date + ' ' + payload.time,
    note: 'tasks.ini保存済み',
    summary: payload.summary,
    genre: payload.genre,
    reserveSource: payload.reserveSource,
    dedupeKey: '',
    space: payload.space,
    channel: -1,
    kind: 0,
    instance: 0,
    networkId: payload.networkId,
    tsId: payload.tsId,
    serviceId: payload.serviceId,
    eventId: payload.eventId,
    preferredTunerIndex: payload.preferredTunerIndex,
    requiredTuners: payload.requiredTuners
  }, gReservationList.length);
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Header_ReservationCommit_(std::wstring& js)
        {
            js += LR"JS(
function applyReservationCommitResult_(result, fallbackApply){
  if (!result || result.ok !== true) {
    return false;
  }

  if (Array.isArray(result.reservations)) {
    setReservationMirrorList_(result.reservations, result.revision);
    return true;
  }

  if (Array.isArray(result.reservationData)) {
    setReservationMirrorList_(result.reservationData, result.revision);
    return true;
  }

  if (Array.isArray(result.list)) {
    setReservationMirrorList_(result.list, result.revision);
    return true;
  }

  if (typeof fallbackApply === 'function') {
    fallbackApply();
    publishReservationMirror_();
  }

  if (Number.isFinite(Number(result.revision))) {
    gReservationRevision = Number(result.revision);
  } else {
    gReservationRevision += 1;
  }

  if (typeof refreshReservationVisuals === 'function') {
    refreshReservationVisuals();
  }

  return true;
}

function makeCommitBusyResult_(){
  return {
    ok: false,
    changed: false,
    reason: 'commit-busy',
    revision: gReservationRevision
  };
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Header_ReservationStore1_(std::wstring& js)
        {
            js += LR"JS(
window.__epgReservationStore = {
  getRevision(){
    return gReservationRevision;
  },

  getList(){
    return Array.isArray(gReservationList) ? gReservationList : [];
  },

  setList(list, revision){
    setReservationMirrorList_(list, revision);
    return {
      ok: true,
      changed: true,
      revision: gReservationRevision
    };
  },

  upsertProgram(program, mode){
    if (gReservationCommitBusy) {
      return Promise.resolve(makeCommitBusyResult_());
    }

    const fixedMode = normalizeReservationMode(mode);
    const payload = buildProgramReservationPayload_(program, fixedMode);

    gReservationCommitBusy = true;

    return callReservationBackend_('upsertProgramReservation', payload)
      .then(result => {
        applyReservationCommitResult_(result, () => {
          const eventKey = getProgramEventKey(program);

          gReservationList = gReservationList.filter(item => {
            if (!item) return false;
            if (item.mode !== fixedMode) return true;
            return getReservationEventKey(item) !== eventKey;
          });

          gReservationList.push(makeReservationEntryFromProgram_(program, fixedMode));
          gSuppressedProgramEventKeys.delete(eventKey);
        });

        return result;
      })
      .finally(() => {
        gReservationCommitBusy = false;
      });
  },

  removeProgram(program, mode){
    if (gReservationCommitBusy) {
      return Promise.resolve(makeCommitBusyResult_());
    }

    const fixedMode = normalizeReservationMode(mode);
    const payload = buildProgramReservationPayload_(program, fixedMode);

    gReservationCommitBusy = true;

    return callReservationBackend_('removeProgramReservation', payload)
      .then(result => {
        applyReservationCommitResult_(result, () => {
          const eventKey = getProgramEventKey(program);

          gReservationList = gReservationList.filter(item => {
            if (!item) return false;
            if (item.mode !== fixedMode) return true;
            return getReservationEventKey(item) !== eventKey;
          });

          gSuppressedProgramEventKeys.add(eventKey);
        });

        return result;
      })
      .finally(() => {
        gReservationCommitBusy = false;
      });
  },
)JS";
        }

        void AppendRuntimeGeneratedScript_Header_ReservationStore2_(std::wstring& js)
        {
            js += LR"JS(
  setProgramEnabled(program, mode, enabled){
    if (gReservationCommitBusy) {
      return Promise.resolve(makeCommitBusyResult_());
    }

    const fixedMode = normalizeReservationMode(mode);
    const payload = buildProgramReservationPayload_(program, fixedMode);

    payload.enabled = !!enabled;
    payload.disabledByUser = !enabled;
    payload.excludeFromAuto = !enabled;

    gReservationCommitBusy = true;

    return callReservationBackend_('setProgramReservationEnabled', payload)
      .then(result => {
        applyReservationCommitResult_(result, () => {
          const eventKey = getProgramEventKey(program);

          gReservationList = gReservationList.map(item => {
            if (!item) return item;
            if (item.mode !== fixedMode) return item;
            if (getReservationEventKey(item) !== eventKey) return item;

            const next = Object.assign({}, item);
            next.enabled = !!enabled;
            next.disabledByUser = !enabled;
            next.excludeFromAuto = !enabled;
            return next;
          });
        });

        return result;
      })
      .finally(() => {
        gReservationCommitBusy = false;
      });
  },

  removeReservationById(id){
    if (gReservationCommitBusy) {
      return Promise.resolve(makeCommitBusyResult_());
    }

    const reservationId = String(id || '');
    if (!reservationId) {
      return Promise.resolve({
        ok: false,
        changed: false,
        reason: 'empty-id',
        revision: gReservationRevision
      });
    }

    gReservationCommitBusy = true;

    return callReservationBackend_('removeReservationById', { id: reservationId })
      .then(result => {
        applyReservationCommitResult_(result, () => {
          gReservationList = gReservationList.filter(item => {
            return String(item && item.id ? item.id : '') !== reservationId;
          });
        });

        return result;
      })
      .finally(() => {
        gReservationCommitBusy = false;
      });
  },
)JS";
        }

        void AppendRuntimeGeneratedScript_Header_ReservationStore3_(std::wstring& js)
        {
            js += LR"JS(
  toggleReservationEnabledById(id){
    const reservationId = String(id || '');
    const item = gReservationList.find(v => String(v && v.id ? v.id : '') === reservationId);

    if (!item) {
      return Promise.resolve({
        ok: false,
        changed: false,
        reason: 'not-found',
        revision: gReservationRevision
      });
    }

    if (gReservationCommitBusy) {
      return Promise.resolve(makeCommitBusyResult_());
    }

    const nextEnabled = !(item.enabled !== false);

    gReservationCommitBusy = true;

    return callReservationBackend_('setReservationEnabledById', {
      id: reservationId,
      enabled: nextEnabled
    }).then(result => {
      applyReservationCommitResult_(result, () => {
        gReservationList = gReservationList.map(v => {
          if (String(v && v.id ? v.id : '') !== reservationId) return v;

          const next = Object.assign({}, v);
          next.enabled = nextEnabled;
          next.disabledByUser = !nextEnabled;
          next.excludeFromAuto = !nextEnabled;
          return next;
        });
      });

      return result;
    }).finally(() => {
      gReservationCommitBusy = false;
    });
  }
};
)JS";
        }

        void AppendRuntimeGeneratedScript_Header_EventKey_(std::wstring& js)
        {
            js += LR"JS(
function getTunerPrefixBySpace(space){
  if (space === 0) return 'T';
  if (space === 1 || space === 2) return 'S';
  return 'U';
}

function formatReservationTunerLabel(item){
  const prefix = getTunerPrefixBySpace(Number(item && item.space));
  const idx = Number(item && item.preferredTunerIndex);

  if (!Number.isFinite(idx) || idx < 0) {
    return prefix + '自動';
  }

  const familyIndices = Array.isArray(gReservationList)
    ? Array.from(
        new Set(
          gReservationList
            .filter(v => getTunerPrefixBySpace(Number(v && v.space)) === prefix)
            .map(v => Number(v && v.preferredTunerIndex))
            .filter(v => Number.isFinite(v) && v >= 0)
        )
      ).sort((a, b) => a - b)
    : [];

  const localPos = familyIndices.indexOf(idx);
  if (localPos >= 0) {
    return prefix + String(localPos + 1);
  }

  return prefix + String(idx + 1);
}

function makeEventKeyFromParts(dedupeKey, serviceId, eventId, serviceName, title, dateText, timeText){
  const dk = normalizeText(dedupeKey || '');
  if (dk) {
    return 'dedupe:' + dk.normalize('NFKC');
  }

  const sid = Number(serviceId || 0);
  const eid = Number(eventId || 0);
  const dateKey = normalizeText(dateText || '');
  const timeKey = normalizeText(timeText || '');

  if (sid > 0 && eid > 0) {
    return 'sid:' + sid + '|eid:' + eid + '|date:' + dateKey + '|time:' + timeKey;
  }

  return 'svc:' + normalizeServiceKey(serviceName || '')
    + '|ttl:' + normalizeSearchText(title || '')
    + '|date:' + dateKey
    + '|time:' + timeKey;
}

function getProgramEventKey(program){
  if (!program) return '';
  return makeEventKeyFromParts(
    '',
    program.serviceId || 0,
    program.eventId || 0,
    program.service || '',
    program.title || '',
    program.dateText || '',
    program.time || ''
  );
}

function getReservationEventKey(item){
  if (!item) return '';
  return makeEventKeyFromParts(
    item.dedupeKey || '',
    item.serviceId || 0,
    item.eventId || 0,
    item.service || item.target || '',
    item.label || '',
    item.date || '',
    item.time || ''
  );
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Header_QueryReservation_(std::wstring& js)
        {
            js += LR"JS(
function clearSuppressedProgramEventKey(eventKey){
  if (!eventKey) return;
  gSuppressedProgramEventKeys.delete(eventKey);
}

function isSuppressedReservation(item){
  if (!item || item.mode === 'epg') return false;
  const key = getReservationEventKey(item);
  return key ? gSuppressedProgramEventKeys.has(key) : false;
}

function getVisibleBaseReservations(){
  return gReservationList.filter(item => item && !isSuppressedReservation(item));
}

function getMergedReservationList(){
  return getVisibleBaseReservations().concat(gUiProgramReservations);
}

function getProgramReservations(program, includeDisabled){
  const eventKey = getProgramEventKey(program);
  if (!eventKey) return [];

  return getMergedReservationList().filter(item => {
    if (!item) return false;
    if (!includeDisabled && !item.enabled) return false;
    if (item.mode === 'epg') return false;
    return getReservationEventKey(item) === eventKey;
  });
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Header_StyleReservation_(std::wstring& js)
        {
            js += LR"JS(
function buildInlineTagHtml(label, bg, fg){
  return '<span style="display:inline-block;margin-left:6px;padding:2px 8px;border-radius:999px;font-size:12px;line-height:1.4;background:'
    + bg + ';color:' + fg + ';white-space:nowrap;">'
    + escapeHtml(label)
    + '</span>';
}

function getProgramReservationState(program){
  const matches = getProgramReservations(program, true);
  const enabledMatches = matches.filter(item => item.enabled);
  const disabledMatches = matches.filter(item => !item.enabled);

  const tunerLabels = Array.from(
    new Set(
      matches
        .map(item => formatReservationTunerLabel(item))
        .filter(v => !!v)
    )
  );

  const enabledRecord = enabledMatches.some(item => item.mode === 'record');
  const enabledWatch  = enabledMatches.some(item => item.mode === 'watch');
  const enabledReserve = enabledMatches.some(item => item.mode === 'reserve');

  const disabledRecord = disabledMatches.some(item => item.mode === 'record');
  const disabledWatch  = disabledMatches.some(item => item.mode === 'watch');
  const disabledReserve = disabledMatches.some(item => item.mode === 'reserve');

  let mode = 'none';
  let label = '未予約';

  if (enabledRecord) {
    mode = 'record';
    label = '録画予約';
  } else if (enabledWatch) {
    mode = 'watch';
    label = '視聴予約';
  } else if (enabledReserve) {
    mode = 'reserve';
    label = '予約';
  } else if (disabledRecord) {
    mode = 'record-disabled';
    label = '録画予約(無効)';
  } else if (disabledWatch) {
    mode = 'watch-disabled';
    label = '視聴予約(無効)';
  } else if (disabledReserve) {
    mode = 'reserve-disabled';
    label = '予約(無効)';
  }

  return {
    reserved: matches.length > 0,
    count: matches.length,
    enabledCount: enabledMatches.length,
    disabledCount: disabledMatches.length,
    mode: mode,
    modeLabel: label,
    tunerLabels: tunerLabels
  };
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Header_StyleReservation2_(std::wstring& js)
        {
            js += LR"JS(
function getProgramRowInlineStyle(state){
  if (!state || !state.reserved) return '';

  if (state.mode === 'record') {
    return 'background:rgba(255,132,156,.14);box-shadow:inset 4px 0 0 #d24a61;';
  }
  if (state.mode === 'watch') {
    return 'background:rgba(123,170,255,.12);box-shadow:inset 4px 0 0 #4f8cff;';
  }
  if (state.mode === 'reserve') {
    return 'background:rgba(173,142,255,.12);box-shadow:inset 4px 0 0 #8c6cff;';
  }
  if (state.mode === 'record-disabled') {
    return 'background:rgba(255,132,156,.08);box-shadow:inset 4px 0 0 #c7a3ad;';
  }
  if (state.mode === 'watch-disabled') {
    return 'background:rgba(123,170,255,.08);box-shadow:inset 4px 0 0 #9db2d8;';
  }
  if (state.mode === 'reserve-disabled') {
    return 'background:rgba(173,142,255,.08);box-shadow:inset 4px 0 0 #b2a2d8;';
  }

  return '';
}

function getProgramTagsHtml(state){
  if (!state || !state.reserved) return '';

  let html = '';
  if (state.mode === 'record') {
    html += buildInlineTagHtml('録画予約', '#8f2b3b', '#ffffff');
  } else if (state.mode === 'watch') {
    html += buildInlineTagHtml('視聴予約', '#2d6cdf', '#ffffff');
  } else if (state.mode === 'reserve') {
    html += buildInlineTagHtml('予約', '#6f58c9', '#ffffff');
  } else if (state.mode === 'record-disabled') {
    html += buildInlineTagHtml('録画予約(無効)', '#d7c2c8', '#4a2b33');
  } else if (state.mode === 'watch-disabled') {
    html += buildInlineTagHtml('視聴予約(無効)', '#ccd8ea', '#243a62');
  } else if (state.mode === 'reserve-disabled') {
    html += buildInlineTagHtml('予約(無効)', '#ddd6ef', '#463d69');
  }

  state.tunerLabels.forEach(label => {
    html += buildInlineTagHtml(label, '#374151', '#ffffff');
  });

  return html;
}

function buildActionButtonHtml(label, onClickJs, kind){
  let bg = '#2d6cdf';
  let border = '#4f8cff';
  let fg = '#ffffff';

  if (kind === 'danger') {
    bg = '#8f2b3b';
    border = '#d24a61';
  } else if (kind === 'record') {
    bg = '#8f2b3b';
    border = '#d24a61';
  } else if (kind === 'sub') {
    bg = '#243043';
    border = '#42506a';
  } else if (kind === 'disabled') {
    bg = '#d7dee8';
    border = '#aab8c8';
    fg = '#243043';
  }

  const safeOnClick =
    String(onClickJs || '') + ';event.preventDefault();event.stopPropagation();';

  return '<button type="button" '
    + 'style="padding:7px 11px;border-radius:8px;border:1px solid ' + border + ';'
    + 'background:' + bg + ';color:' + fg + ';cursor:pointer;font:inherit;" '
    + 'onclick="' + escapeHtml(safeOnClick) + '">'
    + escapeHtml(label)
    + '</button>';
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Header_Actions_(std::wstring& js)
        {
            js += LR"JS(
function refreshReservationVisuals(){
  if (Array.isArray(gLastResults)) {
    renderSearchResults(gLastResults);
  }
  if (gSelectedProgram) {
    showProgramDetail(gSelectedProgram);
  }
  if (typeof renderReservationPage === 'function') {
    renderReservationPage();
  }
  if (typeof refreshFrameReservationButtons === 'function') {
    refreshFrameReservationButtons();
  }
}

function reserveProgramWithMode(program, mode){
  const fixedMode = normalizeReservationMode(mode);
  if (fixedMode !== 'record' && fixedMode !== 'watch') {
    return Promise.resolve({
      ok: false,
      reason: 'invalid-mode'
    });
  }

  if (!window.__epgReservationStore) {
    setText('detailLastUpdate', '予約Storeが未初期化です。');
    return Promise.resolve({
      ok: false,
      reason: 'reservation-store-missing'
    });
  }

  setText('detailLastUpdate', '予約を保存中...');

  return window.__epgReservationStore
    .upsertProgram(program, fixedMode)
    .then(result => {
      if (!result || result.ok !== true) {
        const reason = result && result.reason ? result.reason : 'reservation-save-failed';
        setText('detailLastUpdate', '予約保存に失敗: ' + reason);
      } else {
        setText('detailLastUpdate', '予約を保存しました。');
      }
      return result;
    });
}

function cancelProgram(program){
  if (!program || !window.__epgReservationStore) {
    setText('detailLastUpdate', '予約Storeが未初期化です。');
    return Promise.resolve({
      ok: false,
      reason: 'reservation-store-missing'
    });
  }

  setText('detailLastUpdate', '予約を削除中...');

  const state = getProgramReservationState(program);
  const tasks = [];

  if (state.mode === 'record' || state.mode === 'record-disabled') {
    tasks.push(window.__epgReservationStore.removeProgram(program, 'record'));
  }

  if (state.mode === 'watch' || state.mode === 'watch-disabled') {
    tasks.push(window.__epgReservationStore.removeProgram(program, 'watch'));
  }

  if (tasks.length === 0) {
    tasks.push(window.__epgReservationStore.removeProgram(program, 'record'));
    tasks.push(window.__epgReservationStore.removeProgram(program, 'watch'));
  }

  return Promise.all(tasks)
    .then(results => {
      const ok = results.some(r => r && r.ok === true);
      if (!ok) {
        const found = results.find(r => r && r.reason);
        const reason = found ? found.reason : 'reservation-remove-failed';
        setText('detailLastUpdate', '予約削除に失敗: ' + reason);
      } else {
        setText('detailLastUpdate', '予約を削除しました。');
      }
      return {
        ok: ok,
        results: results
      };
    });
}

function resolveReserveModeFromSetting(){
  if (EPG_RESERVE_BUTTON_MODE === 'record') return 'record';
  if (EPG_RESERVE_BUTTON_MODE === 'watch') return 'watch';
  return '';
}

function reserveProgram(program){
  const mode = resolveReserveModeFromSetting();
  if (!mode) {
    return;
  }
  reserveProgramWithMode(program, mode);
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Header_(std::wstring& js)
        {
            AppendRuntimeGeneratedScript_Header_Base_(js);
            AppendRuntimeGeneratedScript_Header_NormalizeReservation_(js);
            AppendRuntimeGeneratedScript_Header_ReservationMirror_(js);
            AppendRuntimeGeneratedScript_Header_ReservationPayload_(js);
            AppendRuntimeGeneratedScript_Header_ReservationCommit_(js);
            AppendRuntimeGeneratedScript_Header_ReservationStore1_(js);
            AppendRuntimeGeneratedScript_Header_ReservationStore2_(js);
            AppendRuntimeGeneratedScript_Header_ReservationStore3_(js);
            AppendRuntimeGeneratedScript_Header_EventKey_(js);
            AppendRuntimeGeneratedScript_Header_QueryReservation_(js);
            AppendRuntimeGeneratedScript_Header_StyleReservation_(js);
            AppendRuntimeGeneratedScript_Header_StyleReservation2_(js);
            AppendRuntimeGeneratedScript_Header_Actions_(js);
        }

        void AppendRuntimeGeneratedScript_FrameAndCache_1_(std::wstring& js)
        {
            js += LR"JS(
function getConfigServiceDataMap_(){
  try {
    if (window.CONFIG_SERVICE_DATA &&
        typeof window.CONFIG_SERVICE_DATA === 'object') {
      return window.CONFIG_SERVICE_DATA;
    }
  } catch (_) {
  }

  try {
    if (typeof CONFIG_SERVICE_DATA !== 'undefined' &&
        CONFIG_SERVICE_DATA) {
      return CONFIG_SERVICE_DATA;
    }
  } catch (_) {
  }

  return {};
}

function makeFallbackSpaceInfo_(space){
  const n = Number(space);
  return {
    label:
      (n === 0) ? '地上波' :
      (n === 1) ? 'BS' :
      (n === 2) ? 'CS' : ('Space ' + String(n)),
    currentSrc: 'epg_space' + String(n) + '.html',
    beforeSrc: 'epg_space' + String(n) + '_before.html',
    hasCurrent: true,
    hasBefore: true
  };
}

function getSpaceInfo_(space){
  const fb = makeFallbackSpaceInfo_(space);

  try {
    if (typeof SPACE_DATA !== 'undefined' && SPACE_DATA && SPACE_DATA[String(space)]) {
      const raw = SPACE_DATA[String(space)] || {};
      return {
        label: raw.label || fb.label,
        currentSrc: raw.currentSrc || fb.currentSrc,
        beforeSrc: raw.beforeSrc || fb.beforeSrc,
        hasCurrent: !!(raw.currentSrc || fb.currentSrc),
        hasBefore: !!(raw.beforeSrc || fb.beforeSrc)
      };
    }
  } catch (_) {
  }

  return fb;
}

function bindFrame(src, exists, frameId, emptyId, linkId, emptyMessage){
  const frame = document.getElementById(frameId);
  const empty = document.getElementById(emptyId);
  const link = document.getElementById(linkId);
  if (!frame || !empty || !link) return;

  const nextSrc = String(src || '');
  const prevSrc = frame.getAttribute('data-bound-src') || '';

  if (nextSrc) {
    if (prevSrc !== nextSrc) {
      frame.src = nextSrc;
      frame.setAttribute('data-bound-src', nextSrc);
    }
    frame.style.display = 'block';
    empty.textContent = '';
    empty.classList.remove('open');
    link.href = nextSrc;
    link.style.pointerEvents = 'auto';
    link.style.opacity = '1';
  } else {
    if (prevSrc !== '') {
      frame.src = 'about:blank';
      frame.setAttribute('data-bound-src', '');
    }
    frame.style.display = 'none';
    empty.textContent = emptyMessage;
    empty.classList.add('open');
    link.href = '#';
    link.style.pointerEvents = 'none';
    link.style.opacity = '0.45';
  }
}

function getFrameDocument_(frameId){
  const frame = document.getElementById(frameId);
  if (!frame) return null;

  try {
    return frame.contentDocument || (frame.contentWindow ? frame.contentWindow.document : null);
  } catch (_) {
    return null;
  }
}
)JS";
        }

        void AppendRuntimeGeneratedScript_FrameAndCache_2_(std::wstring& js)
        {
            js += LR"JS(
function inferServiceNameFromCell_(cell){
  try {
    const table = cell.closest('table');
    const headRow = table ? table.querySelector('thead tr') : null;
    if (!headRow) return '';

    const idx = cell.cellIndex;
    if (!Number.isFinite(idx) || idx <= 0) return '';

    const headers = headRow.children;
    if (idx >= headers.length) return '';

    return normalizeText(headers[idx].textContent || '');
  } catch (_) {
    return '';
  }
}

function inferDateFromCell_(cell){
  try {
    const sec = cell.closest('section[data-day]');
    if (sec && sec.dataset && sec.dataset.day) {
      return normalizeText(sec.dataset.day);
    }
  } catch (_) {
  }
  return '';
}

function collectServiceNamesFromFrame_(frameId){
  const doc = getFrameDocument_(frameId);
  if (!doc) return [];

  const out = [];
  const seen = new Set();

  const add = (name) => {
    const v = normalizeText(name || '');
    const key = normalizeServiceKey(v);
    if (!v || seen.has(key)) return;
    seen.add(key);
    out.push(v);
  };

  doc.querySelectorAll('thead tr th').forEach((th, idx) => {
    if (idx === 0) return;
    add(th.textContent || '');
  });

  doc.querySelectorAll('[data-service],[data-service-name]').forEach(el => {
    add(el.getAttribute('data-service') || el.getAttribute('data-service-name') || '');
  });

  return out;
}

function collectProgramsFromFrame_(frameId, pageKind){
  const doc = getFrameDocument_(frameId);
  if (!doc) return [];

  const cells = Array.from(
    doc.querySelectorAll('[data-epg-program="1"], .prog-cell[data-title], td[data-title], td[data-event-id]')
  );

  const out = [];

  cells.forEach((cell, idx) => {
    const title =
      normalizeText(cell.getAttribute('data-title') || cell.getAttribute('title') || '');
    const time =
      normalizeText(cell.getAttribute('data-time') || '');
    const summary =
      normalizeText(cell.getAttribute('data-summary') || '');
    const genre =
      normalizeText(cell.getAttribute('data-genre') || '');
    const service =
      normalizeText(
        cell.getAttribute('data-service') ||
        cell.getAttribute('data-service-name') ||
        inferServiceNameFromCell_(cell)
      );
    const dateText =
      normalizeText(cell.getAttribute('data-date') || inferDateFromCell_(cell));
    const weekdayText =
      normalizeText(cell.getAttribute('data-weekday') || '');
    const eventId =
      Number(cell.getAttribute('data-event-id') || 0);
    const serviceId =
      Number(cell.getAttribute('data-service-id') || 0);

    if (!title && eventId <= 0) {
      return;
    }

    out.push({
      id: 'frame-' + frameId + '-' + idx,
      page: pageKind || 'now',
      dateText: dateText,
      weekdayText: weekdayText,
      time: time,
      title: title,
      titleSearch: normalizeSearchText(title),
      summary: summary,
      summarySearch: normalizeSearchText(summary),
      fullTextSearch: normalizeSearchText(title + '\n' + summary),
      service: service || '未判定',
      serviceKey: normalizeServiceKey(service || '未判定'),
      serviceSearch: normalizeSearchText(service || ''),
      genre: genre || '--',
      serviceId: serviceId,
      eventId: eventId
    });
  });

  return out;
}
)JS";
        }

        void AppendRuntimeGeneratedScript_FrameAndCache_3_(std::wstring& js)
        {
            js += LR"JS(
function mapSearchDataItem_(item, idx, pageKind, spaceValue){
  const serviceName = item.service || item.serviceName || '未判定';

  return {
    id: 'epg-' + String(spaceValue) + '-' + String(pageKind) + '-' + String(idx),
    space: Number(spaceValue),
    page: pageKind,
    dateText: item.date || item.dateText || '',
    weekdayText: item.weekday || item.weekdayText || '',
    time: item.time || item.timeText || '',
    title: item.title || '',
    titleSearch: normalizeSearchText(item.title || ''),
    summary: item.summary || '',
    summarySearch: normalizeSearchText(item.summary || ''),
    fullTextSearch: normalizeSearchText((item.title || '') + '\n' + (item.summary || '')),
    service: serviceName,
    serviceKey: normalizeServiceKey(serviceName),
    serviceSearch: normalizeSearchText(serviceName),
    genre: item.genre || '--',
    serviceId: Number(item.serviceId || 0),
    eventId: Number(item.eventId || 0)
  };
}

function rebuildProgramCache(force){
  const key = String(gCurrentSpace);
  const cached = (!force && gProgramCacheBySpace[key]) ? gProgramCacheBySpace[key] : null;

  if (cached) {
    gProgramCache = cached;
  } else {
    const nowRaw =
      (window.EPG_SEARCH_DATA && Array.isArray(window.EPG_SEARCH_DATA[key]))
        ? window.EPG_SEARCH_DATA[key]
        : [];

    const pastRaw =
      (window.EPG_SEARCH_DATA_PAST && Array.isArray(window.EPG_SEARCH_DATA_PAST[key]))
        ? window.EPG_SEARCH_DATA_PAST[key]
        : [];

    const spaceNum = Number(gCurrentSpace);

    let nowList = nowRaw.map((item, idx) =>
      mapSearchDataItem_(item, idx, 'now', spaceNum)
    );

    let pastList = pastRaw.map((item, idx) =>
      mapSearchDataItem_(item, idx, 'past', spaceNum)
    );

    if (nowList.length === 0) {
      nowList = collectProgramsFromFrame_('currentFrame', 'now').map((item, idx) => {
        item.id = item.id || ('frame-current-' + idx);
        item.space = spaceNum;
        item.page = 'now';
        return item;
      });
    }

    if (pastList.length === 0) {
      pastList = collectProgramsFromFrame_('beforeFrame', 'past').map((item, idx) => {
        item.id = item.id || ('frame-before-' + idx);
        item.space = spaceNum;
        item.page = 'past';
        return item;
      });
    }

    gProgramCache = {
      now: nowList,
      past: pastList
    };

    gProgramCacheBySpace[key] = gProgramCache;
  }

  const detailLastUpdate = document.getElementById('detailLastUpdate');
  if (detailLastUpdate) {
    detailLastUpdate.textContent =
      '検索元 現在・本日以降 ' + gProgramCache.now.length +
      ' 件 / 過去 ' + gProgramCache.past.length + ' 件を読込済み';
  }
}
)JS";
        }

        void AppendRuntimeGeneratedScript_FrameAndCache_4_(std::wstring& js)
        {
            js += LR"JS(
function refreshServiceList(){
  const select = document.getElementById('serviceList');
  if (!select) return;

  const selected = new Set(
    Array.from(select.selectedOptions || []).map(o => String(o.value || ''))
  );

  const cfgMap = getConfigServiceDataMap_();

  const groups = [
    { space: 0, label: '地上波', items: [] },
    { space: 1, label: 'BS',     items: [] },
    { space: 2, label: 'CS',     items: [] }
  ];

  groups.forEach(group => {
    const cfg = cfgMap[String(group.space)];
    const seen = new Set();

    if (!Array.isArray(cfg)) {
      return;
    }

    cfg.forEach(name => {
      const display = normalizeText(name || '');
      const key = normalizeServiceKey(display);
      if (!display || !key || seen.has(key)) {
        return;
      }

      seen.add(key);

      group.items.push({
        value: String(group.space) + '|' + key,
        oldValue: key,
        display: display
      });
    });

    group.items.sort((a, b) => {
      if (a.display < b.display) return -1;
      if (a.display > b.display) return 1;
      return 0;
    });
  });

  select.innerHTML = '';

  let appended = 0;

  groups.forEach(group => {
    if (!group.items.length) {
      return;
    }

    const optGroup = document.createElement('optgroup');
    optGroup.label = group.label;

    group.items.forEach(entry => {
      const opt = document.createElement('option');
      opt.value = entry.value;
      opt.textContent = entry.display;

      if (selected.has(entry.value) || selected.has(entry.oldValue)) {
        opt.selected = true;
      }

      optGroup.appendChild(opt);
      appended += 1;
    });

    select.appendChild(optGroup);
  });

  if (appended === 0) {
    const fallback = [];
    const seen = new Set();

    const addFallback = (name) => {
      const display = normalizeText(name || '');
      const key = normalizeServiceKey(display);
      if (!display || !key || seen.has(key)) {
        return;
      }

      seen.add(key);
      fallback.push({
        value: String(gCurrentSpace) + '|' + key,
        oldValue: key,
        display: display
      });
    };

    collectServiceNamesFromFrame_('currentFrame').forEach(addFallback);
    collectServiceNamesFromFrame_('beforeFrame').forEach(addFallback);

    if (fallback.length > 0) {
      const optGroup = document.createElement('optgroup');
      optGroup.label =
        Number(gCurrentSpace) === 0 ? '地上波' :
        Number(gCurrentSpace) === 1 ? 'BS' :
        Number(gCurrentSpace) === 2 ? 'CS' :
        'Space ' + String(gCurrentSpace);

      fallback.forEach(entry => {
        const opt = document.createElement('option');
        opt.value = entry.value;
        opt.textContent = entry.display;

        if (selected.has(entry.value) || selected.has(entry.oldValue)) {
          opt.selected = true;
        }

        optGroup.appendChild(opt);
      });

      select.appendChild(optGroup);
      appended = fallback.length;
    }
  }

  if (appended === 0) {
    const opt = document.createElement('option');
    opt.value = '';
    opt.textContent = '放送局候補なし';
    opt.selected = true;
    select.appendChild(opt);
  }
}
)JS";
        }

        void AppendRuntimeGeneratedScript_FrameAndCache_5_(std::wstring& js)
        {
            js += LR"JS(
function cloneReservationListForFrameSync_(list){
  if (!Array.isArray(list)) {
    return [];
  }

  return list
    .filter(item => item && item.mode !== 'epg')
    .map(item => Object.assign({}, item));
}

function getReservationListForFrameSync_(){
  if (typeof getMergedReservationList === 'function') {
    return cloneReservationListForFrameSync_(getMergedReservationList());
  }

  if (Array.isArray(gReservationList)) {
    return cloneReservationListForFrameSync_(gReservationList);
  }

  if (Array.isArray(window.EPG_RESERVATION_DATA)) {
    return cloneReservationListForFrameSync_(window.EPG_RESERVATION_DATA);
  }

  return [];
}

function syncReservationListToFrame_(frameId){
  const frame = document.getElementById(frameId);
  if (!frame) return false;

  try {
    const win = frame.contentWindow;
    if (!win) return false;

    const list = getReservationListForFrameSync_();

    if (typeof win.__epgPortalSetReservationList === 'function') {
      win.__epgPortalSetReservationList(list, gReservationRevision);
    } else {
      win.gReservationList = list.slice();

      if (typeof win.invalidateReservationStateCache === 'function') {
        win.invalidateReservationStateCache();
      }
    }

    return true;
  } catch (_) {
    return false;
  }
}

function refreshFrameReservationButtons(){
  ['currentFrame', 'beforeFrame'].forEach(frameId => {
    const frame = document.getElementById(frameId);
    if (!frame) return;

    try {
      const win = frame.contentWindow;
      if (!win) return;

      syncReservationListToFrame_(frameId);

      if (typeof win.__epgPortalProgramTableRenderAll === 'function') {
        win.__epgPortalProgramTableRenderAll();
      }
    } catch (_) {
    }
  });
}
)JS";
        }

        void AppendRuntimeGeneratedScript_FrameAndCache_6_(std::wstring& js)
        {
            js += LR"JS(
function registerFrameReservationHooks(){
  ['currentFrame', 'beforeFrame'].forEach(frameId => {
    const frame = document.getElementById(frameId);
    if (!frame || frame.getAttribute('data-epg-hooked') === '1') return;

    frame.addEventListener('load', () => {
      try {
        const win = frame.contentWindow;
        if (win) {
          syncReservationListToFrame_(frameId);

          if (typeof win.__epgPortalProgramTableRenderAll === 'function') {
            win.__epgPortalProgramTableRenderAll();
          }
        }
      } catch (_) {
      }

      gProgramCacheBySpace[String(gCurrentSpace)] = null;
      rebuildProgramCache(true);
      refreshServiceList();
    });

    frame.setAttribute('data-epg-hooked', '1');
  });
}
)JS";
        }

        void AppendRuntimeGeneratedScript_FrameAndCache_(std::wstring& js)
        {
            AppendRuntimeGeneratedScript_FrameAndCache_1_(js);
            AppendRuntimeGeneratedScript_FrameAndCache_2_(js);
            AppendRuntimeGeneratedScript_FrameAndCache_3_(js);
            AppendRuntimeGeneratedScript_FrameAndCache_4_(js);
            AppendRuntimeGeneratedScript_FrameAndCache_5_(js);
            AppendRuntimeGeneratedScript_FrameAndCache_6_(js);
        }

        void AppendRuntimeGeneratedScript_Render_Reservation1_(std::wstring& js)
        {
            js += LR"JS(
function renderReservationPage(){
  const wrap = document.getElementById('reservationListWrap');
  const summary = document.getElementById('reservationSummary');
  const countEl = document.getElementById('detailReservationCount');

  const merged = getMergedReservationList();
  if (countEl) countEl.textContent = String(merged.length);
  if (!wrap || !summary) return;

  const items = merged
    .filter(item => item.space < 0 || item.space === gCurrentSpace)
    .slice()
    .sort((a, b) => {
      const ak = ((a.date || '') + ' ' + (a.time || '') + ' ' + (a.label || ''));
      const bk = ((b.date || '') + ' ' + (b.time || '') + ' ' + (b.label || ''));
      return ak.localeCompare(bk, 'ja');
    });

  const spaceInfo = getSpaceInfo_(gCurrentSpace);
  const spaceLabel = (spaceInfo && spaceInfo.label) ? spaceInfo.label : ('Space ' + gCurrentSpace);

  const epgCount = items.filter(item => item.mode === 'epg').length;
  const programCount = items.filter(item => item.mode !== 'epg').length;
  const enabledCount = items.filter(item => item.enabled).length;
  const disabledCount = items.length - enabledCount;

  summary.textContent = items.length > 0
    ? spaceLabel + ' の予約 ' + items.length + ' 件 / EPG取得 ' + epgCount + ' 件 / 番組予約 ' + programCount + ' 件 / 有効 ' + enabledCount + ' 件 / 無効 ' + disabledCount + ' 件'
    : spaceLabel + ' の予約はありません。';

  if (items.length === 0) {
    wrap.innerHTML = '<div class="placeholder" style="margin:12px;">まだ予約はありません。</div>';
    return;
  }

  finishRenderReservationPage(items);
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Render_Reservation2_(std::wstring& js)
        {
            js += LR"JS(
function buildReservationRows(items){
  return items.map((item, index) => {
    const modeClass = item.enabled ? (item.mode || 'reserve') : 'disabled';

    const modeLabel = !item.enabled
      ? (item.mode === 'record'
          ? '録画予約(無効)'
          : (item.mode === 'watch'
              ? '視聴予約(無効)'
              : (item.mode === 'epg' ? 'EPG取得予約(無効)' : '予約(無効)')))
      : (item.mode === 'record'
          ? '録画予約'
          : (item.mode === 'watch'
              ? '視聴予約'
              : (item.mode === 'epg' ? 'EPG取得予約' : '予約')));

    const tunerLabel = formatReservationTunerLabel(item);
    const serviceLabel = item.service || item.target || (item.mode === 'epg' ? 'EPG対象サービス' : '--');
    const dateLabel = item.date || '--';
    const timeLabel = item.time || '--';
    const noteLabel = item.note || item.schedule || '';
    const categoryLabel = item.category || '';
    const statusLabel = item.enabled ? '有効' : '無効';

    const idLiteral = JSON.stringify(String(item.id || ''));

const toggleCall =
  '(function(id){'
  + 'const fn=window.requestReservationListToggleEnabledById||window.toggleReservationEnabledById;'
  + 'if(typeof fn==="function"){return fn(id);}'
  + 'console.warn("[reservation-list] toggle handler missing", id);'
  + '})(' + idLiteral + ')';

const removeCall =
  '(function(id){'
  + 'const fn=window.requestReservationListRemoveById||window.removeReservationById;'
  + 'if(typeof fn==="function"){return fn(id);}'
  + 'console.warn("[reservation-list] remove handler missing", id);'
  + '})(' + idLiteral + ')';

const actionHtml = ''
  + '<div style="display:flex;gap:6px;flex-wrap:wrap;">'
  + buildActionButtonHtml(
      item.enabled ? '無効化' : '有効化',
      toggleCall,
      'disabled')
  + buildActionButtonHtml(
      '削除',
      removeCall,
      'danger')
  + '</div>';

    return ''
      + '<tr class="reservation-row reservation-row-' + escapeHtml(modeClass) + '" data-reservation-index="' + index + '">'
      + '<td><span class="reservation-tag ' + escapeHtml(modeClass) + '">' + escapeHtml(modeLabel) + '</span></td>'
      + '<td>' + escapeHtml(tunerLabel) + '</td>'
      + '<td>' + escapeHtml(dateLabel) + '</td>'
      + '<td>' + escapeHtml(timeLabel) + '</td>'
      + '<td><div class="result-title">' + escapeHtml(item.label || '予約項目') + '</div></td>'
      + '<td>' + escapeHtml(serviceLabel) + '</td>'
      + '<td>' + escapeHtml(noteLabel) + '</td>'
      + '<td>' + escapeHtml(categoryLabel) + '</td>'
      + '<td>' + escapeHtml(statusLabel) + '</td>'
      + '<td style="white-space:nowrap;">' + actionHtml + '</td>'
      + '</tr>';
  }).join('');
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Render_Reservation3_(std::wstring& js)
        {
            js += LR"JS(
function finishRenderReservationPage(items){
  const wrap = document.getElementById('reservationListWrap');
  if (!wrap) return;

  const rows = buildReservationRows(items);

  wrap.innerHTML =
    '<table class="result-table reservation-table">'
    + '<thead><tr>'
    + '<th>種別</th>'
    + '<th>チューナー</th>'
    + '<th>日付</th>'
    + '<th>時刻</th>'
    + '<th>タイトル</th>'
    + '<th>対象</th>'
    + '<th>備考</th>'
    + '<th>分類</th>'
    + '<th>状態</th>'
    + '<th>操作</th>'
    + '</tr></thead>'
    + '<tbody>' + rows + '</tbody>'
    + '</table>';
}

function removeReservationById(reservationId){
  const id = String(reservationId || '');
  if (!id) return Promise.resolve({ ok:false, reason:'empty-id' });

  if (!window.__epgReservationStore) {
    setText('detailLastUpdate', '予約Storeが未初期化です。');
    return Promise.resolve({ ok:false, reason:'reservation-store-missing' });
  }

  setText('detailLastUpdate', '予約を削除中...');

  return window.__epgReservationStore
    .removeReservationById(id)
    .then(result => {
      if (!result || result.ok !== true) {
        const reason = result && result.reason ? result.reason : 'reservation-remove-failed';
        setText('detailLastUpdate', '予約削除に失敗: ' + reason);
      } else {
        setText('detailLastUpdate', '予約を削除しました。');
      }

      refreshReservationVisuals();
      return result;
    });
}

function toggleReservationEnabledById(reservationId){
  const id = String(reservationId || '');
  if (!id) return Promise.resolve({ ok:false, reason:'empty-id' });

  if (!window.__epgReservationStore) {
    setText('detailLastUpdate', '予約Storeが未初期化です。');
    return Promise.resolve({ ok:false, reason:'reservation-store-missing' });
  }

  setText('detailLastUpdate', '予約状態を保存中...');

  return window.__epgReservationStore
    .toggleReservationEnabledById(id)
    .then(result => {
      if (!result || result.ok !== true) {
        const reason = result && result.reason ? result.reason : 'reservation-toggle-failed';
        setText('detailLastUpdate', '予約状態の保存に失敗: ' + reason);
      } else {
        setText('detailLastUpdate', '予約状態を保存しました。');
      }

      refreshReservationVisuals();
      return result;
    });
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Render_Search1_(std::wstring& js)
        {
            js += LR"JS(
function renderSearchResults(results){
  const wrap = document.getElementById('searchResultsWrap');
  if (!wrap) return;

  if (!results || results.length === 0) {
    wrap.innerHTML = '<div class="placeholder" style="margin:12px;">条件に一致する番組はありませんでした。</div>';
    setText('detailSearchCount', '0');
    return;
  }

  finishRenderSearchResults(results);
}

function makeSearchActionHtml(item, index, state){
  if (state.reserved) {
    const buttons = [];

    if (state.mode === 'record' || state.mode === 'record-disabled') {
      buttons.push(
        buildActionButtonHtml(
          state.mode === 'record' ? '録画無効化' : '録画有効化',
          'event.stopPropagation();toggleRecordReservationDisabled(' + index + ')',
          'disabled'));
    } else if (state.mode === 'watch' || state.mode === 'watch-disabled') {
      buttons.push(
        buildActionButtonHtml(
          state.mode === 'watch' ? '視聴無効化' : '視聴有効化',
          'event.stopPropagation();toggleWatchReservationDisabled(' + index + ')',
          'disabled'));
    }

    buttons.push(
      buildActionButtonHtml(
        '取消',
        'event.stopPropagation();requestProgramCancelByIndex(' + index + ')',
        'danger'));

    return '<div style="display:flex;gap:6px;flex-wrap:wrap;justify-content:flex-start;">'
      + buttons.join('')
      + '</div>';
  }

  if (EPG_RESERVE_BUTTON_MODE === 'record') {
    return buildActionButtonHtml(
      '録画予約',
      'event.stopPropagation();requestProgramReserveByIndexWithMode(' + index + ', "record")',
      'record');
  }

  if (EPG_RESERVE_BUTTON_MODE === 'watch') {
    return buildActionButtonHtml(
      '視聴予約',
      'event.stopPropagation();requestProgramReserveByIndexWithMode(' + index + ', "watch")',
      'watch');
  }

  return ''
    + '<div style="display:flex;gap:6px;flex-wrap:wrap;justify-content:flex-start;">'
    + buildActionButtonHtml(
        '録画予約',
        'event.stopPropagation();requestProgramReserveByIndexWithMode(' + index + ', "record")',
        'record')
    + buildActionButtonHtml(
        '視聴予約',
        'event.stopPropagation();requestProgramReserveByIndexWithMode(' + index + ', "watch")',
        'watch')
    + '</div>';
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Render_Search2_(std::wstring& js)
        {
            js += LR"JS(
function buildSearchResultRows(results){
  return results.map((item, index) => {
    const state = getProgramReservationState(item);
    const rowStyle = getProgramRowInlineStyle(state);
    const titleTags = getProgramTagsHtml(state);
    const actionHtml = makeSearchActionHtml(item, index, state);

    return ''
      + '<tr class="result-row result-row-' + escapeHtml(state.mode || 'none') + '" '
      + 'data-result-index="' + index + '"'
      + (rowStyle ? ' style="' + rowStyle + '"' : '')
      + '>'
      + '<td>' + escapeHtml(item.dateText || '--') + '</td>'
      + '<td>' + escapeHtml(item.weekdayText || '--') + '</td>'
      + '<td>' + escapeHtml(item.time || '--') + '</td>'
      + '<td><div class="result-title">' + escapeHtml(item.title || '--') + titleTags + '</div></td>'
      + '<td><div class="result-summary">' + escapeHtml(item.summary || '') + '</div></td>'
      + '<td>' + escapeHtml(item.service || '--') + '</td>'
      + '<td>' + escapeHtml(item.genre || '--') + '</td>'
      + '<td style="white-space:nowrap;">' + actionHtml + '</td>'
      + '</tr>';
  }).join('');
}

function finishRenderSearchResults(results){
  const wrap = document.getElementById('searchResultsWrap');
  if (!wrap) return;

  const rows = buildSearchResultRows(results);

  wrap.innerHTML =
    '<table class="result-table result-table-list">'
    + '<thead><tr>'
    + '<th>日付</th><th>曜日</th><th>時刻</th><th>タイトル</th><th>内容</th><th>放送局</th><th>ジャンル</th><th>操作</th>'
    + '</tr></thead>'
    + '<tbody>' + rows + '</tbody>'
    + '</table>';

  wrap.querySelectorAll('.result-row').forEach(row => {
    row.addEventListener('click', () => {
      const index = Number(row.getAttribute('data-result-index'));
      if (!Number.isFinite(index) || !gLastResults[index]) return;
      wrap.querySelectorAll('.result-row').forEach(r => r.classList.remove('active'));
      row.classList.add('active');
      showProgramDetail(gLastResults[index]);
    });
  });

  setText('detailSearchCount', String(results.length));
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Render_Detail_(std::wstring& js)
        {
            js += LR"JS(
function updateSearchSummary(count, keyword, requestedTarget, effectiveTarget){
  const summary = document.getElementById('searchSummary');
  const statusBox = document.getElementById('searchStatusBox');

  const labelOf = (target) => target === 'past' ? '過去' : (target === 'all' ? '全体' : '現在・今後');

  let msg = '一覧表示 / 対象: ' + labelOf(effectiveTarget) + ' / 件数: ' + count + ' / 検索語: ' + (keyword || 'なし');
  if (requestedTarget !== effectiveTarget) {
    msg += ' / 過去検索データ未生成のため ' + labelOf(effectiveTarget) + ' にフォールバック';
  }

  if (summary) summary.textContent = msg;
  if (statusBox) statusBox.textContent = msg;
}

function showProgramDetail(item){
  if (!item) return;
  gSelectedProgram = item;

  const state = getProgramReservationState(item);

  setText('detailTitle', item.title || '--');
  setText('detailDate', item.dateText || '--');
  setText('detailWeekday', item.weekdayText || '--');
  setText('detailTime', item.time || '--');
  setText('detailService', item.service || '--');
  setText('detailGenre', item.genre || '--');
  setText('detailSummary', item.summary || '詳細なし');
  setText('detailLastUpdate', '選択中: ' + (item.title || '--'));

  if (state.reserved) {
    setText(
      'detailReservationState',
      '予約状態: ' + state.modeLabel + (state.tunerLabels.length > 0 ? ' / ' + state.tunerLabels.join(', ') : '')
    );
  } else {
    setText('detailReservationState', '予約状態: 未予約');
  }

  if (typeof updateDetailReservationPanel === 'function') {
    updateDetailReservationPanel(item);
  }
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Render_(std::wstring& js)
        {
            AppendRuntimeGeneratedScript_Render_Reservation1_(js);
            AppendRuntimeGeneratedScript_Render_Reservation2_(js);
            AppendRuntimeGeneratedScript_Render_Reservation3_(js);
            AppendRuntimeGeneratedScript_Render_Search1_(js);
            AppendRuntimeGeneratedScript_Render_Search2_(js);
            AppendRuntimeGeneratedScript_Render_Detail_(js);
        }

        void AppendRuntimeGeneratedScript_Action_(std::wstring& js)
        {
            js += LR"JS(
function isReservationPageVisible_(){
  const page = document.getElementById('page-reservation');
  if (!page) {
    return gCurrentPage === 'reservation';
  }

  if (page.classList && page.classList.contains('active')) {
    return true;
  }

  return gCurrentPage === 'reservation';
}

function scheduleSpaceRefresh(force){
  const token = ++gSwitchRefreshToken;

  const runner = function(){
    if (token !== gSwitchRefreshToken) return;

    rebuildProgramCache(!!force);
    refreshServiceList();
    syncDetailTargetFromCurrentPage();

    // 予約一覧ページが表示中の時だけ予約一覧DOMを再構築する。
    // 初期表示や番組表閲覧中に毎回予約一覧テーブルを作ると重くなる。
    if (isReservationPageVisible_()) {
      renderReservationPage();
    }
  };

  if (typeof window.requestAnimationFrame === 'function') {
    window.requestAnimationFrame(runner);
  } else {
    window.setTimeout(runner, 0);
  }
}

function switchPage(name, btn){
  const allowed = new Set(['now', 'past', 'search', 'reservation']);
  const next = allowed.has(String(name || '')) ? String(name) : 'now';

  gCurrentPage = next;
  if (next === 'now' || next === 'past') {
    gLastBrowsePage = next;
  }

  document.querySelectorAll('.page-section').forEach(sec => {
    sec.classList.remove('active');
    sec.setAttribute('aria-hidden', 'true');
  });

  const target = document.getElementById('page-' + next);
  if (target) {
    target.classList.add('active');
    target.setAttribute('aria-hidden', 'false');
  }

  const radioIdMap = {
    now: 'pageTabNow',
    past: 'pageTabPast',
    search: 'pageTabSearch',
    reservation: 'pageTabReservation'
  };

  const radio = document.getElementById(radioIdMap[next]);
  if (radio) {
    radio.checked = true;
  }

  document.querySelectorAll('.menu [data-page]').forEach(el => {
    el.classList.remove('active');
    el.setAttribute('aria-pressed', 'false');
  });

  if (btn && btn.setAttribute) {
    btn.classList.add('active');
    btn.setAttribute('aria-pressed', 'true');
  } else {
    const autoBtn = document.querySelector('.menu [data-page="' + next + '"]');
    if (autoBtn) {
      autoBtn.classList.add('active');
      autoBtn.setAttribute('aria-pressed', 'true');
    }
  }

  if (next === 'reservation') {
    renderReservationPage();
  }

  syncDetailTargetFromCurrentPage();
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Footer_Page_(std::wstring& js)
        {
            js += LR"JS(
function getActiveSearchTarget(){
  if (gCurrentPage === 'past') return 'past';
  if (gCurrentPage === 'now') return 'now';
  return gLastBrowsePage === 'past' ? 'past' : 'now';
}

function syncDetailTargetFromCurrentPage(){
  const detailTarget = document.getElementById('detailTarget');
  if (detailTarget) {
    detailTarget.value = getActiveSearchTarget();
  }
}

function syncPageStateFromDom(){
  const activeSection = document.querySelector('.page-section.active');
  if (activeSection && activeSection.id) {
    const name = activeSection.id.replace(/^page-/, '');
    if (name) {
      gCurrentPage = name;
      if (name === 'now' || name === 'past') {
        gLastBrowsePage = name;
      }
      return;
    }
  }

  const pageTabNow = document.getElementById('pageTabNow');
  const pageTabPast = document.getElementById('pageTabPast');
  const pageTabSearch = document.getElementById('pageTabSearch');
  const pageTabReservation = document.getElementById('pageTabReservation');

  const checkedRadio =
    (pageTabNow && pageTabNow.checked) ? 'now' :
    (pageTabPast && pageTabPast.checked) ? 'past' :
    (pageTabSearch && pageTabSearch.checked) ? 'search' :
    (pageTabReservation && pageTabReservation.checked) ? 'reservation' :
    '';

  if (checkedRadio) {
    gCurrentPage = checkedRadio;
    if (checkedRadio === 'now' || checkedRadio === 'past') {
      gLastBrowsePage = checkedRadio;
    }
  }
}

function runQuickSearch(){
  const quick = document.getElementById('quickKeyword');
  const detail = document.getElementById('detailKeyword');
  if (detail && quick) {
    detail.value = quick.value || '';
  }
  syncDetailTargetFromCurrentPage();
  runDetailSearch();
}

function openDetailSearch(){
  const ov = document.getElementById('detailSearchOverlay');
  if (!ov) return;

  ov.classList.add('open');
  ov.setAttribute('aria-hidden', 'false');

  ensureSearchScopeControls_();

  const quick = document.getElementById('quickKeyword');
  const detail = document.getElementById('detailKeyword');
  if (quick && detail && quick.value && !detail.value) {
    detail.value = quick.value;
  }

  syncDetailTargetFromCurrentPage();
  rebuildProgramCache(true);
  refreshServiceList();
}

function closeDetailSearch(){
  const ov = document.getElementById('detailSearchOverlay');
  if (!ov) return;

  ov.classList.remove('open');
  ov.setAttribute('aria-hidden', 'true');
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Footer_Page2_(std::wstring& js)
        {
            js += LR"JS(
function toggleMainSwitchTableFocus(){
  const layout = document.getElementById('mainLayout');
  const btn = document.getElementById('toggleMainSwitchTableFocusBtn');
  if (!layout || !btn) return;

  const expanded = layout.classList.toggle('table-focus-mode');
  btn.textContent = expanded ? '通常表示に戻す' : '番組表を拡大';
  btn.classList.toggle('toggle-focus-active', expanded);
}

function applyQuickView(){
  const mode = document.getElementById('displayMode');
  if (!mode) return;

  if (mode.value === 'past') switchPage('past');
  else switchPage('now');
}

function switchSpace(space, btn){
  const nextSpace = Number(space);
  const info = getSpaceInfo_(nextSpace);

  const changed = (gCurrentSpace !== nextSpace);
  gCurrentSpace = nextSpace;

  document.querySelectorAll('[data-space]').forEach(b => b.classList.remove('active-space'));
  if (btn) {
    btn.classList.add('active-space');
  } else {
    const autoBtn = document.querySelector('[data-space="' + nextSpace + '"]');
    if (autoBtn) autoBtn.classList.add('active-space');
  }

  setText('spaceStatusLabel', info.label + ' / Space ' + nextSpace);

  bindFrame(
    info.currentSrc,
    info.hasCurrent,
    'currentFrame',
    'currentEmpty',
    'currentStandaloneLink',
    info.label + ' の番組表はまだ生成されていません。'
  );

  bindFrame(
    info.beforeSrc,
    info.hasBefore,
    'beforeFrame',
    'beforeEmpty',
    'beforeStandaloneLink',
    info.label + ' の過去番組表はまだ生成されていません。'
  );

  scheduleSpaceRefresh(changed);
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Footer_Search1_(std::wstring& js)
        {
            js += LR"JS(
function parseServiceFilterValue_(value){
  const text = String(value || '');
  const pos = text.indexOf('|');

  if (pos < 0) {
    return {
      space: null,
      serviceKey: normalizeServiceKey(text)
    };
  }

  const sp = Number(text.slice(0, pos));
  const servicePart = text.slice(pos + 1);

  return {
    space: Number.isFinite(sp) ? Math.trunc(sp) : null,
    serviceKey: normalizeServiceKey(servicePart)
  };
}

function getSelectedServiceFilters_(){
  return getSelectedValues('serviceList')
    .map(parseServiceFilterValue_)
    .filter(v => v && v.serviceKey);
}

function matchesServiceFilters_(item, filters){
  if (!Array.isArray(filters) || filters.length === 0) {
    return true;
  }

  const itemSpace =
    Number.isFinite(Number(item && item.space))
      ? Math.trunc(Number(item.space))
      : Number(gCurrentSpace);

  const itemServiceKey =
    item && item.serviceKey
      ? normalizeServiceKey(item.serviceKey)
      : normalizeServiceKey(item && item.service ? item.service : '');

  if (!itemServiceKey) {
    return false;
  }

  return filters.some(filter => {
    if (!filter || !filter.serviceKey) {
      return false;
    }

    if (filter.space !== null && filter.space !== itemSpace) {
      return false;
    }

    return filter.serviceKey === itemServiceKey;
  });
}

function makeKeywordTokens_(keyword){
  const raw = normalizeText(keyword || '');
  if (!raw) return [];

  const parts = raw
    .split(/[\s\u3000]+/g)
    .map(v => normalizeSearchText(v))
    .filter(v => v.length > 0);

  if (parts.length > 0) {
    return Array.from(new Set(parts));
  }

  const single = normalizeSearchText(raw);
  return single ? [single] : [];
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Footer_Search2_(std::wstring& js)
        {
            js += LR"JS(
function ensureSearchScopeControls_(){
  if (document.getElementById('detailSearchIncludeSummary')) {
    return;
  }

  const keyword = document.getElementById('detailKeyword');
  if (!keyword) {
    return;
  }

  const wrap = document.createElement('div');
  wrap.id = 'detailSearchScopeWrap';
  wrap.style.marginTop = '8px';
  wrap.style.display = 'flex';
  wrap.style.alignItems = 'center';
  wrap.style.gap = '10px';
  wrap.style.flexWrap = 'wrap';
  wrap.style.fontSize = '13px';

  const titleLabel = document.createElement('span');
  titleLabel.textContent = '検索対象: タイトル';

  const label = document.createElement('label');
  label.style.display = 'inline-flex';
  label.style.alignItems = 'center';
  label.style.gap = '6px';
  label.style.cursor = 'pointer';

  const check = document.createElement('input');
  check.type = 'checkbox';
  check.id = 'detailSearchIncludeSummary';
  check.checked = false;

  const text = document.createElement('span');
  text.textContent = '概要も検索する';

  label.appendChild(check);
  label.appendChild(text);

  const note = document.createElement('span');
  note.id = 'detailSearchScopeNote';
  note.textContent = '通常は番組名検索を優先します。';
  note.style.opacity = '0.75';

  wrap.appendChild(titleLabel);
  wrap.appendChild(label);
  wrap.appendChild(note);

  const parent = keyword.parentElement;
  if (parent) {
    parent.appendChild(wrap);
  }
}

function isSummarySearchEnabled_(){
  const check = document.getElementById('detailSearchIncludeSummary');
  return !!(check && check.checked);
}

function getKeywordHitInfo_(item, keywordTokens, includeSummary){
  if (!Array.isArray(keywordTokens) || keywordTokens.length === 0) {
    return {
      ok: true,
      field: 'none'
    };
  }

  const titleText = item && item.titleSearch
    ? String(item.titleSearch)
    : normalizeSearchText(item && item.title ? item.title : '');

  const titleAll = keywordTokens.every(token => titleText.includes(token));
  if (titleAll) {
    return {
      ok: true,
      field: 'title'
    };
  }

  if (!includeSummary) {
    return {
      ok: false,
      field: ''
    };
  }

  const summaryText = item && item.summarySearch
    ? String(item.summarySearch)
    : normalizeSearchText(item && item.summary ? item.summary : '');

  const summaryAll = keywordTokens.every(token => summaryText.includes(token));
  if (summaryAll) {
    return {
      ok: true,
      field: 'summary'
    };
  }

  return {
    ok: false,
    field: ''
  };
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Footer_Search3_(std::wstring& js)
        {
            js += LR"JS(
function runDetailSearch(){
  ensureSearchScopeControls_();
  rebuildProgramCache(true);

  const keyword = normalizeText(
    (document.getElementById('detailKeyword') && document.getElementById('detailKeyword').value) ||
    (document.getElementById('quickKeyword') && document.getElementById('quickKeyword').value) ||
    ''
  );

  const targetSel = document.getElementById('detailTarget');
  const requestedTarget = targetSel ? targetSel.value : 'now';
  const effectiveTarget = resolveEffectiveSearchTarget(requestedTarget);

  const dateFrom = (document.getElementById('detailDateFrom') && document.getElementById('detailDateFrom').value) || '';
  const dateTo   = (document.getElementById('detailDateTo')   && document.getElementById('detailDateTo').value)   || '';

  const selectedServiceFilters = getSelectedServiceFilters_();
  const selectedGenres = new Set(getSelectedValues('genreList'));
  const keywordTokens = makeKeywordTokens_(keyword);
  const includeSummary = isSummarySearchEnabled_();

  let source = [];
  if (effectiveTarget === 'now' || effectiveTarget === 'all') {
    source = source.concat(gProgramCache.now);
  }
  if (effectiveTarget === 'past' || effectiveTarget === 'all') {
    source = source.concat(gProgramCache.past);
  }

  const results = source.filter(item => {
    const hitInfo = getKeywordHitInfo_(item, keywordTokens, includeSummary);
    if (!hitInfo.ok) {
      return false;
    }

    item.__searchHitField = hitInfo.field;

    if (!matchesServiceFilters_(item, selectedServiceFilters)) {
      return false;
    }

    if (selectedGenres.size > 0 && !selectedGenres.has(item.genre)) {
      return false;
    }

    if (dateFrom && item.dateText && item.dateText < dateFrom) {
      return false;
    }

    if (dateTo && item.dateText && item.dateText > dateTo) {
      return false;
    }

    return true;
  });

  gLastResults = results;
  renderSearchResults(results);

  const scopeLabel = includeSummary ? 'タイトル+概要' : 'タイトル';
  updateSearchSummary(
    results.length,
    keyword ? (keyword + ' / 対象: ' + scopeLabel) : ('なし / 対象: ' + scopeLabel),
    requestedTarget,
    effectiveTarget
  );

  closeDetailSearch();
  switchPage('search');

  if (results.length > 0) {
    showProgramDetail(results[0]);
  } else {
    setText(
      'detailLastUpdate',
      (requestedTarget !== effectiveTarget)
        ? '過去検索データ未生成のため 現在・本日以降 を検索'
        : '一致する番組はありません'
    );
  }
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Footer_Search4_(std::wstring& js)
        {
            js += LR"JS(
window.__epgPortalDebugLastResults = function(keyword){
  const tokens = makeKeywordTokens_(keyword || (
    (document.getElementById('detailKeyword') && document.getElementById('detailKeyword').value) ||
    (document.getElementById('quickKeyword') && document.getElementById('quickKeyword').value) ||
    ''
  ));

  const makeSnippet = (text) => {
    const raw = String(text || '');
    const normalized = normalizeSearchText(raw);

    for (const token of tokens) {
      const pos = normalized.indexOf(token);
      if (pos >= 0) {
        const roughStart = Math.max(0, pos - 40);
        const roughEnd = Math.min(raw.length, pos + 120);
        return raw.slice(roughStart, roughEnd);
      }
    }

    return raw.slice(0, 160);
  };

  return Array.isArray(gLastResults)
    ? gLastResults.map((item, index) => ({
        index: index,
        title: item.title || '',
        service: item.service || '',
        date: item.dateText || '',
        time: item.time || '',
        hitField: item.__searchHitField || '',
        titleHasKeyword: tokens.some(t => normalizeSearchText(item.title || '').includes(t)),
        summaryHasKeyword: tokens.some(t => normalizeSearchText(item.summary || '').includes(t)),
        summarySnippet: makeSnippet(item.summary || '')
      }))
    : [];
};
)JS";
        }

        void AppendRuntimeGeneratedScript_Footer_Search_(std::wstring& js)
        {
            AppendRuntimeGeneratedScript_Footer_Search1_(js);
            AppendRuntimeGeneratedScript_Footer_Search2_(js);
            AppendRuntimeGeneratedScript_Footer_Search3_(js);
            AppendRuntimeGeneratedScript_Footer_Search4_(js);
        }

        void AppendRuntimeGeneratedScript_Footer_ProgramCore1_(std::wstring& js)
        {
            js += LR"JS(
function buildProgramLikeFromPayload(payload){
  return {
    serviceId: Number(payload && payload.serviceId ? payload.serviceId : 0),
    eventId: Number(payload && payload.eventId ? payload.eventId : 0),
    service: String(payload && (payload.serviceName || payload.service) ? (payload.serviceName || payload.service) : ''),
    title: String(payload && (payload.titleLabel || payload.title) ? (payload.titleLabel || payload.title) : ''),
    dateText: String(payload && (payload.dateText || payload.date) ? (payload.dateText || payload.date) : ''),
    time: String(payload && (payload.timeText || payload.time) ? (payload.timeText || payload.time) : ''),
    weekdayText: String(payload && payload.weekdayText ? payload.weekdayText : ''),
    summary: String(payload && payload.summary ? payload.summary : ''),
    genre: String(payload && payload.genre ? payload.genre : ''),
    networkId: Number(payload && payload.networkId ? payload.networkId : 0),
    tsId: Number(payload && payload.tsId ? payload.tsId : 0)
  };
}

function getProgramButtonStateFromPayload(payload){
  const program = buildProgramLikeFromPayload(payload);
  const matches = getProgramReservations(program, true);

  const watchMatches = matches.filter(item => item.mode === 'watch');
  const recordMatches = matches.filter(item => item.mode === 'record');

  return {
    watchReserved: watchMatches.length > 0,
    recordReserved: recordMatches.length > 0,
    watchEnabled: watchMatches.some(item => item.enabled !== false) || (watchMatches.length === 0),
    recordEnabled: recordMatches.some(item => item.enabled !== false) || (recordMatches.length === 0)
  };
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Footer_ProgramCore2_(std::wstring& js)
        {
            js += LR"JS(
function toggleProgramReservationByPayload(payload, mode){
  const fixedMode = normalizeReservationMode(mode);
  const program = buildProgramLikeFromPayload(payload);
  const state = getProgramButtonStateFromPayload(payload);

  const alreadyReserved =
    fixedMode === 'watch' ? state.watchReserved : state.recordReserved;

  if (!window.__epgReservationStore) {
    return Promise.resolve({
      ok: false,
      changed: false,
      reason: 'reservation-store-missing',
      state: state
    });
  }

  if (alreadyReserved) {
    return window.__epgReservationStore
      .removeProgram(program, fixedMode)
      .then(result => {
        refreshReservationVisuals();
        return Object.assign({}, result || {}, {
          state: getProgramButtonStateFromPayload(payload)
        });
      });
  }

  return window.__epgReservationStore
    .upsertProgram(program, fixedMode)
    .then(result => {
      refreshReservationVisuals();
      return Object.assign({}, result || {}, {
        state: getProgramButtonStateFromPayload(payload)
      });
    });
}

function toggleProgramReservationDisableByPayload(payload, mode){
  const program = buildProgramLikeFromPayload(payload);
  const fixedMode = normalizeReservationMode(mode);
  const state = getProgramButtonStateFromPayload(payload);

  const currentEnabled =
    fixedMode === 'watch'
      ? state.watchEnabled
      : state.recordEnabled;

  if (!window.__epgReservationStore) {
    return Promise.resolve({
      ok: false,
      changed: false,
      reason: 'reservation-store-missing',
      state: state
    });
  }

  return window.__epgReservationStore
    .setProgramEnabled(program, fixedMode, !currentEnabled)
    .then(result => {
      refreshReservationVisuals();
      return Object.assign({}, result || {}, {
        state: getProgramButtonStateFromPayload(payload)
      });
    });
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Footer_ProgramCore3_(std::wstring& js)
        {
            js += LR"JS(
function publishDynamicProgramTableBridgeFallback_(){
  try {
    if (window.top && window.top !== window) {
      // iframe側の dynamic runtime は親 bridge を絶対に上書きしない。
      return;
    }
  } catch (_) {
    return;
  }

  try {
    if (window.__epgPortalSwitchProgramTableBridge) {
      window.__epgPortalProgramTableBridge =
        window.__epgPortalSwitchProgramTableBridge;
      return;
    }

    if (window.__epgPortalProgramTableBridge &&
        window.__epgPortalProgramTableBridge.__epgSwitchBridge === true) {
      return;
    }
  } catch (_) {
  }

  window.__epgPortalProgramTableBridge = {
    __epgDynamicFallbackBridge: true,

    getState: getProgramButtonStateFromPayload,

    toggle(payload, mode){
      const beforeState = getProgramButtonStateFromPayload(payload);

      Promise.resolve(toggleProgramReservationByPayload(payload, mode))
        .then(() => {
          refreshReservationVisuals();
          refreshFrameReservationButtons();
        });

      return {
        ok: true,
        pending: true,
        state: beforeState
      };
    },

    toggleDisable(payload, mode){
      const beforeState = getProgramButtonStateFromPayload(payload);

      Promise.resolve(toggleProgramReservationDisableByPayload(payload, mode))
        .then(() => {
          refreshReservationVisuals();
          refreshFrameReservationButtons();
        });

      return {
        ok: true,
        pending: true,
        state: beforeState
      };
    }
  };
}

window.__epgPortalProgramTableRenderAll = function(){
  refreshFrameReservationButtons();
};
)JS";
        }

        void AppendRuntimeGeneratedScript_Footer_ProgramCore_(std::wstring& js)
        {
            AppendRuntimeGeneratedScript_Footer_ProgramCore1_(js);
            AppendRuntimeGeneratedScript_Footer_ProgramCore2_(js);
            AppendRuntimeGeneratedScript_Footer_ProgramCore3_(js);
        }

        void AppendRuntimeGeneratedScript_Footer_ProgramOps_(std::wstring& js)
        {
            js += LR"JS(
function requestWatchReservation(index){
  if (!Array.isArray(gLastResults)) return;
  if (!Number.isFinite(index) || index < 0 || index >= gLastResults.length) return;
  const item = gLastResults[index];
  if (!item) return;

  const bridge = window.__epgPortalProgramTableBridge;
  if (bridge && typeof bridge.toggle === 'function') {
    bridge.toggle(item, 'watch');
    refreshReservationVisuals();
    return;
  }

  toggleProgramReservationByPayload(item, 'watch');
}

function requestRecordReservation(index){
  if (!Array.isArray(gLastResults)) return;
  if (!Number.isFinite(index) || index < 0 || index >= gLastResults.length) return;
  const item = gLastResults[index];
  if (!item) return;

  const bridge = window.__epgPortalProgramTableBridge;
  if (bridge && typeof bridge.toggle === 'function') {
    bridge.toggle(item, 'record');
    refreshReservationVisuals();
    return;
  }

  toggleProgramReservationByPayload(item, 'record');
}

function toggleWatchReservationDisabled(index){
  if (!Array.isArray(gLastResults)) return;
  if (!Number.isFinite(index) || index < 0 || index >= gLastResults.length) return;
  const item = gLastResults[index];
  if (!item) return;

  const bridge = window.__epgPortalProgramTableBridge;
  if (bridge && typeof bridge.toggleDisable === 'function') {
    bridge.toggleDisable(item, 'watch');
    refreshReservationVisuals();
    return;
  }

  toggleProgramReservationDisableByPayload(item, 'watch');
}

function toggleRecordReservationDisabled(index){
  if (!Array.isArray(gLastResults)) return;
  if (!Number.isFinite(index) || index < 0 || index >= gLastResults.length) return;
  const item = gLastResults[index];
  if (!item) return;

  const bridge = window.__epgPortalProgramTableBridge;
  if (bridge && typeof bridge.toggleDisable === 'function') {
    bridge.toggleDisable(item, 'record');
    refreshReservationVisuals();
    return;
  }

  toggleProgramReservationDisableByPayload(item, 'record');
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Footer_ProgramOps2_(std::wstring& js)
        {
            js += LR"JS(
function requestProgramReserveByIndexWithMode(index, mode){
  const fixedMode = String(mode || '').toLowerCase();
  if (fixedMode === 'watch') {
    requestWatchReservation(index);
    return;
  }
  requestRecordReservation(index);
}

function requestProgramReserveByIndex(index){
  const mode = (EPG_RESERVE_BUTTON_MODE === 'watch') ? 'watch' : 'record';
  requestProgramReserveByIndexWithMode(index, mode);
}

function requestProgramCancelByIndex(index){
  if (!Array.isArray(gLastResults)) return;
  if (!Number.isFinite(index) || index < 0 || index >= gLastResults.length) return;
  const item = gLastResults[index];
  if (!item) return;

  const state = getProgramButtonStateFromPayload(item);
  const bridge = window.__epgPortalProgramTableBridge;

  if (bridge && typeof bridge.toggle === 'function') {
    if (state.recordReserved) {
      bridge.toggle(item, 'record');
    }
    if (state.watchReserved) {
      bridge.toggle(item, 'watch');
    }
    refreshReservationVisuals();
    return;
  }

  cancelProgram(buildProgramLikeFromPayload(item));
  refreshReservationVisuals();
}

function requestSelectedProgramReserveWithMode(mode){
  if (!gSelectedProgram) return;

  const index = Array.isArray(gLastResults)
    ? gLastResults.indexOf(gSelectedProgram)
    : -1;

  if (index < 0) return;
  requestProgramReserveByIndexWithMode(index, mode);
}

function requestSelectedProgramReserve(){
  const mode = (EPG_RESERVE_BUTTON_MODE === 'watch') ? 'watch' : 'record';
  requestSelectedProgramReserveWithMode(mode);
}

function requestSelectedProgramCancel(){
  if (!gSelectedProgram) return;
  cancelProgram(buildProgramLikeFromPayload(gSelectedProgram));
  refreshReservationVisuals();
}
)JS";
        }

        void AppendRuntimeGeneratedScript_Footer_Init_(std::wstring& js)
        {
            js += LR"JS(
function initPortalRuntime(){
  try {
    syncReservationMirrorFromStaticData_('initPortalRuntime');
  } catch (e) {
    console.error('[EPG-PORTAL] reservation static sync failed', e);
  }

  const dateInput = document.getElementById('displayDate');
  if (dateInput && !dateInput.value) {
    const now = new Date();
    const y = now.getFullYear();
    const m = String(now.getMonth() + 1).padStart(2, '0');
    const d = String(now.getDate()).padStart(2, '0');
    dateInput.value = y + '-' + m + '-' + d;
  }

  registerFrameReservationHooks();
  publishProgramTableBridge();

  ensureSearchScopeControls_();
  syncPageStateFromDom();

  // switchSpace() 内で bindFrame() と scheduleSpaceRefresh() が走る。
  // そのため、この後で rebuildProgramCache(true) / refreshServiceList() を
  // もう一度直接呼ばない。
  const initialSpace = (typeof DEFAULT_SPACE === 'number') ? DEFAULT_SPACE : gCurrentSpace;
  switchSpace(initialSpace, document.querySelector('[data-space="' + initialSpace + '"]'));

  // ページ表示だけを確定する。
  // 予約一覧ページの場合は switchPage() 内で renderReservationPage() される。
  switchPage(gCurrentPage || 'now');

  // iframe 側の予約ボタン同期は registerFrameReservationHooks() の load イベントで行う。
  // 初期化直後に refreshFrameReservationButtons() を直接呼ぶと、
  // iframeロード前に空振りした上で処理が重くなるため呼ばない。
}

window.switchPage = switchPage;
window.runQuickSearch = runQuickSearch;
window.openDetailSearch = openDetailSearch;
window.closeDetailSearch = closeDetailSearch;
window.toggleMainSwitchTableFocus = toggleMainSwitchTableFocus;
window.applyQuickView = applyQuickView;
window.switchSpace = switchSpace;
window.runDetailSearch = runDetailSearch;
window.requestProgramReserveByIndex = requestProgramReserveByIndex;
window.requestProgramCancelByIndex = requestProgramCancelByIndex;
window.requestSelectedProgramReserve = requestSelectedProgramReserve;
window.requestSelectedProgramReserveWithMode = requestSelectedProgramReserveWithMode;
window.requestSelectedProgramCancel = requestSelectedProgramCancel;
window.requestProgramReserveByIndexWithMode = requestProgramReserveByIndexWithMode;
window.removeReservationById = removeReservationById;
window.toggleReservationEnabledById = toggleReservationEnabledById;

window.__epgPortalGetLastResults = function(){
  return Array.isArray(gLastResults) ? gLastResults : [];
};

window.__epgPortalGetSelectedProgram = function(){
  return gSelectedProgram || null;
};

window.__epgPortalGetCurrentSpace = function(){
  return Number(gCurrentSpace);
};

window.__epgPortalGetCurrentPage = function(){
  return String(gCurrentPage || '');
};

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', initPortalRuntime, { once: true });
} else {
  initPortalRuntime();
}
})();
)JS";
        }

        void AppendRuntimeGeneratedScript_Footer_(std::wstring& js)
        {
            AppendRuntimeGeneratedScript_Footer_Page_(js);
            AppendRuntimeGeneratedScript_Footer_Page2_(js);
            AppendRuntimeGeneratedScript_Footer_Search_(js);
            AppendRuntimeGeneratedScript_Footer_ProgramCore_(js);
            AppendRuntimeGeneratedScript_Footer_ProgramOps_(js);
            AppendRuntimeGeneratedScript_Footer_ProgramOps2_(js);
            AppendRuntimeGeneratedScript_Footer_Init_(js);
        }

        using RuntimeAppendFunc_ = void (*)(std::wstring&);

        struct RuntimeGeneratedBlock_
        {
            std::wstring_view name;
            RuntimeAppendFunc_ append;
        };

        constexpr std::array<RuntimeGeneratedBlock_, 5> GetRuntimeGeneratedBlocks_()
        {
            return { {
                { L"Header / ReservationStore / EventKey", AppendRuntimeGeneratedScript_Header_ },
                { L"FrameAndCache",                       AppendRuntimeGeneratedScript_FrameAndCache_ },
                { L"Render / Reservation / Search",       AppendRuntimeGeneratedScript_Render_ },
                { L"Action / PageSwitch",                 AppendRuntimeGeneratedScript_Action_ },
                { L"Footer / Search / ProgramBridge",     AppendRuntimeGeneratedScript_Footer_ }
            } };
        }

        void AppendRuntimeGeneratedScriptBlock_(
            std::wstring& js,
            const RuntimeGeneratedBlock_& block)
        {
            const std::size_t before = js.size();

            js += L"\n";
            js += L"// ===== ";
            js += block.name;
            js += L" =====\n";

            if (block.append) {
                block.append(js);
            }

            js += L"\n";
            js += L"// ===== end: ";
            js += block.name;
            js += L" =====\n";

            DebugLogFormat(
                L"[EPG-PORTAL][DYNAMIC][BLOCK] %ls: +%zu chars",
                std::wstring(block.name).c_str(),
                js.size() - before);
        }

        std::wstring BuildRuntimeGeneratedScript_()
        {
            std::wstring js;

            // C2026対策ではなく、生成時の再確保低減用。
            // IIFE開始は AppendRuntimeGeneratedScript_Header_Base_() 側にある。
            js.reserve(128 * 1024);

            for (const auto& block : GetRuntimeGeneratedBlocks_()) {
                AppendRuntimeGeneratedScriptBlock_(js, block);
            }

            return js;
        }

        bool WriteRuntimeGeneratedScript_(const std::filesystem::path& jsPath)
        {
            const std::wstring js = BuildRuntimeGeneratedScript_();

            const bool ok =
                EpgHtmlPortal_Common::WriteUtf8BomTextFile(
                    jsPath,
                    js);

            if (ok) {
                DebugLogFormat(
                    L"[EPG-PORTAL][DYNAMIC] runtime generated internally: %ls chars=%zu",
                    jsPath.wstring().c_str(),
                    js.size());
            }

            return ok;
        }

        bool EnsureRuntimeScriptAssets_(const EpgHtmlPortal::WriteOptions& opt)
        {
            const std::filesystem::path outputPath =
                opt.outputDir / L"epg_portal_runtime.js";

            DebugLog(L"[EPG-PORTAL][DYNAMIC] internal runtime を強制再生成します。");
            return WriteRuntimeGeneratedScript_(outputPath);
        }
    }

    bool WriteDynamicAssets(const EpgHtmlPortal::WriteOptions& opt)
    {
        bool ok = true;

        // 検索DB本体を生成する。
        // 空スタブは使わない。
        if (!EpgHtmlPortal_SearchData::WriteSearchDataJs(opt)) {
            DebugLog(L"[EPG-PORTAL][WARN] search data js の生成に失敗しました。");
            ok = false;
        }

        if (!EpgHtmlPortal_ReservationData::WriteReservationDataJs(opt)) {
            DebugLog(L"[EPG-PORTAL][WARN] reservation data js の生成に失敗しました。");
            ok = false;
        }

        if (!EpgHtmlPortal_SwitchScript::WriteRuntimeSwitchFragment(opt)) {
            DebugLog(L"[EPG-PORTAL][WARN] switch runtime fragment の生成に失敗しました。");
            ok = false;
        }

        if (!EpgHtmlPortal_SwitchScript::WriteProgramTableSwitchFragment(opt)) {
            DebugLog(L"[EPG-PORTAL][WARN] program table switch js の生成に失敗しました。");
            ok = false;
        }

        if (!EnsureRuntimeScriptAssets_(opt)) {
            DebugLog(L"[EPG-PORTAL][WARN] runtime js の生成に失敗しました。");
            ok = false;
        }

        return ok;
    }

    std::wstring BuildRuntimeScriptBlock(
        std::wstring_view jsSpaceData,
        std::wstring_view jsConfigServiceData,
        int initialSpace)
    {
        return EpgHtmlPortal_RuntimeData::BuildRuntimeScriptBlock(
            jsSpaceData,
            jsConfigServiceData,
            initialSpace,
            L"epg_portal_runtime.js");
    }
}