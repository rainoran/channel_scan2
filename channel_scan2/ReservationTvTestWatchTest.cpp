#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ReservationTvTestWatchTest.hpp"

#include "TVTestIntegration.hpp"
#include "DebugManager.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <ctime>
#include <string>

namespace
{
    using Clock = std::chrono::system_clock;

    std::wstring TrimCopy_(const std::wstring& s)
    {
        const std::wstring ws = L" \t\r\n";

        const std::size_t first = s.find_first_not_of(ws);
        if (first == std::wstring::npos) {
            return {};
        }

        const std::size_t last = s.find_last_not_of(ws);
        return s.substr(first, last - first + 1);
    }

    std::wstring MakeIndexedKey_(const wchar_t* base, int index)
    {
        return std::wstring(base) + std::to_wstring(index);
    }

    std::wstring ReadIniString_(
        const std::wstring& iniPath,
        const wchar_t* section,
        const std::wstring& key,
        const wchar_t* defValue = L"")
    {
        wchar_t buf[4096]{};

        ::GetPrivateProfileStringW(
            section,
            key.c_str(),
            defValue,
            buf,
            static_cast<DWORD>(_countof(buf)),
            iniPath.c_str());

        return TrimCopy_(buf);
    }

    int ReadIniInt_(
        const std::wstring& iniPath,
        const wchar_t* section,
        const std::wstring& key,
        int defValue)
    {
        return static_cast<int>(
            ::GetPrivateProfileIntW(
                section,
                key.c_str(),
                defValue,
                iniPath.c_str()));
    }

    std::wstring NormalizeForMatch_(const std::wstring& s)
    {
        std::wstring out;
        out.reserve(s.size());

        for (wchar_t ch : s) {
            if (ch == L'\u3000' || ch == L' ' || ch == L'\t' ||
                ch == L'\r' || ch == L'\n') {
                continue;
            }

            out.push_back(static_cast<wchar_t>(std::towlower(ch)));
        }

        return out;
    }

    bool ContainsNormalized_(
        const std::wstring& haystack,
        const std::wstring& needle)
    {
        const std::wstring h = NormalizeForMatch_(haystack);
        const std::wstring n = NormalizeForMatch_(needle);

        if (n.empty()) {
            return false;
        }

        return h.find(n) != std::wstring::npos;
    }

    bool ParseIsoDateTime_(
        const std::wstring& date,
        const std::wstring& time,
        Clock::time_point& out)
    {
        if (date.size() < 10 || time.size() < 5) {
            return false;
        }

        int y = 0;
        int mo = 0;
        int d = 0;
        int h = 0;
        int mi = 0;
        int s = 0;

        if (swscanf_s(date.c_str(), L"%d-%d-%d", &y, &mo, &d) != 3) {
            return false;
        }

        const int scanned = swscanf_s(time.c_str(), L"%d:%d:%d", &h, &mi, &s);
        if (scanned < 2) {
            return false;
        }

        if (scanned == 2) {
            s = 0;
        }

        if (y <= 0 || mo <= 0 || d <= 0 ||
            h < 0 || h > 23 ||
            mi < 0 || mi > 59 ||
            s < 0 || s > 59) {
            return false;
        }

        std::tm tmv{};
        tmv.tm_year = y - 1900;
        tmv.tm_mon = mo - 1;
        tmv.tm_mday = d;
        tmv.tm_hour = h;
        tmv.tm_min = mi;
        tmv.tm_sec = s;
        tmv.tm_isdst = -1;

        const std::time_t tt = std::mktime(&tmv);
        if (tt == static_cast<std::time_t>(-1)) {
            return false;
        }

        out = Clock::from_time_t(tt);
        return true;
    }

    bool IsValidRequest_(
        const ReservationTvTestWatchTest::Request& req,
        std::wstring& reason)
    {
        if (req.tunerIndex < 0) {
            reason = L"tunerIndex is invalid";
            return false;
        }

        if (req.networkId == 0 || req.tsId == 0 || req.serviceId == 0) {
            reason = L"networkId / tsId / serviceId is required";
            return false;
        }

        if (req.requireProgramTitleMatch && TrimCopy_(req.title).empty()) {
            reason = L"title is required when requireProgramTitleMatch is true";
            return false;
        }

        return true;
    }

