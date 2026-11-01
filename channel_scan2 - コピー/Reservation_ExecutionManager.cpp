#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <atomic>
#include <memory>

#include "Reservation_ExecutionManager.hpp"

#include "ConfigManager.h"
#include "SystemInfoManager.hpp"
#include "TaskKinds.hpp"
#include "TaskSystemConnector.hpp"
#include "Reservation_TargetVerifier.hpp"

#include "DebugManager.h"



namespace
{
    std::wstring ToLowerTrim_(std::wstring s)
    {
        auto isWs = [](wchar_t c) noexcept {
            return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
            };

        while (!s.empty() && isWs(s.front())) {
            s.erase(s.begin());
        }

        while (!s.empty() && isWs(s.back())) {
            s.pop_back();
        }

        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
            });

        return s;
    }

    bool ParseReserveExecIsoYmd_(
        const std::wstring& text,
        int& outYmd) noexcept
    {
        outYmd = 0;

        unsigned y = 0;
        unsigned m = 0;
        unsigned d = 0;

        if (swscanf_s(text.c_str(), L"%u-%u-%u", &y, &m, &d) != 3) {
            return false;
        }

        if (y < 1900 || m < 1 || m > 12 || d < 1 || d > 31) {
            return false;
        }

        outYmd = static_cast<int>(y * 10000 + m * 100 + d);
        return true;
    }

    bool ParseReserveExecIsoHHMMSS_(
        const std::wstring& text,
        int& outHHMM,
        int& outSec) noexcept
    {
        outHHMM = -1;
        outSec = 0;

        unsigned hh = 0;
        unsigned mm = 0;
        unsigned ss = 0;

        const int n = swscanf_s(text.c_str(), L"%u:%u:%u", &hh, &mm, &ss);
        if (n < 2) {
            return false;
        }

        if (hh > 47 || mm > 59 || ss > 59) {
            return false;
        }

        outHHMM = static_cast<int>(hh * 100 + mm);
        outSec = static_cast<int>(ss);
        return true;
    }

    bool LocalReserveExecYmdHHMMSSToUtcFileTime_(
        int ymd,
        int hhmm,
        int sec,
        FILETIME& outUtcFileTime) noexcept
    {
        outUtcFileTime = FILETIME{};

        if (ymd <= 0 || hhmm < 0) {
            return false;
        }

        SYSTEMTIME local{};
        local.wYear = static_cast<WORD>(ymd / 10000);
        local.wMonth = static_cast<WORD>((ymd / 100) % 100);
        local.wDay = static_cast<WORD>(ymd % 100);
        local.wHour = static_cast<WORD>(hhmm / 100);
        local.wMinute = static_cast<WORD>(hhmm % 100);
        local.wSecond = static_cast<WORD>((std::clamp)(sec, 0, 59));
        local.wMilliseconds = 0;

        SYSTEMTIME utc{};
        if (!::TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) {
            return false;
        }

        return ::SystemTimeToFileTime(&utc, &outUtcFileTime) != 0;
    }

    ULONGLONG FileTimeToU64ReserveExec_(const FILETIME& ft) noexcept
    {
        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return u.QuadPart;
    }

    bool TryEntryReserveEndFileTime_(
        const TaskListManager::TaskEntry& entry,
        FILETIME& outFileTime) noexcept
    {
        outFileTime = FILETIME{};

        int endYmd = 0;
        int endHHMM = -1;
        int endSec = 0;

        if (ParseReserveExecIsoYmd_(entry.reserve.endDate, endYmd) &&
            ParseReserveExecIsoHHMMSS_(entry.reserve.endTime, endHHMM, endSec)) {
            return LocalReserveExecYmdHHMMSSToUtcFileTime_(
                endYmd,
                endHHMM,
                endSec,
                outFileTime);
        }

        if (entry.display.endYmd > 0 && entry.display.endHHMM >= 0) {
            return LocalReserveExecYmdHHMMSSToUtcFileTime_(
                entry.display.endYmd,
                entry.display.endHHMM,
                0,
                outFileTime);
        }

        return false;
    }

    bool TryTaskDueFileTime_(
        const TaskListManager::TaskEntry& entry,
        FILETIME& outFileTime) noexcept
    {
        outFileTime = FILETIME{};

        if (entry.schedule.type != TaskListManager::ScheduleType::OneShot) {
            return false;
        }

        if (entry.schedule.dueYmd <= 0 || entry.schedule.dueHHMM < 0) {
            return false;
        }

        return LocalReserveExecYmdHHMMSSToUtcFileTime_(
            entry.schedule.dueYmd,
            entry.schedule.dueHHMM,
            0,
            outFileTime);
    }

    bool IsProgramReservationTaskKind_(std::uint32_t kind) noexcept
    {
        return kind == TaskKinds::RecPrepare ||
            kind == TaskKinds::RecStart ||
            kind == TaskKinds::RecStop;
    }

    bool IsContinuationCandidateTaskKind_(std::uint32_t kind) noexcept
    {
        return kind == TaskKinds::RecPrepare ||
            kind == TaskKinds::RecStart;
    }

    bool IsFileTimeNear_(
        const FILETIME& base,
        const FILETIME& target,
        int beforeMinutes,
        int afterMinutes) noexcept
    {
        const ULONGLONG baseValue = FileTimeToU64ReserveExec_(base);
        const ULONGLONG targetValue = FileTimeToU64ReserveExec_(target);

        constexpr ULONGLONG kMinute100ns =
            60ULL * 1000ULL * 1000ULL * 10ULL;

        const ULONGLONG before =
            static_cast<ULONGLONG>((std::max)(0, beforeMinutes)) * kMinute100ns;

        const ULONGLONG after =
            static_cast<ULONGLONG>((std::max)(0, afterMinutes)) * kMinute100ns;

        const ULONGLONG minValue =
            (baseValue > before) ? (baseValue - before) : 0ULL;

        const ULONGLONG maxValue =
            baseValue + after;

        return targetValue >= minValue && targetValue <= maxValue;
    }

    std::wstring ReadReservationExecutionString_(
        const wchar_t* key,
        const wchar_t* fallback)
    {
        wchar_t buf[MAX_PATH]{};

        const std::wstring iniPath =
            ConfigManager::GetInstance().GetIniPath();

        if (iniPath.empty()) {
            return fallback ? fallback : L"";
        }

        ::GetPrivateProfileStringW(
            L"ReservationExecution",
            key ? key : L"",
            fallback ? fallback : L"",
            buf,
            static_cast<DWORD>(_countof(buf)),
            iniPath.c_str());

        return std::wstring(buf);
    }

    std::filesystem::path GetExeDirectoryForRecord_()
    {
        wchar_t exePath[MAX_PATH]{};
        const DWORD len =
            ::GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(_countof(exePath)));

        if (len == 0 || len >= _countof(exePath)) {
            return std::filesystem::current_path();
        }

        return std::filesystem::path(exePath).parent_path();
    }

    std::filesystem::path ResolveRecordFolder_()
    {
        std::wstring folder =
            ReadReservationExecutionString_(L"RecordFolder", L"");

        if (folder.empty()) {
            folder = (GetExeDirectoryForRecord_() / L"record").wstring();
        }

        std::filesystem::path p(folder);

        if (p.is_relative()) {
            p = GetExeDirectoryForRecord_() / p;
        }

        std::error_code ec{};
        std::filesystem::create_directories(p, ec);

        return p;
    }

    bool IsWindowsInvalidFileNameChar_(wchar_t ch) noexcept
    {
        if (ch < 0x20) {
            return true;
        }

        switch (ch) {
        case L'<':
        case L'>':
        case L':':
        case L'"':
        case L'/':
        case L'\\':
        case L'|':
        case L'?':
        case L'*':
            return true;

        default:
            return false;
        }
    }

    std::wstring SanitizeRecordFileBaseName_(std::wstring name)
    {
        if (name.empty()) {
            name = L"record";
        }

        for (auto& ch : name) {
            if (IsWindowsInvalidFileNameChar_(ch)) {
                ch = L'＿';
            }
        }

        while (!name.empty() &&
            (name.back() == L' ' || name.back() == L'.')) {
            name.pop_back();
        }

        while (!name.empty() &&
            (name.front() == L' ' || name.front() == L'.')) {
            name.erase(name.begin());
        }

        if (name.empty()) {
            name = L"record";
        }

        // Windows の予約名をざっくり回避。
        const std::wstring lowered = ToLowerTrim_(name);
        if (lowered == L"con" ||
            lowered == L"prn" ||
            lowered == L"aux" ||
            lowered == L"nul" ||
            lowered == L"com1" ||
            lowered == L"com2" ||
            lowered == L"com3" ||
            lowered == L"com4" ||
            lowered == L"com5" ||
            lowered == L"com6" ||
            lowered == L"com7" ||
            lowered == L"com8" ||
            lowered == L"com9" ||
            lowered == L"lpt1" ||
            lowered == L"lpt2" ||
            lowered == L"lpt3" ||
            lowered == L"lpt4" ||
            lowered == L"lpt5" ||
            lowered == L"lpt6" ||
            lowered == L"lpt7" ||
            lowered == L"lpt8" ||
            lowered == L"lpt9") {
            name = L"record_" + name;
        }

        constexpr std::size_t kMaxBaseName = 120;
        if (name.size() > kMaxBaseName) {
            name.resize(kMaxBaseName);
            while (!name.empty() &&
                (name.back() == L' ' || name.back() == L'.')) {
                name.pop_back();
            }
            if (name.empty()) {
                name = L"record";
            }
        }

        return name;
    }

    std::filesystem::path MakeUniqueRecordPath_(
        const TaskListManager::TaskEntry& entry)
    {
        const std::filesystem::path folder =
            ResolveRecordFolder_();

        const std::wstring baseName =
            SanitizeRecordFileBaseName_(
                !entry.reserve.titleLabel.empty()
                ? entry.reserve.titleLabel
                : (!entry.reserve.title.empty()
                    ? entry.reserve.title
                    : (!entry.display.label.empty()
                        ? entry.display.label
                        : entry.name)));

        const std::wstring ext = L".ts";

        std::filesystem::path first =
            folder / (baseName + ext);

        std::error_code ec{};
        if (!std::filesystem::exists(first, ec)) {
            return first;
        }

        for (int i = 1; i <= 9999; ++i) {
            std::filesystem::path candidate =
                folder / (baseName + L"（" + std::to_wstring(i) + L"）" + ext);

            ec.clear();
            if (!std::filesystem::exists(candidate, ec)) {
                return candidate;
            }
        }

        SYSTEMTIME st{};
        ::GetLocalTime(&st);

        wchar_t stamp[64]{};
        swprintf_s(
            stamp,
            L"_%04u%02u%02u_%02u%02u%02u",
            static_cast<unsigned>(st.wYear),
            static_cast<unsigned>(st.wMonth),
            static_cast<unsigned>(st.wDay),
            static_cast<unsigned>(st.wHour),
            static_cast<unsigned>(st.wMinute),
            static_cast<unsigned>(st.wSecond));

        return folder / (baseName + stamp + ext);
    }

    std::wstring QuoteTvTestCommandArg_(const std::wstring& text)
    {
        std::wstring out;
        out.reserve(text.size() + 2);
        out.push_back(L'"');

        for (wchar_t ch : text) {
            // TVTestBridge側の簡易パーサで壊れないよう、引用符は置換する。
            if (ch == L'"') {
                out.push_back(L'\'');
            }
            else {
                out.push_back(ch);
            }
        }

        out.push_back(L'"');
        return out;
    }

    struct InternalRecordVerifyState_
    {
        std::atomic_bool serviceLogged{ false };
        std::atomic_bool programLogged{ false };
        std::atomic_bool packetLogged{ false };

        // system.log は異常・補正系のみ。
        std::atomic_bool mismatchLogged{ false };
        std::atomic_bool nextMatchLogged{ false };
        std::atomic_bool adjustTriedLogged{ false };
        std::atomic_bool stopTimeAdjustedLogged{ false };
        std::atomic_bool stopByDelayLogged{ false };
    };

    std::uint16_t ReadU16BE_(const std::uint8_t* p) noexcept
    {
        return static_cast<std::uint16_t>(
            (static_cast<unsigned>(p[0]) << 8) |
            static_cast<unsigned>(p[1]));
    }

    bool GetTsPacketPayload_(
        const TsPacket& pkt,
        std::uint16_t& outPid,
        bool& outPayloadUnitStart,
        const std::uint8_t*& outPayload,
        std::size_t& outPayloadSize) noexcept
    {
        outPid = 0;
        outPayloadUnitStart = false;
        outPayload = nullptr;
        outPayloadSize = 0;

        const auto* p = pkt.data();
        const std::size_t n = pkt.size();

        if (p == nullptr || n < 188) {
            return false;
        }

        if (p[0] != 0x47) {
            return false;
        }

        outPayloadUnitStart = (p[1] & 0x40) != 0;
        outPid = static_cast<std::uint16_t>(
            ((static_cast<unsigned>(p[1]) & 0x1F) << 8) |
            static_cast<unsigned>(p[2]));

        const int afc = (p[3] >> 4) & 0x03;

        // 00: reserved, 10: adaptation only
        if (afc == 0 || afc == 2) {
            return false;
        }

        std::size_t pos = 4;

        if (afc == 3) {
            if (pos >= n) {
                return false;
            }

            const std::size_t adaptationLength = p[pos];
            pos += 1 + adaptationLength;

            if (pos >= n) {
                return false;
            }
        }

        outPayload = p + pos;
        outPayloadSize = n - pos;
        return outPayloadSize > 0;
    }

    bool GetPsiSectionStart_(
        const TsPacket& pkt,
        std::uint16_t expectedPid,
        const std::uint8_t*& outSection,
        std::size_t& outAvailableBytes) noexcept
    {
        outSection = nullptr;
        outAvailableBytes = 0;

        std::uint16_t pid = 0;
        bool payloadUnitStart = false;
        const std::uint8_t* payload = nullptr;
        std::size_t payloadSize = 0;

        if (!GetTsPacketPayload_(
            pkt,
            pid,
            payloadUnitStart,
            payload,
            payloadSize)) {
            return false;
        }

        if (pid != expectedPid || !payloadUnitStart) {
            return false;
        }

        if (payloadSize < 2) {
            return false;
        }

        const std::size_t pointerField = payload[0];

        if (1 + pointerField >= payloadSize) {
            return false;
        }

        outSection = payload + 1 + pointerField;
        outAvailableBytes = payloadSize - 1 - pointerField;

        return outAvailableBytes >= 8;
    }

    bool TryObservedFromPatPacket_(
        const TsPacket& pkt,
        const Reservation_TargetVerifier::ReservationTarget& target,
        Reservation_TargetVerifier::ObservedTarget& outObserved) noexcept
    {
        const std::uint8_t* sec = nullptr;
        std::size_t avail = 0;

        if (!GetPsiSectionStart_(pkt, 0x0000, sec, avail)) {
            return false;
        }

        if (avail < 12 || sec[0] != 0x00) {
            return false;
        }

        const std::size_t sectionLength =
            ((static_cast<std::size_t>(sec[1]) & 0x0F) << 8) |
            static_cast<std::size_t>(sec[2]);

        if (sectionLength < 9) {
            return false;
        }

        const std::uint16_t tsId = ReadU16BE_(sec + 3);

        const std::size_t sectionEndNoCrc =
            (std::min<std::size_t>)(avail, 3 + sectionLength - 4);

        if (sectionEndNoCrc <= 8) {
            return false;
        }

        bool foundTargetService = false;
        std::uint16_t firstServiceId = 0;

        for (std::size_t pos = 8; pos + 4 <= sectionEndNoCrc; pos += 4) {
            const std::uint16_t programNumber = ReadU16BE_(sec + pos);

            // program_number 0 は NIT PID。
            if (programNumber == 0) {
                continue;
            }

            if (firstServiceId == 0) {
                firstServiceId = programNumber;
            }

            if (programNumber == target.serviceId) {
                foundTargetService = true;
                break;
            }
        }

        if (firstServiceId == 0) {
            return false;
        }

        outObserved = Reservation_TargetVerifier::ObservedTarget{};
        outObserved.space = target.space;
        outObserved.networkId = target.networkId; // PATでは取得できないため、networkId照合は無効化して使う。
        outObserved.tsId = tsId;
        outObserved.serviceId = foundTargetService ? target.serviceId : firstServiceId;
        outObserved.serviceName = target.serviceName;

        return true;
    }

    bool TryObservedFromEitPacket_(
        const TsPacket& pkt,
        const Reservation_TargetVerifier::ReservationTarget& target,
        Reservation_TargetVerifier::ObservedTarget& outObserved) noexcept
    {
        const std::uint8_t* sec = nullptr;
        std::size_t avail = 0;

        if (!GetPsiSectionStart_(pkt, 0x0012, sec, avail)) {
            return false;
        }

        if (avail < 18) {
            return false;
        }

        const std::uint8_t tableId = sec[0];

        if (!((tableId >= 0x4E && tableId <= 0x6F))) {
            return false;
        }

        const std::size_t sectionLength =
            ((static_cast<std::size_t>(sec[1]) & 0x0F) << 8) |
            static_cast<std::size_t>(sec[2]);

        if (sectionLength < 15) {
            return false;
        }

        const std::uint16_t serviceId = ReadU16BE_(sec + 3);
        const std::uint16_t tsId = ReadU16BE_(sec + 8);
        const std::uint16_t originalNetworkId = ReadU16BE_(sec + 10);

        const std::size_t sectionEndNoCrc =
            (std::min<std::size_t>)(avail, 3 + sectionLength - 4);

        if (sectionEndNoCrc <= 14) {
            return false;
        }

        // 予約対象サービス以外のEITは照合対象にしない。
        if (target.serviceId != 0 && serviceId != target.serviceId) {
            return false;
        }

        for (std::size_t pos = 14; pos + 12 <= sectionEndNoCrc;) {
            const std::uint16_t eventId = ReadU16BE_(sec + pos);

            const std::size_t descLoopLen =
                ((static_cast<std::size_t>(sec[pos + 10]) & 0x0F) << 8) |
                static_cast<std::size_t>(sec[pos + 11]);

            outObserved = Reservation_TargetVerifier::ObservedTarget{};
            outObserved.space = target.space;
            outObserved.networkId = originalNetworkId;
            outObserved.tsId = tsId;
            outObserved.serviceId = serviceId;
            outObserved.eventId = eventId;
            outObserved.serviceName = target.serviceName;

            if (target.eventId == 0 || eventId == target.eventId) {
                return true;
            }

            const std::size_t next = pos + 12 + descLoopLen;
            if (next <= pos) {
                break;
            }

            pos = next;
        }

        return false;
    }

    bool TryBuildObservedTargetFromPackets_(
        const Reservation_RawTsRecorder::PacketBlock& block,
        const Reservation_TargetVerifier::ReservationTarget& target,
        Reservation_TargetVerifier::ObservedTarget& outObserved,
        bool& outFromEit) noexcept
    {
        outObserved = Reservation_TargetVerifier::ObservedTarget{};
        outFromEit = false;

        if (block.packets == nullptr) {
            return false;
        }

        // EITが取れたら番組 eventId まで見られるので優先。
        for (const auto& pkt : *block.packets) {
            if (TryObservedFromEitPacket_(pkt, target, outObserved)) {
                outFromEit = true;
                return true;
            }
        }

        // EITがまだ来ていない場合は、PATで serviceId / tsId の確認だけ行う。
        for (const auto& pkt : *block.packets) {
            if (TryObservedFromPatPacket_(pkt, target, outObserved)) {
                outFromEit = false;
                return true;
            }
        }

        return false;
    }

    constexpr long long kInternalRecordDelayStopMinutes_ = 10;
    // 録画ついでのEIT収集時間。
