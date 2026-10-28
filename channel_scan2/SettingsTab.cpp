#include "SettingsTab.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <CommDlg.h>
#include <CommCtrl.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <cwchar>
#include <climits>

#include <ShlObj.h>     // CoTaskMemFree
#include <ShObjIdl.h>   // IFileDialog
#include <winreg.h>

#include "ConfigManager.h"
#include "bondriver_scan.h" // ScanBonDriverFilesEx()
#include "DebugManager.h"
#include "SystemInfoManager.hpp"

// ---- control IDs ----
static constexpr int IDC_ED_BON_TERRESTRIAL = 1001;
static constexpr int IDC_BT_BROWSE_BON_TERRESTRIAL = 1002;

static constexpr int IDC_ED_BON_SATELLITE = 1008;
static constexpr int IDC_BT_BROWSE_BON_SATELLITE = 1009;

static constexpr int IDC_BT_SET_BON_TERRESTRIAL = 1015;
static constexpr int IDC_BT_SET_BON_SATELLITE = 1016;

static constexpr int IDC_ED_SCANDIR = 1011;
static constexpr int IDC_BT_BROWSEDIR = 1012;
static constexpr int IDC_BT_SCAN = 1013;
static constexpr int IDC_CB_BONLIST = 1014;

static constexpr int IDC_CHK_STARTUP = 1020;
static constexpr int IDC_CHK_MINIMIZE = 1021;
static constexpr int IDC_ED_TUNERCOUNT = 1022;
static constexpr int IDC_SP_TUNERCOUNT = 1023;

static constexpr int IDC_CHK_WRITEFILE = 1003;
static constexpr int IDC_CB_CPU = 1004;
static constexpr int IDC_CB_ENC = 1005;
static constexpr int IDC_BT_RELOAD = 1006;
static constexpr int IDC_BT_SAVE = 1007;

static constexpr int IDC_CHK_RESERVATION_USE_TVTEST = 1030;
static constexpr int IDC_ED_RESERVATION_RECORDDIR = 1031;
static constexpr int IDC_BT_RESERVATION_RECORDDIR = 1032;
static constexpr int IDC_CHK_RESERVATION_TVTEST_RECORD_SHOW_VIDEO = 1033;

static constexpr int IDC_TAB_SETTINGS_PAGE = 1040;

static constexpr int IDC_ED_TIMESHIFT_ROOT = 1050;
static constexpr int IDC_BT_TIMESHIFT_ROOT = 1051;
static constexpr int IDC_ED_TIMESHIFT_MAX_GB = 1052;
static constexpr int IDC_SP_TIMESHIFT_MAX_GB = 1053;

static constexpr int TIMESHIFT_CAPACITY_GB_MIN = 1;

// アプリ側の人工的な上限は設けない。
// UpDown / EDIT の int 表現上限を実質上限にする。
static constexpr int TIMESHIFT_CAPACITY_GB_MAX = INT_MAX;

// tuner count range
static constexpr int TUNER_COUNT_MIN = 1;
static constexpr int TUNER_COUNT_MAX = 16;

namespace {

    inline HMENU CtrlId(int id) noexcept
    {
        return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
    }

    BOOL CALLBACK EnumSetFontProc(HWND child, LPARAM lp)
    {
        HFONT font = reinterpret_cast<HFONT>(lp);
        if (font) {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        return TRUE;
    }

    void ApplyFontToAllChildren(HWND parent, HFONT font)
    {
        if (!parent || !font) return;
        EnumChildWindows(parent, EnumSetFontProc, reinterpret_cast<LPARAM>(font));
    }

    std::wstring GetPathFromEdit(HWND hEdit)
    {
        if (!hEdit) return {};
        wchar_t buf[2048]{};
        GetWindowTextW(hEdit, buf, static_cast<int>(_countof(buf)));
        return std::wstring(buf);
    }

    void SetComboDroppedWidth(HWND cb, int widthPx)
    {
        if (!cb) return;
        SendMessageW(cb, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(widthPx), 0);
    }

    int ClampInt(int v, int lo, int hi) noexcept
    {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    }

    int MaxInt_(int a, int b) noexcept
    {
        return (a > b) ? a : b;
    }

    int MinInt_(int a, int b) noexcept
    {
        return (a < b) ? a : b;
    }

    int RectValueToInt_(LONG v) noexcept
    {
        if (v < static_cast<LONG>(INT_MIN)) {
            return INT_MIN;
        }

        if (v > static_cast<LONG>(INT_MAX)) {
            return INT_MAX;
        }

        return static_cast<int>(v);
    }

    int GetEditInt(HWND hEdit, int defv)
    {
        if (!hEdit) return defv;

        wchar_t buf[64]{};
        GetWindowTextW(hEdit, buf, static_cast<int>(_countof(buf)));

        wchar_t* end = nullptr;
        long v = std::wcstol(buf, &end, 10);
        if (end == buf) return defv;
        if (v < INT_MIN) return INT_MIN;
        if (v > INT_MAX) return INT_MAX;
        return static_cast<int>(v);
    }

    void SetEditInt(HWND hEdit, int v)
    {
        if (!hEdit) return;

        wchar_t buf[64]{};
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d", v);
        SetWindowTextW(hEdit, buf);
    }

    void SetUpDownRange(HWND hSpin, int low, int high)
    {
        if (!hSpin) return;
        SendMessageW(hSpin, UDM_SETRANGE32, static_cast<WPARAM>(low), static_cast<LPARAM>(high));
    }

    void SetUpDownPos(HWND hSpin, int v)
    {
        if (!hSpin) return;
        SendMessageW(hSpin, UDM_SETPOS32, 0, static_cast<LPARAM>(v));
    }

    int GetSpinWidth() noexcept
    {
        const int cx = GetSystemMetrics(SM_CXVSCROLL);
        return (cx > 0) ? cx : 18;
    }

    std::wstring GetExePathQuoted()
    {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        std::wstring cmd = L"\"";
        cmd += exePath;
        cmd += L"\"";
        return cmd;
    }


    std::filesystem::path GetExeDirectoryPath_()
    {
        wchar_t exePath[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            return std::filesystem::current_path();
        }
        return std::filesystem::path(exePath).parent_path();
    }

    std::wstring GetConfigIniPath_()
    {
        const std::wstring& p = ConfigManager::GetInstance().GetIniPath();
        if (!p.empty()) {
            return p;
        }
        return (GetExeDirectoryPath_() / L"config.ini").wstring();
    }

    std::wstring ReadIniString_(
        const wchar_t* section,
        const wchar_t* key,
        const wchar_t* defValue)
    {
        const std::wstring ini = GetConfigIniPath_();
        wchar_t buf[2048]{};
        GetPrivateProfileStringW(
            section,
            key,
            defValue ? defValue : L"",
            buf,
            static_cast<DWORD>(_countof(buf)),
            ini.c_str());
        return std::wstring(buf);
    }

    bool ReadIniBool_(
        const wchar_t* section,
        const wchar_t* key,
        bool defValue)
    {
        const std::wstring raw = ReadIniString_(section, key, defValue ? L"1" : L"0");
        if (raw == L"1" || raw == L"true" || raw == L"TRUE" || raw == L"on" || raw == L"ON") return true;
        if (raw == L"0" || raw == L"false" || raw == L"FALSE" || raw == L"off" || raw == L"OFF") return false;
        return defValue;
    }

    bool WriteIniString_(
        const wchar_t* section,
        const wchar_t* key,
        const std::wstring& value)
    {
        const std::wstring ini = GetConfigIniPath_();
        return WritePrivateProfileStringW(section, key, value.c_str(), ini.c_str()) != FALSE;
    }

    bool WriteIniBool_(
        const wchar_t* section,
        const wchar_t* key,
        bool value)
    {
        return WriteIniString_(section, key, value ? L"1" : L"0");
    }

    bool ReadReservationTvTestRecordShowVideo_()
    {
        return ReadIniBool_(
            L"ReservationExecution",
            L"TvTestRecordShowVideo",
            true);
    }

    bool WriteReservationTvTestRecordShowVideo_(bool showVideo)
    {
        return WriteIniBool_(
            L"ReservationExecution",
            L"TvTestRecordShowVideo",
            showVideo);
    }

    std::wstring MakeDefaultReservationRecordFolder_()
    {
        try {
            return (GetExeDirectoryPath_() / L"recordings").wstring();
        }
        catch (...) {
            return L"recordings";
        }
    }

    bool BrowseFolderToEdit_(HWND owner, HWND edit, const wchar_t* title)
    {
        if (!edit) {
            return false;
        }

        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool needUninit = SUCCEEDED(hr);

        IFileDialog* pfd = nullptr;
        HRESULT hr2 = CoCreateInstance(
            CLSID_FileOpenDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&pfd));

        bool selected = false;

        if (SUCCEEDED(hr2) && pfd) {
            DWORD opts = 0;
            pfd->GetOptions(&opts);
            pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            if (title && *title) {
                pfd->SetTitle(title);
            }

            const std::wstring current = GetPathFromEdit(edit);
            if (!current.empty()) {
                IShellItem* folder = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(current.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
                    pfd->SetFolder(folder);
                    folder->Release();
                }
            }

            hr2 = pfd->Show(owner);
            if (SUCCEEDED(hr2)) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(pfd->GetResult(&item)) && item) {
                    PWSTR pszPath = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath) {
                        SetWindowTextW(edit, pszPath);
                        CoTaskMemFree(pszPath);
                        selected = true;
                    }
                    item->Release();
                }
            }
            pfd->Release();
        }

        if (needUninit) {
            CoUninitialize();
        }

        return selected;
    }

    constexpr const wchar_t* kRunKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr const wchar_t* kRunValueName = L"TSCEPG";

    constexpr const wchar_t* kBonDriverIniSection = L"BonDriver";
    constexpr const wchar_t* kBonDriverTerrestrialKey = L"Terrestrial";
    constexpr const wchar_t* kBonDriverSatelliteKey = L"Satellite";
    constexpr const wchar_t* kBonDriverDefaultKey = L"Default";

} // namespace

