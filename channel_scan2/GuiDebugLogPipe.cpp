#include "GuiDebugLogPipe.hpp"

#include <CommCtrl.h>
#include <commdlg.h>
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Comdlg32.lib")

#include <filesystem>
#include <vector>
#include <algorithm>
#include <utility>
#include <cwchar>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace {

    static void SetDefaultGuiFont(HWND h, HFONT f)
    {
        if (!h || !f) return;
        SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
    }

    // C4312 対策：HMENU へのID変換は INT_PTR 経由に統一
    static HMENU CtrlId(int id) noexcept
    {
        return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
    }

    static std::wstring ToWideFromBytes_Utf8OrAcp(const std::vector<char>& bytes)
    {
        if (bytes.empty()) return L"";

        auto conv = [&](UINT cp, DWORD flags) -> std::wstring {
            const int need = MultiByteToWideChar(cp, flags, bytes.data(), (int)bytes.size(), nullptr, 0);
            if (need <= 0) return L"";

            std::wstring w;
            w.resize((size_t)need);
            MultiByteToWideChar(cp, flags, bytes.data(), (int)bytes.size(), w.data(), need);
            return w;
            };

        if (auto w = conv(CP_UTF8, MB_ERR_INVALID_CHARS); !w.empty()) return w;
        return conv(CP_ACP, 0);
    }

    static std::wstring GetTextW(HWND h)
    {
        if (!h) return L"";

        const int len = GetWindowTextLengthW(h);
        if (len <= 0) return L"";

        std::wstring s;
        s.resize((size_t)len + 1U);

        const int copied = GetWindowTextW(h, s.data(), len + 1);
        if (copied <= 0) return L"";

        s.resize((size_t)copied);
        return s;
    }

    static std::filesystem::path GetExeDir_()
    {
        wchar_t exePath[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            return std::filesystem::current_path();
        }

        return std::filesystem::path(exePath).parent_path();
    }

    static std::wstring ResolveFilePathForDialog_(const std::wstring& filePath)
    {
        if (filePath.empty()) {
            return L"";
        }

        std::error_code ec;
        std::filesystem::path p(filePath);

        if (p.is_absolute()) {
            return p.wstring();
        }

        const auto candidate = GetExeDir_() / p;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate.wstring();
        }

        return filePath;
    }

    static std::wstring MakeInitialLogDir_(const std::wstring& filePath)
    {
        std::error_code ec;

        const std::wstring resolved = ResolveFilePathForDialog_(filePath);
        if (!resolved.empty()) {
            std::filesystem::path p(resolved);
            const auto parent = p.parent_path();
            if (!parent.empty() && std::filesystem::exists(parent, ec)) {
                return parent.wstring();
            }
        }

        const auto exeDir = GetExeDir_();
        const auto debugDir = exeDir / L"debug";
        if (std::filesystem::exists(debugDir, ec)) {
            return debugDir.wstring();
        }

        return exeDir.wstring();
    }

} // namespace

// ---------------- ini path ----------------
std::wstring GuiDebugLogPipeTab::GetDefaultConfigIniPath_()
{
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path p(exePath);
    return (p.parent_path() / L"config.ini").wstring();
}

// ---------------- file tail ----------------
bool GuiDebugLogPipeTab::ReadNewTextFromFile_(
    const std::wstring& filePath,
    std::uint64_t& cursor,
    std::wstring& outText)
{
    outText.clear();
    if (filePath.empty()) return false;

    HANDLE h = CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        return false;
    }

    const std::uint64_t fileSize = (std::uint64_t)size.QuadPart;
    if (cursor > fileSize) cursor = 0;

    LARGE_INTEGER li{};
    li.QuadPart = (LONGLONG)cursor;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        CloseHandle(h);
        return false;
    }

    constexpr DWORD kMaxRead = 64 * 1024;
    std::vector<char> buf(kMaxRead);

    DWORD readBytes = 0;
    if (!ReadFile(h, buf.data(), kMaxRead, &readBytes, nullptr)) {
        CloseHandle(h);
        return false;
    }

    CloseHandle(h);

    if (readBytes == 0) return true;

    buf.resize(readBytes);
    cursor += readBytes;

    outText = ToWideFromBytes_Utf8OrAcp(buf);
    return true;
}