// 0にすると録画終了まで解析し続ける。
// 便乗更新用途なら3分程度で十分と見て、まずは180秒に固定する。
    constexpr std::uint32_t kRecordingEitAnalyzeSeconds_ = 180;

    struct InternalRecordPfObserved_
    {
        bool hasNow = false;
        bool hasNext = false;

        Reservation_TargetVerifier::ObservedTarget now{};
        Reservation_TargetVerifier::ObservedTarget next{};
    };

    int BcdToInt_(std::uint8_t b) noexcept
    {
        return ((b >> 4) & 0x0F) * 10 + (b & 0x0F);
    }

    bool MjdToYmd_(std::uint16_t mjd, int& y, int& m, int& d) noexcept
    {
        int j = static_cast<int>(mjd) + 2400001 + 68569;
        const int c = 4 * j / 146097;
        j = j - (146097 * c + 3) / 4;
        const int yy = 4000 * (j + 1) / 1461001;
        j = j - 1461 * yy / 4 + 31;
        const int mm = 80 * j / 2447;
        d = j - 2447 * mm / 80;
        j = mm / 11;
        m = mm + 2 - 12 * j;
        y = 100 * (c - 49) + yy + j;

        return y >= 1900 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
    }

    std::wstring MakeIsoDate_(int y, int m, int d)
    {
        wchar_t buf[16]{};
        swprintf_s(buf, L"%04d-%02d-%02d", y, m, d);
        return std::wstring(buf);
    }

    std::wstring MakeIsoTime_(int hh, int mm, int ss)
    {
        wchar_t buf[16]{};
        swprintf_s(buf, L"%02d:%02d:%02d", hh, mm, ss);
        return std::wstring(buf);
    }

    bool DecodeEitStartAndDuration_(
        const std::uint8_t* p,
        std::wstring& startDate,
        std::wstring& startTime,
        std::wstring& endDate,
        std::wstring& endTime,
        int& durationSec) noexcept
    {
        startDate.clear();
        startTime.clear();
        endDate.clear();
        endTime.clear();
        durationSec = 0;

        if (p == nullptr) {
            return false;
        }

        const std::uint16_t mjd = ReadU16BE_(p);

        int y = 0;
        int m = 0;
        int d = 0;
        if (!MjdToYmd_(mjd, y, m, d)) {
            return false;
        }

        const int hh = BcdToInt_(p[2]);
        const int mm = BcdToInt_(p[3]);
        const int ss = BcdToInt_(p[4]);

        const int dh = BcdToInt_(p[5]);
        const int dm = BcdToInt_(p[6]);
        const int ds = BcdToInt_(p[7]);

        if (hh > 47 || mm > 59 || ss > 59 ||
            dh > 47 || dm > 59 || ds > 59) {
            return false;
        }

        durationSec = dh * 3600 + dm * 60 + ds;

        SYSTEMTIME st{};
        st.wYear = static_cast<WORD>(y);
        st.wMonth = static_cast<WORD>(m);
        st.wDay = static_cast<WORD>(d);
        st.wHour = static_cast<WORD>(hh % 24);
        st.wMinute = static_cast<WORD>(mm);
        st.wSecond = static_cast<WORD>(ss);
        st.wMilliseconds = 0;

        FILETIME ft{};
        if (!::SystemTimeToFileTime(&st, &ft)) {
            return false;
        }

        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        u.QuadPart += static_cast<ULONGLONG>(durationSec) * 10000000ULL;

        FILETIME endFt{};
        endFt.dwLowDateTime = u.LowPart;
        endFt.dwHighDateTime = u.HighPart;

        SYSTEMTIME et{};
        if (!::FileTimeToSystemTime(&endFt, &et)) {
            return false;
        }

        startDate = MakeIsoDate_(y, m, d);
        startTime = MakeIsoTime_(hh % 24, mm, ss);

        endDate = MakeIsoDate_(et.wYear, et.wMonth, et.wDay);
        endTime = MakeIsoTime_(et.wHour, et.wMinute, et.wSecond);

        return true;
    }

    bool TryIsoDateTimeToFileTime_(
        const std::wstring& date,
        const std::wstring& time,
        FILETIME& outFileTime) noexcept
    {
        outFileTime = FILETIME{};

        int ymd = 0;
        int hhmm = -1;
        int sec = 0;

        if (!ParseReserveExecIsoYmd_(date, ymd)) {
            return false;
        }

        if (!ParseReserveExecIsoHHMMSS_(time, hhmm, sec)) {
            return false;
        }

        return LocalReserveExecYmdHHMMSSToUtcFileTime_(
            ymd,
            hhmm,
            sec,
            outFileTime);
    }

    bool TryCalcStartDelayMinutes_(
        const Reservation_TargetVerifier::ReservationTarget& target,
        const Reservation_TargetVerifier::ObservedTarget& observed,
        long long& outDelayMinutes) noexcept
    {
        outDelayMinutes = 0;

        if (target.startDate.empty() ||
            target.startTime.empty() ||
            observed.startDate.empty() ||
            observed.startTime.empty()) {
            return false;
        }

        FILETIME targetFt{};
        FILETIME observedFt{};

        if (!TryIsoDateTimeToFileTime_(target.startDate, target.startTime, targetFt)) {
            return false;
        }

        if (!TryIsoDateTimeToFileTime_(observed.startDate, observed.startTime, observedFt)) {
            return false;
        }

        const ULONGLONG targetValue = FileTimeToU64ReserveExec_(targetFt);
        const ULONGLONG observedValue = FileTimeToU64ReserveExec_(observedFt);

        constexpr ULONGLONG kMinute100ns =
            60ULL * 1000ULL * 1000ULL * 10ULL;

        if (observedValue >= targetValue) {
            outDelayMinutes =
                static_cast<long long>((observedValue - targetValue) / kMinute100ns);
        }
        else {
            outDelayMinutes =
                -static_cast<long long>((targetValue - observedValue) / kMinute100ns);
        }

        return true;
    }

    bool TryObservedPfFromEitPacket_(
        const TsPacket& pkt,
        const Reservation_TargetVerifier::ReservationTarget& target,
        InternalRecordPfObserved_& outPf) noexcept
    {
        const std::uint8_t* sec = nullptr;
        std::size_t avail = 0;

        if (!GetPsiSectionStart_(pkt, 0x0012, sec, avail)) {
            return false;
        }

        if (avail < 18) {
            return false;
        }

        const std::uint8_t tableId = sec[0];

        // present/following actual TS のみ。
        if (tableId != 0x4E) {
            return false;
        }

        const std::size_t sectionLength =
            ((static_cast<std::size_t>(sec[1]) & 0x0F) << 8) |
            static_cast<std::size_t>(sec[2]);

        if (sectionLength < 15) {
            return false;
        }

        const std::uint16_t serviceId = ReadU16BE_(sec + 3);
        const std::uint8_t sectionNumber = sec[6];
        const std::uint16_t tsId = ReadU16BE_(sec + 8);
        const std::uint16_t originalNetworkId = ReadU16BE_(sec + 10);

        if (target.serviceId != 0 && serviceId != target.serviceId) {
            return false;
        }

        const std::size_t sectionEndNoCrc =
            (std::min<std::size_t>)(avail, 3 + sectionLength - 4);

        if (sectionEndNoCrc <= 14) {
            return false;
        }

        const std::size_t pos = 14;
        if (pos + 12 > sectionEndNoCrc) {
            return false;
        }

        const std::uint16_t eventId = ReadU16BE_(sec + pos);

        Reservation_TargetVerifier::ObservedTarget observed{};
        observed.space = target.space;
        observed.networkId = originalNetworkId;
        observed.tsId = tsId;
        observed.serviceId = serviceId;
        observed.eventId = eventId;
        observed.serviceName = target.serviceName;

        std::wstring startDate;
        std::wstring startTime;
        std::wstring endDate;
        std::wstring endTime;
        int durationSec = 0;

        if (DecodeEitStartAndDuration_(
            sec + pos + 2,
            startDate,
            startTime,
            endDate,
            endTime,
            durationSec)) {
            observed.startDate = startDate;
            observed.startTime = startTime;
            observed.endDate = endDate;
            observed.endTime = endTime;
        }

        if (sectionNumber == 0) {
            outPf.now = observed;
            outPf.hasNow = true;
            return true;
        }

        if (sectionNumber == 1) {
            outPf.next = observed;
            outPf.hasNext = true;
            return true;
        }

        return false;
    }

    bool TryBuildPfObservedFromPackets_(
        const Reservation_RawTsRecorder::PacketBlock& block,
        const Reservation_TargetVerifier::ReservationTarget& target,
        InternalRecordPfObserved_& outPf) noexcept
    {
        outPf = InternalRecordPfObserved_{};

        if (block.packets == nullptr) {
            return false;
        }

        for (const auto& pkt : *block.packets) {
            (void)TryObservedPfFromEitPacket_(pkt, target, outPf);

            if (outPf.hasNow && outPf.hasNext) {
                return true;
            }
        }

        return outPf.hasNow || outPf.hasNext;
    }
}//namespace

void Reservation_ExecutionManager::EnsureDefaultConfig(ConfigManager& cfg)
{
    Reservation_Config::EnsureDefaults(cfg);
}

