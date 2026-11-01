#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Reservation_ExecutionSettings.hpp"

#include "ConfigManager.h"
#include "DebugManager.h"

#include <cwchar>
#include <Windows.h>
#include <algorithm>
#include <cwctype>
#include <string>

namespace
{
    constexpr const wchar_t* kMissingValue = L"__TSCEPG_RESERVATION_EXEC_MISSING__";

    int ClampInt_(int v, int lo, int hi) noexcept
    {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    }

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

    std::wstring GetIniPath_(const ConfigManager& cfg)
    {
        const std::wstring& p = cfg.GetIniPath();
        return p;
    }

    std::wstring ReadIniString_(
        const ConfigManager& cfg,
        const wchar_t* section,
        const wchar_t* key,
        const wchar_t* defValue)
    {
        const std::wstring ini = GetIniPath_(cfg);
        if (ini.empty()) {
            return defValue ? std::wstring(defValue) : std::wstring();
        }

        wchar_t buf[1024]{};
        ::GetPrivateProfileStringW(
            section,
            key,
            defValue ? defValue : L"",
            buf,
            static_cast<DWORD>(std::size(buf)),
            ini.c_str());

        return std::wstring(buf);
    }

    int ReadIniInt_(
        const ConfigManager& cfg,
        const wchar_t* section,
        const wchar_t* key,
        int defValue)
    {
        const std::wstring ini = GetIniPath_(cfg);
        if (ini.empty()) {
            return defValue;
        }

        return static_cast<int>(::GetPrivateProfileIntW(
            section,
            key,
            defValue,
            ini.c_str()));
    }

    bool ReadIniBool_(
        const ConfigManager& cfg,
        const wchar_t* section,
        const wchar_t* key,
        bool defValue)
    {
        const std::wstring raw = ToLowerTrim_(ReadIniString_(
            cfg,
            section,
            key,
            defValue ? L"1" : L"0"));

        if (raw == L"1" || raw == L"true" || raw == L"yes" || raw == L"on") {
            return true;
        }
        if (raw == L"0" || raw == L"false" || raw == L"no" || raw == L"off") {
            return false;
        }
        return defValue;
    }

    bool WriteIniString_(
        const ConfigManager& cfg,
        const wchar_t* section,
        const wchar_t* key,
        const std::wstring& value)
    {
        const std::wstring ini = GetIniPath_(cfg);
        if (ini.empty()) {
            return false;
        }

        return ::WritePrivateProfileStringW(
            section,
            key,
            value.c_str(),
            ini.c_str()) != FALSE;
    }

    bool WriteIniInt_(
        const ConfigManager& cfg,
        const wchar_t* section,
        const wchar_t* key,
        int value)
    {
        wchar_t buf[64]{};
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d", value);
        return WriteIniString_(cfg, section, key, buf);
    }

    bool WriteIniBool_(
        const ConfigManager& cfg,
        const wchar_t* section,
        const wchar_t* key,
        bool value)
    {
        return WriteIniString_(cfg, section, key, value ? L"1" : L"0");
    }

    bool IsIniKeyMissing_(
        const ConfigManager& cfg,
        const wchar_t* section,
        const wchar_t* key)
    {
        const std::wstring ini = GetIniPath_(cfg);
        if (ini.empty()) {
            return true;
        }

        wchar_t buf[128]{};
        ::GetPrivateProfileStringW(
            section,
            key,
            kMissingValue,
            buf,
            static_cast<DWORD>(std::size(buf)),
            ini.c_str());

        return std::wstring(buf) == kMissingValue;
    }

    void EnsureDefault_(
        const ConfigManager& cfg,
        const wchar_t* section,
        const wchar_t* key,
        const wchar_t* defValue)
    {
        if (IsIniKeyMissing_(cfg, section, key)) {
            WriteIniString_(cfg, section, key, defValue ? std::wstring(defValue) : std::wstring());
        }
    }

    std::wstring NormalizeChannelCommandMode_(std::wstring mode)
    {
        mode = ToLowerTrim_(std::move(mode));
        if (mode == L"set_channel" || mode == L"setchannel") {
            return L"set_channel";
        }
        if (mode == L"set_service" || mode == L"setservice") {
            return L"set_service";
        }
        if (mode == L"set_channel_by_ids" || mode == L"setchannelbyids" || mode == L"ids") {
            return L"set_channel_by_ids";
        }
        return L"set_channel_by_ids";
    }
}

void Reservation_Config::EnsureDefaults(ConfigManager& cfg)
{
    // ConfigManager::EnsureDefault は private なので直接呼ばない。
    // このユニット側では GetIniPath() + WinAPI で不足キーだけを追記する。
    EnsureDefault_(cfg, SectionName, L"EnableExecution", L"1");
    EnsureDefault_(cfg, SectionName, L"EnableWatchReservation", L"1");
    EnsureDefault_(cfg, SectionName, L"EnableTvTestWatch", L"1");
    EnsureDefault_(cfg, SectionName, L"EnableRecordExecution", L"1");
    EnsureDefault_(cfg, SectionName, L"EnableTsPipeDuringWatch", L"0");
    EnsureDefault_(cfg, SectionName, L"TvTestRecordShowVideo", L"1");
    EnsureDefault_(cfg, SectionName, L"RecordFolder", L"");
    EnsureDefault_(cfg, SectionName, L"PrepareSeconds", L"30");
}

