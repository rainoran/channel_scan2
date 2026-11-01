#pragma once
#ifndef SETTINGS_TAB_HPP
#define SETTINGS_TAB_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <string>
#include <functional>
#include <vector>

#include "GuiActions.hpp"

class SettingsTab
{
public:
    bool Create(HWND hParent, const RECT& rc, HINSTANCE hInst, HFONT hFont,
        std::function<GuiActions& ()>&& getActions,
        std::function<void(std::wstring_view)>&& log);

    void Show(bool show);
    void Move(const RECT& rc);

    void ApplyToConfig();
    void ReloadFromConfig();

private:
    enum class PageIndex : int
    {
        BonDriver = 0,
        General = 1,
        Reservation = 2,
        TimeShift = 3,
        Advanced = 4,
        Count = 5
    };

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    void OnCreate();
    void OnSize(int w, int h);

    void LoadFromConfigToUI();
    void SaveFromUIToConfig();

    void BrowseTerrestrialBonDriverFile();
    void BrowseSatelliteBonDriverFile();
    bool BrowseBonDriverFileToEdit_(HWND targetEdit, const wchar_t* logLabel);

    std::wstring GetSelectedScannedBonDriverPath_() const;
    void ApplySelectedBonDriverToTerrestrial_();
    void ApplySelectedBonDriverToSatellite_();

    
    void BrowseScanFolder();
    void BrowseReservationRecordFolder();
    void BrowseTimeShiftRootFolder();

    void ScanBonDriverInFolder();
    void OnBonListSelChanged();

    bool GetStartupRegistered_() const;
    bool SetStartupRegistered_(bool enable, std::wstring& outErr) const;

    void RegisterPageControl_(PageIndex page, HWND child);
    void ShowSettingsPage_(PageIndex page);
    RECT GetSettingsPageRect_() const;

    HWND CreatePageStatic_(
        PageIndex page,
        const wchar_t* text,
        int x,
        int y,
        int w,
        int h);

    HWND CreatePageButton_(
        PageIndex page,
        const wchar_t* text,
        DWORD style,
        int x,
        int y,
        int w,
        int h,
        int id);

    HWND CreatePageEdit_(
        PageIndex page,
        DWORD exStyle,
        DWORD style,
        int x,
        int y,
        int w,
        int h,
        int id);

    HWND CreatePageCombo_(
        PageIndex page,
        DWORD exStyle,
        DWORD style,
        int x,
        int y,
        int w,
        int h,
        int id);

private:
    HINSTANCE hInst = nullptr;
    HWND hWnd = nullptr;
    HFONT hFont = nullptr;

    std::function<GuiActions& ()> getActions;
    std::function<void(std::wstring_view)> log;

    HWND hSettingsTab = nullptr;
    PageIndex currentPage_ = PageIndex::BonDriver;

    std::vector<HWND> pageBonDriver_;
    std::vector<HWND> pageGeneral_;
    std::vector<HWND> pageReservation_;
    std::vector<HWND> pageTimeShift_;
    std::vector<HWND> pageAdvanced_;

    HWND lblBonTerrestrial = nullptr;
    HWND lblBonSatellite = nullptr;
    HWND lblScanDir = nullptr;
    HWND lblBonList = nullptr;
    HWND lblTunerCount = nullptr;
    HWND lblReservationRecordDir = nullptr;
    
    
    
    

    HWND lblTimeShiftRootFolder = nullptr;
    HWND lblTimeShiftMaxBytesGB = nullptr;

    HWND lblCpu = nullptr;
    HWND lblEnc = nullptr;

    HWND edBonTerrestrial = nullptr;
    HWND btBrowseTerrestrial = nullptr;

    HWND edBonSatellite = nullptr;
    HWND btBrowseSatellite = nullptr;

    HWND btSetBonTerrestrial = nullptr;
    HWND btSetBonSatellite = nullptr;

    HWND edScanDir = nullptr;
    HWND btBrowseDir = nullptr;
    HWND btScan = nullptr;
    HWND cbBonList = nullptr;

    HWND chkStartup = nullptr;
    HWND chkMinimizeToTray = nullptr;
    HWND edTunerCount = nullptr;
    HWND spTunerCount = nullptr;

    HWND chkWriteFile = nullptr;

    HWND chkReservationUseTvTest = nullptr;
    HWND chkTvTestRecordShowVideo = nullptr;
    HWND edReservationRecordDir = nullptr;
    HWND btReservationRecordDir = nullptr;

    HWND edTimeShiftRootFolder = nullptr;
    HWND btTimeShiftRootFolder = nullptr;
    HWND edTimeShiftMaxBytesGB = nullptr;
    HWND spTimeShiftMaxBytesGB = nullptr;

    HWND cbCpu = nullptr;
    HWND cbEnc = nullptr;
    HWND btReload = nullptr;
    HWND btSave = nullptr;

    std::vector<std::wstring> scannedBonPaths;
};

#endif // SETTINGS_TAB_HPP