void Reservation_ExecutionManager::ConfigureFromConfig(ConfigManager& cfg)
{
    Configure(Reservation_Config::Load(cfg));
}

void Reservation_ExecutionManager::Configure(const Reservation_ExecutionSettings& settings)
{
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = settings;
    eitMonitor_.Configure(settings_);
}

void Reservation_ExecutionManager::SetTaskSystem(TaskSystemConnector* taskSystem) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    taskSystem_ = taskSystem;
}

void Reservation_ExecutionManager::Reset() noexcept
{
    rawTsRecorder_.ResetAll();
    recordingEitAnalyzer_.ResetAll();

    // EIT監視スレッドの停止は mutex_ の外で行う。
    eitMonitor_.Reset();

    std::lock_guard<std::mutex> lock(mutex_);
    active_.clear();
    internalRecordOwnerByTuner_.clear();
    tvTestRecordOwnerByTuner_.clear();
}

std::wstring Reservation_ExecutionManager::SafeTitle_(
    const TaskListManager::TaskEntry& entry)
{
    if (!entry.reserve.titleLabel.empty()) {
        return entry.reserve.titleLabel;
    }

    if (!entry.reserve.title.empty()) {
        return entry.reserve.title;
    }

    if (!entry.display.label.empty()) {
        return entry.display.label;
    }

    return entry.name.empty() ? L"(番組名なし)" : entry.name;
}

std::wstring Reservation_ExecutionManager::SafeService_(
    const TaskListManager::TaskEntry& entry)
{
    if (!entry.reserve.serviceName.empty()) {
        return entry.reserve.serviceName;
    }

    if (!entry.display.target.empty()) {
        return entry.display.target;
    }

    return L"(放送局不明)";
}

std::wstring Reservation_ExecutionManager::ModeLabel_(
    const TaskListManager::TaskEntry& entry)
{
    if (IsWatch_(entry)) {
        return L"視聴";
    }

    if (IsRecord_(entry)) {
        return L"録画";
    }

    return L"予約";
}

void Reservation_ExecutionManager::WriteSystemStartLog_(
    const TaskListManager::TaskEntry& entry,
    int tunerIndex,
    const Reservation_ExecutionSettings& settings)
{
    const wchar_t* backend = L"不明";

    if (IsRecord_(entry)) {
        backend = settings.enableTvTestWatch
            ? L"TVTest録画"
            : L"内部RawTS録画";
    }
    else if (IsWatch_(entry)) {
        backend = settings.enableTvTestWatch
            ? L"TVTest視聴"
            : L"内部視聴";
    }

    SystemInfoLogInfo(
        L"%ls予約の開始時刻になりました。番組=\"%ls\" 放送局=\"%ls\" 実行方式=%ls チューナー=T%d",
        ModeLabel_(entry).c_str(),
        SafeTitle_(entry).c_str(),
        SafeService_(entry).c_str(),
        backend,
        tunerIndex + 1);
}

void Reservation_ExecutionManager::WriteSystemSelectedLog_(
    const TaskListManager::TaskEntry& entry)
{
    SystemInfoLogInfo(
        L"%ls予約のチャンネル切替が完了しました。番組=\"%ls\" 放送局=\"%ls\" 結果=成功",
        ModeLabel_(entry).c_str(),
        SafeTitle_(entry).c_str(),
        SafeService_(entry).c_str());
}

void Reservation_ExecutionManager::WriteSystemEndLog_(
    const TaskListManager::TaskEntry& entry,
    bool succeeded)
{
    if (succeeded) {
        SystemInfoLogInfo(
            L"%ls予約を終了しました。番組=\"%ls\" 放送局=\"%ls\" 結果=成功",
            ModeLabel_(entry).c_str(),
            SafeTitle_(entry).c_str(),
            SafeService_(entry).c_str());
        return;
    }

    SystemInfoLogWarn(
        L"%ls予約を終了しました。番組=\"%ls\" 放送局=\"%ls\" 結果=警告",
        ModeLabel_(entry).c_str(),
        SafeTitle_(entry).c_str(),
        SafeService_(entry).c_str());
}

void Reservation_ExecutionManager::WriteSystemFailLog_(
    const TaskListManager::TaskEntry& entry,
    const wchar_t* stage,
    const std::wstring& reason)
{
    SystemInfoLogWarn(
        L"%ls予約の実行に失敗しました。番組=\"%ls\" 放送局=\"%ls\" 段階=%ls 理由=%ls",
        ModeLabel_(entry).c_str(),
        SafeTitle_(entry).c_str(),
        SafeService_(entry).c_str(),
        stage ? stage : L"(unknown)",
        reason.empty() ? L"(unknown)" : reason.c_str());
}

bool Reservation_ExecutionManager::IsWatch_(
    const TaskListManager::TaskEntry& entry) noexcept
{
    return ToLowerTrim_(entry.reserve.reserveMode) == L"watch";
}

bool Reservation_ExecutionManager::IsRecord_(
    const TaskListManager::TaskEntry& entry) noexcept
{
    return ToLowerTrim_(entry.reserve.reserveMode) == L"record";
}
bool Reservation_ExecutionManager::FindReservationEntry_(
    std::uint32_t instance,
    TaskListManager::TaskEntry& out) const
{
    TaskSystemConnector* taskSystem = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        taskSystem = taskSystem_;
    }

    if (taskSystem == nullptr) {
        return false;
    }

    const auto snap = taskSystem->GetTaskListManager().GetSnapshot();

    for (const auto& t : snap) {
        if (t.request.instance == instance && t.request.kind == TaskKinds::RecStart) {
            out = t;
            return true;
        }
    }

    for (const auto& t : snap) {
        if (t.request.instance != instance) continue;
        if (t.request.kind == TaskKinds::RecPrepare ||
            t.request.kind == TaskKinds::RecStart ||
            t.request.kind == TaskKinds::RecStop) {
            out = t;
            return true;
        }
    }

    return false;
}

Reservation_TvTestController Reservation_ExecutionManager::BuildTvTestController_(
    int tunerIndex,
    bool recordLaunchMode) const
{
    Reservation_ExecutionSettings settings{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings = settings_;
    }

    Reservation_TvTestController controller;
    controller.Configure(settings);

    // 重要:
    // ConfigureForTuner() の前に録画起動モードを渡す。
    // ConfigureForTuner() 内で /nodshow 追加判定を行うため。
    controller.SetRecordLaunchMode(recordLaunchMode);

    controller.ConfigureForTuner(ConfigManager::GetInstance(), tunerIndex);
    return controller;
}

void Reservation_ExecutionManager::UpdateActivePrepare_(
    const TaskListManager::TaskEntry& entry,
    const TimeKeeper::TaskRequest& req)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ActiveState& st = active_[req.instance];
    st.prepareSeen = true;
    st.title = SafeTitle_(entry);
    st.serviceName = SafeService_(entry);
    st.reserveMode = entry.reserve.reserveMode;
}

void Reservation_ExecutionManager::UpdateActivePrepareResult_(
    const TaskListManager::TaskEntry& entry,
    const TimeKeeper::TaskRequest& req,
    bool tvtestReady,
    bool serviceSelected)
{
    std::lock_guard<std::mutex> lock(mutex_);

    ActiveState& st = active_[req.instance];

    st.prepareSeen = true;
    st.prepareSucceeded = tvtestReady && serviceSelected;
    st.preparedTvTestReady = tvtestReady;
    st.preparedServiceSelected = serviceSelected;

    st.anyFailure = st.anyFailure || !(tvtestReady && serviceSelected);

    st.title = SafeTitle_(entry);
    st.serviceName = SafeService_(entry);
    st.reserveMode = entry.reserve.reserveMode;
}

bool Reservation_ExecutionManager::WasServicePrepared_(
    std::uint32_t instance) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = active_.find(instance);
    if (it == active_.end()) {
        return false;
    }

    const ActiveState& st = it->second;

    return st.prepareSeen &&
        st.prepareSucceeded &&
        st.preparedTvTestReady &&
        st.preparedServiceSelected;
}

void Reservation_ExecutionManager::SetActiveRecordFilePath_(
    std::uint32_t instance,
    const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    active_[instance].recordFilePath = path;
}

std::wstring Reservation_ExecutionManager::GetActiveRecordFilePath_(
    std::uint32_t instance) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = active_.find(instance);
    if (it == active_.end()) {
        return L"";
    }

    return it->second.recordFilePath;
}

bool Reservation_ExecutionManager::IsInternalRecordStartingOrRunning_(
    std::uint32_t instance) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = active_.find(instance);
    if (it == active_.end()) {
        return false;
    }

    const ActiveState& st = it->second;
    return st.internalRecordStarting || st.internalRecordRunning;
}

bool Reservation_ExecutionManager::IsInternalRecordOwner_(
    int tunerIndex,
    std::uint32_t instance) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = internalRecordOwnerByTuner_.find(tunerIndex);
    return it != internalRecordOwnerByTuner_.end() &&
        it->second == instance;
}

std::uint32_t Reservation_ExecutionManager::GetInternalRecordOwnerOnTuner_(
    int tunerIndex) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = internalRecordOwnerByTuner_.find(tunerIndex);
    if (it == internalRecordOwnerByTuner_.end()) {
        return 0;
    }

    return it->second;
}

bool Reservation_ExecutionManager::TryAcquireInternalRecordTuner_(
    int tunerIndex,
    std::uint32_t instance,
    std::uint32_t& busyInstance)
{
    busyInstance = 0;

    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = internalRecordOwnerByTuner_.find(tunerIndex);
    if (it != internalRecordOwnerByTuner_.end()) {
        if (it->second == instance) {
            active_[instance].internalRecordTunerIndex = tunerIndex;
            return true;
        }

        busyInstance = it->second;
        return false;
    }

    internalRecordOwnerByTuner_[tunerIndex] = instance;
    active_[instance].internalRecordTunerIndex = tunerIndex;
    return true;
}

void Reservation_ExecutionManager::ReleaseInternalRecordTuner_(
    int tunerIndex,
    std::uint32_t instance) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = internalRecordOwnerByTuner_.find(tunerIndex);
    if (it != internalRecordOwnerByTuner_.end() &&
        it->second == instance) {
        internalRecordOwnerByTuner_.erase(it);
    }
}

bool Reservation_ExecutionManager::StopInternalRecordOwnerForPreparePriority_(
    int tunerIndex,
    std::uint32_t requestInstance,
    const TaskListManager::TaskEntry& requestEntry)
{
    const std::uint32_t owner =
        GetInternalRecordOwnerOnTuner_(tunerIndex);

    if (owner == 0 || owner == requestInstance) {
        return true;
    }

    SystemInfoLogWarn(
        L"次予約の準備時間に入ったため、同一チューナーの前録画を停止します。次番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d 前instance=%u 次instance=%u",
        SafeTitle_(requestEntry).c_str(),
        SafeService_(requestEntry).c_str(),
        tunerIndex + 1,
        static_cast<unsigned>(owner),
        static_cast<unsigned>(requestInstance));

    DebugLogFormat(
        L"[ReservationExecution][PREPARE-PRIORITY] stop previous internal record tuner=%d owner=%u request=%u title=%ls service=%ls",
        tunerIndex,
        static_cast<unsigned>(owner),
        static_cast<unsigned>(requestInstance),
        SafeTitle_(requestEntry).c_str(),
        SafeService_(requestEntry).c_str());

    const auto rr = rawTsRecorder_.Stop(owner);

    const auto eitFlush =
        recordingEitAnalyzer_.Flush(owner);

    NotifyRecordingEitUpdated_(owner,eitFlush);

    recordingEitAnalyzer_.Reset(owner);

    DebugLogFormat(
        L"[ReservationExecution][PREPARE-PRIORITY-EIT-FLUSH] owner=%u ok=%d wrote=%d appended=%d raw=%llu unique=%llu input=%llu eitPkt=%llu path=%ls reason=%ls",
        static_cast<unsigned>(owner),
        eitFlush.ok ? 1 : 0,
        eitFlush.wrote ? 1 : 0,
        eitFlush.appended ? 1 : 0,
        static_cast<unsigned long long>(eitFlush.rawEvents),
        static_cast<unsigned long long>(eitFlush.uniqueEvents),
        static_cast<unsigned long long>(eitFlush.inputPackets),
        static_cast<unsigned long long>(eitFlush.eitPackets),
        eitFlush.outputPath.empty() ? L"(empty)" : eitFlush.outputPath.c_str(),
        eitFlush.reason.empty() ? L"(none)" : eitFlush.reason.c_str());

    MarkInternalRecordStopped_(owner, tunerIndex);

    DebugLogFormat(
        L"[ReservationExecution][PREPARE-PRIORITY] previous stop result owner=%u ok=%d bytes=%llu packets=%llu reason=%ls",
        static_cast<unsigned>(owner),
        rr.ok ? 1 : 0,
        static_cast<unsigned long long>(rr.bytesWritten),
        static_cast<unsigned long long>(rr.packetsWritten),
        rr.reason.c_str());

    // 安全優先。Stop対象セッションが無くても所有権は解放済みにする。
    return true;
}

bool Reservation_ExecutionManager::TryBeginInternalRecordStart_(
    std::uint32_t instance,
    int tunerIndex,
    const std::wstring& recordPath,
    bool& alreadyActive)
{
    alreadyActive = false;

    std::lock_guard<std::mutex> lock(mutex_);

    ActiveState& st = active_[instance];

    if (st.internalRecordStarting || st.internalRecordRunning) {
        alreadyActive = true;
        return false;
    }

    st.internalRecordStartAttempted = true;
    st.internalRecordStarting = true;
    st.internalRecordRunning = false;
    st.internalRecordStartFailed = false;
    st.internalRecordTunerIndex = tunerIndex;

    if (!recordPath.empty()) {
        st.recordFilePath = recordPath;
    }

    return true;
}

void Reservation_ExecutionManager::FinishInternalRecordStart_(
    std::uint32_t instance,
    bool succeeded) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    ActiveState& st = active_[instance];

    st.internalRecordStartAttempted = true;
    st.internalRecordStarting = false;
    st.internalRecordRunning = succeeded;
    st.internalRecordStartFailed = !succeeded;

    // 開始失敗時も予定保存先は消さない。
}

void Reservation_ExecutionManager::MarkInternalRecordStopped_(
    std::uint32_t instance,
    int tunerIndex) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = active_.find(instance);
    if (it != active_.end()) {
        it->second.internalRecordStarting = false;
        it->second.internalRecordRunning = false;
        it->second.internalRecordTunerIndex = -1;
    }

    const auto ownerIt = internalRecordOwnerByTuner_.find(tunerIndex);
    if (ownerIt != internalRecordOwnerByTuner_.end() &&
        ownerIt->second == instance) {
        internalRecordOwnerByTuner_.erase(ownerIt);
    }
}