// ---------------- window proc ----------------
LRESULT CALLBACK GuiDebugLogPipeTab::WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    GuiDebugLogPipeTab* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<GuiDebugLogPipeTab*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)self);
        self->hPage_ = hWnd;
    }
    else {
        self = reinterpret_cast<GuiDebugLogPipeTab*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    return self ? self->HandleMessage(msg, wp, lp) : DefWindowProcW(hWnd, msg, wp, lp);
}

LRESULT GuiDebugLogPipeTab::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        OnCreate();
        return 0;

    case WM_SIZE:
        OnSize(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_COMMAND:
        OnCommand(LOWORD(wp));
        return 0;

    case WM_TIMER:
        if (wp == kTimerId) OnTimer();
        return 0;

    default:
        return DefWindowProcW(hPage_, msg, wp, lp);
    }
}

bool GuiDebugLogPipeTab::Create(
    HWND hParent, RECT rc, HINSTANCE hInst, HFONT hFont,
    std::function<GuiActions& ()> getActions,
    std::function<void(std::wstring_view)> onLog)
{
    hParent_ = hParent;
    hInst_ = hInst;
    hFont_ = hFont;
    getActions_ = std::move(getActions);
    log_ = std::move(onLog);

    const wchar_t* kCls = L"TSCEPG_DebugLogTab";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &GuiDebugLogPipeTab::WndProc;
    wc.hInstance = hInst_;
    wc.lpszClassName = kCls;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    WNDCLASSEXW tmp{};
    if (!GetClassInfoExW(hInst_, kCls, &tmp)) {
        RegisterClassExW(&wc);
    }

    hPage_ = CreateWindowExW(
        0, kCls, L"",
        WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
        hParent_, nullptr, hInst_, this);

    if (!hPage_) return false;

    LoadFromIni_();
    ApplyIniToControls_();
    SetTimer(hPage_, kTimerId, 200, nullptr);
    return true;
}

void GuiDebugLogPipeTab::Show(bool show)
{
    if (!hPage_) return;
    ShowWindow(hPage_, show ? SW_SHOW : SW_HIDE);
}

void GuiDebugLogPipeTab::Move(RECT rc)
{
    if (!hPage_) return;
    MoveWindow(hPage_, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
}

void GuiDebugLogPipeTab::OnCreate()
{
    hChkEnable_ = CreateWindowExW(
        0, L"BUTTON", L"デバッグログ表示を有効化",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        10, 10, 260, 24,
        hPage_, CtrlId(IDC_CHK_ENABLE), hInst_, nullptr);

    hBtnRunChScanDbg_ = CreateWindowExW(
        0, L"BUTTON", L"チャンネルスキャン（デバッグ用）",
        WS_CHILD | WS_VISIBLE,
        280, 10, 260, 24,
        hPage_, CtrlId(IDC_BTN_RUN_CHSCAN_DBG), hInst_, nullptr);

    CreateWindowExW(
        0, L"STATIC", L"ログファイル:",
        WS_CHILD | WS_VISIBLE,
        10, 44, 80, 20,
        hPage_, nullptr, hInst_, nullptr);

    hEdFile_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        90, 40, 360, 24,
        hPage_, CtrlId(IDC_ED_FILE), hInst_, nullptr);

    hBtnApply_ = CreateWindowExW(
        0, L"BUTTON", L"適用",
        WS_CHILD | WS_VISIBLE,
        460, 40, 70, 24,
        hPage_, CtrlId(IDC_BTN_APPLY), hInst_, nullptr);

    hBtnBrowse_ = CreateWindowExW(
        0, L"BUTTON", L"参照...",
        WS_CHILD | WS_VISIBLE,
        538, 40, 80, 24,
        hPage_, CtrlId(IDC_BTN_BROWSE), hInst_, nullptr);

    hBtnClear_ = CreateWindowExW(
        0, L"BUTTON", L"クリア",
        WS_CHILD | WS_VISIBLE,
        626, 40, 70, 24,
        hPage_, CtrlId(IDC_BTN_CLEAR), hInst_, nullptr);

    hLogEdit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        10, 74, 660, 300,
        hPage_, CtrlId(IDC_ED_LOG), hInst_, nullptr);

    // ログ表示が32KB付近で止まらないよう、表示側だけ上限を少し広げる。
    // 無制限にはせず、重いログでGUIが固まりにくい範囲に抑える。
    SendMessageW(hLogEdit_, EM_SETLIMITTEXT, static_cast<WPARAM>(4U * 1024U * 1024U), 0);

    SetDefaultGuiFont(hChkEnable_, hFont_);
    SetDefaultGuiFont(hBtnRunChScanDbg_, hFont_);
    SetDefaultGuiFont(hEdFile_, hFont_);
    SetDefaultGuiFont(hBtnApply_, hFont_);
    SetDefaultGuiFont(hBtnBrowse_, hFont_);
    SetDefaultGuiFont(hBtnClear_, hFont_);
    SetDefaultGuiFont(hLogEdit_, hFont_);

    ApplyIniToControls_();
}

