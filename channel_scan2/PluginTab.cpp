#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <CommCtrl.h>
#pragma comment(lib, "Comctl32.lib")

#include "PluginTab.hpp"

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#include "ConfigManager.h"

#include <Shellapi.h>
#pragma comment(lib, "Shell32.lib")

#include <algorithm>
#include <filesystem>
#include <map>
#include <sstream>
#include <iomanip>
#include <cwctype>


namespace
{
    constexpr wchar_t kPluginTabClassName[] = L"TSCEPG_PluginTab";

    static HMENU CtrlId(int id) noexcept
    {
        return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
    }

    static void SetDefaultGuiFont(HWND hWnd, HFONT hFont)
    {
        if (hWnd != nullptr && hFont != nullptr) {
            SendMessageW(hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        }
    }

    static std::wstring ToLowerCopy(std::wstring s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
            [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        return s;
    }

    static std::wstring FormatFileSize(std::uint64_t size)
    {
        std::wostringstream oss;
        if (size >= 1024ULL * 1024ULL * 1024ULL) {
            oss << std::fixed << std::setprecision(2)
                << (static_cast<double>(size) / (1024.0 * 1024.0 * 1024.0)) << L" GB";
        }
        else if (size >= 1024ULL * 1024ULL) {
            oss << std::fixed << std::setprecision(2)
                << (static_cast<double>(size) / (1024.0 * 1024.0)) << L" MB";
        }
        else if (size >= 1024ULL) {
            oss << std::fixed << std::setprecision(2)
                << (static_cast<double>(size) / 1024.0) << L" KB";
        }
        else {
            oss << size << L" B";
        }
        return oss.str();
    }

    static std::wstring FormatFileTimeLocal(const FILETIME& ft)
    {
        if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) {
            return L"-";
        }

        FILETIME localFt{};
        SYSTEMTIME st{};
        if (!FileTimeToLocalFileTime(&ft, &localFt)) {
            return L"-";
        }
        if (!FileTimeToSystemTime(&localFt, &st)) {
            return L"-";
        }

        wchar_t buf[64]{};
        swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u",
            static_cast<unsigned>(st.wYear),
            static_cast<unsigned>(st.wMonth),
            static_cast<unsigned>(st.wDay),
            static_cast<unsigned>(st.wHour),
            static_cast<unsigned>(st.wMinute),
            static_cast<unsigned>(st.wSecond));
        return buf;
    }

    static std::map<std::wstring, bool> ReadPluginEnabledMap(const std::wstring& iniPath)
    {
        std::map<std::wstring, bool> out;

        const int count = static_cast<int>(
            GetPrivateProfileIntW(L"Plugins", L"Count", 0, iniPath.c_str()));

        for (int i = 0; i < count; ++i) {
            wchar_t keyFile[64]{};
            wchar_t keyEnabled[64]{};
            swprintf_s(keyFile, L"Plugin%d_File", i);
            swprintf_s(keyEnabled, L"Plugin%d_Enabled", i);

            wchar_t fileName[MAX_PATH]{};
            GetPrivateProfileStringW(
                L"Plugins", keyFile, L"", fileName,
                static_cast<DWORD>(std::size(fileName)), iniPath.c_str());

            if (fileName[0] == L'\0') {
                continue;
            }

            const bool enabled =
                GetPrivateProfileIntW(L"Plugins", keyEnabled, 1, iniPath.c_str()) != 0;

            out.emplace(fileName, enabled);
        }

        return out;
    }

    static int InsertListViewItemW(HWND hList, const LVITEMW* pItem) noexcept
    {
        return static_cast<int>(
            ::SendMessageW(
                hList,
                LVM_INSERTITEMW,
                0,
                reinterpret_cast<LPARAM>(pItem)));
    }

