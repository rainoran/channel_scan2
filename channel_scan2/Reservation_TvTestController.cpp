#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Reservation_TvTestController.hpp"

#include "ConfigManager.h"
#include "DebugManager.h"

#include <Windows.h>
#include <algorithm>
#include <cwctype>
#include <sstream>

namespace
{
    std::wstring ToLowerTrim_(std::wstring s)
    {
        auto isWs = [](wchar_t c) noexcept {
            return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
        };
        while (!s.empty() && isWs(s.front())) s.erase(s.begin());
        while (!s.empty() && isWs(s.back())) s.pop_back();
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return s;
    }

    bool IsKeyBoundaryChar_(wchar_t ch) noexcept
    {
        return ch == L' ' ||
            ch == L'\t' ||
            ch == L'\r' ||
            ch == L'\n' ||
            ch == L';' ||
            ch == L',' ||
            ch == L'|' ||
            ch == L'(' ||
            ch == L'[';
    }

    bool TryReadIntKey_(const std::wstring& text, const wchar_t* key, int& outValue)
    {
        if (key == nullptr || key[0] == L'\0') {
            return false;
        }

        const std::wstring token = std::wstring(key) + L"=";

        std::size_t pos = 0;
        while (true) {
            pos = text.find(token, pos);
            if (pos == std::wstring::npos) {
                return false;
            }

            if (pos == 0 || IsKeyBoundaryChar_(text[pos - 1])) {
                break;
            }

            pos += token.size();
        }

        const wchar_t* begin = text.c_str() + pos + token.size();
        wchar_t* end = nullptr;

        const long v = std::wcstol(begin, &end, 10);
        if (end == begin) {
            return false;
        }

        outValue = static_cast<int>(v);
        return true;
    }

    struct TVTestStateSnapshot_
    {
        int space = -1;
        int channel = -1;
        int sid = 0;
        int tsid = 0;
        int nid = 0;
        int signal = 0;
        int bitrate = 0;
    };

    bool ParseTVTestStateSnapshot_(
        const std::wstring& text,
        TVTestStateSnapshot_& out)
    {
        bool any = false;

        int value = 0;

        if (TryReadIntKey_(text, L"space", value)) {
            out.space = value;
            any = true;
        }

        if (TryReadIntKey_(text, L"channel", value)) {
            out.channel = value;
            any = true;
        }

        if (TryReadIntKey_(text, L"sid", value)) {
            out.sid = value;
            any = true;
        }

        if (TryReadIntKey_(text, L"tsid", value)) {
            out.tsid = value;
            any = true;
        }

        if (TryReadIntKey_(text, L"nid", value)) {
            out.nid = value;
            any = true;
        }

        if (TryReadIntKey_(text, L"signal", value)) {
            out.signal = value;
            any = true;
        }

        if (TryReadIntKey_(text, L"bitrate", value)) {
            out.bitrate = value;
            any = true;
        }

        return any;
    }
}// namespace

void Reservation_TvTestController::Configure(const Reservation_ExecutionSettings& settings)
{
    settings_ = settings;
}

void Reservation_TvTestController::SetRecordLaunchMode(bool recordLaunchMode) noexcept
{
    recordLaunchMode_ = recordLaunchMode;
}
bool Reservation_TvTestController::ConfigureForTuner(ConfigManager& cfg, int tunerIndex)
{
    tunerIndex_ = tunerIndex;

    TVTestIntegration::Settings tvs =
        Reservation_Config::LoadTVTestSettingsForTuner(
            cfg,
            tunerIndex,
            settings_);

    if (recordLaunchMode_ && !settings_.tvTestRecordShowVideo) {
        if (!tvs.extraLaunchArgs.empty()) {
            tvs.extraLaunchArgs += L" ";
        }

        tvs.extraLaunchArgs += L"/nodshow";

        DebugLogFormat(
            L"[ReservationTvTestController] record launch mode: add /nodshow tuner=%d",
            tunerIndex);
    }
    else {
        DebugLogFormat(
            L"[ReservationTvTestController] record launch mode=%d showVideo=%d tuner=%d",
            recordLaunchMode_ ? 1 : 0,
            settings_.tvTestRecordShowVideo ? 1 : 0,
            tunerIndex);
    }

    tvtest_.Configure(tvs);
    return tvs.enabled;
}


