#include "Reservation_EpgRebuildAction.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cwctype>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "DebugManager.h"
#include "TaskKinds.hpp"
#include "TaskListManager.hpp"
#include "TaskReservationCache.hpp"

namespace ReservationEpgRebuildAction
{
    namespace
    {
        std::wstring ToLowerAscii_(std::wstring s)
        {
            for (auto& ch : s) {
                if (ch >= L'A' && ch <= L'Z') {
                    ch = static_cast<wchar_t>(ch - L'A' + L'a');
                }
            }
            return s;
        }

        std::wstring NormalizeMode_(const std::wstring& mode)
        {
            const auto m = ToLowerAscii_(mode);

            if (m == L"watch") {
                return L"watch";
            }
            if (m == L"record") {
                return L"record";
            }

            return {};
        }

        bool IsProgramReservationTask_(const TaskListManager::TaskEntry& task)
        {
            if (!NormalizeMode_(task.reserve.reserveMode).empty()) {
                return true;
            }

            const auto cat = ToLowerAscii_(task.display.category);
            return cat == L"watch" || cat == L"record";
        }

        bool IsProvisionalTitle_(const std::wstring& title)
        {
            if (title.empty()) {
                return true;
            }

            return title.find(L"未定") != std::wstring::npos ||
                title.find(L"番組未定") != std::wstring::npos ||
                title.find(L"番組内容未定") != std::wstring::npos ||
                title.find(L"仮") != std::wstring::npos ||
                title.find(L"調整中") != std::wstring::npos ||
                title.find(L"詳細未定") != std::wstring::npos;
        }

        std::wstring NormalizeCompareText_(const std::wstring& s)
        {
            std::wstring out;
            out.reserve(s.size());

            for (wchar_t ch : s) {
                if (ch == L' ' ||
                    ch == L'\t' ||
                    ch == L'\r' ||
                    ch == L'\n' ||
                    ch == L'\u3000') {
                    continue;
                }

                out.push_back(ch);
            }

            return out;
        }

        std::wstring StartHHMM_(const std::wstring& time)
        {
            if (time.size() < 5) {
                return {};
            }

            std::wstring digits;
            digits.reserve(5);

            for (wchar_t ch : time) {
                if ((ch >= L'0' && ch <= L'9') || ch == L':') {
                    digits.push_back(ch);
                    if (digits.size() >= 5) {
                        break;
                    }
                }
            }

            if (digits.size() < 5 || digits[2] != L':') {
                return {};
            }

            return digits;
        }

        int ParseYmd_(const std::wstring& date)
        {
            if (date.size() < 10) {
                return 0;
            }

            try {
                const int y = std::stoi(date.substr(0, 4));
                const int m = std::stoi(date.substr(5, 2));
                const int d = std::stoi(date.substr(8, 2));
                return y * 10000 + m * 100 + d;
            }
            catch (...) {
                return 0;
            }
        }

        int ParseHHMM_(const std::wstring& time)
        {
            const auto hhmm = StartHHMM_(time);
            if (hhmm.size() < 5) {
                return 0;
            }

            try {
                const int h = std::stoi(hhmm.substr(0, 2));
                const int m = std::stoi(hhmm.substr(3, 2));
                return h * 100 + m;
            }
            catch (...) {
                return 0;
            }
        }

        int ParseMinutesOfDay_(const std::wstring& time)
        {
            const auto hhmm = StartHHMM_(time);
            if (hhmm.size() < 5) {
                return -1;
            }

            try {
                const int hour = std::stoi(hhmm.substr(0, 2));
                const int minute = std::stoi(hhmm.substr(3, 2));

                if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
                    return -1;
                }

                return hour * 60 + minute;
            }
            catch (...) {
                return -1;
            }
        }

        using ProgramTime_ = std::chrono::sys_seconds;

        bool ParseProgramTime_(
            const std::wstring& date,
            const std::wstring& time,
            ProgramTime_& out)
        {
            if (date.size() < 10 || time.size() < 5) {
                return false;
            }

            try {
                const int y = std::stoi(date.substr(0, 4));
                const unsigned m = static_cast<unsigned>(std::stoi(date.substr(5, 2)));
                const unsigned d = static_cast<unsigned>(std::stoi(date.substr(8, 2)));
                const int hour = std::stoi(time.substr(0, 2));
                const int minute = std::stoi(time.substr(3, 2));
                const int second =
                    time.size() >= 8 ? std::stoi(time.substr(6, 2)) : 0;

                const std::chrono::year_month_day ymd{
                    std::chrono::year{ y },
                    std::chrono::month{ m },
                    std::chrono::day{ d }
                };

                if (!ymd.ok() ||
                    hour < 0 || hour > 23 ||
                    minute < 0 || minute > 59 ||
                    second < 0 || second > 59) {
                    return false;
                }

                out = std::chrono::sys_days{ ymd } +
                    std::chrono::hours{ hour } +
                    std::chrono::minutes{ minute } +
                    std::chrono::seconds{ second };
                return true;
            }
            catch (...) {
                return false;
            }
        }