    static BOOL SetListViewItemTextDirectW(
        HWND hList,
        int row,
        int subItem,
        const wchar_t* text) noexcept
    {
        LVITEMW item{};
        item.iSubItem = subItem;
        item.pszText = const_cast<LPWSTR>(text != nullptr ? text : L"");
        return static_cast<BOOL>(
            ::SendMessageW(
                hList,
                LVM_SETITEMTEXTW,
                static_cast<WPARAM>(row),
                reinterpret_cast<LPARAM>(&item)));
    }

}

bool PluginTab::Create(
    HWND hParent,
    const RECT& rc,
    HINSTANCE hInst,
    HFONT hFont,
    std::function<void(std::wstring_view)> logCallback)
{
    hParent_ = hParent;
    hInst_ = hInst;
    hFont_ = hFont;
    log_ = std::move(logCallback);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &PluginTab::WndProc;
    wc.hInstance = hInst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kPluginTabClassName;

    if (!GetClassInfoExW(hInst_, kPluginTabClassName, &wc)) {
        if (!RegisterClassExW(&wc)) {
            return false;
        }
    }

    hWnd_ = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kPluginTabClassName,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        rc.left,
        rc.top,
        rc.right - rc.left,
        rc.bottom - rc.top,
        hParent_,
        nullptr,
        hInst_,
        this);

    return hWnd_ != nullptr;
}

void PluginTab::Show(bool show)
{
    if (hWnd_ != nullptr) {
        ShowWindow(hWnd_, show ? SW_SHOW : SW_HIDE);
    }
}

void PluginTab::Move(const RECT& rc)
{
    if (hWnd_ == nullptr) {
        return;
    }

    MoveWindow(
        hWnd_,
        rc.left,
        rc.top,
        rc.right - rc.left,
        rc.bottom - rc.top,
        TRUE);

    OnSize(rc.right - rc.left, rc.bottom - rc.top);
}

void PluginTab::ApplyToConfig()
{
    SyncCheckStatesFromList_();
    SaveEnabledStateToIni_();

    if (log_) {
        log_(L"[PluginTab] プラグイン有効状態を保存しました。");
    }
}

void PluginTab::ReloadFromConfig()
{
    RescanPlugins_();

    if (log_) {
        log_(L"[PluginTab] プラグイン一覧を再読込しました。");
    }
}

LRESULT CALLBACK PluginTab::WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PluginTab* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<PluginTab*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->hWnd_ = hWnd;
        }
    }
    else {
        self = reinterpret_cast<PluginTab*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->HandleMessage(hWnd, msg, wp, lp);
    }

    return DefWindowProcW(hWnd, msg, wp, lp);
}

LRESULT PluginTab::HandleMessage(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        OnCreate();
        return 0;

    case WM_SIZE:
        OnSize(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PLUGIN_RESCAN:
            RescanPlugins_();
            return 0;
        case IDC_PLUGIN_OPEN_FOLDER:
            OpenPluginsFolder_();
            return 0;
        default:
            break;
        }
        break;

    case WM_NOTIFY:
    {
        const auto* nm = reinterpret_cast<const NMHDR*>(lp);
        if (nm != nullptr && nm->hwndFrom == hList_) {
            if (nm->code == LVN_ITEMCHANGED) {
                const auto* lv = reinterpret_cast<const NMLISTVIEW*>(lp);
                if (lv != nullptr && lv->iItem >= 0 &&
                    lv->iItem < static_cast<int>(plugins_.size())) {

                    const bool checked = ListView_GetCheckState(hList_, lv->iItem) != FALSE;
                    plugins_[lv->iItem].enabled = checked;
                    ListView_SetItemText(
                        hList_,
                        lv->iItem,
                        0,
                        const_cast<LPWSTR>(checked ? L"有効" : L"無効"));

                    if ((lv->uNewState & LVIS_SELECTED) != 0) {
                        UpdateDetailText_(lv->iItem);
                    }
                    UpdateSummaryText_();
                }
            }
            else if (nm->code == NM_CLICK || nm->code == NM_DBLCLK) {
                UpdateDetailText_(FindSelectedIndex_());
                return 0;
            }
        }
        break;
    }

    default:
        break;
    }

    return DefWindowProcW(hWnd, msg, wp, lp);
}

