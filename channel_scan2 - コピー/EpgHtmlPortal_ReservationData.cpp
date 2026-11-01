#include "EpgHtmlPortal_ReservationData.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "DebugManager.h"
#include "EpgHtmlPortal_Common.hpp"
#include "TaskKinds.hpp"
#include "TaskListManager.hpp"
#include "TaskSystemConnector.hpp"

namespace EpgHtmlPortal_ReservationData
{
    namespace
    {
        struct ReservationEntry
        {
            std::wstring id;
            std::wstring label;
            std::wstring category;
            std::wstring mode;

            std::wstring dateText;
            std::wstring weekdayText;
            std::wstring timeText;
            std::wstring endDateText;
            std::wstring endTimeText;

            std::wstring serviceName;
            std::wstring targetText;
            std::wstring scheduleText;
            std::wstring note;
            std::wstring summary;
            std::wstring genre;
            std::wstring reserveSource;
            std::wstring dedupeKey;
            std::wstring excludeKey;

            int space = -1;
            int channel = -1;
            int kind = 0;
            int instance = 0;

            int preferredTunerIndex = -1;
            int requiredTuners = 1;

            std::uint16_t networkId = 0;
            std::uint16_t tsId = 0;
            std::uint16_t serviceId = 0;
            std::uint16_t eventId = 0;

            bool enabled = true;
            bool disabledByUser = false;
            bool excludeFromAuto = false;
            bool autoReserved = false;
        };

        std::wstring GetMapValue(
            const std::map<std::wstring, std::wstring>& values,
            std::initializer_list<const wchar_t*> keys)
        {
            for (const auto* key : keys) {
                const auto it = values.find(
                    EpgHtmlPortal_Common::ToLowerAsciiW(std::wstring(key)));
                if (it != values.end() && !it->second.empty()) {
                    return it->second;
                }
            }
            return {};
        }

        bool ParseBoolLoose(std::wstring value, bool def = false)
        {
            value = EpgHtmlPortal_Common::ToLowerAsciiW(
                EpgHtmlPortal_Common::TrimW(std::move(value)));

            if (value == L"1" || value == L"true" || value == L"yes" || value == L"on") {
                return true;
            }
            if (value == L"0" || value == L"false" || value == L"no" || value == L"off") {
                return false;
            }
            return def;
        }

        int ParseIntLoose(std::wstring value, int def = 0)
        {
            value = EpgHtmlPortal_Common::TrimW(std::move(value));
            if (value.empty()) {
                return def;
            }

            try {
                std::size_t idx = 0;
                const int base =
                    (value.size() > 2 &&
                        value[0] == L'0' &&
                        (value[1] == L'x' || value[1] == L'X')) ? 16 : 10;

                const int out = std::stoi(value, &idx, base);
                return idx > 0 ? out : def;
            }
            catch (...) {
                return def;
            }
        }

        std::wstring ResolveTaskLabel(const std::map<std::wstring, std::wstring>& values)
        {
            std::wstring label = GetMapValue(
                values,
                { L"titlelabel", L"label", L"name", L"title", L"programtitle", L"program", L"taskname" });

            if (!label.empty()) {
                return label;
            }

            const std::wstring service = GetMapValue(
                values,
                { L"servicename", L"service", L"channelname" });

            const std::wstring note = GetMapValue(
                values,
                { L"note", L"summary", L"description" });

            if (!service.empty() && !note.empty()) {
                return service + L" / " + note;
            }
            if (!service.empty()) {
                return service;
            }
            return L"予約項目";
        }

        std::wstring InferReservationMode(const std::map<std::wstring, std::wstring>& values)
        {
            const std::wstring explicitMode =
                EpgHtmlPortal_Common::ToLowerAsciiW(
                    GetMapValue(values, { L"mode", L"reservationmode", L"reservemode", L"category" }));

            if (explicitMode.find(L"epg") != std::wstring::npos ||
                explicitMode.find(L"eit") != std::wstring::npos ||
                explicitMode.find(L"取得") != std::wstring::npos) {
                return L"epg";
            }

            if (explicitMode.find(L"record") != std::wstring::npos ||
                explicitMode.find(L"録画") != std::wstring::npos) {
                return L"record";
            }

            if (explicitMode.find(L"watch") != std::wstring::npos ||
                explicitMode.find(L"view") != std::wstring::npos ||
                explicitMode.find(L"視聴") != std::wstring::npos) {
                return L"watch";
            }

            const std::wstring joined =
                EpgHtmlPortal_Common::ToLowerAsciiW(
                    GetMapValue(values, { L"label", L"name", L"title" }) + L" " +
                    GetMapValue(values, { L"kind", L"taskkind", L"category" }) + L" " +
                    GetMapValue(values, { L"note", L"summary", L"description" }));

            if (joined.find(L"epg") != std::wstring::npos ||
                joined.find(L"eit") != std::wstring::npos) {
                return L"epg";
            }

            if (joined.find(L"record") != std::wstring::npos ||
                joined.find(L"録画") != std::wstring::npos ||
                joined.find(L"recstart") != std::wstring::npos) {
                return L"record";
            }

            if (joined.find(L"watch") != std::wstring::npos ||
                joined.find(L"view") != std::wstring::npos ||
                joined.find(L"視聴") != std::wstring::npos) {
                return L"watch";
            }

            return L"reserve";
        }

        std::wstring FormatDueDate(int dueYmd)
        {
            if (dueYmd <= 0) {
                return {};
            }

            const int year = dueYmd / 10000;
            const int month = (dueYmd / 100) % 100;
            const int day = dueYmd % 100;

            std::wostringstream oss;
            oss << year << L"-";
            if (month < 10) oss << L'0';
            oss << month << L"-";
            if (day < 10) oss << L'0';
            oss << day;
            return oss.str();
        }

