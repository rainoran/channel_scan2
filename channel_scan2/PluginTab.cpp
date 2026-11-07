#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "PluginTab.hpp"
#include "ConfigManager.h"
#include <Shellapi.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shell32.lib")

namespace
{
    constexpr wchar_t kWindowClass[] = L"TSCEPG_PluginTab";
    constexpr std::uint32_t kAbiVersion = 0x00010000u;
    constexpr std::uint32_t kMaxActions = 64; // 右クリックから使える機能は最大64件
    constexpr std::uint32_t kMaxSummary = 256;

    // DLL側のTSCEPGPluginSDK.hpp（ABI 1.0）とのバイナリ互換宣言。
    // この定義はPluginTab.cppだけで使い、DLLとのリンクは一切しない。
    struct AbiPluginInfo final
    {
        std::uint32_t structSize = sizeof(AbiPluginInfo);
        std::uint32_t abiVersion = kAbiVersion;
        std::uint32_t pluginKind = 0;
        std::uint32_t flags = 0;
        wchar_t pluginId[64]{};
        wchar_t displayName[64]{};
        wchar_t author[64]{};
        wchar_t version[32]{};
        wchar_t description[256]{};
    };
    struct AbiHostApi final
    {
        std::uint32_t structSize = sizeof(AbiHostApi);
        std::uint32_t abiVersion = kAbiVersion;
        void(__cdecl* Log)(std::uint32_t, const wchar_t*) = nullptr;
        bool(__cdecl* GetPath)(std::uint32_t, wchar_t*, std::uint32_t) = nullptr;
        bool(__cdecl* ReadIniString)(const wchar_t*, const wchar_t*, const wchar_t*, wchar_t*, std::uint32_t) = nullptr;
        bool(__cdecl* WriteIniString)(const wchar_t*, const wchar_t*, const wchar_t*) = nullptr;
        void* reserved0 = nullptr;
        void* reserved1 = nullptr;
    };
    // 操作列挙は従来SDKにない任意エクスポート。なければボタンは表示しない。
    struct AbiActionInfo final
    {
        std::uint32_t structSize = sizeof(AbiActionInfo);
        std::uint32_t flags = 0;
        wchar_t actionId[64]{};
        wchar_t displayName[96]{};
    };
    using FnGetInfo = bool(__cdecl*)(AbiPluginInfo*) noexcept;
    using FnCreate = bool(__cdecl*)(const AbiHostApi*, void**) noexcept;
    using FnDestroy = void(__cdecl*)(void*) noexcept;
    using FnInitialize = bool(__cdecl*)(void*) noexcept;
    using FnShutdown = void(__cdecl*)(void*) noexcept;
    using FnSummary = bool(__cdecl*)(void*, wchar_t*, std::uint32_t) noexcept;
    using FnCommand = bool(__cdecl*)(void*, const wchar_t*, const void*, std::uint32_t) noexcept;
    using FnActionCount = std::uint32_t(__cdecl*)(void*) noexcept;
    using FnActionInfo = bool(__cdecl*)(void*, std::uint32_t, AbiActionInfo*) noexcept;
    using FnCanUnload = bool(__cdecl*)() noexcept;