void PluginTab::OnCreate()
{
    hSummary_ = CreateWindowExW(
        0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 100, 24,
        hWnd_, CtrlId(IDC_PLUGIN_SUMMARY), hInst_, nullptr);

    hBtnRescan_ = CreateWindowExW(
        0, L"BUTTON", L"再読み込み",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 100, 28,
        hWnd_, CtrlId(IDC_PLUGIN_RESCAN), hInst_, nullptr);

    hBtnOpenFolder_ = CreateWindowExW(
        0, L"BUTTON", L"フォルダを開く",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 120, 28,
        hWnd_, CtrlId(IDC_PLUGIN_OPEN_FOLDER), hInst_, nullptr);

    hList_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
        0, 0, 100, 100,
        hWnd_, CtrlId(IDC_PLUGIN_LIST), hInst_, nullptr);

    hDetail_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        0, 0, 100, 100,
        hWnd_, CtrlId(IDC_PLUGIN_DETAIL), hInst_, nullptr);

    SetDefaultGuiFont(hSummary_, hFont_);
    SetDefaultGuiFont(hBtnRescan_, hFont_);
    SetDefaultGuiFont(hBtnOpenFolder_, hFont_);
    SetDefaultGuiFont(hList_, hFont_);
    SetDefaultGuiFont(hDetail_, hFont_);

    ListView_SetExtendedListViewStyle(
        hList_,
        LVS_EX_FULLROWSELECT |
        LVS_EX_GRIDLINES |
        LVS_EX_CHECKBOXES |
        LVS_EX_DOUBLEBUFFER |
        LVS_EX_LABELTIP);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.iSubItem = 0;
    col.cx = 70;
    col.pszText = const_cast<LPWSTR>(L"状態");
    ListView_InsertColumn(hList_, 0, &col);

    col.iSubItem = 1;
    col.cx = 220;
    col.pszText = const_cast<LPWSTR>(L"プラグイン名");
    ListView_InsertColumn(hList_, 1, &col);

    col.iSubItem = 2;
    col.cx = 80;
    col.pszText = const_cast<LPWSTR>(L"種別");
    ListView_InsertColumn(hList_, 2, &col);

    col.iSubItem = 3;
    col.cx = 100;
    col.pszText = const_cast<LPWSTR>(L"サイズ");
    ListView_InsertColumn(hList_, 3, &col);

    col.iSubItem = 4;
    col.cx = 150;
    col.pszText = const_cast<LPWSTR>(L"更新日時");
    ListView_InsertColumn(hList_, 4, &col);

    col.iSubItem = 5;
    col.cx = 520;
    col.pszText = const_cast<LPWSTR>(L"パス");
    ListView_InsertColumn(hList_, 5, &col);

    RescanPlugins_();
}

void PluginTab::OnSize(int width, int height)
{
    if (hWnd_ == nullptr) {
        return;
    }

    const int pad = 10;
    const int topH = 28;
    const int btnW1 = 100;
    const int btnW2 = 120;
    const int btnH = 28;
    const int gap = 8;

    const int topY = pad;
    int rightX = width - pad;

    MoveWindow(hBtnOpenFolder_, rightX - btnW2, topY, btnW2, btnH, TRUE);
    rightX -= (btnW2 + gap);
    MoveWindow(hBtnRescan_, rightX - btnW1, topY, btnW1, btnH, TRUE);

    const int summaryW = std::max(0, rightX - pad - gap);
    MoveWindow(hSummary_, pad, topY + 4, summaryW, topH, TRUE);

    const int listTop = topY + topH + gap;
    const int detailH = std::max(120, (height - listTop - pad * 2) / 3);
    const int listH = std::max(120, height - listTop - detailH - gap - pad);

    MoveWindow(hList_, pad, listTop, width - pad * 2, listH, TRUE);
    MoveWindow(hDetail_, pad, listTop + listH + gap, width - pad * 2, detailH, TRUE);
}