        bool ParseTaskTime_(
            int ymd,
            int hhmm,
            int second,
            ProgramTime_& out)
        {
            if (ymd <= 0 || hhmm < 0) {
                return false;
            }

            wchar_t date[16]{};
            wchar_t time[16]{};

            swprintf_s(
                date,
                L"%04d-%02d-%02d",
                ymd / 10000,
                (ymd / 100) % 100,
                ymd % 100);

            swprintf_s(
                time,
                L"%02d:%02d:%02d",
                hhmm / 100,
                hhmm % 100,
                (std::clamp)(second, 0, 59));

            return ParseProgramTime_(date, time, out);
        }

        void SplitProgramTime_(
            ProgramTime_ value,
            int& ymd,
            int& hhmm,
            int& second)
        {
            const auto dayPoint = std::chrono::floor<std::chrono::days>(value);
            const std::chrono::year_month_day date{ dayPoint };
            const std::chrono::hh_mm_ss tod{ value - dayPoint };

            ymd =
                static_cast<int>(date.year()) * 10000 +
                static_cast<int>(static_cast<unsigned>(date.month())) * 100 +
                static_cast<int>(static_cast<unsigned>(date.day()));

            hhmm =
                static_cast<int>(tod.hours().count()) * 100 +
                static_cast<int>(tod.minutes().count());

            second = static_cast<int>(tod.seconds().count());
        }

        ProgramTime_ CurrentLocalProgramTime_(const Options& options)
        {
            if (options.nowProvider) {
                return std::chrono::time_point_cast<std::chrono::seconds>(
                    options.nowProvider());
            }

            const std::time_t raw = std::time(nullptr);
            std::tm local{};

#if defined(_WIN32)
            if (localtime_s(&local, &raw) != 0) {
                return std::chrono::time_point_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now());
            }
#else
            if (localtime_r(&raw, &local) == nullptr) {
                return std::chrono::time_point_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now());
            }
