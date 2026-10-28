#pragma once
#ifndef RESERVATION_PROGRAM_TIME_UTIL_HPP
#define RESERVATION_PROGRAM_TIME_UTIL_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <string>
#include <string_view>

namespace Reservation_ProgramTimeUtil
{
    struct ProgramTimePlan
    {
        bool valid = false;

        int startYmd = 0;
        int startHHMM = -1;
        int startSecond = 0;

        int endYmd = 0;
        int endHHMM = -1;
        int endSecond = 0;

        std::uint32_t durationSec = 0;
        bool crossesDay = false;
        bool startAdjustedByBroadcastDay = false;
        bool usedDisplayStart = false;
        bool usedDisplayEnd = false;
        bool usedDurationEnd = false;

        std::wstring startDate; // YYYY-MM-DD
        std::wstring startTime; // HH:MM:SS
        std::wstring endDate;   // YYYY-MM-DD
        std::wstring endTime;   // HH:MM:SS
    };

    struct StageTimingPlan
    {
        bool valid = false;

        int prepareYmd = 0;
        int prepareHHMM = -1;
        int prepareSecond = 0;

        int startYmd = 0;
        int startHHMM = -1;
        int startSecond = 0;

        int stopYmd = 0;
        int stopHHMM = -1;
        int stopSecond = 0;
    };

    int ParseDateToYmd(std::wstring_view text);
    int ParseTimeToHHMM(std::wstring_view text, int* outSecond = nullptr);

    std::wstring FormatYmdIso(int ymd);
    std::wstring FormatHHMMSS(int hhmm, int second = 0);

    bool ShiftLocalYmdHHMMSS(
        int ymd,
        int hhmm,
        int second,
        int deltaSeconds,
        int& outYmd,
        int& outHHMM,
        int& outSecond);

    ProgramTimePlan BuildProgramTimePlan(
        std::wstring_view explicitStartDate,
        std::wstring_view explicitStartTime,
        std::wstring_view explicitEndDate,
        std::wstring_view explicitEndTime,
        std::wstring_view broadcastDateText,
        std::wstring_view broadcastTimeText,
        std::uint32_t durationSec,
        int broadcastDayStartHour = 4);

    StageTimingPlan BuildStageTimingPlan(
        const ProgramTimePlan& program,
        int prepareLeadSec,
        int startAdjustSec,
        int endAdjustSec);

    bool IsValidProgramTimePlan(const ProgramTimePlan& plan) noexcept;
}

#endif // RESERVATION_PROGRAM_TIME_UTIL_HPP

