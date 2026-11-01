#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "NetworkSettingsTab.hpp"

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#include "ConfigManager.h"

#include <CommCtrl.h>
#include <CommDlg.h>
#include <wincrypt.h>
#include <algorithm>
#include <array>
#include <string>
#include <vector>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Crypt32.lib")

namespace
{
    constexpr wchar_t kClassName[] = L"TSCEPG_NetworkSettingsTab";
    constexpr wchar_t kPageHostClassName[] = L"TSCEPG_NetworkSettingsPageHost";

    enum ControlId : int
    {
        IDC_NET_SUBTAB = 5100,

        IDC_NET_ENABLE_SERVER = 5101,
        IDC_NET_BIND_ADDRESS,
        IDC_NET_HTTP_PORT,
        IDC_NET_ALLOW_REMOTE,
        IDC_NET_AUTO_START,

        IDC_NET_ENABLE_HTTPS,
        IDC_NET_HTTPS_PORT,
        IDC_NET_PFX_PATH,
        IDC_NET_BROWSE_PFX,
        IDC_NET_PFX_PASSWORD,

        IDC_NET_ENABLE_PASSWORD_AUTH,
        IDC_NET_LOGIN_USER,
        IDC_NET_LOGIN_PASSWORD,

        IDC_NET_REQUEST_TIMEOUT,
        IDC_NET_ENABLE_CORS,
        IDC_NET_OPEN_BROWSER
    };

    LRESULT CALLBACK PageHostWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_COMMAND:
        case WM_NOTIFY:
        {
            // 入力コントロールの通知は直接の親であるページホストへ届く。
            // 実際の設定処理を持つ NetworkSettingsTab まで転送する。
            const HWND hTab = GetParent(hWnd);
            const HWND hSettingsTab = hTab ? GetParent(hTab) : nullptr;
            if (hSettingsTab) {
                return SendMessageW(hSettingsTab, msg, wp, lp);
            }
            break;
        }