        std::wstring FormatDueTime(int dueHHMM)
        {
            if (dueHHMM < 0) {
                return {};
            }

            const int h = (dueHHMM / 100) % 100;
            const int m = dueHHMM % 100;

            std::wostringstream oss;
            if (h < 10) oss << L'0';
            oss << h << L":";
            if (m < 10) oss << L'0';
            oss << m;
            return oss.str();
        }

        bool ParseTaskSectionKindInstance(
            const std::wstring& sectionName,
            int& kind,
            int& instance)
        {
            kind = 0;
            instance = 0;

            unsigned k = 0;
            unsigned i = 0;
            if (swscanf_s(sectionName.c_str(), L"Task_%u_%u", &k, &i) == 2) {
                kind = static_cast<int>(k);
                instance = static_cast<int>(i);
                return true;
            }
            return false;
        }

        bool IsEpgTaskKind_(std::uint32_t kind) noexcept
        {
            return kind == TaskKinds::EpgFull || kind == TaskKinds::EpgLight;
        }

        bool IsRecordTaskKind_(std::uint32_t kind) noexcept
        {
            return kind == TaskKinds::RecPrepare
                || kind == TaskKinds::RecStart
                || kind == TaskKinds::RecStop;
        }

        std::wstring NormalizeMode_(std::wstring mode)
        {
            mode = EpgHtmlPortal_Common::ToLowerAsciiW(
                EpgHtmlPortal_Common::TrimW(std::move(mode)));

            if (mode == L"watch") {
                return L"watch";
            }
            return L"record";
        }

        int GetReservationDisplayPriority_(const ReservationEntry& e) noexcept
        {
            switch (static_cast<std::uint32_t>(e.kind)) {
            case TaskKinds::RecStart:
                return 300;
            case TaskKinds::RecPrepare:
                return 200;
            case TaskKinds::RecStop:
                return 100;
            default:
                return 0;
            }
        }

        void MergeReservationEntryMissingFields_(
            ReservationEntry& dst,
            const ReservationEntry& src)
        {
            auto mergeW = [](std::wstring& d, const std::wstring& s) {
                if (d.empty() && !s.empty()) d = s;
                };
            auto mergeI = [](auto& d, const auto& s, const auto& emptyValue) {
                if (d == emptyValue && s != emptyValue) d = s;
                };

            mergeW(dst.id, src.id);
            mergeW(dst.label, src.label);
            mergeW(dst.category, src.category);
            mergeW(dst.mode, src.mode);

            mergeW(dst.dateText, src.dateText);
            mergeW(dst.weekdayText, src.weekdayText);
            mergeW(dst.timeText, src.timeText);
            mergeW(dst.endDateText, src.endDateText);
            mergeW(dst.endTimeText, src.endTimeText);

            mergeW(dst.serviceName, src.serviceName);
            mergeW(dst.targetText, src.targetText);
            mergeW(dst.scheduleText, src.scheduleText);
            mergeW(dst.note, src.note);
            mergeW(dst.summary, src.summary);
            mergeW(dst.genre, src.genre);
            mergeW(dst.reserveSource, src.reserveSource);
            mergeW(dst.dedupeKey, src.dedupeKey);
            mergeW(dst.excludeKey, src.excludeKey);

            mergeI(dst.space, src.space, -1);
            mergeI(dst.channel, src.channel, -1);
            mergeI(dst.kind, src.kind, 0);
            mergeI(dst.instance, src.instance, 0);

            mergeI(dst.preferredTunerIndex, src.preferredTunerIndex, -1);
            mergeI(dst.requiredTuners, src.requiredTuners, 1);

            mergeI(dst.networkId, src.networkId, static_cast<std::uint16_t>(0));
            mergeI(dst.tsId, src.tsId, static_cast<std::uint16_t>(0));
            mergeI(dst.serviceId, src.serviceId, static_cast<std::uint16_t>(0));
            mergeI(dst.eventId, src.eventId, static_cast<std::uint16_t>(0));

            dst.enabled = dst.enabled && src.enabled;
            dst.disabledByUser = dst.disabledByUser || src.disabledByUser;
            dst.excludeFromAuto = dst.excludeFromAuto || src.excludeFromAuto;
            dst.autoReserved = dst.autoReserved || src.autoReserved;
        }

        std::vector<ReservationEntry> CompactReservationEntries_(
            std::vector<ReservationEntry> entries)
        {
            std::vector<ReservationEntry> out;
            std::map<std::wstring, std::size_t> indexMap;

            out.reserve(entries.size());

            for (auto& entry : entries) {
                const std::wstring fixedMode = NormalizeMode_(entry.mode);
                const bool isProgramReservation =
                    !entry.dedupeKey.empty() &&
                    (fixedMode == L"watch" || fixedMode == L"record");

                const std::wstring compactKey =
                    isProgramReservation
                    ? (fixedMode + L"|" + entry.dedupeKey)
                    : entry.id;

                const auto it = indexMap.find(compactKey);
                if (it == indexMap.end()) {
                    indexMap.emplace(compactKey, out.size());
                    out.push_back(std::move(entry));
                    continue;
                }

                ReservationEntry incoming = std::move(entry);
                ReservationEntry& current = out[it->second];

                if (GetReservationDisplayPriority_(incoming) >
                    GetReservationDisplayPriority_(current)) {
                    std::swap(current, incoming);
                }

                MergeReservationEntryMissingFields_(current, incoming);
            }

            return out;
        }