Reservation_ExecutionSettings Reservation_Config::Load(ConfigManager& cfg)
{
    EnsureDefaults(cfg);

    // 念のため、ConfigManager側も最新値を読んでいる前提。
    // MainUI側で既に LoadConfig() 済みなら二重読み込み不要。
    Reservation_ExecutionSettings s{};

    s.enableExecution =
        cfg.GetReservationExecutionEnableExecution();

    s.enableWatchReservation =
        cfg.GetReservationExecutionEnableWatchReservation();

    s.enableTvTestWatch =
        cfg.GetReservationExecutionEnableTvTestWatch();

    s.enableRecordExecution =
        cfg.GetReservationExecutionEnableRecordExecution();

    s.enableTsPipeDuringWatch =
        cfg.GetReservationExecutionEnableTsPipeDuringWatch();

    s.tvTestRecordShowVideo =
        cfg.GetReservationExecutionTvTestRecordShowVideo();

    s.recordFolder =
        cfg.GetReservationExecutionRecordFolder();

    s.prepareSeconds =
        ClampInt_(cfg.GetReservationExecutionPrepareSeconds(), 0, 3600);

    // 以下は config.ini からは削除した旧詳細項目。
    // 既存コード互換のため、内部既定値として固定する。
    s.enableEitMonitorDuringWatch = false;
    s.enableAutoAdjustReservationTime = false;
    s.enableEpgUpdateFromWatching = false;

    s.useSetSilent = false;
    s.useSetPreview = true;
    s.enableDebugLog = false;

    s.eitMonitorIntervalSec = 10;
    s.tvTestConnectTimeoutMs = 5000;
    s.tvTestCommandTimeoutMs = 5000;
    s.tvTestLaunchWaitMs = 3000;
    s.tvTestChannelCommandMode = L"set_channel_by_ids";

    return s;
}

void Reservation_Config::Save(ConfigManager& cfg, const Reservation_ExecutionSettings& s)
{
    cfg.SetReservationExecutionEnableExecution(s.enableExecution);
    cfg.SetReservationExecutionEnableWatchReservation(s.enableWatchReservation);
    cfg.SetReservationExecutionEnableTvTestWatch(s.enableTvTestWatch);
    cfg.SetReservationExecutionEnableRecordExecution(s.enableRecordExecution);
    cfg.SetReservationExecutionEnableTsPipeDuringWatch(s.enableTsPipeDuringWatch);
    cfg.SetReservationExecutionTvTestRecordShowVideo(s.tvTestRecordShowVideo);
    cfg.SetReservationExecutionRecordFolder(s.recordFolder);
    cfg.SetReservationExecutionPrepareSeconds(ClampInt_(s.prepareSeconds, 0, 3600));
}

TVTestIntegration::Settings Reservation_Config::LoadTVTestSettingsForTuner(
    ConfigManager& cfg,
    int tunerIndex,
    const Reservation_ExecutionSettings& reservationSettings)
{
    const int tunerCount = cfg.GetTunersCount();

    if (tunerIndex < 0 || tunerIndex >= tunerCount) {
        tunerIndex = 0;
    }

    TVTestIntegration::Settings tv{};

    tv.enabled = cfg.GetTunerTvTestUseBridge(tunerIndex);

    // Tuner0 -> /DID 0, Tuner1 -> /DID 1
    // config側に TvTestDidX があるが、まずはチューナー番号と一致させる。
    // DID文字列の変換を複雑化させるより、TVTestBridge_0 / _1 と一致させる方が安全。
    tv.didIndex = tunerIndex;

    // 新方式では pipe に _A/_B を後付けしない。
    // config.ini の TvTestControlPipeNameX / TvTestTsPipeNameX をそのまま使う。
    tv.useDidSuffix = false;

    // TVTest本体。
    // 新設定の TvTestExePathX を優先し、空なら旧互換として ExternalAppPathX、
    // それも空なら CommonExternalAppPath に落とす。
    tv.tvtestExePath = cfg.GetTunerTvTestExePath(tunerIndex);
    if (tv.tvtestExePath.empty()) {
        tv.tvtestExePath = cfg.GetTunerExternalAppPath(tunerIndex);
    }
    if (tv.tvtestExePath.empty()) {
        tv.tvtestExePath = cfg.GetTunerGlobalCommonExternalAppPath();
    }

    // TVTest起動時に /d で渡す BonDriver。
    // 内部用 InternalBonDriverPathX は絶対に使わない。
    tv.bonDriverPath = cfg.GetTunerTvTestBonDriverPath(tunerIndex);
    if (tv.bonDriverPath.empty()) {
        // 旧互換。古い設定では BonDriverPathX をTVTest用にも使っていたため fallback する。
        tv.bonDriverPath = cfg.GetBonDriverPathForTuner(tunerIndex);
    }

    tv.controlPipeName = cfg.GetTunerTvTestControlPipeName(tunerIndex);
    tv.tsPipeName = cfg.GetTunerTvTestTsPipeName(tunerIndex);

    tv.pipeTimeoutMs = static_cast<DWORD>(
        ClampInt_(
            reservationSettings.tvTestCommandTimeoutMs,
            100,
            60000));

    tv.launchWaitMs = static_cast<DWORD>(
        ClampInt_(
            reservationSettings.tvTestLaunchWaitMs,
            0,
            60000));

    DebugLogFormat(
        L"[Reservation_Config][TVTest] tuner=%d enabled=%d exe=%ls bon=%ls did=%d control=%ls ts=%ls",
        tunerIndex,
        tv.enabled ? 1 : 0,
        tv.tvtestExePath.empty() ? L"(empty)" : tv.tvtestExePath.c_str(),
        tv.bonDriverPath.empty() ? L"(empty)" : tv.bonDriverPath.c_str(),
        tv.didIndex,
        tv.controlPipeName.empty() ? L"(empty)" : tv.controlPipeName.c_str(),
        tv.tsPipeName.empty() ? L"(empty)" : tv.tsPipeName.c_str());

    return tv;
}