bool Reservation_TvTestController::IsEnabled() const noexcept
{
    return tvtest_.IsEnabled();
}

Reservation_TvTestController::Result Reservation_TvTestController::Fail_(std::wstring reason) const
{
    Result r{};
    r.ok = false;
    r.reason = std::move(reason);
    return r;
}

Reservation_TvTestController::Result Reservation_TvTestController::Ok_(std::wstring response) const
{
    Result r{};
    r.ok = true;
    r.response = std::move(response);
    return r;
}

Reservation_TvTestController::Result Reservation_TvTestController::EnsureReady()
{
    if (!tvtest_.IsEnabled()) {
        return Fail_(L"TVTest bridge is disabled for this tuner");
    }

    std::wstring response;
    if (tvtest_.Ping(&response)) {
        return Ok_(response.empty() ? L"PING OK" : response);
    }

    if (!tvtest_.LaunchTVTest()) {
        return Fail_(L"TVTest launch failed");
    }

    if (settings_.tvTestLaunchWaitMs > 0) {
        ::Sleep(static_cast<DWORD>(settings_.tvTestLaunchWaitMs));
    }

    response.clear();
    if (!tvtest_.Ping(&response)) {
        return Fail_(L"TVTest pipe ping failed after launch");
    }

    return Ok_(response.empty() ? L"PING OK after launch" : response);
}

Reservation_TvTestController::Result Reservation_TvTestController::ApplyViewOptions()
{
    if (recordLaunchMode_ && !settings_.tvTestRecordShowVideo) {
        DebugLogFormat(
            L"[ReservationTvTestController] skip view options because /nodshow record mode tuner=%d",
            tunerIndex_);

        return Ok_(L"view options skipped for /nodshow record mode");
    }

    std::wstring response;

    if (!tvtest_.SetSilent(settings_.useSetSilent, &response)) {
        return Fail_(L"SET_SILENT failed");
    }

    response.clear();
    if (!tvtest_.SetPreview(settings_.useSetPreview, &response)) {
        return Fail_(L"SET_PREVIEW failed");
    }

    return Ok_(response);
}

std::wstring Reservation_TvTestController::BuildReservationChannelCommand_(
    const TaskListManager::TaskEntry& entry) const
{
    const auto& r = entry.reserve;
    const std::wstring mode = ToLowerTrim_(settings_.tvTestChannelCommandMode);

    if (mode == L"set_service") {
        if (r.serviceId == 0) {
            return {};
        }
        std::wostringstream oss;
        oss << L"SET_SERVICE " << static_cast<unsigned>(r.serviceId);
        return oss.str();
    }

    if (mode == L"set_channel") {
        // 予約データにはTVTestのチャンネルリスト順が無いケースがあるため、channel=-1は失敗扱いになりやすい。
        // まずはset_channel_by_idsを推奨する。
        if (r.serviceId == 0) {
            return {};
        }
        std::wostringstream oss;
        oss << L"SET_CHANNEL " << r.space << L" " << -1 << L" " << static_cast<unsigned>(r.serviceId);
        return oss.str();
    }

    // 既定: SET_CHANNEL_BY_IDS <space> <networkId> <tsId> <serviceId>
    if (r.networkId == 0 || r.tsId == 0 || r.serviceId == 0) {
        return {};
    }

    std::wostringstream oss;
    oss << L"SET_CHANNEL_BY_IDS "
        << r.space << L" "
        << static_cast<unsigned>(r.networkId) << L" "
        << static_cast<unsigned>(r.tsId) << L" "
        << static_cast<unsigned>(r.serviceId);
    return oss.str();
}