        std::vector<ReservationEntry> LoadReservationEntriesFromTaskIni(
            const std::filesystem::path& taskIniPath)
        {
            std::vector<ReservationEntry> out;

            if (taskIniPath.empty() || !std::filesystem::exists(taskIniPath)) {
                return out;
            }

            std::wstring text;
            if (!EpgHtmlPortal_Common::ReadTextFileToWide(taskIniPath, text)) {
                return out;
            }

            if (text.empty()) {
                return out;
            }

            std::wistringstream iss(text);
            std::wstring line;
            std::wstring currentSection;
            std::map<std::wstring, std::wstring> currentValues;

            auto flushSection = [&]() {
                if (currentSection.empty()) {
                    return;
                }

                if (currentSection.rfind(L"Task_", 0) != 0) {
                    currentSection.clear();
                    currentValues.clear();
                    return;
                }

                const std::wstring category = GetMapValue(currentValues, { L"category", L"taskcategory" });
                const std::wstring mode = InferReservationMode(currentValues);
                const std::wstring label = ResolveTaskLabel(currentValues);
                const std::wstring title = GetMapValue(currentValues, { L"title", L"programtitle", L"program" });
                const std::wstring titleLabel = GetMapValue(currentValues, { L"titlelabel", L"label" });
                const std::wstring service = GetMapValue(currentValues, { L"servicename", L"service", L"channelname", L"channel" });
                const std::wstring target = GetMapValue(currentValues, { L"target", L"targetname" });

                const int networkId = ParseIntLoose(GetMapValue(currentValues, { L"networkid", L"network_id", L"nid" }), 0);
                const int tsId = ParseIntLoose(GetMapValue(currentValues, { L"tsid", L"ts_id" }), 0);
                const int serviceId = ParseIntLoose(GetMapValue(currentValues, { L"serviceid", L"service_id", L"sid" }), 0);
                const int eventId = ParseIntLoose(GetMapValue(currentValues, { L"eventid", L"event_id", L"eid" }), 0);
                const bool enabled = ParseBoolLoose(GetMapValue(currentValues, { L"enabled", L"isenabled" }), true);

                int kind = 0;
                int instance = 0;
                ParseTaskSectionKindInstance(currentSection, kind, instance);

                const bool hasReservationHint =
                    !service.empty() ||
                    !target.empty() ||
                    serviceId > 0 ||
                    eventId > 0 ||
                    !title.empty() ||
                    !titleLabel.empty() ||
                    label.find(L"予約") != std::wstring::npos ||
                    label.find(L"録画") != std::wstring::npos ||
                    label.find(L"視聴") != std::wstring::npos ||
                    mode == L"record" ||
                    mode == L"watch" ||
                    mode == L"epg" ||
                    EpgHtmlPortal_Common::ToLowerAsciiW(category).find(L"epg") != std::wstring::npos;

                if (!hasReservationHint) {
                    currentSection.clear();
                    currentValues.clear();
                    return;
                }

                ReservationEntry entry{};
                entry.id = currentSection;
                entry.label = !titleLabel.empty() ? titleLabel : label;
                entry.category = category;
                entry.mode = mode;
                entry.enabled = enabled;

                entry.serviceName = !service.empty() ? service : target;
                entry.targetText = target;

                entry.networkId = static_cast<std::uint16_t>((std::max)(0, networkId));
                entry.tsId = static_cast<std::uint16_t>((std::max)(0, tsId));
                entry.serviceId = static_cast<std::uint16_t>((std::max)(0, serviceId));
                entry.eventId = static_cast<std::uint16_t>((std::max)(0, eventId));

                entry.kind = kind;
                entry.instance = instance;
                entry.space = ParseIntLoose(GetMapValue(currentValues, { L"space", L"currentspace" }), -1);
                entry.channel = ParseIntLoose(GetMapValue(currentValues, { L"channel", L"currentchannel" }), -1);
                entry.preferredTunerIndex =
                    ParseIntLoose(
                        GetMapValue(
                            currentValues,
                            {
                                L"preferredtunerindex",
                                L"preferredtuner",
                                L"tunerindex",
                                L"tuner",
                                L"tunerid"
                            }),
                        -1);

                entry.requiredTuners =
                    (std::max)(
                        1,
                        ParseIntLoose(
                            GetMapValue(
                                currentValues,
                                {
                                    L"requiredtuners",
                                    L"reqtuners",
                                    L"tunercount"
                                }),
                            1));

                entry.note = GetMapValue(currentValues, { L"note", L"summary", L"description" });
                entry.summary = GetMapValue(currentValues, { L"summary", L"description" });
                entry.genre = GetMapValue(currentValues, { L"genre", L"genrename" });
                entry.reserveSource = GetMapValue(currentValues, { L"reservesource", L"source" });
                entry.dedupeKey = GetMapValue(currentValues, { L"dedupekey", L"dedupe", L"uniquekey" });
                entry.excludeKey = GetMapValue(currentValues, { L"excludekey", L"excludefromautokey", L"autoblockkey" });

                entry.disabledByUser = ParseBoolLoose(
                    GetMapValue(currentValues, { L"disabledbyuser", L"disabled_by_user" }), false);
                entry.excludeFromAuto = ParseBoolLoose(
                    GetMapValue(currentValues, { L"excludefromauto", L"exclude_from_auto" }), false);
                entry.autoReserved = ParseBoolLoose(
                    GetMapValue(currentValues, { L"autoreserved", L"auto_reserved" }), false);

                entry.scheduleText = GetMapValue(currentValues, { L"schedule", L"notetext", L"scheduletxt" });

                const int dueYmd = ParseIntLoose(GetMapValue(currentValues, { L"dueymd", L"dateymd" }), 0);
                const int dueHHMM = ParseIntLoose(GetMapValue(currentValues, { L"duehhmm", L"starthhmm" }), -1);
                const int endYmd = ParseIntLoose(GetMapValue(currentValues, { L"endymd" }), 0);
                const int endHHMM = ParseIntLoose(GetMapValue(currentValues, { L"endhhmm" }), -1);

                entry.dateText = GetMapValue(currentValues, { L"date", L"datetext", L"startdate" });
                entry.weekdayText = GetMapValue(currentValues, { L"weekday", L"weekdaytext" });
                entry.timeText = GetMapValue(currentValues, { L"time", L"timetext", L"starttime" });
                entry.endDateText = GetMapValue(currentValues, { L"enddate", L"enddatetext" });
                entry.endTimeText = GetMapValue(currentValues, { L"endtime", L"endtimetext" });

                if (entry.dateText.empty()) entry.dateText = FormatDueDate(dueYmd);
                if (entry.timeText.empty()) entry.timeText = FormatDueTime(dueHHMM);
                if (entry.endDateText.empty()) entry.endDateText = FormatDueDate(endYmd);
                if (entry.endTimeText.empty()) entry.endTimeText = FormatDueTime(endHHMM);

                if (entry.scheduleText.empty()) {
                    if (!entry.dateText.empty() && !entry.timeText.empty()) {
                        entry.scheduleText = entry.dateText + L" " + entry.timeText;
                    }
                    else if (!entry.dateText.empty()) {
                        entry.scheduleText = entry.dateText;
                    }
                    else if (!entry.timeText.empty()) {
                        entry.scheduleText = entry.timeText;
                    }
                }

                out.push_back(std::move(entry));

                currentSection.clear();
                currentValues.clear();
                };

            while (std::getline(iss, line)) {
                if (!line.empty() && line.back() == L'\r') {
                    line.pop_back();
                }

                const std::wstring trimmed = EpgHtmlPortal_Common::TrimW(line);
                if (trimmed.empty() || trimmed[0] == L';' || trimmed[0] == L'#') {
                    continue;
                }

                if (trimmed.front() == L'[' && trimmed.back() == L']') {
                    flushSection();
                    currentSection = EpgHtmlPortal_Common::TrimW(
                        trimmed.substr(1, trimmed.size() - 2));
                    continue;
                }

                const std::size_t eq = trimmed.find(L'=');
                if (eq == std::wstring::npos) {
                    continue;
                }

                currentValues[
                    EpgHtmlPortal_Common::ToLowerAsciiW(
                        EpgHtmlPortal_Common::TrimW(trimmed.substr(0, eq)))] =
                    EpgHtmlPortal_Common::TrimW(trimmed.substr(eq + 1));
            }

            flushSection();
            return CompactReservationEntries_(std::move(out));
        }