        case WM_ERASEBKGND:
        {
            RECT rc{};
            GetClientRect(hWnd, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
            return 1;
        }
        default:
            break;
        }
        return DefWindowProcW(hWnd, msg, wp, lp);
    }

    bool EnsureClassRegistered(HINSTANCE hInst)
    {
        WNDCLASSEXW wc{};
        if (GetClassInfoExW(hInst, kClassName, &wc) == FALSE) {
            wc = {};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = NetworkSettingsTab::WndProc;
            wc.hInstance = hInst;
            wc.lpszClassName = kClassName;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            wc.style = CS_HREDRAW | CS_VREDRAW;

            if (RegisterClassExW(&wc) == 0) {
                return false;
            }
        }

        WNDCLASSEXW pageWc{};
        if (GetClassInfoExW(hInst, kPageHostClassName, &pageWc) == FALSE) {
            pageWc = {};
            pageWc.cbSize = sizeof(pageWc);
            pageWc.lpfnWndProc = PageHostWndProc;
            pageWc.hInstance = hInst;
            pageWc.lpszClassName = kPageHostClassName;
            pageWc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            pageWc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            pageWc.style = CS_HREDRAW | CS_VREDRAW;

            if (RegisterClassExW(&pageWc) == 0) {
                return false;
            }
        }

        return true;
    }

    void SetDefaultGuiFont(HWND hWnd, HFONT hFont)
    {
        if (hWnd && hFont) {
            SendMessageW(hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        }
    }

    int ClampPort(int value, int defValue)
    {
        if (value < 1 || value > 65535) {
            return defValue;
        }
        return value;
    }

    int ClampTimeoutMs(int value, int defValue)
    {
        if (value < 100) {
            return defValue;
        }
        if (value > 600000) {
            return 600000;
        }
        return value;
    }

    int ParseIntOrDefault(const std::wstring& s, int defValue)
    {
        if (s.empty()) {
            return defValue;
        }
        try {
            return std::stoi(s);
        }
        catch (...) {
            return defValue;
        }
    }

    void SetCheck(HWND hWnd, bool checked)
    {
        SendMessageW(hWnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    bool GetCheck(HWND hWnd)
    {
        return SendMessageW(hWnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    int HexValue(wchar_t ch) noexcept
    {
        if (ch >= L'0' && ch <= L'9') return ch - L'0';
        if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
        if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
        return -1;
    }

    std::wstring ProtectSecret(const std::wstring& plainText)
    {
        if (plainText.empty()) return {};

        DATA_BLOB input{};
        input.cbData = static_cast<DWORD>(plainText.size() * sizeof(wchar_t));
        input.pbData = reinterpret_cast<BYTE*>(const_cast<wchar_t*>(plainText.data()));

        DATA_BLOB output{};
        if (!::CryptProtectData(
            &input,
            L"TSCEPG Network Secret",
            nullptr,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &output)) {
            return {};
        }

        static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
        std::wstring encoded;
        encoded.resize(static_cast<std::size_t>(output.cbData) * 2U);
        for (DWORD i = 0; i < output.cbData; ++i) {
            const BYTE value = output.pbData[i];
            encoded[static_cast<std::size_t>(i) * 2U] = kHex[value >> 4];
            encoded[static_cast<std::size_t>(i) * 2U + 1U] = kHex[value & 0x0F];
        }

        ::SecureZeroMemory(output.pbData, output.cbData);
        ::LocalFree(output.pbData);
        return encoded;
    }

    std::wstring UnprotectSecret(const std::wstring& encoded)
    {
        if (encoded.empty() || (encoded.size() % 2U) != 0U) return {};

        std::vector<BYTE> encrypted(encoded.size() / 2U);
        for (std::size_t i = 0; i < encrypted.size(); ++i) {
            const int hi = HexValue(encoded[i * 2U]);
            const int lo = HexValue(encoded[i * 2U + 1U]);
            if (hi < 0 || lo < 0) return {};
            encrypted[i] = static_cast<BYTE>((hi << 4) | lo);
        }

        DATA_BLOB input{};
        input.cbData = static_cast<DWORD>(encrypted.size());
        input.pbData = encrypted.data();

        DATA_BLOB output{};
        if (!::CryptUnprotectData(
            &input,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &output)) {
            ::SecureZeroMemory(encrypted.data(), encrypted.size());
            return {};
        }

        std::wstring plainText;
        if ((output.cbData % sizeof(wchar_t)) == 0U) {
            const auto* text = reinterpret_cast<const wchar_t*>(output.pbData);
            plainText.assign(text, text + output.cbData / sizeof(wchar_t));
        }

        ::SecureZeroMemory(encrypted.data(), encrypted.size());
        ::SecureZeroMemory(output.pbData, output.cbData);
        ::LocalFree(output.pbData);
        return plainText;
    }

    UINT GetWindowDpiSafe(HWND hWnd)
    {
        if (!hWnd) {
            return 96;
        }

        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        static const auto pGetDpiForWindow =
            reinterpret_cast<GetDpiForWindowFn>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));

        if (pGetDpiForWindow) {
            const UINT dpi = pGetDpiForWindow(hWnd);
            return dpi == 0 ? 96U : dpi;
        }

        HDC hdc = GetDC(hWnd);
        if (!hdc) {
            return 96;
        }

        const int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(hWnd, hdc);
        return dpiX > 0 ? static_cast<UINT>(dpiX) : 96U;
    }

    HWND CreatePageHost(HWND hParent, HINSTANCE hInst)
    {
        return CreateWindowExW(
            WS_EX_CONTROLPARENT,
            kPageHostClassName,
            L"",
            WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 100, 100,
            hParent,
            nullptr,
            hInst,
            nullptr);
    }
}

bool NetworkSettingsTab::Create(HWND hParent, const RECT& rc, HINSTANCE hInst, HFONT hFont, ConfigManager& cfg)
{
    hInst_ = hInst;
    hFont_ = hFont;
    cfg_ = &cfg;

    if (!EnsureClassRegistered(hInst_)) {
        return false;
    }

    hWnd_ = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kClassName,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        rc.left,
        rc.top,
        rc.right - rc.left,
        rc.bottom - rc.top,
        hParent,
        nullptr,
        hInst_,
        this);

    if (!hWnd_) {
        return false;
    }

    Show(false);
    return true;
}

void NetworkSettingsTab::Show(bool show)
{
    if (!hWnd_) {
        return;
    }

    ShowWindow(hWnd_, show ? SW_SHOW : SW_HIDE);

    if (show) {
        RECT rc{};
        GetClientRect(hWnd_, &rc);
        LayoutControls(rc);
    }
}

void NetworkSettingsTab::Move(const RECT& rc)
{
    if (!hWnd_) {
        return;
    }

    MoveWindow(hWnd_, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);

    RECT local{};
    GetClientRect(hWnd_, &local);
    LayoutControls(local);
}

void NetworkSettingsTab::ApplyToConfig()
{
    if (!cfg_) {
        return;
    }

    ConfigManager::NetworkSettings settings = cfg_->LoadNetworkSettings();

    settings.enableServer = GetCheck(hChkEnableServer_);
    settings.allowRemoteAccess = GetCheck(hChkAllowRemote_);
    settings.autoStartServer = GetCheck(hChkAutoStart_);
    // HTTPS / Schannel は次段階で実装する。誤って有効設定を保存しない。
    settings.enableHttps = false;
    settings.enablePasswordAuth = GetCheck(hChkEnablePasswordAuth_);
    settings.enableCors = GetCheck(hChkEnableCors_);
    settings.openBrowserOnStart = GetCheck(hChkOpenBrowser_);

    settings.bindAddress = settings.allowRemoteAccess ? L"0.0.0.0" : L"127.0.0.1";
    settings.httpPort = ClampPort(ParseIntOrDefault(GetText(hEditHttpPort_), 8080), 8080);
    settings.httpsPort = ClampPort(ParseIntOrDefault(GetText(hEditHttpsPort_), 8443), 8443);
    settings.requestTimeoutMs = ClampTimeoutMs(
        ParseIntOrDefault(GetText(hEditRequestTimeout_), 15000), 15000);
    settings.pfxPath = GetText(hEditPfxPath_);
    settings.loginUser = GetText(hEditLoginUser_);

    std::wstring pfxPassword = GetText(hEditPfxPassword_);
    std::wstring loginPassword = GetText(hEditLoginPassword_);

    const std::wstring protectedPfxPassword = ProtectSecret(pfxPassword);
    if (pfxPassword.empty() || !protectedPfxPassword.empty()) {
        settings.pfxPasswordProtected = protectedPfxPassword;
    }

    const std::wstring protectedLoginPassword = ProtectSecret(loginPassword);
    if (loginPassword.empty() || !protectedLoginPassword.empty()) {
        settings.loginPasswordProtected = protectedLoginPassword;
    }
    settings.sessionTimeoutMinutes = 720;
    settings.tlsMode = L"http";
    settings.listenScope = settings.allowRemoteAccess ? L"lan" : L"local";

    cfg_->SaveNetworkSettings(settings);

    if (!pfxPassword.empty()) {
        ::SecureZeroMemory(pfxPassword.data(), pfxPassword.size() * sizeof(wchar_t));
    }
    if (!loginPassword.empty()) {
        ::SecureZeroMemory(loginPassword.data(), loginPassword.size() * sizeof(wchar_t));
    }

    SetText(hEditBindAddress_, settings.bindAddress);
    SetText(hEditHttpPort_, std::to_wstring(settings.httpPort));
    SetText(hEditHttpsPort_, std::to_wstring(settings.httpsPort));
    SetText(hEditRequestTimeout_, std::to_wstring(settings.requestTimeoutMs));

    UpdateControlEnableState();
}

void NetworkSettingsTab::ReloadFromConfig()
{
    if (!cfg_) {
        return;
    }

    const ConfigManager::NetworkSettings settings = cfg_->LoadNetworkSettings();

    SetCheck(hChkEnableServer_, settings.enableServer);
    SetCheck(hChkAllowRemote_, settings.allowRemoteAccess);
    SetCheck(hChkAutoStart_, settings.autoStartServer);
    SetCheck(hChkEnableHttps_, false);
    SetCheck(hChkEnablePasswordAuth_, settings.enablePasswordAuth);
    SetCheck(hChkEnableCors_, settings.enableCors);
    SetCheck(hChkOpenBrowser_, settings.openBrowserOnStart);

    SetText(hEditBindAddress_, settings.bindAddress);
    SetText(hEditHttpPort_, std::to_wstring(settings.httpPort));
    SetText(hEditHttpsPort_, std::to_wstring(settings.httpsPort));
    SetText(hEditPfxPath_, settings.pfxPath);
    SetText(hEditPfxPassword_, UnprotectSecret(settings.pfxPasswordProtected));
    SetText(hEditLoginUser_, settings.loginUser);
    SetText(hEditLoginPassword_, UnprotectSecret(settings.loginPasswordProtected));
    SetText(hEditRequestTimeout_, std::to_wstring(settings.requestTimeoutMs));

    UpdateControlEnableState();
}

std::wstring NetworkSettingsTab::GetText(HWND hWnd) const
{
    if (!hWnd) {
        return L"";
    }

    const int len = GetWindowTextLengthW(hWnd);
    if (len <= 0) {
        return L"";
    }

    std::wstring s(static_cast<size_t>(len) + 1U, L'\0');
    GetWindowTextW(hWnd, s.data(), len + 1);
    s.resize(static_cast<size_t>(len));
    return s;
}

void NetworkSettingsTab::SetText(HWND hWnd, const std::wstring& text)
{
    if (hWnd) {
        SetWindowTextW(hWnd, text.c_str());
    }
}

bool NetworkSettingsTab::BrowseFile(HWND hTargetEdit, const wchar_t* title, const wchar_t* filter)
{
    if (!hWnd_ || !hTargetEdit) {
        return false;
    }

    wchar_t fileName[MAX_PATH]{};
    const std::wstring current = GetText(hTargetEdit);
    if (!current.empty()) {
        wcsncpy_s(fileName, current.c_str(), _TRUNCATE);
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd_;
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = static_cast<DWORD>(std::size(fileName));
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetOpenFileNameW(&ofn)) {
        return false;
    }

    SetText(hTargetEdit, fileName);
    return true;
}

LRESULT CALLBACK NetworkSettingsTab::WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    NetworkSettingsTab* self = reinterpret_cast<NetworkSettingsTab*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* pThis = reinterpret_cast<NetworkSettingsTab*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        if (pThis) {
            pThis->hWnd_ = hWnd;
        }
        return TRUE;
    }

    if (!self) {
        return DefWindowProcW(hWnd, msg, wp, lp);
    }

    return self->HandleMessage(hWnd, msg, wp, lp);
}