Reservation_TvTestController::Result Reservation_TvTestController::SelectReservationService(
    const TaskListManager::TaskEntry& entry)
{
    const std::wstring command = BuildReservationChannelCommand_(entry);
    if (command.empty()) {
        return Fail_(L"TVTest channel command is unavailable because reservation IDs are incomplete");
    }

    std::wstring response;
    if (!tvtest_.SendRawCommand(command, &response)) {
        return Fail_(std::wstring(L"TVTest command failed: ") + command);
    }

    DebugLogFormat(
        L"[ReservationTvTestController][CHANNEL-COMMAND] tuner=%d command=%ls response=%ls",
        tunerIndex_,
        command.c_str(),
        response.empty() ? L"(empty)" : response.c_str());

    Result ready =
        WaitReservationServiceReady(
            entry,
            6000,
            300);

    if (!ready.ok) {
        std::wostringstream oss;
        oss << L"TVTest channel switch was not confirmed. command="
            << command
            << L" reason="
            << ready.reason;

        return Fail_(oss.str());
    }

    return Ok_(ready.response.empty() ? response : ready.response);
}

Reservation_TvTestController::Result
Reservation_TvTestController::ValidateReservationServiceState_(
    const TaskListManager::TaskEntry& entry,
    const std::wstring& stateText) const
{
    const auto& r = entry.reserve;

    TVTestStateSnapshot_ state{};
    if (!ParseTVTestStateSnapshot_(stateText, state)) {
        return Fail_(L"TVTest state parse failed");
    }

    if (r.serviceId == 0) {
        return Fail_(L"reservation serviceId is empty");
    }

    if (state.sid != static_cast<int>(r.serviceId)) {
        std::wostringstream oss;
        oss << L"serviceId mismatch. expected="
            << static_cast<unsigned>(r.serviceId)
            << L" actual="
            << state.sid;
        return Fail_(oss.str());
    }

    if (r.networkId != 0 &&
        state.nid != static_cast<int>(r.networkId)) {
        std::wostringstream oss;
        oss << L"networkId mismatch. expected="
            << static_cast<unsigned>(r.networkId)
            << L" actual="
            << state.nid;
        return Fail_(oss.str());
    }

    if (r.tsId != 0 &&
        state.tsid != static_cast<int>(r.tsId)) {
        std::wostringstream oss;
        oss << L"tsId mismatch. expected="
            << static_cast<unsigned>(r.tsId)
            << L" actual="
            << state.tsid;
        return Fail_(oss.str());
    }

    if (r.space >= 0 && state.space != r.space) {
        std::wostringstream oss;
        oss << L"space mismatch. expected="
            << r.space
            << L" actual="
            << state.space;
        return Fail_(oss.str());
    }

    DebugLogFormat(
        L"[ReservationTvTestController][CHANNEL-READY] tuner=%d space=%d channel=%d nid=%d tsid=%d sid=%d signal=%d bitrate=%d",
        tunerIndex_,
        state.space,
        state.channel,
        state.nid,
        state.tsid,
        state.sid,
        state.signal,
        state.bitrate);

    return Ok_(stateText);
}

