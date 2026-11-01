#pragma once
#ifndef NETWORK_SETTINGS_TAB_HPP
#define NETWORK_SETTINGS_TAB_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <string>

class ConfigManager;

class NetworkSettingsTab
{
public:
    NetworkSettingsTab() = default;
    ~NetworkSettingsTab() = default;

    bool Create(HWND hParent, const RECT& rc, HINSTANCE hInst, HFONT hFont, ConfigManager& cfg);
    void Show(bool show);
    void Move(const RECT& rc);

    void ApplyToConfig();
    void ReloadFromConfig();

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    LRESULT HandleMessage(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);

    void OnCreate();
    void LayoutControls(const RECT& rc);
    void LayoutCurrentPage();
    void ShowSelectedSubPage();
    void UpdateControlEnableState();

    int GetCurrentSubTabIndex() const noexcept;
    int ScaleByDpi(int value) const;
    int GetTextLineHeight() const;

    std::wstring GetText(HWND hWnd) const;
    void SetText(HWND hWnd, const std::wstring& text);

    bool BrowseFile(HWND hTargetEdit, const wchar_t* title, const wchar_t* filter);

private:
    HINSTANCE hInst_ = nullptr;
    HFONT hFont_ = nullptr;
    ConfigManager* cfg_ = nullptr;

    HWND hWnd_ = nullptr;
    HWND hSubTab_ = nullptr;

    HWND hPageBasic_ = nullptr;
    HWND hPageHttps_ = nullptr;
    HWND hPageAuth_ = nullptr;
    HWND hPageAdvanced_ = nullptr;

    // 基本ページ
    HWND hChkEnableServer_ = nullptr;
    HWND hStaticBindAddress_ = nullptr;
    HWND hEditBindAddress_ = nullptr;
    HWND hStaticHttpPort_ = nullptr;
    HWND hEditHttpPort_ = nullptr;
    HWND hChkAllowRemote_ = nullptr;
    HWND hChkAutoStart_ = nullptr;
    HWND hStaticBasicNote_ = nullptr;

    // HTTPSページ
    HWND hChkEnableHttps_ = nullptr;
    HWND hStaticHttpsPort_ = nullptr;
    HWND hEditHttpsPort_ = nullptr;
    HWND hStaticPfxPath_ = nullptr;
    HWND hEditPfxPath_ = nullptr;
    HWND hBtnBrowsePfx_ = nullptr;
    HWND hStaticPfxPassword_ = nullptr;
    HWND hEditPfxPassword_ = nullptr;
    HWND hStaticHttpsNote_ = nullptr;

    // 認証ページ
    HWND hChkEnablePasswordAuth_ = nullptr;
    HWND hStaticLoginUser_ = nullptr;
    HWND hEditLoginUser_ = nullptr;
    HWND hStaticLoginPassword_ = nullptr;
    HWND hEditLoginPassword_ = nullptr;
    HWND hStaticAuthNote_ = nullptr;

    // 補助ページ
    HWND hStaticRequestTimeout_ = nullptr;
    HWND hEditRequestTimeout_ = nullptr;
    HWND hChkEnableCors_ = nullptr;
    HWND hChkOpenBrowser_ = nullptr;
    HWND hStaticAdvancedNote_ = nullptr;
};

#endif // NETWORK_SETTINGS_TAB_HPP
