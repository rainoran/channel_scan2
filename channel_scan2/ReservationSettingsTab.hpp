#pragma once
#ifndef RESERVATION_SETTINGS_TAB_HPP
#define RESERVATION_SETTINGS_TAB_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <string>

#include "ConfigManager.h"

class ReservationSettingsTab {
public:
    ReservationSettingsTab() = default;
    ReservationSettingsTab(const ReservationSettingsTab&) = delete;
    ReservationSettingsTab& operator=(const ReservationSettingsTab&) = delete;

    bool Create(HWND hParent, const RECT& rcPage, HINSTANCE hInst, HFONT hFont,
        ConfigManager& cfg,
        int tunerCount,
        const std::wstring& title = L"予約設定");

    void Move(const RECT& rcPage);
    void Show(bool visible) noexcept;

    void ApplyToConfig();
    void ReloadFromConfig();
    void UpdateTunerCount(int tunerCount);

private:
    bool CreateControls_(const std::wstring& title);
    void ApplyToControls_();
    void ReadFromControls_();
    void LoadFromConfig_();
    void WriteToConfig_();

    void RebuildTunerCombo_(int tunerCount);
    void RebuildConflictCombo_();
    void RebuildEpgReserveButtonModeCombo_();
    void ApplyFont_(HWND h) const noexcept;

    static LRESULT CALLBACK PageWndProc_(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT PageWndProcImpl_(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    static constexpr int kStartAdvanceMinSec = 0;
    static constexpr int kStartAdvanceMaxSec = 600;

    static constexpr int kStartAdjustMinSec = -1;
    static constexpr int kStartAdjustMaxSec = +1;

    static constexpr int kEndAdjustMinMin = -30;
    static constexpr int kEndAdjustMaxMin = +180;

private:
    HINSTANCE m_hInst = nullptr;
    HFONT m_hFont = nullptr;

    HWND m_hParent = nullptr;
    HWND m_hPage = nullptr;

    ConfigManager* m_cfg = nullptr;
    int m_tunerCount = 1;

    // cached values
    int m_startAdvanceSec = 30;
    int m_startAdjustSec = 0;
    int m_endAdjustMin = 0;
    int m_preferredTuner = -1;
    ConfigManager::ReservationConflictPolicy m_conflictPolicy =
        ConfigManager::ReservationConflictPolicy::AutoTryNextTuner;
    bool m_pauseEpgWhileRecording = true;
    ConfigManager::EpgReserveButtonMode m_epgReserveButtonMode =
        ConfigManager::EpgReserveButtonMode::Select;

    // ---- UI parts ----
    HWND m_hTitle = nullptr;
    HWND m_hSepLine = nullptr;

    HWND m_hStartAdvanceLabel = nullptr;
    HWND m_hStartAdvanceEdit = nullptr;
    HWND m_hStartAdvanceSpin = nullptr;

    HWND m_hStartAdjustLabel = nullptr;
    HWND m_hStartAdjustEdit = nullptr;
    HWND m_hStartAdjustSpin = nullptr;

    HWND m_hEndAdjustLabel = nullptr;
    HWND m_hEndAdjustEdit = nullptr;
    HWND m_hEndAdjustSpin = nullptr;

    HWND m_hPreferredTunerLabel = nullptr;
    HWND m_hPreferredTunerCombo = nullptr;

    HWND m_hConflictLabel = nullptr;
    HWND m_hConflictCombo = nullptr;

    HWND m_hEpgReserveButtonModeLabel = nullptr;
    HWND m_hEpgReserveButtonModeCombo = nullptr;

    HWND m_hPauseEpgCheck = nullptr;

    HWND m_hApplyBtn = nullptr;
    HWND m_hReloadBtn = nullptr;
};

#endif // RESERVATION_SETTINGS_TAB_HPP