    TVTestIntegration::Settings LoadSettingsFromConfig_(
        const std::wstring& configIniPath,
        int tunerIndex)
    {
        TVTestIntegration::Settings s{};

        if (tunerIndex < 0) {
            tunerIndex = 0;
        }

        const std::wstring section =
            L"Tuner" + std::to_wstring(tunerIndex);

        s.enabled =
            ReadIniInt_(
                configIniPath,
                section.c_str(),
                MakeIndexedKey_(L"TvTestUseBridge", tunerIndex),
                0) != 0;

        s.didIndex =
            ReadIniInt_(
                configIniPath,
                section.c_str(),
                MakeIndexedKey_(L"TvTestDid", tunerIndex),
                tunerIndex);

        s.useDidSuffix = false;

        s.tvtestExePath =
            ReadIniString_(
                configIniPath,
                section.c_str(),
                MakeIndexedKey_(L"TvTestExePath", tunerIndex),
                L"");

        if (s.tvtestExePath.empty()) {
            s.tvtestExePath =
                ReadIniString_(
                    configIniPath,
                    section.c_str(),
                    MakeIndexedKey_(L"ExternalAppPath", tunerIndex),
                    L"");
        }

        s.bonDriverPath =
            ReadIniString_(
                configIniPath,
                section.c_str(),
                MakeIndexedKey_(L"BonDriverPath", tunerIndex),
                L"");

        s.controlPipeName =
            ReadIniString_(
                configIniPath,
                section.c_str(),
                MakeIndexedKey_(L"TvTestControlPipeName", tunerIndex),
                L"\\\\.\\pipe\\TVTestBridge_{TUNER}");

        s.tsPipeName =
            ReadIniString_(
                configIniPath,
                section.c_str(),
                MakeIndexedKey_(L"TvTestTsPipeName", tunerIndex),
                L"\\\\.\\pipe\\TVTestBridgeTS_{TUNER}");

        const int requestTimeoutMs =
            ReadIniInt_(
                configIniPath,
                L"Network",
                L"RequestTimeoutMs",
                5000);

        s.pipeTimeoutMs =
            static_cast<DWORD>(
                std::clamp(requestTimeoutMs, 1500, 15000));

        s.launchWaitMs = 3000;

        DebugLogFormat(
            L"[ReservationTvTestWatchTest] load tuner config section=%ls tuner=%d enabled=%d did=%d exe=%ls bon=%ls pipe=%ls tsPipe=%ls",
            section.c_str(),
            tunerIndex,
            s.enabled ? 1 : 0,
            s.didIndex,
            s.tvtestExePath.empty() ? L"(empty)" : s.tvtestExePath.c_str(),
            s.bonDriverPath.empty() ? L"(empty)" : s.bonDriverPath.c_str(),
            s.controlPipeName.empty() ? L"(empty)" : s.controlPipeName.c_str(),
            s.tsPipeName.empty() ? L"(empty)" : s.tsPipeName.c_str());

        return s;
    }

    bool WaitForPing_(
        const TVTestIntegration& tvtest,
        DWORD waitMs,
        DWORD intervalMs,
        std::wstring& lastResponse)
    {
        const ULONGLONG start = ::GetTickCount64();
        const DWORD safeInterval = intervalMs < 50 ? 50 : intervalMs;

        while (::GetTickCount64() - start < waitMs) {
            std::wstring response;
            if (tvtest.Ping(&response)) {
                lastResponse = response.empty() ? L"(response empty)" : response;
                return true;
            }

            ::Sleep(safeInterval);
        }

        lastResponse.clear();
        return false;
    }

