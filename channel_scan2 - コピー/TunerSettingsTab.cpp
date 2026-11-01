#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "TunerSettingsTab.hpp"
#include "ConfigManager.h"
#include "DebugManager.h"
#include "TVTestIntegration.hpp"
#include "ReservationTvTestWatchTest.hpp"

#include <Windows.h>
#include <CommCtrl.h>
#include <CommDlg.h>
#include <Shellapi.h>

#include <algorithm>
#include <cstdarg>
#include <cwchar>
#include <filesystem>
#include <string>
#include <system_error>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>
#include <iterator>


#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Shell32.lib")

namespace
{
    constexpr wchar_t kTunerSettingsTabClassName[] = L"TSCEPG_TunerSettingsTab";
    constexpr wchar_t kTunerSettingsPageClassName[] = L"TSCEPG_TunerSettingsPage";

    constexpr wchar_t kDefaultTVTestControlPipe[] = L"\\\\.\\pipe\\TVTestBridge";
    constexpr wchar_t kDefaultTVTestTsPipe[] = L"\\\\.\\pipe\\TVTestBridgeTS";

    constexpr int IDC_TUNER_TAB = 6100;
    constexpr int IDC_TUNER_SUBTAB = 6101;

    constexpr int IDC_TUNER_CHK_ENABLED = 6110;
    constexpr int IDC_TUNER_ED_NAME = 6111;
    constexpr int IDC_TUNER_ED_BON = 6112;
    constexpr int IDC_TUNER_CB_RECEIVE = 6113;
    constexpr int IDC_TUNER_BTN_BROWSE_BON = 6114;
    constexpr int IDC_TUNER_ED_INTERNAL_BON = 6115;
    constexpr int IDC_TUNER_BTN_BROWSE_INTERNAL_BON = 6116;

    


    constexpr int IDC_TUNER_CB_ROLE = 6120;
    constexpr int IDC_TUNER_CHK_PARALLEL = 6121;
    constexpr int IDC_TUNER_ED_PRIORITY = 6122;
    constexpr int IDC_TUNER_CB_TIMESHIFT_LOCK_CHANNEL = 6123;
    constexpr int IDC_TUNER_BTN_TIMESHIFT_RELOAD_CHANNELS = 6124;

    constexpr int IDC_TUNER_CHK_EXTAPP = 6130;
    constexpr int IDC_TUNER_ED_EXTAPP = 6131;
    constexpr int IDC_TUNER_BTN_BROWSE_EXTAPP = 6132;
    constexpr int IDC_TUNER_BTN_TEST_EXTAPP = 6133;

    constexpr int IDC_TUNER_CHK_TVTEST = 6140;
    constexpr int IDC_TUNER_ED_TVTEST_EXE = 6141;
    constexpr int IDC_TUNER_BTN_BROWSE_TVTEST_EXE = 6142;
    constexpr int IDC_TUNER_ED_TVTEST_CTRLPIPE = 6143;
    constexpr int IDC_TUNER_ED_TVTEST_TSPIPE = 6144;
    constexpr int IDC_TUNER_BTN_TVTEST_LAUNCH = 6145;
    constexpr int IDC_TUNER_BTN_TVTEST_PING = 6146;
    constexpr int IDC_TUNER_BTN_TVTEST_STATE = 6147;
    constexpr int IDC_TUNER_BTN_TVTEST_PROGRAM = 6148;
    constexpr int IDC_TUNER_BTN_TVTEST_ENABLE_TS = 6149;
    constexpr int IDC_TUNER_BTN_TVTEST_DISABLE_TS = 6150;
    constexpr int IDC_TUNER_BTN_TVTEST_RESET_PIPES = 6151;
    constexpr int IDC_TUNER_BTN_TVTEST_STATUS_DETAIL = 6152;
    constexpr int IDC_TUNER_BTN_TVTEST_RELOAD_INI = 6153;
    constexpr int IDC_TUNER_TVTEST_TESTTAB = 6154;
    constexpr int IDC_TUNER_ED_TVTEST_BON = 6155;
    constexpr int IDC_TUNER_BTN_BROWSE_TVTEST_BON = 6156;


    constexpr int IDC_TUNER_CB_TVTEST_CHANNEL_LIST = 6180;
    constexpr int IDC_TUNER_BTN_TVTEST_RELOAD_LIST = 6181;
    constexpr int IDC_TUNER_BTN_TVTEST_SET_CHANNEL = 6183;
    constexpr int IDC_TUNER_BTN_TVTEST_TS_READ = 6184;
    constexpr int IDC_TUNER_BTN_TVTEST_WATCH_FLOW_TEST = 6185;


    constexpr int kOuterMargin = 6;
    constexpr int kTunerTabHeight = 30;
    constexpr int kGapBetweenTabs = 6;

    constexpr int kLabelX = 16;
    constexpr int kLabelW = 120;
    constexpr int kCtrlX = 140;
    constexpr int kCtrlH = 24;
    constexpr int kBrowseButtonW = 84;
    constexpr int kTestButtonW = 120;
    constexpr int kSmallButtonW = 92;
    constexpr int kCtrlGap = 8;
    constexpr int kMinEditW = 180;
    constexpr int kRightPadding = 16;

    HMENU CtrlId(int id) noexcept
    {
        return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
    }

    void SetFontIf(HWND hWnd, HFONT hFont)
    {
        if (hWnd != nullptr && hFont != nullptr) {
            SendMessageW(hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        }
    }

    void SetChecked(HWND hWnd, bool checked)
    {
        if (hWnd != nullptr) {
            SendMessageW(hWnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        }
    }

    bool IsChecked(HWND hWnd)
    {
        if (hWnd == nullptr) {
            return false;
        }
        return SendMessageW(hWnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    void SetEditInt(HWND hEdit, int value)
    {
        if (hEdit == nullptr) {
            return;
        }

        wchar_t buf[64]{};
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d", value);
        SetWindowTextW(hEdit, buf);
    }

    int GetEditInt(HWND hEdit, int defaultValue)
    {
        if (hEdit == nullptr) {
            return defaultValue;
        }

        wchar_t buf[64]{};
        GetWindowTextW(hEdit, buf, static_cast<int>(_countof(buf)));

        wchar_t* endPtr = nullptr;
        const long value = std::wcstol(buf, &endPtr, 10);
        if (endPtr == buf) {
            return defaultValue;
        }
        return static_cast<int>(value);
    }

    void SetComboSelSafe(HWND hCombo, int index)
    {
        if (hCombo == nullptr) {
            return;
        }

        const int count = static_cast<int>(SendMessageW(hCombo, CB_GETCOUNT, 0, 0));
        if (count <= 0) {
            return;
        }

        if (index < 0 || index >= count) {
            index = 0;
        }

        SendMessageW(hCombo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
    }

    void ReportCreateWindowError(HWND hOwner, const wchar_t* what)
    {
        const DWORD errorCode = GetLastError();
        wchar_t message[256]{};
        _snwprintf_s(
            message,
            _countof(message),
            _TRUNCATE,
            L"%s の生成に失敗しました。(GetLastError=%lu)",
            what,
            static_cast<unsigned long>(errorCode));

        OutputDebugStringW(message);
        OutputDebugStringW(L"\n");

        if (hOwner != nullptr) {
            MessageBoxW(hOwner, message, L"TunerSettingsTab", MB_OK | MB_ICONERROR);
        }
    }

    void CreateStaticLabel(
        HWND hParent,
        HINSTANCE hInst,
        HFONT hFont,
        int x,
        int y,
        int width,
        int height,
        const wchar_t* text)
    {
        HWND hLabel = CreateWindowExW(
            0,
            L"STATIC",
            text,
            WS_CHILD | WS_VISIBLE,
            x,
            y,
            width,
            height,
            hParent,
            nullptr,
            hInst,
            nullptr);

        SetFontIf(hLabel, hFont);
    }

    void TunerDebug(const wchar_t* format, ...)
    {
        wchar_t buffer[1024]{};

        va_list args;
        va_start(args, format);
        _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, args);
        va_end(args);

        DebugLogFormat(L"[TunerSettingsTab] %ls", buffer);
    }

    std::wstring GetWindowTextString(HWND hWnd)
    {
        if (hWnd == nullptr) {
            return {};
        }

        const int len = GetWindowTextLengthW(hWnd);
        if (len <= 0) {
            return {};
        }

        std::wstring text(static_cast<size_t>(len) + 1, L'\0');
        const int copied = GetWindowTextW(hWnd, text.data(), len + 1);
        if (copied < 0) {
            return {};
        }
        text.resize(static_cast<size_t>(copied));
        return text;
    }

    std::wstring TrimAndUnquote(std::wstring text)
    {
        const size_t first = text.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) {
            return {};
        }

        const size_t last = text.find_last_not_of(L" \t\r\n");
        text = text.substr(first, last - first + 1);

        if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"') {
            text = text.substr(1, text.size() - 2);
        }

        return text;
    }

    std::filesystem::path GetModuleDirectoryPath()
    {
        wchar_t modulePath[MAX_PATH]{};
        const DWORD len = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(_countof(modulePath)));
        if (len == 0 || len >= _countof(modulePath)) {
            return {};
        }
        return std::filesystem::path(modulePath).parent_path();
    }

    std::string WideToUtf8(const std::wstring& src)
    {
        if (src.empty()) {
            return {};
        }

        const int need = WideCharToMultiByte(
            CP_UTF8,
            0,
            src.c_str(),
            static_cast<int>(src.size()),
            nullptr,
            0,
            nullptr,
            nullptr);

        if (need <= 0) {
            return {};
        }

        std::string out(static_cast<size_t>(need), '\0');
        const int written = WideCharToMultiByte(
            CP_UTF8,
            0,
            src.c_str(),
            static_cast<int>(src.size()),
            out.data(),
            need,
            nullptr,
            nullptr);

        if (written != need) {
            return {};
        }

        return out;
    }

    std::wstring Utf8ToWide(const char* src, int len)
    {
        if (src == nullptr || len <= 0) {
            return {};
        }

        const int need = MultiByteToWideChar(CP_UTF8, 0, src, len, nullptr, 0);
        if (need <= 0) {
            return {};
        }

        std::wstring out(static_cast<size_t>(need), L'\0');
        const int written = MultiByteToWideChar(CP_UTF8, 0, src, len, out.data(), need);
        if (written != need) {
            return {};
        }

        return out;
    }

    std::wstring TrimCopy_(std::wstring s)
    {
        auto is_ws = [](wchar_t c) {
            return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
            };

        while (!s.empty() && is_ws(s.front())) {
            s.erase(s.begin());
        }
        while (!s.empty() && is_ws(s.back())) {
            s.pop_back();
        }
        return s;
    }

    bool TryParseInt_(const std::wstring& text, int& out)
    {
        const std::wstring t = TrimCopy_(text);
        if (t.empty()) {
            return false;
        }

        wchar_t* endPtr = nullptr;
        const long v = std::wcstol(t.c_str(), &endPtr, 10);
        if (endPtr == t.c_str()) {
            return false;
        }

        out = static_cast<int>(v);
        return true;
    }

    std::vector<std::wstring> SplitCsvLine_(const std::wstring& line)
    {
        std::vector<std::wstring> cols;
        std::wstring current;
        bool inQuotes = false;

        for (size_t i = 0; i < line.size(); ++i) {
            const wchar_t ch = line[i];

            if (ch == L'"') {
                if (inQuotes && i + 1 < line.size() && line[i + 1] == L'"') {
                    current.push_back(L'"');
                    ++i;
                }
                else {
                    inQuotes = !inQuotes;
                }
                continue;
            }

            if (ch == L',' && !inQuotes) {
                cols.push_back(current);
                current.clear();
                continue;
            }

            current.push_back(ch);
        }

        cols.push_back(current);
        return cols;
    }

    std::wstring CodePageToWide_(const std::string& src, UINT codePage)
    {
        if (src.empty()) {
            return {};
        }

        const int need = MultiByteToWideChar(
            codePage,
            0,
            src.data(),
            static_cast<int>(src.size()),
            nullptr,
            0);

        if (need <= 0) {
            return {};
        }

        std::wstring out(static_cast<size_t>(need), L'\0');
        const int written = MultiByteToWideChar(
            codePage,
            0,
            src.data(),
            static_cast<int>(src.size()),
            out.data(),
            need);

        if (written != need) {
            return {};
        }

        return out;
    }

    std::wstring Utf8ToWideStrict_(const char* src, int len)
    {
        if (src == nullptr || len <= 0) {
            return {};
        }

        const int need = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            src,
            len,
            nullptr,
            0);

        if (need <= 0) {
            return {};
        }

        std::wstring out(static_cast<size_t>(need), L'\0');
        const int written = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            src,
            len,
            out.data(),
            need);

        if (written != need) {
            return {};
        }