    FARPROC FindExport(HMODULE module, const char* name) noexcept
    {
        FARPROC proc = ::GetProcAddress(module, name);
#if defined(_M_IX86)
        // x86 __cdecl exports can have a leading underscore if no .def aliases are used.
        if (!proc) {
            std::string decorated = "_";
            decorated += name;
            proc = ::GetProcAddress(module, decorated.c_str());
        }
#endif
        return proc;
    }
    template<typename T> T Proc(HMODULE module, const char* name) noexcept
    {
        return reinterpret_cast<T>(FindExport(module, name));
    }
    HMENU ControlId(int id) noexcept { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); }
    std::wstring Lower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return value;
    }
    std::wstring ExeDirectory()
    {
        std::vector<wchar_t> buffer(512);
        while (buffer.size() <= 32768) {
            const DWORD n = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (!n) return {};
            if (n < buffer.size()) return std::filesystem::path(std::wstring(buffer.data(), n)).parent_path().wstring();
            buffer.resize(buffer.size() * 2);
        }
        return {};
    }
    void CopyWide(wchar_t* dest, std::uint32_t cap, const std::wstring& text) noexcept
    {
        if (!dest || !cap) return;
        const auto n = (std::min)(text.size(), static_cast<std::size_t>(cap - 1));
        if (n) std::wmemcpy(dest, text.c_str(), n);
        dest[n] = L'\0';
    }
    void __cdecl HostLog(std::uint32_t level, const wchar_t* message)
    {
        const std::wstring line = L"[Plugin:" + std::to_wstring(level) + L"] " +
            (message ? message : L"") + L"\n";
        ::OutputDebugStringW(line.c_str());
    }
    bool __cdecl HostGetPath(std::uint32_t kind, wchar_t* out, std::uint32_t cap)
    {
        if (!out || !cap) return false;
        out[0] = L'\0';
        std::wstring path;
        if (kind == 0) path = ExeDirectory();
        else if (kind == 1) path = ConfigManager::GetInstance().GetIniPath();
        else if (kind == 2) path = (std::filesystem::path(ExeDirectory()) / L"plugins").wstring();
        else return false;
        if (path.empty() || path.size() >= cap) return false;
        CopyWide(out, cap, path);
        return true;
    }
    bool __cdecl HostReadIni(const wchar_t* section, const wchar_t* key,
        const wchar_t* fallback, wchar_t* out, std::uint32_t cap)
    {
        if (!section || !key || !out || !cap) return false;
        const auto path = ConfigManager::GetInstance().GetIniPath();
        ::GetPrivateProfileStringW(section, key, fallback ? fallback : L"", out, cap, path.c_str());
        return true;
    }
    bool __cdecl HostWriteIni(const wchar_t* section, const wchar_t* key, const wchar_t* value)
    {
        if (!section || !key) return false;
        const auto path = ConfigManager::GetInstance().GetIniPath();
        return ::WritePrivateProfileStringW(section, key, value, path.c_str()) != FALSE;
    }
    const AbiHostApi& HostApi()
    {
        static const AbiHostApi api = [] {
            AbiHostApi value;
            value.Log = &HostLog;
            value.GetPath = &HostGetPath;
            value.ReadIniString = &HostReadIni;
            value.WriteIniString = &HostWriteIni;
            return value;
            }();
        return api;
    }
    void SetFont(HWND hwnd, HFONT font)
    {
        if (hwnd && font) ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    void Cell(HWND list, int row, int col, const std::wstring& text)
    {
        LVITEMW item{};
        item.iSubItem = col;
        item.pszText = const_cast<LPWSTR>(text.c_str());
        ::SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item));
    }
    std::wstring FileSizeText(std::uint64_t size)
    {
        if (size >= 1024 * 1024) return std::to_wstring(size / (1024 * 1024)) + L" MB";
        if (size >= 1024) return std::to_wstring(size / 1024) + L" KB";
        return std::to_wstring(size) + L" B";
    }
    std::wstring FileTimeText(FILETIME ft)
    {
        FILETIME local{};
        SYSTEMTIME st{};
        if (!::FileTimeToLocalFileTime(&ft, &local) || !::FileTimeToSystemTime(&local, &st)) return L"-";
        wchar_t output[32]{};
        swprintf_s(output, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        return output;
    }
}

// トレイアイコンの「プラグイン」メニューを動的に構築する。
// チェックボックスによるDLL有効/無効は本タブ側だけで管理する。
bool PluginTab::AppendTrayPluginMenu(HMENU parentMenu)
{
    trayCommands_.clear();
    if (!parentMenu || stopping_) return false;

    HMENU pluginsMenu = ::CreatePopupMenu();
    if (!pluginsMenu) return false;

    UINT nextId = TRAY_PLUGIN_COMMAND_FIRST;
    std::size_t addedPlugins = 0;

    for (const auto& plugin : plugins_) {
        if (nextId >= TRAY_PLUGIN_COMMAND_LIMIT) break;
        if (!plugin.enabled || !plugin.running || !plugin.commandProc || plugin.actions.empty()) {
            continue;
        }

        HMENU actionMenu = ::CreatePopupMenu();
        if (!actionMenu) continue;

        std::vector<TrayCommand> pending;
        pending.reserve((std::min)(plugin.actions.size(), static_cast<std::size_t>(64)));

        const std::size_t count = (std::min)(plugin.actions.size(), static_cast<std::size_t>(64));
        for (std::size_t index = 0; index < count && nextId < TRAY_PLUGIN_COMMAND_LIMIT; ++index) {
            const auto& action = plugin.actions[index];
            if (action.id.empty()) continue;

            const std::wstring label = action.label.empty() ? action.id : action.label;
            if (!::AppendMenuW(actionMenu, MF_STRING, nextId, label.c_str())) continue;
            pending.push_back({ nextId, plugin.fileName, action.id });
            ++nextId;
        }

        if (pending.empty()) {
            ::DestroyMenu(actionMenu);
            continue;
        }

        const std::wstring name = plugin.displayName.empty() ? plugin.fileName : plugin.displayName;
        if (!::AppendMenuW(pluginsMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(actionMenu), name.c_str())) {
            ::DestroyMenu(actionMenu);
            continue;
        }
        // AppendMenu成功後、actionMenuの所有権はpluginsMenuに移る。
        trayCommands_.insert(trayCommands_.end(), pending.begin(), pending.end());
        ++addedPlugins;
    }

    if (addedPlugins == 0) {
        ::DestroyMenu(pluginsMenu);
        return false;
    }

    if (!::AppendMenuW(parentMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(pluginsMenu), L"プラグイン")) {
        ::DestroyMenu(pluginsMenu);
        trayCommands_.clear();
        return false;
    }
    return true;
}