    std::wstring BuildSetChannelByIdsCommand_(
        const ReservationTvTestWatchTest::Request& req)
    {
        wchar_t buf[160]{};

        _snwprintf_s(
            buf,
            _countof(buf),
            _TRUNCATE,
            L"SET_CHANNEL_BY_IDS %d %u %u %u",
            req.space,
            static_cast<unsigned>(req.networkId),
            static_cast<unsigned>(req.tsId),
            static_cast<unsigned>(req.serviceId));

        return std::wstring(buf);
    }

    std::wstring BuildSetChannelFallbackCommand_(
        const ReservationTvTestWatchTest::Request& req)
    {
        wchar_t buf[160]{};

        if (req.serviceId != 0) {
            _snwprintf_s(
                buf,
                _countof(buf),
                _TRUNCATE,
                L"SET_CHANNEL %d %d %u",
                req.space,
                req.channel,
                static_cast<unsigned>(req.serviceId));
        }
        else {
            _snwprintf_s(
                buf,
                _countof(buf),
                _TRUNCATE,
                L"SET_CHANNEL %d %d",
                req.space,
                req.channel);
        }

        return std::wstring(buf);
    }

    struct MatchResult
    {
        bool channelMatched = false;
        bool programMatched = false;
        bool matched = false;

        std::wstring combinedText;
    };

    MatchResult CheckReservationMatch_(
        const ReservationTvTestWatchTest::Request& req,
        const std::wstring& stateResponse,
        const std::wstring& statusResponse,
        const std::wstring& programResponse)
    {
        MatchResult r{};

        r.combinedText.reserve(
            stateResponse.size() +
            statusResponse.size() +
            programResponse.size() +
            16);

        r.combinedText += stateResponse;
        r.combinedText += L"\n";
        r.combinedText += statusResponse;
        r.combinedText += L"\n";
        r.combinedText += programResponse;

        const std::wstring sidText =
            std::to_wstring(static_cast<unsigned>(req.serviceId));

        const std::wstring eidText =
            std::to_wstring(static_cast<unsigned>(req.eventId));

        const bool serviceNameHit =
            !req.serviceName.empty() &&
            ContainsNormalized_(r.combinedText, req.serviceName);

        const bool serviceIdHit =
            !sidText.empty() &&
            r.combinedText.find(sidText) != std::wstring::npos;

        r.channelMatched = serviceNameHit || serviceIdHit;

        const bool titleHit =
            !req.title.empty() &&
            ContainsNormalized_(programResponse, req.title);

        const bool eventIdHit =
            req.eventId != 0 &&
            r.combinedText.find(eidText) != std::wstring::npos;

        r.programMatched = titleHit || eventIdHit;

        if (req.requireProgramTitleMatch) {
            r.matched = r.programMatched;
        }
        else {
            r.matched = r.programMatched || r.channelMatched;
        }

        return r;
    }

    void QueryTvTestSnapshot_(
        const TVTestIntegration& tvtest,
        ReservationTvTestWatchTest::Result& result)
    {
        std::wstring state;
        if (tvtest.QueryState(state)) {
            result.stateResponse = state.empty() ? L"(response empty)" : state;
        }

        std::wstring status;
        if (tvtest.QueryStatus(status)) {
            result.statusResponse = status.empty() ? L"(response empty)" : status;
        }

        std::wstring program;
        if (tvtest.QueryProgram(program)) {
            result.programResponse = program.empty() ? L"(response empty)" : program;
        }
    }

    bool ExtractIntField_(
        const std::wstring& text,
        const wchar_t* key,
        int& outValue)
    {
        outValue = 0;

        if (key == nullptr || key[0] == L'\0') {
            return false;
        }

        const std::wstring pattern = std::wstring(key) + L"=";
        const std::size_t pos = text.find(pattern);
        if (pos == std::wstring::npos) {
            return false;
        }

        std::size_t p = pos + pattern.size();
        bool negative = false;

        if (p < text.size() && text[p] == L'-') {
            negative = true;
            ++p;
        }

        if (p >= text.size() || text[p] < L'0' || text[p] > L'9') {
            return false;
        }

        int value = 0;
        while (p < text.size() && text[p] >= L'0' && text[p] <= L'9') {
            value = value * 10 + static_cast<int>(text[p] - L'0');
            ++p;
        }

        outValue = negative ? -value : value;
        return true;
    }