Reservation_TvTestController::Result
Reservation_TvTestController::WaitReservationServiceReady(
    const TaskListManager::TaskEntry& entry,
    DWORD timeoutMs,
    DWORD intervalMs)
{
    if (timeoutMs < 500) {
        timeoutMs = 500;
    }

    if (intervalMs < 50) {
        intervalMs = 50;
    }

    const ULONGLONG begin = ::GetTickCount64();
    const ULONGLONG deadline = begin + static_cast<ULONGLONG>(timeoutMs);

    std::wstring lastState;
    std::wstring lastReason = L"no state";

    for (;;) {
        std::wstring stateText;
        if (tvtest_.QueryState(stateText)) {
            lastState = stateText;

            Result check = ValidateReservationServiceState_(entry, stateText);
            if (check.ok) {
                return check;
            }

            lastReason = check.reason;

            DebugLogFormat(
                L"[ReservationTvTestController][CHANNEL-WAIT] tuner=%d not ready reason=%ls state=%ls",
                tunerIndex_,
                check.reason.empty() ? L"(unknown)" : check.reason.c_str(),
                stateText.empty() ? L"(empty)" : stateText.c_str());
        }
        else {
            lastReason = L"GET_STATE failed";

            DebugLogFormat(
                L"[ReservationTvTestController][CHANNEL-WAIT] tuner=%d GET_STATE failed",
                tunerIndex_);
        }

        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline) {
            break;
        }

        const ULONGLONG remain = deadline - now;
        const DWORD sleepMs =
            static_cast<DWORD>((std::min<ULONGLONG>)(
                static_cast<ULONGLONG>(intervalMs),
                remain));

        if (sleepMs > 0) {
            ::Sleep(sleepMs);
        }
    }

    std::wostringstream oss;
    oss << L"TVTest channel switch timeout. tuner="
        << tunerIndex_
        << L" reason="
        << lastReason;

    if (!lastState.empty()) {
        oss << L" lastState=" << lastState;
    }

    return Fail_(oss.str());
}

Reservation_TvTestController::Result Reservation_TvTestController::QueryStatus()
{
    std::wstring response;
    if (!tvtest_.QueryStatus(response)) {
        return Fail_(L"GET_STATUS failed");
    }
    return Ok_(response);
}

Reservation_TvTestController::Result Reservation_TvTestController::QueryProgram()
{
    std::wstring response;
    if (!tvtest_.QueryProgram(response)) {
        return Fail_(L"GET_PROGRAM failed");
    }
    return Ok_(response);
}

Reservation_TvTestController::Result Reservation_TvTestController::EnableTs(bool enable)
{
    std::wstring response;
    if (!tvtest_.EnableTs(enable, &response)) {
        return Fail_(enable ? L"ENABLE_TS failed" : L"DISABLE_TS failed");
    }
    return Ok_(response);
}

namespace
{
    std::wstring QuoteTvTestArgLocal_(const std::wstring& text)
    {
        std::wstring out;
        out.reserve(text.size() + 2);
        out.push_back(L'"');

        for (wchar_t ch : text) {
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
}

Reservation_TvTestController::Result Reservation_TvTestController::StartRecord(
    const std::wstring& outputPath)
{
    if (outputPath.empty()) {
        return Fail_(L"START_RECORD output path is empty");
    }

    const std::wstring command =
        L"START_RECORD " + QuoteTvTestArgLocal_(outputPath);

    std::wstring response;
    if (!tvtest_.SendRawCommand(command, &response)) {
        return Fail_(response.empty() ? L"START_RECORD failed" : response);
    }

    return Ok_(response.empty() ? command : response);
}

Reservation_TvTestController::Result Reservation_TvTestController::StopRecord()
{
    std::wstring response;
    if (!tvtest_.SendRawCommand(L"STOP_RECORD", &response)) {
        return Fail_(response.empty() ? L"STOP_RECORD failed" : response);
    }

    return Ok_(response.empty() ? L"STOP_RECORD OK" : response);
}

Reservation_TvTestController::Result Reservation_TvTestController::CloseTvTest()
{
    std::wstring response;

    if (!tvtest_.CloseTVTest(&response)) {
        return Fail_(
            response.empty()
            ? L"TVTest CLOSE failed"
            : response);
    }

    return Ok_(
        response.empty()
        ? L"CLOSE OK"
        : response);
}

Reservation_TvTestController::Result Reservation_TvTestController::SendRawCommand(const std::wstring& command)
{
    std::wstring response;
    if (!tvtest_.SendRawCommand(command, &response)) {
        return Fail_(std::wstring(L"raw command failed: ") + command);
    }
    return Ok_(response);
}