void GuiDebugLogPipeTab::OnSize(int w, int h)
{
    const int pad = 10;
    const int topH = 74;

    const int btnH = 24;
    const int topY = 10;

    const int runW = 260;
    const int runX = (std::max)(pad, w - pad - runW);
    MoveWindow(hBtnRunChScanDbg_, runX, topY, runW, btnH, TRUE);

    const int chkW = (std::max)(120, runX - pad - pad);
    MoveWindow(hChkEnable_, pad, topY, chkW, btnH, TRUE);

    const int fileX = 90;
    const int btnY = 40;

    const int applyW = 70;
    const int browseW = 80;
    const int clearW = 70;
    const int gap = 8;
    const int buttonsW = applyW + browseW + clearW + gap * 3;

    const int fileW = (std::max)(120, w - pad - fileX - buttonsW);
    int x = fileX + fileW + gap;

    MoveWindow(hEdFile_, fileX, btnY, fileW, btnH, TRUE);

    MoveWindow(hBtnApply_, x, btnY, applyW, btnH, TRUE);
    x += applyW + gap;

    MoveWindow(hBtnBrowse_, x, btnY, browseW, btnH, TRUE);
    x += browseW + gap;

    MoveWindow(hBtnClear_, x, btnY, clearW, btnH, TRUE);

    const int logY = topH;
    const int logH = (std::max)(60, h - logY - pad);
    MoveWindow(hLogEdit_, pad, logY, (std::max)(100, w - pad * 2), logH, TRUE);
}

void GuiDebugLogPipeTab::OnCommand(int id)
{
    switch (id)
    {
    case IDC_CHK_ENABLE:
    {
        const LRESULT chk = SendMessageW(hChkEnable_, BM_GETCHECK, 0, 0);
        enabled_ = (chk == BST_CHECKED);
        SaveToIni_();
        if (log_) log_(enabled_ ? L"[DEBUG] GUI debug log enabled." : L"[DEBUG] GUI debug log disabled.");
        break;
    }

    case IDC_BTN_APPLY:
    {
        ApplyLogFilePath_(GetTextW(hEdFile_));
        break;
    }

    case IDC_BTN_BROWSE:
    {
        BrowseLogFile_();
        break;
    }

    case IDC_BTN_CLEAR:
        ClearText();
        break;

    case IDC_BTN_RUN_CHSCAN_DBG:
    {
        if (!getActions_) {
            if (log_) log_(L"[WARN] getActions_ が未接続です。");
            break;
        }

        GuiActions& act = getActions_();
        if (act.RunChannelScanDebugFilterTest) {
            if (log_) log_(L"[DEBUG] チャンネルスキャン（デバッグ用）開始要求");
            act.RunChannelScanDebugFilterTest();
            if (log_) log_(L"[DEBUG] チャンネルスキャン（デバッグ用）終了");
        }
        else {
            if (log_) log_(L"[WARN] RunChannelScanDebugFilterTest が未接続です。");
        }
        break;
    }

    default:
        break;
    }
}

void GuiDebugLogPipeTab::OnTimer()
{
    if (!enabled_) return;

    std::wstring text;
    if (ReadNewTextFromFile_(logFilePath_, fileCursor_, text)) {
        if (!text.empty()) {
            AppendText(text);
        }
    }
}