    struct TvTestStateInfo_
    {
        bool hasSpace = false;
        bool hasChannel = false;
        bool hasSid = false;
        bool hasTsid = false;
        bool hasNid = false;

        int space = -1;
        int channel = -1;
        int sid = 0;
        int tsid = 0;
        int nid = 0;
    };

    TvTestStateInfo_ ParseTvTestStateInfo_(const std::wstring& stateText)
    {
        TvTestStateInfo_ s{};

        s.hasSpace = ExtractIntField_(stateText, L"space", s.space);
        s.hasChannel = ExtractIntField_(stateText, L"channel", s.channel);
        s.hasSid = ExtractIntField_(stateText, L"sid", s.sid);
        s.hasTsid = ExtractIntField_(stateText, L"tsid", s.tsid);
        s.hasNid = ExtractIntField_(stateText, L"nid", s.nid);

        return s;
    }

    bool IsTargetChannelState_(
        const ReservationTvTestWatchTest::Request& req,
        const std::wstring& stateText)
    {
        const TvTestStateInfo_ s = ParseTvTestStateInfo_(stateText);

        if (!s.hasSpace || !s.hasSid) {
            return false;
        }

        if (s.space != req.space) {
            return false;
        }

        if (s.sid != static_cast<int>(req.serviceId)) {
            return false;
        }

        if (req.networkId != 0) {
            if (!s.hasNid || s.nid != static_cast<int>(req.networkId)) {
                return false;
            }
        }

        if (req.tsId != 0) {
            if (!s.hasTsid || s.tsid != static_cast<int>(req.tsId)) {
                return false;
            }
        }

        if (req.channel >= 0 && s.hasChannel) {
            if (s.channel != req.channel) {
                return false;
            }
        }

        return true;
    }

    bool QueryControlSnapshot_(
        const TVTestIntegration& tvtest,
        ReservationTvTestWatchTest::Result& result,
        bool needStatus)
    {
        bool ok = true;

        std::wstring ping;
        if (tvtest.Ping(&ping)) {
            result.pingResponse = ping.empty() ? L"(response empty)" : ping;
        }
        else {
            ok = false;
        }

        std::wstring state;
        if (tvtest.QueryState(state)) {
            result.stateResponse = state.empty() ? L"(response empty)" : state;
        }
        else {
            ok = false;
        }

        if (needStatus) {
            std::wstring status;
            if (tvtest.QueryStatus(status)) {
                result.statusResponse = status.empty() ? L"(response empty)" : status;
            }
            else {
                ok = false;
            }
        }

        return ok;
    }

