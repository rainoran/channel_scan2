#pragma once
#pragma once
#ifndef PLUGIN_TAB_HPP
#define PLUGIN_TAB_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <CommCtrl.h>

#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <cstdint>

class PluginTab
{
public:
    PluginTab() = default;

    bool Create(
        HWND hParent,
        const RECT& rc,
        HINSTANCE hInst,
        HFONT hFont,
        std::function<void(std::wstring_view)> logCallback);

    void Show(bool show);
    void Move(const RECT& rc);

    void ApplyToConfig();
    void ReloadFromConfig();

private:
    struct PluginItem
    {
        std::wstring fileName;
        std::wstring fullPath;
        std::wstring type;
        std::uint64_t fileSize = 0;
        FILETIME lastWriteTime{};
        bool enabled = true;
    };

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);

    void OnCreate();
    void OnSize(int width, int height);

    void RescanPlugins_();
    void OpenPluginsFolder_();

    void LoadPluginsFromFolder_();
    void RefreshList_();
    void SyncCheckStatesFromList_();

    void UpdateSummaryText_();
    void UpdateDetailText_(int index);
    void SaveEnabledStateToIni_();

    std::wstring GetIniPath_() const;
    std::wstring GetExeDir_() const;
    std::wstring GetPluginsDir_() const;
    void EnsurePluginsFolderExists_() const;

    int FindSelectedIndex_() const;
    void SelectRow_(int index);

private:
    static constexpr int IDC_PLUGIN_LIST = 5101;
    static constexpr int IDC_PLUGIN_RESCAN = 5102;
    static constexpr int IDC_PLUGIN_OPEN_FOLDER = 5103;
    static constexpr int IDC_PLUGIN_SUMMARY = 5104;
    static constexpr int IDC_PLUGIN_DETAIL = 5105;

    HWND hWnd_ = nullptr;
    HWND hParent_ = nullptr;
    HWND hList_ = nullptr;
    HWND hBtnRescan_ = nullptr;
    HWND hBtnOpenFolder_ = nullptr;
    HWND hSummary_ = nullptr;
    HWND hDetail_ = nullptr;

    HINSTANCE hInst_ = nullptr;
    HFONT hFont_ = nullptr;

    std::function<void(std::wstring_view)> log_;

    std::vector<PluginItem> plugins_;
};

#endif // PLUGIN_TAB_HPP