        bool ParseLooseDateYmd_(const std::wstring& s, int& ymd) noexcept
        {
            ymd = 0;

            unsigned y = 0, m = 0, d = 0;
            if (swscanf_s(s.c_str(), L"%u/%u/%u", &y, &m, &d) == 3 ||
                swscanf_s(s.c_str(), L"%u-%u-%u", &y, &m, &d) == 3) {
                ymd = static_cast<int>(y * 10000 + m * 100 + d);
                return true;
            }

            return false;
        }

        bool ParseLooseTimeRange_(const std::wstring& s, int& startHHMM, int& endHHMM) noexcept
        {
            startHHMM = -1;
            endHHMM = -1;

            unsigned sh = 0, sm = 0, eh = 0, em = 0;

            if (swscanf_s(s.c_str(), L"%u:%u～%u:%u", &sh, &sm, &eh, &em) == 4 ||
                swscanf_s(s.c_str(), L"%u:%u〜%u:%u", &sh, &sm, &eh, &em) == 4 ||
                swscanf_s(s.c_str(), L"%u:%u-%u:%u", &sh, &sm, &eh, &em) == 4) {
                startHHMM = static_cast<int>(sh * 100 + sm);
                endHHMM = static_cast<int>(eh * 100 + em);
                return true;
            }

            if (swscanf_s(s.c_str(), L"%u:%u", &sh, &sm) == 2) {
                startHHMM = static_cast<int>(sh * 100 + sm);
                endHHMM = -1;
                return true;
            }

            return false;
        }

        bool ShiftLocalDateByDays_(int baseYmd, int deltaDays, int& outYmd)
        {
            outYmd = baseYmd;

            SYSTEMTIME st{};
            st.wYear = static_cast<WORD>(baseYmd / 10000);
            st.wMonth = static_cast<WORD>((baseYmd / 100) % 100);
            st.wDay = static_cast<WORD>(baseYmd % 100);

            FILETIME ft{};
            if (!::SystemTimeToFileTime(&st, &ft)) {
                return false;
            }

            ULARGE_INTEGER u{};
            u.LowPart = ft.dwLowDateTime;
            u.HighPart = ft.dwHighDateTime;

            constexpr ULONGLONG kOneDay100ns =
                24ULL * 60ULL * 60ULL * 1000ULL * 1000ULL * 10ULL;

            const LONGLONG shifted =
                static_cast<LONGLONG>(u.QuadPart) +
                static_cast<LONGLONG>(deltaDays) * static_cast<LONGLONG>(kOneDay100ns);

            if (shifted < 0) {
                return false;
            }

            u.QuadPart = static_cast<ULONGLONG>(shifted);
            ft.dwLowDateTime = u.LowPart;
            ft.dwHighDateTime = u.HighPart;

            SYSTEMTIME st2{};
            if (!::FileTimeToSystemTime(&ft, &st2)) {
                return false;
            }

            outYmd = static_cast<int>(st2.wYear) * 10000
                + static_cast<int>(st2.wMonth) * 100
                + static_cast<int>(st2.wDay);

            return true;
        }

