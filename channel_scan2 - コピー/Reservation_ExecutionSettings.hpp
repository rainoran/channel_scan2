#pragma once
#ifndef RESERVATION_EXECUTION_SETTINGS_HPP
#define RESERVATION_EXECUTION_SETTINGS_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <string>

#include "TVTestIntegration.hpp"

class ConfigManager;

struct Reservation_ExecutionSettings
{
    bool enableExecution = true;
    bool enableWatchReservation = true;
    bool enableTvTestWatch = true;
    bool enableRecordExecution = true;
    bool enableTsPipeDuringWatch = false;

    // 1 = 録画中もTVTest映像表示
    // 0 = 録画予約でTVTestを新規起動する時に /nodshow を付ける
    bool tvTestRecordShowVideo = true;

    // 今回 config.ini に残す予約実行設定
    std::wstring recordFolder;
    int prepareSeconds = 30;

    // 以下は config.ini からは削除したが、
    // 既存コード互換用に内部既定値として残す。
    bool enableEitMonitorDuringWatch = false;
    bool enableAutoAdjustReservationTime = false;
    bool enableEpgUpdateFromWatching = false;

    bool useSetSilent = false;
    bool useSetPreview = true;
    bool enableDebugLog = false;

    int eitMonitorIntervalSec = 10;
    int tvTestConnectTimeoutMs = 5000;
    int tvTestCommandTimeoutMs = 5000;
    int tvTestLaunchWaitMs = 3000;

    // set_service        : SET_SERVICE <serviceId>
    // set_channel        : SET_CHANNEL <space> <channel> <serviceId>
    // set_channel_by_ids : SET_CHANNEL_BY_IDS <space> <networkId> <tsId> <serviceId>
    std::wstring tvTestChannelCommandMode = L"set_channel_by_ids";
};

namespace Reservation_Config
{
    constexpr const wchar_t* SectionName = L"ReservationExecution";

    void EnsureDefaults(ConfigManager& cfg);
    Reservation_ExecutionSettings Load(ConfigManager& cfg);
    void Save(ConfigManager& cfg, const Reservation_ExecutionSettings& s);

    TVTestIntegration::Settings LoadTVTestSettingsForTuner(
        ConfigManager& cfg,
        int tunerIndex,
        const Reservation_ExecutionSettings& reservationSettings);
}

#endif // RESERVATION_EXECUTION_SETTINGS_HPP