        return out;
    }

    std::wstring DecodeTextFileToWide_(const std::vector<unsigned char>& bytes)
    {
        if (bytes.empty()) {
            return {};
        }

        // UTF-16 LE BOM
        if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
            std::wstring out;
            const size_t wcharCount = (bytes.size() - 2) / 2;
            out.reserve(wcharCount);

            for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
                const wchar_t ch =
                    static_cast<wchar_t>(
                        static_cast<unsigned short>(bytes[i]) |
                        (static_cast<unsigned short>(bytes[i + 1]) << 8));
                out.push_back(ch);
            }
            return out;
        }

        // UTF-8 BOM
        if (bytes.size() >= 3 &&
            bytes[0] == 0xEF &&
            bytes[1] == 0xBB &&
            bytes[2] == 0xBF) {
            return Utf8ToWideStrict_(
                reinterpret_cast<const char*>(bytes.data() + 3),
                static_cast<int>(bytes.size() - 3));
        }

        const char* raw = reinterpret_cast<const char*>(bytes.data());
        const int rawLen = static_cast<int>(bytes.size());

        // BOMなしUTF-8は「厳密に」判定
        if (std::wstring utf8 = Utf8ToWideStrict_(raw, rawLen); !utf8.empty()) {
            return utf8;
        }

        // 日本語環境向けに CP932 を優先
        if (std::wstring cp932 = CodePageToWide_(std::string(raw, raw + rawLen), 932); !cp932.empty()) {
            return cp932;
        }

        // 最後の保険
        return CodePageToWide_(std::string(raw, raw + rawLen), CP_ACP);
    }

    std::filesystem::path BuildTVTestCh2Path_(const std::wstring& tvtestExePath,
        const std::wstring& bonDriverPath)
    {
        const std::wstring exe = TrimAndUnquote(tvtestExePath);
        const std::wstring bon = TrimAndUnquote(bonDriverPath);

        if (exe.empty() || bon.empty()) {
            return {};
        }

        std::filesystem::path tvtestDir = std::filesystem::path(exe).parent_path();
        std::filesystem::path bonName = std::filesystem::path(bon).filename();
        bonName.replace_extension(L".ch2");

        return tvtestDir / bonName;
    }

    std::filesystem::path GetExeDirectoryPathForTunerSettings_()
    {
        wchar_t modulePath[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);

        if (n == 0 || n >= MAX_PATH) {
            return std::filesystem::current_path();
        }

        return std::filesystem::path(modulePath).parent_path();
    }

    std::filesystem::path BuildChannelListPathForSpace_(int space)
    {
        return GetExeDirectoryPathForTunerSettings_() /
            (L"channellist_space" + std::to_wstring(space) + L".ch");
    }

    bool FileExists_(const std::filesystem::path& path)
    {
        std::error_code ec;
        return std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec);
    }

    std::wstring MakeTimeShiftChannelDisplayName_(
        const std::wstring& serviceName,
        int space,
        int channel,
        std::uint16_t serviceId,
        std::uint16_t networkId,
        std::uint16_t tsId)
    {
        std::wstring s = serviceName.empty() ? L"(no service)" : serviceName;

        s += L"  [Space=";
        s += std::to_wstring(space);
        s += L" Ch=";
        s += std::to_wstring(channel);
        s += L" SID=";
        s += std::to_wstring(static_cast<unsigned>(serviceId));
        s += L" NID=";
        s += std::to_wstring(static_cast<unsigned>(networkId));
        s += L" TSID=";
        s += std::to_wstring(static_cast<unsigned>(tsId));
        s += L"]";

        return s;
    }

    LRESULT CALLBACK TunerSettingsPageProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        HWND hOwner = reinterpret_cast<HWND>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            hOwner = reinterpret_cast<HWND>(cs->lpCreateParams);
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(hOwner));
        }

        switch (msg) {
        case WM_COMMAND:
        case WM_NOTIFY:
            if (hOwner != nullptr) {
                return SendMessageW(hOwner, msg, wp, lp);
            }
            break;
        default:
            break;
        }

        return DefWindowProcW(hWnd, msg, wp, lp);
    }

    bool RegisterPageWindowClass(HINSTANCE hInst)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = TunerSettingsPageProc;
        wc.hInstance = hInst;
        wc.lpszClassName = kTunerSettingsPageClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);

        if (!RegisterClassExW(&wc)) {
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                return false;
            }
        }
        return true;
    }

    std::wstring MakeDefaultTVTestControlPipeForTuner_(int tunerIndex)
    {
        if (tunerIndex < 0) {
            tunerIndex = 0;
        }

        return L"\\\\.\\pipe\\TVTestBridge_" + std::to_wstring(tunerIndex);
    }

    std::wstring MakeDefaultTVTestTsPipeForTuner_(int tunerIndex)
    {
        if (tunerIndex < 0) {
            tunerIndex = 0;
        }

        return L"\\\\.\\pipe\\TVTestBridgeTS_" + std::to_wstring(tunerIndex);
    }
}

bool TunerSettingsTab::Create(HWND hParent, const RECT& rc, HINSTANCE hInst, HFONT hFont, ConfigManager& cfg)
{
    hInst_ = hInst;
    hFont_ = hFont;
    cfg_ = &cfg;

    tunerCount_ = cfg.GetTunersCount();
    if (tunerCount_ < 1) {
        tunerCount_ = 1;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TunerSettingsTab::WndProc;
    wc.hInstance = hInst_;
    wc.lpszClassName = kTunerSettingsTabClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);

    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
    }

    if (!RegisterPageWindowClass(hInst_)) {
        return false;
    }

    hWnd_ = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kTunerSettingsTabClassName,
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

    return hWnd_ != nullptr;
}

void TunerSettingsTab::Show(bool show)
{
    if (hWnd_ != nullptr) {
        ShowWindow(hWnd_, show ? SW_SHOW : SW_HIDE);
    }
}

void TunerSettingsTab::Move(const RECT& rc)
{
    if (hWnd_ != nullptr) {
        MoveWindow(hWnd_, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
    }
}

void TunerSettingsTab::ApplyToConfig()
{
    SavePageToConfig(loadedTunerIndex_);
    UpdateControlEnableState();
    RedrawActiveSubPage();
}

void TunerSettingsTab::ReloadFromConfig()
{
    if (cfg_ == nullptr) {
        return;
    }

    cfg_->LoadConfig();

    tunerCount_ = cfg_->GetTunersCount();
    if (tunerCount_ < 1) {
        tunerCount_ = 1;
    }

    RebuildTunerTabs();
    RebuildSubTabs();
    RebuildTVTestTestTabs();

    loadedTunerIndex_ = GetCurrentTunerIndex();
    LoadPageFromConfig(loadedTunerIndex_);
    ShowSelectedSubPage();
    ShowSelectedTVTestTestPage();

    RECT rc{};
    GetClientRect(hWnd_, &rc);
    LayoutChildren(rc.right - rc.left, rc.bottom - rc.top);
    RedrawActiveSubPage();
}

void TunerSettingsTab::UpdateTunerCount(int tunerCount)
{
    SavePageToConfig(loadedTunerIndex_);

    if (tunerCount < 1) {
        tunerCount = 1;
    }
    tunerCount_ = tunerCount;

    RebuildTunerTabs();
    RebuildSubTabs();
    RebuildTVTestTestTabs();

    loadedTunerIndex_ = GetCurrentTunerIndex();
    LoadPageFromConfig(loadedTunerIndex_);
    ShowSelectedSubPage();
    ShowSelectedTVTestTestPage();

    RECT rc{};
    GetClientRect(hWnd_, &rc);
    LayoutChildren(rc.right - rc.left, rc.bottom - rc.top);
    RedrawActiveSubPage();
}

LRESULT CALLBACK TunerSettingsTab::WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    TunerSettingsTab* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<TunerSettingsTab*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->hWnd_ = hWnd;
        }
    }
    else {
        self = reinterpret_cast<TunerSettingsTab*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->HandleMessage(msg, wp, lp);
    }

    return DefWindowProcW(hWnd, msg, wp, lp);
}

LRESULT TunerSettingsTab::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        OnCreate();
        return 0;

    case WM_NOTIFY:
    {
        const auto* notifyHeader = reinterpret_cast<const NMHDR*>(lp);
        if (notifyHeader == nullptr) {
            break;
        }

        if (notifyHeader->hwndFrom == hTunerTab_) {
            if (notifyHeader->code == TCN_SELCHANGING) {
                SavePageToConfig(loadedTunerIndex_);
                return FALSE;
            }
            if (notifyHeader->code == TCN_SELCHANGE) {
                loadedTunerIndex_ = GetCurrentTunerIndex();
                ShowSelectedTunerPage();
                return 0;
            }
        }

        if (notifyHeader->hwndFrom == hSubTab_) {
            if (notifyHeader->code == TCN_SELCHANGE) {
                ShowSelectedSubPage();
                return 0;
            }
        }

        if (notifyHeader->hwndFrom == hTVTestTestTab_) {
            if (notifyHeader->code == TCN_SELCHANGE) {
                ShowSelectedTVTestTestPage();
                return 0;
            }
        }
        break;
    }

    case WM_COMMAND:
    {
        const int ctrlId = LOWORD(wp);
        const int notifyCode = HIWORD(wp);

        if (notifyCode == BN_CLICKED) {
            if (ctrlId == IDC_TUNER_CHK_EXTAPP || ctrlId == IDC_TUNER_CHK_TVTEST) {
                UpdateControlEnableState();
                RedrawActiveSubPage();
                return 0;
            }
            if (ctrlId == IDC_TUNER_BTN_BROWSE_BON) {
                BrowseBonDriverPath();
                RedrawActiveSubPage();
                return 0;
            }
            if (ctrlId == IDC_TUNER_BTN_BROWSE_EXTAPP) {
                BrowseExternalAppPath();
                RedrawActiveSubPage();
                return 0;
            }
            if (ctrlId == IDC_TUNER_BTN_TEST_EXTAPP) {
                RunExternalAppTest();
                return 0;
            }
            if (ctrlId == IDC_TUNER_BTN_BROWSE_TVTEST_EXE) {
                BrowseTVTestExePath();
                RedrawActiveSubPage();
                return 0;
            }
            if (ctrlId == IDC_TUNER_BTN_TVTEST_RESET_PIPES) {
                ResetTVTestPipePathsToDefault();
                RedrawActiveSubPage();
                return 0;
            }


            if (ctrlId == IDC_TUNER_BTN_TVTEST_LAUNCH) {
                RunTVTestLaunchTest();
                return 0;
            }
            if (ctrlId == IDC_TUNER_BTN_TVTEST_PING) {
                RunTVTestPingTest();
                return 0;
            }
            if (ctrlId == IDC_TUNER_BTN_TVTEST_STATE) {
                RunTVTestStateTest();
                return 0;
            }
            if (ctrlId == IDC_TUNER_BTN_TVTEST_PROGRAM) {
                RunTVTestProgramTest();
                return 0;
            }
            if (ctrlId == IDC_TUNER_BTN_TVTEST_STATUS_DETAIL) {
                RunTVTestStatusDetailTest();
                return 0;
            }
            if (ctrlId == IDC_TUNER_BTN_TVTEST_RELOAD_INI) {
                RunTVTestReloadIniTest();
                return 0;
            }
            if (ctrlId == IDC_TUNER_BTN_TVTEST_ENABLE_TS) {
                RunTVTestEnableTs();
                return 0;
            }
            if (ctrlId == IDC_TUNER_BTN_TVTEST_DISABLE_TS) {
                RunTVTestDisableTs();
                return 0;
            }

            if (ctrlId == IDC_TUNER_BTN_TVTEST_SET_CHANNEL) {
                RunTVTestSetChannelTest();
                return 0;
            }
            if (ctrlId == IDC_TUNER_BTN_TVTEST_RELOAD_LIST) {
                ReloadTVTestChannelList();
                RedrawActiveSubPage();
                return 0;
            }
            if (ctrlId == IDC_TUNER_BTN_TVTEST_TS_READ) {
                RunTVTestTsReadTest();
                return 0;
            }
            if (ctrlId == IDC_TUNER_BTN_TVTEST_WATCH_FLOW_TEST) {
                RunTVTestWatchReservationFlowTest();
                return 0;
            }
            if (ctrlId == IDC_TUNER_BTN_BROWSE_INTERNAL_BON) {
                BrowseInternalBonDriverPath();
                RedrawActiveSubPage();
                return 0;
            }
            if (ctrlId == IDC_TUNER_BTN_BROWSE_TVTEST_BON) {
                BrowseTVTestBonDriverPath();
                RedrawActiveSubPage();
                return 0;
            }
            if (ctrlId == IDC_TUNER_BTN_TIMESHIFT_RELOAD_CHANNELS) {
                ReloadTimeShiftChannelList();
                SelectTimeShiftLockChannelFromConfig(loadedTunerIndex_);
                UpdateControlEnableState();
                RedrawActiveSubPage();
                return 0;
            }
        }
        if (notifyCode == CBN_SELCHANGE) {
            if (ctrlId == IDC_TUNER_CB_ROLE) {
                UpdateControlEnableState();
                RedrawActiveSubPage();
                return 0;
            }
        }
        break;
    }

    case WM_SIZE:
    {
        const int width = LOWORD(lp);
        const int height = HIWORD(lp);
        LayoutChildren(width, height);
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hWnd_, msg, wp, lp);
}

