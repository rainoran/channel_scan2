#pragma once
#ifndef TASK_LIST_MANAGER_HPP
#define TASK_LIST_MANAGER_HPP

#include <string>
#include <vector>
#include <mutex>
#include <optional>
#include <cstdint>

#include "TimeKeeper.hpp"
#include "TaskKinds.hpp"

class TaskListManager final
{
public:
    enum class ScheduleType : std::uint8_t {
        None = 0,
        Daily,
        Hourly,
        Interval,
        OneShot,
    };

    struct Schedule {
        ScheduleType type = ScheduleType::None;

        int dailyHour = 3;
        int dailyMinute = 30;
        int hourlyAtMinute = 5;
        int intervalMinutes = 0;

        // OneShot 用。
        //
        // dueYmd:
        //   YYYYMMDD。0以下は未設定。
        //
        // dueHHMM:
        //   HHMM。0000 は 00:00 として有効。
        //   無効判定では dueHHMM <= 0 を使わないこと。
        //
        // dueSecond:
        //   0〜59。00:00:30 などの秒指定に使う。
        int dueYmd = 0;
        int dueHHMM = 0;
        int dueSecond = 0;

        int lateToleranceMinutes = -1;

        // 範囲指定。
        // windowStartHHMM / windowEndHHMM も 0000 は有効。
        bool useTimeWindow = false;
        int windowStartYmd = 0;
        int windowStartHHMM = 0;
        int windowStartSecond = 0;
        int windowEndYmd = 0;
        int windowEndHHMM = 0;
        int windowEndSecond = 0;
    };

    struct DisplayInfo {
        std::wstring label;
        std::wstring category;
        std::wstring target;
        std::wstring note;
        int endYmd = 0;
        int endHHMM = 0;
    };

    struct ReserveInfo {
        int space = 0;
        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;
        std::uint16_t serviceId = 0;
        std::uint16_t eventId = 0;

        std::wstring title;
        std::wstring titleLabel;
        std::wstring serviceName;
        std::wstring summary;
        std::wstring genre;

        std::wstring dateText;
        std::wstring weekdayText;
        std::wstring timeText;

        std::wstring startDate;   // YYYY-MM-DD
        std::wstring startTime;   // HH:MM:SS
        std::wstring endDate;     // YYYY-MM-DD
        std::wstring endTime;     // HH:MM:SS
        std::uint32_t durationSec = 0;

        std::wstring reserveMode;   // watch / record
        std::wstring reserveSource; // epg_portal / epg_portal_switch / auto_reserve
        std::wstring dedupeKey;

        bool disabledByUser = false;
        bool excludeFromAuto = false;
        bool autoReserved = false;
        std::wstring excludeKey;
    };

    struct TaskEntry {
        std::wstring name;
        DisplayInfo display{};
        ReserveInfo reserve{};   
        bool enabled = true;

        TimeKeeper::TaskRequest request{};
        Schedule schedule{};

        bool requiresIdle = false;
        int idleRequiredSeconds = 0;
    };

public:
    void Clear();
    void UpsertTask(const TaskEntry& entry);
    bool RemoveTask(std::uint32_t kind, std::uint32_t instance);

    std::optional<TaskEntry> Find(std::uint32_t kind, std::uint32_t instance) const;
    std::vector<TaskEntry> GetSnapshot() const;

    void EnsureDefaultEpgTasks(std::uint8_t requiredTuners = 1);

    TimeKeeper::Settings BuildTimeKeeperSettingsFromTasks(const TimeKeeper::Settings& base) const;
    void ApplyOneShotsToTimeKeeper(TimeKeeper& tk) const;
    void ApplyToTimeKeeper(TimeKeeper& tk, const TimeKeeper::Settings& base, const TimeKeeper::Hooks& hooks) const;

    bool LoadFromIni(const std::wstring& path);
    bool SaveToIni(const std::wstring& path) const;

private:
    static std::uint64_t MakeKey_(std::uint32_t kind, std::uint32_t instance);

private:
    mutable std::mutex mtx_;
    std::vector<TaskEntry> tasks_;
};

#endif // TASK_LIST_MANAGER_HPP