        std::wstring FormatIsoDate_(int ymd)
        {
            wchar_t buf[16]{};
            swprintf_s(buf, L"%04d-%02d-%02d",
                ymd / 10000,
                (ymd / 100) % 100,
                ymd % 100);
            return std::wstring(buf);
        }

        std::wstring FormatIsoTime_(int hhmm)
        {
            wchar_t buf[16]{};
            swprintf_s(buf, L"%02d:%02d:00",
                (hhmm / 100) % 100,
                hhmm % 100);
            return std::wstring(buf);
        }

        std::uint32_t HashFNV1a32_(const std::wstring& s) noexcept
        {
            std::uint32_t h = 2166136261u;
            for (wchar_t ch : s) {
                h ^= static_cast<std::uint32_t>(ch);
                h *= 16777619u;
            }
            return (h == 0u) ? 1u : h;
        }

        std::wstring BuildDedupeKeyFromPayload_(
            const ProgramReservationPayload& payload)
        {
            if (!payload.dedupeKey.empty()) {
                return payload.dedupeKey;
            }

            const std::wstring fixedMode = NormalizeMode_(payload.reserveMode);

            wchar_t buf[320]{};
            swprintf_s(
                buf,
                L"%d-%u-%u-%u-%u-%ls-%ls-%ls",
                payload.space,
                static_cast<unsigned>(payload.networkId),
                static_cast<unsigned>(payload.tsId),
                static_cast<unsigned>(payload.serviceId),
                static_cast<unsigned>(payload.eventId),
                payload.startDate.c_str(),
                payload.startTime.c_str(),
                fixedMode.c_str());

            return std::wstring(buf);
        }

        bool NormalizePayloadDateTime_(ProgramReservationPayload& payload)
        {
            if (!payload.startDate.empty() &&
                !payload.startTime.empty() &&
                !payload.endDate.empty() &&
                !payload.endTime.empty()) {
                return true;
            }

            int ymd = 0;
            if (!ParseLooseDateYmd_(payload.dateText, ymd)) {
                return false;
            }

            int startHHMM = -1;
            int endHHMM = -1;
            if (!ParseLooseTimeRange_(payload.timeText, startHHMM, endHHMM)) {
                return false;
            }

            if (startHHMM < 0) {
                return false;
            }

            int endYmd = ymd;
            if (endHHMM >= 0 && endHHMM < startHHMM) {
                if (!ShiftLocalDateByDays_(ymd, 1, endYmd)) {
                    endYmd = ymd;
                }
            }

            if (payload.startDate.empty()) {
                payload.startDate = FormatIsoDate_(ymd);
            }
            if (payload.startTime.empty()) {
                payload.startTime = FormatIsoTime_(startHHMM);
            }
            if (payload.endDate.empty()) {
                payload.endDate = FormatIsoDate_(endYmd);
            }
            if (payload.endTime.empty()) {
                payload.endTime = FormatIsoTime_(endHHMM >= 0 ? endHHMM : startHHMM);
            }

            if (payload.durationSec == 0 && endHHMM >= 0) {
                const int sh = startHHMM / 100;
                const int sm = startHHMM % 100;
                const int eh = endHHMM / 100;
                const int em = endHHMM % 100;

                int mins = (eh * 60 + em) - (sh * 60 + sm);
                if (mins < 0) mins += 24 * 60;
                if (mins > 0) {
                    payload.durationSec = static_cast<std::uint32_t>(mins * 60);
                }
            }

            return true;
        }

        TaskSystemConnector::EpgReserveCommandEx MakeReserveCommand_(
            const ProgramReservationPayload& payload)
        {
            TaskSystemConnector::EpgReserveCommandEx cmd{};
            cmd.space = payload.space;
            cmd.networkId = payload.networkId;
            cmd.tsId = payload.tsId;
            cmd.serviceId = payload.serviceId;
            cmd.eventId = payload.eventId;

            cmd.title = payload.title;
            cmd.titleLabel = payload.titleLabel;
            cmd.serviceName = payload.serviceName;
            cmd.summary = payload.summary;
            cmd.genre = payload.genre;

            cmd.dateText = payload.dateText;
            cmd.weekdayText = payload.weekdayText;
            cmd.timeText = payload.timeText;

            cmd.startDate = payload.startDate;
            cmd.startTime = payload.startTime;
            cmd.endDate = payload.endDate;
            cmd.endTime = payload.endTime;
            cmd.durationSec = payload.durationSec;

            cmd.reserveMode = NormalizeMode_(payload.reserveMode);
            cmd.reserveSource =
                payload.reserveSource.empty() ? L"epg_portal_switch" : payload.reserveSource;
            cmd.dedupeKey = BuildDedupeKeyFromPayload_(payload);

            cmd.enabled = payload.enabled;
            cmd.disabledByUser = payload.disabledByUser;
            cmd.excludeFromAuto = payload.excludeFromAuto;
            cmd.autoReserved = payload.autoReserved;
            cmd.excludeKey =
                payload.excludeKey.empty() ? cmd.dedupeKey : payload.excludeKey;

            cmd.requiredTuners = (payload.requiredTuners == 0) ? 1 : payload.requiredTuners;
            cmd.preferredTunerIndex = payload.preferredTunerIndex;

            return cmd;
        }