bool SettingsTab::Create(HWND hParent, const RECT& rc, HINSTANCE inst, HFONT font,
    std::function<GuiActions& ()>&& a,
    std::function<void(std::wstring_view)>&& l)
{
    hInst = inst;
    hFont = font;
    getActions = std::move(a);
    log = std::move(l);

    // UpDown / ComboBox を含むコモンコントロールを明示初期化
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &SettingsTab::WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"TSCEPG_SettingsTab";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);

    if (!RegisterClassExW(&wc)) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
    }

    hWnd = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        wc.lpszClassName,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        rc.left,
        rc.top,
        rc.right - rc.left,
        rc.bottom - rc.top,
        hParent,
        nullptr,
        hInst,
        this);

    return (hWnd != nullptr);
}

void SettingsTab::Show(bool show)
{
    if (!hWnd) return;
    ShowWindow(hWnd, show ? SW_SHOW : SW_HIDE);
}

void SettingsTab::Move(const RECT& rc)
{
    if (!hWnd) return;
    MoveWindow(hWnd, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
}

void SettingsTab::ApplyToConfig()
{
    SaveFromUIToConfig();
}

void SettingsTab::ReloadFromConfig()
{
    ConfigManager::GetInstance().LoadConfig();
    LoadFromConfigToUI();
}

LRESULT CALLBACK SettingsTab::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    SettingsTab* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<SettingsTab*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hWnd = h;
    }
    else {
        self = reinterpret_cast<SettingsTab*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }

    return self ? self->HandleMessage(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT SettingsTab::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        OnCreate();
        return 0;

    case WM_SIZE:
        OnSize(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_ERASEBKGND:
    {
        HDC hdc = reinterpret_cast<HDC>(wp);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = reinterpret_cast<HDC>(wp);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }

    case WM_NOTIFY:
    {
        const auto* nm = reinterpret_cast<const NMHDR*>(lp);

        if (nm && nm->hwndFrom == hSettingsTab && nm->code == TCN_SELCHANGE) {
            const int sel = static_cast<int>(TabCtrl_GetCurSel(hSettingsTab));

            if (sel >= 0 && sel < static_cast<int>(PageIndex::Count)) {
                currentPage_ = static_cast<PageIndex>(sel);
                ShowSettingsPage_(currentPage_);

                RECT rc{};
                GetClientRect(hWnd, &rc);
                OnSize(rc.right - rc.left, rc.bottom - rc.top);
            }

            return 0;
        }

        return 0;
    }

    case WM_COMMAND:
    {
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);

        if (id == IDC_BT_BROWSE_BON_TERRESTRIAL) {
            BrowseTerrestrialBonDriverFile();
            return 0;
        }

        if (id == IDC_BT_BROWSE_BON_SATELLITE) {
            BrowseSatelliteBonDriverFile();
            return 0;
        }

        if (id == IDC_BT_SET_BON_TERRESTRIAL) {
            ApplySelectedBonDriverToTerrestrial_();
            return 0;
        }

        if (id == IDC_BT_SET_BON_SATELLITE) {
            ApplySelectedBonDriverToSatellite_();
            return 0;
        }
        if (id == IDC_BT_BROWSEDIR) {
            BrowseScanFolder();
            return 0;
        }
        if (id == IDC_BT_RESERVATION_RECORDDIR) {
            BrowseReservationRecordFolder();
            return 0;
        }
        if (id == IDC_BT_TIMESHIFT_ROOT) {
            BrowseTimeShiftRootFolder();
            return 0;
		}
        if (id == IDC_BT_SCAN) {
            ScanBonDriverInFolder();
            return 0;
        }
        if (id == IDC_CB_BONLIST && code == CBN_SELCHANGE) {
            OnBonListSelChanged();
            return 0;
        }
        if (id == IDC_BT_RELOAD) {
            ReloadFromConfig();
            if (log) log(L"[設定] config.ini を再読み込みしました。");
            return 0;
        }
        if (id == IDC_BT_SAVE) {
            ApplyToConfig();
            ConfigManager::GetInstance().SaveConfig();

            if (log) log(L"[設定] 保存しました。");
            return 0;
        }
        return 0;
    }

    default:
        return DefWindowProcW(hWnd, msg, wp, lp);
    }
}