void TunerSettingsTab::OnCreate()
{
    TunerDebug(L"OnCreate begin");

    hTunerTab_ = CreateWindowExW(
        0,
        WC_TABCONTROLW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
        0, 0, 100, 100,
        hWnd_,
        CtrlId(IDC_TUNER_TAB),
        hInst_,
        nullptr);

    if (hTunerTab_ == nullptr) {
        ReportCreateWindowError(hWnd_, L"親タブ(hTunerTab_)");
        return;
    }

    hSubTab_ = CreateWindowExW(
        0,
        WC_TABCONTROLW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
        0, 0, 100, 100,
        hWnd_,
        CtrlId(IDC_TUNER_SUBTAB),
        hInst_,
        nullptr);

    if (hSubTab_ == nullptr) {
        ReportCreateWindowError(hWnd_, L"小タブ(hSubTab_)");
        return;
    }

    hBasicPage_ = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kTunerSettingsPageClassName,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 100, 100,
        hSubTab_,
        nullptr,
        hInst_,
        hWnd_);

    if (hBasicPage_ == nullptr) {
        ReportCreateWindowError(hWnd_, L"基本ページ(hBasicPage_)");
        return;
    }

    hOperationPage_ = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kTunerSettingsPageClassName,
        L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 100, 100,
        hSubTab_,
        nullptr,
        hInst_,
        hWnd_);

    if (hOperationPage_ == nullptr) {
        ReportCreateWindowError(hWnd_, L"運用ページ(hOperationPage_)");
        return;
    }

    hExternalPage_ = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kTunerSettingsPageClassName,
        L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 100, 100,
        hSubTab_,
        nullptr,
        hInst_,
        hWnd_);

    if (hExternalPage_ == nullptr) {
        ReportCreateWindowError(hWnd_, L"外部連携ページ(hExternalPage_)");
        return;
    }

    hTVTestPage_ = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kTunerSettingsPageClassName,
        L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 100, 100,
        hSubTab_,
        nullptr,
        hInst_,
        hWnd_);

    if (hTVTestPage_ == nullptr) {
        ReportCreateWindowError(hWnd_, L"TVTestページ(hTVTestPage_)");
        return;
    }

    hChkEnabled_ = CreateWindowExW(
        0, L"BUTTON", L"有効",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        16, 16, 120, 24,
        hBasicPage_, CtrlId(IDC_TUNER_CHK_ENABLED), hInst_, nullptr);

    CreateStaticLabel(hBasicPage_, hInst_, hFont_, 16, 52, kLabelW, 20, L"チューナー名：");
    hEdName_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        140, 48, 280, 24,
        hBasicPage_, CtrlId(IDC_TUNER_ED_NAME), hInst_, nullptr);

    CreateStaticLabel(hBasicPage_, hInst_, hFont_, 16, 84, kLabelW, 20, L"内部BonDriver：");
    hEdInternalBonDriver_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        140, 80, 420, 24,
        hBasicPage_, CtrlId(IDC_TUNER_ED_INTERNAL_BON), hInst_, nullptr);

    hBtnBrowseInternalBonDriver_ = CreateWindowExW(
        0, L"BUTTON", L"参照...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        568, 80, kBrowseButtonW, kCtrlH,
        hBasicPage_, CtrlId(IDC_TUNER_BTN_BROWSE_INTERNAL_BON), hInst_, nullptr);

    // 旧メンバー互換。既存関数がまだ hEdBonDriver_ を参照していても、
    // 内部BonDriver欄を指すようにしておく。
    hEdBonDriver_ = hEdInternalBonDriver_;
    hBtnBrowseBonDriver_ = hBtnBrowseInternalBonDriver_;

    CreateStaticLabel(hBasicPage_, hInst_, hFont_, 16, 116, kLabelW, 20, L"受信方式：");
    hCbReceiveMode_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        140, 112, 180, 180,
        hBasicPage_, CtrlId(IDC_TUNER_CB_RECEIVE), hInst_, nullptr);

    SendMessageW(hCbReceiveMode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"汎用"));
    SendMessageW(hCbReceiveMode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"地上波"));
    SendMessageW(hCbReceiveMode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"BS"));
    SendMessageW(hCbReceiveMode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"CS"));

    CreateStaticLabel(hOperationPage_, hInst_, hFont_, 16, 20, kLabelW, 20, L"用途：");
    hCbUseRole_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        140, 16, 220, 220,
        hOperationPage_, CtrlId(IDC_TUNER_CB_ROLE), hInst_, nullptr);

    SendMessageW(hCbUseRole_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"汎用"));
    SendMessageW(hCbUseRole_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"録画専用"));
    SendMessageW(hCbUseRole_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"EPG専用"));
    SendMessageW(hCbUseRole_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"スキャン専用"));
    SendMessageW(hCbUseRole_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"視聴専用"));
    SendMessageW(hCbUseRole_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"タイムシフト専用"));

    hChkAllowParallel_ = CreateWindowExW(
        0, L"BUTTON", L"並列使用を許可",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        16, 56, 180, 24,
        hOperationPage_, CtrlId(IDC_TUNER_CHK_PARALLEL), hInst_, nullptr);

    CreateStaticLabel(hOperationPage_, hInst_, hFont_, 16, 92, kLabelW, 20, L"優先度：");
    hEdPriority_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
        140, 88, 80, 24,
        hOperationPage_, CtrlId(IDC_TUNER_ED_PRIORITY), hInst_, nullptr);

    hLblTimeShiftLockChannel_ = CreateWindowExW(
        0,
        L"STATIC",
        L"タイムシフト固定ch：",
        WS_CHILD | WS_VISIBLE,
        16,
        128,
        140,
        20,
        hOperationPage_,
        nullptr,
        hInst_,
        nullptr);

    hCbTimeShiftLockChannel_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"COMBOBOX",
        L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        140,
        124,
        420,
        240,
        hOperationPage_,
        CtrlId(IDC_TUNER_CB_TIMESHIFT_LOCK_CHANNEL),
        hInst_,
        nullptr);

    hBtnTimeShiftReloadChannels_ = CreateWindowExW(
        0,
        L"BUTTON",
        L"一覧更新",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        568,
        124,
        kBrowseButtonW,
        kCtrlH,
        hOperationPage_,
        CtrlId(IDC_TUNER_BTN_TIMESHIFT_RELOAD_CHANNELS),
        hInst_,
        nullptr);

    hChkUseExternalApp_ = CreateWindowExW(
        0, L"BUTTON", L"外部アプリを使用",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        16, 20, 180, 24,
        hExternalPage_, CtrlId(IDC_TUNER_CHK_EXTAPP), hInst_, nullptr);

    CreateStaticLabel(hExternalPage_, hInst_, hFont_, 16, 56, kLabelW, 20, L"外部アプリパス：");
    hEdExternalApp_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        140, 52, 420, 24,
        hExternalPage_, CtrlId(IDC_TUNER_ED_EXTAPP), hInst_, nullptr);

    hBtnBrowseExternalApp_ = CreateWindowExW(
        0, L"BUTTON", L"参照...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        568, 52, kBrowseButtonW, kCtrlH,
        hExternalPage_, CtrlId(IDC_TUNER_BTN_BROWSE_EXTAPP), hInst_, nullptr);

    hBtnTestExternalApp_ = CreateWindowExW(
        0, L"BUTTON", L"起動テスト",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        140, 84, kTestButtonW, kCtrlH,
        hExternalPage_, CtrlId(IDC_TUNER_BTN_TEST_EXTAPP), hInst_, nullptr);

    hChkUseTVTest_ = CreateWindowExW(
        0, L"BUTTON", L"TVTest連携を使用",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        16, 20, 180, 24,
        hTVTestPage_, CtrlId(IDC_TUNER_CHK_TVTEST), hInst_, nullptr);

    CreateStaticLabel(hTVTestPage_, hInst_, hFont_, 16, 56, kLabelW, 20, L"TVTest.exe：");
    hEdTVTestExe_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        140, 52, 420, 24,
        hTVTestPage_, CtrlId(IDC_TUNER_ED_TVTEST_EXE), hInst_, nullptr);

    hBtnBrowseTVTestExe_ = CreateWindowExW(
        0, L"BUTTON", L"参照...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        568, 52, kBrowseButtonW, kCtrlH,
        hTVTestPage_, CtrlId(IDC_TUNER_BTN_BROWSE_TVTEST_EXE), hInst_, nullptr);

    CreateStaticLabel(hTVTestPage_, hInst_, hFont_, 16, 88, kLabelW, 20, L"TVTest用BonDriver：");
    hEdTVTestBonDriver_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        140, 84, 420, 24,
        hTVTestPage_, CtrlId(IDC_TUNER_ED_TVTEST_BON), hInst_, nullptr);

    hBtnBrowseTVTestBonDriver_ = CreateWindowExW(
        0, L"BUTTON", L"参照...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        568, 84, kBrowseButtonW, kCtrlH,
        hTVTestPage_, CtrlId(IDC_TUNER_BTN_BROWSE_TVTEST_BON), hInst_, nullptr);

    CreateStaticLabel(hTVTestPage_, hInst_, hFont_, 16, 120, kLabelW, 20, L"制御パイプ：");
    hEdTVTestControlPipe_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        140, 116, 420, 24,
        hTVTestPage_, CtrlId(IDC_TUNER_ED_TVTEST_CTRLPIPE), hInst_, nullptr);

    CreateStaticLabel(hTVTestPage_, hInst_, hFont_, 16, 152, kLabelW, 20, L"TSパイプ：");
    hEdTVTestTsPipe_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        140, 148, 420, 24,
        hTVTestPage_, CtrlId(IDC_TUNER_ED_TVTEST_TSPIPE), hInst_, nullptr);

    hTVTestTestTab_ = CreateWindowExW(
        0,
        WC_TABCONTROLW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
        16, 180, 620, 220,
        hTVTestPage_,
        CtrlId(IDC_TUNER_TVTEST_TESTTAB),
        hInst_,
        nullptr);

    if (hTVTestTestTab_ == nullptr) {
        ReportCreateWindowError(hWnd_, L"TVTestテスト小タブ(hTVTestTestTab_)");
        return;
    }

    hTVTestBasicTestPage_ = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kTunerSettingsPageClassName,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 100, 100,
        hTVTestTestTab_,
        nullptr,
        hInst_,
        hWnd_);

    if (hTVTestBasicTestPage_ == nullptr) {
        ReportCreateWindowError(hWnd_, L"TVTest基本テストページ");
        return;
    }

    hTVTestAdvancedTestPage_ = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kTunerSettingsPageClassName,
        L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 100, 100,
        hTVTestTestTab_,
        nullptr,
        hInst_,
        hWnd_);

    if (hTVTestAdvancedTestPage_ == nullptr) {
        ReportCreateWindowError(hWnd_, L"TVTest詳細テストページ");
        return;
    }

    // ---- 基本テスト page ----
    hBtnTVTestResetPipes_ = CreateWindowExW(
        0, L"BUTTON", L"パイプ初期化",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        16, 16, kTestButtonW, kCtrlH,
        hTVTestBasicTestPage_, CtrlId(IDC_TUNER_BTN_TVTEST_RESET_PIPES), hInst_, nullptr);

    hBtnTVTestLaunch_ = CreateWindowExW(
        0, L"BUTTON", L"起動テスト",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        144, 16, kTestButtonW, kCtrlH,
        hTVTestBasicTestPage_, CtrlId(IDC_TUNER_BTN_TVTEST_LAUNCH), hInst_, nullptr);

    hBtnTVTestPing_ = CreateWindowExW(
        0, L"BUTTON", L"PING",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        272, 16, kSmallButtonW, kCtrlH,
        hTVTestBasicTestPage_, CtrlId(IDC_TUNER_BTN_TVTEST_PING), hInst_, nullptr);

    hBtnTVTestState_ = CreateWindowExW(
        0, L"BUTTON", L"状態取得",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        16, 48, kSmallButtonW, kCtrlH,
        hTVTestBasicTestPage_, CtrlId(IDC_TUNER_BTN_TVTEST_STATE), hInst_, nullptr);

    hBtnTVTestProgram_ = CreateWindowExW(
        0, L"BUTTON", L"番組取得",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        116, 48, kSmallButtonW, kCtrlH,
        hTVTestBasicTestPage_, CtrlId(IDC_TUNER_BTN_TVTEST_PROGRAM), hInst_, nullptr);

    hBtnTVTestEnableTs_ = CreateWindowExW(
        0, L"BUTTON", L"TS ON",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        216, 48, kSmallButtonW, kCtrlH,
        hTVTestBasicTestPage_, CtrlId(IDC_TUNER_BTN_TVTEST_ENABLE_TS), hInst_, nullptr);

    hBtnTVTestDisableTs_ = CreateWindowExW(
        0, L"BUTTON", L"TS OFF",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        316, 48, kSmallButtonW, kCtrlH,
        hTVTestBasicTestPage_, CtrlId(IDC_TUNER_BTN_TVTEST_DISABLE_TS), hInst_, nullptr);

    // ---- 詳細テスト page ----
    hBtnTVTestStatusDetail_ = CreateWindowExW(
        0, L"BUTTON", L"状態詳細",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        16, 16, kSmallButtonW, kCtrlH,
        hTVTestAdvancedTestPage_, CtrlId(IDC_TUNER_BTN_TVTEST_STATUS_DETAIL), hInst_, nullptr);

    hBtnTVTestReloadIni_ = CreateWindowExW(
        0, L"BUTTON", L"INI再読込",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        116, 16, kSmallButtonW, kCtrlH,
        hTVTestAdvancedTestPage_, CtrlId(IDC_TUNER_BTN_TVTEST_RELOAD_INI), hInst_, nullptr);

    hLblTVTestChannel_ = CreateWindowExW(
        0, L"STATIC", L"チャンネル：",
        WS_CHILD | WS_VISIBLE,
        16, 56, 72, 20,
        hTVTestAdvancedTestPage_, nullptr, hInst_, nullptr);

    hCbTVTestChannelList_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        96, 52, 360, 240,
        hTVTestAdvancedTestPage_, CtrlId(IDC_TUNER_CB_TVTEST_CHANNEL_LIST), hInst_, nullptr);

    hBtnTVTestReloadChannelList_ = CreateWindowExW(
        0, L"BUTTON", L"一覧再読込",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        464, 52, kTestButtonW, kCtrlH,
        hTVTestAdvancedTestPage_, CtrlId(IDC_TUNER_BTN_TVTEST_RELOAD_LIST), hInst_, nullptr);

    hBtnTVTestSetChannel_ = CreateWindowExW(
        0, L"BUTTON", L"切替テスト",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        16, 84, kTestButtonW, kCtrlH,
        hTVTestAdvancedTestPage_, CtrlId(IDC_TUNER_BTN_TVTEST_SET_CHANNEL), hInst_, nullptr);

    hBtnTVTestTsRead_ = CreateWindowExW(
        0, L"BUTTON", L"TS受信テスト",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        144, 84, kTestButtonW, kCtrlH,
        hTVTestAdvancedTestPage_, CtrlId(IDC_TUNER_BTN_TVTEST_TS_READ), hInst_, nullptr);

    hBtnTVTestWatchReservationFlowTest_ = CreateWindowExW(
        0, L"BUTTON", L"視聴予約テスト",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        272, 84, kTestButtonW + 24, kCtrlH,
        hTVTestAdvancedTestPage_,
        CtrlId(IDC_TUNER_BTN_TVTEST_WATCH_FLOW_TEST),
        hInst_,
        nullptr);

    SetFontIf(hChkUseTVTest_, hFont_);
    SetFontIf(hEdTVTestExe_, hFont_);
    SetFontIf(hBtnBrowseTVTestExe_, hFont_);
    SetFontIf(hEdTVTestControlPipe_, hFont_);
    SetFontIf(hEdTVTestTsPipe_, hFont_);
    SetFontIf(hTVTestTestTab_, hFont_);
    SetFontIf(hTVTestBasicTestPage_, hFont_);
    SetFontIf(hTVTestAdvancedTestPage_, hFont_);

    SetFontIf(hBtnTVTestResetPipes_, hFont_);
    SetFontIf(hBtnTVTestLaunch_, hFont_);
    SetFontIf(hBtnTVTestPing_, hFont_);
    SetFontIf(hBtnTVTestState_, hFont_);
    SetFontIf(hBtnTVTestProgram_, hFont_);
    SetFontIf(hBtnTVTestEnableTs_, hFont_);
    SetFontIf(hBtnTVTestDisableTs_, hFont_);

    SetFontIf(hBtnTVTestStatusDetail_, hFont_);
    SetFontIf(hBtnTVTestReloadIni_, hFont_);
    SetFontIf(hLblTVTestChannel_, hFont_);
    SetFontIf(hCbTVTestChannelList_, hFont_);
    SetFontIf(hBtnTVTestReloadChannelList_, hFont_);
    SetFontIf(hBtnTVTestSetChannel_, hFont_);
    SetFontIf(hBtnTVTestTsRead_, hFont_);
    SetFontIf(hBtnTVTestWatchReservationFlowTest_, hFont_);

    SetFontIf(hEdInternalBonDriver_, hFont_);
    SetFontIf(hLblTimeShiftLockChannel_, hFont_);
    SetFontIf(hCbTimeShiftLockChannel_, hFont_);
    SetFontIf(hBtnTimeShiftReloadChannels_, hFont_);
    SetFontIf(hBtnBrowseInternalBonDriver_, hFont_);
    SetFontIf(hEdTVTestBonDriver_, hFont_);
    SetFontIf(hBtnBrowseTVTestBonDriver_, hFont_);

    RebuildTunerTabs();
    RebuildSubTabs();
    RebuildTVTestTestTabs();

    loadedTunerIndex_ = GetCurrentTunerIndex();
    LoadPageFromConfig(loadedTunerIndex_);
    ShowSelectedSubPage();
    ShowSelectedTVTestTestPage();

    RECT rc{};
    GetClientRect(hWnd_, &rc);
    LayoutChildren(rc.right - rc.left, rc.bottom - rc.top);
    ReloadTVTestChannelList();
    RedrawActiveSubPage();
}