bool PluginTab::ExecuteTrayPluginCommand(UINT commandId)
{
    if (stopping_ || commandId < TRAY_PLUGIN_COMMAND_FIRST ||
        commandId >= TRAY_PLUGIN_COMMAND_LIMIT) return false;

    // スナップショットのコマンドIDから名前で再検索する。
    // DLLの再スキャン・ON/OFF後でも古いindexを参照しない。
    const auto command = std::find_if(trayCommands_.begin(), trayCommands_.end(),
        [commandId](const TrayCommand& item) { return item.commandId == commandId; });
    if (command == trayCommands_.end()) return false;

    auto plugin = std::find_if(plugins_.begin(), plugins_.end(),
        [&command](const PluginItem& item) { return item.fileName == command->pluginFileName; });
    if (plugin == plugins_.end() || !plugin->enabled || !plugin->running ||
        !plugin->commandProc || !plugin->handle) return false;

    const auto action = std::find_if(plugin->actions.begin(), plugin->actions.end(),
        [&command](const Action& item) { return item.id == command->actionId; });
    if (action == plugin->actions.end()) return false;

    const auto execute = reinterpret_cast<FnCommand>(plugin->commandProc);
    // コマンド実行中にDLLがコマンド一覧を変更し得るので値をコピーしておく。
    const std::wstring actionId = action->id;
    const std::wstring fileName = plugin->fileName;
    void* const pluginHandle = plugin->handle;
    const bool ok = execute(pluginHandle, actionId.c_str(), nullptr, 0);
    Log_(L"[PluginTab][Tray] command=" + actionId + (ok ? L" OK" : L" FAILED"));
    if (const int selected = SelectedIndex_(); selected >= 0 &&
        selected < static_cast<int>(plugins_.size()) &&
        plugins_[selected].fileName == fileName && plugins_[selected].running) {
        UpdateDetail_(selected);
    }
    return ok;
}

PluginTab::~PluginTab()
{
    StopAllPlugins_();
}

bool PluginTab::Create(HWND parent, const RECT& rect, HINSTANCE instance, HFONT font,
    std::function<void(std::wstring_view)> logCallback)
{
    parent_ = parent;
    instance_ = instance;
    font_ = font;
    log_ = std::move(logCallback);
    WNDCLASSEXW klass{};
    klass.cbSize = sizeof(klass);
    klass.lpfnWndProc = &PluginTab::WndProc;
    klass.hInstance = instance_;
    klass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    klass.lpszClassName = kWindowClass;
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (!::GetClassInfoExW(instance_, kWindowClass, &existing) && !::RegisterClassExW(&klass)) return false;
    hwnd_ = ::CreateWindowExW(WS_EX_CONTROLPARENT, kWindowClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        parent_, nullptr, instance_, this);
    return hwnd_ != nullptr;
}
void PluginTab::Show(bool show) { if (hwnd_) ::ShowWindow(hwnd_, show ? SW_SHOW : SW_HIDE); }
void PluginTab::Move(const RECT& rect)
{
    if (hwnd_) ::MoveWindow(hwnd_, rect.left, rect.top,
        rect.right - rect.left, rect.bottom - rect.top, TRUE);
}
void PluginTab::ApplyToConfig() { SaveEnabledState_(); }
void PluginTab::ReloadFromConfig() { RescanPlugins_(); }