void GuiDebugLogPipeTab::AppendText(std::wstring_view s)
{
    if (!hLogEdit_) return;

    const int len = GetWindowTextLengthW(hLogEdit_);
    SendMessageW(hLogEdit_, EM_SETSEL, (WPARAM)len, (LPARAM)len);

    std::wstring line(s);
    SendMessageW(hLogEdit_, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
}

void GuiDebugLogPipeTab::ClearText()
{
    if (!hLogEdit_) return;
    SetWindowTextW(hLogEdit_, L"");
}

void GuiDebugLogPipeTab::BrowseLogFile_()
{
    std::wstring selected;
    if (!SelectLogFileByDialog_(selected)) {
        return;
    }

    ApplyLogFilePath_(selected);
}

void GuiDebugLogPipeTab::ApplyLogFilePath_(const std::wstring& filePath)
{
    if (filePath.empty()) {
        if (log_) log_(L"[WARN] ログファイルパスが空です。");
        return;
    }

    logFilePath_ = filePath;
    fileCursor_ = 0;

    if (hEdFile_) {
        SetWindowTextW(hEdFile_, logFilePath_.c_str());
    }

    ClearText();
    SaveToIni_();

    if (log_) {
        std::wstring msg = L"[DEBUG] log file selected: ";
        msg += logFilePath_;
        log_(msg);
    }
}

bool GuiDebugLogPipeTab::SelectLogFileByDialog_(std::wstring& outPath) const
{
    outPath.clear();

    std::vector<wchar_t> fileBuf(32768, L'\0');

    const std::wstring currentPath = ResolveFilePathForDialog_(logFilePath_);
    if (!currentPath.empty()) {
        wcsncpy_s(fileBuf.data(), fileBuf.size(), currentPath.c_str(), _TRUNCATE);
    }

    const std::wstring initialDir = MakeInitialLogDir_(logFilePath_);

    static const wchar_t kFilter[] =
        L"ログファイル (*.log;*.txt)\0*.log;*.txt\0"
        L"すべてのファイル (*.*)\0*.*\0";

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hPage_;
    ofn.hInstance = hInst_;
    ofn.lpstrFilter = kFilter;
    ofn.lpstrFile = fileBuf.data();
    ofn.nMaxFile = static_cast<DWORD>(fileBuf.size());
    ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
    ofn.lpstrTitle = L"表示するログファイルを選択";
    ofn.Flags =
        OFN_FILEMUSTEXIST |
        OFN_PATHMUSTEXIST |
        OFN_HIDEREADONLY |
        OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) {
        return false;
    }

    outPath = fileBuf.data();
    return !outPath.empty();
}

void GuiDebugLogPipeTab::LoadFromIni_()
{
    const std::wstring ini = GetDefaultConfigIniPath_();

    enabled_ = (GetPrivateProfileIntW(L"Debug", L"GuiLogEnabled", 1, ini.c_str()) != 0);

    wchar_t buf[MAX_PATH]{};
    GetPrivateProfileStringW(L"Debug", L"GuiLogFile", L"debug.log", buf, (DWORD)_countof(buf), ini.c_str());
    logFilePath_ = buf;
}

void GuiDebugLogPipeTab::ApplyIniToControls_()
{
    if (hChkEnable_) {
        SendMessageW(hChkEnable_, BM_SETCHECK, enabled_ ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (hEdFile_) {
        SetWindowTextW(hEdFile_, logFilePath_.c_str());
    }
}

void GuiDebugLogPipeTab::ReloadFromConfig()
{
    LoadFromIni_();
    ApplyIniToControls_();
    fileCursor_ = 0;
}

void GuiDebugLogPipeTab::SaveToIni_() const
{
    const std::wstring ini = GetDefaultConfigIniPath_();
    WritePrivateProfileStringW(L"Debug", L"GuiLogEnabled", enabled_ ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Debug", L"GuiLogFile", logFilePath_.c_str(), ini.c_str());
}

void GuiDebugLogPipeTab::ApplyToConfig()
{
    SaveToIni_();
}