#endif

            const std::chrono::year_month_day date{
                std::chrono::year{ local.tm_year + 1900 },
                std::chrono::month{ static_cast<unsigned>(local.tm_mon + 1) },
                std::chrono::day{ static_cast<unsigned>(local.tm_mday) }
            };

            return std::chrono::sys_days{ date } +
                std::chrono::hours{ local.tm_hour } +
                std::chrono::minutes{ local.tm_min } +
                std::chrono::seconds{ local.tm_sec };
        }

        std::uint32_t HashFNV1a32_(const std::wstring& text) noexcept
        {
            std::uint32_t hash = 2166136261u;

            for (wchar_t ch : text) {
                const std::uint32_t value = static_cast<std::uint32_t>(ch);

                for (unsigned shift = 0; shift < 32; shift += 8) {
                    hash ^= (value >> shift) & 0xFFu;
                    hash *= 16777619u;
                }
            }

            return hash == 0 ? 1u : hash;
        }

        std::wstring ReservationTitle_(const TaskListManager::TaskEntry& task)
        {
            return task.reserve.titleLabel.empty()
                ? task.reserve.title
                : task.reserve.titleLabel;
        }

        std::wstring ProgramTitle_(const LatestProgramInfo& program)
        {
            return program.titleLabel.empty()
                ? program.title
                : program.titleLabel;
        }

        bool TitlesCompatible_(
            const TaskListManager::TaskEntry& task,
            const LatestProgramInfo& program)
        {
            const auto taskTitle = NormalizeCompareText_(ReservationTitle_(task));
            const auto programTitle = NormalizeCompareText_(ProgramTitle_(program));

            if (taskTitle.empty() || programTitle.empty()) {
                return false;
            }

            return taskTitle == programTitle;
        }

        std::wstring MakeStrictEventKey_(int space,
            std::uint16_t networkId,
            std::uint16_t tsId,
            std::uint16_t serviceId,
            std::uint16_t eventId)
        {
            if (networkId == 0 || tsId == 0 || serviceId == 0 || eventId == 0) {
                return {};
            }

            return L"event|" +
                std::to_wstring(space) + L"|" +
                std::to_wstring(networkId) + L"|" +
                std::to_wstring(tsId) + L"|" +
                std::to_wstring(serviceId) + L"|" +
                std::to_wstring(eventId);
        }

        std::wstring MakeLooseEventKey_(int space,
            std::uint16_t serviceId,
            std::uint16_t eventId)
        {
            if (serviceId == 0 || eventId == 0) {
                return {};
            }

            return L"event-loose|" +
                std::to_wstring(space) + L"|" +
                std::to_wstring(serviceId) + L"|" +
                std::to_wstring(eventId);
        }

        std::wstring MakeTimeKey_(int space,
            std::uint16_t serviceId,
            const std::wstring& startDate,
            const std::wstring& startTime)
        {
            const auto hhmm = StartHHMM_(startTime);

            if (serviceId == 0 || startDate.empty() || hhmm.empty()) {
                return {};
            }

            return L"time|" +
                std::to_wstring(space) + L"|" +
                std::to_wstring(serviceId) + L"|" +
                startDate + L"|" +
                hhmm;
        }

        std::wstring MakeSoftTimeKey_(int space,
            const std::wstring& serviceName,
            const std::wstring& startDate,
            const std::wstring& startTime)
        {
            const auto service = NormalizeCompareText_(serviceName);
            const auto hhmm = StartHHMM_(startTime);

            if (service.empty() || startDate.empty() || hhmm.empty()) {
                return {};
            }

            return L"time-soft|" +
                std::to_wstring(space) + L"|" +
                service + L"|" +
                startDate + L"|" +
                hhmm;
        }

        std::wstring MakeRescheduleTitleKey_(
            int space,
            std::uint16_t networkId,
            std::uint16_t tsId,
            std::uint16_t serviceId,
            const std::wstring& title)
        {
            const auto normalizedTitle = NormalizeCompareText_(title);

            if (serviceId == 0 || normalizedTitle.empty()) {
                return {};
            }

            return L"reschedule|" +
                std::to_wstring(space) + L"|" +
                std::to_wstring(networkId) + L"|" +
                std::to_wstring(tsId) + L"|" +
                std::to_wstring(serviceId) + L"|" +
                normalizedTitle;
        }

        std::wstring MakeDedupeKey_(
            const TaskListManager::TaskEntry& task,
            const LatestProgramInfo& program)
        {
            const auto mode = NormalizeMode_(task.reserve.reserveMode);

            return std::to_wstring(program.space) +
                L"-" + std::to_wstring(program.networkId) +
                L"-" + std::to_wstring(program.tsId) +
                L"-" + std::to_wstring(program.serviceId) +
                L"-" + std::to_wstring(program.eventId) +
                L"-" + program.startDate +
                L"-" + program.startTime +
                L"-" + mode;
        }

        struct ProgramIndex
        {
            // EventID は長期間の EIT 内で再利用されるため、候補を1件に潰さない。
            std::unordered_map<std::wstring, std::vector<const LatestProgramInfo*>> strictEvent;
            std::unordered_map<std::wstring, std::vector<const LatestProgramInfo*>> looseEvent;
            std::unordered_map<std::wstring, std::vector<const LatestProgramInfo*>> time;
            std::unordered_map<std::wstring, std::vector<const LatestProgramInfo*>> rescheduleTitle;
            std::unordered_map<std::wstring, const LatestProgramInfo*> softTime;
        };

        void AddEventIndexItem_(
            std::unordered_map<std::wstring, std::vector<const LatestProgramInfo*>>& map,
            const std::wstring& key,
            const LatestProgramInfo& program)
        {
            if (!key.empty()) {
                map[key].push_back(&program);
            }
        }

        void AddIndexItem_(
            std::unordered_map<std::wstring, const LatestProgramInfo*>& map,
            const std::wstring& key,
            const LatestProgramInfo& program)
        {
            if (key.empty()) {
                return;
            }

            if (map.find(key) == map.end()) {
                map.emplace(key, &program);
            }
        }

        ProgramIndex BuildProgramIndex_(const std::vector<LatestProgramInfo>& programs)
        {
            ProgramIndex index{};

            for (const auto& p : programs) {
                AddEventIndexItem_(
                    index.strictEvent,
                    MakeStrictEventKey_(
                        p.space,
                        p.networkId,
                        p.tsId,
                        p.serviceId,
                        p.eventId),
                    p);

                AddEventIndexItem_(
                    index.looseEvent,
                    MakeLooseEventKey_(
                        p.space,
                        p.serviceId,
                        p.eventId),
                    p);

                AddEventIndexItem_(
                    index.time,
                    MakeTimeKey_(
                        p.space,
                        p.serviceId,
                        p.startDate,
                        p.startTime),
                    p);

                AddEventIndexItem_(
                    index.rescheduleTitle,
                    MakeRescheduleTitleKey_(
                        p.space,
                        p.networkId,
                        p.tsId,
                        p.serviceId,
                        ProgramTitle_(p)),
                    p);

                AddIndexItem_(
                    index.softTime,
                    MakeSoftTimeKey_(
                        p.space,
                        p.serviceName,
                        p.startDate,
                        p.startTime),
                    p);
            }

            return index;
        }

        const LatestProgramInfo* SelectExactTimeCandidate_(
            const std::vector<const LatestProgramInfo*>& candidates,
            const TaskListManager::TaskEntry& task)
        {
            const LatestProgramInfo* best = nullptr;
            int bestScore = (std::numeric_limits<int>::min)();
            bool ambiguous = false;

            for (const LatestProgramInfo* candidate : candidates) {
                if (candidate == nullptr) {
                    continue;
                }

                // 両方に識別子があるのに異なる候補は、同一時刻でも別サービス。
                if (task.reserve.networkId != 0 &&
                    candidate->networkId != 0 &&
                    task.reserve.networkId != candidate->networkId) {
                    continue;
                }

                if (task.reserve.tsId != 0 &&
                    candidate->tsId != 0 &&
                    task.reserve.tsId != candidate->tsId) {
                    continue;
                }

                const bool eventIdMatches =
                    task.reserve.eventId != 0 &&
                    candidate->eventId != 0 &&
                    task.reserve.eventId == candidate->eventId;

                const bool titleMatches =
                    TitlesCompatible_(task, *candidate);

                const bool provisional =
                    IsProvisionalTitle_(task.reserve.title) ||
                    IsProvisionalTitle_(task.reserve.titleLabel);

                // 同一サービス・同一時刻でも、確定済み予約とEventIDもタイトルも
                // 異なる番組へは追従しない。番組差し替えを予約の同一性と誤認する
                // ことを防ぐ。仮タイトルだけは時刻枠から正式化を許可する。
                if (!provisional && !eventIdMatches && !titleMatches) {
                    continue;
                }

                int score = 0;

                if (eventIdMatches) {
                    score += 8;
                }

                if (task.reserve.networkId != 0 &&
                    candidate->networkId == task.reserve.networkId) {
                    score += 4;
                }

                if (task.reserve.tsId != 0 &&
                    candidate->tsId == task.reserve.tsId) {
                    score += 4;
                }

                if (titleMatches) {
                    score += 2;
                }

                if (score > bestScore) {
                    best = candidate;
                    bestScore = score;
                    ambiguous = false;
                }
                else if (score == bestScore) {
                    // 同点候補がある場合は安全のため更新しない。
                    ambiguous = true;
                }
            }

            return ambiguous ? nullptr : best;
        }

        const LatestProgramInfo* SelectGuardedEventCandidate_(
            const std::vector<const LatestProgramInfo*>& candidates,
            const TaskListManager::TaskEntry& task,
            std::uint32_t maxShiftMinutes,
            bool requireTitleMatch)
        {
            // EventID の再利用を排除するため、日付が違う候補は許可しない。
            if (task.reserve.startDate.empty()) {
                return nullptr;
            }

            const int taskMinute = ParseMinutesOfDay_(task.reserve.startTime);
            if (taskMinute < 0) {
                return nullptr;
            }

            const LatestProgramInfo* best = nullptr;
            std::uint32_t bestDifference = (std::numeric_limits<std::uint32_t>::max)();
            bool ambiguous = false;

            for (const LatestProgramInfo* candidate : candidates) {
                if (candidate == nullptr ||
                    candidate->startDate != task.reserve.startDate) {
                    continue;
                }

                if (requireTitleMatch && !TitlesCompatible_(task, *candidate)) {
                    continue;
                }

                const int candidateMinute = ParseMinutesOfDay_(candidate->startTime);
                if (candidateMinute < 0) {
                    continue;
                }

                const auto difference = static_cast<std::uint32_t>(
                    taskMinute > candidateMinute
                    ? taskMinute - candidateMinute
                    : candidateMinute - taskMinute);

                if (difference > maxShiftMinutes) {
                    continue;
                }

                if (difference < bestDifference) {
                    best = candidate;
                    bestDifference = difference;
                    ambiguous = false;
                }
                else if (difference == bestDifference) {
                    // 同順位候補を恣意的に選ばない。
                    ambiguous = true;
                }
            }

            return ambiguous ? nullptr : best;
        }

        const LatestProgramInfo* FindRescheduledProgram_(
            const ProgramIndex& index,
            const TaskListManager::TaskEntry& task,
            const Options& options,
            bool& ambiguous)
        {
            ambiguous = false;

            if (!options.enableRescheduleFollow ||
                options.maxRescheduleDays == 0 ||
                IsProvisionalTitle_(ReservationTitle_(task))) {
                return nullptr;
            }

            ProgramTime_ oldStart{};
            if (!ParseProgramTime_(
                task.reserve.startDate,
                task.reserve.startTime,
                oldStart)) {
                return nullptr;
            }

            const auto key = MakeRescheduleTitleKey_(
                task.reserve.space,
                task.reserve.networkId,
                task.reserve.tsId,
                task.reserve.serviceId,
                ReservationTitle_(task));

            const auto it = index.rescheduleTitle.find(key);
            if (key.empty() || it == index.rescheduleTitle.end()) {
                return nullptr;
            }

            const ProgramTime_ limit =
                oldStart + std::chrono::days{ options.maxRescheduleDays };

            const LatestProgramInfo* best = nullptr;
            ProgramTime_ bestStart = ProgramTime_::max();

            for (const LatestProgramInfo* candidate : it->second) {
                if (candidate == nullptr ||
                    !TitlesCompatible_(task, *candidate)) {
                    continue;
                }

                ProgramTime_ candidateStart{};
                if (!ParseProgramTime_(
                    candidate->startDate,
                    candidate->startTime,
                    candidateStart)) {
                    continue;
                }

                // 同日内の時刻変更は通常のEventID照合へ任せる。
                // 延期追従は元の開始日時より後にある別日番組だけを対象にする。
                if (candidate->startDate == task.reserve.startDate ||
                    candidateStart <= oldStart ||
                    candidateStart > limit) {
                    continue;
                }

                if (candidateStart < bestStart) {
                    best = candidate;
                    bestStart = candidateStart;
                    ambiguous = false;
                }
                else if (candidateStart == bestStart) {
                    // 同一時刻に同じタイトルが複数ある場合は自動確定しない。
                    ambiguous = true;
                }
            }

            return ambiguous ? nullptr : best;
        }

        const LatestProgramInfo* FindProgramForTask_(
            const ProgramIndex& index,
            const TaskListManager::TaskEntry& task,
            const Options& options,
            std::wstring& reason)
        {
            const int space = task.reserve.space;

            // 1) サービスID + 開始日時の完全一致を最優先する。
            {
                const auto key = MakeTimeKey_(
                    space,
                    task.reserve.serviceId,
                    task.reserve.startDate,
                    task.reserve.startTime);

                const auto it = index.time.find(key);
                if (!key.empty() && it != index.time.end()) {
                    if (const auto* selected = SelectExactTimeCandidate_(
                        it->second,
                        task)) {
                        reason = L"time-exact";
                        return selected;
                    }
                }
            }

            // 2) 仮タイトルはサービス名 + 開始日時の完全一致だけで正式化する。
            if (IsProvisionalTitle_(task.reserve.title) ||
                IsProvisionalTitle_(task.reserve.titleLabel)) {
                const auto key = MakeSoftTimeKey_(
                    space,
                    task.reserve.serviceName,
                    task.reserve.startDate,
                    task.reserve.startTime);

                const auto it = index.softTime.find(key);
                if (!key.empty() && it != index.softTime.end()) {
                    reason = L"soft-time-provisional";
                    return it->second;
                }
            }

            // 3) 完全なサービス識別子 + EventID。
            // 同日かつ許容時刻差内の一意な候補だけを採用する。
            {
                const auto key = MakeStrictEventKey_(
                    space,
                    task.reserve.networkId,
                    task.reserve.tsId,
                    task.reserve.serviceId,
                    task.reserve.eventId);

                const auto it = index.strictEvent.find(key);
                if (!key.empty() && it != index.strictEvent.end()) {
                    if (const auto* selected = SelectGuardedEventCandidate_(
                        it->second,
                        task,
                        options.maxEventTimeShiftMinutes,
                        options.requireTitleMatchForStrictEvent)) {
                        reason = L"strict-event-guarded";
                        return selected;
                    }
                }
            }

            // 4) NID / TSID 欠落時の補助照合。
            // 日時ガードに加え、通常はタイトル完全一致も要求する。
            {
                const auto key = MakeLooseEventKey_(
                    space,
                    task.reserve.serviceId,
                    task.reserve.eventId);

                const auto it = index.looseEvent.find(key);
                if (!key.empty() && it != index.looseEvent.end()) {
                    if (const auto* selected = SelectGuardedEventCandidate_(
                        it->second,
                        task,
                        options.maxEventTimeShiftMinutes,
                        options.requireTitleMatchForLooseEvent)) {
                        reason = L"loose-event-guarded";
                        return selected;
                    }
                }
            }

            return nullptr;
        }

        bool SetIfChanged_(std::wstring& dst, const std::wstring& src)
        {
            if (src.empty()) {
                return false;
            }

            if (dst == src) {
                return false;
            }

            dst = src;
            return true;
        }

        bool SetIfChangedU16_(std::uint16_t& dst, std::uint16_t src)
        {
            if (src == 0 || dst == src) {
                return false;
            }

            dst = src;
            return true;
        }

        bool SetIfChangedInt_(int& dst, int src)
        {
            if (src == 0 || dst == src) {
                return false;
            }

            dst = src;
            return true;
        }

        bool SetIfChangedU32_(std::uint32_t& dst, std::uint32_t src)
        {
            if (src == 0 || dst == src) {
                return false;
            }

            dst = src;
            return true;
        }

        bool UpdateTaskFromProgram_(
            TaskListManager::TaskEntry& task,
            const LatestProgramInfo& program,
            const std::wstring& matchReason)
        {
            bool changed = false;
            const std::wstring oldDedupeKey = task.reserve.dedupeKey;

            const std::wstring beforeTitle =
                task.reserve.titleLabel.empty()
                ? task.reserve.title
                : task.reserve.titleLabel;

            if (task.reserve.space != program.space) {
                task.reserve.space = program.space;
                changed = true;
            }
            changed |= SetIfChangedU16_(task.reserve.networkId, program.networkId);
            changed |= SetIfChangedU16_(task.reserve.tsId, program.tsId);
            changed |= SetIfChangedU16_(task.reserve.serviceId, program.serviceId);
            changed |= SetIfChangedU16_(task.reserve.eventId, program.eventId);

            changed |= SetIfChanged_(task.reserve.serviceName, program.serviceName);
            changed |= SetIfChanged_(task.reserve.title, program.title);
            changed |= SetIfChanged_(task.reserve.titleLabel,
                program.titleLabel.empty() ? program.title : program.titleLabel);
            changed |= SetIfChanged_(task.reserve.summary, program.summary);
            changed |= SetIfChanged_(task.reserve.genre, program.genre);

            changed |= SetIfChanged_(task.reserve.dateText, program.dateText);
            changed |= SetIfChanged_(task.reserve.weekdayText, program.weekdayText);
            changed |= SetIfChanged_(task.reserve.timeText, program.timeText);

            changed |= SetIfChanged_(task.reserve.startDate, program.startDate);
            changed |= SetIfChanged_(task.reserve.startTime, program.startTime);
            changed |= SetIfChanged_(task.reserve.endDate, program.endDate);
            changed |= SetIfChanged_(task.reserve.endTime, program.endTime);
            changed |= SetIfChangedU32_(task.reserve.durationSec, program.durationSec);

            if (!program.title.empty()) {
                const std::wstring displayTitle =
                    program.titleLabel.empty()
                    ? program.title
                    : program.titleLabel;

                const bool displayTitleWasEmpty = task.display.label.empty();
                const bool displayTitleWasOld =
                    NormalizeCompareText_(task.display.label) ==
                    NormalizeCompareText_(beforeTitle);

                if (displayTitleWasEmpty ||
                    displayTitleWasOld ||
                    IsProvisionalTitle_(task.display.label)) {
                    changed |= SetIfChanged_(
                        task.display.label,
                        displayTitle);
                }

                const wchar_t* suffix = L"";
                if (task.request.kind == TaskKinds::RecPrepare) {
                    suffix = L" / Prepare";
                }
                else if (task.request.kind == TaskKinds::RecStart) {
                    suffix = L" / Start";
                }
                else if (task.request.kind == TaskKinds::RecStop) {
                    suffix = L" / Stop";
                }

                if (task.name.empty() ||
                    NormalizeCompareText_(task.name).find(
                        NormalizeCompareText_(beforeTitle)) !=
                    std::wstring::npos) {
                    changed |= SetIfChanged_(
                        task.name,
                        displayTitle + suffix);
                }
            }

            changed |= SetIfChanged_(task.display.target, program.serviceName);

            const int endYmd = ParseYmd_(program.endDate);
            const int endHHMM = ParseHHMM_(program.endTime);

            if (endYmd > 0) {
                changed |= SetIfChangedInt_(task.display.endYmd, endYmd);
            }

            if (endHHMM > 0) {
                changed |= SetIfChangedInt_(task.display.endHHMM, endHHMM);
            }

            // 番組識別情報が正式化された場合、dedupeKey / excludeKey も更新する。
            if (!NormalizeMode_(task.reserve.reserveMode).empty()) {
                const auto newKey = MakeDedupeKey_(task, program);
                if (!newKey.empty()) {
                    changed |= SetIfChanged_(task.reserve.dedupeKey, newKey);

                    // 自動予約ルール側が作った除外キーは意味が異なるため保持する。
                    // 空、または旧dedupeと同じ場合だけ番組キーへ追従させる。
                    if (task.reserve.excludeKey.empty() ||
                        task.reserve.excludeKey == oldDedupeKey) {
                        changed |= SetIfChanged_(task.reserve.excludeKey, newKey);
                    }
                }
            }

            if (changed) {
                DebugLogFormat(
                    L"[ReservationEpgRebuild] updated reservation by EPG: reason=%ls title=%ls -> %ls service=%ls start=%ls %ls",
                    matchReason.c_str(),
                    beforeTitle.c_str(),
                    (program.titleLabel.empty() ? program.title : program.titleLabel).c_str(),
                    program.serviceName.c_str(),
                    program.startDate.c_str(),
                    program.startTime.c_str());
            }

            return changed;
        }

        bool RetargetTaskTiming_(
            TaskListManager::TaskEntry& task,
            const TaskListManager::TaskEntry& source,
            const LatestProgramInfo& program)
        {
            ProgramTime_ oldStart{};
            ProgramTime_ oldEnd{};
            ProgramTime_ newStart{};
            ProgramTime_ newEnd{};
            ProgramTime_ oldDue{};

            if (!ParseProgramTime_(
                source.reserve.startDate,
                source.reserve.startTime,
                oldStart) ||
                !ParseProgramTime_(
                    source.reserve.endDate,
                    source.reserve.endTime,
                    oldEnd) ||
                !ParseProgramTime_(
                    program.startDate,
                    program.startTime,
                    newStart) ||
                !ParseProgramTime_(
                    program.endDate,
                    program.endTime,
                    newEnd) ||
                !ParseTaskTime_(
                    source.request.dispatchYmd,
                    source.request.dispatchHHMM,
                    source.request.dispatchSecond,
                    oldDue)) {
                return false;
            }

            const bool isStopTask = source.request.kind == TaskKinds::RecStop;
            const ProgramTime_ oldAnchor = isStopTask ? oldEnd : oldStart;
            const ProgramTime_ newAnchor = isStopTask ? newEnd : newStart;
            const ProgramTime_ newDue = newAnchor + (oldDue - oldAnchor);

            int dueYmd = 0;
            int dueHHMM = 0;
            int dueSecond = 0;
            SplitProgramTime_(newDue, dueYmd, dueHHMM, dueSecond);

            bool changed = false;

            if (task.request.dispatchYmd != dueYmd) {
                task.request.dispatchYmd = dueYmd;
                changed = true;
            }
            if (task.request.dispatchHHMM != dueHHMM) {
                task.request.dispatchHHMM = dueHHMM;
                changed = true;
            }
            if (task.request.dispatchSecond != dueSecond) {
                task.request.dispatchSecond = dueSecond;
                changed = true;
            }

            if (task.schedule.dueYmd != dueYmd) {
                task.schedule.dueYmd = dueYmd;
                changed = true;
            }
            if (task.schedule.dueHHMM != dueHHMM) {
                task.schedule.dueHHMM = dueHHMM;
                changed = true;
            }
            if (task.schedule.dueSecond != dueSecond) {
                task.schedule.dueSecond = dueSecond;
                changed = true;
            }

            ProgramTime_ displayEnd =
                source.request.kind == TaskKinds::RecPrepare
                ? newStart
                : (source.request.kind == TaskKinds::RecStart ? newEnd : newDue);

            int displayEndYmd = 0;
            int displayEndHHMM = 0;
            int ignoredSecond = 0;
            SplitProgramTime_(
                displayEnd,
                displayEndYmd,
                displayEndHHMM,
                ignoredSecond);

            if (task.display.endYmd != displayEndYmd) {
                task.display.endYmd = displayEndYmd;
                changed = true;
            }
            if (task.display.endHHMM != displayEndHHMM) {
                task.display.endHHMM = displayEndHHMM;
                changed = true;
            }

            wchar_t note[256]{};
            swprintf_s(
                note,
                L"開始 %ls %ls / 終了 %ls %ls / EPG再構築追従",
                program.startDate.c_str(),
                program.startTime.c_str(),
                program.endDate.c_str(),
                program.endTime.c_str());

            changed |= SetIfChanged_(task.display.note, note);
            return changed;
        }

        bool IsStarted_(
            const TaskListManager::TaskEntry& representative,
            ProgramTime_ now)
        {
            ProgramTime_ start{};
            if (!ParseProgramTime_(
                representative.reserve.startDate,
                representative.reserve.startTime,
                start)) {
                // 判定不能時に既存予約を移動しない。
                return true;
            }

            return now >= start;
        }

        const TaskListManager::TaskEntry* PickRepresentative_(
            const std::vector<std::size_t>& indices,
            const std::vector<TaskListManager::TaskEntry>& tasks)
        {
            const TaskListManager::TaskEntry* fallback = nullptr;

            for (const std::size_t index : indices) {
                if (index >= tasks.size()) {
                    continue;
                }

                const auto& task = tasks[index];
                if (fallback == nullptr) {
                    fallback = &task;
                }

                if (task.request.kind == TaskKinds::RecStart) {
                    return &task;
                }
            }

            return fallback;
        }

        std::uint32_t AllocateRescheduledInstance_(
            const std::wstring& dedupeKey,
            std::uint32_t sourceInstance,
            const std::unordered_set<std::uint32_t>& usedInstances)
        {
            std::uint32_t candidate = HashFNV1a32_(dedupeKey);
            if (usedInstances.find(candidate) == usedInstances.end()) {
                return candidate;
            }

            for (std::uint32_t salt = 1; salt < 1024; ++salt) {
                candidate = HashFNV1a32_(
                    dedupeKey + L"|from=" +
                    std::to_wstring(sourceInstance) + L"|salt=" +
                    std::to_wstring(salt));

                if (usedInstances.find(candidate) == usedInstances.end()) {
                    return candidate;
                }
            }

            return 0;
        }
    }

    Result Run(const Options& options)
    {
        Result result{};
        result.programCount = options.latestPrograms.size();

        if (options.tasksIniPath.empty()) {
            result.ok = false;
            result.message = L"tasksIniPath is empty";
            return result;
        }

        TaskListManager manager{};

        if (!manager.LoadFromIni(options.tasksIniPath)) {
            result.ok = false;
            result.message = L"LoadFromIni failed";
            return result;
        }

        auto tasks = manager.GetSnapshot();
        result.taskCount = tasks.size();

        const ProgramIndex index = BuildProgramIndex_(options.latestPrograms);
        const ProgramTime_ now = CurrentLocalProgramTime_(options);

        std::unordered_map<std::uint32_t, std::vector<std::size_t>> groups;
        std::unordered_set<std::uint32_t> usedInstances;
        std::unordered_set<std::wstring> existingDedupeKeys;

        for (std::size_t i = 0; i < tasks.size(); ++i) {
            const auto& task = tasks[i];
            if (!IsProgramReservationTask_(task) ||
                task.request.instance == 0) {
                continue;
            }

            groups[task.request.instance].push_back(i);
            usedInstances.insert(task.request.instance);

            if (!task.reserve.dedupeKey.empty()) {
                existingDedupeKeys.insert(task.reserve.dedupeKey);
            }
        }

        for (const auto& [sourceInstance, indices] : groups) {
            const TaskListManager::TaskEntry* representative =
                PickRepresentative_(indices, tasks);

            if (representative == nullptr ||
                !representative->enabled ||
                representative->reserve.disabledByUser) {
                continue;
            }

            std::wstring reason;
            const LatestProgramInfo* program = FindProgramForTask_(
                index,
                *representative,
                options,
                reason);

            bool rescheduled = false;
            bool ambiguous = false;

            if (program == nullptr) {
                program = FindRescheduledProgram_(
                    index,
                    *representative,
                    options,
                    ambiguous);

                if (program != nullptr) {
                    rescheduled = true;
                    reason = L"reschedule-title";
                }
            }

            if (ambiguous) {
                ++result.ambiguousSkippedCount;

                UpdatedItem item{};
                item.kind = TaskKinds::RecStart;
                item.instance = sourceInstance;
                item.sourceInstance = sourceInstance;
                item.action = UpdateAction::AmbiguousSkipped;
                item.beforeTitle = ReservationTitle_(*representative);
                item.afterTitle = item.beforeTitle;
                item.reason = L"reschedule-ambiguous";
                result.updatedItems.push_back(std::move(item));
                continue;
            }

            if (program == nullptr) {
                continue;
            }

            ++result.matchedCount;

            const std::wstring beforeTitle =
                ReservationTitle_(*representative);

            const std::wstring afterTitle =
                program->titleLabel.empty()
                ? program->title
                : program->titleLabel;

            const std::wstring targetDedupe =
                MakeDedupeKey_(*representative, *program);

            if (rescheduled &&
                !targetDedupe.empty() &&
                targetDedupe != representative->reserve.dedupeKey &&
                existingDedupeKeys.find(targetDedupe) !=
                existingDedupeKeys.end()) {
                ++result.duplicateSkippedCount;

                UpdatedItem item{};
                item.kind = TaskKinds::RecStart;
                item.instance = sourceInstance;
                item.sourceInstance = sourceInstance;
                item.action = UpdateAction::DuplicateSkipped;
                item.beforeTitle = beforeTitle;
                item.afterTitle = afterTitle;
                item.reason = L"reschedule-duplicate";
                item.program = *program;
                result.updatedItems.push_back(std::move(item));
                continue;
            }

            const bool alreadyStarted =
                IsStarted_(*representative, now);

            // 放送開始後の現在枠は Reservation_EitMonitor の責務。
            // EPG再構築側では実行中グループを変更しない。
            if (alreadyStarted && !rescheduled) {
                continue;
            }

            if (alreadyStarted &&
                rescheduled &&
                !options.createReservationAfterStart) {
                continue;
            }

            const bool createNew =
                rescheduled && alreadyStarted;

            if (createNew) {
                const std::uint32_t newInstance =
                    AllocateRescheduledInstance_(
                        targetDedupe,
                        sourceInstance,
                        usedInstances);

                if (newInstance == 0) {
                    ++result.duplicateSkippedCount;
                    continue;
                }

                std::size_t changedTasks = 0;

                for (const std::size_t taskIndex : indices) {
                    if (taskIndex >= tasks.size()) {
                        continue;
                    }

                    const auto& source = tasks[taskIndex];
                    auto created = source;
                    created.request.instance = newInstance;
                    created.reserve.reserveSource =
                        L"epg_reschedule:" +
                        std::to_wstring(sourceInstance);

                    bool changed = UpdateTaskFromProgram_(
                        created,
                        *program,
                        reason);

                    changed |= RetargetTaskTiming_(
                        created,
                        source,
                        *program);

                    if (changed) {
                        manager.UpsertTask(created);
                        ++changedTasks;
                    }
                }

                if (changedTasks > 0) {
                    result.updatedCount += changedTasks;
                    ++result.createdReservationCount;
                    usedInstances.insert(newInstance);

                    if (!targetDedupe.empty()) {
                        existingDedupeKeys.insert(targetDedupe);
                    }

                    UpdatedItem item{};
                    item.kind = TaskKinds::RecStart;
                    item.instance = newInstance;
                    item.sourceInstance = sourceInstance;
                    item.action = UpdateAction::CreateRescheduled;
                    item.beforeTitle = beforeTitle;
                    item.afterTitle = afterTitle;
                    item.reason = reason;
                    item.program = *program;
                    result.updatedItems.push_back(std::move(item));
                }

                continue;
            }

            std::size_t changedTasks = 0;

            for (const std::size_t taskIndex : indices) {
                if (taskIndex >= tasks.size()) {
                    continue;
                }

                const auto source = tasks[taskIndex];
                auto updated = source;

                bool changed = UpdateTaskFromProgram_(
                    updated,
                    *program,
                    reason);

                changed |= RetargetTaskTiming_(
                    updated,
                    source,
                    *program);

                if (changed) {
                    manager.UpsertTask(updated);
                    ++changedTasks;
                }
            }

            if (changedTasks > 0) {
                result.updatedCount += changedTasks;
                ++result.updatedReservationCount;

                if (!targetDedupe.empty()) {
                    existingDedupeKeys.insert(targetDedupe);
                }

                UpdatedItem item{};
                item.kind = TaskKinds::RecStart;
                item.instance = sourceInstance;
                item.sourceInstance = sourceInstance;
                item.action = UpdateAction::UpdateExisting;
                item.beforeTitle = beforeTitle;
                item.afterTitle = afterTitle;
                item.reason = reason;
                item.program = *program;
                result.updatedItems.push_back(std::move(item));
            }
        }

        if (result.updatedCount > 0 && options.saveTasksWhenChanged) {
            if (!manager.SaveToIni(options.tasksIniPath)) {
                result.ok = false;
                result.message = L"SaveToIni failed after EPG reconciliation";
                return result;
            }

            result.tasksSaved = true;
        }

        if (options.forceCacheRebuild || result.updatedCount > 0) {
            const auto cacheResult =
                TaskReservationCache::WriteAllFromTasks(
                    options.tasksIniPath,
                    manager.GetSnapshot());

            if (!cacheResult.ok) {
                result.ok = false;
                result.message = L"reservation cache rebuild failed: " + cacheResult.message;
                return result;
            }

            result.reservationCacheUpdated = true;
        }

        if (options.refreshReservationDataJs &&
            options.rebuildReservationDataJs) {
            result.reservationDataJsUpdated =
                options.rebuildReservationDataJs(options.tasksIniPath);
        }

        result.ok = true;
        result.message =
            L"ok taskCount=" + std::to_wstring(result.taskCount) +
            L" programCount=" + std::to_wstring(result.programCount) +
            L" matched=" + std::to_wstring(result.matchedCount) +
            L" updatedTasks=" + std::to_wstring(result.updatedCount) +
            L" updatedReservations=" +
            std::to_wstring(result.updatedReservationCount) +
            L" createdReservations=" +
            std::to_wstring(result.createdReservationCount) +
            L" duplicateSkipped=" +
            std::to_wstring(result.duplicateSkippedCount) +
            L" ambiguousSkipped=" +
            std::to_wstring(result.ambiguousSkippedCount) +
            L" saved=" + std::to_wstring(result.tasksSaved ? 1 : 0) +
            L" cache=" + std::to_wstring(result.reservationCacheUpdated ? 1 : 0);

        DebugLogFormat(
            L"[ReservationEpgRebuild] %ls",
            result.message.c_str());

        return result;
    }
}