LRESULT CALLBACK PluginTab::WndProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    PluginTab* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        if (!cs || !cs->lpCreateParams) return FALSE;
        self = static_cast<PluginTab*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else {
        self = reinterpret_cast<PluginTab*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        try {
            LRESULT result = self->HandleMessage(hwnd, message, wp, lp);
            if (message == WM_NCDESTROY) {
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                self->hwnd_ = nullptr;
            }
            return result;
        }
        catch (const std::exception&) {
            ::OutputDebugStringW(L"[PluginTab] C++ exception in window callback\n");
            return message == WM_CREATE ? -1 : 0;
        }
        catch (...) {
            ::OutputDebugStringW(L"[PluginTab] unknown exception in window callback\n");
            return message == WM_CREATE ? -1 : 0;
        }
    }
    return ::DefWindowProcW(hwnd, message, wp, lp);
}
LRESULT PluginTab::HandleMessage(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    switch (message) {
    case WM_CREATE: OnCreate(); return 0;
    case WM_SIZE: OnSize(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_DESTROY:
        ClearActionButtons_();
        StopAllPlugins_();
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED) {
            const int id = LOWORD(wp);
            if (id == IDC_PLUGIN_RESCAN) { RescanPlugins_(); return 0; }
            if (id == IDC_PLUGIN_OPEN_FOLDER) { OpenPluginsFolder_(); return 0; }
            if (id >= IDC_PLUGIN_ACTION_BASE && id < IDC_PLUGIN_ACTION_BASE + MAX_PLUGIN_ACTIONS) {
                ExecuteAction_(id - IDC_PLUGIN_ACTION_BASE); return 0;
            }
        }
        break;
    case WM_CONTEXTMENU:
        // DLL有効／無効はチェックボックスの操作だけで行う。
        // コンテキストメニューには「プラグインの機能」だけを並べる。
        if (reinterpret_cast<HWND>(wp) == list_ && !refreshing_ && !stopping_) {
            ShowPluginContextMenu_(lp);
            return 0;
        }
        break;
    case WM_NOTIFY:
        if (lp && list_ && !refreshing_ && !stopping_) {
            const auto* nm = reinterpret_cast<const NMHDR*>(lp);
            if (nm->hwndFrom != list_) break;
            if (nm->code == LVN_ITEMCHANGED) {
                const auto* lv = reinterpret_cast<const NMLISTVIEW*>(lp);
                if (lv->iItem < 0 || lv->iItem >= static_cast<int>(plugins_.size()) ||
                    !(lv->uChanged & LVIF_STATE)) return 0;
                const UINT changes = lv->uOldState ^ lv->uNewState;
                if (changes & LVIS_STATEIMAGEMASK) {
                    const int idx = lv->iItem;
                    // state-image 0は未初期化。変更済みチェックボックスのみに反応。
                    const UINT stateImage = (lv->uNewState & LVIS_STATEIMAGEMASK) >> 12;
                    if (stateImage != 1 && stateImage != 2) return 0;
                    auto& plugin = plugins_[idx];
                    const bool checked = stateImage == 2;
                    if (plugin.enabled != checked) {
                        plugin.enabled = checked;
                        if (checked) StartPlugin_(plugin); else StopPlugin_(plugin);
                        UpdateRow_(idx);
                        UpdateSummary_();
                        UpdateDetail_(SelectedIndex_());
                        RebuildActionButtons_();
                        SaveEnabledState_();
                    }
                }
                if (changes & LVIS_SELECTED) {
                    UpdateDetail_(SelectedIndex_());
                    RebuildActionButtons_();
                }
                return 0;
            }
            if (nm->code == NM_CLICK || nm->code == NM_DBLCLK) {
                UpdateDetail_(SelectedIndex_());
                RebuildActionButtons_();
                return 0;
            }
        }
        break;
    default: break;
    }
    return ::DefWindowProcW(hwnd, message, wp, lp);
}

void PluginTab::OnCreate()
{
    summary_ = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 100, 24, hwnd_, ControlId(IDC_PLUGIN_SUMMARY), instance_, nullptr);
    rescan_ = ::CreateWindowExW(0, L"BUTTON", L"再読み込み", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 100, 28, hwnd_, ControlId(IDC_PLUGIN_RESCAN), instance_, nullptr);
    openFolder_ = ::CreateWindowExW(0, L"BUTTON", L"フォルダを開く", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 120, 28, hwnd_, ControlId(IDC_PLUGIN_OPEN_FOLDER), instance_, nullptr);
    list_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
        0, 0, 100, 100, hwnd_, ControlId(IDC_PLUGIN_LIST), instance_, nullptr);
    detail_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        0, 0, 100, 100, hwnd_, ControlId(IDC_PLUGIN_DETAIL), instance_, nullptr);
    for (HWND control : { summary_, rescan_, openFolder_, list_, detail_ }) SetFont(control, font_);
    if (list_) {
        ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
            LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        constexpr const wchar_t* labels[] = { L"状態", L"プラグイン名", L"実行状態", L"バージョン", L"サイズ", L"更新日時", L"パス" };
        constexpr int widths[] = { 70, 220, 160, 90, 90, 145, 450 };
        for (int i = 0; i < 7; ++i) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.iSubItem = i;
            col.cx = widths[i];
            col.pszText = const_cast<LPWSTR>(labels[i]);
            ListView_InsertColumn(list_, i, &col);
        }
    }
    RescanPlugins_();
}
void PluginTab::OnSize(int width, int height)
{
    if (!list_) return;
    constexpr int pad = 10, gap = 8, topH = 28;
    const int usable = (std::max)(0, width - 2 * pad);
    if (openFolder_) ::MoveWindow(openFolder_, (std::max)(pad, width - pad - 120), pad, 120, 28, TRUE);
    if (rescan_) ::MoveWindow(rescan_, (std::max)(pad, width - pad - 228), pad, 100, 28, TRUE);
    if (summary_) ::MoveWindow(summary_, pad, pad + 3, (std::max)(0, width - 260), 28, TRUE);
    const int listY = pad + topH + gap;
    const int buttonRows = (static_cast<int>(actionButtons_.size()) + 3) / 4;
    const int actionSpace = buttonRows ? buttonRows * 34 + gap : 0;
    const int detailH = (std::max)(90, height / 4);
    const int listH = (std::max)(80, height - listY - detailH - actionSpace - 2 * gap - pad);
    ::MoveWindow(list_, pad, listY, usable, listH, TRUE);
    if (detail_) ::MoveWindow(detail_, pad, listY + listH + gap, usable, detailH, TRUE);
    const int actionTop = listY + listH + gap + detailH + gap;
    const int cols = (std::max)(1, (std::min)(4, usable / 156));
    for (std::size_t i = 0; i < actionButtons_.size(); ++i) {
        auto& button = actionButtons_[i];
        if (button.hwnd) ::MoveWindow(button.hwnd,
            pad + static_cast<int>(i % cols) * 156,
            actionTop + static_cast<int>(i / cols) * 34, 150, 28, TRUE);
    }
}
std::wstring PluginTab::IniPath_() const { return ConfigManager::GetInstance().GetIniPath(); }
std::wstring PluginTab::ExeDir_() const { return ExeDirectory(); }
std::wstring PluginTab::PluginsDir_() const { return (std::filesystem::path(ExeDir_()) / L"plugins").wstring(); }
void PluginTab::Log_(std::wstring_view message) const { if (log_) log_(message); }
int PluginTab::SelectedIndex_() const
{
    return list_ ? ListView_GetNextItem(list_, -1, LVNI_SELECTED) : -1;
}