        ReservationEntry MakeReservationEntryFromTask_(
            const TaskListManager::TaskEntry& t)
        {
            ReservationEntry entry{};

            entry.id = L"Task_" + std::to_wstring(t.request.kind)
                + L"_" + std::to_wstring(t.request.instance);

            entry.label =
                !t.reserve.titleLabel.empty() ? t.reserve.titleLabel :
                !t.display.label.empty() ? t.display.label :
                !t.name.empty() ? t.name : L"予約項目";

            entry.category =
                !t.display.category.empty() ? t.display.category :
                (IsEpgTaskKind_(t.request.kind) ? L"EPG" :
                    IsRecordTaskKind_(t.request.kind) ? L"RECORD" : L"GENERAL");

            entry.mode =
                !t.reserve.reserveMode.empty() ? NormalizeMode_(t.reserve.reserveMode) :
                IsEpgTaskKind_(t.request.kind) ? L"epg" :
                IsRecordTaskKind_(t.request.kind) ? L"record" : L"reserve";

            entry.enabled = t.enabled;
            entry.disabledByUser = t.reserve.disabledByUser;
            entry.excludeFromAuto = t.reserve.excludeFromAuto;
            entry.autoReserved = t.reserve.autoReserved;

            entry.dateText = t.reserve.dateText;
            entry.weekdayText = t.reserve.weekdayText;
            entry.timeText = t.reserve.timeText;
            entry.endDateText = t.reserve.endDate;
            entry.endTimeText = t.reserve.endTime;

            if (entry.dateText.empty() && t.schedule.dueYmd > 0) {
                entry.dateText = FormatDueDate(t.schedule.dueYmd);
            }
            if (entry.timeText.empty() && t.schedule.dueHHMM >= 0) {
                entry.timeText = FormatDueTime(t.schedule.dueHHMM);
            }
            if (entry.endDateText.empty() && t.display.endYmd > 0) {
                entry.endDateText = FormatDueDate(t.display.endYmd);
            }
            if (entry.endTimeText.empty() && t.display.endHHMM >= 0) {
                entry.endTimeText = FormatDueTime(t.display.endHHMM);
            }

            entry.serviceName = t.reserve.serviceName;
            if (entry.serviceName.empty()) {
                entry.serviceName = t.display.target;
            }

            entry.targetText = t.display.target;
            entry.scheduleText = !t.display.note.empty() ? t.display.note : t.name;
            entry.note = t.display.note;
            entry.summary = t.reserve.summary;
            entry.genre = t.reserve.genre;
            entry.reserveSource = t.reserve.reserveSource;
            entry.dedupeKey = t.reserve.dedupeKey;
            entry.excludeKey = t.reserve.excludeKey;

            entry.space = t.reserve.space;
            entry.channel = -1;
            entry.kind = static_cast<int>(t.request.kind);
            entry.instance = static_cast<int>(t.request.instance);

            entry.preferredTunerIndex = t.request.preferredTunerIndex;
            entry.requiredTuners =
                (t.request.requiredTuners == 0) ? 1 : static_cast<int>(t.request.requiredTuners);

            entry.networkId = t.reserve.networkId;
            entry.tsId = t.reserve.tsId;
            entry.serviceId = t.reserve.serviceId;
            entry.eventId = t.reserve.eventId;

            return entry;
        }

        std::vector<ReservationEntry> LoadReservationEntriesFromConnector_(
            const TaskSystemConnector& connector)
        {
            std::vector<ReservationEntry> out;
            const auto snap = connector.GetTaskListManager().GetSnapshot();
            out.reserve(snap.size());

            for (const auto& t : snap) {
                const bool hasReservationHint =
                    IsEpgTaskKind_(t.request.kind) ||
                    IsRecordTaskKind_(t.request.kind) ||
                    !t.reserve.serviceName.empty() ||
                    !t.reserve.title.empty() ||
                    !t.reserve.titleLabel.empty() ||
                    t.reserve.serviceId > 0 ||
                    t.reserve.eventId > 0 ||
                    !t.display.target.empty();

                if (!hasReservationHint) {
                    continue;
                }

                out.push_back(MakeReservationEntryFromTask_(t));
            }

            return CompactReservationEntries_(std::move(out));
        }

