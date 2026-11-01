#pragma once
#ifndef GUI_DEBUG_LOG_PIPE_HPP
#define GUI_DEBUG_LOG_PIPE_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <string>
#include <string_view>
#include <functional>
#include <cstdint>

#include "GuiActions.hpp"

// 「デバッグログ表示＋ON/OFF＋デバッグ実行」タブ
class GuiDebugLogPipeTab
{
public:
    GuiDebugLogPipeTab() = default;
    GuiDebugLogPipeTab(const GuiDebugLogPipeTab&) = delete;
    GuiDebugLogPipeTab& operator=(const GuiDebugLogPipeTab&) = delete;

    bool Create(
        HWND hParent,
        RECT rc,
        HINSTANCE hInst,
        HFONT hFont,
        std::function<GuiActions& ()> getActions,
        std::function<void(std::wstring_view)> onLog
    );

    void Show(bool show);
    void Move(RECT rc);
    void ApplyToConfig();
    void ReloadFromConfig();
    HWND GetHwnd() const noexcept { return hPage_; }

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    void OnCreate();
    void OnSize(int w, int h);
    void OnCommand(int id);
    void OnTimer();

    void AppendText(std::wstring_view s);
    void ClearText();

    void LoadFromIni_();
    void SaveToIni_() const;
    void ApplyIniToControls_();

    void BrowseLogFile_();
    void ApplyLogFilePath_(const std::wstring& filePath);
    bool SelectLogFileByDialog_(std::wstring& outPath) const;

    static std::wstring GetDefaultConfigIniPath_();
    static bool ReadNewTextFromFile_(
        const std::wstring& filePath,
        std::uint64_t& cursor,
        std::wstring& outText
    );

private:
    HWND hParent_ = nullptr;
    HWND hPage_ = nullptr;

    HWND hChkEnable_ = nullptr;
    HWND hEdFile_ = nullptr;
    HWND hBtnApply_ = nullptr;
    HWND hBtnBrowse_ = nullptr;
    HWND hBtnClear_ = nullptr;
    HWND hLogEdit_ = nullptr;
    HWND hBtnRunChScanDbg_ = nullptr;

    HINSTANCE hInst_ = nullptr;
    HFONT hFont_ = nullptr;

    std::function<void(std::wstring_view)> log_;
    std::function<GuiActions& ()> getActions_;

    bool enabled_ = true;
    std::wstring logFilePath_ = L"debug.log";
    std::uint64_t fileCursor_ = 0;

    static constexpr UINT_PTR kTimerId = 1;

    static constexpr int IDC_CHK_ENABLE = 1001;
    static constexpr int IDC_ED_FILE = 1002;
    static constexpr int IDC_BTN_APPLY = 1003;
    static constexpr int IDC_BTN_CLEAR = 1004;
    static constexpr int IDC_ED_LOG = 1005;
    static constexpr int IDC_BTN_BROWSE = 1006;
    static constexpr int IDC_BTN_RUN_CHSCAN_DBG = 1101;
};

#endif // GUI_DEBUG_LOG_PIPE_HPP