LRESULT NetworkSettingsTab::HandleMessage(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        OnCreate();
        ReloadFromConfig();
        return 0;

    case WM_SIZE:
    {
        RECT rc{};
        GetClientRect(hWnd, &rc);
        LayoutControls(rc);
        ShowSelectedSubPage();
        return 0;
    }

    case WM_NOTIFY:
    {
        const NMHDR* hdr = reinterpret_cast<const NMHDR*>(lp);
        if (hdr && hdr->hwndFrom == hSubTab_ && hdr->code == TCN_SELCHANGE) {
            ShowSelectedSubPage();
            LayoutCurrentPage();
            return 0;
        }
        break;
    }

    case WM_COMMAND:
    {
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);

        if (code == BN_CLICKED) {
            switch (id) {
            case IDC_NET_ENABLE_SERVER:
            case IDC_NET_ENABLE_HTTPS:
            case IDC_NET_ENABLE_PASSWORD_AUTH:
            case IDC_NET_ALLOW_REMOTE:
            case IDC_NET_AUTO_START:
            case IDC_NET_ENABLE_CORS:
            case IDC_NET_OPEN_BROWSER:
                UpdateControlEnableState();
                ApplyToConfig();
                return 0;

            case IDC_NET_BROWSE_PFX:
                if (BrowseFile(
                    hEditPfxPath_,
                    L"Schannel用PFX証明書を選択",
                    L"PFX証明書 (*.pfx;*.p12)\0*.pfx;*.p12\0すべてのファイル (*.*)\0*.*\0\0")) {
                    ApplyToConfig();
                }
                return 0;

            default:
                break;
            }
        }

        if (code == EN_KILLFOCUS) {
            switch (id) {
            case IDC_NET_HTTP_PORT:
            case IDC_NET_HTTPS_PORT:
            case IDC_NET_PFX_PATH:
            case IDC_NET_PFX_PASSWORD:
            case IDC_NET_LOGIN_USER:
            case IDC_NET_LOGIN_PASSWORD:
            case IDC_NET_REQUEST_TIMEOUT:
                ApplyToConfig();
                return 0;

            default:
                break;
            }
        }
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hWnd, msg, wp, lp);
}