bool Reservation_ExecutionManager::IsTvTestRecordStartingOrRunning_(
    std::uint32_t instance) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = active_.find(instance);
    if (it == active_.end()) {
        return false;
    }

    const ActiveState& st = it->second;
    return st.tvTestRecordStarting || st.tvTestRecordRunning;
}

bool Reservation_ExecutionManager::IsTvTestRecordOwner_(
    int tunerIndex,
    std::uint32_t instance) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = tvTestRecordOwnerByTuner_.find(tunerIndex);
    return it != tvTestRecordOwnerByTuner_.end() &&
        it->second == instance;
}

bool Reservation_ExecutionManager::WasTvTestRecordStartFailed_(
    std::uint32_t instance) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = active_.find(instance);
    if (it == active_.end()) {
        return false;
    }

    const ActiveState& st = it->second;

    return st.tvTestRecordStartAttempted &&
        st.tvTestRecordStartFailed &&
        !st.tvTestRecordRunning;
}

std::uint32_t Reservation_ExecutionManager::GetTvTestRecordOwnerOnTuner_(
    int tunerIndex) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = tvTestRecordOwnerByTuner_.find(tunerIndex);
    if (it == tvTestRecordOwnerByTuner_.end()) {
        return 0;
    }

    return it->second;
}

bool Reservation_ExecutionManager::TryAcquireTvTestRecordTuner_(
    int tunerIndex,
    std::uint32_t instance,
    std::uint32_t& busyInstance)
{
    busyInstance = 0;

    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = tvTestRecordOwnerByTuner_.find(tunerIndex);
    if (it != tvTestRecordOwnerByTuner_.end()) {
        if (it->second == instance) {
            active_[instance].tvTestRecordTunerIndex = tunerIndex;
            return true;
        }

        busyInstance = it->second;
        return false;
    }

    tvTestRecordOwnerByTuner_[tunerIndex] = instance;
    active_[instance].tvTestRecordTunerIndex = tunerIndex;
    return true;
}

void Reservation_ExecutionManager::ReleaseTvTestRecordTuner_(
    int tunerIndex,
    std::uint32_t instance) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = tvTestRecordOwnerByTuner_.find(tunerIndex);
    if (it != tvTestRecordOwnerByTuner_.end() &&
        it->second == instance) {
        tvTestRecordOwnerByTuner_.erase(it);
    }
}

bool Reservation_ExecutionManager::TryBeginTvTestRecordStart_(
    std::uint32_t instance,
    int tunerIndex,
    const std::wstring& recordPath,
    bool& alreadyActive)
{
    alreadyActive = false;

    std::lock_guard<std::mutex> lock(mutex_);

    ActiveState& st = active_[instance];

    if (st.tvTestRecordStarting || st.tvTestRecordRunning) {
        alreadyActive = true;
        return false;
    }

    st.tvTestRecordStartAttempted = true;
    st.tvTestRecordStarting = true;
    st.tvTestRecordRunning = false;
    st.tvTestRecordStartFailed = false;
    st.tvTestRecordTunerIndex = tunerIndex;

    if (!recordPath.empty()) {
        st.recordFilePath = recordPath;
    }

    return true;
}

void Reservation_ExecutionManager::FinishTvTestRecordStart_(
    std::uint32_t instance,
    bool succeeded) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    ActiveState& st = active_[instance];

    st.tvTestRecordStartAttempted = true;
    st.tvTestRecordStarting = false;
    st.tvTestRecordRunning = succeeded;
    st.tvTestRecordStartFailed = !succeeded;

    // 重要:
    // 開始失敗時でも、予定保存先はログ・UI・後続Stop判定に必要なので消さない。
    // recordFilePath は TryBeginTvTestRecordStart_() / SetActiveRecordFilePath_() で保持する。
}



void Reservation_ExecutionManager::MarkTvTestRecordStopped_(
    std::uint32_t instance,
    int tunerIndex) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = active_.find(instance);
    if (it != active_.end()) {
        it->second.tvTestRecordStarting = false;
        it->second.tvTestRecordRunning = false;
        it->second.tvTestRecordTunerIndex = -1;
    }

    const auto ownerIt = tvTestRecordOwnerByTuner_.find(tunerIndex);
    if (ownerIt != tvTestRecordOwnerByTuner_.end() &&
        ownerIt->second == instance) {
        tvTestRecordOwnerByTuner_.erase(ownerIt);
    }
}

bool Reservation_ExecutionManager::TryGetReserveStartTimeMs_(
    const TaskListManager::TaskEntry& entry,
    ULONGLONG& outStartTickMs,
    DWORD fallbackMaxMs) noexcept
{
    const ULONGLONG nowTick = ::GetTickCount64();

    outStartTickMs = nowTick + static_cast<ULONGLONG>(fallbackMaxMs);

    int startYmd = 0;
    int startHHMM = -1;
    int startSec = 0;

    if (!ParseReserveExecIsoYmd_(entry.reserve.startDate, startYmd)) {
        return false;
    }

    if (!ParseReserveExecIsoHHMMSS_(entry.reserve.startTime, startHHMM, startSec)) {
        return false;
    }

    FILETIME targetFt{};
    if (!LocalReserveExecYmdHHMMSSToUtcFileTime_(
        startYmd,
        startHHMM,
        startSec,
        targetFt)) {
        return false;
    }

    FILETIME nowFt{};
    ::GetSystemTimeAsFileTime(&nowFt);

    const ULONGLONG target = FileTimeToU64ReserveExec_(targetFt);
    const ULONGLONG now = FileTimeToU64ReserveExec_(nowFt);

    if (target <= now) {
        outStartTickMs = nowTick;
        return true;
    }

    constexpr ULONGLONG kMsTo100ns = 10000ULL;
    const ULONGLONG diffMs = (target - now) / kMsTo100ns;

    outStartTickMs = nowTick + diffMs;
    return true;
}



void Reservation_ExecutionManager::UpdateActiveStart_(
    const TaskListManager::TaskEntry& entry,
    const TimeKeeper::TaskRequest& req,
    bool succeeded)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ActiveState& st = active_[req.instance];
    st.startSeen = true;
    st.startSucceeded = succeeded;
    st.anyFailure = st.anyFailure || !succeeded;
    st.title = SafeTitle_(entry);
    st.serviceName = SafeService_(entry);
    st.reserveMode = entry.reserve.reserveMode;
}

void Reservation_ExecutionManager::UpdateActiveStop_(
    const TaskListManager::TaskEntry& entry,
    const TimeKeeper::TaskRequest& req,
    bool succeeded)
{
    ActiveState snapshot{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ActiveState& st = active_[req.instance];
        st.stopSeen = true;
        st.stopSucceeded = succeeded;
        st.anyFailure = st.anyFailure || !succeeded;
        st.title = SafeTitle_(entry);
        st.serviceName = SafeService_(entry);
        st.reserveMode = entry.reserve.reserveMode;
        snapshot = st;
    }

    LogBothFailedIfNeeded_(req.instance, snapshot);
}

void Reservation_ExecutionManager::LogBothFailedIfNeeded_(std::uint32_t instance, const ActiveState& st) const
{
    if (st.startSeen && st.stopSeen && !st.startSucceeded && !st.stopSucceeded) {
        SystemInfoLogError(
            L"予約実行確認に失敗しました。開始処理と終了処理の両方に失敗しました。instance=%u title=%ls service=%ls mode=%ls",
            static_cast<unsigned>(instance),
            st.title.c_str(),
            st.serviceName.c_str(),
            st.reserveMode.c_str());
    }
}

bool Reservation_ExecutionManager::CompleteProgramReservationGroup_(
    std::uint32_t instance)
{
    TaskSystemConnector* taskSystem = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        taskSystem = taskSystem_;
    }

    if (taskSystem == nullptr) {
        DebugLogFormat(
            L"[ReservationExecution][STOP] complete skipped: taskSystem is null inst=%u",
            static_cast<unsigned>(instance));
        return false;
    }

    const bool ok = taskSystem->RemoveProgramReservationGroupAndApply(instance);

    DebugLogFormat(
        L"[ReservationExecution][STOP] complete reservation group inst=%u ok=%d",
        static_cast<unsigned>(instance),
        ok ? 1 : 0);

    return ok;
}

bool Reservation_ExecutionManager::HasNearbyFollowingProgramReservationOnSameTuner_(
    const TaskListManager::TaskEntry& currentEntry,
    const TimeKeeper::TaskRequest& req,
    int tunerIndex) const
{
    TaskSystemConnector* taskSystem = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        taskSystem = taskSystem_;
    }

    if (taskSystem == nullptr) {
        return false;
    }

    FILETIME currentEndFt{};
    if (!TryEntryReserveEndFileTime_(currentEntry, currentEndFt)) {
        DebugLogFormat(
            L"[ReservationExecution][CONTINUE] current end time unavailable inst=%u",
            static_cast<unsigned>(req.instance));
        return false;
    }

    const auto snap = taskSystem->GetTaskListManager().GetSnapshot();

    for (const auto& t : snap) {
        if (!t.enabled) {
            continue;
        }

        if (t.request.instance == req.instance) {
            continue;
        }

        if (!IsContinuationCandidateTaskKind_(t.request.kind)) {
            continue;
        }

        if (t.request.preferredTunerIndex != tunerIndex) {
            continue;
        }

        FILETIME dueFt{};
        if (!TryTaskDueFileTime_(t, dueFt)) {
            continue;
        }

        // RecPrepare は開始30秒前でもHHMM粒度では1分前に見えることがある。
        // 連続予約として、前番組終了の5分前〜5分後に次予約のPrepare/Startがあれば
        // TVTestを閉じない。
        if (IsFileTimeNear_(currentEndFt, dueFt, 5, 5)) {
            DebugLogFormat(
                L"[ReservationExecution][CONTINUE] following reservation found currentInst=%u nextInst=%u nextKind=%u tuner=%d title=%ls",
                static_cast<unsigned>(req.instance),
                static_cast<unsigned>(t.request.instance),
                static_cast<unsigned>(t.request.kind),
                tunerIndex,
                SafeTitle_(t).c_str());

            return true;
        }
    }

    return false;
}

bool Reservation_ExecutionManager::ShouldKeepTvTestAliveAfterStop_(
    const TaskListManager::TaskEntry& currentEntry,
    const TimeKeeper::TaskRequest& req,
    int tunerIndex) const
{
    if (!IsProgramReservationTaskKind_(req.kind)) {
        return false;
    }

    return HasNearbyFollowingProgramReservationOnSameTuner_(
        currentEntry,
        req,
        tunerIndex);
}