        bool WriteReservationDataJsImpl_(
            const std::filesystem::path& jsPath,
            const std::vector<ReservationEntry>& items,
            std::wstring_view sourceLabel)
        {
            std::ofstream ofs(jsPath, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                DebugLogFormat(
                    L"[EPG-PORTAL][ERROR] reservation data open failed: %ls",
                    jsPath.wstring().c_str());
                return false;
            }

            static constexpr unsigned char kUtf8Bom[3] = { 0xEF, 0xBB, 0xBF };
            ofs.write(reinterpret_cast<const char*>(kUtf8Bom), 3);

            DebugLogFormat(
                L"[EPG-PORTAL] reservation source: %ls items=%zu",
                std::wstring(sourceLabel).c_str(),
                items.size());

            std::wstring body;
            body.reserve(4096);
            body += L"window.EPG_RESERVATION_DATA = [\n";

            for (const auto& item : items) {
                body += L"  {";
                body += L"id: " + EpgHtmlPortal_Common::ToJsStringLiteral(item.id) + L", ";
                body += L"label: " + EpgHtmlPortal_Common::ToJsStringLiteral(item.label) + L", ";
                body += L"category: " + EpgHtmlPortal_Common::ToJsStringLiteral(item.category) + L", ";
                body += L"mode: " + EpgHtmlPortal_Common::ToJsStringLiteral(item.mode) + L", ";
                body += L"enabled: " + EpgHtmlPortal_Common::ToJsBool(item.enabled) + L", ";
                body += L"disabledByUser: " + EpgHtmlPortal_Common::ToJsBool(item.disabledByUser) + L", ";
                body += L"excludeFromAuto: " + EpgHtmlPortal_Common::ToJsBool(item.excludeFromAuto) + L", ";
                body += L"autoReserved: " + EpgHtmlPortal_Common::ToJsBool(item.autoReserved) + L", ";
                body += L"date: " + EpgHtmlPortal_Common::ToJsStringLiteral(item.dateText) + L", ";
                body += L"weekday: " + EpgHtmlPortal_Common::ToJsStringLiteral(item.weekdayText) + L", ";
                body += L"time: " + EpgHtmlPortal_Common::ToJsStringLiteral(item.timeText) + L", ";
                body += L"endDate: " + EpgHtmlPortal_Common::ToJsStringLiteral(item.endDateText) + L", ";
                body += L"endTime: " + EpgHtmlPortal_Common::ToJsStringLiteral(item.endTimeText) + L", ";
                body += L"service: " + EpgHtmlPortal_Common::ToJsStringLiteral(item.serviceName) + L", ";
                body += L"target: " + EpgHtmlPortal_Common::ToJsStringLiteral(item.targetText) + L", ";
                body += L"schedule: " + EpgHtmlPortal_Common::ToJsStringLiteral(item.scheduleText) + L", ";
                body += L"note: " + EpgHtmlPortal_Common::ToJsStringLiteral(item.note) + L", ";
                body += L"summary: " + EpgHtmlPortal_Common::ToJsStringLiteral(item.summary) + L", ";
                body += L"genre: " + EpgHtmlPortal_Common::ToJsStringLiteral(item.genre) + L", ";
                body += L"space: " + std::to_wstring(item.space) + L", ";
                body += L"channel: " + std::to_wstring(item.channel) + L", ";
                body += L"kind: " + std::to_wstring(item.kind) + L", ";
                body += L"instance: " + std::to_wstring(item.instance) + L", ";
                body += L"preferredTunerIndex: " + std::to_wstring(item.preferredTunerIndex) + L", ";
                body += L"requiredTuners: " + std::to_wstring(item.requiredTuners) + L", ";
                body += L"networkId: " + std::to_wstring(item.networkId) + L", ";
                body += L"tsId: " + std::to_wstring(item.tsId) + L", ";
                body += L"serviceId: " + std::to_wstring(item.serviceId) + L", ";
                body += L"eventId: " + std::to_wstring(item.eventId) + L", ";
                body += L"reserveSource: " + EpgHtmlPortal_Common::ToJsStringLiteral(item.reserveSource) + L", ";
                body += L"dedupeKey: " + EpgHtmlPortal_Common::ToJsStringLiteral(item.dedupeKey) + L", ";
                body += L"excludeKey: " + EpgHtmlPortal_Common::ToJsStringLiteral(item.excludeKey) + L"},\n";

                if (body.size() > (1u << 20)) {
                    const std::string utf8 = EpgHtmlPortal_Common::WideToUtf8(body);
                    ofs.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
                    body.clear();
                }
            }

            body += L"];\n";
            const std::string utf8 = EpgHtmlPortal_Common::WideToUtf8(body);
            ofs.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
            return static_cast<bool>(ofs);
        }
    }

    std::filesystem::path ResolveTaskIniPath(const EpgHtmlPortal::WriteOptions& opt)
    {
        return EpgHtmlPortal_Common::ResolveAuxiliaryPath(opt.outputDir, opt.taskIniPath);
    }

    bool WriteReservationDataJs(const EpgHtmlPortal::WriteOptions& opt)
    {
        const std::filesystem::path jsPath = opt.outputDir / opt.reservationDataJsFileName;
        const std::filesystem::path taskPath = ResolveTaskIniPath(opt);
        const auto items = LoadReservationEntriesFromTaskIni(taskPath);

        const std::wstring sourceLabel =
            taskPath.empty() ? L"(empty)" : taskPath.wstring();

        return WriteReservationDataJsImpl_(jsPath, items, sourceLabel);
    }

    bool WriteReservationDataJs(
        const EpgHtmlPortal::WriteOptions& opt,
        const TaskSystemConnector& connector)
    {
        const std::filesystem::path jsPath = opt.outputDir / opt.reservationDataJsFileName;
        const auto items = LoadReservationEntriesFromConnector_(connector);

        std::wstring sourceLabel = L"connector-live";
        const std::wstring& taskIni = connector.GetTasksIniPath();
        if (!taskIni.empty()) {
            sourceLabel += L" / ";
            sourceLabel += taskIni;
        }

        return WriteReservationDataJsImpl_(jsPath, items, sourceLabel);
    }

    std::wstring BuildProgramReservationDedupeKey(
        const ProgramReservationPayload& payload)
    {
        ProgramReservationPayload fixed = payload;
        if (!NormalizePayloadDateTime_(fixed)) {
            return {};
        }
        return BuildDedupeKeyFromPayload_(fixed);
    }

    std::optional<ProgramReservationPayload> NormalizeProgramReservationPayload(
        const ProgramReservationPayload& payload)
    {
        ProgramReservationPayload fixed = payload;

        if (fixed.titleLabel.empty()) {
            fixed.titleLabel = fixed.title;
        }
        if (fixed.reserveSource.empty()) {
            fixed.reserveSource = L"epg_portal_switch";
        }
        fixed.reserveMode = NormalizeMode_(fixed.reserveMode);
        fixed.requiredTuners = (fixed.requiredTuners == 0) ? 1 : fixed.requiredTuners;

        if (!NormalizePayloadDateTime_(fixed)) {
            return std::nullopt;
        }

        fixed.dedupeKey = BuildDedupeKeyFromPayload_(fixed);
        if (fixed.excludeKey.empty()) {
            fixed.excludeKey = fixed.dedupeKey;
        }

        return fixed;
    }