bool PluginTab::StartPlugin_(PluginItem& plugin)
{
    if (plugin.running) return true;
    if (plugin.module) StopPlugin_(plugin);
    plugin.status = L"読み込み中";
    const HMODULE mod = ::LoadLibraryExW(plugin.fullPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!mod) {
        plugin.status = L"ロード失敗 (" + std::to_wstring(::GetLastError()) + L")";
        Log_(L"[PluginTab] LoadLibrary失敗: " + plugin.fileName);
        return false;
    }
    const auto getInfo = Proc<FnGetInfo>(mod, "TSCEPG_GetPluginInfo");
    const auto create = Proc<FnCreate>(mod, "TSCEPG_CreatePlugin");
    const auto destroy = Proc<FnDestroy>(mod, "TSCEPG_DestroyPlugin");
    const auto init = Proc<FnInitialize>(mod, "TSCEPG_Initialize");
    const auto shutdown = Proc<FnShutdown>(mod, "TSCEPG_Shutdown");
    if (!getInfo || !create || !destroy || !init || !shutdown) {
        plugin.status = L"SDK必須関数不足";
        ::FreeLibrary(mod);
        return false;
    }
    AbiPluginInfo info{};
    if (!getInfo(&info) || info.structSize != sizeof(info) || info.abiVersion != kAbiVersion) {
        plugin.status = L"SDK ABI不一致";
        ::FreeLibrary(mod);
        return false;
    }
    info.displayName[63] = 0;
    info.description[255] = 0;
    info.version[31] = 0;
    void* handle = nullptr;
    if (!create(&HostApi(), &handle) || !handle) {
        plugin.status = L"生成失敗";
        if (handle) destroy(handle);
        // 生成失敗のDLLは何も開始していない保証がないため解放を保留。
        retainedModules_.push_back(mod);
        return false;
    }
    plugin.module = mod;
    plugin.handle = handle;
    plugin.shutdownProc = reinterpret_cast<FARPROC>(shutdown);
    plugin.destroyProc = reinterpret_cast<FARPROC>(destroy);
    plugin.summaryProc = FindExport(mod, "TSCEPG_GetPanelSummary");
    plugin.commandProc = FindExport(mod, "TSCEPG_ExecuteCommand");
    plugin.canUnloadProc = FindExport(mod, "TSCEPG_CanUnload");
    if (!init(handle)) {
        plugin.status = L"初期化失敗";
        // 可能ならライフサイクルを完了し、強制FreeLibraryはしない。
        StopPlugin_(plugin);
        plugin.status = L"初期化失敗（停止済）";
        return false;
    }
    plugin.running = true;
    plugin.status = L"実行中";
    if (info.displayName[0]) plugin.displayName = info.displayName;
    plugin.description = info.description;
    plugin.version = info.version;

    const auto actionCount = Proc<FnActionCount>(mod, "TSCEPG_GetActionCount");
    const auto actionInfo = Proc<FnActionInfo>(mod, "TSCEPG_GetActionInfo");
    if (actionCount && actionInfo && plugin.commandProc) {
        const auto count = (std::min)(actionCount(handle), kMaxActions);
        for (std::uint32_t i = 0; i < count; ++i) {
            AbiActionInfo action{};
            if (!actionInfo(handle, i, &action)) continue;
            action.actionId[63] = 0;
            action.displayName[95] = 0;
            if (!action.actionId[0]) continue;
            plugin.actions.push_back({ action.actionId,
                action.displayName[0] ? action.displayName : action.actionId });
        }
    }
    Log_(L"[PluginTab] 起動成功: " + plugin.fileName);
    return true;
}
void PluginTab::StopPlugin_(PluginItem& plugin)
{
    if (!plugin.module) {
        plugin.running = false;
        plugin.actions.clear();
        return;
    }
    const HMODULE module = plugin.module;
    const auto shutdown = reinterpret_cast<FnShutdown>(plugin.shutdownProc);
    const auto destroy = reinterpret_cast<FnDestroy>(plugin.destroyProc);
    const auto canUnload = reinterpret_cast<FnCanUnload>(plugin.canUnloadProc);
    const void* oldHandle = plugin.handle;
    plugin.running = false;
    plugin.actions.clear();
    plugin.handle = nullptr;
    plugin.module = nullptr;
    plugin.shutdownProc = nullptr;
    plugin.destroyProc = nullptr;
    plugin.summaryProc = nullptr;
    plugin.commandProc = nullptr;
    plugin.canUnloadProc = nullptr;
    if (oldHandle) {
        if (shutdown) shutdown(const_cast<void*>(oldHandle));
        if (destroy) destroy(const_cast<void*>(oldHandle));
    }
    if (canUnload && canUnload()) {
        ::FreeLibrary(module);
        plugin.status = L"停止";
    }
    else {
        retainedModules_.push_back(module);
        plugin.status = L"停止（DLL保持）";
    }
}
void PluginTab::StopAllPlugins_()
{
    if (stopping_) return;
    stopping_ = true;
    for (auto& p : plugins_) if (p.module) StopPlugin_(p);
    stopping_ = false;
}
void PluginTab::SaveEnabledState_()
{
    const auto ini = IniPath_();
    ::WritePrivateProfileStringW(L"Plugins", nullptr, nullptr, ini.c_str());
    ::WritePrivateProfileStringW(L"Plugins", L"Root", PluginsDir_().c_str(), ini.c_str());
    const auto count = std::to_wstring(plugins_.size());
    ::WritePrivateProfileStringW(L"Plugins", L"Count", count.c_str(), ini.c_str());
    for (std::size_t i = 0; i < plugins_.size(); ++i) {
        wchar_t fileKey[64]{}, pathKey[64]{}, enabledKey[64]{};
        swprintf_s(fileKey, L"Plugin%zu_File", i);
        swprintf_s(pathKey, L"Plugin%zu_Path", i);
        swprintf_s(enabledKey, L"Plugin%zu_Enabled", i);
        const auto& plugin = plugins_[i];
        ::WritePrivateProfileStringW(L"Plugins", fileKey, plugin.fileName.c_str(), ini.c_str());
        ::WritePrivateProfileStringW(L"Plugins", pathKey, plugin.fullPath.c_str(), ini.c_str());
        ::WritePrivateProfileStringW(L"Plugins", enabledKey, plugin.enabled ? L"1" : L"0", ini.c_str());
    }
}
void PluginTab::RescanPlugins_()
{
    trayCommands_.clear();
    std::map<std::wstring, bool> states;
    const auto ini = IniPath_();
    const auto count = (std::min)(::GetPrivateProfileIntW(L"Plugins", L"Count", 0, ini.c_str()), 4096u);
    for (UINT i = 0; i < count; ++i) {
        wchar_t fileKey[64]{}, enabledKey[64]{}, fileName[512]{};
        swprintf_s(fileKey, L"Plugin%u_File", i);
        swprintf_s(enabledKey, L"Plugin%u_Enabled", i);
        ::GetPrivateProfileStringW(L"Plugins", fileKey, L"", fileName,
            static_cast<DWORD>(std::size(fileName)), ini.c_str());
        if (fileName[0]) states[Lower(fileName)] =
            ::GetPrivateProfileIntW(L"Plugins", enabledKey, 0, ini.c_str()) != 0;
    }
    // 再スキャン中に状態を読み取り直す必要はない。チェック操作は即座に反映している。
    for (const auto& plugin : plugins_) states[Lower(plugin.fileName)] = plugin.enabled;
    const int selected = SelectedIndex_();
    const std::wstring selection = (selected >= 0 && selected < static_cast<int>(plugins_.size()))
        ? plugins_[selected].fileName : L"";
    ClearActionButtons_();
    StopAllPlugins_();
    plugins_.clear();
    const std::filesystem::path dir(PluginsDir_());
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ec.clear();
    std::filesystem::directory_iterator it(dir, ec);
    const std::filesystem::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || fileEc) continue;
        const auto& path = it->path();
        if (Lower(path.extension().wstring()) != L".dll") continue;
        PluginItem p{};
        p.fileName = path.filename().wstring();
        p.fullPath = path.wstring();
        p.displayName = p.fileName;
        p.status = L"停止";
        const auto found = states.find(Lower(p.fileName));
        p.enabled = found != states.end() && found->second;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (::GetFileAttributesExW(p.fullPath.c_str(), GetFileExInfoStandard, &data)) {
            p.fileSize = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            p.lastWriteTime = data.ftLastWriteTime;
        }
        plugins_.push_back(std::move(p));
    }
    std::sort(plugins_.begin(), plugins_.end(),
        [](const auto& a, const auto& b) { return Lower(a.fileName) < Lower(b.fileName); });
    StartEnabledPlugins_();
    RefreshList_();
    UpdateSummary_();
    int row = 0;
    for (int i = 0; i < static_cast<int>(plugins_.size()); ++i)
        if (!selection.empty() && plugins_[i].fileName == selection) { row = i; break; }
    if (!plugins_.empty() && list_) {
        refreshing_ = true;
        ListView_SetItemState(list_, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        refreshing_ = false;
    }
    UpdateDetail_(SelectedIndex_());
    RebuildActionButtons_();
    Log_(L"[PluginTab] DLLを再スキャンしました");
}
void PluginTab::StartEnabledPlugins_()
{
    for (auto& p : plugins_) if (p.enabled) StartPlugin_(p);
}
void PluginTab::RefreshList_()
{
    if (!list_) return;
    refreshing_ = true;
    ListView_DeleteAllItems(list_);
    for (int i = 0; i < static_cast<int>(plugins_.size()); ++i) {
        const auto& p = plugins_[i];
        const std::wstring state = p.enabled ? L"有効" : L"無効";
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(state.c_str());
        const int row = static_cast<int>(::SendMessageW(list_, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));
        if (row < 0) continue;
        ListView_SetCheckState(list_, row, p.enabled ? TRUE : FALSE);
        Cell(list_, row, 1, p.displayName);
        Cell(list_, row, 2, p.status);
        Cell(list_, row, 3, p.version);
        Cell(list_, row, 4, FileSizeText(p.fileSize));
        Cell(list_, row, 5, FileTimeText(p.lastWriteTime));
        Cell(list_, row, 6, p.fullPath);
    }
    refreshing_ = false;
}
void PluginTab::UpdateRow_(int index)
{
    if (!list_ || index < 0 || index >= static_cast<int>(plugins_.size())) return;
    const bool old = refreshing_;
    refreshing_ = true;
    const auto& p = plugins_[index];
    Cell(list_, index, 0, p.enabled ? L"有効" : L"無効");
    Cell(list_, index, 1, p.displayName);
    Cell(list_, index, 2, p.status);
    Cell(list_, index, 3, p.version);
    refreshing_ = old;
}
void PluginTab::UpdateSummary_()
{
    if (!summary_) return;
    std::size_t enabled = 0, running = 0;
    for (const auto& p : plugins_) { if (p.enabled) ++enabled; if (p.running) ++running; }
    const std::wstring text = L"検出: " + std::to_wstring(plugins_.size()) +
        L" / 有効: " + std::to_wstring(enabled) + L" / 実行中: " + std::to_wstring(running);
    ::SetWindowTextW(summary_, text.c_str());
}
void PluginTab::UpdateDetail_(int index)
{
    if (!detail_) return;
    if (index < 0 || index >= static_cast<int>(plugins_.size())) {
        ::SetWindowTextW(detail_, L"プラグインを選択してください。");
        return;
    }
    const auto& p = plugins_[index];
    std::wstring text = L"名前: " + p.displayName + L"\r\nファイル: " + p.fileName +
        L"\r\nバージョン: " + p.version + L"\r\n状態: " + p.status +
        L"\r\n説明: " + p.description + L"\r\nパス: " + p.fullPath;
    if (p.running && p.summaryProc) {
        std::array<wchar_t, kMaxSummary> output{};
        const auto fn = reinterpret_cast<FnSummary>(p.summaryProc);
        if (fn(p.handle, output.data(), static_cast<std::uint32_t>(output.size()))) {
            output.back() = L'\0';
            text += L"\r\n\r\nSDK状態:\r\n";
            text += output.data();
        }
    }
    ::SetWindowTextW(detail_, text.c_str());
}
void PluginTab::ClearActionButtons_()
{
    for (auto& b : actionButtons_) if (b.hwnd && ::IsWindow(b.hwnd)) ::DestroyWindow(b.hwnd);
    actionButtons_.clear();
}
void PluginTab::RebuildActionButtons_()
{
    ClearActionButtons_();
    const int idx = SelectedIndex_();
    if (idx < 0 || idx >= static_cast<int>(plugins_.size())) return;
    const auto& p = plugins_[idx];
    if (!p.running) return;
    for (std::uint32_t i = 0; i < p.actions.size() && i < kMaxActions; ++i) {
        HWND button = ::CreateWindowExW(0, L"BUTTON", p.actions[i].label.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 150, 28,
            hwnd_, ControlId(IDC_PLUGIN_ACTION_BASE + static_cast<int>(i)), instance_, nullptr);
        if (!button) continue;
        SetFont(button, font_);
        actionButtons_.push_back({ button, i });
    }
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    OnSize(rc.right - rc.left, rc.bottom - rc.top);
}
void PluginTab::ExecutePluginCommand_(std::uint32_t actionIndex)
{
    const int selected = SelectedIndex_();
    if (selected < 0 || selected >= static_cast<int>(plugins_.size())) return;
    auto& plugin = plugins_[selected];
    if (!plugin.enabled || !plugin.running || !plugin.commandProc ||
        actionIndex >= plugin.actions.size()) return;

    // PluginTabはコマンドを中継するだけ。個別機能の状態はDLL側が管理する。
    const std::wstring command = plugin.actions[actionIndex].id;
    const auto execute = reinterpret_cast<FnCommand>(plugin.commandProc);
    const bool success = execute(plugin.handle, command.c_str(), nullptr, 0);
    Log_(L"[PluginTab] command=" + command + (success ? L" OK" : L" FAILED"));
    UpdateDetail_(selected);
}