void NetworkSettingsTab::OnCreate()
{
    hSubTab_ = CreateWindowExW(
        0,
        WC_TABCONTROLW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, 100, 100,
        hWnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_SUBTAB)),
        hInst_,
        nullptr);

    SetDefaultGuiFont(hSubTab_, hFont_);

    TCITEMW tie{};
    tie.mask = TCIF_TEXT;

    tie.pszText = const_cast<LPWSTR>(L"基本");
    TabCtrl_InsertItem(hSubTab_, 0, &tie);
    tie.pszText = const_cast<LPWSTR>(L"HTTPS");
    TabCtrl_InsertItem(hSubTab_, 1, &tie);
    tie.pszText = const_cast<LPWSTR>(L"認証");
    TabCtrl_InsertItem(hSubTab_, 2, &tie);
    tie.pszText = const_cast<LPWSTR>(L"補助");
    TabCtrl_InsertItem(hSubTab_, 3, &tie);

    // ここが重要:
    // ページ親を hWnd_ ではなく hSubTab_ にする
    hPageBasic_ = CreatePageHost(hSubTab_, hInst_);
    hPageHttps_ = CreatePageHost(hSubTab_, hInst_);
    hPageAuth_ = CreatePageHost(hSubTab_, hInst_);
    hPageAdvanced_ = CreatePageHost(hSubTab_, hInst_);

    SetDefaultGuiFont(hPageBasic_, hFont_);
    SetDefaultGuiFont(hPageHttps_, hFont_);
    SetDefaultGuiFont(hPageAuth_, hFont_);
    SetDefaultGuiFont(hPageAdvanced_, hFont_);

    const DWORD labelStyle = WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX;
    const DWORD editStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL;
    const DWORD checkStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX;
    const DWORD buttonStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;

    // 基本ページ
    hChkEnableServer_ = CreateWindowExW(0, L"BUTTON", L"HTTPサーバーを有効にする", checkStyle,
        0, 0, 100, 24, hPageBasic_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_ENABLE_SERVER)), hInst_, nullptr);
    hStaticBindAddress_ = CreateWindowExW(0, L"STATIC", L"待受アドレス（自動設定）", labelStyle,
        0, 0, 100, 18, hPageBasic_, nullptr, hInst_, nullptr);
    hEditBindAddress_ = CreateWindowExW(0, L"EDIT", L"", editStyle | ES_READONLY,
        0, 0, 100, 24, hPageBasic_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_BIND_ADDRESS)), hInst_, nullptr);
    hStaticHttpPort_ = CreateWindowExW(0, L"STATIC", L"HTTP Port", labelStyle,
        0, 0, 100, 18, hPageBasic_, nullptr, hInst_, nullptr);
    hEditHttpPort_ = CreateWindowExW(0, L"EDIT", L"", editStyle | ES_NUMBER,
        0, 0, 100, 24, hPageBasic_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_HTTP_PORT)), hInst_, nullptr);
    hChkAllowRemote_ = CreateWindowExW(0, L"BUTTON", L"ホームネットワーク内の端末から接続を許可する", checkStyle,
        0, 0, 100, 24, hPageBasic_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_ALLOW_REMOTE)), hInst_, nullptr);
    hChkAutoStart_ = CreateWindowExW(0, L"BUTTON", L"アプリ起動時にサーバー起動を許可する", checkStyle,
        0, 0, 100, 24, hPageBasic_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_AUTO_START)), hInst_, nullptr);
    hStaticBasicNote_ = CreateWindowExW(0, L"STATIC",
        L"チェックなしはこのPCのみ（127.0.0.1）、チェックありはホームネットワーク向け（0.0.0.0）に自動設定します。",
        labelStyle,
        0, 0, 100, 40, hPageBasic_, nullptr, hInst_, nullptr);

    // HTTPSページ
    hChkEnableHttps_ = CreateWindowExW(0, L"BUTTON", L"HTTPS を有効にする（未実装）", checkStyle,
        0, 0, 100, 24, hPageHttps_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_ENABLE_HTTPS)), hInst_, nullptr);
    hStaticHttpsPort_ = CreateWindowExW(0, L"STATIC", L"HTTPS Port", labelStyle,
        0, 0, 100, 18, hPageHttps_, nullptr, hInst_, nullptr);
    hEditHttpsPort_ = CreateWindowExW(0, L"EDIT", L"", editStyle | ES_NUMBER,
        0, 0, 100, 24, hPageHttps_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_HTTPS_PORT)), hInst_, nullptr);
    hStaticPfxPath_ = CreateWindowExW(0, L"STATIC", L"PFX証明書パス", labelStyle,
        0, 0, 100, 18, hPageHttps_, nullptr, hInst_, nullptr);
    hEditPfxPath_ = CreateWindowExW(0, L"EDIT", L"", editStyle,
        0, 0, 100, 24, hPageHttps_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_PFX_PATH)), hInst_, nullptr);
    hBtnBrowsePfx_ = CreateWindowExW(0, L"BUTTON", L"参照...", buttonStyle,
        0, 0, 80, 24, hPageHttps_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_BROWSE_PFX)), hInst_, nullptr);
    hStaticPfxPassword_ = CreateWindowExW(0, L"STATIC", L"PFXパスワード", labelStyle,
        0, 0, 100, 18, hPageHttps_, nullptr, hInst_, nullptr);
    hEditPfxPassword_ = CreateWindowExW(0, L"EDIT", L"", editStyle | ES_PASSWORD,
        0, 0, 100, 24, hPageHttps_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_PFX_PASSWORD)), hInst_, nullptr);
    hStaticHttpsNote_ = CreateWindowExW(0, L"STATIC",
        L"Schannel対応は未実装です。このページは将来設定用の表示のみで、現在の通信はHTTPを使用します。",
        labelStyle,
        0, 0, 100, 40, hPageHttps_, nullptr, hInst_, nullptr);

    // 認証ページ
    hChkEnablePasswordAuth_ = CreateWindowExW(0, L"BUTTON", L"パスワードログインを有効にする", checkStyle,
        0, 0, 100, 24, hPageAuth_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_ENABLE_PASSWORD_AUTH)), hInst_, nullptr);
    hStaticLoginUser_ = CreateWindowExW(0, L"STATIC", L"ログインユーザー名", labelStyle,
        0, 0, 100, 18, hPageAuth_, nullptr, hInst_, nullptr);
    hEditLoginUser_ = CreateWindowExW(0, L"EDIT", L"", editStyle,
        0, 0, 100, 24, hPageAuth_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_LOGIN_USER)), hInst_, nullptr);
    hStaticLoginPassword_ = CreateWindowExW(0, L"STATIC", L"ログインパスワード", labelStyle,
        0, 0, 100, 18, hPageAuth_, nullptr, hInst_, nullptr);
    hEditLoginPassword_ = CreateWindowExW(0, L"EDIT", L"", editStyle | ES_PASSWORD,
        0, 0, 100, 24, hPageAuth_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_LOGIN_PASSWORD)), hInst_, nullptr);
    hStaticAuthNote_ = CreateWindowExW(0, L"STATIC",
        L"ユーザー名とパスワードは必須です。認証情報はWindows DPAPIで暗号化保存します。現在はHTTPのためLAN内試験に限定してください。",
        labelStyle,
        0, 0, 100, 48, hPageAuth_, nullptr, hInst_, nullptr);

    // 補助ページ
    hStaticRequestTimeout_ = CreateWindowExW(0, L"STATIC", L"要求タイムアウト(ms)", labelStyle,
        0, 0, 100, 18, hPageAdvanced_, nullptr, hInst_, nullptr);
    hEditRequestTimeout_ = CreateWindowExW(0, L"EDIT", L"", editStyle | ES_NUMBER,
        0, 0, 100, 24, hPageAdvanced_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_REQUEST_TIMEOUT)), hInst_, nullptr);
    hChkEnableCors_ = CreateWindowExW(0, L"BUTTON", L"CORS を許可する", checkStyle,
        0, 0, 100, 24, hPageAdvanced_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_ENABLE_CORS)), hInst_, nullptr);
    hChkOpenBrowser_ = CreateWindowExW(0, L"BUTTON", L"サーバー起動時にブラウザ起動を許可する", checkStyle,
        0, 0, 100, 24, hPageAdvanced_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_OPEN_BROWSER)), hInst_, nullptr);
    hStaticAdvancedNote_ = CreateWindowExW(0, L"STATIC",
        L"補助設定は開発段階の使い勝手を整える項目です。必要に応じて今後ログや認証などの項目をここへ追加できます。",
        labelStyle,
        0, 0, 100, 48, hPageAdvanced_, nullptr, hInst_, nullptr);

    const std::array<HWND, 28> ctrls = {
        hChkEnableServer_, hStaticBindAddress_, hEditBindAddress_, hStaticHttpPort_, hEditHttpPort_, hChkAllowRemote_, hChkAutoStart_, hStaticBasicNote_,
        hChkEnableHttps_, hStaticHttpsPort_, hEditHttpsPort_, hStaticPfxPath_, hEditPfxPath_, hBtnBrowsePfx_, hStaticPfxPassword_, hEditPfxPassword_, hStaticHttpsNote_,
        hChkEnablePasswordAuth_, hStaticLoginUser_, hEditLoginUser_, hStaticLoginPassword_, hEditLoginPassword_, hStaticAuthNote_,
        hStaticRequestTimeout_, hEditRequestTimeout_, hChkEnableCors_, hChkOpenBrowser_, hStaticAdvancedNote_
    };

    for (HWND h : ctrls) {
        SetDefaultGuiFont(h, hFont_);
    }

    TabCtrl_SetCurSel(hSubTab_, 0);
    ShowSelectedSubPage();

    RECT rc{};
    GetClientRect(hWnd_, &rc);
    LayoutControls(rc);
}