void Reservation_ExecutionManager::OnRecPrepare(
    int tunerIndex,
    IBonDriver2* bon,
    const TimeKeeper::TaskRequest& req)
{
    DebugLogFormat(
        L"[ReservationExecution][PREPARE] enter tuner=%d kind=%u inst=%u",
        tunerIndex,
        static_cast<unsigned>(req.kind),
        static_cast<unsigned>(req.instance));

    Reservation_ExecutionSettings settings{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings = settings_;
    }

    if (!settings.enableExecution) {
        DebugLogFormat(
            L"[ReservationExecution][PREPARE] skipped: execution disabled inst=%u",
            static_cast<unsigned>(req.instance));
        return;
    }

    TaskListManager::TaskEntry entry{};
    if (!FindReservationEntry_(req.instance, entry)) {
        SystemInfoLogWarn(
            L"予約準備対象が見つかりません。instance=%u",
            static_cast<unsigned>(req.instance));

        DebugLogFormat(
            L"[ReservationExecution][PREPARE] entry not found inst=%u",
            static_cast<unsigned>(req.instance));
        return;
    }

    UpdateActivePrepare_(entry, req);

    const bool watch = IsWatch_(entry);
    const bool record = IsRecord_(entry);

    if (!watch && !record) {
        SystemInfoLogWarn(
            L"未知の予約モードの準備処理です。instance=%u mode=%ls title=%ls",
            static_cast<unsigned>(req.instance),
            entry.reserve.reserveMode.c_str(),
            SafeTitle_(entry).c_str());

        UpdateActivePrepareResult_(entry, req, false, false);
        return;
    }

    if (watch && !settings.enableWatchReservation) {
        SystemInfoLogWarn(
            L"視聴予約準備は設定により無効です。instance=%u title=%ls",
            static_cast<unsigned>(req.instance),
            SafeTitle_(entry).c_str());

        UpdateActivePrepareResult_(entry, req, false, false);
        return;
    }

    const bool useTvTestBackend = settings.enableTvTestWatch;

    if (watch && !useTvTestBackend) {
        SystemInfoLogWarn(
            L"内部視聴予約はまだ未対応です。instance=%u title=%ls",
            static_cast<unsigned>(req.instance),
            SafeTitle_(entry).c_str());

        UpdateActivePrepareResult_(entry, req, false, false);
        return;
    }

    if (record && !useTvTestBackend) {
        SystemInfoLogInfo(
            L"内部録画予約の事前録画開始を行います。番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d",
            SafeTitle_(entry).c_str(),
            SafeService_(entry).c_str(),
            tunerIndex + 1);

        DebugLogFormat(
            L"[ReservationExecution][PREPARE-INTERNAL-RECORD] start preroll inst=%u tuner=%d bon=%p title=%ls service=%ls",
            static_cast<unsigned>(req.instance),
            tunerIndex,
            static_cast<void*>(bon),
            SafeTitle_(entry).c_str(),
            SafeService_(entry).c_str());

        const bool started =
            StartRecordReservation_(
                entry,
                req,
                tunerIndex,
                bon);

        DebugLogFormat(
            L"[ReservationExecution][PREPARE-INTERNAL-RECORD] result inst=%u started=%d running=%d",
            static_cast<unsigned>(req.instance),
            started ? 1 : 0,
            rawTsRecorder_.IsRunning(req.instance) ? 1 : 0);

        if (started) {
            SystemInfoLogInfo(
                L"内部録画予約の事前録画を開始しました。番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d",
                SafeTitle_(entry).c_str(),
                SafeService_(entry).c_str(),
                tunerIndex + 1);

            // 内部録画では「準備完了」= RawTS保存開始済み。
            UpdateActivePrepareResult_(entry, req, true, true);
            return;
        }

        SystemInfoLogWarn(
            L"内部録画予約の事前録画開始に失敗しました。番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d",
            SafeTitle_(entry).c_str(),
            SafeService_(entry).c_str(),
            tunerIndex + 1);

        UpdateActivePrepareResult_(entry, req, false, false);
        return;
    }

    if (record && useTvTestBackend) {
        SystemInfoLogInfo(
            L"TVTest録画予約の事前録画開始を行います。番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d",
            SafeTitle_(entry).c_str(),
            SafeService_(entry).c_str(),
            tunerIndex + 1);

        DebugLogFormat(
            L"[ReservationExecution][PREPARE-TVTEST-RECORD] start preroll inst=%u tuner=%d title=%ls service=%ls",
            static_cast<unsigned>(req.instance),
            tunerIndex,
            SafeTitle_(entry).c_str(),
            SafeService_(entry).c_str());

        const bool started =
            StartRecordReservation_(
                entry,
                req,
                tunerIndex,
                bon);

        DebugLogFormat(
            L"[ReservationExecution][PREPARE-TVTEST-RECORD] result inst=%u started=%d running=%d",
            static_cast<unsigned>(req.instance),
            started ? 1 : 0,
            IsTvTestRecordStartingOrRunning_(req.instance) ? 1 : 0);

        if (started) {
            SystemInfoLogInfo(
                L"TVTest録画予約の事前録画を開始しました。番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d",
                SafeTitle_(entry).c_str(),
                SafeService_(entry).c_str(),
                tunerIndex + 1);

            UpdateActivePrepareResult_(entry, req, true, true);
            return;
        }

        SystemInfoLogWarn(
            L"TVTest録画予約の事前録画開始に失敗しました。番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d",
            SafeTitle_(entry).c_str(),
            SafeService_(entry).c_str(),
            tunerIndex + 1);

        UpdateActivePrepareResult_(entry, req, false, false);
        return;
    }

    SystemInfoLogInfo(
        L"%ls予約の事前準備を開始しました。番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d",
        ModeLabel_(entry).c_str(),
        SafeTitle_(entry).c_str(),
        SafeService_(entry).c_str(),
        tunerIndex + 1);

    auto controller = BuildTvTestController_(tunerIndex);

    ULONGLONG startTickMs = 0;
    const DWORD fallbackMaxMs = 30000;
    if (!TryGetReserveStartTimeMs_(entry, startTickMs, fallbackMaxMs)) {
        startTickMs = ::GetTickCount64() + fallbackMaxMs;
    }

    // 開始時刻の直前まで粘る。ただし、RecStartを詰まらせないよう1秒前で打ち切る。
    const ULONGLONG hardDeadline =
        (startTickMs > 1000)
        ? (startTickMs - 1000)
        : ::GetTickCount64();

    constexpr DWORD kPrepareRetryIntervalMs = 500;

    bool tvtestReady = false;
    bool serviceSelected = false;

    std::wstring lastReadyReason;
    std::wstring lastSelectReason;

    for (;;) {
        const auto ready = controller.EnsureReady();
        tvtestReady = ready.ok;

        DebugLogFormat(
            L"[ReservationExecution][PREPARE] EnsureReady inst=%u ok=%d response=%ls reason=%ls",
            static_cast<unsigned>(req.instance),
            ready.ok ? 1 : 0,
            ready.response.c_str(),
            ready.reason.c_str());

        if (ready.ok) {
            const auto view = controller.ApplyViewOptions();

            DebugLogFormat(
                L"[ReservationExecution][PREPARE] ApplyViewOptions inst=%u ok=%d response=%ls reason=%ls",
                static_cast<unsigned>(req.instance),
                view.ok ? 1 : 0,
                view.response.c_str(),
                view.reason.c_str());

            const auto sel = controller.SelectReservationService(entry);
            serviceSelected = sel.ok;

            DebugLogFormat(
                L"[ReservationExecution][PREPARE] SelectReservationService inst=%u ok=%d response=%ls reason=%ls",
                static_cast<unsigned>(req.instance),
                sel.ok ? 1 : 0,
                sel.response.c_str(),
                sel.reason.c_str());

            if (sel.ok) {
                SystemInfoLogInfo(
                    L"%ls予約の事前チャンネル切替が完了しました。番組=\"%ls\" 放送局=\"%ls\"",
                    ModeLabel_(entry).c_str(),
                    SafeTitle_(entry).c_str(),
                    SafeService_(entry).c_str());

                UpdateActivePrepareResult_(entry, req, true, true);
                return;
            }

            lastSelectReason = sel.reason;
        }
        else {
            lastReadyReason = ready.reason;
        }

        const ULONGLONG now = ::GetTickCount64();
        if (now >= hardDeadline) {
            break;
        }

        const ULONGLONG remain = hardDeadline - now;
        const DWORD sleepMs =
            static_cast<DWORD>((std::min<ULONGLONG>)(remain, kPrepareRetryIntervalMs));

        if (sleepMs == 0) {
            break;
        }

        ::Sleep(sleepMs);
    }

    SystemInfoLogWarn(
        L"%ls予約の事前準備が開始時刻前に完了しませんでした。番組=\"%ls\" 放送局=\"%ls\" ready=%d selected=%d reason=%ls",
        ModeLabel_(entry).c_str(),
        SafeTitle_(entry).c_str(),
        SafeService_(entry).c_str(),
        tvtestReady ? 1 : 0,
        serviceSelected ? 1 : 0,
        !lastSelectReason.empty()
        ? lastSelectReason.c_str()
        : (!lastReadyReason.empty() ? lastReadyReason.c_str() : L"(unknown)"));

    UpdateActivePrepareResult_(entry, req, tvtestReady, serviceSelected);
}

bool Reservation_ExecutionManager::StartRecordReservation_(
    const TaskListManager::TaskEntry& entry,
    const TimeKeeper::TaskRequest& req,
    int tunerIndex,
    IBonDriver2* bon)
{
    Reservation_ExecutionSettings settings{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings = settings_;
    }

    if (settings.enableTvTestWatch) {
        if (IsTvTestRecordStartingOrRunning_(req.instance)) {
            DebugLogFormat(
                L"[ReservationExecution][TVTEST-RECORD-START] already starting/running inst=%u tuner=%d",
                static_cast<unsigned>(req.instance),
                tunerIndex);

            return true;
        }

        if (!StopTvTestRecordOwnerForPreparePriority_(
            tunerIndex,
            req.instance,
            entry)) {
            return false;
        }

        const std::filesystem::path recordPath =
            MakeUniqueRecordPath_(entry);

        DebugLogFormat(
            L"[ReservationExecution][TVTEST-RECORD-PATH] inst=%u tuner=%d path=%ls",
            static_cast<unsigned>(req.instance),
            tunerIndex,
            recordPath.wstring().c_str());

        SetActiveRecordFilePath_(
            req.instance,
            recordPath.wstring());

        return StartTvTestRecordReservation_(
            entry,
            req,
            tunerIndex,
            recordPath);
    }

    if (IsInternalRecordStartingOrRunning_(req.instance)) {
        DebugLogFormat(
            L"[ReservationExecution][RECORD-START] internal record already starting/running inst=%u tuner=%d",
            static_cast<unsigned>(req.instance),
            tunerIndex);

        return true;
    }

    const std::filesystem::path recordPath =
        MakeUniqueRecordPath_(entry);

    return StartInternalRecordReservation_(
        entry,
        req,
        tunerIndex,
        bon,
        recordPath);


}

bool Reservation_ExecutionManager::StartTvTestRecordReservation_(
    const TaskListManager::TaskEntry& entry,
    const TimeKeeper::TaskRequest& req,
    int tunerIndex,
    const std::filesystem::path& recordPath)
{
    std::uint32_t busyInstance = 0;
    if (!TryAcquireTvTestRecordTuner_(
        tunerIndex,
        req.instance,
        busyInstance)) {
        SystemInfoLogWarn(
            L"TVTest録画予約はチューナー使用中のため開始できません。番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d 使用中instance=%u",
            SafeTitle_(entry).c_str(),
            SafeService_(entry).c_str(),
            tunerIndex + 1,
            static_cast<unsigned>(busyInstance));
        return false;
    }

    bool alreadyActive = false;
    if (!TryBeginTvTestRecordStart_(
        req.instance,
        tunerIndex,
        recordPath.wstring(),
        alreadyActive)) {
        if (!alreadyActive) {
            ReleaseTvTestRecordTuner_(tunerIndex, req.instance);
        }

        return alreadyActive;
    }

    auto controller = BuildTvTestController_(tunerIndex, true);

    const auto ready = controller.EnsureReady();
    if (!ready.ok) {
        FinishTvTestRecordStart_(req.instance, false);
        ReleaseTvTestRecordTuner_(tunerIndex, req.instance);
        WriteSystemFailLog_(entry, L"tvtest-record-ready", ready.reason);
        return false;
    }

    const bool prepared = WasServicePrepared_(req.instance);

    if (!prepared) {
        const auto view = controller.ApplyViewOptions();
        if (!view.ok) {
            SystemInfoLogWarn(
                L"TVTest録画予約の表示設定に失敗しました。番組=\"%ls\" 放送局=\"%ls\" 理由=%ls",
                SafeTitle_(entry).c_str(),
                SafeService_(entry).c_str(),
                view.reason.empty() ? L"(unknown)" : view.reason.c_str());
        }

        const auto sel = controller.SelectReservationService(entry);
        if (!sel.ok) {
            FinishTvTestRecordStart_(req.instance, false);
            ReleaseTvTestRecordTuner_(tunerIndex, req.instance);
            WriteSystemFailLog_(entry, L"tvtest-record-set-channel", sel.reason);
            return false;
        }

        WriteSystemSelectedLog_(entry);
    }

    const std::wstring command =
        L"START_RECORD " + QuoteTvTestCommandArg_(recordPath.wstring());

    const auto rec = controller.SendRawCommand(command);

    DebugLogFormat(
        L"[ReservationExecution][TVTEST-RECORD-START] inst=%u ok=%d path=%ls response=%ls reason=%ls",
        static_cast<unsigned>(req.instance),
        rec.ok ? 1 : 0,
        recordPath.wstring().c_str(),
        rec.response.c_str(),
        rec.reason.c_str());

    if (!rec.ok) {
        FinishTvTestRecordStart_(req.instance, false);
        ReleaseTvTestRecordTuner_(tunerIndex, req.instance);
        WriteSystemFailLog_(entry, L"tvtest-start-record", rec.reason);
        return false;
    }

    FinishTvTestRecordStart_(req.instance, true);

    const auto tsEnable = controller.EnableTs(true);

    DebugLogFormat(
        L"[ReservationExecution][TVTEST-RECORD-TS-ENABLE] inst=%u ok=%d response=%ls reason=%ls",
        static_cast<unsigned>(req.instance),
        tsEnable.ok ? 1 : 0,
        tsEnable.response.empty() ? L"(empty)" : tsEnable.response.c_str(),
        tsEnable.reason.empty() ? L"(none)" : tsEnable.reason.c_str());

    if (!tsEnable.ok) {
        SystemInfoLogWarn(
            L"TVTest録画中TS出力の有効化に失敗しました。番組=\"%ls\" 放送局=\"%ls\" 理由=%ls",
            SafeTitle_(entry).c_str(),
            SafeService_(entry).c_str(),
            tsEnable.reason.empty() ? L"(unknown)" : tsEnable.reason.c_str());
    }

    TaskSystemConnector* taskSystem = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        taskSystem = taskSystem_;
    }

    eitMonitor_.StartRecordMonitor(
        req.instance,
        entry,
        [this, tunerIndex]() -> Reservation_EitMonitor::Snapshot
        {
            auto c = BuildTvTestController_(tunerIndex);
            return eitMonitor_.QueryOnce(c);
        },
        taskSystem,
        [this, tunerIndex, entry](std::uint32_t instance)
        {
            const std::wstring path =
                GetActiveRecordFilePath_(instance);

            StopTvTestRecordOnlyFromMonitor_(
                instance,
                tunerIndex,
                entry,
                path);
        });

    SystemInfoLogInfo(
        L"TVTest録画予約を開始しました。番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d 保存先=\"%ls\"",
        SafeTitle_(entry).c_str(),
        SafeService_(entry).c_str(),
        tunerIndex + 1,
        recordPath.wstring().c_str());

    return true;
}

bool Reservation_ExecutionManager::StopTvTestRecordReservation_(
    const TaskListManager::TaskEntry& entry,
    const TimeKeeper::TaskRequest& req,
    int tunerIndex,
    const std::wstring& recordPath)
{
    eitMonitor_.StopRecordMonitor(req.instance);

    std::wstring effectiveRecordPath = recordPath;
    if (effectiveRecordPath.empty()) {
        effectiveRecordPath = GetActiveRecordFilePath_(req.instance);
    }

    const bool owned =
        IsTvTestRecordOwner_(
            tunerIndex,
            req.instance);

    const bool active =
        IsTvTestRecordStartingOrRunning_(req.instance);

    if (!owned && !active) {
        const bool startFailed =
            WasTvTestRecordStartFailed_(req.instance);

        DebugLogFormat(
            L"[ReservationExecution][TVTEST-RECORD-STOP] already stopped inst=%u tuner=%d startFailed=%d path=%ls",
            static_cast<unsigned>(req.instance),
            tunerIndex,
            startFailed ? 1 : 0,
            effectiveRecordPath.empty() ? L"(unknown)" : effectiveRecordPath.c_str());

        if (startFailed) {
            SystemInfoLogWarn(
                L"TVTest録画予約は開始に失敗していたため停止対象がありません。番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d 予定保存先=\"%ls\"",
                SafeTitle_(entry).c_str(),
                SafeService_(entry).c_str(),
                tunerIndex + 1,
                effectiveRecordPath.empty() ? L"(unknown)" : effectiveRecordPath.c_str());

            return false;
        }

        SystemInfoLogInfo(
            L"TVTest録画予約は既に停止済みです。番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d 保存先=\"%ls\"",
            SafeTitle_(entry).c_str(),
            SafeService_(entry).c_str(),
            tunerIndex + 1,
            effectiveRecordPath.empty() ? L"(unknown)" : effectiveRecordPath.c_str());

        return true;
    }

    auto controller = BuildTvTestController_(tunerIndex);

    const auto rec = controller.SendRawCommand(L"STOP_RECORD");

    DebugLogFormat(
        L"[ReservationExecution][TVTEST-RECORD-STOP] inst=%u ok=%d path=%ls response=%ls reason=%ls",
        static_cast<unsigned>(req.instance),
        rec.ok ? 1 : 0,
        effectiveRecordPath.empty() ? L"(unknown)" : effectiveRecordPath.c_str(),
        rec.response.c_str(),
        rec.reason.c_str());

    if (!rec.ok) {
        WriteSystemFailLog_(entry, L"tvtest-stop-record", rec.reason);
        return false;
    }

    const auto tsDisable = controller.EnableTs(false);

    DebugLogFormat(
        L"[ReservationExecution][TVTEST-RECORD-TS-DISABLE] inst=%u ok=%d response=%ls reason=%ls",
        static_cast<unsigned>(req.instance),
        tsDisable.ok ? 1 : 0,
        tsDisable.response.empty() ? L"(empty)" : tsDisable.response.c_str(),
        tsDisable.reason.empty() ? L"(none)" : tsDisable.reason.c_str());

    if (!tsDisable.ok) {
        SystemInfoLogWarn(
            L"TVTest録画終了時のTS出力停止に失敗しました。番組=\"%ls\" 放送局=\"%ls\" 理由=%ls",
            SafeTitle_(entry).c_str(),
            SafeService_(entry).c_str(),
            tsDisable.reason.empty() ? L"(unknown)" : tsDisable.reason.c_str());
    }

    MarkTvTestRecordStopped_(req.instance, tunerIndex);

    SystemInfoLogInfo(
        L"TVTest録画予約を停止しました。番組=\"%ls\" 放送局=\"%ls\" 保存先=\"%ls\"",
        SafeTitle_(entry).c_str(),
        SafeService_(entry).c_str(),
        effectiveRecordPath.empty() ? L"(unknown)" : effectiveRecordPath.c_str());

    return true;
}