void TunerSettingsTab::LayoutChildren(int width, int height)
{
    if (hTunerTab_ == nullptr) {
        return;
    }

    const int clientWidth = (width > 0) ? width : 0;
    const int clientHeight = (height > 0) ? height : 0;

    const int tunerTabX = kOuterMargin;
    const int tunerTabY = kOuterMargin;
    const int tunerTabWidth =
        (clientWidth - kOuterMargin * 2 > 0) ? (clientWidth - kOuterMargin * 2) : 0;
    const int tunerTabHeight = kTunerTabHeight;

    MoveWindow(hTunerTab_, tunerTabX, tunerTabY, tunerTabWidth, tunerTabHeight, TRUE);

    if (hSubTab_ == nullptr) {
        return;
    }

    const int subTabX = kOuterMargin;
    const int subTabY = tunerTabY + tunerTabHeight + kGapBetweenTabs;
    const int subTabWidth = tunerTabWidth;
    const int subTabHeight =
        (clientHeight - subTabY - kOuterMargin > 0) ? (clientHeight - subTabY - kOuterMargin) : 0;

    MoveWindow(hSubTab_, subTabX, subTabY, subTabWidth, subTabHeight, TRUE);

    RECT innerRect{};
    GetClientRect(hSubTab_, &innerRect);
    TabCtrl_AdjustRect(hSubTab_, FALSE, &innerRect);

    const int pageX = innerRect.left;
    const int pageY = innerRect.top;
    const int pageWidth = innerRect.right - innerRect.left;
    const int pageHeight = innerRect.bottom - innerRect.top;

    if (hBasicPage_ != nullptr) {
        MoveWindow(hBasicPage_, pageX, pageY, pageWidth, pageHeight, TRUE);
    }
    if (hOperationPage_ != nullptr) {
        MoveWindow(hOperationPage_, pageX, pageY, pageWidth, pageHeight, TRUE);
    }
    if (hExternalPage_ != nullptr) {
        MoveWindow(hExternalPage_, pageX, pageY, pageWidth, pageHeight, TRUE);
    }
    if (hTVTestPage_ != nullptr) {
        MoveWindow(hTVTestPage_, pageX, pageY, pageWidth, pageHeight, TRUE);
    }

    LayoutPageControls(pageWidth, pageHeight);
    RedrawActiveSubPage();
}

void TunerSettingsTab::LayoutPageControls(int pageWidth, int /*pageHeight*/)
{
    const int browseX = std::max(kCtrlX + kMinEditW + kCtrlGap, pageWidth - kRightPadding - kBrowseButtonW);
    const int editW = std::max(kMinEditW, browseX - kCtrlX - kCtrlGap);
    const int timeShiftComboW =
        std::max(kMinEditW, browseX - kCtrlX - kCtrlGap);

    if (hLblTimeShiftLockChannel_ != nullptr) {
        MoveWindow(hLblTimeShiftLockChannel_, kLabelX, 128, kLabelW + 30, 20, TRUE);
    }

    if (hCbTimeShiftLockChannel_ != nullptr) {
        MoveWindow(hCbTimeShiftLockChannel_, kCtrlX, 124, timeShiftComboW, 240, TRUE);
    }

    if (hBtnTimeShiftReloadChannels_ != nullptr) {
        MoveWindow(hBtnTimeShiftReloadChannels_, browseX, 124, kBrowseButtonW, kCtrlH, TRUE);
    }

    if (hEdInternalBonDriver_ != nullptr) {
        MoveWindow(hEdInternalBonDriver_, kCtrlX, 80, editW, kCtrlH, TRUE);
    }
    if (hBtnBrowseInternalBonDriver_ != nullptr) {
        MoveWindow(hBtnBrowseInternalBonDriver_, browseX, 80, kBrowseButtonW, kCtrlH, TRUE);
    }

    if (hEdExternalApp_ != nullptr) {
        MoveWindow(hEdExternalApp_, kCtrlX, 52, editW, kCtrlH, TRUE);
    }
    if (hBtnBrowseExternalApp_ != nullptr) {
        MoveWindow(hBtnBrowseExternalApp_, browseX, 52, kBrowseButtonW, kCtrlH, TRUE);
    }
    if (hBtnTestExternalApp_ != nullptr) {
        MoveWindow(hBtnTestExternalApp_, kCtrlX, 84, kTestButtonW, kCtrlH, TRUE);
    }

    if (hEdTVTestExe_ != nullptr) {
        MoveWindow(hEdTVTestExe_, kCtrlX, 52, editW, kCtrlH, TRUE);
    }
    if (hBtnBrowseTVTestExe_ != nullptr) {
        MoveWindow(hBtnBrowseTVTestExe_, browseX, 52, kBrowseButtonW, kCtrlH, TRUE);
    }
    if (hEdTVTestBonDriver_ != nullptr) {
        MoveWindow(hEdTVTestBonDriver_, kCtrlX, 84, editW, kCtrlH, TRUE);
    }
    if (hBtnBrowseTVTestBonDriver_ != nullptr) {
        MoveWindow(hBtnBrowseTVTestBonDriver_, browseX, 84, kBrowseButtonW, kCtrlH, TRUE);
    }
    if (hEdTVTestControlPipe_ != nullptr) {
        MoveWindow(hEdTVTestControlPipe_, kCtrlX, 116, editW, kCtrlH, TRUE);
    }
    if (hEdTVTestTsPipe_ != nullptr) {
        MoveWindow(hEdTVTestTsPipe_, kCtrlX, 148, editW, kCtrlH, TRUE);
    }

    RECT tvRect{};
    if (hTVTestPage_ != nullptr) {
        GetClientRect(hTVTestPage_, &tvRect);
    }

    const int buttonGap = 8;
    const int testTabX = 16;
    const int testTabY = 180;
    const int testTabW = std::max<int>(220, pageWidth - 32);
    const int testTabH = std::max<int>(160, static_cast<int>(tvRect.bottom) - testTabY - 12);

    if (hTVTestTestTab_ != nullptr) {
        MoveWindow(hTVTestTestTab_, testTabX, testTabY, testTabW, testTabH, TRUE);

        RECT innerRect{};
        GetClientRect(hTVTestTestTab_, &innerRect);
        TabCtrl_AdjustRect(hTVTestTestTab_, FALSE, &innerRect);

        const int innerW = std::max<int>(0, static_cast<int>(innerRect.right - innerRect.left));
        const int innerH = std::max<int>(0, static_cast<int>(innerRect.bottom - innerRect.top));

        if (hTVTestBasicTestPage_ != nullptr) {
            MoveWindow(hTVTestBasicTestPage_, innerRect.left, innerRect.top, innerW, innerH, TRUE);
        }
        if (hTVTestAdvancedTestPage_ != nullptr) {
            MoveWindow(hTVTestAdvancedTestPage_, innerRect.left, innerRect.top, innerW, innerH, TRUE);
        }

        // ---- 基本テスト ----
        if (hBtnTVTestResetPipes_ != nullptr) {
            MoveWindow(hBtnTVTestResetPipes_, 16, 16, kTestButtonW, kCtrlH, TRUE);
        }
        if (hBtnTVTestLaunch_ != nullptr) {
            MoveWindow(hBtnTVTestLaunch_, 144, 16, kTestButtonW, kCtrlH, TRUE);
        }
        if (hBtnTVTestPing_ != nullptr) {
            MoveWindow(hBtnTVTestPing_, 272, 16, kSmallButtonW, kCtrlH, TRUE);
        }
        if (hBtnTVTestState_ != nullptr) {
            MoveWindow(hBtnTVTestState_, 16, 48, kSmallButtonW, kCtrlH, TRUE);
        }
        if (hBtnTVTestProgram_ != nullptr) {
            MoveWindow(hBtnTVTestProgram_, 116, 48, kSmallButtonW, kCtrlH, TRUE);
        }
        if (hBtnTVTestEnableTs_ != nullptr) {
            MoveWindow(hBtnTVTestEnableTs_, 216, 48, kSmallButtonW, kCtrlH, TRUE);
        }
        if (hBtnTVTestDisableTs_ != nullptr) {
            MoveWindow(hBtnTVTestDisableTs_, 316, 48, kSmallButtonW, kCtrlH, TRUE);
        }

        // ---- 詳細テスト ----
        if (hBtnTVTestStatusDetail_ != nullptr) {
            MoveWindow(hBtnTVTestStatusDetail_, 16, 16, kSmallButtonW, kCtrlH, TRUE);
        }
        if (hBtnTVTestReloadIni_ != nullptr) {
            MoveWindow(hBtnTVTestReloadIni_, 116, 16, kSmallButtonW, kCtrlH, TRUE);
        }
        if (hLblTVTestChannel_ != nullptr) {
            MoveWindow(hLblTVTestChannel_, 16, 56, 72, 20, TRUE);
        }

        const int comboX = 96;
        const int comboW = std::max<int>(220, innerW - comboX - 16 - kTestButtonW - buttonGap);

        if (hCbTVTestChannelList_ != nullptr) {
            MoveWindow(hCbTVTestChannelList_, comboX, 52, comboW, 240, TRUE);
        }
        if (hBtnTVTestReloadChannelList_ != nullptr) {
            MoveWindow(hBtnTVTestReloadChannelList_, comboX + comboW + buttonGap, 52, kTestButtonW, kCtrlH, TRUE);
        }
        if (hBtnTVTestSetChannel_ != nullptr) {
            MoveWindow(hBtnTVTestSetChannel_, 16, 84, kTestButtonW, kCtrlH, TRUE);
        }
        if (hBtnTVTestTsRead_ != nullptr) {
            MoveWindow(hBtnTVTestTsRead_, 144, 84, kTestButtonW, kCtrlH, TRUE);
        }
        if (hBtnTVTestWatchReservationFlowTest_ != nullptr) {
            MoveWindow(hBtnTVTestWatchReservationFlowTest_,272,84,kTestButtonW + 24,kCtrlH,TRUE);
        }
    }
}

void TunerSettingsTab::RebuildTunerTabs()
{
    if (hTunerTab_ == nullptr) {
        return;
    }

    int current = GetCurrentTunerIndex();
    if (current < 0) {
        current = 0;
    }

    while (TabCtrl_GetItemCount(hTunerTab_) > 0) {
        TabCtrl_DeleteItem(hTunerTab_, 0);
    }

    TCITEMW item{};
    item.mask = TCIF_TEXT;

    for (int i = 0; i < tunerCount_; ++i) {
        std::wstring title = L"チューナー" + std::to_wstring(i + 1);
        item.pszText = title.data();
        TabCtrl_InsertItem(hTunerTab_, i, &item);
    }

    if (current >= tunerCount_) {
        current = tunerCount_ - 1;
    }
    if (current < 0) {
        current = 0;
    }

    TabCtrl_SetCurSel(hTunerTab_, current);
}

void TunerSettingsTab::RebuildSubTabs()
{
    if (hSubTab_ == nullptr) {
        return;
    }

    int current = GetCurrentSubTabIndex();
    if (current < 0 || current > 3) {
        current = 0;
    }

    while (TabCtrl_GetItemCount(hSubTab_) > 0) {
        TabCtrl_DeleteItem(hSubTab_, 0);
    }

    TCITEMW item{};
    item.mask = TCIF_TEXT;

    std::wstring tabTitle0 = L"基本";
    item.pszText = tabTitle0.data();
    TabCtrl_InsertItem(hSubTab_, 0, &item);

    std::wstring tabTitle1 = L"運用";
    item.pszText = tabTitle1.data();
    TabCtrl_InsertItem(hSubTab_, 1, &item);

    std::wstring tabTitle2 = L"外部連携";
    item.pszText = tabTitle2.data();
    TabCtrl_InsertItem(hSubTab_, 2, &item);

    std::wstring tabTitle3 = L"TVTest";
    item.pszText = tabTitle3.data();
    TabCtrl_InsertItem(hSubTab_, 3, &item);

    TabCtrl_SetCurSel(hSubTab_, current);
}

void TunerSettingsTab::RebuildTVTestTestTabs()
{
    if (hTVTestTestTab_ == nullptr) {
        return;
    }

    int current = TabCtrl_GetCurSel(hTVTestTestTab_);
    if (current < 0 || current > 1) {
        current = 0;
    }

    while (TabCtrl_GetItemCount(hTVTestTestTab_) > 0) {
        TabCtrl_DeleteItem(hTVTestTestTab_, 0);
    }

    TCITEMW item{};
    item.mask = TCIF_TEXT;

    std::wstring tabTitle0 = L"基本テスト";
    item.pszText = tabTitle0.data();
    TabCtrl_InsertItem(hTVTestTestTab_, 0, &item);

    std::wstring tabTitle1 = L"詳細テスト";
    item.pszText = tabTitle1.data();
    TabCtrl_InsertItem(hTVTestTestTab_, 1, &item);

    TabCtrl_SetCurSel(hTVTestTestTab_, current);
}