void SettingsTab::RegisterPageControl_(PageIndex page, HWND child)
{
    if (!child) {
        return;
    }

    switch (page) {
    case PageIndex::BonDriver:
        pageBonDriver_.push_back(child);
        break;

    case PageIndex::General:
        pageGeneral_.push_back(child);
        break;

    case PageIndex::Reservation:
        pageReservation_.push_back(child);
        break;

    case PageIndex::TimeShift:
        pageTimeShift_.push_back(child);
        break;

    case PageIndex::Advanced:
        pageAdvanced_.push_back(child);
        break;

    default:
        break;
    }
}

void SettingsTab::ShowSettingsPage_(PageIndex page)
{
    auto showList =
        [](const std::vector<HWND>& list, bool show)
        {
            for (HWND h : list) {
                if (h) {
                    ShowWindow(h, show ? SW_SHOW : SW_HIDE);
                    EnableWindow(h, show ? TRUE : FALSE);
                }
            }
        };

    showList(pageBonDriver_, page == PageIndex::BonDriver);
    showList(pageGeneral_, page == PageIndex::General);
    showList(pageReservation_, page == PageIndex::Reservation);
    showList(pageTimeShift_, page == PageIndex::TimeShift);
    showList(pageAdvanced_, page == PageIndex::Advanced);
}

RECT SettingsTab::GetSettingsPageRect_() const
{
    RECT rc{};
    if (!hWnd) {
        return rc;
    }

    GetClientRect(hWnd, &rc);

    if (hSettingsTab) {
        RECT tabRc{};
        GetClientRect(hSettingsTab, &tabRc);
        TabCtrl_AdjustRect(hSettingsTab, FALSE, &tabRc);

        POINT lt{ tabRc.left, tabRc.top };
        POINT rb{ tabRc.right, tabRc.bottom };

        MapWindowPoints(hSettingsTab, hWnd, &lt, 1);
        MapWindowPoints(hSettingsTab, hWnd, &rb, 1);

        rc.left = lt.x;
        rc.top = lt.y;
        rc.right = rb.x;
        rc.bottom = rb.y;
    }

    rc.left += 10;
    rc.top += 10;
    rc.right -= 10;
    rc.bottom -= 10;

    return rc;
}

HWND SettingsTab::CreatePageStatic_(
    PageIndex page,
    const wchar_t* text,
    int x,
    int y,
    int w,
    int h)
{
    HWND child = CreateWindowExW(
        0,
        L"STATIC",
        text ? text : L"",
        WS_CHILD | WS_VISIBLE,
        x,
        y,
        w,
        h,
        hWnd,
        nullptr,
        hInst,
        nullptr);

    RegisterPageControl_(page, child);
    return child;
}

HWND SettingsTab::CreatePageButton_(
    PageIndex page,
    const wchar_t* text,
    DWORD style,
    int x,
    int y,
    int w,
    int h,
    int id)
{
    HWND child = CreateWindowExW(
        0,
        L"BUTTON",
        text ? text : L"",
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        w,
        h,
        hWnd,
        CtrlId(id),
        hInst,
        nullptr);

    RegisterPageControl_(page, child);
    return child;
}

HWND SettingsTab::CreatePageEdit_(
    PageIndex page,
    DWORD exStyle,
    DWORD style,
    int x,
    int y,
    int w,
    int h,
    int id)
{
    HWND child = CreateWindowExW(
        exStyle,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        w,
        h,
        hWnd,
        CtrlId(id),
        hInst,
        nullptr);

    RegisterPageControl_(page, child);
    return child;
}

HWND SettingsTab::CreatePageCombo_(
    PageIndex page,
    DWORD exStyle,
    DWORD style,
    int x,
    int y,
    int w,
    int h,
    int id)
{
    HWND child = CreateWindowExW(
        exStyle,
        L"COMBOBOX",
        L"",
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        w,
        h,
        hWnd,
        CtrlId(id),
        hInst,
        nullptr);

    RegisterPageControl_(page, child);
    return child;
}