int NetworkSettingsTab::GetCurrentSubTabIndex() const noexcept
{
    if (!hSubTab_) {
        return 0;
    }
    const int sel = TabCtrl_GetCurSel(hSubTab_);
    return sel < 0 ? 0 : sel;
}

int NetworkSettingsTab::ScaleByDpi(int value) const
{
    const UINT dpi = GetWindowDpiSafe(hWnd_ ? hWnd_ : GetDesktopWindow());
    return MulDiv(value, static_cast<int>(dpi), 96);
}

int NetworkSettingsTab::GetTextLineHeight() const
{
    if (!hWnd_) {
        return 16;
    }

    HDC hdc = GetDC(hWnd_);
    if (!hdc) {
        return 16;
    }

    HFONT oldFont = nullptr;
    if (hFont_) {
        oldFont = static_cast<HFONT>(SelectObject(hdc, hFont_));
    }

    TEXTMETRICW tm{};
    const BOOL ok = GetTextMetricsW(hdc, &tm);

    if (oldFont) {
        SelectObject(hdc, oldFont);
    }
    ReleaseDC(hWnd_, hdc);

    if (!ok) {
        return 16;
    }

    return tm.tmHeight + tm.tmExternalLeading;
}

void NetworkSettingsTab::LayoutControls(const RECT& rc)
{
    const int width = (std::max<int>)(0, static_cast<int>(rc.right - rc.left));
    const int height = (std::max<int>)(0, static_cast<int>(rc.bottom - rc.top));
    if (width <= 0 || height <= 0 || !hSubTab_) {
        return;
    }

    const int pad = ScaleByDpi(8);
    const int tabW = (std::max<int>)(100, width - pad * 2);
    const int tabH = (std::max<int>)(100, height - pad * 2);

    MoveWindow(hSubTab_, pad, pad, tabW, tabH, TRUE);
    LayoutCurrentPage();
}