    ReservationMutationResult UpsertProgramReservation(
        TaskSystemConnector& connector,
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramReservationPayload& payload,
        bool regenerateReservationJs)
    {
        ReservationMutationResult result{};

        if (!connector.IsInitialized()) {
            result.reason = L"connector-not-initialized";
            return result;
        }

        const auto normalized = NormalizeProgramReservationPayload(payload);
        if (!normalized.has_value()) {
            result.reason = L"normalize-failed";
            return result;
        }

        const auto cmd = MakeReserveCommand_(normalized.value());

        result.dedupeKey = cmd.dedupeKey;
        result.instance = HashFNV1a32_(cmd.dedupeKey);

        result.ok = connector.ScheduleProgramReservation(cmd);
        result.changed = result.ok;

        if (!result.ok) {
            result.reason = L"schedule-program-reservation-failed";
            return result;
        }

        if (regenerateReservationJs) {
            if (!WriteReservationDataJs(opt, connector)) {
                result.reason = L"reservation-js-regenerate-failed";
            }
        }

        return result;
    }

    ReservationMutationResult RemoveProgramReservation(
        TaskSystemConnector& connector,
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramReservationPayload& payload,
        bool regenerateReservationJs)
    {
        ReservationMutationResult result{};

        if (!connector.IsInitialized()) {
            result.reason = L"connector-not-initialized";
            return result;
        }

        const auto normalized = NormalizeProgramReservationPayload(payload);
        if (!normalized.has_value()) {
            result.reason = L"normalize-failed";
            return result;
        }

        result.dedupeKey = normalized->dedupeKey;
        result.instance = HashFNV1a32_(result.dedupeKey);

        const auto& tl = connector.GetTaskListManager();

        const bool hasPrepare = tl.Find(TaskKinds::RecPrepare, result.instance).has_value();
        const bool hasStart = tl.Find(TaskKinds::RecStart, result.instance).has_value();
        const bool hasStop = tl.Find(TaskKinds::RecStop, result.instance).has_value();

        bool allOk = true;

        if (hasPrepare) {
            allOk = connector.RemoveTaskAndApply(TaskKinds::RecPrepare, result.instance) && allOk;
            result.changed = true;
        }
        if (hasStart) {
            allOk = connector.RemoveTaskAndApply(TaskKinds::RecStart, result.instance) && allOk;
            result.changed = true;
        }
        if (hasStop) {
            allOk = connector.RemoveTaskAndApply(TaskKinds::RecStop, result.instance) && allOk;
            result.changed = true;
        }

        result.ok = allOk;

        if (!result.changed) {
            result.reason = L"not-found";
            return result;
        }

        if (!result.ok) {
            result.reason = L"remove-task-and-apply-failed";
            return result;
        }

        if (regenerateReservationJs) {
            if (!WriteReservationDataJs(opt, connector)) {
                result.reason = L"reservation-js-regenerate-failed";
            }
        }

        return result;
    }

    ReservationMutationResult SetProgramReservationEnabled(
        TaskSystemConnector& connector,
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramReservationPayload& payload,
        bool enabled,
        bool disabledByUser,
        bool excludeFromAuto,
        bool regenerateReservationJs)
    {
        ReservationMutationResult result{};

        if (!connector.IsInitialized()) {
            result.reason = L"connector-not-initialized";
            return result;
        }

        const auto normalized = NormalizeProgramReservationPayload(payload);
        if (!normalized.has_value()) {
            result.reason = L"normalize-failed";
            return result;
        }

        result.dedupeKey = normalized->dedupeKey;
        result.instance = HashFNV1a32_(result.dedupeKey);

        const auto snap = connector.GetTaskListManager().GetSnapshot();

        bool allOk = true;
        for (const auto& t : snap) {
            if (!IsRecordTaskKind_(t.request.kind)) {
                continue;
            }
            if (t.request.instance != result.instance) {
                continue;
            }

            auto updated = t;
            updated.enabled = enabled;
            updated.reserve.disabledByUser = disabledByUser;
            updated.reserve.excludeFromAuto = excludeFromAuto;

            allOk = connector.UpsertTaskAndApply(updated) && allOk;
            result.changed = true;
        }

        result.ok = allOk;

        if (!result.changed) {
            result.reason = L"not-found";
            return result;
        }

        if (!result.ok) {
            result.reason = L"upsert-task-and-apply-failed";
            return result;
        }

        if (regenerateReservationJs) {
            if (!WriteReservationDataJs(opt, connector)) {
                result.reason = L"reservation-js-regenerate-failed";
            }
        }

        return result;
    }


ReservationMutationResult ApplyProgramReservationMutation(
    TaskSystemConnector& connector,
    const EpgHtmlPortal::WriteOptions& opt,
    const ProgramReservationMutationCommand& cmd)
{
    switch (cmd.action) {
    case ProgramReservationMutationAction::Upsert:
        return UpsertProgramReservation(
            connector,
            opt,
            cmd.payload,
            cmd.regenerateReservationJs);

    case ProgramReservationMutationAction::Remove:
        return RemoveProgramReservation(
            connector,
            opt,
            cmd.payload,
            cmd.regenerateReservationJs);

    case ProgramReservationMutationAction::SetEnabled:
        return SetProgramReservationEnabled(
            connector,
            opt,
            cmd.payload,
            cmd.enabled,
            cmd.disabledByUser,
            cmd.excludeFromAuto,
            cmd.regenerateReservationJs);

    default:
    {
        ReservationMutationResult r{};
        r.ok = false;
        r.changed = false;
        r.reason = L"unknown-mutation-action";
        return r;
    }
    }
}
} // namespace EpgHtmlPortal_ReservationData