void SettingsTab::OnCreate()
{
    hSettingsTab = CreateWindowExW(
        0,
        WC_TABCONTROLW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        6,
        6,
        100,
        100,
        hWnd,
        CtrlId(IDC_TAB_SETTINGS_PAGE),
        hInst,
        nullptr);

    {
        TCITEMW item{};
        item.mask = TCIF_TEXT;

        item.pszText = const_cast<LPWSTR>(L"チューナーDLL");
        TabCtrl_InsertItem(hSettingsTab, 0, &item);

        item.pszText = const_cast<LPWSTR>(L"基本");
        TabCtrl_InsertItem(hSettingsTab, 1, &item);

        item.pszText = const_cast<LPWSTR>(L"予約実行");
        TabCtrl_InsertItem(hSettingsTab, 2, &item);

        item.pszText = const_cast<LPWSTR>(L"タイムシフト");
        TabCtrl_InsertItem(hSettingsTab, 3, &item);

        item.pszText = const_cast<LPWSTR>(L"詳細");
        TabCtrl_InsertItem(hSettingsTab, 4, &item);

        TabCtrl_SetCurSel(hSettingsTab, static_cast<int>(currentPage_));
    }

    // ---- BonDriver page ----
    lblBonTerrestrial = CreatePageStatic_(
        PageIndex::BonDriver,
        L"地上波 BonDriver：",
        10,
        12,
        150,
        20);

    edBonTerrestrial = CreatePageEdit_(
        PageIndex::BonDriver,
        WS_EX_CLIENTEDGE,
        ES_AUTOHSCROLL,
        170,
        10,
        470,
        24,
        IDC_ED_BON_TERRESTRIAL);

    btBrowseTerrestrial = CreatePageButton_(
        PageIndex::BonDriver,
        L"参照...",
        0,
        650,
        10,
        90,
        24,
        IDC_BT_BROWSE_BON_TERRESTRIAL);

    lblBonSatellite = CreatePageStatic_(
        PageIndex::BonDriver,
        L"衛星 BonDriver：",
        10,
        48,
        150,
        20);

    edBonSatellite = CreatePageEdit_(
        PageIndex::BonDriver,
        WS_EX_CLIENTEDGE,
        ES_AUTOHSCROLL,
        170,
        46,
        470,
        24,
        IDC_ED_BON_SATELLITE);

    btBrowseSatellite = CreatePageButton_(
        PageIndex::BonDriver,
        L"参照...",
        0,
        650,
        46,
        90,
        24,
        IDC_BT_BROWSE_BON_SATELLITE);

    lblScanDir = CreatePageStatic_(
        PageIndex::BonDriver,
        L"検索フォルダ：",
        10,
        84,
        150,
        20);

    edScanDir = CreatePageEdit_(
        PageIndex::BonDriver,
        WS_EX_CLIENTEDGE,
        ES_AUTOHSCROLL,
        170,
        82,
        470,
        24,
        IDC_ED_SCANDIR);

    btBrowseDir = CreatePageButton_(
        PageIndex::BonDriver,
        L"フォルダ...",
        0,
        650,
        82,
        90,
        24,
        IDC_BT_BROWSEDIR);

    btScan = CreatePageButton_(
        PageIndex::BonDriver,
        L"スキャン",
        0,
        10,
        118,
        90,
        26,
        IDC_BT_SCAN);

    lblBonList = CreatePageStatic_(
        PageIndex::BonDriver,
        L"検出BonDriver：",
        110,
        122,
        140,
        20);

    cbBonList = CreatePageCombo_(
        PageIndex::BonDriver,
        WS_EX_CLIENTEDGE,
        CBS_DROPDOWNLIST,
        230,
        118,
        510,
        200,
        IDC_CB_BONLIST);

    btSetBonTerrestrial = CreatePageButton_(
        PageIndex::BonDriver,
        L"地上波へ設定",
        0,
        230,
        152,
        120,
        26,
        IDC_BT_SET_BON_TERRESTRIAL);

    btSetBonSatellite = CreatePageButton_(
        PageIndex::BonDriver,
        L"衛星へ設定",
        0,
        360,
        152,
        120,
        26,
        IDC_BT_SET_BON_SATELLITE);

    SetComboDroppedWidth(cbBonList, 900);

    // ---- General page ----
    chkStartup = CreatePageButton_(
        PageIndex::General,
        L"スタートアップに登録（Windowsログイン時に起動）",
        BS_AUTOCHECKBOX,
        10,
        12,
        420,
        22,
        IDC_CHK_STARTUP);

    chkMinimizeToTray = CreatePageButton_(
        PageIndex::General,
        L"最小化時にトレイへ格納",
        BS_AUTOCHECKBOX,
        10,
        42,
        260,
        22,
        IDC_CHK_MINIMIZE);

    lblTunerCount = CreatePageStatic_(
        PageIndex::General,
        L"チューナー数：",
        10,
        82,
        140,
        20);

    edTunerCount = CreatePageEdit_(
        PageIndex::General,
        WS_EX_CLIENTEDGE,
        ES_AUTOHSCROLL | ES_NUMBER,
        160,
        79,
        80,
        24,
        IDC_ED_TUNERCOUNT);

    {
        const int spinW = GetSpinWidth();

        spTunerCount = CreateWindowExW(
            0,
            UPDOWN_CLASSW,
            nullptr,
            WS_CHILD | WS_VISIBLE | UDS_ARROWKEYS | UDS_SETBUDDYINT | UDS_NOTHOUSANDS,
            160 + 80,
            79,
            spinW,
            24,
            hWnd,
            CtrlId(IDC_SP_TUNERCOUNT),
            hInst,
            nullptr);

        RegisterPageControl_(PageIndex::General, spTunerCount);
    }

    SendMessageW(spTunerCount, UDM_SETBUDDY, reinterpret_cast<WPARAM>(edTunerCount), 0);
    SetUpDownRange(spTunerCount, TUNER_COUNT_MIN, TUNER_COUNT_MAX);
    SetUpDownPos(spTunerCount, TUNER_COUNT_MIN);

    chkWriteFile = CreatePageButton_(
        PageIndex::General,
        L"デバッグログをファイルへ出力",
        BS_AUTOCHECKBOX,
        10,
        120,
        300,
        22,
        IDC_CHK_WRITEFILE);

    // ---- Reservation page ----
    chkReservationUseTvTest = CreatePageButton_(
        PageIndex::Reservation,
        L"予約でTVTest連携を使用する",
        BS_AUTOCHECKBOX,
        10,
        12,
        360,
        22,
        IDC_CHK_RESERVATION_USE_TVTEST);

    chkTvTestRecordShowVideo = CreatePageButton_(
        PageIndex::Reservation,
        L"TVTest録画中も映像を表示する（OFFで /nodshow 起動）",
        BS_AUTOCHECKBOX,
        10,
        42,
        520,
        22,
        IDC_CHK_RESERVATION_TVTEST_RECORD_SHOW_VIDEO);

    lblReservationRecordDir = CreatePageStatic_(
        PageIndex::Reservation,
        L"録画保存フォルダ：",
        10,
        82,
        140,
        20);

    edReservationRecordDir = CreatePageEdit_(
        PageIndex::Reservation,
        WS_EX_CLIENTEDGE,
        ES_AUTOHSCROLL,
        160,
        79,
        480,
        24,
        IDC_ED_RESERVATION_RECORDDIR);

    btReservationRecordDir = CreatePageButton_(
        PageIndex::Reservation,
        L"フォルダ...",
        0,
        650,
        79,
        90,
        24,
        IDC_BT_RESERVATION_RECORDDIR);

    // ---- TimeShift page ----
    lblTimeShiftRootFolder = CreatePageStatic_(
        PageIndex::TimeShift,
        L"保存先フォルダ：",
        10,
        12,
        140,
        20);

    edTimeShiftRootFolder = CreatePageEdit_(
        PageIndex::TimeShift,
        WS_EX_CLIENTEDGE,
        ES_AUTOHSCROLL,
        160,
        9,
        480,
        24,
        IDC_ED_TIMESHIFT_ROOT);

    btTimeShiftRootFolder = CreatePageButton_(
        PageIndex::TimeShift,
        L"フォルダ...",
        0,
        650,
        9,
        90,
        24,
        IDC_BT_TIMESHIFT_ROOT);

    lblTimeShiftMaxBytesGB = CreatePageStatic_(
        PageIndex::TimeShift,
        L"全体容量上限 GB：",
        10,
        52,
        150,
        20);

    edTimeShiftMaxBytesGB = CreatePageEdit_(
        PageIndex::TimeShift,
        WS_EX_CLIENTEDGE,
        ES_AUTOHSCROLL | ES_NUMBER,
        160,
        49,
        120,
        24,
        IDC_ED_TIMESHIFT_MAX_GB);

    {
        const int spinW = GetSpinWidth();

        spTimeShiftMaxBytesGB = CreateWindowExW(
            0,
            UPDOWN_CLASSW,
            nullptr,
            WS_CHILD | WS_VISIBLE | UDS_ARROWKEYS | UDS_SETBUDDYINT | UDS_NOTHOUSANDS,
            160 + 120,
            49,
            spinW,
            24,
            hWnd,
            CtrlId(IDC_SP_TIMESHIFT_MAX_GB),
            hInst,
            nullptr);

        RegisterPageControl_(PageIndex::TimeShift, spTimeShiftMaxBytesGB);
    }

    SendMessageW(
        spTimeShiftMaxBytesGB,
        UDM_SETBUDDY,
        reinterpret_cast<WPARAM>(edTimeShiftMaxBytesGB),
        0);

    SetUpDownRange(
        spTimeShiftMaxBytesGB,
        TIMESHIFT_CAPACITY_GB_MIN,
        TIMESHIFT_CAPACITY_GB_MAX);

    SetUpDownPos(
        spTimeShiftMaxBytesGB,
        TIMESHIFT_CAPACITY_GB_MIN);

    // ---- Advanced page ----
    lblCpu = CreatePageStatic_(
        PageIndex::Advanced,
        L"CPU命令セット：",
        10,
        12,
        140,
        20);

    cbCpu = CreatePageCombo_(
        PageIndex::Advanced,
        WS_EX_CLIENTEDGE,
        CBS_DROPDOWNLIST,
        160,
        9,
        240,
        200,
        IDC_CB_CPU);

    lblEnc = CreatePageStatic_(
        PageIndex::Advanced,
        L"ログ文字コード：",
        10,
        52,
        140,
        20);

    cbEnc = CreatePageCombo_(
        PageIndex::Advanced,
        WS_EX_CLIENTEDGE,
        CBS_DROPDOWNLIST,
        160,
        49,
        240,
        200,
        IDC_CB_ENC);

    btReload = CreatePageButton_(
        PageIndex::Advanced,
        L"再読み込み",
        0,
        10,
        92,
        120,
        28,
        IDC_BT_RELOAD);

    btSave = CreatePageButton_(
        PageIndex::Advanced,
        L"保存",
        0,
        140,
        92,
        120,
        28,
        IDC_BT_SAVE);

    // ---- Combo items ----
    {
        // AUTO:
        //   CPU機能判定に任せる通常運用。
        //
        // SCALAR:
        //   SIMD不具合時の安全退避用。
        //
        // SSE2 / SSE4.2 / AVX2:
        //   ベンチ・検証・強制指定用。
        const wchar_t* cpuItems[] = {
            L"AUTO",
            L"SCALAR",
            L"SSE2",
            L"SSE4.2",
            L"AVX2"
        };

        for (const auto* s : cpuItems) {
            SendMessageW(cbCpu, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        }

        const wchar_t* encItems[] = { L"sjis", L"utf8" };
        for (const auto* s : encItems) {
            SendMessageW(cbEnc, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        }
    }

    ApplyFontToAllChildren(hWnd, hFont);

    LoadFromConfigToUI();

    if (GetWindowTextLengthW(edScanDir) == 0) {
        wchar_t cwd[MAX_PATH]{};
        if (GetCurrentDirectoryW(MAX_PATH, cwd)) {
            SetWindowTextW(edScanDir, cwd);
        }
    }

    ShowSettingsPage_(currentPage_);

    RECT rc{};
    GetClientRect(hWnd, &rc);
    OnSize(rc.right - rc.left, rc.bottom - rc.top);
}

bool SettingsTab::GetStartupRegistered_() const
{
    HKEY hKey{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    wchar_t buf[2048]{};
    DWORD cb = sizeof(buf);

    const LONG rc = RegQueryValueExW(hKey, kRunValueName, nullptr, &type,
        reinterpret_cast<LPBYTE>(buf), &cb);
    RegCloseKey(hKey);

    if (rc != ERROR_SUCCESS) return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return false;
    return true;
}

bool SettingsTab::SetStartupRegistered_(bool enable, std::wstring& outErr) const
{
    outErr.clear();

    HKEY hKey{};
    DWORD disp = 0;
    const LONG r0 = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        kRunKeyPath,
        0,
        nullptr,
        0,
        KEY_SET_VALUE | KEY_QUERY_VALUE,
        nullptr,
        &hKey,
        &disp);

    if (r0 != ERROR_SUCCESS) {
        outErr = L"RegCreateKeyExW failed: " + std::to_wstring(static_cast<unsigned long>(r0));
        return false;
    }

    bool ok = true;

    if (enable) {
        const std::wstring cmd = GetExePathQuoted();
        const DWORD cb = static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t));
        const LONG r1 = RegSetValueExW(
            hKey,
            kRunValueName,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(cmd.c_str()),
            cb);

        if (r1 != ERROR_SUCCESS) {
            outErr = L"RegSetValueExW failed: " + std::to_wstring(static_cast<unsigned long>(r1));
            ok = false;
        }
    }
    else {
        const LONG r1 = RegDeleteValueW(hKey, kRunValueName);
        if (r1 != ERROR_SUCCESS && r1 != ERROR_FILE_NOT_FOUND) {
            outErr = L"RegDeleteValueW failed: " + std::to_wstring(static_cast<unsigned long>(r1));
            ok = false;
        }
    }

    RegCloseKey(hKey);
    return ok;
}