void PluginTab::RescanPlugins_()
{
    SyncCheckStatesFromList_();
    LoadPluginsFromFolder_();
    RefreshList_();
    UpdateSummaryText_();

    if (!plugins_.empty()) {
        SelectRow_(0);
        UpdateDetailText_(0);
    }
    else {
        UpdateDetailText_(-1);
    }

    if (log_) {
        log_(L"[PluginTab] plugins フォルダを走査しました。");
    }
}

void PluginTab::OpenPluginsFolder_()
{
    EnsurePluginsFolderExists_();
    const std::wstring dir = GetPluginsDir_();
    ShellExecuteW(hWnd_, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void PluginTab::LoadPluginsFromFolder_()
{
    const std::map<std::wstring, bool> enabledMap = ReadPluginEnabledMap(GetIniPath_());

    plugins_.clear();
    EnsurePluginsFolderExists_();

    const std::filesystem::path dir(GetPluginsDir_());
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }

        const auto ext = ToLowerCopy(entry.path().extension().wstring());
        if (ext != L".dll") {
            continue;
        }

        PluginItem item{};
        item.fileName = entry.path().filename().wstring();
        item.fullPath = entry.path().wstring();
        item.type = L"DLL";

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(item.fullPath.c_str(), GetFileExInfoStandard, &fad)) {
            ULARGE_INTEGER li{};
            li.LowPart = fad.nFileSizeLow;
            li.HighPart = fad.nFileSizeHigh;
            item.fileSize = li.QuadPart;
            item.lastWriteTime = fad.ftLastWriteTime;
        }

        auto it = enabledMap.find(item.fileName);
        item.enabled = (it != enabledMap.end()) ? it->second : true;

        plugins_.push_back(std::move(item));
    }

    std::sort(
        plugins_.begin(),
        plugins_.end(),
        [](const PluginItem& a, const PluginItem& b) {
            return ToLowerCopy(a.fileName) < ToLowerCopy(b.fileName);
        });
}

void PluginTab::RefreshList_()
{
    if (hList_ == nullptr) {
        return;
    }

    ListView_DeleteAllItems(hList_);

    for (int i = 0; i < static_cast<int>(plugins_.size()); ++i) {
        auto& p = plugins_[i];

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(p.enabled ? L"有効" : L"無効");

        const int row = InsertListViewItemW(hList_, &item);
        if (row < 0) {
            continue;
        }

        ListView_SetCheckState(hList_, row, p.enabled ? TRUE : FALSE);

        SetListViewItemTextDirectW(hList_, row, 1, p.fileName.c_str());
        SetListViewItemTextDirectW(hList_, row, 2, p.type.c_str());

        const std::wstring sizeText = FormatFileSize(p.fileSize);
        const std::wstring timeText = FormatFileTimeLocal(p.lastWriteTime);

        SetListViewItemTextDirectW(hList_, row, 3, sizeText.c_str());
        SetListViewItemTextDirectW(hList_, row, 4, timeText.c_str());
        SetListViewItemTextDirectW(hList_, row, 5, p.fullPath.c_str());
    }
}

void PluginTab::SyncCheckStatesFromList_()
{
    if (hList_ == nullptr) {
        return;
    }

    const int count = ListView_GetItemCount(hList_);
    const int n = std::min(count, static_cast<int>(plugins_.size()));
    for (int i = 0; i < n; ++i) {
        plugins_[i].enabled = ListView_GetCheckState(hList_, i) != FALSE;
    }
}

void PluginTab::UpdateSummaryText_()
{
    if (hSummary_ == nullptr) {
        return;
    }

    std::size_t enabledCount = 0;
    for (const auto& p : plugins_) {
        if (p.enabled) {
            ++enabledCount;
        }
    }

    std::wstring text = L"plugins: " + GetPluginsDir_()
        + L"  /  検出: " + std::to_wstring(plugins_.size())
        + L"  /  有効: " + std::to_wstring(enabledCount);

    SetWindowTextW(hSummary_, text.c_str());
}

