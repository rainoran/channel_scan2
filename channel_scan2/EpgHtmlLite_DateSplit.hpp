#ifndef EPG_HTML_LITE_DATE_SPLIT_HPP
#define EPG_HTML_LITE_DATE_SPLIT_HPP
#pragma once

#include <ctime>
#include <string>
#include <string_view>
#include <cwchar>

namespace EpgHtmlLite
{
    struct LiteDateYmd
    {
        int year = 0;
        int month = 0;
        int day = 0;

        bool IsValid() const noexcept
        {
            return year >= 1900 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
        }
    };

    inline bool ParseLiteDate(std::wstring_view text, LiteDateYmd& out) noexcept
    {
        out = {};

        int y = 0;
        int m = 0;
        int d = 0;

        const std::wstring s(text);

        if (swscanf_s(s.c_str(), L"%d/%d/%d", &y, &m, &d) == 3 ||
            swscanf_s(s.c_str(), L"%d-%d-%d", &y, &m, &d) == 3) {
            out.year = y;
            out.month = m;
            out.day = d;
            return out.IsValid();
        }

        return false;
    }

    inline bool ParseLiteTimeMinutes(std::wstring_view text, int& minutes) noexcept
    {
        minutes = -1;

        int h = 0;
        int m = 0;

        const std::wstring s(text);

        if (swscanf_s(s.c_str(), L"%d:%d", &h, &m) != 2) {
            return false;
        }

        if (h < 0 || h > 23 || m < 0 || m > 59) {
            return false;
        }

        minutes = h * 60 + m;
        return true;
    }

    inline std::time_t LiteDateToTimeT(LiteDateYmd date) noexcept
    {
        if (!date.IsValid()) {
            return static_cast<std::time_t>(-1);
        }

        std::tm tmv{};
        tmv.tm_year = date.year - 1900;
        tmv.tm_mon = date.month - 1;
        tmv.tm_mday = date.day;
        tmv.tm_hour = 12;
        tmv.tm_min = 0;
        tmv.tm_sec = 0;
        tmv.tm_isdst = -1;

        return std::mktime(&tmv);
    }

    inline LiteDateYmd AddLiteDays(LiteDateYmd date, int days) noexcept
    {
        const std::time_t base = LiteDateToTimeT(date);
        if (base == static_cast<std::time_t>(-1)) {
            return {};
        }

        std::time_t shifted = base + static_cast<std::time_t>(days) * 24 * 60 * 60;

        std::tm outTm{};
#ifdef _WIN32
        localtime_s(&outTm, &shifted);
#else
        outTm = *std::localtime(&shifted);
#endif

        LiteDateYmd out{};
        out.year = outTm.tm_year + 1900;
        out.month = outTm.tm_mon + 1;
        out.day = outTm.tm_mday;
        return out;
    }

    inline int LiteDaysBetween(LiteDateYmd base, LiteDateYmd target) noexcept
    {
        const std::time_t a = LiteDateToTimeT(base);
        const std::time_t b = LiteDateToTimeT(target);

        if (a == static_cast<std::time_t>(-1) ||
            b == static_cast<std::time_t>(-1)) {
            return -99999;
        }

        return static_cast<int>(std::difftime(b, a) / (24.0 * 60.0 * 60.0));
    }

    inline std::wstring FormatLiteDateYmd(LiteDateYmd date)
    {
        wchar_t buf[16]{};

        if (!date.IsValid()) {
            return {};
        }

        swprintf_s(buf, L"%04d-%02d-%02d", date.year, date.month, date.day);
        return std::wstring(buf);
    }

    inline std::wstring FormatLiteDateCompact(LiteDateYmd date)
    {
        wchar_t buf[16]{};

        if (!date.IsValid()) {
            return {};
        }

        swprintf_s(buf, L"%04d%02d%02d", date.year, date.month, date.day);
        return std::wstring(buf);
    }

    inline LiteDateYmd CalcLiteBroadcastDate(LiteDateYmd startDate, int startMinutes) noexcept
    {
        if (!startDate.IsValid()) {
            return {};
        }

        // 放送日区切り 04:00
        if (startMinutes >= 0 && startMinutes < 4 * 60) {
            return AddLiteDays(startDate, -1);
        }

        return startDate;
    }

    inline int CalcLiteTimeBlock(int startMinutes) noexcept
    {
        if (startMinutes < 0) {
            return -1;
        }

        const int h = startMinutes / 60;

        if (h >= 4 && h < 10) {
            return 0;
        }
        if (h >= 10 && h < 16) {
            return 1;
        }
        if (h >= 16 && h < 22) {
            return 2;
        }

        return 3;
    }

    inline std::wstring GetLiteBlockLabel(int block)
    {
        switch (block) {
        case 0: return L"04:00-09:59";
        case 1: return L"10:00-15:59";
        case 2: return L"16:00-21:59";
        case 3: return L"22:00-03:59";
        default: return L"全時間";
        }
    }
}

#endif // EPG_HTML_LITE_DATE_SPLIT_HPP