void SettingsTab::LoadFromConfigToUI()
{
    auto& cfg = ConfigManager::GetInstance();

    {
        const std::wstring terrestrialBon =
            cfg.GetBonDriverTerrestrialPath();

        const std::wstring satelliteBon =
            cfg.GetBonDriverSatellitePath();

        SetWindowTextW(
            edBonTerrestrial,
            terrestrialBon.c_str());

        SetWindowTextW(
            edBonSatellite,
            satelliteBon.c_str());
    }

    SendMessageW(chkWriteFile, BM_SETCHECK,
        cfg.GetDebugWriteToFile() ? BST_CHECKED : BST_UNCHECKED, 0);

    SendMessageW(chkMinimizeToTray, BM_SETCHECK,
        cfg.GetUiMinimizeToTray() ? BST_CHECKED : BST_UNCHECKED, 0);

    {
        int c = cfg.GetTunersCount();
        c = ClampInt(c, TUNER_COUNT_MIN, TUNER_COUNT_MAX);
        SetEditInt(edTunerCount, c);
        SetUpDownPos(spTunerCount, c);
    }

    {
        const bool regOn = GetStartupRegistered_();
        SendMessageW(chkStartup, BM_SETCHECK, regOn ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    {
        const bool useTvTest =
            cfg.GetReservationExecutionEnableTvTestWatch();

        SendMessageW(
            chkReservationUseTvTest,
            BM_SETCHECK,
            useTvTest ? BST_CHECKED : BST_UNCHECKED,
            0);

        const bool showVideo =
            cfg.GetReservationExecutionTvTestRecordShowVideo();

        SendMessageW(
            chkTvTestRecordShowVideo,
            BM_SETCHECK,
            showVideo ? BST_CHECKED : BST_UNCHECKED,
            0);

        std::wstring recordDir =
            cfg.GetReservationExecutionRecordFolder();

        if (recordDir.empty()) {
            recordDir = MakeDefaultReservationRecordFolder_();
        }

        SetWindowTextW(edReservationRecordDir, recordDir.c_str());
    }

    {
        std::wstring root = cfg.GetTimeShiftRootFolderRaw();
        if (root.empty()) {
            root = L"timeshift";
        }

        SetWindowTextW(edTimeShiftRootFolder, root.c_str());

        {
            int maxGb = cfg.GetTimeShiftMaxBytesGB();
            maxGb = ClampInt(
                maxGb,
                TIMESHIFT_CAPACITY_GB_MIN,
                TIMESHIFT_CAPACITY_GB_MAX);

            SetEditInt(edTimeShiftMaxBytesGB, maxGb);
            SetUpDownPos(spTimeShiftMaxBytesGB, maxGb);
        }
    }

    const std::wstring cpu = cfg.GetSelectedCPUFeature();
    const int cpuCount =
        static_cast<int>(SendMessageW(cbCpu, CB_GETCOUNT, 0, 0));

    int sel = 0; // 既定は AUTO

    for (int i = 0; i < cpuCount; ++i) {
        wchar_t buf[64]{};
        SendMessageW(cbCpu, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(buf));

        if (cpu == buf) {
            sel = i;
            break;
        }
    }

    // 空表示防止。
    // config.ini 側が未知値・空値でも AUTO を表示する。
    if (cpuCount > 0) {
        if (sel < 0 || sel >= cpuCount) {
            sel = 0;
        }

        SendMessageW(cbCpu, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
    }

    const std::wstring enc = cfg.GetLogEncoding();
    const int encCount = static_cast<int>(SendMessageW(cbEnc, CB_GETCOUNT, 0, 0));
    int sel2 = 0;
    for (int i = 0; i < encCount; ++i) {
        wchar_t buf[64]{};
        SendMessageW(cbEnc, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(buf));
        if (enc == buf) {
            sel2 = i;
            break;
        }
    }
    SendMessageW(cbEnc, CB_SETCURSEL, sel2, 0);
}



void SettingsTab::SaveFromUIToConfig()
{
    auto& cfg = ConfigManager::GetInstance();

    {
        wchar_t bonTerrestrial[2048]{};
        wchar_t bonSatellite[2048]{};

        GetWindowTextW(
            edBonTerrestrial,
            bonTerrestrial,
            static_cast<int>(_countof(bonTerrestrial)));

        GetWindowTextW(
            edBonSatellite,
            bonSatellite,
            static_cast<int>(_countof(bonSatellite)));

        const std::wstring terrestrial =
            bonTerrestrial;

        const std::wstring satellite =
            bonSatellite;

        cfg.SetBonDriverTerrestrialPath(terrestrial);
        cfg.SetBonDriverSatellitePath(satellite);

        // Default は未知space / 旧互換用。
        // 基本は地上波。地上波が空なら衛星。
        cfg.SetBonDriverDefaultPath(
            !terrestrial.empty()
            ? terrestrial
            : satellite);

        if (log) {
            std::wstring msg = L"[設定] 地上波 BonDriver=";
            msg += terrestrial.empty() ? L"(未設定)" : terrestrial;
            log(msg);

            msg = L"[設定] 衛星 BonDriver=";
            msg += satellite.empty() ? L"(未設定)" : satellite;
            log(msg);
        }
    }

    const bool writeFile =
        (SendMessageW(chkWriteFile, BM_GETCHECK, 0, 0) == BST_CHECKED);
    cfg.SetDebugWriteToFile(writeFile);

    const bool minimizeToTray =
        (SendMessageW(chkMinimizeToTray, BM_GETCHECK, 0, 0) == BST_CHECKED);
    cfg.SetUiMinimizeToTray(minimizeToTray);

    {
        int c = GetEditInt(edTunerCount, cfg.GetTunersCount());
        c = ClampInt(c, TUNER_COUNT_MIN, TUNER_COUNT_MAX);
        cfg.SetTunersCount(c);
        SetEditInt(edTunerCount, c);
        SetUpDownPos(spTunerCount, c);
    }

    {
        const bool enableStartup =
            (SendMessageW(chkStartup, BM_GETCHECK, 0, 0) == BST_CHECKED);

        std::wstring err;
        const bool ok = SetStartupRegistered_(enableStartup, err);

        cfg.SetUiStartupEnabled(enableStartup);

        if (!ok) {
            if (log) {
                std::wstring msg = L"[設定][WARN] スタートアップ登録の変更に失敗: ";
                msg += err;
                log(msg);
            }
        }
        else {
            if (log) {
                log(enableStartup ? L"[設定] スタートアップ登録=ON"
                    : L"[設定] スタートアップ登録=OFF");
            }
        }
    }

    {
        const bool useTvTest =
            (SendMessageW(chkReservationUseTvTest, BM_GETCHECK, 0, 0) == BST_CHECKED);

        cfg.SetReservationExecutionEnableTvTestWatch(useTvTest);

        const bool showVideo =
            (SendMessageW(chkTvTestRecordShowVideo, BM_GETCHECK, 0, 0) == BST_CHECKED);

        cfg.SetReservationExecutionTvTestRecordShowVideo(showVideo);

        DebugLogFormat(
            L"[SettingsTab][ReservationExecution] EnableTvTestWatch=%d TvTestRecordShowVideo=%d",
            useTvTest ? 1 : 0,
            showVideo ? 1 : 0);

        SystemInfoLogInfo(
            L"予約実行設定を更新しました。TVTest連携=%ls TVTest録画中映像表示=%ls",
            useTvTest ? L"ON" : L"OFF",
            showVideo ? L"ON" : L"OFF (/nodshow)");

        std::wstring recordDir = GetPathFromEdit(edReservationRecordDir);
        if (recordDir.empty()) {
            recordDir = MakeDefaultReservationRecordFolder_();
            SetWindowTextW(edReservationRecordDir, recordDir.c_str());
        }

        try {
            if (!recordDir.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(recordDir), ec);

                if (ec) {
                    DebugLogFormat(
                        L"[SettingsTab][ReservationExecution] create_directories failed path=%ls error=%hs",
                        recordDir.c_str(),
                        ec.message().c_str());
                }
            }
        }
        catch (...) {
            DebugLogFormat(
                L"[SettingsTab][ReservationExecution] create_directories exception path=%ls",
                recordDir.c_str());
        }

        cfg.SetReservationExecutionRecordFolder(recordDir);

        if (log) {
            log(useTvTest
                ? L"[設定] 予約TVTest連携=ON"
                : L"[設定] 予約TVTest連携=OFF");

            log(showVideo
                ? L"[設定] TVTest録画中の映像表示=ON"
                : L"[設定] TVTest録画中の映像表示=OFF (/nodshow)");
        }
    }

    
    {
        std::wstring root = GetPathFromEdit(edTimeShiftRootFolder);
        if (root.empty()) {
            root = L"timeshift";
            SetWindowTextW(edTimeShiftRootFolder, root.c_str());
        }

        cfg.SetTimeShiftRootFolderRaw(root);

        int maxGb =
            GetEditInt(
                edTimeShiftMaxBytesGB,
                cfg.GetTimeShiftMaxBytesGB());

        maxGb =
            ClampInt(
                maxGb,
                TIMESHIFT_CAPACITY_GB_MIN,
                TIMESHIFT_CAPACITY_GB_MAX);

        cfg.SetTimeShiftMaxBytesGB(maxGb);

        SetEditInt(edTimeShiftMaxBytesGB, maxGb);
        SetUpDownPos(spTimeShiftMaxBytesGB, maxGb);

        if (log) {
            std::wstring msg = L"[設定] タイムシフト保存先=";
            msg += root;
            log(msg);

            msg = L"[設定] タイムシフト全体容量上限GB=";
            msg += std::to_wstring(maxGb);
            log(msg);
        }
    }

	const int cpuSel = static_cast<int>(SendMessageW(cbCpu, CB_GETCURSEL, 0, 0));
    if (cpuSel >= 0) {
        wchar_t buf[64]{};
        SendMessageW(cbCpu, CB_GETLBTEXT, cpuSel, reinterpret_cast<LPARAM>(buf));
        cfg.SetSelectedCPUFeature(buf);
    }

    const int encSel = static_cast<int>(SendMessageW(cbEnc, CB_GETCURSEL, 0, 0));
    if (encSel >= 0) {
        wchar_t buf[64]{};
        SendMessageW(cbEnc, CB_GETLBTEXT, encSel, reinterpret_cast<LPARAM>(buf));
        cfg.SetLogEncoding(buf);
    }
}

bool SettingsTab::BrowseBonDriverFileToEdit_(
    HWND targetEdit,
    const wchar_t* logLabel)
{
    if (!targetEdit) {
        return false;
    }

    wchar_t file[MAX_PATH]{};

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = _countof(file);
    ofn.lpstrFilter =
        L"BonDriver DLL (*.dll)\0*.dll\0すべてのファイル (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (!GetOpenFileNameW(&ofn)) {
        return false;
    }

    SetWindowTextW(targetEdit, file);

    if (log) {
        std::wstring msg = L"[設定] ";
        msg += (logLabel != nullptr && logLabel[0] != L'\0')
            ? logLabel
            : L"BonDriver";
        msg += L" を選択しました。";
        log(msg);
    }

    return true;
}

void SettingsTab::BrowseTerrestrialBonDriverFile()
{
    (void)BrowseBonDriverFileToEdit_(
        edBonTerrestrial,
        L"地上波 BonDriver");
}

void SettingsTab::BrowseSatelliteBonDriverFile()
{
    (void)BrowseBonDriverFileToEdit_(
        edBonSatellite,
        L"衛星 BonDriver");
}

void SettingsTab::BrowseScanFolder()
{
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = SUCCEEDED(hr);

    IFileDialog* pfd = nullptr;
    HRESULT hr2 = CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pfd));

    if (SUCCEEDED(hr2) && pfd) {
        DWORD opts = 0;
        pfd->GetOptions(&opts);
        pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

        hr2 = pfd->Show(hWnd);
        if (SUCCEEDED(hr2)) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(pfd->GetResult(&item)) && item) {
                PWSTR pszPath = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath) {
                    SetWindowTextW(edScanDir, pszPath);
                    CoTaskMemFree(pszPath);
                    if (log) log(L"[設定] 検索フォルダを選択しました。");
                }
                item->Release();
            }
        }
        pfd->Release();
    }

    if (needUninit) {
        CoUninitialize();
    }
}