void PluginTab::UpdateDetailText_(int index)
{
    if (hDetail_ == nullptr) {
        return;
    }

    if (index < 0 || index >= static_cast<int>(plugins_.size())) {
        SetWindowTextW(
            hDetail_,
            L"プラグインが選択されていません。\r\n\r\n"
            L"このタブでは plugins フォルダ内の DLL を検出し、"
            L"有効 / 無効 の状態だけを管理します。");
        return;
    }

    const auto& p = plugins_[index];

    std::wstring text;
    text.reserve(1024);

    text += L"プラグイン名: ";
    text += p.fileName;
    text += L"\r\n";

    text += L"状態: ";
    text += (p.enabled ? L"有効" : L"無効");
    text += L"\r\n";

    text += L"種別: ";
    text += p.type;
    text += L"\r\n";

    text += L"サイズ: ";
    text += FormatFileSize(p.fileSize);
    text += L"\r\n";

    text += L"更新日時: ";
    text += FormatFileTimeLocal(p.lastWriteTime);
    text += L"\r\n";

    text += L"パス: ";
    text += p.fullPath;
    text += L"\r\n\r\n";

    text += L"※ ここでは動作エラーは表示しません。\r\n";
    text += L"※ 表示しているのは検出状態と有効フラグです。";

    SetWindowTextW(hDetail_, text.c_str());
}

void PluginTab::SaveEnabledStateToIni_()
{
    const std::wstring iniPath = GetIniPath_();

    WritePrivateProfileStringW(L"Plugins", nullptr, nullptr, iniPath.c_str());
    WritePrivateProfileStringW(L"Plugins", L"Root", GetPluginsDir_().c_str(), iniPath.c_str());

    const std::wstring countText = std::to_wstring(plugins_.size());
    WritePrivateProfileStringW(L"Plugins", L"Count", countText.c_str(), iniPath.c_str());

    for (int i = 0; i < static_cast<int>(plugins_.size()); ++i) {
        const auto& p = plugins_[i];

        wchar_t keyFile[64]{};
        wchar_t keyPath[64]{};
        wchar_t keyEnabled[64]{};

        swprintf_s(keyFile, L"Plugin%d_File", i);
        swprintf_s(keyPath, L"Plugin%d_Path", i);
        swprintf_s(keyEnabled, L"Plugin%d_Enabled", i);

        WritePrivateProfileStringW(L"Plugins", keyFile, p.fileName.c_str(), iniPath.c_str());
        WritePrivateProfileStringW(L"Plugins", keyPath, p.fullPath.c_str(), iniPath.c_str());
        WritePrivateProfileStringW(
            L"Plugins", keyEnabled, p.enabled ? L"1" : L"0", iniPath.c_str());
    }
}
std::wstring PluginTab::GetIniPath_() const
{
    return ConfigManager::GetInstance().GetIniPath();
}

std::wstring PluginTab::GetExeDir_() const
{
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return std::filesystem::path(exePath).parent_path().wstring();
}

std::wstring PluginTab::GetPluginsDir_() const
{
    return (std::filesystem::path(GetExeDir_()) / L"plugins").wstring();
}

void PluginTab::EnsurePluginsFolderExists_() const
{
    std::error_code ec;
    std::filesystem::create_directories(GetPluginsDir_(), ec);
}

int PluginTab::FindSelectedIndex_() const
{
    if (hList_ == nullptr) {
        return -1;
    }
    return ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
}

void PluginTab::SelectRow_(int index)
{
    if (hList_ == nullptr || index < 0) {
        return;
    }

    ListView_SetItemState(hList_, index,
        LVIS_SELECTED | LVIS_FOCUSED,
        LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(hList_, index, FALSE);
}