    bool WaitTvTestControlReady_(
        const TVTestIntegration& tvtest,
        const ReservationTvTestWatchTest::Request& req,
        ReservationTvTestWatchTest::Result& result)
    {
        const DWORD timeoutMs =
            req.pingWaitMs < 5000 ? 5000 : req.pingWaitMs;

        const DWORD intervalMs =
            req.pingIntervalMs < 100 ? 100 : req.pingIntervalMs;

        const ULONGLONG start = ::GetTickCount64();

        int consecutiveReady = 0;
        int attempt = 0;

        DebugLogFormat(
            L"[ReservationTvTestWatchTest] wait-control-ready-v2 begin timeout=%lu interval=%lu",
            static_cast<unsigned long>(timeoutMs),
            static_cast<unsigned long>(intervalMs));

        while (::GetTickCount64() - start < timeoutMs) {
            ++attempt;

            std::wstring ping;
            const bool pingOk = tvtest.Ping(&ping);

            if (pingOk) {
                result.pingResponse =
                    ping.empty() ? L"(response empty)" : ping;
            }

            // PING直後に連続でGET_STATEを投げると、
            // 現状のTVTestBridgeでは open failed が起きやすい。
            // control-ready段階ではGET_STATEを必須にしない。
            ::Sleep(80);

            std::wstring status;
            const bool statusOk = tvtest.QueryStatus(status);

            if (statusOk) {
                result.statusResponse =
                    status.empty() ? L"(response empty)" : status;
            }

            const bool hasPong =
                result.pingResponse.find(L"OK PONG") != std::wstring::npos;

            const bool hasStatus =
                result.statusResponse.find(L"STATUS ") != std::wstring::npos;

            const bool ready =
                (pingOk || hasPong) &&
                (statusOk || hasStatus);

            if (ready) {
                ++consecutiveReady;
            }
            else {
                consecutiveReady = 0;
            }

            DebugLogFormat(
                L"[ReservationTvTestWatchTest] wait-control-ready-v2 attempt=%d pingOk=%d statusOk=%d ready=%d consecutive=%d ping=%ls status=%ls",
                attempt,
                pingOk ? 1 : 0,
                statusOk ? 1 : 0,
                ready ? 1 : 0,
                consecutiveReady,
                result.pingResponse.empty() ? L"(empty)" : result.pingResponse.c_str(),
                result.statusResponse.empty() ? L"(empty)" : result.statusResponse.c_str());

            if (consecutiveReady >= 2) {
                DebugLog(L"[ReservationTvTestWatchTest] wait-control-ready-v2 ok");
                return true;
            }

            ::Sleep(intervalMs);
        }

        DebugLogFormat(
            L"[ReservationTvTestWatchTest] wait-control-ready-v2 timeout ping=%ls status=%ls state=%ls",
            result.pingResponse.empty() ? L"(empty)" : result.pingResponse.c_str(),
            result.statusResponse.empty() ? L"(empty)" : result.statusResponse.c_str(),
            result.stateResponse.empty() ? L"(empty)" : result.stateResponse.c_str());

        return false;
    }

    bool WaitTvTestChannelReady_(
        const TVTestIntegration& tvtest,
        const ReservationTvTestWatchTest::Request& req,
        ReservationTvTestWatchTest::Result& result,
        DWORD timeoutMs)
    {
        const DWORD safeTimeoutMs = timeoutMs < 3000 ? 3000 : timeoutMs;
        const ULONGLONG start = ::GetTickCount64();

        int attempt = 0;

        DebugLogFormat(
            L"[ReservationTvTestWatchTest] wait-channel-ready begin target space=%d ch=%d nid=%u tsid=%u sid=%u timeout=%lu",
            req.space,
            req.channel,
            static_cast<unsigned>(req.networkId),
            static_cast<unsigned>(req.tsId),
            static_cast<unsigned>(req.serviceId),
            static_cast<unsigned long>(safeTimeoutMs));

        while (::GetTickCount64() - start < safeTimeoutMs) {
            ++attempt;

            std::wstring state;
            if (tvtest.QueryState(state)) {
                result.stateResponse = state.empty() ? L"(response empty)" : state;

                if (IsTargetChannelState_(req, result.stateResponse)) {
                    DebugLogFormat(
                        L"[ReservationTvTestWatchTest] wait-channel-ready ok attempt=%d state=%ls",
                        attempt,
                        result.stateResponse.c_str());
                    return true;
                }
            }

            std::wstring status;
            if (tvtest.QueryStatus(status)) {
                result.statusResponse = status.empty() ? L"(response empty)" : status;
            }

            DebugLogFormat(
                L"[ReservationTvTestWatchTest] wait-channel-ready pending attempt=%d state=%ls status=%ls",
                attempt,
                result.stateResponse.c_str(),
                result.statusResponse.c_str());

            ::Sleep(400);
        }

        DebugLogFormat(
            L"[ReservationTvTestWatchTest] wait-channel-ready timeout state=%ls status=%ls",
            result.stateResponse.c_str(),
            result.statusResponse.c_str());

        return false;
    }