void Reservation_ExecutionManager::OnRecStart(
    int tunerIndex,
    IBonDriver2* bon,
    const TimeKeeper::TaskRequest& req)
{
    Reservation_ExecutionSettings settings{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings = settings_;
    }

    if (!settings.enableExecution) {
        return;
    }

    TaskListManager::TaskEntry entry{};
    if (!FindReservationEntry_(req.instance, entry)) {
        SystemInfoLogWarn(
            L"予約起動対象が見つかりません。instance=%u",
            static_cast<unsigned>(req.instance));
        return;
    }

    WriteSystemStartLog_(entry, tunerIndex, settings);

    bool succeeded = false;

    if (IsWatch_(entry)) {
        if (!settings.enableWatchReservation || !settings.enableTvTestWatch) {
            WriteSystemFailLog_(
                entry,
                L"watch-disabled",
                L"視聴予約は設定により無効です。");

            UpdateActiveStart_(entry, req, false);
            return;
        }

        const bool prepared = WasServicePrepared_(req.instance);

        auto controller = BuildTvTestController_(tunerIndex);

        const auto ready = controller.EnsureReady();
        if (!ready.ok) {
            WriteSystemFailLog_(entry, L"tvtest-ready", ready.reason);
            UpdateActiveStart_(entry, req, false);
            return;
        }

        bool succeeded = false;

        if (prepared) {
            succeeded = true;

            SystemInfoLogInfo(
                L"視聴予約を開始しました。番組=\"%ls\" 放送局=\"%ls\" 事前準備済みTVTestを使用します。",
                SafeTitle_(entry).c_str(),
                SafeService_(entry).c_str());

            DebugLogFormat(
                L"[ReservationExecution][START] prepared route inst=%u title=%ls service=%ls",
                static_cast<unsigned>(req.instance),
                SafeTitle_(entry).c_str(),
                SafeService_(entry).c_str());
        }
        else {
            DebugLogFormat(
                L"[ReservationExecution][START] prepare not completed. fallback select. inst=%u title=%ls service=%ls",
                static_cast<unsigned>(req.instance),
                SafeTitle_(entry).c_str(),
                SafeService_(entry).c_str());

            const auto view = controller.ApplyViewOptions();
            if (!view.ok) {
                SystemInfoLogWarn(
                    L"%ls予約のTVTest表示設定に失敗しました。番組=\"%ls\" 放送局=\"%ls\" 理由=%ls",
                    ModeLabel_(entry).c_str(),
                    SafeTitle_(entry).c_str(),
                    SafeService_(entry).c_str(),
                    view.reason.empty() ? L"(unknown)" : view.reason.c_str());
            }

            const auto sel = controller.SelectReservationService(entry);
            if (!sel.ok) {
                WriteSystemFailLog_(entry, L"set-channel", sel.reason);
                UpdateActiveStart_(entry, req, false);
                return;
            }

            succeeded = true;
            WriteSystemSelectedLog_(entry);

            DebugLogFormat(
                L"[ReservationExecution][START] fallback selected inst=%u title=%ls service=%ls response=%ls",
                static_cast<unsigned>(req.instance),
                SafeTitle_(entry).c_str(),
                SafeService_(entry).c_str(),
                sel.response.c_str());
        }

        if (succeeded && settings.enableTsPipeDuringWatch) {
            const auto ts = controller.EnableTs(true);
            if (!ts.ok) {
                SystemInfoLogWarn(
                    L"%ls予約中TS出力の有効化に失敗しました。番組=\"%ls\" 放送局=\"%ls\" 理由=%ls",
                    ModeLabel_(entry).c_str(),
                    SafeTitle_(entry).c_str(),
                    SafeService_(entry).c_str(),
                    ts.reason.empty() ? L"(unknown)" : ts.reason.c_str());
            }
        }

        if (succeeded) {
            eitMonitor_.StartWatchMonitor(req.instance, entry, controller);
        }

        UpdateActiveStart_(entry, req, succeeded);
        return;
    }

    if (IsRecord_(entry)) {
        if (settings.enableTvTestWatch) {
            if (IsTvTestRecordStartingOrRunning_(req.instance)) {
                DebugLogFormat(
                    L"[ReservationExecution][START-TVTEST-RECORD] already starting/running from prepare inst=%u tuner=%d",
                    static_cast<unsigned>(req.instance),
                    tunerIndex);

                SystemInfoLogInfo(
                    L"TVTest録画予約は事前準備時点で開始中または開始済みです。番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d",
                    SafeTitle_(entry).c_str(),
                    SafeService_(entry).c_str(),
                    tunerIndex + 1);

                UpdateActiveStart_(entry, req, true);
                return;
            }
        }
        else {
            if (IsInternalRecordStartingOrRunning_(req.instance)) {
                DebugLogFormat(
                    L"[ReservationExecution][START-INTERNAL-RECORD] already starting/running from prepare inst=%u tuner=%d",
                    static_cast<unsigned>(req.instance),
                    tunerIndex);

                SystemInfoLogInfo(
                    L"内部録画予約は事前準備時点で開始中または開始済みです。番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d",
                    SafeTitle_(entry).c_str(),
                    SafeService_(entry).c_str(),
                    tunerIndex + 1);

                UpdateActiveStart_(entry, req, true);
                return;
            }
        }

        const bool recordStarted =
            StartRecordReservation_(
                entry,
                req,
                tunerIndex,
                bon);

        UpdateActiveStart_(entry, req, recordStarted);
        return;
    }

    WriteSystemFailLog_(
        entry,
        L"unknown-mode",
        L"未知の予約モードです。");

    UpdateActiveStart_(entry, req, false);
}

bool Reservation_ExecutionManager::StopRecordReservation_(
    const TaskListManager::TaskEntry& entry,
    const TimeKeeper::TaskRequest& req,
    int tunerIndex)
{
    Reservation_ExecutionSettings settings{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings = settings_;
    }

    const std::wstring recordPath =
        GetActiveRecordFilePath_(req.instance);

    if (settings.enableTvTestWatch) {
        const bool stopped =
            StopTvTestRecordReservation_(
                entry,
                req,
                tunerIndex,
                recordPath);

        if (!stopped) {
            SystemInfoLogWarn(
                L"TVTest録画停止に失敗したため、TVTest終了処理をスキップします。番組=\"%ls\" 放送局=\"%ls\" 保存先=\"%ls\"",
                SafeTitle_(entry).c_str(),
                SafeService_(entry).c_str(),
                recordPath.empty() ? L"(unknown)" : recordPath.c_str());

            return false;
        }

        const bool keepAlive =
            ShouldKeepTvTestAliveAfterStop_(entry, req, tunerIndex);

        auto controller = BuildTvTestController_(tunerIndex);

        bool succeeded = true;

        if (keepAlive) {
            SystemInfoLogInfo(
                L"連続予約のためTVTestを終了しません。終了番組=\"%ls\" 放送局=\"%ls\" 保存先=\"%ls\"",
                SafeTitle_(entry).c_str(),
                SafeService_(entry).c_str(),
                recordPath.empty() ? L"(unknown)" : recordPath.c_str());

            DebugLogFormat(
                L"[ReservationExecution][RECORD-STOP] keep tvtest alive inst=%u tuner=%d",
                static_cast<unsigned>(req.instance),
                tunerIndex);
        }
        else {
            const auto close = controller.CloseTvTest();

            DebugLogFormat(
                L"[ReservationExecution][RECORD-STOP] close tvtest inst=%u ok=%d response=%ls reason=%ls",
                static_cast<unsigned>(req.instance),
                close.ok ? 1 : 0,
                close.response.c_str(),
                close.reason.c_str());

            if (!close.ok) {
                succeeded = false;

                SystemInfoLogWarn(
                    L"録画予約終了時のTVTest終了に失敗しました。番組=\"%ls\" 放送局=\"%ls\" 保存先=\"%ls\" 理由=%ls",
                    SafeTitle_(entry).c_str(),
                    SafeService_(entry).c_str(),
                    recordPath.empty() ? L"(unknown)" : recordPath.c_str(),
                    close.reason.empty() ? L"(unknown)" : close.reason.c_str());
            }
        }

        if (succeeded) {
            SystemInfoLogInfo(
                L"録画予約を終了しました。番組=\"%ls\" 放送局=\"%ls\" 保存先=\"%ls\" 結果=成功",
                SafeTitle_(entry).c_str(),
                SafeService_(entry).c_str(),
                recordPath.empty() ? L"(unknown)" : recordPath.c_str());
        }
        else {
            SystemInfoLogWarn(
                L"録画予約を終了しました。番組=\"%ls\" 放送局=\"%ls\" 保存先=\"%ls\" 結果=警告",
                SafeTitle_(entry).c_str(),
                SafeService_(entry).c_str(),
                recordPath.empty() ? L"(unknown)" : recordPath.c_str());
        }

        return succeeded;
    }

    const bool stopped =
        StopInternalRecordReservation_(
            entry,
            req,
            tunerIndex,
            recordPath);

    if (stopped) {
        SystemInfoLogInfo(
            L"内部録画予約を終了しました。番組=\"%ls\" 放送局=\"%ls\" 保存先=\"%ls\" 結果=成功",
            SafeTitle_(entry).c_str(),
            SafeService_(entry).c_str(),
            recordPath.empty() ? L"(unknown)" : recordPath.c_str());
    }
    else {
        SystemInfoLogWarn(
            L"内部録画予約を終了しました。番組=\"%ls\" 放送局=\"%ls\" 保存先=\"%ls\" 結果=警告",
            SafeTitle_(entry).c_str(),
            SafeService_(entry).c_str(),
            recordPath.empty() ? L"(unknown)" : recordPath.c_str());
    }

    return stopped;
}

void Reservation_ExecutionManager::OnRecStop(
    int tunerIndex,
    IBonDriver2* /*bon*/,
    const TimeKeeper::TaskRequest& req)
{
    DebugLogFormat(
        L"[ReservationExecution][STOP] enter tuner=%d kind=%u inst=%u",
        tunerIndex,
        static_cast<unsigned>(req.kind),
        static_cast<unsigned>(req.instance));

    Reservation_ExecutionSettings settings{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings = settings_;
    }

    if (!settings.enableExecution) {
        DebugLogFormat(
            L"[ReservationExecution][STOP] skipped: execution disabled inst=%u",
            static_cast<unsigned>(req.instance));
        return;
    }

    TaskListManager::TaskEntry entry{};
    if (!FindReservationEntry_(req.instance, entry)) {
        SystemInfoLogWarn(
            L"予約終了対象が見つかりません。instance=%u",
            static_cast<unsigned>(req.instance));

        DebugLogFormat(
            L"[ReservationExecution][STOP] entry not found inst=%u",
            static_cast<unsigned>(req.instance));
        return;
    }

    bool succeeded = true;

    if (IsWatch_(entry)) {
        DebugLogFormat(
            L"[ReservationExecution][STOP] watch stop begin inst=%u title=%ls service=%ls tuner=%d",
            static_cast<unsigned>(req.instance),
            SafeTitle_(entry).c_str(),
            SafeService_(entry).c_str(),
            tunerIndex);

        eitMonitor_.StopWatchMonitor(req.instance);

        auto controller = BuildTvTestController_(tunerIndex);

        const bool keepAlive =
            ShouldKeepTvTestAliveAfterStop_(entry, req, tunerIndex);

        if (!keepAlive && settings.enableTsPipeDuringWatch) {
            const auto ts = controller.EnableTs(false);

            DebugLogFormat(
                L"[ReservationExecution][STOP] disable ts inst=%u ok=%d response=%ls reason=%ls",
                static_cast<unsigned>(req.instance),
                ts.ok ? 1 : 0,
                ts.response.c_str(),
                ts.reason.c_str());

            if (!ts.ok) {
                succeeded = false;

                SystemInfoLogWarn(
                    L"%ls予約終了時のTS出力停止に失敗しました。番組=\"%ls\" 放送局=\"%ls\" 理由=%ls",
                    ModeLabel_(entry).c_str(),
                    SafeTitle_(entry).c_str(),
                    SafeService_(entry).c_str(),
                    ts.reason.empty() ? L"(unknown)" : ts.reason.c_str());
            }
        }
        else if (keepAlive) {
            DebugLogFormat(
                L"[ReservationExecution][STOP] keep ts state for following reservation inst=%u tuner=%d",
                static_cast<unsigned>(req.instance),
                tunerIndex);
        }

        // 単独予約では終了時刻でTVTestを閉じる。
        // 連続予約で閉じない判定は後で ShouldKeepTvTestAliveAfterStop_ として分離する。
        if (keepAlive) {
            SystemInfoLogInfo(
                L"連続予約のためTVTestを終了しません。終了番組=\"%ls\" 放送局=\"%ls\"",
                SafeTitle_(entry).c_str(),
                SafeService_(entry).c_str());

            DebugLogFormat(
                L"[ReservationExecution][STOP] skip close tvtest because following reservation exists inst=%u tuner=%d",
                static_cast<unsigned>(req.instance),
                tunerIndex);
        }
        else {
            const auto close = controller.CloseTvTest();

            DebugLogFormat(
                L"[ReservationExecution][STOP] close tvtest inst=%u ok=%d response=%ls reason=%ls",
                static_cast<unsigned>(req.instance),
                close.ok ? 1 : 0,
                close.response.c_str(),
                close.reason.c_str());

            if (!close.ok) {
                succeeded = false;

                SystemInfoLogWarn(
                    L"%ls予約終了時のTVTest終了に失敗しました。番組=\"%ls\" 放送局=\"%ls\" 理由=%ls",
                    ModeLabel_(entry).c_str(),
                    SafeTitle_(entry).c_str(),
                    SafeService_(entry).c_str(),
                    close.reason.empty() ? L"(unknown)" : close.reason.c_str());
            }
        }

        WriteSystemEndLog_(entry, succeeded);
        UpdateActiveStop_(entry, req, succeeded);

        const bool completed = CompleteProgramReservationGroup_(req.instance);
        if (!completed) {
            SystemInfoLogWarn(
                L"%ls予約の終了後削除に失敗しました。番組=\"%ls\" 放送局=\"%ls\" instance=%u",
                ModeLabel_(entry).c_str(),
                SafeTitle_(entry).c_str(),
                SafeService_(entry).c_str(),
                static_cast<unsigned>(req.instance));
        }

        DebugLogFormat(
            L"[ReservationExecution][STOP] watch stop end inst=%u succeeded=%d completed=%d",
            static_cast<unsigned>(req.instance),
            succeeded ? 1 : 0,
            completed ? 1 : 0);

        return;
    }

    if (IsRecord_(entry)) {
        const bool recordStopped =
            StopRecordReservation_(
                entry,
                req,
                tunerIndex);

        UpdateActiveStop_(entry, req, recordStopped);

        const bool completed =
            CompleteProgramReservationGroup_(req.instance);

        if (!completed) {
            SystemInfoLogWarn(
                L"録画予約の終了後削除に失敗しました。番組=\"%ls\" 放送局=\"%ls\" instance=%u",
                SafeTitle_(entry).c_str(),
                SafeService_(entry).c_str(),
                static_cast<unsigned>(req.instance));
        }

        DebugLogFormat(
            L"[ReservationExecution][STOP] record stop end inst=%u stopped=%d completed=%d",
            static_cast<unsigned>(req.instance),
            recordStopped ? 1 : 0,
            completed ? 1 : 0);

        return;
    }

    WriteSystemFailLog_(
        entry,
        L"unknown-mode-stop",
        L"未知の予約モードの終了処理です。");

    UpdateActiveStop_(entry, req, false);
    (void)CompleteProgramReservationGroup_(req.instance);
}

