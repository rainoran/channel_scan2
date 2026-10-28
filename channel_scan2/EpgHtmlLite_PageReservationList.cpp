#include "EpgHtmlLite_PageReservationList.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <Windows.h>
#endif

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "EpgHtmlLite_Common.hpp"

namespace
{
    using namespace EpgHtmlLite;

    constexpr const wchar_t* kReservationListScriptFile =
        L"epg_lite_reservation_list.js";

    constexpr const wchar_t* kReservationDataScriptFile =
        L"reservation_data.js";

    std::wstring HtmlEscape(
        const std::wstring_view s)
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
            default:
                out.push_back(ch);
                break;
            }
        }

        return out;
    }

    bool WriteUtf8BomTextFile(
        const std::filesystem::path& path,
        const std::wstring& content)
    {
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
            const int required =
                ::WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    content.c_str(),
                    static_cast<int>(content.size()),
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
                    content.c_str(),
                    static_cast<int>(content.size()),
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
        for (wchar_t wc : content) {
            ofs.put(
                static_cast<char>(wc & 0xFF));
        }
#endif

        return ofs.good();
    }

    std::wstring MakeSpaceLabel(
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

    std::wstring BuildReservationListFileName(
        const int space)
    {
        return L"epg_lite_reservation_list_space" +
            std::to_wstring(space) +
            L".html";
    }

    std::wstring BuildEmptyReservationDataScript()
    {
        std::wstring js;
        js += L"window.EPG_RESERVATION_DATA = window.EPG_RESERVATION_DATA || [];\n";
        js += L"window.EPG_LITE_RESERVATION_DATA = window.EPG_LITE_RESERVATION_DATA || null;\n";
        return js;
    }

    static void AppendReservationListScript_Core_(
        std::wstring& js)
    {
        js += L"(function () {\n";
        js += L"'use strict';\n";
        js += L"\n";

        js += L"const DATA_FILE = 'reservation_data.js';\n";
        js += L"const VIEW_CACHE_FILE = '/cache/reservation/view/tasks_view_index.tsv';\n";
        js += L"let gItems = [];\n";
        js += L"let gFilter = 'all';\n";
        js += L"let gSearchText = '';\n";
        js += L"let gDataSource = 'none';\n";
        js += L"\n";

        js += L"function splitTsvLine(line) {\n";
        js += L"    return String(line || '').split('\\t');\n";
        js += L"}\n";
        js += L"\n";

        js += L"function parseReservationViewCacheTsv(text) {\n";
        js += L"    const rows = [];\n";
        js += L"    const src = String(text || '').replace(/^\\uFEFF/, '');\n";
        js += L"    const lines = src.split(/\\r?\\n/);\n";
        js += L"    let header = null;\n";
        js += L"\n";
        js += L"    for (let i = 0; i < lines.length; ++i) {\n";
        js += L"        const line = lines[i];\n";
        js += L"        if (!line || /^\\s*$/.test(line)) continue;\n";
        js += L"        if (line.charAt(0) === '#') continue;\n";
        js += L"\n";
        js += L"        const cols = splitTsvLine(line);\n";
        js += L"        if (!header) {\n";
        js += L"            header = cols.map(function (v) { return String(v || '').trim(); });\n";
        js += L"            continue;\n";
        js += L"        }\n";
        js += L"\n";
        js += L"        const row = {};\n";
        js += L"        for (let c = 0; c < header.length; ++c) {\n";
        js += L"            row[header[c]] = c < cols.length ? cols[c] : '';\n";
        js += L"        }\n";
        js += L"        rows.push(row);\n";
        js += L"    }\n";
        js += L"\n";
        js += L"    return rows;\n";
        js += L"}\n";
        js += L"\n";

        js += L"function normalizeEntryFromViewCacheRow(row, index) {\n";
        js += L"    if (!row || typeof row !== 'object') return null;\n";
        js += L"\n";
        js += L"    const mode = normalizeMode(row.mode || row.reserveMode || row.category || '');\n";
        js += L"    const kind = intValue(row.kind, 0);\n";
        js += L"    const instance = intValue(row.instance, 0);\n";
        js += L"    const title = textValue(row.title || row.label || row.name || '予約項目');\n";
        js += L"    const serviceName = textValue(row.serviceName || row.service || row.target || '');\n";
        js += L"    const startDate = textValue(row.startDate || row.date || row.dateText || '');\n";
        js += L"    const startTime = textValue(row.startTime || row.time || row.timeText || '');\n";
        js += L"    const endDate = textValue(row.endDate || row.endDateText || '');\n";
        js += L"    const endTime = textValue(row.endTime || row.endTimeText || '');\n";
        js += L"    const dedupeKey = textValue(row.dedupeKey || '');\n";
        js += L"    const reserveKey = textValue(row.reserveKey || '');\n";
        js += L"    const fallbackKey = textValue(row.fallbackKey || '');\n";
        js += L"\n";
        js += L"    let timeText = startTime;\n";
        js += L"    if (startTime && endTime) {\n";
        js += L"        timeText = startTime + '～' + endTime;\n";
        js += L"    }\n";
        js += L"\n";
        js += L"    return {\n";
        js += L"        raw: row,\n";
        js += L"        id: dedupeKey || reserveKey || fallbackKey || ('task-' + String(kind) + '-' + String(instance) + '-' + String(index)),\n";
        js += L"        label: title,\n";
        js += L"        mode: mode,\n";
        js += L"        enabled: boolValue(row.enabled, true),\n";
        js += L"        disabledByUser: boolValue(row.disabledByUser, false),\n";
        js += L"        excludeFromAuto: boolValue(row.excludeFromAuto, false),\n";
        js += L"        autoReserved: boolValue(row.autoReserved, false),\n";
        js += L"        date: startDate,\n";
        js += L"        weekday: textValue(row.weekday || row.weekdayText || ''),\n";
        js += L"        time: timeText,\n";
        js += L"        endDate: endDate,\n";
        js += L"        endTime: endTime,\n";
        js += L"        service: serviceName,\n";
        js += L"        summary: textValue(row.note || row.summary || ''),\n";
        js += L"        genre: textValue(row.genre || ''),\n";
        js += L"        dedupeKey: dedupeKey,\n";
        js += L"        excludeKey: textValue(row.excludeKey || dedupeKey),\n";
        js += L"        reserveKey: reserveKey,\n";
        js += L"        fallbackKey: fallbackKey,\n";
        js += L"        reserveSource: 'view_cache',\n";
        js += L"        space: intValue(row.space, 0),\n";
        js += L"        channel: intValue(row.channel, -1),\n";
        js += L"        networkId: intValue(row.networkId, 0),\n";
        js += L"        tsId: intValue(row.tsId, 0),\n";
        js += L"        serviceId: intValue(row.serviceId, 0),\n";
        js += L"        eventId: intValue(row.eventId, 0),\n";
        js += L"        kind: kind,\n";
        js += L"        instance: instance,\n";
        js += L"        preferredTunerIndex: intValue(row.preferredTunerIndex, -1),\n";
        js += L"        requiredTuners: intValue(row.requiredTuners, 1),\n";
        js += L"        fixed: false,\n";
        js += L"        deleted: false\n";
        js += L"    };\n";
        js += L"}\n";
        js += L"\n";

        js += L"function loadReservationViewCacheEntriesSync() {\n";
        js += L"    try {\n";
        js += L"        const xhr = new XMLHttpRequest();\n";
        js += L"        const sep = VIEW_CACHE_FILE.indexOf('?') >= 0 ? '&' : '?';\n";
        js += L"        xhr.open('GET', VIEW_CACHE_FILE + sep + 'v=' + String(Date.now()), false);\n";
        js += L"        xhr.send(null);\n";
        js += L"\n";
        js += L"        if (xhr.status < 200 || xhr.status >= 300) {\n";
        js += L"            return null;\n";
        js += L"        }\n";
        js += L"\n";
        js += L"        const rows = parseReservationViewCacheTsv(xhr.responseText || '');\n";
        js += L"        const entries = rows\n";
        js += L"            .map(normalizeEntryFromViewCacheRow)\n";
        js += L"            .filter(Boolean);\n";
        js += L"\n";
        js += L"        gDataSource = 'view-cache';\n";
        js += L"        return entries;\n";
        js += L"    } catch (_) {\n";
        js += L"        return null;\n";
        js += L"    }\n";
        js += L"}\n";
        js += L"\n";

        js += L"function toArrayData() {\n";
        js += L"    const cacheEntries = loadReservationViewCacheEntriesSync();\n";
        js += L"    if (Array.isArray(cacheEntries)) {\n";
        js += L"        return cacheEntries;\n";
        js += L"    }\n";
        js += L"\n";
        js += L"    const lite = window.EPG_LITE_RESERVATION_DATA;\n";
        js += L"    if (Array.isArray(lite)) {\n";
        js += L"        gDataSource = 'lite-js';\n";
        js += L"        return lite;\n";
        js += L"    }\n";
        js += L"    if (lite && Array.isArray(lite.items)) {\n";
        js += L"        gDataSource = 'lite-js';\n";
        js += L"        return lite.items;\n";
        js += L"    }\n";
        js += L"\n";
        js += L"    const normal = window.EPG_RESERVATION_DATA;\n";
        js += L"    if (Array.isArray(normal)) {\n";
        js += L"        gDataSource = 'reservation_data.js';\n";
        js += L"        return normal;\n";
        js += L"    }\n";
        js += L"    if (normal && Array.isArray(normal.items)) {\n";
        js += L"        gDataSource = 'reservation_data.js';\n";
        js += L"        return normal.items;\n";
        js += L"    }\n";
        js += L"\n";
        js += L"    gDataSource = 'empty';\n";
        js += L"    return [];\n";
        js += L"}\n";

        js += L"function boolValue(v, defValue) {\n";
        js += L"    if (v === true) return true;\n";
        js += L"    if (v === false) return false;\n";
        js += L"    if (v == null) return !!defValue;\n";
        js += L"    const s = String(v).toLowerCase().trim();\n";
        js += L"    if (s === '1' || s === 'true' || s === 'yes' || s === 'on') return true;\n";
        js += L"    if (s === '0' || s === 'false' || s === 'no' || s === 'off') return false;\n";
        js += L"    return !!defValue;\n";
        js += L"}\n";
        js += L"\n";

        js += L"function intValue(v, defValue) {\n";
        js += L"    const n = Number(v);\n";
        js += L"    return Number.isFinite(n) ? Math.trunc(n) : defValue;\n";
        js += L"}\n";
        js += L"\n";

        js += L"function textValue(v) {\n";
        js += L"    return String(v == null ? '' : v);\n";
        js += L"}\n";
        js += L"\n";

        js += L"function normalizeMode(mode) {\n";
        js += L"    const v = textValue(mode).toLowerCase().trim();\n";
        js += L"    if (v === 'record') return 'record';\n";
        js += L"    if (v === 'watch') return 'watch';\n";
        js += L"    if (v === 'epg' || v.indexOf('epg_') === 0) return 'epg';\n";
        js += L"    if (v === 'reserve') return 'reserve';\n";
        js += L"    return v || 'reserve';\n";
        js += L"}\n";
        js += L"\n";

        js += L"function modeLabel(mode) {\n";
        js += L"    switch (normalizeMode(mode)) {\n";
        js += L"    case 'record': return '録画';\n";
        js += L"    case 'watch': return '視聴';\n";
        js += L"    case 'epg': return 'EPG';\n";
        js += L"    default: return '予約';\n";
        js += L"    }\n";
        js += L"}\n";
        js += L"\n";

        js += L"function normalizeSearchText(s) {\n";
        js += L"    let v = textValue(s);\n";
        js += L"    try { v = v.normalize('NFKC'); } catch (_) {}\n";
        js += L"    return v.toLowerCase().replace(/\\s+/g, '').trim();\n";
        js += L"}\n";
        js += L"\n";

        js += L"function htmlEscape(s) {\n";
        js += L"    return textValue(s)\n";
        js += L"        .replace(/&/g, '&amp;')\n";
        js += L"        .replace(/</g, '&lt;')\n";
        js += L"        .replace(/>/g, '&gt;')\n";
        js += L"        .replace(/\\\"/g, '&quot;');\n";
        js += L"}\n";
        js += L"\n";

        js += L"function setText(id, text) {\n";
        js += L"    const el = document.getElementById(id);\n";
        js += L"    if (el) el.textContent = textValue(text);\n";
        js += L"}\n";
        js += L"\n";

        js += L"function makeElement(tag, className, text) {\n";
        js += L"    const el = document.createElement(tag);\n";
        js += L"    if (className) el.className = className;\n";
        js += L"    if (text != null) el.textContent = textValue(text);\n";
        js += L"    return el;\n";
        js += L"}\n";
        js += L"\n";

        js += L"function isFixedEpgConfigTask(item) {\n";
        js += L"    if (!item) return false;\n";
        js += L"    const mode = normalizeMode(item.mode || item.reserveMode || item.taskType);\n";
        js += L"    const kind = intValue(item.kind, 0);\n";
        js += L"    const instance = intValue(item.instance, 0);\n";
        js += L"    return mode === 'epg' && (kind === 1 || kind === 2) && instance <= 0;\n";
        js += L"}\n";
        js += L"\n";

        js += L"function normalizeEntry(src, index) {\n";
        js += L"    const mode = normalizeMode(src.mode || src.reserveMode || src.taskType || src.category);\n";
        js += L"    const enabled = boolValue(src.enabled, true);\n";
        js += L"    const disabledByUser = boolValue(src.disabledByUser, false);\n";
        js += L"    const id = textValue(src.id || src.reserveKey || src.dedupeKey || ('reservation-' + index));\n";
        js += L"    const label = textValue(src.label || src.titleLabel || src.title || src.programTitle || '予約項目');\n";
        js += L"    const service = textValue(src.service || src.serviceName || src.target || src.channelName || '');\n";
        js += L"    const date = textValue(src.date || src.dateText || src.startDate || '');\n";
        js += L"    const time = textValue(src.displayTimeText || src.time || src.timeText || src.startTime || '');\n";
        js += L"    const endDate = textValue(src.endDate || src.endDateText || '');\n";
        js += L"    const endTime = textValue(src.endTime || src.endTimeText || '');\n";
        js += L"\n";
        js += L"    return {\n";
        js += L"        raw: src,\n";
        js += L"        id: id,\n";
        js += L"        label: label,\n";
        js += L"        mode: mode,\n";
        js += L"        enabled: enabled,\n";
        js += L"        disabledByUser: disabledByUser,\n";
        js += L"        excludeFromAuto: boolValue(src.excludeFromAuto, false),\n";
        js += L"        autoReserved: boolValue(src.autoReserved, false),\n";
        js += L"        date: date,\n";
        js += L"        weekday: textValue(src.weekday || src.weekdayText || ''),\n";
        js += L"        time: time,\n";
        js += L"        endDate: endDate,\n";
        js += L"        endTime: endTime,\n";
        js += L"        service: service,\n";
        js += L"        summary: textValue(src.summary || src.note || src.description || ''),\n";
        js += L"        genre: textValue(src.genre || ''),\n";
        js += L"        dedupeKey: textValue(src.dedupeKey || ''),\n";
        js += L"        excludeKey: textValue(src.excludeKey || src.dedupeKey || ''),\n";
        js += L"        reserveSource: textValue(src.reserveSource || src.source || ''),\n";
        js += L"        space: intValue(src.space, 0),\n";
        js += L"        channel: intValue(src.channel, -1),\n";
        js += L"        networkId: intValue(src.networkId, 0),\n";
        js += L"        tsId: intValue(src.tsId, 0),\n";
        js += L"        serviceId: intValue(src.serviceId, 0),\n";
        js += L"        eventId: intValue(src.eventId, 0),\n";
        js += L"        kind: intValue(src.kind, 0),\n";
        js += L"        instance: intValue(src.instance, 0),\n";
        js += L"        preferredTunerIndex: intValue(src.preferredTunerIndex, -1),\n";
        js += L"        requiredTuners: intValue(src.requiredTuners, 1),\n";
        js += L"        fixed: false,\n";
        js += L"        deleted: false\n";
        js += L"    };\n";
        js += L"}\n";
        js += L"\n";

        js += L"function normalizeAllEntries() {\n";
        js += L"    const list = toArrayData();\n";
        js += L"    return list.map(normalizeEntry).filter(function (item) {\n";
        js += L"        item.fixed = isFixedEpgConfigTask(item);\n";
        js += L"        return !!item.id || !!item.label;\n";
        js += L"    });\n";
        js += L"}\n";
        js += L"\n";

        js += L"function compareEntry(a, b) {\n";
        js += L"    const ad = textValue(a.date) + ' ' + textValue(a.time);\n";
        js += L"    const bd = textValue(b.date) + ' ' + textValue(b.time);\n";
        js += L"    if (ad !== bd) return ad < bd ? -1 : 1;\n";
        js += L"    if (a.service !== b.service) return a.service < b.service ? -1 : 1;\n";
        js += L"    if (a.label !== b.label) return a.label < b.label ? -1 : 1;\n";
        js += L"    return a.id < b.id ? -1 : a.id > b.id ? 1 : 0;\n";
        js += L"}\n";
    }

    static void AppendReservationListScript_FilterRender_(
        std::wstring& js)
    {
        js += L"function entryStatus(item) {\n";
        js += L"    if (item.deleted) return 'deleted';\n";
        js += L"    if (!item.enabled) return 'disabled';\n";
        js += L"    return 'enabled';\n";
        js += L"}\n";
        js += L"\n";

        js += L"function statusLabel(item) {\n";
        js += L"    const st = entryStatus(item);\n";
        js += L"    if (st === 'deleted') return '削除済み';\n";
        js += L"    if (st === 'disabled') return item.disabledByUser ? '手動無効' : '無効';\n";
        js += L"    return '有効';\n";
        js += L"}\n";
        js += L"\n";

        js += L"function entryMatchesFilter(item) {\n";
        js += L"    if (!item) return false;\n";
        js += L"    if (item.deleted && gFilter !== 'deleted') return false;\n";
        js += L"\n";
        js += L"    switch (gFilter) {\n";
        js += L"    case 'enabled':\n";
        js += L"        if (!item.enabled || item.deleted) return false;\n";
        js += L"        break;\n";
        js += L"    case 'disabled':\n";
        js += L"        if (item.enabled || item.deleted) return false;\n";
        js += L"        break;\n";
        js += L"    case 'record':\n";
        js += L"    case 'watch':\n";
        js += L"    case 'epg':\n";
        js += L"        if (normalizeMode(item.mode) !== gFilter) return false;\n";
        js += L"        break;\n";
        js += L"    case 'deleted':\n";
        js += L"        if (!item.deleted) return false;\n";
        js += L"        break;\n";
        js += L"    case 'other':\n";
        js += L"        if (['record', 'watch', 'epg'].indexOf(normalizeMode(item.mode)) >= 0) return false;\n";
        js += L"        break;\n";
        js += L"    case 'all':\n";
        js += L"    default:\n";
        js += L"        break;\n";
        js += L"    }\n";
        js += L"\n";
        js += L"    const q = normalizeSearchText(gSearchText);\n";
        js += L"    if (!q) return true;\n";
        js += L"\n";
        js += L"    const hay = normalizeSearchText([\n";
        js += L"        item.label,\n";
        js += L"        item.service,\n";
        js += L"        item.summary,\n";
        js += L"        item.genre,\n";
        js += L"        item.date,\n";
        js += L"        item.time,\n";
        js += L"        item.id,\n";
        js += L"        item.dedupeKey\n";
        js += L"    ].join(' '));\n";
        js += L"\n";
        js += L"    return hay.indexOf(q) >= 0;\n";
        js += L"}\n";
        js += L"\n";

        js += L"function updateFilterButtons() {\n";
        js += L"    document.querySelectorAll('[data-reserve-filter]').forEach(function (btn) {\n";
        js += L"        const value = btn.getAttribute('data-reserve-filter') || 'all';\n";
        js += L"        if (value === gFilter) btn.classList.add('active');\n";
        js += L"        else btn.classList.remove('active');\n";
        js += L"    });\n";
        js += L"}\n";
        js += L"\n";

        js += L"function updateSummary(viewItems) {\n";
        js += L"    const all = gItems.filter(function (v) { return !v.deleted; });\n";
        js += L"    const enabled = all.filter(function (v) { return v.enabled; }).length;\n";
        js += L"    const disabled = all.filter(function (v) { return !v.enabled; }).length;\n";
        js += L"    const record = all.filter(function (v) { return normalizeMode(v.mode) === 'record'; }).length;\n";
        js += L"    const watch = all.filter(function (v) { return normalizeMode(v.mode) === 'watch'; }).length;\n";
        js += L"    const epg = all.filter(function (v) { return normalizeMode(v.mode) === 'epg'; }).length;\n";
        js += L"\n";
        js += L"    setText('reserveCountAll', all.length);\n";
        js += L"    setText('reserveCountView', viewItems.length);\n";
        js += L"    setText('reserveCountEnabled', enabled);\n";
        js += L"    setText('reserveCountDisabled', disabled);\n";
        js += L"    setText('reserveCountRecord', record);\n";
        js += L"    setText('reserveCountWatch', watch);\n";
        js += L"    setText('reserveCountEpg', epg);\n";
        js += L"}\n";
        js += L"\n";

        js += L"function makeBadge(item) {\n";
        js += L"    const badge = makeElement('span', 'reserve-badge reserve-badge-' + entryStatus(item), statusLabel(item));\n";
        js += L"    return badge;\n";
        js += L"}\n";
        js += L"\n";

        js += L"function makeModeBadge(item) {\n";
        js += L"    return makeElement('span', 'reserve-mode reserve-mode-' + normalizeMode(item.mode), modeLabel(item.mode));\n";
        js += L"}\n";
        js += L"\n";

        js += L"function makeTimeText(item) {\n";
        js += L"    let out = '';\n";
        js += L"    if (item.date) out += item.date;\n";
        js += L"    if (item.weekday) out += ' ' + item.weekday;\n";
        js += L"    if (item.time) out += (out ? ' ' : '') + item.time;\n";
        js += L"    if (item.endTime) out += ' ～ ' + item.endTime;\n";
        js += L"    return out || '-';\n";
        js += L"}\n";
        js += L"\n";

        js += L"function appendMetaLine(parent, label, value) {\n";
        js += L"    if (!value) return;\n";
        js += L"    const div = makeElement('div', 'reserve-meta-line', '');\n";
        js += L"    const k = makeElement('span', 'reserve-meta-key', label);\n";
        js += L"    const v = makeElement('span', 'reserve-meta-value', value);\n";
        js += L"    div.appendChild(k);\n";
        js += L"    div.appendChild(v);\n";
        js += L"    parent.appendChild(div);\n";
        js += L"}\n";
        js += L"\n";

        js += L"function makePayloadForMutation(item) {\n";
        js += L"    return {\n";
        js += L"        id: item.id,\n";
        js += L"        kind: item.kind,\n";
        js += L"        instance: item.instance,\n";
        js += L"        mode: item.mode,\n";
        js += L"        reserveMode: item.mode,\n";
        js += L"        enabled: item.enabled,\n";
        js += L"        disabledByUser: item.disabledByUser,\n";
        js += L"        excludeFromAuto: item.excludeFromAuto,\n";
        js += L"        dedupeKey: item.dedupeKey,\n";
        js += L"        excludeKey: item.excludeKey,\n";
        js += L"        title: item.label,\n";
        js += L"        titleLabel: item.label,\n";
        js += L"        serviceName: item.service,\n";
        js += L"        service: item.service,\n";
        js += L"        dateText: item.date,\n";
        js += L"        timeText: item.time,\n";
        js += L"        endDate: item.endDate,\n";
        js += L"        endTime: item.endTime,\n";
        js += L"        space: item.space,\n";
        js += L"        networkId: item.networkId,\n";
        js += L"        tsId: item.tsId,\n";
        js += L"        serviceId: item.serviceId,\n";
        js += L"        eventId: item.eventId,\n";
        js += L"        requiredTuners: item.requiredTuners,\n";
        js += L"        preferredTunerIndex: item.preferredTunerIndex\n";
        js += L"    };\n";
        js += L"}\n";
    }

    static void AppendReservationListScript_Api_(
        std::wstring& js)
    {
        js += L"function getApiBase() {\n";
        js += L"    try {\n";
        js += L"        if (window.EPG_RUNTIME && window.EPG_RUNTIME.reservationApiBase) {\n";
        js += L"            return String(window.EPG_RUNTIME.reservationApiBase);\n";
        js += L"        }\n";
        js += L"    } catch (_) {}\n";
        js += L"\n";
        js += L"    try {\n";
        js += L"        if (window.parent &&\n";
        js += L"            window.parent !== window &&\n";
        js += L"            window.parent.EPG_RUNTIME &&\n";
        js += L"            window.parent.EPG_RUNTIME.reservationApiBase) {\n";
        js += L"            return String(window.parent.EPG_RUNTIME.reservationApiBase);\n";
        js += L"        }\n";
        js += L"    } catch (_) {}\n";
        js += L"\n";
        js += L"    try {\n";
        js += L"        if (window.top &&\n";
        js += L"            window.top !== window &&\n";
        js += L"            window.top.EPG_RUNTIME &&\n";
        js += L"            window.top.EPG_RUNTIME.reservationApiBase) {\n";
        js += L"            return String(window.top.EPG_RUNTIME.reservationApiBase);\n";
        js += L"        }\n";
        js += L"    } catch (_) {}\n";
        js += L"\n";
        js += L"    // Lite版の標準API。index.html側に EPG_RUNTIME が無くても同一ホストで動かす。\n";
        js += L"    return '/api/reservation';\n";
        js += L"}\n";

        js += L"function encodeForm(payload) {\n";
        js += L"    const pairs = [];\n";
        js += L"    Object.keys(payload || {}).forEach(function (key) {\n";
        js += L"        const value = payload[key];\n";
        js += L"        if (value === undefined || value === null) return;\n";
        js += L"        pairs.push(encodeURIComponent(key) + '=' + encodeURIComponent(String(value)));\n";
        js += L"    });\n";
        js += L"    return pairs.join('&');\n";
        js += L"}\n";
        js += L"\n";

        js += L"function postMutationSync(path, payload) {\n";
        js += L"    const apiBase = getApiBase();\n";
        js += L"    if (!apiBase) {\n";
        js += L"        return { ok:true, source:'local-preview', reason:'reservation-api-disabled' };\n";
        js += L"    }\n";
        js += L"\n";
        js += L"    try {\n";
        js += L"        const xhr = new XMLHttpRequest();\n";
        js += L"        xhr.open('POST', apiBase + path, false);\n";
        js += L"        xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded; charset=UTF-8');\n";
        js += L"        xhr.send(encodeForm(payload));\n";
        js += L"\n";
        js += L"        let obj = null;\n";
        js += L"        try { obj = JSON.parse(xhr.responseText || '{}'); } catch (_) { obj = null; }\n";
        js += L"\n";
        js += L"        if (obj && typeof obj === 'object') {\n";
        js += L"            if (obj.ok === undefined) obj.ok = xhr.status >= 200 && xhr.status < 300;\n";
        js += L"            if (obj.status === undefined) obj.status = xhr.status;\n";
        js += L"            return obj;\n";
        js += L"        }\n";
        js += L"\n";
        js += L"        return {\n";
        js += L"            ok: xhr.status >= 200 && xhr.status < 300,\n";
        js += L"            status: xhr.status,\n";
        js += L"            source: 'http',\n";
        js += L"            reason: 'invalid-json-response'\n";
        js += L"        };\n";
        js += L"    } catch (e) {\n";
        js += L"        return {\n";
        js += L"            ok:false,\n";
        js += L"            source:'http',\n";
        js += L"            reason:'http-error',\n";
        js += L"            message:e && e.message ? String(e.message) : String(e)\n";
        js += L"        };\n";
        js += L"    }\n";
        js += L"}\n";
        js += L"\n";

        js += L"function notifyParentReservationChanged(reason) {\n";
        js += L"    const detail = {\n";
        js += L"        source:'epg-lite-reservation-list',\n";
        js += L"        reason:String(reason || ''),\n";
        js += L"        stamp:Date.now()\n";
        js += L"    };\n";
        js += L"\n";
        js += L"    try {\n";
        js += L"        window.dispatchEvent(new CustomEvent('epg-reservation-list-changed', { detail:detail }));\n";
        js += L"    } catch (_) {}\n";
        js += L"\n";
        js += L"    try {\n";
        js += L"        if (window.parent && window.parent !== window) {\n";
        js += L"            window.parent.dispatchEvent(new CustomEvent('epg-reservation-list-changed', { detail:detail }));\n";
        js += L"        }\n";
        js += L"    } catch (_) {}\n";
        js += L"\n";
        js += L"    try {\n";
        js += L"        if (window.parent &&\n";
        js += L"            window.parent !== window &&\n";
        js += L"            typeof window.parent.__epgPortalQueueReservationUiSync === 'function') {\n";
        js += L"            window.parent.__epgPortalQueueReservationUiSync('lite-reservation-list-' + String(reason || 'change'), true);\n";
        js += L"        }\n";
        js += L"    } catch (_) {}\n";
        js += L"}\n";
        js += L"\n";

        js += L"function setStatus(message, isError) {\n";
        js += L"    const el = document.getElementById('reservationStatus');\n";
        js += L"    if (!el) return;\n";
        js += L"    el.textContent = textValue(message);\n";
        js += L"    if (isError) el.classList.add('error');\n";
        js += L"    else el.classList.remove('error');\n";
        js += L"}\n";
        js += L"\n";

        js += L"function reloadDataScriptSync() {\n";
        js += L"    const cacheEntries = loadReservationViewCacheEntriesSync();\n";
        js += L"    if (Array.isArray(cacheEntries)) {\n";
        js += L"        gItems = cacheEntries;\n";
        js += L"        renderReservationList();\n";
        js += L"        return true;\n";
        js += L"    }\n";
        js += L"\n";
        js += L"    try {\n";
        js += L"        const xhr = new XMLHttpRequest();\n";
        js += L"        const sep = DATA_FILE.indexOf('?') >= 0 ? '&' : '?';\n";
        js += L"        xhr.open('GET', DATA_FILE + sep + 'v=' + String(Date.now()), false);\n";
        js += L"        xhr.send(null);\n";
        js += L"\n";
        js += L"        if (xhr.status < 200 || xhr.status >= 300) {\n";
        js += L"            return false;\n";
        js += L"        }\n";
        js += L"\n";
        js += L"        const fn = new Function(xhr.responseText || '');\n";
        js += L"        fn.call(window);\n";
        js += L"        gItems = normalizeAllEntries();\n";
        js += L"        renderReservationList();\n";
        js += L"        return true;\n";
        js += L"    } catch (_) {\n";
        js += L"        return false;\n";
        js += L"    }\n";
        js += L"}\n";

        js += L"function scheduleDataReload() {\n";
        js += L"    try {\n";
        js += L"        window.setTimeout(function () {\n";
        js += L"            if (reloadDataScriptSync()) {\n";
        js += L"                setStatus('予約データを再読込しました', false);\n";
        js += L"            }\n";
        js += L"        }, 250);\n";
        js += L"    } catch (_) {}\n";
        js += L"}\n";
    }

    static void AppendReservationListScript_RenderRows_(
        std::wstring& js)
    {
        js += L"function makeActionButton(label, className, disabled, onClick) {\n";
        js += L"    const btn = makeElement('button', 'reserve-action ' + className, label);\n";
        js += L"    btn.type = 'button';\n";
        js += L"    btn.disabled = !!disabled;\n";
        js += L"    btn.addEventListener('click', function (ev) {\n";
        js += L"        ev.preventDefault();\n";
        js += L"        ev.stopPropagation();\n";
        js += L"        if (!btn.disabled) onClick();\n";
        js += L"    });\n";
        js += L"    return btn;\n";
        js += L"}\n";
        js += L"\n";

        js += L"function closeExistingDetailRows() {\n";
        js += L"    document.querySelectorAll('tr.reserve-detail-row').forEach(function (row) {\n";
        js += L"        row.remove();\n";
        js += L"    });\n";
        js += L"    document.querySelectorAll('tr.reserve-row.detail-open').forEach(function (row) {\n";
        js += L"        row.classList.remove('detail-open');\n";
        js += L"    });\n";
        js += L"}\n";
        js += L"\n";

        js += L"function appendDetailLine(parent, label, value) {\n";
        js += L"    if (!value) return;\n";
        js += L"    const div = makeElement('div', 'reserve-detail-line', '');\n";
        js += L"    div.appendChild(makeElement('span', 'reserve-detail-key', label));\n";
        js += L"    div.appendChild(makeElement('span', 'reserve-detail-value', value));\n";
        js += L"    parent.appendChild(div);\n";
        js += L"}\n";
        js += L"\n";

        js += L"function toggleDetailRow(baseRow, item) {\n";
        js += L"    if (!baseRow || !item) return;\n";
        js += L"\n";
        js += L"    if (baseRow.classList.contains('detail-open')) {\n";
        js += L"        closeExistingDetailRows();\n";
        js += L"        return;\n";
        js += L"    }\n";
        js += L"\n";
        js += L"    closeExistingDetailRows();\n";
        js += L"    baseRow.classList.add('detail-open');\n";
        js += L"\n";
        js += L"    const detailRow = document.createElement('tr');\n";
        js += L"    detailRow.className = 'reserve-detail-row';\n";
        js += L"\n";
        js += L"    const td = document.createElement('td');\n";
        js += L"    td.colSpan = 5;\n";
        js += L"\n";
        js += L"    const box = makeElement('div', 'reserve-detail-box', '');\n";
        js += L"    appendDetailLine(box, 'タイトル', item.label);\n";
        js += L"    appendDetailLine(box, '概要', item.summary);\n";
        js += L"    appendDetailLine(box, 'ジャンル', item.genre);\n";
        js += L"    appendDetailLine(box, 'ID', item.id);\n";
        js += L"    appendDetailLine(box, 'DedupeKey', item.dedupeKey);\n";
        js += L"    appendDetailLine(box, 'SID', item.serviceId > 0 ? String(item.serviceId) : '');\n";
        js += L"    appendDetailLine(box, 'EID', item.eventId > 0 ? String(item.eventId) : '');\n";
        js += L"    appendDetailLine(box, 'ONID', item.networkId > 0 ? String(item.networkId) : '');\n";
        js += L"    appendDetailLine(box, 'TSID', item.tsId > 0 ? String(item.tsId) : '');\n";
        js += L"    appendDetailLine(box, 'Source', item.reserveSource);\n";
        js += L"\n";
        js += L"    td.appendChild(box);\n";
        js += L"    detailRow.appendChild(td);\n";
        js += L"\n";
        js += L"    baseRow.parentNode.insertBefore(detailRow, baseRow.nextSibling);\n";
        js += L"}\n";
        js += L"\n";

        js += L"function renderRow(item) {\n";
        js += L"    const tr = document.createElement('tr');\n";
        js += L"    tr.className = 'reserve-row reserve-row-' + entryStatus(item);\n";
        js += L"    if (item.fixed) tr.classList.add('reserve-row-fixed');\n";
        js += L"    tr.dataset.reserveId = item.id;\n";
        js += L"\n";
        js += L"    const tdState = document.createElement('td');\n";
        js += L"    tdState.appendChild(makeBadge(item));\n";
        js += L"    tdState.appendChild(document.createElement('br'));\n";
        js += L"    tdState.appendChild(makeModeBadge(item));\n";
        js += L"    if (item.fixed) {\n";
        js += L"        tdState.appendChild(document.createElement('br'));\n";
        js += L"        tdState.appendChild(makeElement('span', 'reserve-fixed-label', '固定'));\n";
        js += L"    }\n";
        js += L"    tr.appendChild(tdState);\n";
        js += L"\n";
        js += L"    const tdTime = document.createElement('td');\n";
        js += L"    tdTime.appendChild(makeElement('div', 'reserve-time-main', makeTimeText(item)));\n";
        js += L"    tr.appendChild(tdTime);\n";
        js += L"\n";
        js += L"    const tdService = document.createElement('td');\n";
        js += L"    tdService.appendChild(makeElement('div', 'reserve-service', item.service || '-'));\n";
        js += L"    tr.appendChild(tdService);\n";
        js += L"\n";
        js += L"    const tdTitle = document.createElement('td');\n";
        js += L"    tdTitle.appendChild(makeElement('div', 'reserve-title compact-title', item.label || '-'));\n";
        js += L"    if (item.genre) {\n";
        js += L"        tdTitle.appendChild(makeElement('div', 'reserve-genre compact-genre', item.genre));\n";
        js += L"    }\n";
        js += L"    tr.appendChild(tdTitle);\n";
        js += L"\n";
        js += L"    const tdAction = document.createElement('td');\n";
        js += L"    const box = makeElement('div', 'reserve-action-box', '');\n";
        js += L"    const canMutate = !item.fixed && !item.deleted;\n";
        js += L"\n";
        js += L"    box.appendChild(makeActionButton(\n";
        js += L"        item.enabled ? '無効化' : '有効化',\n";
        js += L"        'toggle',\n";
        js += L"        !canMutate,\n";
        js += L"        function () { toggleReservationItem(item); }\n";
        js += L"    ));\n";
        js += L"\n";
        js += L"    box.appendChild(makeActionButton(\n";
        js += L"        '削除',\n";
        js += L"        'remove',\n";
        js += L"        !canMutate,\n";
        js += L"        function () { removeReservationItem(item); }\n";
        js += L"    ));\n";
        js += L"\n";
        js += L"    box.appendChild(makeActionButton(\n";
        js += L"        '詳細',\n";
        js += L"        'detail',\n";
        js += L"        false,\n";
        js += L"        function () { toggleDetailRow(tr, item); }\n";
        js += L"    ));\n";
        js += L"\n";
        js += L"    tdAction.appendChild(box);\n";
        js += L"    tr.appendChild(tdAction);\n";
        js += L"\n";
        js += L"    tr.addEventListener('dblclick', function () {\n";
        js += L"        toggleDetailRow(tr, item);\n";
        js += L"    });\n";
        js += L"\n";
        js += L"    return tr;\n";
        js += L"}\n";

        js += L"function renderReservationList() {\n";
        js += L"    updateFilterButtons();\n";
        js += L"\n";
        js += L"    const tbody = document.getElementById('reservationRows');\n";
        js += L"    const empty = document.getElementById('reservationEmpty');\n";
        js += L"    if (!tbody) return;\n";
        js += L"\n";
        js += L"    closeExistingDetailRows();\n";
        js += L"\n";
        js += L"    const viewItems = gItems\n";
        js += L"        .filter(entryMatchesFilter)\n";
        js += L"        .slice()\n";
        js += L"        .sort(compareEntry);\n";
        js += L"\n";
        js += L"    tbody.textContent = '';\n";
        js += L"\n";
        js += L"    const frag = document.createDocumentFragment();\n";
        js += L"    viewItems.forEach(function (item) {\n";
        js += L"        frag.appendChild(renderRow(item));\n";
        js += L"    });\n";
        js += L"    tbody.appendChild(frag);\n";
        js += L"\n";
        js += L"    if (empty) {\n";
        js += L"        empty.style.display = viewItems.length > 0 ? 'none' : '';\n";
        js += L"    }\n";
        js += L"\n";
        js += L"    updateSummary(viewItems);\n";
        js += L"}\n";

        js += L"function toggleReservationItem(item) {\n";
        js += L"    if (!item || item.fixed || item.deleted) return;\n";
        js += L"\n";
        js += L"    const nextEnabled = !item.enabled;\n";
        js += L"    const payload = makePayloadForMutation(item);\n";
        js += L"    payload.enabled = nextEnabled;\n";
        js += L"    payload.disabledByUser = nextEnabled ? false : true;\n";
        js += L"    payload.excludeFromAuto = nextEnabled ? false : true;\n";
        js += L"\n";
        js += L"    const result = postMutationSync('/set_enabled_by_id', payload);\n";
        js += L"    if (!result || result.ok === false) {\n";
        js += L"        setStatus('有効/無効の変更に失敗しました: ' + textValue(result && result.reason), true);\n";
        js += L"        return;\n";
        js += L"    }\n";
        js += L"\n";
        js += L"    item.enabled = nextEnabled;\n";
        js += L"    item.disabledByUser = !nextEnabled;\n";
        js += L"    item.excludeFromAuto = !nextEnabled;\n";
        js += L"\n";
        js += L"    renderReservationList();\n";
        js += L"    notifyParentReservationChanged('toggle-enabled');\n";
        js += L"\n";
        js += L"    if (result.source === 'local-preview') {\n";
        js += L"        setStatus('ローカル表示だけ変更しました。API接続後にtasks.iniへ反映します。', false);\n";
        js += L"    } else {\n";
        js += L"        setStatus('予約状態を変更しました', false);\n";
        js += L"        scheduleDataReload();\n";
        js += L"    }\n";
        js += L"}\n";
        js += L"\n";

        js += L"function removeReservationItem(item) {\n";
        js += L"    if (!item || item.fixed || item.deleted) return;\n";
        js += L"\n";
        js += L"    const message = '予約を削除しますか？\\n' + item.label;\n";
        js += L"    if (!window.confirm(message)) return;\n";
        js += L"\n";
        js += L"    const payload = makePayloadForMutation(item);\n";
        js += L"    const result = postMutationSync('/remove_by_id', payload);\n";
        js += L"    if (!result || result.ok === false) {\n";
        js += L"        setStatus('予約削除に失敗しました: ' + textValue(result && result.reason), true);\n";
        js += L"        return;\n";
        js += L"    }\n";
        js += L"\n";
        js += L"    item.deleted = true;\n";
        js += L"    renderReservationList();\n";
        js += L"    notifyParentReservationChanged('remove');\n";
        js += L"\n";
        js += L"    if (result.source === 'local-preview') {\n";
        js += L"        setStatus('ローカル表示だけ削除済みにしました。API接続後にtasks.iniへ反映します。', false);\n";
        js += L"    } else {\n";
        js += L"        setStatus('予約を削除しました', false);\n";
        js += L"        scheduleDataReload();\n";
        js += L"    }\n";
        js += L"}\n";
    }

    static void AppendReservationListScript_Boot_(
        std::wstring& js)
    {
        js += L"function bindControls() {\n";
        js += L"    document.querySelectorAll('[data-reserve-filter]').forEach(function (btn) {\n";
        js += L"        btn.addEventListener('click', function () {\n";
        js += L"            gFilter = btn.getAttribute('data-reserve-filter') || 'all';\n";
        js += L"            renderReservationList();\n";
        js += L"        });\n";
        js += L"    });\n";
        js += L"\n";
        js += L"    const search = document.getElementById('reservationSearch');\n";
        js += L"    if (search) {\n";
        js += L"        search.addEventListener('input', function () {\n";
        js += L"            gSearchText = search.value || '';\n";
        js += L"            renderReservationList();\n";
        js += L"        });\n";
        js += L"    }\n";
        js += L"\n";
        js += L"    const reload = document.getElementById('reservationReload');\n";
        js += L"    if (reload) {\n";
        js += L"        reload.addEventListener('click', function () {\n";
        js += L"            if (reloadDataScriptSync()) {\n";
        js += L"                setStatus('予約データを再読込しました', false);\n";
        js += L"            } else {\n";
        js += L"                gItems = normalizeAllEntries();\n";
        js += L"                renderReservationList();\n";
        js += L"                setStatus('現在読み込み済みの予約データで再描写しました', false);\n";
        js += L"            }\n";
        js += L"        });\n";
        js += L"    }\n";
        js += L"}\n";
        js += L"\n";

        js += L"document.addEventListener('DOMContentLoaded', function () {\n";
        js += L"    bindControls();\n";
        js += L"    gItems = normalizeAllEntries();\n";
        js += L"    renderReservationList();\n";
        js += L"\n";
        js += L"    const apiBase = getApiBase();\n";
        js += L"    const sourceText = gDataSource === 'view-cache'\n";
        js += L"        ? '予約Viewキャッシュ'\n";
        js += L"        : gDataSource;\n";
        js += L"    if (apiBase) {\n";
        js += L"        setStatus('予約データを読み込みました / source=' + sourceText + ' / API接続先: ' + apiBase, false);\n";
        js += L"    } else {\n";
        js += L"        setStatus('予約データを読み込みました / source=' + sourceText + ' / API未接続', false);\n";
        js += L"    }\n";
        js += L"});\n";
        js += L"\n";

        js += L"window.__epgLiteReservationListReload = function () {\n";
        js += L"    return reloadDataScriptSync();\n";
        js += L"};\n";
        js += L"\n";

        js += L"})();\n";
    }

    std::wstring BuildReservationListScript()
    {
        std::wstring js;
        js.reserve(64 * 1024);

        AppendReservationListScript_Core_(js);
        AppendReservationListScript_FilterRender_(js);
        AppendReservationListScript_Api_(js);
        AppendReservationListScript_RenderRows_(js);
        AppendReservationListScript_Boot_(js);

        return js;
    }

    void AppendReservationPageCss_(
        std::wostringstream& oss)
    {
        oss << L".page-section{\n";
        oss << L"    background:var(--lite-panel2);\n";
        oss << L"    border:1px solid var(--lite-line);\n";
        oss << L"    border-radius:12px;\n";
        oss << L"    padding:14px 16px;\n";
        oss << L"    margin-bottom:14px;\n";
        oss << L"}\n";

        oss << L".reserve-toolbar{\n";
        oss << L"    display:flex;\n";
        oss << L"    flex-wrap:wrap;\n";
        oss << L"    align-items:center;\n";
        oss << L"    gap:8px;\n";
        oss << L"    margin-bottom:12px;\n";
        oss << L"}\n";

        oss << L".reserve-filter-btn,\n";
        oss << L".reserve-reload-btn{\n";
        oss << L"    appearance:none;\n";
        oss << L"    border:1px solid #41506a;\n";
        oss << L"    background:#1a2330;\n";
        oss << L"    color:var(--lite-text);\n";
        oss << L"    border-radius:999px;\n";
        oss << L"    padding:6px 11px;\n";
        oss << L"    font-size:12px;\n";
        oss << L"    font-weight:700;\n";
        oss << L"    cursor:pointer;\n";
        oss << L"}\n";

        oss << L".reserve-filter-btn.active{\n";
        oss << L"    background:#1d4d7f;\n";
        oss << L"    border-color:#6bb7ff;\n";
        oss << L"}\n";

        oss << L".reserve-search{\n";
        oss << L"    min-width:220px;\n";
        oss << L"    max-width:420px;\n";
        oss << L"    flex:1 1 260px;\n";
        oss << L"    border:1px solid #41506a;\n";
        oss << L"    background:#0f141c;\n";
        oss << L"    color:var(--lite-text);\n";
        oss << L"    border-radius:9px;\n";
        oss << L"    padding:7px 10px;\n";
        oss << L"    font-size:13px;\n";
        oss << L"}\n";

        oss << L".reserve-summary-grid{\n";
        oss << L"    display:grid;\n";
        oss << L"    grid-template-columns:repeat(auto-fit,minmax(120px,1fr));\n";
        oss << L"    gap:8px;\n";
        oss << L"    margin-bottom:12px;\n";
        oss << L"}\n";

        oss << L".reserve-summary-card{\n";
        oss << L"    border:1px solid var(--lite-line);\n";
        oss << L"    background:rgba(255,255,255,0.03);\n";
        oss << L"    border-radius:10px;\n";
        oss << L"    padding:9px 10px;\n";
        oss << L"}\n";

        oss << L".reserve-summary-card .label{\n";
        oss << L"    color:var(--lite-sub);\n";
        oss << L"    font-size:11px;\n";
        oss << L"}\n";

        oss << L".reserve-summary-card .value{\n";
        oss << L"    color:var(--lite-text);\n";
        oss << L"    font-size:20px;\n";
        oss << L"    font-weight:800;\n";
        oss << L"    margin-top:3px;\n";
        oss << L"}\n";

        oss << L".reserve-status{\n";
        oss << L"    color:var(--lite-sub);\n";
        oss << L"    font-size:12px;\n";
        oss << L"    margin:8px 0 0;\n";
        oss << L"}\n";

        oss << L".reserve-status.error{\n";
        oss << L"    color:#ffb0b0;\n";
        oss << L"}\n";

        oss << L".reserve-table-wrap{\n";
        oss << L"    overflow:auto;\n";
        oss << L"    border:1px solid var(--lite-line);\n";
        oss << L"    border-radius:12px;\n";
        oss << L"}\n";

        oss << L".reserve-table{\n";
        oss << L"    width:100%;\n";
        oss << L"    border-collapse:collapse;\n";
        oss << L"    min-width:920px;\n";
        oss << L"}\n";

        oss << L".reserve-table th,\n";
        oss << L".reserve-table td{\n";
        oss << L"    padding:9px 8px;\n";
        oss << L"    border-top:1px solid var(--lite-line);\n";
        oss << L"    text-align:left;\n";
        oss << L"    vertical-align:top;\n";
        oss << L"    font-size:13px;\n";
        oss << L"}\n";

        oss << L".reserve-table thead th{\n";
        oss << L"    position:sticky;\n";
        oss << L"    top:0;\n";
        oss << L"    z-index:2;\n";
        oss << L"    background:var(--lite-panel);\n";
        oss << L"    color:var(--lite-text);\n";
        oss << L"    font-weight:800;\n";
        oss << L"}\n";

        oss << L".reserve-row:nth-child(even){\n";
        oss << L"    background:rgba(255,255,255,0.02);\n";
        oss << L"}\n";

        oss << L".reserve-row-disabled{\n";
        oss << L"    opacity:0.72;\n";
        oss << L"}\n";

        oss << L".reserve-row-deleted{\n";
        oss << L"    opacity:0.48;\n";
        oss << L"    text-decoration:line-through;\n";
        oss << L"}\n";

        oss << L".reserve-row-fixed{\n";
        oss << L"    background:rgba(80,90,110,0.12);\n";
        oss << L"}\n";

        oss << L".reserve-badge,\n";
        oss << L".reserve-mode,\n";
        oss << L".reserve-fixed-label{\n";
        oss << L"    display:inline-block;\n";
        oss << L"    padding:3px 8px;\n";
        oss << L"    border-radius:999px;\n";
        oss << L"    font-size:11px;\n";
        oss << L"    font-weight:800;\n";
        oss << L"    margin:0 4px 4px 0;\n";
        oss << L"}\n";

        oss << L".reserve-badge-enabled{\n";
        oss << L"    background:#124e35;\n";
        oss << L"    color:#dff8ea;\n";
        oss << L"}\n";

        oss << L".reserve-badge-disabled{\n";
        oss << L"    background:#5f4a17;\n";
        oss << L"    color:#fff0cb;\n";
        oss << L"}\n";

        oss << L".reserve-badge-deleted{\n";
        oss << L"    background:#4a2020;\n";
        oss << L"    color:#ffd8d8;\n";
        oss << L"}\n";

        oss << L".reserve-mode{\n";
        oss << L"    background:#22344d;\n";
        oss << L"    color:#dcecff;\n";
        oss << L"}\n";

        oss << L".reserve-fixed-label{\n";
        oss << L"    background:#3a3d45;\n";
        oss << L"    color:#d8dde8;\n";
        oss << L"}\n";

        oss << L".reserve-title{\n";
        oss << L"    font-weight:800;\n";
        oss << L"    margin-bottom:4px;\n";
        oss << L"}\n";

        oss << L".reserve-service,\n";
        oss << L".reserve-time-main{\n";
        oss << L"    font-weight:700;\n";
        oss << L"}\n";

        oss << L".reserve-sub,\n";
        oss << L".reserve-meta-line{\n";
        oss << L"    color:var(--lite-sub);\n";
        oss << L"    font-size:12px;\n";
        oss << L"    line-height:1.45;\n";
        oss << L"}\n";

        oss << L".reserve-genre{\n";
        oss << L"    color:#76e0a6;\n";
        oss << L"    font-size:12px;\n";
        oss << L"    margin-top:3px;\n";
        oss << L"}\n";

        oss << L".reserve-meta-key{\n";
        oss << L"    color:#90a1bc;\n";
        oss << L"    margin-right:5px;\n";
        oss << L"}\n";

        oss << L".reserve-action-box{\n";
        oss << L"    display:flex;\n";
        oss << L"    flex-wrap:wrap;\n";
        oss << L"    gap:7px;\n";
        oss << L"}\n";

        oss << L".reserve-action{\n";
        oss << L"    appearance:none;\n";
        oss << L"    border:1px solid #41506a;\n";
        oss << L"    background:#253041;\n";
        oss << L"    color:var(--lite-text);\n";
        oss << L"    border-radius:999px;\n";
        oss << L"    padding:6px 11px;\n";
        oss << L"    font-size:12px;\n";
        oss << L"    font-weight:800;\n";
        oss << L"    cursor:pointer;\n";
        oss << L"}\n";

        oss << L".reserve-action.remove{\n";
        oss << L"    border-color:#925050;\n";
        oss << L"    background:#4a2525;\n";
        oss << L"}\n";

        oss << L".reserve-action:disabled{\n";
        oss << L"    opacity:0.42;\n";
        oss << L"    cursor:default;\n";
        oss << L"}\n";

        oss << L".reserve-empty{\n";
        oss << L"    padding:18px;\n";
        oss << L"    color:var(--lite-sub);\n";
        oss << L"    font-size:13px;\n";
        oss << L"}\n";

        oss << L".compact-title{\n";
        oss << L"    max-width:720px;\n";
        oss << L"    overflow:hidden;\n";
        oss << L"    text-overflow:ellipsis;\n";
        oss << L"    white-space:nowrap;\n";
        oss << L"}\n";

        oss << L".compact-genre{\n";
        oss << L"    white-space:nowrap;\n";
        oss << L"    overflow:hidden;\n";
        oss << L"    text-overflow:ellipsis;\n";
        oss << L"}\n";

        oss << L".reserve-action.detail{\n";
        oss << L"    border-color:#52698d;\n";
        oss << L"    background:#1f344d;\n";
        oss << L"}\n";

        oss << L".reserve-row.detail-open{\n";
        oss << L"    background:rgba(77,163,255,0.08);\n";
        oss << L"}\n";

        oss << L".reserve-detail-row td{\n";
        oss << L"    background:rgba(0,0,0,0.18);\n";
        oss << L"    border-top:1px solid #3c4a60;\n";
        oss << L"}\n";

        oss << L".reserve-detail-box{\n";
        oss << L"    display:grid;\n";
        oss << L"    grid-template-columns:repeat(auto-fit,minmax(240px,1fr));\n";
        oss << L"    gap:6px 14px;\n";
        oss << L"    padding:8px 10px;\n";
        oss << L"}\n";

        oss << L".reserve-detail-line{\n";
        oss << L"    color:var(--lite-sub);\n";
        oss << L"    font-size:12px;\n";
        oss << L"    line-height:1.55;\n";
        oss << L"    overflow-wrap:anywhere;\n";
        oss << L"}\n";

        oss << L".reserve-detail-key{\n";
        oss << L"    display:inline-block;\n";
        oss << L"    min-width:72px;\n";
        oss << L"    color:#90a1bc;\n";
        oss << L"    font-weight:700;\n";
        oss << L"}\n";

        oss << L".reserve-detail-value{\n";
        oss << L"    color:var(--lite-text);\n";
        oss << L"}\n";
    }

    std::wstring BuildReservationListHtml(
        const Context& ctx)
    {
        const std::wstring pageFile =
            BuildReservationListFileName(ctx.space);

        Common::HeaderContext header{};
        header.title = L"予約リスト";
        header.subtitle =
            L"Lite Reservation List / " +
            MakeSpaceLabel(ctx.space) +
            L" / reservation_data.js 連携";

        header.mainLinks =
            Common::BuildMainMenuLinks(
                ctx.space,
                MenuId::ReservationList);

        header.showMainMenuRow = false;
        header.showGuideControlRow = false;
        header.showGenreRow = false;

        std::wostringstream oss;

        oss << L"<!DOCTYPE html>\n";
        oss << L"<html lang=\"ja\">\n";
        oss << L"<head>\n";
        oss << L"<meta charset=\"UTF-8\">\n";
        oss << L"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
        oss << L"<title>予約リスト</title>\n";
        oss << L"<style>\n";
        oss << Common::BuildCommonCss();
        AppendReservationPageCss_(oss);
        oss << L"</style>\n";
        oss << L"<script src=\"";
        oss << HtmlEscape(kReservationDataScriptFile);
        oss << L"\"></script>\n";
        oss << L"<script src=\"";
        oss << HtmlEscape(kReservationListScriptFile);
        oss << L"\" defer></script>\n";
        oss << L"</head>\n";
        oss << L"<body>\n";

        oss << Common::BuildLayoutBegin(header);

        oss << L"<section class=\"page-section\">\n";
        oss << L"<div class=\"reserve-summary-grid\">\n";

        oss << L"<div class=\"reserve-summary-card\"><div class=\"label\">全予約</div><div class=\"value\" id=\"reserveCountAll\">0</div></div>\n";
        oss << L"<div class=\"reserve-summary-card\"><div class=\"label\">表示中</div><div class=\"value\" id=\"reserveCountView\">0</div></div>\n";
        oss << L"<div class=\"reserve-summary-card\"><div class=\"label\">有効</div><div class=\"value\" id=\"reserveCountEnabled\">0</div></div>\n";
        oss << L"<div class=\"reserve-summary-card\"><div class=\"label\">無効</div><div class=\"value\" id=\"reserveCountDisabled\">0</div></div>\n";
        oss << L"<div class=\"reserve-summary-card\"><div class=\"label\">録画</div><div class=\"value\" id=\"reserveCountRecord\">0</div></div>\n";
        oss << L"<div class=\"reserve-summary-card\"><div class=\"label\">視聴</div><div class=\"value\" id=\"reserveCountWatch\">0</div></div>\n";
        oss << L"<div class=\"reserve-summary-card\"><div class=\"label\">EPG</div><div class=\"value\" id=\"reserveCountEpg\">0</div></div>\n";

        oss << L"</div>\n";

        oss << L"<div class=\"reserve-toolbar\">\n";
        oss << L"<button type=\"button\" class=\"reserve-filter-btn active\" data-reserve-filter=\"all\">すべて</button>\n";
        oss << L"<button type=\"button\" class=\"reserve-filter-btn\" data-reserve-filter=\"enabled\">有効</button>\n";
        oss << L"<button type=\"button\" class=\"reserve-filter-btn\" data-reserve-filter=\"disabled\">無効</button>\n";
        oss << L"<button type=\"button\" class=\"reserve-filter-btn\" data-reserve-filter=\"record\">録画</button>\n";
        oss << L"<button type=\"button\" class=\"reserve-filter-btn\" data-reserve-filter=\"watch\">視聴</button>\n";
        oss << L"<button type=\"button\" class=\"reserve-filter-btn\" data-reserve-filter=\"epg\">EPG</button>\n";
        oss << L"<button type=\"button\" class=\"reserve-filter-btn\" data-reserve-filter=\"other\">その他</button>\n";
        oss << L"<button type=\"button\" class=\"reserve-filter-btn\" data-reserve-filter=\"deleted\">削除済み</button>\n";
        oss << L"<input id=\"reservationSearch\" class=\"reserve-search\" type=\"search\" placeholder=\"番組名・放送局・ID検索\">\n";
        oss << L"<button type=\"button\" id=\"reservationReload\" class=\"reserve-reload-btn\">再読込</button>\n";
        oss << L"</div>\n";

        oss << L"<div class=\"reserve-status\" id=\"reservationStatus\">予約データ読込中...</div>\n";

        oss << L"</section>\n";

        oss << L"<section class=\"page-section\">\n";
        oss << L"<div class=\"reserve-table-wrap\">\n";
        oss << L"<table class=\"reserve-table\" id=\"reservationTable\">\n";
        oss << L"<thead>\n";
        oss << L"<tr>\n";
        oss << L"<th>状態</th>\n";
        oss << L"<th>日時</th>\n";
        oss << L"<th>サービス</th>\n";
        oss << L"<th>番組情報</th>\n";
        oss << L"<th>操作</th>\n";
        oss << L"</tr>\n";
        oss << L"</thead>\n";
        oss << L"<tbody id=\"reservationRows\"></tbody>\n";
        oss << L"</table>\n";
        oss << L"<div class=\"reserve-empty\" id=\"reservationEmpty\" style=\"display:none;\">\n";
        oss << L"表示できる予約がありません。reservation_data.js が未生成、または条件に一致する予約がありません。\n";
        oss << L"</div>\n";
        oss << L"</div>\n";
        oss << L"</section>\n";

        oss << L"<section class=\"page-section\">\n";
        oss << L"<div class=\"reserve-sub\">\n";
        oss << L"出力ファイル: ";
        oss << HtmlEscape(pageFile);
        oss << L"<br>\n";
        oss << L"参照データ: /cache/reservation/view/tasks_view_index.tsv 優先";
        oss << L"<br>\n";
        oss << L"予備データ: ";
        oss << HtmlEscape(kReservationDataScriptFile);
        oss << L"<br>\n";
        oss << L"API接続先: /api/reservation。操作時は /set_enabled_by_id と /remove_by_id へPOSTします。\n";
        oss << L"</div>\n";
        oss << L"</section>\n";

        oss << Common::BuildLayoutEnd();

        oss << L"</body>\n";
        oss << L"</html>\n";

        return oss.str();
    }
}

