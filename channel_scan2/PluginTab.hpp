#pragma once
#ifndef PLUGIN_TAB_HPP
#define PLUGIN_TAB_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <CommCtrl.h>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// SDKヘッダーはDLL開発専用。ホストはDLLを動的に解決する。
class PluginTab
{
public:
    PluginTab() = default;
    ~PluginTab();
    PluginTab(const PluginTab&) = delete;
    PluginTab& operator=(const PluginTab&) = delete;

    bool Create(HWND parent, const RECT& rect, HINSTANCE instance, HFONT font,
        std::function<void(std::wstring_view)> logCallback);
    void Show(bool show);
    void Move(const RECT& rect);
    void ApplyToConfig();
    void ReloadFromConfig();

    // トレイメニュー用: 実行中のプラグイン操作をサブメニューとして登録。
    // メニュー生成とコマンド実行はGUIスレッドから行う。
    // SDKヘッダーへの依存は追加しない。
    static constexpr UINT TRAY_PLUGIN_COMMAND_FIRST = 48000u;
    static constexpr UINT TRAY_PLUGIN_COMMAND_LIMIT = 52096u; // exclusive
    bool AppendTrayPluginMenu(HMENU parentMenu);
    bool ExecuteTrayPluginCommand(UINT commandId);


private:
    struct Action { std::wstring id; std::wstring label; };
    struct PluginItem
    {
        std::wstring fileName;
        std::wstring fullPath;
        std::wstring displayName;
        std::wstring description;
        std::wstring version;
        std::wstring status;
        std::uint64_t fileSize = 0;
        FILETIME lastWriteTime{};
        bool enabled = false;
        bool running = false;
        HMODULE module = nullptr;
        void* handle = nullptr;
        // 実際の関数ポインターはcpp内で解決し、型をヘッダーに露出させない。
        FARPROC shutdownProc = nullptr;
        FARPROC destroyProc = nullptr;
        FARPROC summaryProc = nullptr;
        FARPROC commandProc = nullptr;
        FARPROC canUnloadProc = nullptr;
        std::vector<Action> actions;
    };
    struct ActionButton { HWND hwnd = nullptr; std::uint32_t actionIndex = 0; };
    struct TrayCommand {
        UINT commandId = 0;
        std::wstring pluginFileName;
        std::wstring actionId;
    };


    static constexpr int IDC_PLUGIN_LIST = 5101;
    static constexpr int IDC_PLUGIN_RESCAN = 5102;
    static constexpr int IDC_PLUGIN_OPEN_FOLDER = 5103;
    static constexpr int IDC_PLUGIN_SUMMARY = 5104;
    static constexpr int IDC_PLUGIN_DETAIL = 5105;
    static constexpr int IDC_PLUGIN_ACTION_BASE = 5200;
    static constexpr int MAX_PLUGIN_ACTIONS = 8; // 下部に常時表示するボタンの最大数
    static constexpr int IDC_PLUGIN_CONTEXT_ACTION_BASE = 6000;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wp, LPARAM lp);
    void OnCreate();
    void OnSize(int width, int height);
    void RescanPlugins_();
    void OpenPluginsFolder_();
    void RefreshList_();
    void UpdateRow_(int index);
    void UpdateSummary_();
    void UpdateDetail_(int index);
    void RebuildActionButtons_();
    void ClearActionButtons_();
    void ExecuteAction_(int index);
    void ExecutePluginCommand_(std::uint32_t actionIndex);
    void ShowPluginContextMenu_(LPARAM contextPosition);
    void StartEnabledPlugins_();
    bool StartPlugin_(PluginItem& plugin);
    void StopPlugin_(PluginItem& plugin);
    void StopAllPlugins_();
    void SaveEnabledState_();
    void Log_(std::wstring_view message) const;
    int SelectedIndex_() const;
    std::wstring IniPath_() const;
    std::wstring ExeDir_() const;
    std::wstring PluginsDir_() const;

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    HWND list_ = nullptr;
    HWND summary_ = nullptr;
    HWND detail_ = nullptr;
    HWND rescan_ = nullptr;
    HWND openFolder_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HFONT font_ = nullptr;
    bool refreshing_ = false;
    bool stopping_ = false;
    std::function<void(std::wstring_view)> log_;
    std::vector<PluginItem> plugins_;
    std::vector<ActionButton> actionButtons_;
    std::vector<TrayCommand> trayCommands_;
    std::vector<HMODULE> retainedModules_;
};

#endif // PLUGIN_TAB_HPP