void SettingsTab::BrowseReservationRecordFolder()
{
    if (BrowseFolderToEdit_(hWnd, edReservationRecordDir, L"録画保存フォルダを選択")) {
        if (log) log(L"[設定] 録画保存フォルダを選択しました。");
    }
}

void SettingsTab::BrowseTimeShiftRootFolder()
{
    if (BrowseFolderToEdit_(hWnd, edTimeShiftRootFolder, L"タイムシフト保存先フォルダを選択")) {
        if (log) log(L"[設定] タイムシフト保存先フォルダを選択しました。");
    }
}

void SettingsTab::ScanBonDriverInFolder()
{
    const std::wstring dir = GetPathFromEdit(edScanDir);
    if (dir.empty()) {
        if (log) log(L"[設定] 検索フォルダが空です。");
        return;
    }

    const auto r = ScanBonDriverFilesEx(dir, false);
    if (r.ec) {
        if (log) {
            std::wstring m = L"[設定] スキャン失敗: ";
            m += std::wstring(r.ec.message().begin(), r.ec.message().end());
            log(m);
        }
        return;
    }

    scannedBonPaths = r.list;

    SendMessageW(cbBonList, CB_RESETCONTENT, 0, 0);

    if (scannedBonPaths.empty()) {
        if (log) log(L"[設定] BonDriver DLL が見つかりませんでした。");
        return;
    }

    for (size_t i = 0; i < scannedBonPaths.size(); ++i) {
        const std::wstring& full = scannedBonPaths[i];
        std::wstring show = full;

        try {
            std::filesystem::path p(full);
            show = p.filename().wstring();
        }
        catch (...) {
        }

        const LRESULT idx = SendMessageW(cbBonList, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(show.c_str()));
        if (idx >= 0) {
            SendMessageW(cbBonList, CB_SETITEMDATA, static_cast<WPARAM>(idx), static_cast<LPARAM>(i));
        }
    }

    SendMessageW(cbBonList, CB_SETCURSEL, 0, 0);
    OnBonListSelChanged();

    if (log) {
        std::wstring m = L"[設定] BonDriver を ";
        m += std::to_wstring(scannedBonPaths.size());
        m += L" 件検出しました。";
        log(m);
    }
}