void PluginTab::ExecuteAction_(int buttonIndex)
{
    if (buttonIndex < 0 ||
        buttonIndex >= static_cast<int>(actionButtons_.size())) return;
    ExecutePluginCommand_(actionButtons_[buttonIndex].actionIndex);
}

void PluginTab::ShowPluginContextMenu_(LPARAM contextPosition)
{
    if (!list_ || !::IsWindow(list_) || stopping_) return;

    POINT screen{};
    const bool fromKeyboard = (contextPosition == static_cast<LPARAM>(-1));
    if (fromKeyboard) {
        const int current = SelectedIndex_();
        if (current < 0) return;
        RECT itemRect{};
        if (!ListView_GetItemRect(list_, current, &itemRect, LVIR_BOUNDS)) return;
        screen.x = itemRect.left + 16;
        screen.y = itemRect.bottom;
        ::ClientToScreen(list_, &screen);
    }
    else {
        // GET_X_LPARAM/GET_Y_LPARAM と同じ符号付き座標変換。
        screen.x = static_cast<short>(LOWORD(contextPosition));
        screen.y = static_cast<short>(HIWORD(contextPosition));
        POINT client = screen;
        ::ScreenToClient(list_, &client);
        LVHITTESTINFO hit{};
        hit.pt = client;
        const int index = ListView_HitTest(list_, &hit);
        if (index < 0 || index >= static_cast<int>(plugins_.size())) return;
        // 右クリックした行を操作対象として選択する。
        ListView_SetItemState(list_, index,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }

    const int index = SelectedIndex_();
    if (index < 0 || index >= static_cast<int>(plugins_.size())) return;
    const PluginItem& plugin = plugins_[index];

    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;

    // メニューには「DLL自体のON/OFF」を置かない。
    const bool hasActions = plugin.enabled && plugin.running &&
        plugin.commandProc != nullptr && !plugin.actions.empty();
    if (!hasActions) {
        ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
            plugin.enabled ? L"使用可能な機能がありません" : L"DLLが無効です（チェックボックスで有効化）");
    }
    else {
        const std::size_t count = (std::min)(plugin.actions.size(),
            static_cast<std::size_t>(kMaxActions));
        for (std::size_t i = 0; i < count; ++i) {
            // 表示名とコマンドIDはプラグインが提供する。
            ::AppendMenuW(menu, MF_STRING,
                IDC_PLUGIN_CONTEXT_ACTION_BASE + static_cast<UINT_PTR>(i),
                plugin.actions[i].label.c_str());
        }
    }

    const UINT picked = ::TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        screen.x, screen.y, 0, hwnd_, nullptr);
    ::DestroyMenu(menu);

    if (picked >= IDC_PLUGIN_CONTEXT_ACTION_BASE &&
        picked < IDC_PLUGIN_CONTEXT_ACTION_BASE + kMaxActions) {
        ExecutePluginCommand_(picked - IDC_PLUGIN_CONTEXT_ACTION_BASE);
    }
}

void PluginTab::OpenPluginsFolder_()
{
    const auto dir = PluginsDir_();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ::ShellExecuteW(hwnd_, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