    bool SendSetChannelAndWaitReady_(
        const TVTestIntegration& tvtest,
        const ReservationTvTestWatchTest::Request& req,
        ReservationTvTestWatchTest::Result& result)
    {
        const std::wstring idsCommand = BuildSetChannelByIdsCommand_(req);

        std::wstring idsResponse;
        const bool idsSendOk =
            tvtest.SendRawCommand(idsCommand, &idsResponse);

        result.setChannelResponse =
            idsResponse.empty() ? L"(response empty)" : idsResponse;

        DebugLogFormat(
            L"[ReservationTvTestWatchTest] SET_CHANNEL_BY_IDS send ok=%d cmd=%ls response=%ls",
            idsSendOk ? 1 : 0,
            idsCommand.c_str(),
            result.setChannelResponse.c_str());

        if (WaitTvTestChannelReady_(
            tvtest,
            req,
            result,
            req.verifyTimeoutMs)) {
            result.channelSet = true;
            return true;
        }

        if (req.channel < 0) {
            DebugLog(L"[ReservationTvTestWatchTest] fallback skipped because request.channel < 0");
            return false;
        }

        const std::wstring fallbackCommand =
            BuildSetChannelFallbackCommand_(req);

        std::wstring fallbackResponse;
        const bool fallbackSendOk =
            tvtest.SetChannel(
                req.space,
                req.channel,
                req.serviceId,
                &fallbackResponse);

        result.setChannelResponse =
            fallbackResponse.empty()
            ? result.setChannelResponse
            : fallbackResponse;

        DebugLogFormat(
            L"[ReservationTvTestWatchTest] fallback SET_CHANNEL send ok=%d cmd=%ls response=%ls",
            fallbackSendOk ? 1 : 0,
            fallbackCommand.c_str(),
            result.setChannelResponse.c_str());

        if (WaitTvTestChannelReady_(
            tvtest,
            req,
            result,
            req.verifyTimeoutMs)) {
            result.channelSet = true;
            return true;
        }

        return false;
    }

    bool WaitTvTestProgramMatched_(
        const TVTestIntegration& tvtest,
        const ReservationTvTestWatchTest::Request& req,
        ReservationTvTestWatchTest::Result& result,
        DWORD timeoutMs)
    {
        const DWORD safeTimeoutMs = timeoutMs < 3000 ? 3000 : timeoutMs;
        const ULONGLONG start = ::GetTickCount64();

        int attempt = 0;

        while (::GetTickCount64() - start < safeTimeoutMs) {
            ++attempt;

            QueryTvTestSnapshot_(tvtest, result);

            const MatchResult match = CheckReservationMatch_(
                req,
                result.stateResponse,
                result.statusResponse,
                result.programResponse);

            if (match.matched) {
                DebugLogFormat(
                    L"[ReservationTvTestWatchTest] wait-program-matched ok attempt=%d state=%ls program=%ls",
                    attempt,
                    result.stateResponse.c_str(),
                    result.programResponse.c_str());
                return true;
            }

            DebugLogFormat(
                L"[ReservationTvTestWatchTest] wait-program-matched pending attempt=%d state=%ls program=%ls",
                attempt,
                result.stateResponse.c_str(),
                result.programResponse.c_str());

            ::Sleep(1000);
        }

        return false;
    }

    bool CloseTvTestIfAllowed_(
        const TVTestIntegration& tvtest,
        const ReservationTvTestWatchTest::Request& req,
        bool launchedByThisTest,
        ReservationTvTestWatchTest::Result& result)
    {
        if (!req.closeTvTestOnFinish) {
            return true;
        }

        if (req.closeOnlyIfLaunchedByThisTest && !launchedByThisTest) {
            DebugLog(L"[ReservationTvTestWatchTest] close skipped because TVTest was not launched by this test");
            return true;
        }

        std::wstring closeResponse;
        const bool ok = tvtest.CloseTVTest(&closeResponse);

        result.closeSent = ok;
        result.closeResponse =
            closeResponse.empty() ? L"(response empty)" : closeResponse;

        return ok;
    }

    void SetFail_(
        ReservationTvTestWatchTest::Result& result,
        ReservationTvTestWatchTest::Status status,
        const wchar_t* stage,
        const std::wstring& message)
    {
        result.ok = false;
        result.status = status;
        result.stage = stage ? stage : L"unknown";
        result.message = message;
    }