std::wstring SettingsTab::GetSelectedScannedBonDriverPath_() const
{
    if (!cbBonList) {
        return {};
    }

    const int sel =
        static_cast<int>(
            SendMessageW(cbBonList, CB_GETCURSEL, 0, 0));

    if (sel < 0) {
        return {};
    }

    const LRESULT data =
        SendMessageW(
            cbBonList,
            CB_GETITEMDATA,
            static_cast<WPARAM>(sel),
            0);

    if (data < 0) {
        return {};
    }

    const std::size_t idx =
        static_cast<std::size_t>(data);

    if (idx >= scannedBonPaths.size()) {
        return {};
    }

    return scannedBonPaths[idx];
}

void SettingsTab::ApplySelectedBonDriverToTerrestrial_()
{
    const std::wstring path =
        GetSelectedScannedBonDriverPath_();

    if (path.empty()) {
        if (log) log(L"[設定] 検出BonDriverが選択されていません。");
        return;
    }

    SetWindowTextW(
        edBonTerrestrial,
        path.c_str());

    if (log) {
        log(L"[設定] 検出一覧から地上波 BonDriver へ設定しました。");
    }
}

void SettingsTab::ApplySelectedBonDriverToSatellite_()
{
    const std::wstring path =
        GetSelectedScannedBonDriverPath_();

    if (path.empty()) {
        if (log) log(L"[設定] 検出BonDriverが選択されていません。");
        return;
    }

    SetWindowTextW(
        edBonSatellite,
        path.c_str());

    if (log) {
        log(L"[設定] 検出一覧から衛星 BonDriver へ設定しました。");
    }
}

void SettingsTab::OnBonListSelChanged()
{
    const std::wstring path =
        GetSelectedScannedBonDriverPath_();

    if (path.empty()) {
        return;
    }

    if (log) {
        std::wstring msg = L"[設定] 検出BonDriverを選択: ";
        msg += path;
        log(msg);
    }
}