namespace EpgHtmlLite::PageReservationList
{
    bool Generate(
        const Context& ctx,
        Result& ioResult)
    {
        try {
            const std::filesystem::path htmlPath =
                ctx.outputDir /
                std::filesystem::path(
                    BuildReservationListFileName(ctx.space));

            const std::filesystem::path jsPath =
                ctx.outputDir /
                std::filesystem::path(
                    kReservationListScriptFile);

            const std::filesystem::path dataPath =
                ctx.outputDir /
                std::filesystem::path(
                    kReservationDataScriptFile);

            const std::wstring html =
                BuildReservationListHtml(ctx);

            if (!WriteUtf8BomTextFile(
                htmlPath,
                html)) {
                return false;
            }

            const std::wstring js =
                BuildReservationListScript();

            if (!WriteUtf8BomTextFile(
                jsPath,
                js)) {
                return false;
            }

            ioResult.generatedFiles.push_back(
                htmlPath.wstring());

            ioResult.generatedFiles.push_back(
                jsPath.wstring());

            std::error_code ec;
            const bool hasReservationData =
                std::filesystem::exists(
                    dataPath,
                    ec);

            if (!hasReservationData) {
                const std::wstring emptyData =
                    BuildEmptyReservationDataScript();

                if (!WriteUtf8BomTextFile(
                    dataPath,
                    emptyData)) {
                    return false;
                }

                ioResult.generatedFiles.push_back(
                    dataPath.wstring());
            }

            return true;
        }
        catch (...) {
            return false;
        }
    }
}