    void SetOk_(
        ReservationTvTestWatchTest::Result& result,
        ReservationTvTestWatchTest::Status status,
        const wchar_t* stage,
        const std::wstring& message)
    {
        result.ok = true;
        result.status = status;
        result.stage = stage ? stage : L"done";
        result.message = message;
    }
}

namespace ReservationTvTestWatchTest
{
    Result RunWatchTestFromConfig(
        const std::wstring& configIniPath,
        const Request& request)
    {
        Result result{};

        std::wstring reason;
        if (!IsValidRequest_(request, reason)) {
            SetFail_(result, Status::InvalidArgument, L"validate", reason);
            DebugLogFormat(
                L"[ReservationTvTestWatchTest] validate failed: %ls",
                reason.c_str());
            return result;
        }

        TVTestIntegration::Settings settings =
            LoadSettingsFromConfig_(
                configIniPath,
                request.tunerIndex);

        TVTestIntegration tvtest;
        tvtest.Configure(settings);

        DebugLogFormat(
            L"[ReservationTvTestWatchTest] begin tuner=%d title=%ls service=%ls space=%d ch=%d nid=%u tsid=%u sid=%u eid=%u enabled=%d exe=%ls pipe=%ls",
            request.tunerIndex,
            request.title.empty() ? L"(empty)" : request.title.c_str(),
            request.serviceName.empty() ? L"(empty)" : request.serviceName.c_str(),
            request.space,
            request.channel,
            static_cast<unsigned>(request.networkId),
            static_cast<unsigned>(request.tsId),
            static_cast<unsigned>(request.serviceId),
            static_cast<unsigned>(request.eventId),
            settings.enabled ? 1 : 0,
            settings.tvtestExePath.c_str(),
            settings.controlPipeName.c_str());

        if (!settings.enabled) {
            SetFail_(
                result,
                Status::ConfigDisabled,
                L"config",
                L"TunerTVTest UseBridge is disabled");
            return result;
        }

        std::wstring pingResponse;
        if (!tvtest.Ping(&pingResponse)) {
            if (!request.launchIfNeeded) {
                SetFail_(
                    result,
                    Status::PipeUnavailable,
                    L"ping",
                    L"TVTest pipe is not available");
                return result;
            }

            DebugLog(L"[ReservationTvTestWatchTest] initial PING failed. LaunchTVTest.");

            if (!tvtest.LaunchTVTest()) {
                SetFail_(
                    result,
                    Status::LaunchFailed,
                    L"launch",
                    L"LaunchTVTest failed");
                return result;
            }

            result.launchedByThisTest = true;

            if (!WaitForPing_(
                tvtest,
                request.pingWaitMs,
                request.pingIntervalMs,
                pingResponse)) {
                SetFail_(
                    result,
                    Status::PipeUnavailable,
                    L"wait-ping",
                    L"TVTest pipe did not become ready");
                return result;
            }
        }

        result.pingResponse =
            pingResponse.empty() ? L"(response empty)" : pingResponse;

        if (!WaitTvTestControlReady_(
            tvtest,
            request,
            result)) {
            CloseTvTestIfAllowed_(
                tvtest,
                request,
                result.launchedByThisTest,
                result);

            SetFail_(
                result,
                Status::PipeUnavailable,
                L"control-ready",
                L"TVTest control pipe did not become stable");

            return result;
        }

        if (!SendSetChannelAndWaitReady_(
            tvtest,
            request,
            result)) {
            DebugLogFormat(
                L"[ReservationTvTestWatchTest] set-channel failed after monitoring. state=%ls status=%ls program=%ls",
                result.stateResponse.c_str(),
                result.statusResponse.c_str(),
                result.programResponse.c_str());

            CloseTvTestIfAllowed_(
                tvtest,
                request,
                result.launchedByThisTest,
                result);

            SetFail_(
                result,
                Status::SetChannelFailed,
                L"set-channel",
                L"SET_CHANNEL_BY_IDS / SET_CHANNEL did not reach target channel");

            return result;
        }

        if (!WaitTvTestProgramMatched_(
            tvtest,
            request,
            result,
            request.verifyTimeoutMs)) {
            DebugLogFormat(
                L"[ReservationTvTestWatchTest] verify failed after channel ready title=%ls service=%ls state=%ls status=%ls program=%ls",
                request.title.c_str(),
                request.serviceName.c_str(),
                result.stateResponse.c_str(),
                result.statusResponse.c_str(),
                result.programResponse.c_str());

            if (request.closeOnMismatch) {
                CloseTvTestIfAllowed_(
                    tvtest,
                    request,
                    result.launchedByThisTest,
                    result);
            }

            SetFail_(
                result,
                Status::VerifyFailed,
                L"verify",
                L"reserved program verification failed");

            return result;
        }

        result.verified = true;

        DebugLogFormat(
            L"[ReservationTvTestWatchTest] verify ok title=%ls service=%ls state=%ls program=%ls",
            request.title.empty() ? L"(empty)" : request.title.c_str(),
            request.serviceName.empty() ? L"(empty)" : request.serviceName.c_str(),
            result.stateResponse.empty() ? L"(empty)" : result.stateResponse.c_str(),
            result.programResponse.empty() ? L"(empty)" : result.programResponse.c_str());

        Clock::time_point endTime{};
        const bool hasEndTime =
            ParseIsoDateTime_(request.endDate, request.endTime, endTime);

        const Clock::time_point monitorStart = Clock::now();
        int mismatchCount = 0;

        while (true) {
            const Clock::time_point now = Clock::now();

            if (hasEndTime && now >= endTime) {
                DebugLog(L"[ReservationTvTestWatchTest] end time reached");
                break;
            }

            if (request.maxMonitorMs > 0) {
                const auto elapsedMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - monitorStart).count();

                if (elapsedMs >= static_cast<long long>(request.maxMonitorMs)) {
                    DebugLog(L"[ReservationTvTestWatchTest] maxMonitorMs reached");
                    break;
                }
            }

            const DWORD sleepMs =
                request.monitorPollMs < 1000 ? 1000 : request.monitorPollMs;

            ::Sleep(sleepMs);

            QueryTvTestSnapshot_(tvtest, result);

            const MatchResult match = CheckReservationMatch_(
                request,
                result.stateResponse,
                result.statusResponse,
                result.programResponse);

            if (match.matched) {
                mismatchCount = 0;
                continue;
            }

            ++mismatchCount;

            DebugLogFormat(
                L"[ReservationTvTestWatchTest] monitor mismatch count=%d/%d title=%ls program=%ls",
                mismatchCount,
                request.mismatchLimit,
                request.title.empty() ? L"(empty)" : request.title.c_str(),
                result.programResponse.empty() ? L"(empty)" : result.programResponse.c_str());

            if (mismatchCount >= std::max(1, request.mismatchLimit)) {
                if (request.closeOnMismatch) {
                    CloseTvTestIfAllowed_(
                        tvtest,
                        request,
                        result.launchedByThisTest,
                        result);
                }

                SetFail_(
                    result,
                    Status::ProgramMismatch,
                    L"monitor",
                    L"program mismatch while monitoring");
                return result;
            }
        }

        const bool closeOk =
            CloseTvTestIfAllowed_(
                tvtest,
                request,
                result.launchedByThisTest,
                result);

        if (!closeOk) {
            SetFail_(
                result,
                Status::CloseFailed,
                L"close",
                L"CloseTVTest failed");
            return result;
        }

        SetOk_(
            result,
            Status::Finished,
            L"done",
            L"watch reservation TVTest test finished");

        DebugLogFormat(
            L"[ReservationTvTestWatchTest] finished ok title=%ls closeSent=%d closeResponse=%ls",
            request.title.empty() ? L"(empty)" : request.title.c_str(),
            result.closeSent ? 1 : 0,
            result.closeResponse.empty() ? L"(empty)" : result.closeResponse.c_str());

        return result;
    }
}