void SettingsTab::OnSize(int w, int h)
{
    if (!hWnd) return;

    const int outerPad = 8;
    const int tabX = outerPad;
    const int tabY = outerPad;
    const int tabW = MaxInt_(160, w - outerPad * 2);
    const int tabH = MaxInt_(120, h - outerPad * 2);

    const int labelW = 150;
    const int buttonW = 90;
    const int buttonGap = 10;
    const int rowH = 24;
    const int rowGap = 12;
    const int checkH = 22;
    const int spinW = GetSpinWidth();

    if (hSettingsTab) {
        MoveWindow(
            hSettingsTab,
            tabX,
            tabY,
            tabW,
            tabH,
            TRUE);

        // タブの枠が兄弟コントロールを上書きしないように、ページコントロールを最後に前面へ出す。
        SetWindowPos(
            hSettingsTab,
            HWND_BOTTOM,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    const RECT pageRcRaw = GetSettingsPageRect_();

    const int pageLeft = RectValueToInt_(pageRcRaw.left);
    const int pageTop = RectValueToInt_(pageRcRaw.top);
    const int pageRight = RectValueToInt_(pageRcRaw.right);

    const int x0 = pageLeft + 6;
    const int y0 = pageTop + 6;

    const int leftEdit = x0 + labelW;
    const int rightLimit = MaxInt_(leftEdit + 220, pageRight - 6);

    const int editRight = MaxInt_(leftEdit + 240, rightLimit - buttonW - buttonGap);
    const int editW = MaxInt_(220, editRight - leftEdit);
    const int rightBtnX = editRight + buttonGap;

    const int wideTextW = MaxInt_(260, rightLimit - x0);
    const int comboW = MinInt_(280, MaxInt_(180, rightLimit - leftEdit));

    auto moveLabel = [](HWND h, int x, int y, int w)
        {
            if (h) MoveWindow(h, x, y + 3, w, 20, TRUE);
        };

    auto moveEdit = [](HWND h, int x, int y, int w)
        {
            if (h) MoveWindow(h, x, y, w, 24, TRUE);
        };

    auto moveButton = [](HWND h, int x, int y, int w)
        {
            if (h) MoveWindow(h, x, y, w, 24, TRUE);
        };

    auto bringFront = [](const std::vector<HWND>& list)
        {
            for (HWND h : list) {
                if (h) {
                    SetWindowPos(
                        h,
                        HWND_TOP,
                        0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
            }
        };

    // ------------------------------------------------------------
// BonDriver page
// ------------------------------------------------------------
    {
        int y = y0 + 4;

        moveLabel(lblBonTerrestrial, x0, y, labelW);
        moveEdit(edBonTerrestrial, leftEdit, y, editW);
        moveButton(btBrowseTerrestrial, rightBtnX, y, buttonW);

        y += rowH + rowGap;

        moveLabel(lblBonSatellite, x0, y, labelW);
        moveEdit(edBonSatellite, leftEdit, y, editW);
        moveButton(btBrowseSatellite, rightBtnX, y, buttonW);

        y += rowH + rowGap;

        moveLabel(lblScanDir, x0, y, labelW);
        moveEdit(edScanDir, leftEdit, y, editW);
        moveButton(btBrowseDir, rightBtnX, y, buttonW);

        y += rowH + rowGap + 2;

        if (btScan) {
            MoveWindow(btScan, x0, y, 90, 26, TRUE);
        }

        if (lblBonList) {
            MoveWindow(lblBonList, x0 + 110, y + 4, 120, 20, TRUE);
        }

        const int cbX = x0 + 230;
        const int cbW = MaxInt_(220, rightLimit - cbX);

        if (cbBonList) {
            MoveWindow(cbBonList, cbX, y, cbW, 220, TRUE);
        }

        y += 34;

        if (btSetBonTerrestrial) {
            MoveWindow(btSetBonTerrestrial, cbX, y, 120, 26, TRUE);
        }

        if (btSetBonSatellite) {
            MoveWindow(btSetBonSatellite, cbX + 132, y, 120, 26, TRUE);
        }
    }

    // ------------------------------------------------------------
    // General page
    // ------------------------------------------------------------
    {
        int y = y0 + 8;

        if (chkStartup) {
            MoveWindow(chkStartup, x0, y, wideTextW, checkH, TRUE);
        }

        y += 30;

        if (chkMinimizeToTray) {
            MoveWindow(chkMinimizeToTray, x0, y, wideTextW, checkH, TRUE);
        }

        y += 40;

        moveLabel(lblTunerCount, x0, y, labelW);

        if (edTunerCount) {
            MoveWindow(edTunerCount, leftEdit, y, 80, rowH, TRUE);
        }

        if (spTunerCount) {
            MoveWindow(spTunerCount, leftEdit + 80, y, spinW, rowH, TRUE);
        }

        y += 42;

        if (chkWriteFile) {
            MoveWindow(chkWriteFile, x0, y, wideTextW, checkH, TRUE);
        }
    }

    // ------------------------------------------------------------
    // Reservation page
    // ------------------------------------------------------------
    {
        int y = y0 + 8;

        if (chkReservationUseTvTest) {
            MoveWindow(chkReservationUseTvTest, x0, y, wideTextW, checkH, TRUE);
        }

        y += 30;

        if (chkTvTestRecordShowVideo) {
            MoveWindow(chkTvTestRecordShowVideo, x0, y, wideTextW, checkH, TRUE);
        }

        y += 40;

        moveLabel(lblReservationRecordDir, x0, y, labelW);
        moveEdit(edReservationRecordDir, leftEdit, y, editW);
        moveButton(btReservationRecordDir, rightBtnX, y, buttonW);
    }

    // ------------------------------------------------------------
// TimeShift page
// ------------------------------------------------------------
    {
        int y = y0 + 8;

        moveLabel(lblTimeShiftRootFolder, x0, y, labelW);
        moveEdit(edTimeShiftRootFolder, leftEdit, y, editW);
        moveButton(btTimeShiftRootFolder, rightBtnX, y, buttonW);

        y += rowH + rowGap;

        moveLabel(lblTimeShiftMaxBytesGB, x0, y, labelW);

        if (edTimeShiftMaxBytesGB) {
            MoveWindow(edTimeShiftMaxBytesGB, leftEdit, y, 120, rowH, TRUE);
        }

        if (spTimeShiftMaxBytesGB) {
            MoveWindow(
                spTimeShiftMaxBytesGB,
                leftEdit + 120,
                y,
                spinW,
                rowH,
                TRUE);
        }
    }

    // ------------------------------------------------------------
    // Advanced page
    // ------------------------------------------------------------
    {
        int y = y0 + 8;

        moveLabel(lblCpu, x0, y, labelW);

        if (cbCpu) {
            MoveWindow(cbCpu, leftEdit, y, comboW, 220, TRUE);
        }

        y += 40;

        moveLabel(lblEnc, x0, y, labelW);

        if (cbEnc) {
            MoveWindow(cbEnc, leftEdit, y, comboW, 220, TRUE);
        }

        y += 42;

        if (btReload) {
            MoveWindow(btReload, x0, y, 120, 28, TRUE);
        }

        if (btSave) {
            MoveWindow(btSave, x0 + 132, y, 120, 28, TRUE);
        }
    }

    ShowSettingsPage_(currentPage_);

    switch (currentPage_) {
    case PageIndex::BonDriver:
        bringFront(pageBonDriver_);
        break;

    case PageIndex::General:
        bringFront(pageGeneral_);
        break;

    case PageIndex::Reservation:
        bringFront(pageReservation_);
        break;

    case PageIndex::TimeShift:
        bringFront(pageTimeShift_);
        break;

    case PageIndex::Advanced:
        bringFront(pageAdvanced_);
        break;

    default:
        break;
    }

    if (hSettingsTab) {
        InvalidateRect(hSettingsTab, nullptr, TRUE);
    }
}