bool Reservation_ExecutionManager::StartInternalRecordReservation_(
    const TaskListManager::TaskEntry& entry,
    const TimeKeeper::TaskRequest& req,
    int tunerIndex,
    IBonDriver2* bon,
    const std::filesystem::path& recordPath)
{
    if (rawTsRecorder_.IsRunning(req.instance)) {
        FinishInternalRecordStart_(req.instance, true);

        DebugLogFormat(
            L"[ReservationExecution][INTERNAL-RECORD-START] already running inst=%u tuner=%d path=%ls",
            static_cast<unsigned>(req.instance),
            tunerIndex,
            recordPath.wstring().c_str());

        return true;
    }

    // 準備時間を最上位にする。
    // 同じチューナーで前録画が動いている場合は、前録画を止めて次予約を優先する。
    if (!StopInternalRecordOwnerForPreparePriority_(
        tunerIndex,
        req.instance,
        entry)) {
        return false;
    }

    std::uint32_t busyInstance = 0;
    if (!TryAcquireInternalRecordTuner_(
        tunerIndex,
        req.instance,
        busyInstance)) {
        DebugLogFormat(
            L"[ReservationExecution][INTERNAL-RECORD-BUSY] tuner=%d currentInst=%u requestInst=%u title=%ls service=%ls",
            tunerIndex,
            static_cast<unsigned>(busyInstance),
            static_cast<unsigned>(req.instance),
            SafeTitle_(entry).c_str(),
            SafeService_(entry).c_str());

        SystemInfoLogWarn(
            L"内部録画予約はチューナー使用中のため開始できません。番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d 使用中instance=%u",
            SafeTitle_(entry).c_str(),
            SafeService_(entry).c_str(),
            tunerIndex + 1,
            static_cast<unsigned>(busyInstance));

        return false;
    }

    bool alreadyActive = false;
    if (!TryBeginInternalRecordStart_(
        req.instance,
        tunerIndex,
        recordPath.wstring(),
        alreadyActive)) {
        DebugLogFormat(
            L"[ReservationExecution][INTERNAL-RECORD-START] skipped duplicate start inst=%u tuner=%d alreadyActive=%d",
            static_cast<unsigned>(req.instance),
            tunerIndex,
            alreadyActive ? 1 : 0);

        if (!alreadyActive) {
            ReleaseInternalRecordTuner_(tunerIndex, req.instance);
        }

        return alreadyActive;
    }

    if (bon == nullptr) {
        FinishInternalRecordStart_(req.instance, false);
        ReleaseInternalRecordTuner_(tunerIndex, req.instance);

        WriteSystemFailLog_(
            entry,
            L"internal-record-bon",
            L"IBonDriver2 が nullptr です。");
        return false;
    }

    const std::wstring configIniPath =
        ConfigManager::GetInstance().GetIniPath();

    Reservation_RawTsRecorder::StartRequest sr{};
    sr.instance = req.instance;
    sr.bon = bon;
    sr.outputPath = recordPath.wstring();

    sr.setChannelBeforeStart = true;
    sr.space = entry.reserve.space;
    sr.channel = -1;

    sr.resolveChannelFromConfigIni = true;
    sr.configIniPath = configIniPath;
    sr.allowDisabledServiceForChannelResolve = true;

    sr.resolveChannelFromChannelList = true;
    sr.channelListPath.clear();

    sr.networkId = static_cast<std::uint16_t>(entry.reserve.networkId);
    sr.tsId = static_cast<std::uint16_t>(entry.reserve.tsId);
    sr.serviceId = static_cast<std::uint16_t>(entry.reserve.serviceId);
    sr.serviceName = entry.reserve.serviceName;

    sr.waitForSignalLock = true;
    sr.signalLockTimeoutMs = 5000;

    sr.settleSleepMs = 300;
    sr.discardInitialBlocks = 3;

    sr.idleSleepMs = 5;
    sr.noDataTimeoutMs = 15000;
    sr.flushIntervalBytes = 64ull * 1024ull * 1024ull;
    sr.createParentDirectories = true;

    sr.purpose = Reservation_RawTsRecorder::StartPurpose::ReservationRecord;

    const auto verifyTarget =
        Reservation_TargetVerifier::BuildTargetFromTaskEntry(entry);

    auto verifyState =
        std::make_shared<InternalRecordVerifyState_>();

    TaskSystemConnector* verifyTaskSystem = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        verifyTaskSystem = taskSystem_;
    }

    RecordingEitAnalyzer::StartRequest eitReq{};
    eitReq.instance = req.instance;
    eitReq.space = entry.reserve.space;

    // まずは既存 EpgFull を壊さないため -1。
    // 出力先は EpgPiggy_spaceX_tsidY_sidZ_EIT.epg になる。
    eitReq.physicalChannel = -1;

    eitReq.networkId = static_cast<std::uint16_t>(entry.reserve.networkId);
    eitReq.tsId = static_cast<std::uint16_t>(entry.reserve.tsId);
    eitReq.serviceId = static_cast<std::uint16_t>(entry.reserve.serviceId);

    eitReq.outputDir = L"epg_out";
    eitReq.basePrefix = L"EpgFull";
    eitReq.maxAnalyzeSeconds = kRecordingEitAnalyzeSeconds_;
    eitReq.existingEpgMinBytes = 4096;
    eitReq.enableAribTrace = false;
    eitReq.enableWrite = true;

    (void)recordingEitAnalyzer_.StartSession(eitReq);

    sr.onPackets =
        [this, verifyTarget, verifyState, verifyTaskSystem](const Reservation_RawTsRecorder::PacketBlock& block)
        {
            if (!verifyState) {
                return;
            }

            if (block.packets != nullptr) {
                (void)recordingEitAnalyzer_.UpdateFromPackets(
                    block.instance,
                    *block.packets);
            }

            if (!verifyState->packetLogged.exchange(true, std::memory_order_acq_rel)) {
                const std::uint64_t packetCount =
                    block.packets
                    ? static_cast<std::uint64_t>(block.packets->size())
                    : 0;

                DebugLogFormat(
                    L"[ReservationVerify][PACKET-CALLBACK] inst=%u block=%llu packets=%llu totalPackets=%llu totalBytes=%llu",
                    static_cast<unsigned>(block.instance),
                    static_cast<unsigned long long>(block.blockIndex),
                    static_cast<unsigned long long>(packetCount),
                    static_cast<unsigned long long>(block.totalPackets),
                    static_cast<unsigned long long>(block.totalBytes));
            }

            Reservation_TargetVerifier::ObservedTarget observed{};
            bool fromEit = false;

            if (!TryBuildObservedTargetFromPackets_(
                block,
                verifyTarget,
                observed,
                fromEit)) {
                return;
            }

            Reservation_TargetVerifier::VerifyOptions opt{};
            opt.requireSpace = true;

            // PAT/EITで networkId の意味がズレることがあるため、内部RawTSの初期照合では無効。
            // tsId / serviceId をまず信頼する。
            opt.requireNetworkId = false;

            opt.requireTsId = true;
            opt.requireServiceId = true;

            // EITから予約eventIdが取れた時だけ番組一致まで見る。
            opt.requireEventId = false;
            opt.requireTitle = false;
            opt.allowEmptyObservedProgram = true;

            const auto vr =
                Reservation_TargetVerifier::Verify(
                    verifyTarget,
                    observed,
                    opt);

            if (!vr.ok) {
                if (!verifyState->mismatchLogged.exchange(true, std::memory_order_acq_rel)) {
                    SystemInfoLogWarn(
                        L"内部録画のサービス照合に失敗しました。instance=%u reason=%ls reserve(space=%d nid=%u tsid=%u sid=%u eid=%u) observed(space=%d nid=%u tsid=%u sid=%u eid=%u source=%ls)",
                        static_cast<unsigned>(block.instance),
                        vr.reason.empty() ? L"(unknown)" : vr.reason.c_str(),
                        verifyTarget.space,
                        static_cast<unsigned>(verifyTarget.networkId),
                        static_cast<unsigned>(verifyTarget.tsId),
                        static_cast<unsigned>(verifyTarget.serviceId),
                        static_cast<unsigned>(verifyTarget.eventId),
                        observed.space,
                        static_cast<unsigned>(observed.networkId),
                        static_cast<unsigned>(observed.tsId),
                        static_cast<unsigned>(observed.serviceId),
                        static_cast<unsigned>(observed.eventId),
                        fromEit ? L"EIT" : L"PAT");
                }

                return;
            }

            if (vr.serviceMatched &&
                !verifyState->serviceLogged.exchange(true, std::memory_order_acq_rel)) {
                

                DebugLogFormat(
                    L"[ReservationVerify][SERVICE-MATCH] inst=%u block=%llu tsid=%u sid=%u source=%ls reason=%ls",
                    static_cast<unsigned>(block.instance),
                    static_cast<unsigned long long>(block.blockIndex),
                    static_cast<unsigned>(observed.tsId),
                    static_cast<unsigned>(observed.serviceId),
                    fromEit ? L"EIT" : L"PAT",
                    vr.reason.c_str());
            }

            if (fromEit &&
                verifyTarget.eventId != 0 &&
                observed.eventId == verifyTarget.eventId &&
                !verifyState->programLogged.exchange(true, std::memory_order_acq_rel)) {
                

                DebugLogFormat(
                    L"[ReservationVerify][PROGRAM-MATCH] inst=%u block=%llu eventId=%u tsid=%u sid=%u",
                    static_cast<unsigned>(block.instance),
                    static_cast<unsigned long long>(block.blockIndex),
                    static_cast<unsigned>(observed.eventId),
                    static_cast<unsigned>(observed.tsId),
                    static_cast<unsigned>(observed.serviceId));
            }
        };

    DebugLogFormat(
        L"[ReservationExecution][INTERNAL-RECORD-REQ] inst=%u tuner=%d setCh=%d space=%d ch=%d nid=%u tsid=%u sid=%u config=%ls service=%ls path=%ls",
        static_cast<unsigned>(req.instance),
        tunerIndex,
        sr.setChannelBeforeStart ? 1 : 0,
        sr.space,
        sr.channel,
        static_cast<unsigned>(sr.networkId),
        static_cast<unsigned>(sr.tsId),
        static_cast<unsigned>(sr.serviceId),
        sr.configIniPath.empty() ? L"(auto)" : sr.configIniPath.c_str(),
        sr.serviceName.empty() ? L"(unknown)" : sr.serviceName.c_str(),
        recordPath.wstring().c_str());

    Reservation_RawTsRecorder::Result rr{};
    const bool ok = rawTsRecorder_.Start(sr, &rr);

    if (ok && rr.channelResolved && rr.resolvedChannel >= 0) {
        (void)recordingEitAnalyzer_.UpdateResolvedChannel(
            req.instance,
            sr.space,
            rr.resolvedChannel);
    }

    FinishInternalRecordStart_(req.instance, ok);

    if (!ok) {
        recordingEitAnalyzer_.Reset(req.instance);
        ReleaseInternalRecordTuner_(tunerIndex, req.instance);
    }

    DebugLogFormat(
        L"[ReservationExecution][INTERNAL-RECORD-START] inst=%u ok=%d tuner=%d path=%ls resolved=%d fromConfig=%d serviceEnabled=%d ch=%d setCh=%d setChOk=%d sig=%d sigOk=%d reason=%ls",
        static_cast<unsigned>(req.instance),
        ok ? 1 : 0,
        tunerIndex,
        recordPath.wstring().c_str(),
        rr.channelResolved ? 1 : 0,
        rr.resolvedFromConfigIni ? 1 : 0,
        rr.resolvedServiceEnabled ? 1 : 0,
        rr.resolvedChannel,
        rr.channelSetTried ? 1 : 0,
        rr.channelSetOk ? 1 : 0,
        rr.signalLockTried ? 1 : 0,
        rr.signalLockOk ? 1 : 0,
        rr.reason.c_str());

    if (!ok) {
        WriteSystemFailLog_(
            entry,
            L"internal-record-start",
            rr.reason.empty() ? L"内部録画の開始に失敗しました。" : rr.reason);
        return false;
    }

    SystemInfoLogInfo(
        L"内部録画を開始しました。番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d 保存先=\"%ls\" 方式=RawTS",
        SafeTitle_(entry).c_str(),
        SafeService_(entry).c_str(),
        tunerIndex + 1,
        recordPath.wstring().c_str());

    return true;
}



