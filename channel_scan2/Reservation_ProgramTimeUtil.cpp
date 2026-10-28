#include "Reservation_ProgramTimeUtil.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace Reservation_ProgramTimeUtil
{
    namespace
    {
        struct TimeParts
        {
            int hour = 0;
            int minute = 0;
            int second = 0;
            bool valid = false;
        };

        struct BroadcastWindow
        {
            bool hasStart = false;
            bool hasEnd = false;
            int startRawHour = 0;
            int startRawMinute = 0;
            int endRawHour = 0;
            int endRawMinute = 0;
            int startYmd = 0;
            int startHHMM = -1;
            int endYmd = 0;
            int endHHMM = -1;
            std::uint32_t durationSec = 0;
        };

        bool IsDigit_(wchar_t ch) noexcept
        {
            return ch >= L'0' && ch <= L'9';
        }

        int MakeYmd_(int y, int m, int d) noexcept
        {
            if (y < 1900 || m < 1 || m > 12 || d < 1 || d > 31) {
                return 0;
            }
            return y * 10000 + m * 100 + d;
        }

        int ReadNDigits_(std::wstring_view s, std::size_t pos, std::size_t n) noexcept
        {
            if (pos + n > s.size()) {
                return -1;
            }
            int value = 0;
            for (std::size_t i = 0; i < n; ++i) {
                const wchar_t ch = s[pos + i];
                if (!IsDigit_(ch)) {
                    return -1;
                }
                value = value * 10 + static_cast<int>(ch - L'0');
            }
            return value;
        }

        int ParseDateLoose_(std::wstring_view text) noexcept
        {
            for (std::size_t i = 0; i + 10 <= text.size(); ++i) {
                const int y = ReadNDigits_(text, i, 4);
                if (y < 0) {
                    continue;
                }
                const wchar_t sep1 = text[i + 4];
                const wchar_t sep2 = text[i + 7];
                if (!((sep1 == L'-' || sep1 == L'/') &&
                    (sep2 == L'-' || sep2 == L'/'))) {
                    continue;
                }
                const int m = ReadNDigits_(text, i + 5, 2);
                const int d = ReadNDigits_(text, i + 8, 2);
                const int ymd = MakeYmd_(y, m, d);
                if (ymd > 0) {
                    return ymd;
                }
            }

            std::wstring digits;
            digits.reserve(8);
            for (const wchar_t ch : text) {
                if (IsDigit_(ch)) {
                    digits.push_back(ch);
                    if (digits.size() >= 8) {
                        break;
                    }
                }
            }
            if (digits.size() < 8) {
                return 0;
            }

            const int y = ReadNDigits_(digits, 0, 4);
            const int m = ReadNDigits_(digits, 4, 2);
            const int d = ReadNDigits_(digits, 6, 2);
            return MakeYmd_(y, m, d);
        }

        bool ReadNumber_(std::wstring_view s, std::size_t& pos, int& value, int& digits) noexcept
        {
            value = 0;
            digits = 0;
            while (pos < s.size() && IsDigit_(s[pos])) {
                value = value * 10 + static_cast<int>(s[pos] - L'0');
                ++pos;
                ++digits;
                if (digits > 3) {
                    return false;
                }
            }
            return digits > 0;
        }

        bool FindNextTime_(std::wstring_view text, std::size_t& pos, TimeParts& out) noexcept
        {
            while (pos < text.size()) {
                if (!IsDigit_(text[pos])) {
                    ++pos;
                    continue;
                }

                std::size_t p = pos;
                int hour = 0;
                int hourDigits = 0;
                if (!ReadNumber_(text, p, hour, hourDigits)) {
                    ++pos;
                    continue;
                }

                if (p >= text.size() || (text[p] != L':' && text[p] != L'：')) {
                    ++pos;
                    continue;
                }
                ++p;

                int minute = 0;
                int minuteDigits = 0;
                if (!ReadNumber_(text, p, minute, minuteDigits)) {
                    ++pos;
                    continue;
                }

                int second = 0;
                if (p < text.size() && (text[p] == L':' || text[p] == L'：')) {
                    ++p;
                    int secondDigits = 0;
                    if (!ReadNumber_(text, p, second, secondDigits)) {
                        ++pos;
                        continue;
                    }
                    if (secondDigits != 2) {
                        ++pos;
                        continue;
                    }
                }

                if (hourDigits < 1 || hourDigits > 2 || minuteDigits != 2) {
                    ++pos;
                    continue;
                }
                if (hour < 0 || hour > 48 || minute < 0 || minute > 59 || second < 0 || second > 59) {
                    ++pos;
                    continue;
                }

                out.hour = hour;
                out.minute = minute;
                out.second = second;
                out.valid = true;
                pos = p;
                return true;
            }

            return false;
        }

        int HHMM_(int hour, int minute) noexcept
        {
            return hour * 100 + minute;
        }

        int HourFromHHMM_(int hhmm) noexcept
        {
            return hhmm / 100;
        }

        bool IsEarlyMorning_(int hhmm, int boundaryHour) noexcept
        {
            const int h = HourFromHHMM_(hhmm);
            return h >= 0 && h < boundaryHour;
        }

        bool LocalToFileTime_(int ymd, int hhmm, int second, FILETIME& outUtcFt) noexcept
        {
            SYSTEMTIME local{};
            local.wYear = static_cast<WORD>(ymd / 10000);
            local.wMonth = static_cast<WORD>((ymd / 100) % 100);
            local.wDay = static_cast<WORD>(ymd % 100);
            local.wHour = static_cast<WORD>(hhmm / 100);
            local.wMinute = static_cast<WORD>(hhmm % 100);
            local.wSecond = static_cast<WORD>((std::clamp)(second, 0, 59));
            local.wMilliseconds = 0;

            SYSTEMTIME utc{};
            if (!::TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) {
                return false;
            }
            return ::SystemTimeToFileTime(&utc, &outUtcFt) != 0;
        }

        bool ShiftDays_(int ymd, int days, int& outYmd) noexcept
        {
            int outHHMM = 0;
            int outSec = 0;
            return ShiftLocalYmdHHMMSS(ymd, 1200, 0, days * 24 * 60 * 60, outYmd, outHHMM, outSec);
        }

        bool NormalizeTotalMinutes_(int broadcastYmd, int totalMinutes, int& outYmd, int& outHHMM) noexcept
        {
            if (broadcastYmd <= 0) {
                return false;
            }

            int dayOffset = totalMinutes / (24 * 60);
            int minuteInDay = totalMinutes % (24 * 60);
            if (minuteInDay < 0) {
                minuteInDay += 24 * 60;
                --dayOffset;
            }

            if (!ShiftDays_(broadcastYmd, dayOffset, outYmd)) {
                return false;
            }

            outHHMM = HHMM_(minuteInDay / 60, minuteInDay % 60);
            return true;
        }

        int BroadcastTotalMinutes_(int rawHour, int rawMinute, int boundaryHour) noexcept
        {
            int total = rawHour * 60 + rawMinute;
            if (rawHour >= 0 && rawHour < boundaryHour) {
                total += 24 * 60;
            }
            return total;
        }

        bool ParseBroadcastWindow_(
            std::wstring_view dateText,
            std::wstring_view timeText,
            int boundaryHour,
            BroadcastWindow& out) noexcept
        {
            out = {};

            const int broadcastYmd = ParseDateLoose_(dateText);
            if (broadcastYmd <= 0 || timeText.empty()) {
                return false;
            }

            std::size_t pos = 0;
            TimeParts start{};
            if (!FindNextTime_(timeText, pos, start)) {
                return false;
            }

            out.hasStart = true;
            out.startRawHour = start.hour;
            out.startRawMinute = start.minute;

            const int startTotal = BroadcastTotalMinutes_(start.hour, start.minute, boundaryHour);
            if (!NormalizeTotalMinutes_(broadcastYmd, startTotal, out.startYmd, out.startHHMM)) {
                return false;
            }

            TimeParts end{};
            if (FindNextTime_(timeText, pos, end)) {
                out.hasEnd = true;
                out.endRawHour = end.hour;
                out.endRawMinute = end.minute;

                int endTotal = BroadcastTotalMinutes_(end.hour, end.minute, boundaryHour);
                if (endTotal <= startTotal) {
                    endTotal += 24 * 60;
                }

                if (NormalizeTotalMinutes_(broadcastYmd, endTotal, out.endYmd, out.endHHMM)) {
                    const int durationMin = endTotal - startTotal;
                    if (durationMin > 0) {
                        out.durationSec = static_cast<std::uint32_t>(durationMin * 60);
                    }
                }
                else {
                    out.hasEnd = false;
                }
            }

            return true;
        }

        bool ParseExplicitDateTime_(
            std::wstring_view date,
            std::wstring_view time,
            int& outYmd,
            int& outHHMM,
            int& outSecond) noexcept
        {
            outYmd = 0;
            outHHMM = -1;
            outSecond = 0;

            const int baseYmd = ParseDateLoose_(date);
            if (baseYmd <= 0 || time.empty()) {
                return false;
            }

            std::size_t pos = 0;
            TimeParts t{};
            if (!FindNextTime_(time, pos, t)) {
                return false;
            }

            int dayOffset = 0;
            if (t.hour >= 24) {
                dayOffset = t.hour / 24;
                t.hour %= 24;
            }

            if (!ShiftDays_(baseYmd, dayOffset, outYmd)) {
                return false;
            }

            outHHMM = HHMM_(t.hour, t.minute);
            outSecond = t.second;
            return true;
        }

        int CompareDateTime_(int ymd1, int hhmm1, int ymd2, int hhmm2) noexcept
        {
            if (ymd1 != ymd2) {
                return (ymd1 < ymd2) ? -1 : 1;
            }
            if (hhmm1 != hhmm2) {
                return (hhmm1 < hhmm2) ? -1 : 1;
            }
            return 0;
        }

        std::uint32_t DiffSeconds_(int startYmd, int startHHMM, int endYmd, int endHHMM) noexcept
        {
            FILETIME sft{};
            FILETIME eft{};
            if (!LocalToFileTime_(startYmd, startHHMM, 0, sft) ||
                !LocalToFileTime_(endYmd, endHHMM, 0, eft)) {
                return 0;
            }

            ULARGE_INTEGER s{};
            s.LowPart = sft.dwLowDateTime;
            s.HighPart = sft.dwHighDateTime;

            ULARGE_INTEGER e{};
            e.LowPart = eft.dwLowDateTime;
            e.HighPart = eft.dwHighDateTime;

            if (e.QuadPart <= s.QuadPart) {
                return 0;
            }

            const ULONGLONG diff100ns = e.QuadPart - s.QuadPart;
            return static_cast<std::uint32_t>(diff100ns / (10ULL * 1000ULL * 1000ULL));
        }

        bool IsValidYmd_(int ymd) noexcept
        {
            const int y = ymd / 10000;
            const int m = (ymd / 100) % 100;
            const int d = ymd % 100;

            return y >= 1900 &&
                m >= 1 &&
                m <= 12 &&
                d >= 1 &&
                d <= 31;
        }

        bool IsValidHHMM_(int hhmm) noexcept
        {
            if (hhmm < 0) {
                return false;
            }

            const int h = hhmm / 100;
            const int m = hhmm % 100;

            return h >= 0 &&
                h <= 23 &&
                m >= 0 &&
                m <= 59;
        }

        int CompareDateTimeWithSecond_(
            int ymd1,
            int hhmm1,
            int second1,
            int ymd2,
            int hhmm2,
            int second2) noexcept
        {
            if (ymd1 != ymd2) {
                return (ymd1 < ymd2) ? -1 : 1;
            }

            if (hhmm1 != hhmm2) {
                return (hhmm1 < hhmm2) ? -1 : 1;
            }

            if (second1 != second2) {
                return (second1 < second2) ? -1 : 1;
            }

            return 0;
        }

        bool HasValidProgramTimeFields_(const ProgramTimePlan& plan) noexcept
        {
            if (!IsValidYmd_(plan.startYmd) ||
                !IsValidYmd_(plan.endYmd) ||
                !IsValidHHMM_(plan.startHHMM) ||
                !IsValidHHMM_(plan.endHHMM)) {
                return false;
            }

            if (plan.startSecond < 0 ||
                plan.startSecond > 59 ||
                plan.endSecond < 0 ||
                plan.endSecond > 59) {
                return false;
            }

            if (plan.startDate.empty() ||
                plan.startTime.empty() ||
                plan.endDate.empty() ||
                plan.endTime.empty()) {
                return false;
            }

            return CompareDateTimeWithSecond_(
                plan.startYmd,
                plan.startHHMM,
                plan.startSecond,
                plan.endYmd,
                plan.endHHMM,
                plan.endSecond) < 0;
        }
	}// namespace

    int ParseDateToYmd(std::wstring_view text)
    {
        return ParseDateLoose_(text);
    }

    int ParseTimeToHHMM(std::wstring_view text, int* outSecond)
    {
        if (outSecond) {
            *outSecond = 0;
        }

        std::size_t pos = 0;
        TimeParts t{};
        if (!FindNextTime_(text, pos, t)) {
            return -1;
        }
        if (outSecond) {
            *outSecond = t.second;
        }
        if (t.hour >= 24) {
            t.hour %= 24;
        }
        return HHMM_(t.hour, t.minute);
    }

    std::wstring FormatYmdIso(int ymd)
    {
        if (ymd <= 0) {
            return {};
        }

        const int y = ymd / 10000;
        const int m = (ymd / 100) % 100;
        const int d = ymd % 100;

        wchar_t buf[16]{};
        swprintf_s(buf, L"%04d-%02d-%02d", y, m, d);
        return std::wstring(buf);
    }

    std::wstring FormatHHMMSS(int hhmm, int second)
    {
        if (hhmm < 0) {
            return {};
        }

        const int h = hhmm / 100;
        const int m = hhmm % 100;
        const int s = (std::clamp)(second, 0, 59);

        wchar_t buf[16]{};
        swprintf_s(buf, L"%02d:%02d:%02d", h, m, s);
        return std::wstring(buf);
    }

    bool ShiftLocalYmdHHMMSS(
        int ymd,
        int hhmm,
        int second,
        int deltaSeconds,
        int& outYmd,
        int& outHHMM,
        int& outSecond)
    {
        FILETIME ft{};
        if (!LocalToFileTime_(ymd, hhmm, second, ft)) {
            return false;
        }

        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;

        constexpr LONGLONG kSecond100ns = 1000LL * 1000LL * 10LL;
        LONGLONG value = static_cast<LONGLONG>(u.QuadPart);
        value += static_cast<LONGLONG>(deltaSeconds) * kSecond100ns;
        if (value < 0) {
            value = 0;
        }

        u.QuadPart = static_cast<ULONGLONG>(value);
        ft.dwLowDateTime = u.LowPart;
        ft.dwHighDateTime = u.HighPart;

        SYSTEMTIME utc{};
        if (!::FileTimeToSystemTime(&ft, &utc)) {
            return false;
        }

        SYSTEMTIME local{};
        if (!::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
            return false;
        }

        outYmd = static_cast<int>(local.wYear) * 10000
            + static_cast<int>(local.wMonth) * 100
            + static_cast<int>(local.wDay);
        outHHMM = static_cast<int>(local.wHour) * 100
            + static_cast<int>(local.wMinute);
        outSecond = static_cast<int>(local.wSecond);
        return true;
    }

    ProgramTimePlan BuildProgramTimePlan(
        std::wstring_view explicitStartDate,
        std::wstring_view explicitStartTime,
        std::wstring_view explicitEndDate,
        std::wstring_view explicitEndTime,
        std::wstring_view broadcastDateText,
        std::wstring_view broadcastTimeText,
        std::uint32_t durationSec,
        int broadcastDayStartHour)
    {
        ProgramTimePlan out{};

        broadcastDayStartHour = (std::clamp)(broadcastDayStartHour, 0, 12);

        int explicitStartYmd = 0;
        int explicitStartHHMM = -1;
        int explicitStartSec = 0;
        const bool explicitStartOk = ParseExplicitDateTime_(
            explicitStartDate,
            explicitStartTime,
            explicitStartYmd,
            explicitStartHHMM,
            explicitStartSec);

        int explicitEndYmd = 0;
        int explicitEndHHMM = -1;
        int explicitEndSec = 0;
        const bool explicitEndOk = ParseExplicitDateTime_(
            explicitEndDate,
            explicitEndTime,
            explicitEndYmd,
            explicitEndHHMM,
            explicitEndSec);

        BroadcastWindow bw{};
        const bool broadcastOk = ParseBroadcastWindow_(
            broadcastDateText,
            broadcastTimeText,
            broadcastDayStartHour,
            bw);

        const int broadcastBaseYmd = ParseDateLoose_(broadcastDateText);

        bool useBroadcastStart = false;
        if (!explicitStartOk) {
            useBroadcastStart = broadcastOk && bw.hasStart;
        }
        else if (broadcastOk && bw.hasStart && broadcastBaseYmd > 0) {
            // explicitStartDate / explicitStartTime がある場合は、それを実日時として優先する。
            //
            // 旧処理では、
            //   explicitStartYmd == broadcastBaseYmd && 03:45 < 放送日境界(04:00)
            // のようなケースを「放送日扱い」と見なして +1日していた。
            //
            // しかし現在の番組表 payload は startDate/startTime に実日時を持つため、
            // 2026-05-29 03:45 を 2026-05-30 03:45 に飛ばしてしまう。
            //
            // そのため、明示日時が有効な場合は基本的に二重補正しない。
            // 24:00 以上の表記が来た場合だけ、broadcast window 側を使う。
            if (bw.startRawHour >= 24) {
                useBroadcastStart = true;
            }
        }

        if (useBroadcastStart) {
            out.startYmd = bw.startYmd;
            out.startHHMM = bw.startHHMM;
            out.startSecond = 0;
            out.usedDisplayStart = true;
            out.startAdjustedByBroadcastDay =
                bw.startRawHour >= 24 || bw.startRawHour < broadcastDayStartHour;
        }
        else if (explicitStartOk) {
            out.startYmd = explicitStartYmd;
            out.startHHMM = explicitStartHHMM;
            out.startSecond = explicitStartSec;
        }
        else {
            return out;
        }

        bool endOk = false;
        if (explicitEndOk && CompareDateTime_(explicitEndYmd, explicitEndHHMM, out.startYmd, out.startHHMM) > 0) {
            out.endYmd = explicitEndYmd;
            out.endHHMM = explicitEndHHMM;
            out.endSecond = explicitEndSec;
            endOk = true;
        }

        if (!endOk) {
            const std::uint32_t fallbackDurationSec =
                durationSec > 0 ? durationSec : bw.durationSec;
            if (fallbackDurationSec > 0) {
                if (ShiftLocalYmdHHMMSS(
                    out.startYmd,
                    out.startHHMM,
                    out.startSecond,
                    static_cast<int>(fallbackDurationSec),
                    out.endYmd,
                    out.endHHMM,
                    out.endSecond)) {
                    out.usedDurationEnd = true;
                    endOk = true;
                }
            }
        }

        if (!endOk && broadcastOk && bw.hasEnd &&
            CompareDateTime_(bw.endYmd, bw.endHHMM, out.startYmd, out.startHHMM) > 0) {
            out.endYmd = bw.endYmd;
            out.endHHMM = bw.endHHMM;
            out.endSecond = 0;
            out.usedDisplayEnd = true;
            endOk = true;
        }

        if (!endOk) {
            return out;
        }

        out.durationSec = DiffSeconds_(out.startYmd, out.startHHMM, out.endYmd, out.endHHMM);
        out.crossesDay = out.startYmd != out.endYmd;
        out.startDate = FormatYmdIso(out.startYmd);
        out.startTime = FormatHHMMSS(out.startHHMM, out.startSecond);
        out.endDate = FormatYmdIso(out.endYmd);
        out.endTime = FormatHHMMSS(out.endHHMM, out.endSecond);

        out.valid = HasValidProgramTimeFields_(out);
        return out;
    }

    StageTimingPlan BuildStageTimingPlan(
        const ProgramTimePlan& program,
        int prepareLeadSec,
        int startAdjustSec,
        int endAdjustSec)
    {
        StageTimingPlan out{};
        if (!IsValidProgramTimePlan(program)) {
            return out;
        }

        prepareLeadSec = (std::max)(0, prepareLeadSec);

        if (!ShiftLocalYmdHHMMSS(
            program.startYmd,
            program.startHHMM,
            program.startSecond,
            -prepareLeadSec,
            out.prepareYmd,
            out.prepareHHMM,
            out.prepareSecond)) {
            return out;
        }

        if (!ShiftLocalYmdHHMMSS(
            program.startYmd,
            program.startHHMM,
            program.startSecond,
            startAdjustSec,
            out.startYmd,
            out.startHHMM,
            out.startSecond)) {
            return out;
        }

        if (!ShiftLocalYmdHHMMSS(
            program.endYmd,
            program.endHHMM,
            program.endSecond,
            endAdjustSec,
            out.stopYmd,
            out.stopHHMM,
            out.stopSecond)) {
            return out;
        }

        out.valid = true;
        return out;
    }

    bool IsValidProgramTimePlan(const ProgramTimePlan& plan) noexcept
    {
        return HasValidProgramTimeFields_(plan);
    }
}