void NetworkSettingsTab::LayoutCurrentPage()
{
    if (!hSubTab_) {
        return;
    }

    RECT tabRc{};
    GetClientRect(hSubTab_, &tabRc);
    TabCtrl_AdjustRect(hSubTab_, FALSE, &tabRc);

    const int pageW = (std::max<int>)(100, static_cast<int>(tabRc.right - tabRc.left));
    const int pageH = (std::max<int>)(100, static_cast<int>(tabRc.bottom - tabRc.top));

    const std::array<HWND, 4> pages = { hPageBasic_, hPageHttps_, hPageAuth_, hPageAdvanced_ };
    for (HWND page : pages) {
        if (page) {
            SetWindowPos(page, nullptr, tabRc.left, tabRc.top, pageW, pageH,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }

    auto layoutBasic = [&]() {
        const int textH = GetTextLineHeight();
        const int pad = ScaleByDpi(12);
        const int labelH = (std::max<int>)(ScaleByDpi(18), textH + ScaleByDpi(2));
        const int editH = (std::max<int>)(ScaleByDpi(24), textH + ScaleByDpi(10));
        const int rowH = (std::max<int>)(ScaleByDpi(24), textH + ScaleByDpi(8));
        const int gap = ScaleByDpi(8);
        const int smallEditW = ScaleByDpi(140);
        const int contentW = (std::max<int>)(ScaleByDpi(160), pageW - pad * 2);

        int y = pad;
        MoveWindow(hChkEnableServer_, pad, y, contentW, rowH, TRUE);
        y += rowH + gap;

        MoveWindow(hStaticBindAddress_, pad, y, contentW, labelH, TRUE);
        y += labelH + ScaleByDpi(4);
        MoveWindow(hEditBindAddress_, pad, y, contentW, editH, TRUE);
        y += editH + gap;

        MoveWindow(hStaticHttpPort_, pad, y, contentW, labelH, TRUE);
        y += labelH + ScaleByDpi(4);
        MoveWindow(hEditHttpPort_, pad, y, smallEditW, editH, TRUE);
        y += editH + gap;

        MoveWindow(hChkAllowRemote_, pad, y, contentW, rowH, TRUE);
        y += rowH + ScaleByDpi(6);

        MoveWindow(hChkAutoStart_, pad, y, contentW, rowH, TRUE);
        y += rowH + gap;

        const int noteH = (std::max<int>)(ScaleByDpi(52), pageH - y - pad);
        MoveWindow(hStaticBasicNote_, pad, y, contentW, noteH, TRUE);
        };

    auto layoutHttps = [&]() {
        const int textH = GetTextLineHeight();
        const int pad = ScaleByDpi(12);
        const int labelH = (std::max<int>)(ScaleByDpi(18), textH + ScaleByDpi(2));
        const int editH = (std::max<int>)(ScaleByDpi(24), textH + ScaleByDpi(10));
        const int rowH = (std::max<int>)(ScaleByDpi(24), textH + ScaleByDpi(8));
        const int gap = ScaleByDpi(8);
        const int btnGap = ScaleByDpi(8);
        const int btnW = ScaleByDpi(84);
        const int smallEditW = ScaleByDpi(140);
        const int contentW = (std::max<int>)(ScaleByDpi(180), pageW - pad * 2);
        const int longEditW = (std::max<int>)(ScaleByDpi(120), contentW - btnW - btnGap);

        int y = pad;
        MoveWindow(hChkEnableHttps_, pad, y, contentW, rowH, TRUE);
        y += rowH + gap;

        MoveWindow(hStaticHttpsPort_, pad, y, contentW, labelH, TRUE);
        y += labelH + ScaleByDpi(4);
        MoveWindow(hEditHttpsPort_, pad, y, smallEditW, editH, TRUE);
        y += editH + gap;

        MoveWindow(hStaticPfxPath_, pad, y, contentW, labelH, TRUE);
        y += labelH + ScaleByDpi(4);
        MoveWindow(hEditPfxPath_, pad, y, longEditW, editH, TRUE);
        MoveWindow(hBtnBrowsePfx_, pad + longEditW + btnGap, y, btnW, editH, TRUE);
        y += editH + gap;

        MoveWindow(hStaticPfxPassword_, pad, y, contentW, labelH, TRUE);
        y += labelH + ScaleByDpi(4);
        MoveWindow(hEditPfxPassword_, pad, y, contentW, editH, TRUE);
        y += editH + gap;

        const int noteH = (std::max<int>)(ScaleByDpi(52), pageH - y - pad);
        MoveWindow(hStaticHttpsNote_, pad, y, contentW, noteH, TRUE);
        };

    auto layoutAuth = [&]() {
        const int textH = GetTextLineHeight();
        const int pad = ScaleByDpi(12);
        const int labelH = (std::max<int>)(ScaleByDpi(18), textH + ScaleByDpi(2));
        const int editH = (std::max<int>)(ScaleByDpi(24), textH + ScaleByDpi(10));
        const int rowH = (std::max<int>)(ScaleByDpi(24), textH + ScaleByDpi(8));
        const int gap = ScaleByDpi(8);
        const int contentW = (std::max<int>)(ScaleByDpi(180), pageW - pad * 2);

        int y = pad;
        MoveWindow(hChkEnablePasswordAuth_, pad, y, contentW, rowH, TRUE);
        y += rowH + gap;

        MoveWindow(hStaticLoginUser_, pad, y, contentW, labelH, TRUE);
        y += labelH + ScaleByDpi(4);
        MoveWindow(hEditLoginUser_, pad, y, contentW, editH, TRUE);
        y += editH + gap;

        MoveWindow(hStaticLoginPassword_, pad, y, contentW, labelH, TRUE);
        y += labelH + ScaleByDpi(4);
        MoveWindow(hEditLoginPassword_, pad, y, contentW, editH, TRUE);
        y += editH + gap;

        const int noteH = (std::max<int>)(ScaleByDpi(56), pageH - y - pad);
        MoveWindow(hStaticAuthNote_, pad, y, contentW, noteH, TRUE);
        };

    auto layoutAdvanced = [&]() {
        const int textH = GetTextLineHeight();
        const int pad = ScaleByDpi(12);
        const int labelH = (std::max<int>)(ScaleByDpi(18), textH + ScaleByDpi(2));
        const int editH = (std::max<int>)(ScaleByDpi(24), textH + ScaleByDpi(10));
        const int rowH = (std::max<int>)(ScaleByDpi(24), textH + ScaleByDpi(8));
        const int gap = ScaleByDpi(8);
        const int smallEditW = ScaleByDpi(140);
        const int contentW = (std::max<int>)(ScaleByDpi(160), pageW - pad * 2);

        int y = pad;
        MoveWindow(hStaticRequestTimeout_, pad, y, contentW, labelH, TRUE);
        y += labelH + ScaleByDpi(4);
        MoveWindow(hEditRequestTimeout_, pad, y, smallEditW, editH, TRUE);
        y += editH + gap;

        MoveWindow(hChkEnableCors_, pad, y, contentW, rowH, TRUE);
        y += rowH + ScaleByDpi(6);
        MoveWindow(hChkOpenBrowser_, pad, y, contentW, rowH, TRUE);
        y += rowH + gap;

        const int noteH = (std::max<int>)(ScaleByDpi(56), pageH - y - pad);
        MoveWindow(hStaticAdvancedNote_, pad, y, contentW, noteH, TRUE);
        };

    switch (GetCurrentSubTabIndex()) {
    case 1:
        if (hPageHttps_) {
            SetWindowPos(hPageHttps_, HWND_TOP, tabRc.left, tabRc.top, pageW, pageH,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        layoutHttps();
        break;

    case 2:
        if (hPageAuth_) {
            SetWindowPos(hPageAuth_, HWND_TOP, tabRc.left, tabRc.top, pageW, pageH,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        layoutAuth();
        break;

    case 3:
        if (hPageAdvanced_) {
            SetWindowPos(hPageAdvanced_, HWND_TOP, tabRc.left, tabRc.top, pageW, pageH,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        layoutAdvanced();
        break;

    case 0:
    default:
        if (hPageBasic_) {
            SetWindowPos(hPageBasic_, HWND_TOP, tabRc.left, tabRc.top, pageW, pageH,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        layoutBasic();
        break;
    }

    UpdateControlEnableState();
}

void NetworkSettingsTab::ShowSelectedSubPage()
{
    const int sel = GetCurrentSubTabIndex();

    if (hPageBasic_) {
        ShowWindow(hPageBasic_, sel == 0 ? SW_SHOW : SW_HIDE);
    }
    if (hPageHttps_) {
        ShowWindow(hPageHttps_, sel == 1 ? SW_SHOW : SW_HIDE);
    }
    if (hPageAuth_) {
        ShowWindow(hPageAuth_, sel == 2 ? SW_SHOW : SW_HIDE);
    }
    if (hPageAdvanced_) {
        ShowWindow(hPageAdvanced_, sel == 3 ? SW_SHOW : SW_HIDE);
    }

    if (hSubTab_) {
        RedrawWindow(hSubTab_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
}

void NetworkSettingsTab::UpdateControlEnableState()
{
    const bool enableServer = GetCheck(hChkEnableServer_);
    const bool allowHomeNetwork = GetCheck(hChkAllowRemote_);
    const bool enableHttps = false;
    const bool enablePasswordAuth = enableServer && GetCheck(hChkEnablePasswordAuth_);

    EnableWindow(hEditBindAddress_, FALSE);
    EnableWindow(hEditHttpPort_, enableServer);
    EnableWindow(hChkAllowRemote_, enableServer);
    EnableWindow(hChkAutoStart_, enableServer);
    EnableWindow(hChkEnableHttps_, FALSE);
    EnableWindow(hEditRequestTimeout_, enableServer);
    EnableWindow(hChkEnableCors_, enableServer);
    EnableWindow(hChkOpenBrowser_, enableServer);

    EnableWindow(hEditHttpsPort_, enableHttps);
    EnableWindow(hEditPfxPath_, enableHttps);
    EnableWindow(hBtnBrowsePfx_, enableHttps);
    EnableWindow(hEditPfxPassword_, enableHttps);

    EnableWindow(hChkEnablePasswordAuth_, enableServer);
    EnableWindow(hEditLoginUser_, enablePasswordAuth);
    EnableWindow(hEditLoginPassword_, enablePasswordAuth);

    SetText(hEditBindAddress_, allowHomeNetwork ? L"0.0.0.0" : L"127.0.0.1");
}