bool Reservation_ExecutionManager::StopInternalRecordReservation_(
    const TaskListManager::TaskEntry& entry,
    const TimeKeeper::TaskRequest& req,
    int tunerIndex,
    const std::wstring& recordPath)
{
    const bool owned =
        IsInternalRecordOwner_(
            tunerIndex,
            req.instance);

    const bool active =
        IsInternalRecordStartingOrRunning_(req.instance) ||
        rawTsRecorder_.IsRunning(req.instance);

    if (!owned && !active) {
        DebugLogFormat(
            L"[ReservationExecution][INTERNAL-RECORD-STOP] already stopped inst=%u tuner=%d path=%ls",
            static_cast<unsigned>(req.instance),
            tunerIndex,
            recordPath.empty() ? L"(unknown)" : recordPath.c_str());

        SystemInfoLogInfo(
            L"内部録画予約は既に停止済みです。番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d",
            SafeTitle_(entry).c_str(),
            SafeService_(entry).c_str(),
            tunerIndex + 1);

        return true;
    }

    const auto rr = rawTsRecorder_.Stop(req.instance);

    const auto eitFlush =
        recordingEitAnalyzer_.Flush(req.instance);
    
    NotifyRecordingEitUpdated_(req.instance,eitFlush);

    recordingEitAnalyzer_.Reset(req.instance);

    DebugLogFormat(
        L"[ReservationExecution][RECORDING-EIT-FLUSH] inst=%u ok=%d wrote=%d appended=%d epgChanged=%d taskCheck=%d space=%d ch=%d nid=%u tsid=%u sid=%u raw=%llu unique=%llu dupSession=%llu add=%llu upd=%llu repl=%llu same=%llu input=%llu eitPkt=%llu before=%llu tmp=%llu after=%llu written=%llu increased=%d path=%ls reason=%ls",
        static_cast<unsigned>(req.instance),
        eitFlush.ok ? 1 : 0,
        eitFlush.wrote ? 1 : 0,
        eitFlush.appended ? 1 : 0,
        eitFlush.epgFileChanged ? 1 : 0,
        eitFlush.taskCheckRecommended ? 1 : 0,
        eitFlush.space,
        eitFlush.physicalChannel,
        static_cast<unsigned>(eitFlush.networkId),
        static_cast<unsigned>(eitFlush.tsId),
        static_cast<unsigned>(eitFlush.serviceId),
        static_cast<unsigned long long>(eitFlush.rawEvents),
        static_cast<unsigned long long>(eitFlush.uniqueEvents),
        static_cast<unsigned long long>(eitFlush.duplicateInSession),
        static_cast<unsigned long long>(eitFlush.updateAdded),
        static_cast<unsigned long long>(eitFlush.updateUpdated),
        static_cast<unsigned long long>(eitFlush.updateReplaced),
        static_cast<unsigned long long>(eitFlush.updateUnchanged),
        static_cast<unsigned long long>(eitFlush.inputPackets),
        static_cast<unsigned long long>(eitFlush.eitPackets),
        static_cast<unsigned long long>(eitFlush.beforeSize),
        static_cast<unsigned long long>(eitFlush.tmpSize),
        static_cast<unsigned long long>(eitFlush.afterSize),
        static_cast<unsigned long long>(eitFlush.writtenBytes),
        eitFlush.sizeIncreased ? 1 : 0,
        eitFlush.outputPath.empty() ? L"(empty)" : eitFlush.outputPath.c_str(),
        eitFlush.reason.empty() ? L"(none)" : eitFlush.reason.c_str());

    MarkInternalRecordStopped_(req.instance, tunerIndex);

    DebugLogFormat(
        L"[ReservationExecution][INTERNAL-RECORD-STOP] inst=%u ok=%d tuner=%d path=%ls bytes=%llu packets=%llu blocks=%llu recvFail=%llu writeFail=%llu callbackFail=%llu reason=%ls",
        static_cast<unsigned>(req.instance),
        rr.ok ? 1 : 0,
        tunerIndex,
        rr.outputPath.empty()
        ? (recordPath.empty() ? L"(unknown)" : recordPath.c_str())
        : rr.outputPath.c_str(),
        static_cast<unsigned long long>(rr.bytesWritten),
        static_cast<unsigned long long>(rr.packetsWritten),
        static_cast<unsigned long long>(rr.blocksWritten),
        static_cast<unsigned long long>(rr.receiveFailures),
        static_cast<unsigned long long>(rr.writeFailures),
        static_cast<unsigned long long>(rr.callbackFailures),
        rr.reason.c_str());

    if (!rr.ok) {
        WriteSystemFailLog_(
            entry,
            L"internal-record-stop",
            rr.reason.empty() ? L"内部録画の停止に失敗しました。" : rr.reason);
        return false;
    }

    SystemInfoLogInfo(
        L"内部録画を停止しました。番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d 保存先=\"%ls\" bytes=%llu packets=%llu recvFail=%llu writeFail=%llu callbackFail=%llu",
        SafeTitle_(entry).c_str(),
        SafeService_(entry).c_str(),
        tunerIndex + 1,
        rr.outputPath.empty()
        ? (recordPath.empty() ? L"(unknown)" : recordPath.c_str())
        : rr.outputPath.c_str(),
        static_cast<unsigned long long>(rr.bytesWritten),
        static_cast<unsigned long long>(rr.packetsWritten),
        static_cast<unsigned long long>(rr.receiveFailures),
        static_cast<unsigned long long>(rr.writeFailures),
        static_cast<unsigned long long>(rr.callbackFailures));

    return true;
}

void Reservation_ExecutionManager::NotifyRecordingEitUpdated_(
    std::uint32_t instance,
    const RecordingEitAnalyzer::FlushResult& result) noexcept
{
    if (!result.ok ||
        !result.wrote ||
        !result.taskCheckRecommended) {
        DebugLogFormat(
            L"[ReservationExecution][RECORDING-EIT-TASK-CHECK] skip inst=%u ok=%d wrote=%d taskCheck=%d path=%ls reason=%ls",
            static_cast<unsigned>(instance),
            result.ok ? 1 : 0,
            result.wrote ? 1 : 0,
            result.taskCheckRecommended ? 1 : 0,
            result.outputPath.empty() ? L"(empty)" : result.outputPath.c_str(),
            result.reason.empty() ? L"(none)" : result.reason.c_str());
        return;
    }

    TaskSystemConnector* taskSystem = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        taskSystem = taskSystem_;
    }

    if (taskSystem == nullptr) {
        DebugLogFormat(
            L"[ReservationExecution][RECORDING-EIT-TASK-CHECK] skip taskSystem=null inst=%u space=%d ch=%d nid=%u tsid=%u sid=%u path=%ls",
            static_cast<unsigned>(instance),
            result.space,
            result.physicalChannel,
            static_cast<unsigned>(result.networkId),
            static_cast<unsigned>(result.tsId),
            static_cast<unsigned>(result.serviceId),
            result.outputPath.empty() ? L"(empty)" : result.outputPath.c_str());
        return;
    }

    DebugLogFormat(
        L"[ReservationExecution][RECORDING-EIT-TASK-CHECK] request inst=%u space=%d ch=%d nid=%u tsid=%u sid=%u unique=%llu written=%llu path=%ls",
        static_cast<unsigned>(instance),
        result.space,
        result.physicalChannel,
        static_cast<unsigned>(result.networkId),
        static_cast<unsigned>(result.tsId),
        static_cast<unsigned>(result.serviceId),
        static_cast<unsigned long long>(result.uniqueEvents),
        static_cast<unsigned long long>(result.writtenBytes),
        result.outputPath.empty() ? L"(empty)" : result.outputPath.c_str());

    // 次段階でここに接続する。
    //
    // taskSystem->CheckProgramReservationsAfterEitUpdate(...);
    //
    // まだ実体を作っていない段階なので、今回は通知ログまで。
}

bool Reservation_ExecutionManager::StopTvTestRecordOwnerForPreparePriority_(
    int tunerIndex,
    std::uint32_t requestInstance,
    const TaskListManager::TaskEntry& requestEntry)
{
    const std::uint32_t owner =
        GetTvTestRecordOwnerOnTuner_(tunerIndex);

    if (owner == 0 || owner == requestInstance) {
        return true;
    }

    TaskListManager::TaskEntry ownerEntry{};
    if (!FindReservationEntry_(owner, ownerEntry)) {
        ownerEntry = requestEntry;
    }

    const std::wstring ownerPath =
        GetActiveRecordFilePath_(owner);

    SystemInfoLogWarn(
        L"次予約の準備時間に入ったため、同一チューナーの前TVTest録画を停止します。次番組=\"%ls\" 放送局=\"%ls\" チューナー=T%d 前instance=%u 次instance=%u",
        SafeTitle_(requestEntry).c_str(),
        SafeService_(requestEntry).c_str(),
        tunerIndex + 1,
        static_cast<unsigned>(owner),
        static_cast<unsigned>(requestInstance));

    eitMonitor_.StopRecordMonitor(owner);

    auto controller = BuildTvTestController_(tunerIndex);
    const auto rec = controller.SendRawCommand(L"STOP_RECORD");

    DebugLogFormat(
        L"[ReservationExecution][TVTEST-PREPARE-PRIORITY] stop previous owner=%u ok=%d response=%ls reason=%ls",
        static_cast<unsigned>(owner),
        rec.ok ? 1 : 0,
        rec.response.c_str(),
        rec.reason.c_str());

    if (!rec.ok) {
        SystemInfoLogWarn(
            L"次予約準備のための前TVTest録画停止に失敗しました。前instance=%u 次instance=%u チューナー=T%d 保存先=\"%ls\" 理由=%ls",
            static_cast<unsigned>(owner),
            static_cast<unsigned>(requestInstance),
            tunerIndex + 1,
            ownerPath.empty() ? L"(unknown)" : ownerPath.c_str(),
            rec.reason.empty() ? L"(unknown)" : rec.reason.c_str());

        return false;
    }

    const auto tsDisable = controller.EnableTs(false);

    DebugLogFormat(
        L"[ReservationExecution][TVTEST-PREPARE-PRIORITY-TS-DISABLE] owner=%u ok=%d response=%ls reason=%ls",
        static_cast<unsigned>(owner),
        tsDisable.ok ? 1 : 0,
        tsDisable.response.empty() ? L"(empty)" : tsDisable.response.c_str(),
        tsDisable.reason.empty() ? L"(none)" : tsDisable.reason.c_str());

    if (!tsDisable.ok) {
        SystemInfoLogWarn(
            L"次予約準備による前TVTest録画停止時のTS出力停止に失敗しました。前instance=%u 理由=%ls",
            static_cast<unsigned>(owner),
            tsDisable.reason.empty() ? L"(unknown)" : tsDisable.reason.c_str());
    }

    MarkTvTestRecordStopped_(owner, tunerIndex);

    SystemInfoLogInfo(
        L"次予約準備のため前TVTest録画を停止しました。前instance=%u 次instance=%u チューナー=T%d 保存先=\"%ls\"",
        static_cast<unsigned>(owner),
        static_cast<unsigned>(requestInstance),
        tunerIndex + 1,
        ownerPath.empty() ? L"(unknown)" : ownerPath.c_str());

    return true;
}

void Reservation_ExecutionManager::StopTvTestRecordOnlyFromMonitor_(
    std::uint32_t instance,
    int tunerIndex,
    const TaskListManager::TaskEntry& entry,
    const std::wstring& recordPath) noexcept
{
    try {
        auto controller = BuildTvTestController_(tunerIndex);

        const auto rec = controller.SendRawCommand(L"STOP_RECORD");

        DebugLogFormat(
            L"[ReservationExecution][TVTEST-RECORD-MONITOR-STOP] inst=%u ok=%d response=%ls reason=%ls",
            static_cast<unsigned>(instance),
            rec.ok ? 1 : 0,
            rec.response.c_str(),
            rec.reason.c_str());

        if (!rec.ok) {
            SystemInfoLogWarn(
                L"EIT照合によるTVTest録画停止に失敗しました。instance=%u 番組=\"%ls\" 放送局=\"%ls\" 保存先=\"%ls\" 理由=%ls",
                static_cast<unsigned>(instance),
                SafeTitle_(entry).c_str(),
                SafeService_(entry).c_str(),
                recordPath.empty() ? L"(unknown)" : recordPath.c_str(),
                rec.reason.empty() ? L"(unknown)" : rec.reason.c_str());
            return;
        }

        const auto tsDisable = controller.EnableTs(false);

        DebugLogFormat(
            L"[ReservationExecution][TVTEST-RECORD-MONITOR-TS-DISABLE] inst=%u ok=%d response=%ls reason=%ls",
            static_cast<unsigned>(instance),
            tsDisable.ok ? 1 : 0,
            tsDisable.response.empty() ? L"(empty)" : tsDisable.response.c_str(),
            tsDisable.reason.empty() ? L"(none)" : tsDisable.reason.c_str());

        if (!tsDisable.ok) {
            SystemInfoLogWarn(
                L"EIT照合によるTVTest録画停止時のTS出力停止に失敗しました。instance=%u 理由=%ls",
                static_cast<unsigned>(instance),
                tsDisable.reason.empty() ? L"(unknown)" : tsDisable.reason.c_str());
        }

        MarkTvTestRecordStopped_(instance, tunerIndex);

        SystemInfoLogWarn(
            L"EIT照合によりTVTest録画を停止しました。instance=%u 番組=\"%ls\" 放送局=\"%ls\" 保存先=\"%ls\" result=1",
            static_cast<unsigned>(instance),
            SafeTitle_(entry).c_str(),
            SafeService_(entry).c_str(),
            recordPath.empty() ? L"(unknown)" : recordPath.c_str());
    }
    catch (...) {
        DebugLogFormat(
            L"[ReservationExecution][TVTEST-RECORD-MONITOR-STOP][EXCEPTION] inst=%u",
            static_cast<unsigned>(instance));
    }
}