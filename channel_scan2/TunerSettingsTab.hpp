#pragma once
#ifndef TUNER_SETTINGS_TAB_HPP
#define TUNER_SETTINGS_TAB_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <string>
#include <vector>
#include <cstdint>

class ConfigManager;

class TunerSettingsTab
{
public:
    bool Create(HWND hParent, const RECT& rc, HINSTANCE hInst, HFONT hFont, ConfigManager& cfg);
    void Show(bool show);
    void Move(const RECT& rc);

    void ApplyToConfig();
    void ReloadFromConfig();
    void UpdateTunerCount(int tunerCount);

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    void OnCreate();
    void LayoutChildren(int width, int height);
    void LayoutPageControls(int pageWidth, int pageHeight);

    void RebuildTunerTabs();
    void RebuildSubTabs();

    void ShowSelectedTunerPage();
    void ShowSelectedSubPage();

    void LoadPageFromConfig(int tunerIndex);
    void SavePageToConfig(int tunerIndex);

    void UpdateControlEnableState();
    void RedrawActiveSubPage();

    bool BrowseForFile(HWND hEdit, const wchar_t* title, const wchar_t* filter, const wchar_t* defExt);
    void BrowseBonDriverPath();
    void BrowseExternalAppPath();
    void BrowseTVTestExePath();
    void ResetTVTestPipePathsToDefault();
    void RebuildTVTestTestTabs();
    void ShowSelectedTVTestTestPage();

    void BrowseInternalBonDriverPath();
    void BrowseTVTestBonDriverPath();

    std::wstring GetInternalBonDriverText() const;
    std::wstring GetTVTestBonDriverText() const;

    void RunExternalAppTest();
    bool LaunchExternalApp(const std::wstring& path);

    void RunTVTestLaunchTest();
    void RunTVTestPingTest();
    void RunTVTestStateTest();
    void RunTVTestProgramTest();
    void RunTVTestStatusDetailTest();
    void RunTVTestReloadIniTest();
    void RunTVTestEnableTs();
    void RunTVTestDisableTs();
    void ReloadTVTestChannelList();

    void ReloadTimeShiftChannelList();
    int GetSelectedTimeShiftChannelIndex() const noexcept;
    void SelectTimeShiftLockChannelFromConfig(int tunerIndex);
    void SaveTimeShiftLockChannelToConfig(int tunerIndex);
    void RunTVTestSetChannelTest();
    void RunTVTestTsReadTest();
    void RunTVTestWatchReservationFlowTest();

    bool SendTVTestCommand(const std::wstring& command, std::wstring* response = nullptr);
    std::wstring GetTVTestControlPipeText() const;
    std::wstring GetTVTestTsPipeText() const;

    int GetCurrentTunerIndex() const noexcept;
    int GetCurrentSubTabIndex() const noexcept;

    struct TVTestChannelEntry
    {
        std::wstring displayName;
        std::wstring serviceName;

        int space = 0;
        int channel = 0;
        std::uint16_t serviceId = 0;
        std::uint16_t networkId = 0;
        std::uint16_t tsid = 0;
        int status = 0;
    };

    struct TimeShiftChannelEntry
    {
        std::wstring displayName;
        std::wstring serviceName;

        int space = 0;
        int channel = -1;
        std::uint16_t serviceId = 0;
        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;
    };

    int GetSelectedTVTestChannelIndex() const noexcept;

private:
    HINSTANCE hInst_ = nullptr;
    HWND hWnd_ = nullptr;
    HFONT hFont_ = nullptr;
    ConfigManager* cfg_ = nullptr;

    HWND hTunerTab_ = nullptr;
    HWND hSubTab_ = nullptr;

    HWND hBasicPage_ = nullptr;
    HWND hOperationPage_ = nullptr;
    HWND hExternalPage_ = nullptr;
    HWND hTVTestPage_ = nullptr;
    HWND hTVTestTestTab_ = nullptr;
    HWND hTVTestBasicTestPage_ = nullptr;
    HWND hTVTestAdvancedTestPage_ = nullptr;

    HWND hChkEnabled_ = nullptr;
    HWND hEdName_ = nullptr;
    HWND hEdBonDriver_ = nullptr;
    HWND hBtnBrowseBonDriver_ = nullptr;
    HWND hCbReceiveMode_ = nullptr;

    HWND hEdInternalBonDriver_ = nullptr;
    HWND hBtnBrowseInternalBonDriver_ = nullptr;

    HWND hEdTVTestBonDriver_ = nullptr;
    HWND hBtnBrowseTVTestBonDriver_ = nullptr;

    HWND hCbUseRole_ = nullptr;
    HWND hChkAllowParallel_ = nullptr;
    HWND hEdPriority_ = nullptr;

    HWND hLblTimeShiftLockChannel_ = nullptr;
    HWND hCbTimeShiftLockChannel_ = nullptr;
    HWND hBtnTimeShiftReloadChannels_ = nullptr;

    HWND hChkUseExternalApp_ = nullptr;
    HWND hEdExternalApp_ = nullptr;
    HWND hBtnBrowseExternalApp_ = nullptr;
    HWND hBtnTestExternalApp_ = nullptr;

    HWND hChkUseTVTest_ = nullptr;
    HWND hEdTVTestExe_ = nullptr;
    HWND hBtnBrowseTVTestExe_ = nullptr;
    HWND hBtnTVTestResetPipes_ = nullptr;
    HWND hEdTVTestControlPipe_ = nullptr;
    HWND hEdTVTestTsPipe_ = nullptr;
    HWND hBtnTVTestLaunch_ = nullptr;
    HWND hBtnTVTestPing_ = nullptr;
    HWND hBtnTVTestState_ = nullptr;
    HWND hBtnTVTestProgram_ = nullptr;
    HWND hBtnTVTestStatusDetail_ = nullptr;
    HWND hBtnTVTestReloadIni_ = nullptr;
    HWND hBtnTVTestEnableTs_ = nullptr;
    HWND hBtnTVTestDisableTs_ = nullptr;
    HWND hLblTVTestChannel_ = nullptr;
    HWND hCbTVTestChannelList_ = nullptr;
    HWND hBtnTVTestReloadChannelList_ = nullptr;
    HWND hBtnTVTestSetChannel_ = nullptr;
    HWND hBtnTVTestTsRead_ = nullptr;
    HWND hBtnTVTestWatchReservationFlowTest_ = nullptr;

    std::vector<TVTestChannelEntry> tvtestChannelEntries_;
    std::vector<TimeShiftChannelEntry> timeShiftChannelEntries_;
  

    int tunerCount_ = 1;
    int loadedTunerIndex_ = 0;
};

#endif // TUNER_SETTINGS_TAB_HPP