void TunerSettingsTab::ShowSelectedTVTestTestPage()
{
    if (hTVTestBasicTestPage_ == nullptr || hTVTestAdvancedTestPage_ == nullptr) {
        return;
    }

    const int selected = (hTVTestTestTab_ != nullptr) ? TabCtrl_GetCurSel(hTVTestTestTab_) : 0;
    const int sel = (selected < 0) ? 0 : selected;

    ShowWindow(hTVTestBasicTestPage_, (sel == 0) ? SW_SHOW : SW_HIDE);
    ShowWindow(hTVTestAdvancedTestPage_, (sel == 1) ? SW_SHOW : SW_HIDE);

    if (hTVTestTestTab_ != nullptr) {
        RedrawWindow(
            hTVTestTestTab_,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

int TunerSettingsTab::GetCurrentTunerIndex() const noexcept
{
    if (hTunerTab_ == nullptr) {
        return 0;
    }

    const int selected = TabCtrl_GetCurSel(hTunerTab_);
    return (selected < 0) ? 0 : selected;
}

int TunerSettingsTab::GetCurrentSubTabIndex() const noexcept
{
    if (hSubTab_ == nullptr) {
        return 0;
    }

    const int selected = TabCtrl_GetCurSel(hSubTab_);
    return (selected < 0) ? 0 : selected;
}

void TunerSettingsTab::ShowSelectedTunerPage()
{
    LoadPageFromConfig(loadedTunerIndex_);
    ShowSelectedSubPage();
}

void TunerSettingsTab::ShowSelectedSubPage()
{
    if (hBasicPage_ == nullptr || hOperationPage_ == nullptr || hExternalPage_ == nullptr || hTVTestPage_ == nullptr) {
        return;
    }

    const int selectedSubTab = GetCurrentSubTabIndex();

    ShowWindow(hBasicPage_, (selectedSubTab == 0) ? SW_SHOW : SW_HIDE);
    ShowWindow(hOperationPage_, (selectedSubTab == 1) ? SW_SHOW : SW_HIDE);
    ShowWindow(hExternalPage_, (selectedSubTab == 2) ? SW_SHOW : SW_HIDE);
    ShowWindow(hTVTestPage_, (selectedSubTab == 3) ? SW_SHOW : SW_HIDE);

    UpdateControlEnableState();

    if (selectedSubTab == 3) {
        ShowSelectedTVTestTestPage();
    }

    RedrawActiveSubPage();
}

void TunerSettingsTab::LoadPageFromConfig(int tunerIndex)
{
    if (cfg_ == nullptr) {
        return;
    }

    if (tunerIndex < 0) {
        tunerIndex = 0;
    }
    if (tunerIndex >= tunerCount_) {
        tunerIndex = tunerCount_ - 1;
    }
    if (tunerIndex < 0) {
        tunerIndex = 0;
    }

    SetChecked(hChkEnabled_, cfg_->GetTunerEnabled(tunerIndex));

    if (hEdName_ != nullptr) {
        SetWindowTextW(hEdName_, cfg_->GetTunerName(tunerIndex).c_str());
    }

    if (hEdInternalBonDriver_ != nullptr) {
        std::wstring path = cfg_->GetTunerInternalBonDriverPath(tunerIndex);
        if (path.empty()) {
            path = cfg_->GetBonDriverPathForTuner(tunerIndex);
        }
        SetWindowTextW(hEdInternalBonDriver_, path.c_str());
    }

    SetComboSelSafe(hCbReceiveMode_, static_cast<int>(cfg_->GetTunerReceiveMode(tunerIndex)));
    SetComboSelSafe(hCbUseRole_, static_cast<int>(cfg_->GetTunerUseRole(tunerIndex)));

    SetChecked(hChkAllowParallel_, cfg_->GetTunerAllowParallel(tunerIndex));
    SetEditInt(hEdPriority_, cfg_->GetTunerPriority(tunerIndex));

    ReloadTimeShiftChannelList();
    SelectTimeShiftLockChannelFromConfig(tunerIndex);

    SetChecked(hChkUseExternalApp_, cfg_->GetTunerUseExternalApp(tunerIndex));

    if (hEdExternalApp_ != nullptr) {
        SetWindowTextW(hEdExternalApp_, cfg_->GetTunerExternalAppPath(tunerIndex).c_str());
    }

    SetChecked(hChkUseTVTest_, cfg_->GetTunerTvTestUseBridge(tunerIndex));

    if (hEdTVTestExe_ != nullptr) {
        SetWindowTextW(hEdTVTestExe_, cfg_->GetTunerTvTestExePath(tunerIndex).c_str());
    }

    if (hEdTVTestBonDriver_ != nullptr) {
        std::wstring path = cfg_->GetTunerTvTestBonDriverPath(tunerIndex);
        if (path.empty()) {
            path = cfg_->GetBonDriverPathForTuner(tunerIndex);
        }
        SetWindowTextW(hEdTVTestBonDriver_, path.c_str());
    }
    if (hEdTVTestControlPipe_ != nullptr) {
        SetWindowTextW(hEdTVTestControlPipe_, cfg_->GetTunerTvTestControlPipeName(tunerIndex).c_str());
    }
    if (hEdTVTestTsPipe_ != nullptr) {
        SetWindowTextW(hEdTVTestTsPipe_, cfg_->GetTunerTvTestTsPipeName(tunerIndex).c_str());
    }

    UpdateControlEnableState();
    ReloadTVTestChannelList();
    ShowSelectedTVTestTestPage();
}

void TunerSettingsTab::SavePageToConfig(int tunerIndex)
{
    if (cfg_ == nullptr) {
        return;
    }

    if (tunerIndex < 0) {
        tunerIndex = 0;
    }
    if (tunerIndex >= tunerCount_) {
        tunerIndex = tunerCount_ - 1;
    }
    if (tunerIndex < 0) {
        tunerIndex = 0;
    }

    cfg_->SetTunerEnabled(tunerIndex, IsChecked(hChkEnabled_));

    wchar_t buf[2048]{};

    if (hEdName_ != nullptr) {
        GetWindowTextW(hEdName_, buf, static_cast<int>(_countof(buf)));
        cfg_->SetTunerName(tunerIndex, buf);
    }

    if (hEdInternalBonDriver_ != nullptr) {
        GetWindowTextW(hEdInternalBonDriver_, buf, static_cast<int>(_countof(buf)));

        cfg_->SetTunerInternalBonDriverPath(tunerIndex, buf);

        // 既存互換。
        // しばらくは BonDriverPathX にも内部機能用BonDriverを保存しておく。
        cfg_->SetBonDriverPathForTuner(tunerIndex, buf);
    }

    const int receiveSel = (hCbReceiveMode_ != nullptr)
        ? static_cast<int>(SendMessageW(hCbReceiveMode_, CB_GETCURSEL, 0, 0))
        : 0;
    cfg_->SetTunerReceiveMode(
        tunerIndex,
        static_cast<ConfigManager::TunerReceiveMode>((receiveSel < 0) ? 0 : receiveSel));

    const int roleSel = (hCbUseRole_ != nullptr)
        ? static_cast<int>(SendMessageW(hCbUseRole_, CB_GETCURSEL, 0, 0))
        : 0;
    
    const int safeRoleSel = (roleSel < 0) ? 0 : roleSel;

    cfg_->SetTunerUseRole(
        tunerIndex,
        static_cast<ConfigManager::TunerUseRole>(safeRoleSel));

    cfg_->SetTunerAllowParallel(tunerIndex, IsChecked(hChkAllowParallel_));
    cfg_->SetTunerPriority(tunerIndex, GetEditInt(hEdPriority_, 0));

    if (safeRoleSel == static_cast<int>(ConfigManager::TunerUseRole::TimeShiftOnly)) {
        SaveTimeShiftLockChannelToConfig(tunerIndex);
    }
    cfg_->SetTunerUseExternalApp(tunerIndex, IsChecked(hChkUseExternalApp_));

    if (hEdExternalApp_ != nullptr) {
        GetWindowTextW(hEdExternalApp_, buf, static_cast<int>(_countof(buf)));
        cfg_->SetTunerExternalAppPath(tunerIndex, buf);
    }

    cfg_->SetTunerTvTestUseBridge(tunerIndex, IsChecked(hChkUseTVTest_));

    if (hEdTVTestExe_ != nullptr) {
        cfg_->SetTunerTvTestExePath(tunerIndex, GetWindowTextString(hEdTVTestExe_));
    }
    if (hEdTVTestBonDriver_ != nullptr) {
        GetWindowTextW(hEdTVTestBonDriver_, buf, static_cast<int>(_countof(buf)));
        cfg_->SetTunerTvTestBonDriverPath(tunerIndex, buf);
    }
    if (hEdTVTestControlPipe_ != nullptr) {
        cfg_->SetTunerTvTestControlPipeName(tunerIndex, GetWindowTextString(hEdTVTestControlPipe_));
    }
    if (hEdTVTestTsPipe_ != nullptr) {
        cfg_->SetTunerTvTestTsPipeName(tunerIndex, GetWindowTextString(hEdTVTestTsPipe_));
    }
}

void TunerSettingsTab::UpdateControlEnableState()
{
    const int roleSel = (hCbUseRole_ != nullptr)
        ? static_cast<int>(SendMessageW(hCbUseRole_, CB_GETCURSEL, 0, 0))
        : 0;

    const bool useTimeShift =
        roleSel == static_cast<int>(ConfigManager::TunerUseRole::TimeShiftOnly);
    const bool useExternalApp = IsChecked(hChkUseExternalApp_);
    const bool useTVTest = IsChecked(hChkUseTVTest_);

    if (hEdExternalApp_ != nullptr) {
        EnableWindow(hEdExternalApp_, useExternalApp ? TRUE : FALSE);
    }
    if (hBtnBrowseExternalApp_ != nullptr) {
        EnableWindow(hBtnBrowseExternalApp_, useExternalApp ? TRUE : FALSE);
    }
    if (hBtnTestExternalApp_ != nullptr) {
        EnableWindow(hBtnTestExternalApp_, useExternalApp ? TRUE : FALSE);
    }

    if (hEdTVTestExe_ != nullptr) {
        EnableWindow(hEdTVTestExe_, useTVTest ? TRUE : FALSE);
    }
    if (hEdTVTestBonDriver_ != nullptr) {
        EnableWindow(hEdTVTestBonDriver_, useTVTest ? TRUE : FALSE);
    }
    if (hBtnBrowseTVTestBonDriver_ != nullptr) {
        EnableWindow(hBtnBrowseTVTestBonDriver_, useTVTest ? TRUE : FALSE);
    }

    if (hBtnBrowseTVTestExe_ != nullptr) {
        EnableWindow(hBtnBrowseTVTestExe_, useTVTest ? TRUE : FALSE);
    }
    if (hBtnTVTestResetPipes_ != nullptr) {
        EnableWindow(hBtnTVTestResetPipes_, useTVTest ? TRUE : FALSE);
    }

    if (hEdTVTestControlPipe_ != nullptr) {
        EnableWindow(hEdTVTestControlPipe_, useTVTest ? TRUE : FALSE);
    }
    if (hEdTVTestTsPipe_ != nullptr) {
        EnableWindow(hEdTVTestTsPipe_, useTVTest ? TRUE : FALSE);
    }
    if (hBtnTVTestLaunch_ != nullptr) {
        EnableWindow(hBtnTVTestLaunch_, useTVTest ? TRUE : FALSE);
    }
    if (hBtnTVTestPing_ != nullptr) {
        EnableWindow(hBtnTVTestPing_, useTVTest ? TRUE : FALSE);
    }
    if (hBtnTVTestState_ != nullptr) {
        EnableWindow(hBtnTVTestState_, useTVTest ? TRUE : FALSE);
    }
    if (hBtnTVTestProgram_ != nullptr) {
        EnableWindow(hBtnTVTestProgram_, useTVTest ? TRUE : FALSE);
    }
    if (hBtnTVTestStatusDetail_ != nullptr) {
        EnableWindow(hBtnTVTestStatusDetail_, useTVTest ? TRUE : FALSE);
    }
    if (hBtnTVTestReloadIni_ != nullptr) {
        EnableWindow(hBtnTVTestReloadIni_, useTVTest ? TRUE : FALSE);
    }
    if (hBtnTVTestEnableTs_ != nullptr) {
        EnableWindow(hBtnTVTestEnableTs_, useTVTest ? TRUE : FALSE);
    }
    if (hBtnTVTestDisableTs_ != nullptr) {
        EnableWindow(hBtnTVTestDisableTs_, useTVTest ? TRUE : FALSE);
    }


    if (hCbTVTestChannelList_ != nullptr) {
        EnableWindow(hCbTVTestChannelList_, useTVTest ? TRUE : FALSE);
    }
    if (hBtnTVTestReloadChannelList_ != nullptr) {
        EnableWindow(hBtnTVTestReloadChannelList_, useTVTest ? TRUE : FALSE);
    }
    if (hBtnTVTestSetChannel_ != nullptr) {
        EnableWindow(hBtnTVTestSetChannel_, useTVTest ? TRUE : FALSE);
    }
    if (hBtnTVTestTsRead_ != nullptr) {
        EnableWindow(hBtnTVTestTsRead_, useTVTest ? TRUE : FALSE);
    }
    if (hBtnTVTestWatchReservationFlowTest_ != nullptr) {
        EnableWindow(hBtnTVTestWatchReservationFlowTest_, useTVTest ? TRUE : FALSE);
	}
    if (hLblTimeShiftLockChannel_ != nullptr) {
        EnableWindow(hLblTimeShiftLockChannel_, useTimeShift ? TRUE : FALSE);
    }
    if (hCbTimeShiftLockChannel_ != nullptr) {
        EnableWindow(hCbTimeShiftLockChannel_, useTimeShift ? TRUE : FALSE);
    }
    if (hBtnTimeShiftReloadChannels_ != nullptr) {
        EnableWindow(hBtnTimeShiftReloadChannels_, useTimeShift ? TRUE : FALSE);
    }

}

void TunerSettingsTab::RedrawActiveSubPage()
{
    if (hWnd_ == nullptr || hSubTab_ == nullptr) {
        return;
    }

    HWND hActivePage = hBasicPage_;
    const int selectedSubTab = GetCurrentSubTabIndex();

    if (selectedSubTab == 1) {
        hActivePage = hOperationPage_;
    }
    else if (selectedSubTab == 2) {
        hActivePage = hExternalPage_;
    }
    else if (selectedSubTab == 3) {
        hActivePage = hTVTestPage_;
    }

    if (hActivePage != nullptr) {
        SetWindowPos(
            hActivePage,
            HWND_TOP,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);

        RedrawWindow(
            hActivePage,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }

    RedrawWindow(
        hSubTab_,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);

    RedrawWindow(
        hWnd_,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

bool TunerSettingsTab::BrowseForFile(HWND hEdit, const wchar_t* title, const wchar_t* filter, const wchar_t* defExt)
{
    if (hEdit == nullptr || hWnd_ == nullptr) {
        return false;
    }

    wchar_t fileBuf[2048]{};
    const std::wstring current = GetWindowTextString(hEdit);
    if (!current.empty()) {
        wcsncpy_s(fileBuf, current.c_str(), _TRUNCATE);
    }

    wchar_t initialDirBuf[MAX_PATH]{};
    const wchar_t* initialDir = nullptr;

    if (!current.empty()) {
        std::error_code ec;
        std::filesystem::path p(current);
        std::filesystem::path dir =
            std::filesystem::is_directory(p, ec) ? p : p.parent_path();

        if (!dir.empty()) {
            const std::wstring dirStr = dir.wstring();
            wcsncpy_s(initialDirBuf, dirStr.c_str(), _TRUNCATE);
            initialDir = initialDirBuf;
        }
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd_;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = static_cast<DWORD>(_countof(fileBuf));
    ofn.lpstrTitle = title;
    ofn.lpstrInitialDir = initialDir;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = defExt;

    if (!GetOpenFileNameW(&ofn)) {
        return false;
    }

    SetWindowTextW(hEdit, fileBuf);
    SetFocus(hEdit);
    return true;
}

void TunerSettingsTab::BrowseBonDriverPath()
{
    BrowseInternalBonDriverPath();
}

void TunerSettingsTab::BrowseInternalBonDriverPath()
{
    static constexpr wchar_t kFilter[] =
        L"BonDriver DLL (*.dll)\0*.dll\0"
        L"すべてのファイル (*.*)\0*.*\0\0";

    BrowseForFile(
        hEdInternalBonDriver_,
        L"内部機能用 BonDriver を選択",
        kFilter,
        L"dll");
}

void TunerSettingsTab::BrowseTVTestBonDriverPath()
{
    static constexpr wchar_t kFilter[] =
        L"BonDriver DLL (*.dll)\0*.dll\0"
        L"すべてのファイル (*.*)\0*.*\0\0";

    if (BrowseForFile(
        hEdTVTestBonDriver_,
        L"TVTest用 BonDriver を選択",
        kFilter,
        L"dll")) {
        ReloadTVTestChannelList();
    }
}

void TunerSettingsTab::BrowseExternalAppPath()
{
    static constexpr wchar_t kFilter[] =
        L"実行ファイル (*.exe)\0*.exe\0"
        L"バッチファイル (*.bat;*.cmd)\0*.bat;*.cmd\0"
        L"すべてのファイル (*.*)\0*.*\0\0";

    BrowseForFile(
        hEdExternalApp_,
        L"外部アプリを選択",
        kFilter,
        L"exe");
}

void TunerSettingsTab::BrowseTVTestExePath()
{
    static constexpr wchar_t kFilter[] =
        L"TVTest 実行ファイル (*.exe)\0*.exe\0"
        L"すべてのファイル (*.*)\0*.*\0\0";

    if (BrowseForFile(
        hEdTVTestExe_,
        L"TVTest.exe を選択",
        kFilter,
        L"exe")) {
        ReloadTVTestChannelList();
    }
}

void TunerSettingsTab::ResetTVTestPipePathsToDefault()
{
    const int tunerIndex = GetCurrentTunerIndex();

    const std::wstring controlPipe =
        MakeDefaultTVTestControlPipeForTuner_(tunerIndex);

    const std::wstring tsPipe =
        MakeDefaultTVTestTsPipeForTuner_(tunerIndex);

    if (hEdTVTestControlPipe_ != nullptr) {
        SetWindowTextW(hEdTVTestControlPipe_, controlPipe.c_str());
    }
    if (hEdTVTestTsPipe_ != nullptr) {
        SetWindowTextW(hEdTVTestTsPipe_, tsPipe.c_str());
    }

    DebugLogFormat(
        L"[TunerSettingsTab][TVTest] pipe paths reset to tuner default: tuner=%d control=%ls ts=%ls",
        tunerIndex,
        controlPipe.c_str(),
        tsPipe.c_str());
}



void TunerSettingsTab::RunExternalAppTest()
{
    if (hWnd_ == nullptr) {
        return;
    }

    if (!IsChecked(hChkUseExternalApp_)) {
        MessageBoxW(
            hWnd_,
            L"「外部アプリを使用」をオンにしてから起動テストを実行してください。",
            L"外部アプリ起動テスト",
            MB_OK | MB_ICONINFORMATION);
        return;
    }

    const std::wstring rawPath = GetWindowTextString(hEdExternalApp_);
    const std::wstring path = TrimAndUnquote(rawPath);

    if (path.empty()) {
        MessageBoxW(
            hWnd_,
            L"外部アプリパスが空です。",
            L"外部アプリ起動テスト",
            MB_OK | MB_ICONWARNING);
        return;
    }

    if (!LaunchExternalApp(path)) {
        return;
    }

    DebugLogFormat(L"[TunerSettingsTab] external app test launch ok: %ls", path.c_str());

    MessageBoxW(
        hWnd_,
        L"外部アプリの起動要求を送信しました。",
        L"外部アプリ起動テスト",
        MB_OK | MB_ICONINFORMATION);
}

bool TunerSettingsTab::LaunchExternalApp(const std::wstring& rawPath)
{
    if (hWnd_ == nullptr) {
        return false;
    }

    std::wstring normalized = TrimAndUnquote(rawPath);
    if (normalized.empty()) {
        MessageBoxW(
            hWnd_,
            L"外部アプリパスが空です。",
            L"起動テスト",
            MB_OK | MB_ICONWARNING);
        return false;
    }

    std::filesystem::path appPath(normalized);

    if (appPath.is_relative()) {
        const std::filesystem::path moduleDir = GetModuleDirectoryPath();
        if (!moduleDir.empty()) {
            appPath = moduleDir / appPath;
        }
    }

    std::error_code ec;
    if (!std::filesystem::exists(appPath, ec) || ec) {
        std::wstring msg = L"指定されたファイルが見つかりません。\n\n";
        msg += appPath.wstring();

        DebugLogFormat(L"[TunerSettingsTab] launch failed: file not found: %ls", appPath.wstring().c_str());

        MessageBoxW(
            hWnd_,
            msg.c_str(),
            L"起動テスト",
            MB_OK | MB_ICONERROR);
        return false;
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = hWnd_;
    sei.lpVerb = L"open";
    const std::wstring pathText = appPath.wstring();
    sei.lpFile = pathText.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        const DWORD errorCode = GetLastError();

        wchar_t msg[512]{};
        _snwprintf_s(
            msg,
            _countof(msg),
            _TRUNCATE,
            L"起動に失敗しました。\n\nPath: %ls\nGetLastError=%lu",
            pathText.c_str(),
            static_cast<unsigned long>(errorCode));

        DebugLogFormat(
            L"[TunerSettingsTab] launch failed: path=%ls error=%lu",
            pathText.c_str(),
            static_cast<unsigned long>(errorCode));

        MessageBoxW(
            hWnd_,
            msg,
            L"起動テスト",
            MB_OK | MB_ICONERROR);
        return false;
    }

    if (sei.hProcess != nullptr) {
        CloseHandle(sei.hProcess);
        sei.hProcess = nullptr;
    }

    return true;
}

std::wstring TunerSettingsTab::GetTVTestControlPipeText() const
{
    std::wstring text = TrimAndUnquote(GetWindowTextString(hEdTVTestControlPipe_));
    if (text.empty()) {
        text = MakeDefaultTVTestControlPipeForTuner_(GetCurrentTunerIndex());
    }
    return text;
}

std::wstring TunerSettingsTab::GetTVTestTsPipeText() const
{
    std::wstring text = TrimAndUnquote(GetWindowTextString(hEdTVTestTsPipe_));
    if (text.empty()) {
        text = MakeDefaultTVTestTsPipeForTuner_(GetCurrentTunerIndex());
    }
    return text;
}

std::wstring TunerSettingsTab::GetInternalBonDriverText() const
{
    return TrimAndUnquote(GetWindowTextString(hEdInternalBonDriver_));
}

std::wstring TunerSettingsTab::GetTVTestBonDriverText() const
{
    std::wstring text = TrimAndUnquote(GetWindowTextString(hEdTVTestBonDriver_));

    // 旧互換。
    // TVTest用欄が空なら従来のBonDriver欄を使う。
    // 現在 hEdBonDriver_ は内部BonDriver欄を指しているため、
    // 最終的には TVTest用BonDriver欄の設定を推奨する。
    if (text.empty()) {
        text = TrimAndUnquote(GetWindowTextString(hEdBonDriver_));
    }

    return text;
}

bool TunerSettingsTab::SendTVTestCommand(const std::wstring& command, std::wstring* response)
{
    if (hWnd_ == nullptr) {
        return false;
    }

    if (!IsChecked(hChkUseTVTest_)) {
        MessageBoxW(
            hWnd_,
            L"「TVTest連携を使用」をオンにしてから実行してください。",
            L"TVTest連携",
            MB_OK | MB_ICONINFORMATION);
        return false;
    }

    const std::wstring pipeName = GetTVTestControlPipeText();
    if (pipeName.empty()) {
        MessageBoxW(
            hWnd_,
            L"制御パイプ名が空です。",
            L"TVTest連携",
            MB_OK | MB_ICONWARNING);
        return false;
    }

    DebugLogFormat(
        L"[TunerSettingsTab] SendTVTestCommand pipe=[%ls] cmd=[%ls]",
        pipeName.c_str(),
        command.c_str());

    std::wstring line = command;
    if (line.empty()) {
        return false;
    }
    if (line.back() != L'\n') {
        line.push_back(L'\n');
    }

    HANDLE hPipe = CreateFileW(
        pipeName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (hPipe == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();

        if (err == ERROR_PIPE_BUSY) {
            if (WaitNamedPipeW(pipeName.c_str(), 3000)) {
                hPipe = CreateFileW(
                    pipeName.c_str(),
                    GENERIC_READ | GENERIC_WRITE,
                    0,
                    nullptr,
                    OPEN_EXISTING,
                    0,
                    nullptr);
            }
        }
    }

    if (hPipe == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();

        wchar_t msg[512]{};
        _snwprintf_s(
            msg,
            _countof(msg),
            _TRUNCATE,
            L"TVTest制御パイプを開けませんでした。\n\nPipe: %ls\nGetLastError=%lu",
            pipeName.c_str(),
            static_cast<unsigned long>(err));

        DebugLogFormat(
            L"[TunerSettingsTab] CreateFile pipe failed: pipe=%ls error=%lu",
            pipeName.c_str(),
            static_cast<unsigned long>(err));

        MessageBoxW(hWnd_, msg, L"TVTest連携", MB_OK | MB_ICONERROR);
        return false;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    auto isExpectedReply = [&](const std::string& one) -> bool
        {
            std::string cmdUtf8 = WideToUtf8(command);
            for (char& ch : cmdUtf8) {
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            }

            if (cmdUtf8 == "PING") {
                return one.rfind("OK PONG", 0) == 0;
            }
            if (cmdUtf8 == "GET_STATE") {
                return one.rfind("SNAP reason=\"GET_STATE\"", 0) == 0;
            }
            if (cmdUtf8 == "GET_PROGRAM") {
                return one.rfind("PROGRAM ", 0) == 0;
            }
            if (cmdUtf8 == "GET_STATUS") {
                return one.rfind("STATUS ", 0) == 0;
            }
            if (cmdUtf8 == "ENABLE_TS") {
                return one.rfind("OK TS enabled", 0) == 0;
            }
            if (cmdUtf8 == "DISABLE_TS") {
                return one.rfind("OK TS disabled", 0) == 0;
            }

            // その他は OK / ERR を採用
            return one.rfind("OK", 0) == 0 || one.rfind("ERR", 0) == 0;
        };

    bool success = false;

    do {
        const std::string utf8 = WideToUtf8(line);
        if (utf8.empty()) {
            break;
        }

        DWORD bytesWritten = 0;
        if (!WriteFile(
            hPipe,
            utf8.data(),
            static_cast<DWORD>(utf8.size()),
            &bytesWritten,
            nullptr) ||
            bytesWritten != static_cast<DWORD>(utf8.size())) {
            DebugLogFormat(L"[TunerSettingsTab] WriteFile failed cmd=%ls", command.c_str());
            break;
        }

        if (response == nullptr) {
            success = true;
            break;
        }

        response->clear();

        std::string pending;
        const ULONGLONG deadline = GetTickCount64() + 3000;

        while (GetTickCount64() < deadline) {
            char buf[2048]{};
            DWORD bytesRead = 0;

            BOOL ok = ReadFile(
                hPipe,
                buf,
                static_cast<DWORD>(sizeof(buf) - 1),
                &bytesRead,
                nullptr);

            if (!ok) {
                const DWORD err = GetLastError();

                if (err == ERROR_MORE_DATA && bytesRead > 0) {
                    pending.append(buf, buf + bytesRead);
                }
                else {
                    DebugLogFormat(
                        L"[TunerSettingsTab] ReadFile failed cmd=%ls err=%lu",
                        command.c_str(),
                        static_cast<unsigned long>(err));
                    break;
                }
            }
            else {
                if (bytesRead == 0) {
                    break;
                }
                pending.append(buf, buf + bytesRead);
            }

            for (;;) {
                const size_t eol = pending.find('\n');
                if (eol == std::string::npos) {
                    break;
                }

                std::string one = pending.substr(0, eol);
                pending.erase(0, eol + 1);

                while (!one.empty() && (one.back() == '\r' || one.back() == '\n')) {
                    one.pop_back();
                }
                if (one.empty()) {
                    continue;
                }

                const std::wstring wline = Utf8ToWide(one.c_str(), static_cast<int>(one.size()));
                DebugLogFormat(L"[TunerSettingsTab] TVTest recv: %ls", wline.c_str());

                // 接続直後通知はスキップ
                if (one.rfind("HELLO", 0) == 0) {
                    continue;
                }
                if (one.rfind("SNAP reason=\"CONNECTED\"", 0) == 0) {
                    continue;
                }

                if (!isExpectedReply(one)) {
                    continue;
                }

                *response = wline;
                success = true;
                break;
            }

            if (success) {
                break;
            }
        }

        if (!success && response->empty()) {
            *response = L"(response empty)";
        }

    } while (false);

    CloseHandle(hPipe);
    return success;
}

void TunerSettingsTab::RunTVTestLaunchTest()
{
    if (hWnd_ == nullptr) {
        return;
    }

    if (!IsChecked(hChkUseTVTest_)) {
        MessageBoxW(
            hWnd_,
            L"「TVTest連携を使用」をオンにしてから起動テストを実行してください。",
            L"TVTest起動テスト",
            MB_OK | MB_ICONINFORMATION);
        return;
    }

    const int tunerIndex = GetCurrentTunerIndex();

    const std::wstring exePath =
        TrimAndUnquote(GetWindowTextString(hEdTVTestExe_));

    const std::wstring tvtestBonDriverPath =
        GetTVTestBonDriverText();

    if (exePath.empty()) {
        MessageBoxW(
            hWnd_,
            L"TVTest.exe のパスが空です。",
            L"TVTest起動テスト",
            MB_OK | MB_ICONWARNING);
        return;
    }

    if (tvtestBonDriverPath.empty()) {
        MessageBoxW(
            hWnd_,
            L"TVTest用BonDriverのパスが空です。",
            L"TVTest起動テスト",
            MB_OK | MB_ICONWARNING);
        return;
    }

    TVTestIntegration bridge;
    TVTestIntegration::Settings settings{};

    settings.enabled = true;
    settings.useDidSuffix = false;

    // Tuner0 -> /DID 0, Tuner1 -> /DID 1
    // プラグイン側で TVTestBridge_{TUNER} が 0 / 1 に解決される。
    settings.didIndex = tunerIndex;

    settings.tvtestExePath = exePath;
    settings.bonDriverPath = tvtestBonDriverPath;

    settings.controlPipeName = GetTVTestControlPipeText();
    settings.tsPipeName = GetTVTestTsPipeText();

    settings.pipeTimeoutMs = 3000;
    settings.launchWaitMs = 3000;

    const std::filesystem::path exeFsPath(exePath);
    if (!exeFsPath.parent_path().empty()) {
        settings.tvtestWorkingDirectory = exeFsPath.parent_path().wstring();
    }

    bridge.Configure(settings);

    DebugLogFormat(
        L"[TunerSettingsTab][TVTest] Launch test request tuner=%d exe=%ls bon=%ls did=%d control=%ls ts=%ls",
        tunerIndex,
        exePath.c_str(),
        tvtestBonDriverPath.c_str(),
        settings.didIndex,
        settings.controlPipeName.c_str(),
        settings.tsPipeName.c_str());

    if (!bridge.LaunchTVTest()) {
        DebugLogFormat(
            L"[TunerSettingsTab][TVTest] Launch failed tuner=%d exe=%ls bon=%ls",
            tunerIndex,
            exePath.c_str(),
            tvtestBonDriverPath.c_str());

        MessageBoxW(
            hWnd_,
            L"TVTestの起動に失敗しました。",
            L"TVTest起動テスト",
            MB_OK | MB_ICONERROR);
        return;
    }

    MessageBoxW(
        hWnd_,
        L"TVTestの起動要求を送信しました。",
        L"TVTest起動テスト",
        MB_OK | MB_ICONINFORMATION);
}

void TunerSettingsTab::RunTVTestPingTest()
{
    std::wstring response;
    if (!SendTVTestCommand(L"PING", &response)) {
        DebugLogFormat(L"[TunerSettingsTab][TVTest] PING failed");
        return;
    }

    if (response.empty()) {
        response = L"(response empty)";
    }

    DebugLogFormat(L"[TunerSettingsTab][TVTest] PING => %ls", response.c_str());
}

void TunerSettingsTab::RunTVTestStateTest()
{
    std::wstring response;
    if (!SendTVTestCommand(L"GET_STATE", &response)) {
        DebugLogFormat(L"[TunerSettingsTab][TVTest] GET_STATE failed");
        return;
    }

    if (response.empty()) {
        response = L"(response empty)";
    }

    DebugLogFormat(L"[TunerSettingsTab][TVTest] GET_STATE => %ls", response.c_str());
}

void TunerSettingsTab::RunTVTestProgramTest()
{
    std::wstring response;
    if (!SendTVTestCommand(L"GET_PROGRAM", &response)) {
        DebugLogFormat(L"[TunerSettingsTab][TVTest] GET_PROGRAM failed");
        return;
    }

    if (response.empty()) {
        response = L"(response empty)";
    }

    DebugLogFormat(L"[TunerSettingsTab][TVTest] GET_PROGRAM => %ls", response.c_str());
}

void TunerSettingsTab::RunTVTestStatusDetailTest()
{
    std::wstring response;
    if (!SendTVTestCommand(L"GET_STATUS", &response)) {
        DebugLogFormat(L"[TunerSettingsTab][TVTest] GET_STATUS failed");
        return;
    }

    if (response.empty()) {
        response = L"(response empty)";
    }

    DebugLogFormat(L"[TunerSettingsTab][TVTest] GET_STATUS => %ls", response.c_str());
}

void TunerSettingsTab::RunTVTestReloadIniTest()
{
    std::wstring response;
    if (!SendTVTestCommand(L"RELOAD_INI", &response)) {
        DebugLogFormat(L"[TunerSettingsTab][TVTest] RELOAD_INI failed");
        return;
    }

    if (response.empty()) {
        response = L"(response empty)";
    }

    DebugLogFormat(L"[TunerSettingsTab][TVTest] RELOAD_INI => %ls", response.c_str());
}

void TunerSettingsTab::RunTVTestEnableTs()
{
    std::wstring response;
    if (!SendTVTestCommand(L"ENABLE_TS", &response)) {
        DebugLogFormat(L"[TunerSettingsTab][TVTest] ENABLE_TS failed");
        return;
    }

    if (response.empty()) {
        response = L"(response empty)";
    }

    DebugLogFormat(L"[TunerSettingsTab][TVTest] ENABLE_TS => %ls", response.c_str());
}

void TunerSettingsTab::RunTVTestDisableTs()
{
    std::wstring response;
    if (!SendTVTestCommand(L"DISABLE_TS", &response)) {
        DebugLogFormat(L"[TunerSettingsTab][TVTest] DISABLE_TS failed");
        return;
    }

    if (response.empty()) {
        response = L"(response empty)";
    }

    DebugLogFormat(L"[TunerSettingsTab][TVTest] DISABLE_TS => %ls", response.c_str());
}

int TunerSettingsTab::GetSelectedTVTestChannelIndex() const noexcept
{
    if (hCbTVTestChannelList_ == nullptr) {
        return -1;
    }

    const int sel = static_cast<int>(SendMessageW(hCbTVTestChannelList_, CB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(tvtestChannelEntries_.size())) {
        return -1;
    }
    return sel;
}

int TunerSettingsTab::GetSelectedTimeShiftChannelIndex() const noexcept
{
    if (hCbTimeShiftLockChannel_ == nullptr) {
        return -1;
    }

    const int sel = static_cast<int>(
        SendMessageW(hCbTimeShiftLockChannel_, CB_GETCURSEL, 0, 0));

    if (sel < 0 || sel >= static_cast<int>(timeShiftChannelEntries_.size())) {
        return -1;
    }

    return sel;
}

void TunerSettingsTab::ReloadTimeShiftChannelList()
{
    timeShiftChannelEntries_.clear();

    if (hCbTimeShiftLockChannel_ != nullptr) {
        SendMessageW(hCbTimeShiftLockChannel_, CB_RESETCONTENT, 0, 0);
    }

    const int spacesToTry[] = { 0, 1, 2 };

    for (int space : spacesToTry) {
        const std::filesystem::path chPath =
            BuildChannelListPathForSpace_(space);

        if (!FileExists_(chPath)) {
            continue;
        }

        std::ifstream ifs(chPath, std::ios::binary);
        if (!ifs) {
            continue;
        }

        std::vector<unsigned char> bytes(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());

        if (bytes.empty()) {
            continue;
        }

        const std::wstring text = DecodeTextFileToWide_(bytes);
        if (text.empty()) {
            continue;
        }

        std::wistringstream iss(text);
        std::wstring line;

        while (std::getline(iss, line)) {
            line = TrimCopy_(line);

            if (!line.empty() && line.front() == 0xFEFF) {
                line.erase(line.begin());
            }

            if (line.empty() || line.front() == L';') {
                continue;
            }

            // channellist_spaceX.ch:
            // ServiceName,Space,Channel,ServiceType,ServiceID,NetworkID,TSID,ONID,FreeCA,...
            std::vector<std::wstring> cols = SplitCsvLine_(line);
            if (cols.size() < 7) {
                continue;
            }

            int parsedSpace = 0;
            int channel = -1;
            int serviceType = 0;
            int serviceId = 0;
            int networkId = 0;
            int tsId = 0;

            if (!TryParseInt_(cols[1], parsedSpace)) continue;
            if (!TryParseInt_(cols[2], channel)) continue;
            if (!TryParseInt_(cols[3], serviceType)) continue;
            if (!TryParseInt_(cols[4], serviceId)) continue;
            if (!TryParseInt_(cols[5], networkId)) continue;
            if (!TryParseInt_(cols[6], tsId)) continue;

            // タイムシフト対象はまずテレビサービスだけに絞る。
            // 1=デジタルTV。192等のワンセグ/データ系は除外。
            if (serviceType != 1) {
                continue;
            }

            TimeShiftChannelEntry entry{};
            entry.serviceName = cols[0];
            entry.space = parsedSpace;
            entry.channel = channel;
            entry.serviceId = static_cast<std::uint16_t>(serviceId);
            entry.networkId = static_cast<std::uint16_t>(networkId);
            entry.tsId = static_cast<std::uint16_t>(tsId);

            entry.displayName =
                MakeTimeShiftChannelDisplayName_(
                    entry.serviceName,
                    entry.space,
                    entry.channel,
                    entry.serviceId,
                    entry.networkId,
                    entry.tsId);

            timeShiftChannelEntries_.push_back(std::move(entry));
        }
    }

    if (hCbTimeShiftLockChannel_ != nullptr) {
        for (const auto& entry : timeShiftChannelEntries_) {
            SendMessageW(
                hCbTimeShiftLockChannel_,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(entry.displayName.c_str()));
        }

        SendMessageW(
            hCbTimeShiftLockChannel_,
            CB_SETDROPPEDWIDTH,
            static_cast<WPARAM>(760),
            0);

        if (!timeShiftChannelEntries_.empty()) {
            SendMessageW(hCbTimeShiftLockChannel_, CB_SETCURSEL, 0, 0);
        }
    }

    DebugLogFormat(
        L"[TunerSettingsTab][TimeShift] channel list loaded count=%zu",
        timeShiftChannelEntries_.size());

}

void TunerSettingsTab::SelectTimeShiftLockChannelFromConfig(int tunerIndex)
{
    if (cfg_ == nullptr || hCbTimeShiftLockChannel_ == nullptr) {
        return;
    }

    if (timeShiftChannelEntries_.empty()) {
        ReloadTimeShiftChannelList();
    }

    const int lockSpace = cfg_->GetTunerTimeShiftLockSpace(tunerIndex);
    const int lockChannel = cfg_->GetTunerTimeShiftLockChannel(tunerIndex);
    const std::uint16_t lockServiceId =
        cfg_->GetTunerTimeShiftLockServiceId(tunerIndex);
    const std::uint16_t lockNetworkId =
        cfg_->GetTunerTimeShiftLockNetworkId(tunerIndex);
    const std::uint16_t lockTsId =
        cfg_->GetTunerTimeShiftLockTsId(tunerIndex);

    int selected = -1;

    for (int i = 0; i < static_cast<int>(timeShiftChannelEntries_.size()); ++i) {
        const auto& e = timeShiftChannelEntries_[static_cast<std::size_t>(i)];

        if (e.space == lockSpace &&
            e.channel == lockChannel &&
            e.serviceId == lockServiceId &&
            e.networkId == lockNetworkId &&
            e.tsId == lockTsId) {
            selected = i;
            break;
        }
    }

    if (selected < 0 && !timeShiftChannelEntries_.empty()) {
        selected = 0;
    }

    if (selected >= 0) {
        SendMessageW(
            hCbTimeShiftLockChannel_,
            CB_SETCURSEL,
            static_cast<WPARAM>(selected),
            0);
    }
}

void TunerSettingsTab::SaveTimeShiftLockChannelToConfig(int tunerIndex)
{
    if (cfg_ == nullptr) {
        return;
    }

    const int sel = GetSelectedTimeShiftChannelIndex();
    if (sel < 0 || sel >= static_cast<int>(timeShiftChannelEntries_.size())) {
        cfg_->SetTunerTimeShiftLockSpace(tunerIndex, 0);
        cfg_->SetTunerTimeShiftLockChannel(tunerIndex, -1);
        cfg_->SetTunerTimeShiftLockServiceId(tunerIndex, 0);
        cfg_->SetTunerTimeShiftLockNetworkId(tunerIndex, 0);
        cfg_->SetTunerTimeShiftLockTsId(tunerIndex, 0);
        cfg_->SetTunerTimeShiftLockName(tunerIndex, L"");
        return;
    }

    const TimeShiftChannelEntry& e =
        timeShiftChannelEntries_[static_cast<std::size_t>(sel)];

    cfg_->SetTunerTimeShiftLockSpace(tunerIndex, e.space);
    cfg_->SetTunerTimeShiftLockChannel(tunerIndex, e.channel);
    cfg_->SetTunerTimeShiftLockServiceId(tunerIndex, e.serviceId);
    cfg_->SetTunerTimeShiftLockNetworkId(tunerIndex, e.networkId);
    cfg_->SetTunerTimeShiftLockTsId(tunerIndex, e.tsId);
    cfg_->SetTunerTimeShiftLockName(tunerIndex, e.serviceName);

    DebugLogFormat(
        L"[TunerSettingsTab][TimeShift] saved tuner=%d space=%d ch=%d sid=%u nid=%u tsid=%u name=%ls",
        tunerIndex,
        e.space,
        e.channel,
        static_cast<unsigned>(e.serviceId),
        static_cast<unsigned>(e.networkId),
        static_cast<unsigned>(e.tsId),
        e.serviceName.c_str());
}

void TunerSettingsTab::ReloadTVTestChannelList()
{
    tvtestChannelEntries_.clear();

    if (hCbTVTestChannelList_ != nullptr) {
        SendMessageW(hCbTVTestChannelList_, CB_RESETCONTENT, 0, 0);
    }

    const std::filesystem::path ch2Path =
        BuildTVTestCh2Path_(
            GetWindowTextString(hEdTVTestExe_),
            GetTVTestBonDriverText());

    if (ch2Path.empty()) {
        DebugLogFormat(L"[TunerSettingsTab][TVTest] .ch2 path build failed");
        return;
    }

    std::ifstream ifs(ch2Path, std::ios::binary);
    if (!ifs) {
        DebugLogFormat(L"[TunerSettingsTab][TVTest] .ch2 open failed: %ls", ch2Path.wstring().c_str());
        return;
    }

    std::vector<unsigned char> bytes(
        (std::istreambuf_iterator<char>(ifs)),
        std::istreambuf_iterator<char>());

    if (bytes.empty()) {
        DebugLogFormat(L"[TunerSettingsTab][TVTest] .ch2 is empty: %ls", ch2Path.wstring().c_str());
        return;
    }

    std::wstring text = DecodeTextFileToWide_(bytes);
    if (text.empty()) {
        DebugLogFormat(L"[TunerSettingsTab][TVTest] .ch2 decode failed: %ls", ch2Path.wstring().c_str());
        return;
    }

    std::wistringstream iss(text);
    std::wstring line;

    while (std::getline(iss, line)) {
        line = TrimCopy_(line);

        if (line.empty()) {
            continue;
        }
        if (!line.empty() && line.front() == 0xFEFF) {
            line.erase(line.begin());
        }
        if (line.empty() || line.front() == L';') {
            continue;
        }

        // .ch2:
        // 名称, space, channel, remocon, ?, serviceId, networkId, tsid, status
        std::vector<std::wstring> cols = SplitCsvLine_(line);
        if (cols.size() < 9) {
            continue;
        }

        int space = 0;
        int channel = 0;
        int serviceId = 0;
        int networkId = 0;
        int tsid = 0;
        int status = 0;

        if (!TryParseInt_(cols[1], space)) continue;
        if (!TryParseInt_(cols[2], channel)) continue;
        if (!TryParseInt_(cols[5], serviceId)) continue;
        if (!TryParseInt_(cols[6], networkId)) continue;
        if (!TryParseInt_(cols[7], tsid)) continue;
        if (!TryParseInt_(cols[8], status)) continue;

        // TVTest 側で有効なチャンネルだけ
        if (status != 1) {
            continue;
        }

        TVTestChannelEntry entry{};
        entry.serviceName = cols[0];
        entry.space = space;
        entry.channel = channel;
        entry.serviceId = static_cast<std::uint16_t>(serviceId);
        entry.networkId = static_cast<std::uint16_t>(networkId);
        entry.tsid = static_cast<std::uint16_t>(tsid);
        entry.status = status;

        entry.displayName =
            entry.serviceName +
            L"  [Space=" + std::to_wstring(entry.space) +
            L" Ch=" + std::to_wstring(entry.channel) +
            L" SID=" + std::to_wstring(static_cast<unsigned>(entry.serviceId)) +
            L"]";

        tvtestChannelEntries_.push_back(entry);
    }

    if (hCbTVTestChannelList_ != nullptr) {
        for (const auto& entry : tvtestChannelEntries_) {
            SendMessageW(
                hCbTVTestChannelList_,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(entry.displayName.c_str()));
        }

        if (!tvtestChannelEntries_.empty()) {
            SendMessageW(hCbTVTestChannelList_, CB_SETCURSEL, 0, 0);
        }
    }

    DebugLogFormat(
        L"[TunerSettingsTab][TVTest] .ch2 loaded: count=%zu path=%ls",
        tvtestChannelEntries_.size(),
        ch2Path.wstring().c_str());
}

void TunerSettingsTab::RunTVTestSetChannelTest()
{
    if (tvtestChannelEntries_.empty()) {
        ReloadTVTestChannelList();
    }

    const int sel = GetSelectedTVTestChannelIndex();
    if (sel < 0 || sel >= static_cast<int>(tvtestChannelEntries_.size())) {
        DebugLogFormat(L"[TunerSettingsTab][TVTest] no channel selected");
        return;
    }

    const TVTestChannelEntry& entry = tvtestChannelEntries_[static_cast<size_t>(sel)];

    wchar_t cmd[160]{};
    _snwprintf_s(
        cmd,
        _countof(cmd),
        _TRUNCATE,
        L"SET_CHANNEL_BY_IDS %d %u %u %u",
        entry.space,
        static_cast<unsigned>(entry.networkId),
        static_cast<unsigned>(entry.tsid),
        static_cast<unsigned>(entry.serviceId));

    std::wstring response;
    if (!SendTVTestCommand(cmd, &response)) {
        DebugLogFormat(
            L"[TunerSettingsTab][TVTest] %ls failed | target=%ls",
            cmd,
            entry.displayName.c_str());
        return;
    }

    if (response.empty()) {
        response = L"(response empty)";
    }

    DebugLogFormat(
        L"[TunerSettingsTab][TVTest] %ls => %ls | target=%ls",
        cmd,
        response.c_str(),
        entry.displayName.c_str());

    ::Sleep(1000);

    std::wstring stateResponse;
    if (SendTVTestCommand(L"GET_STATE", &stateResponse)) {
        if (stateResponse.empty()) {
            stateResponse = L"(response empty)";
        }
        DebugLogFormat(L"[TunerSettingsTab][TVTest] GET_STATE(after set) => %ls", stateResponse.c_str());
    }
    else {
        DebugLogFormat(L"[TunerSettingsTab][TVTest] GET_STATE(after set) failed");
    }

    std::wstring programResponse;
    if (SendTVTestCommand(L"GET_PROGRAM", &programResponse)) {
        if (programResponse.empty()) {
            programResponse = L"(response empty)";
        }
        DebugLogFormat(L"[TunerSettingsTab][TVTest] GET_PROGRAM(after set) => %ls", programResponse.c_str());
    }
    else {
        DebugLogFormat(L"[TunerSettingsTab][TVTest] GET_PROGRAM(after set) failed");
    }
}

void TunerSettingsTab::RunTVTestWatchReservationFlowTest()
{
    if (hWnd_ == nullptr) {
        return;
    }

    if (!IsChecked(hChkUseTVTest_)) {
        MessageBoxW(
            hWnd_,
            L"「TVTest連携を使用」をオンにしてから実行してください。",
            L"視聴予約動作テスト",
            MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (tvtestChannelEntries_.empty()) {
        ReloadTVTestChannelList();
    }

    const int sel = GetSelectedTVTestChannelIndex();
    if (sel < 0 || sel >= static_cast<int>(tvtestChannelEntries_.size())) {
        MessageBoxW(
            hWnd_,
            L"TVTestチャンネル一覧からテスト対象のチャンネルを選択してください。",
            L"視聴予約動作テスト",
            MB_OK | MB_ICONWARNING);
        return;
    }

    const TVTestChannelEntry& entry =
        tvtestChannelEntries_[static_cast<std::size_t>(sel)];

    const int tunerIndex = GetCurrentTunerIndex();

    // UI上の現在値をConfigManager側へ反映しておく。
    // config.iniへの保存処理は既存の保存ルートに任せる。
    SavePageToConfig(tunerIndex);

    const std::filesystem::path moduleDir = GetModuleDirectoryPath();
    if (moduleDir.empty()) {
        MessageBoxW(
            hWnd_,
            L"実行ファイルのフォルダを取得できませんでした。",
            L"視聴予約動作テスト",
            MB_OK | MB_ICONERROR);
        return;
    }

    const std::filesystem::path configPath = moduleDir / L"config.ini";

    std::error_code ec;
    if (!std::filesystem::exists(configPath, ec) || ec) {
        std::wstring msg = L"config.ini が見つかりません。\n\n";
        msg += configPath.wstring();

        MessageBoxW(
            hWnd_,
            msg.c_str(),
            L"視聴予約動作テスト",
            MB_OK | MB_ICONERROR);
        return;
    }

    ReservationTvTestWatchTest::Request req{};

    req.tunerIndex = tunerIndex;

    req.space = entry.space;
    req.channel = entry.channel;
    req.networkId = entry.networkId;
    req.tsId = entry.tsid;
    req.serviceId = entry.serviceId;
    req.eventId = 0;

    req.serviceName = entry.serviceName;

    // チューナータブからの手動試験では、予約番組名をまだ持っていないため、
    // まずはチャンネル一致を確認する。
    // 本番予約接続時は tasks_exec_index.tsv / tasks_view_index.tsv 由来の title/eventId を渡す。
    req.title.clear();
    req.requireProgramTitleMatch = false;

    req.launchIfNeeded = true;

    // 試験用。15秒だけ監視して終了する。
    // 既に起動していたTVTestは閉じず、このテストで起動した場合だけ閉じる。
    req.closeTvTestOnFinish = true;
    req.closeOnlyIfLaunchedByThisTest = true;
    req.closeOnMismatch = true;

    req.afterSettleMs = 1000;
    req.verifyTimeoutMs = 15000;
    req.monitorPollMs = 3000;
    req.maxMonitorMs = 15000;
    req.mismatchLimit = 2;

    DebugLogFormat(
        L"[TunerSettingsTab][TVTest][WATCH-FLOW-TEST] begin tuner=%d target=%ls space=%d ch=%d nid=%u tsid=%u sid=%u config=%ls",
        tunerIndex,
        entry.displayName.c_str(),
        entry.space,
        entry.channel,
        static_cast<unsigned>(entry.networkId),
        static_cast<unsigned>(entry.tsid),
        static_cast<unsigned>(entry.serviceId),
        configPath.wstring().c_str());

    const auto result =
        ReservationTvTestWatchTest::RunWatchTestFromConfig(
            configPath.wstring(),
            req);

    DebugLogFormat(
        L"[TunerSettingsTab][TVTest][WATCH-FLOW-TEST] result ok=%d status=%d stage=%ls msg=%ls set=%ls state=%ls statusText=%ls program=%ls close=%ls",
        result.ok ? 1 : 0,
        static_cast<int>(result.status),
        result.stage.c_str(),
        result.message.c_str(),
        result.setChannelResponse.c_str(),
        result.stateResponse.c_str(),
        result.statusResponse.c_str(),
        result.programResponse.c_str(),
        result.closeResponse.c_str());

    wchar_t msg[2048]{};
    _snwprintf_s(
        msg,
        _countof(msg),
        _TRUNCATE,
        L"視聴予約動作テストを終了しました。\n\n"
        L"結果: %ls\n"
        L"段階: %ls\n"
        L"内容: %ls\n\n"
        L"対象: %ls\n"
        L"SET_CHANNEL: %ls\n"
        L"TVTest終了送信: %ls",
        result.ok ? L"成功" : L"失敗",
        result.stage.c_str(),
        result.message.c_str(),
        entry.displayName.c_str(),
        result.setChannelResponse.empty() ? L"(empty)" : result.setChannelResponse.c_str(),
        result.closeSent ? L"あり" : L"なし");

    MessageBoxW(
        hWnd_,
        msg,
        L"視聴予約動作テスト",
        result.ok ? (MB_OK | MB_ICONINFORMATION) : (MB_OK | MB_ICONWARNING));
}

void TunerSettingsTab::RunTVTestTsReadTest()
{
    // まず TS を有効化しておく
    std::wstring enableResponse;
    if (!SendTVTestCommand(L"ENABLE_TS", &enableResponse)) {
        DebugLogFormat(L"[TunerSettingsTab][TVTest] ENABLE_TS before TS read failed");
        return;
    }

    if (enableResponse.empty()) {
        enableResponse = L"(response empty)";
    }

    DebugLogFormat(L"[TunerSettingsTab][TVTest] ENABLE_TS(before read) => %ls", enableResponse.c_str());


    ::Sleep(1000);

    TVTestIntegration bridge;
    TVTestIntegration::Settings settings{};
    settings.enabled = true;
    settings.useDidSuffix = false; // 入力欄のパイプ名をそのまま使う
    settings.tvtestExePath = TrimAndUnquote(GetWindowTextString(hEdTVTestExe_));
    settings.controlPipeName = GetTVTestControlPipeText();
    settings.didIndex = GetCurrentTunerIndex();
    settings.bonDriverPath = GetTVTestBonDriverText();
    settings.tsPipeName = GetTVTestTsPipeText();
    settings.pipeTimeoutMs = 3000;
    settings.launchWaitMs = 3000;

    bridge.Configure(settings);

    if (!bridge.OpenTsPipeSession()) {
        DebugLogFormat(L"[TunerSettingsTab][TVTest] TS pipe open failed: %ls", settings.tsPipeName.c_str());
        return;
    }

    TVTestIntegration::TsFrame frame;
    const bool ok = bridge.ReadNextTsFrameWithTimeout(frame, 12000);
    bridge.CloseTsPipeSession();

    if (!ok) {
        DebugLogFormat(L"[TunerSettingsTab][TVTest] TS frame read timeout/fail");
        return;
    }

    const bool magicOk = (frame.header.magic == 0x31534254u); // 'TSB1'
    const bool aligned188 = (frame.header.payloadBytes % 188u) == 0u;
    const std::size_t packetCount = aligned188 ? static_cast<std::size_t>(frame.header.payloadBytes / 188u) : 0u;

    std::size_t syncOkCount = 0;
    const std::size_t checkCount = std::min<std::size_t>(packetCount, 8u);
    for (std::size_t i = 0; i < checkCount; ++i) {
        const std::size_t pos = i * 188u;
        if (pos < frame.payload.size() && frame.payload[pos] == 0x47u) {
            ++syncOkCount;
        }
    }

    DebugLogFormat(
        L"[TunerSettingsTab][TVTest] TS frame: magic=0x%08X payload=%u aligned188=%d packets=%zu sync47=%zu/%zu flags=0x%08X",
        static_cast<unsigned>(frame.header.magic),
        static_cast<unsigned>(frame.header.payloadBytes),
        aligned188 ? 1 : 0,
        packetCount,
        syncOkCount,
        checkCount,
        static_cast<unsigned>(frame.header.flags));
}