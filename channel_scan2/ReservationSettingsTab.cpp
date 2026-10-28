#include "ReservationSettingsTab.hpp"

#include <CommCtrl.h>
#include <algorithm>
#include <cwchar>

// Windows.h の max/min マクロ衝突対策（確実に潰す）
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace {
    enum : int {
        IDC_RES_START_ADV_ED = 43001,
        IDC_RES_START_ADV_SPIN = 43002,

        IDC_RES_START_ADJ_ED = 43003,
        IDC_RES_START_ADJ_SPIN = 43004,

        IDC_RES_END_ADJ_ED = 43005,
        IDC_RES_END_ADJ_SPIN = 43006,

        IDC_RES_TUNER_COMBO = 43007,
        IDC_RES_CONFLICT_COMBO = 43008,

        IDC_RES_PAUSE_EPG_CHK = 43009,

        IDC_RES_APPLY_BTN = 43010,
        IDC_RES_RELOAD_BTN = 43011,

        IDC_RES_EPG_BUTTON_MODE_COMBO = 43012,
    };

    int ClampInt(int v, int lo, int hi) noexcept {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    }

    void SetEditInt(HWND hEdit, int v) {
        wchar_t buf[64]{};
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d", v);
        SetWindowTextW(hEdit, buf);
    }

    int GetEditInt(HWND hEdit, int defVal) {
        wchar_t buf[128]{};
        GetWindowTextW(hEdit, buf, _countof(buf));
        wchar_t* end = nullptr;
        long v = std::wcstol(buf, &end, 10);
        if (end == buf) return defVal;
        if (v < INT_MIN) return INT_MIN;
        if (v > INT_MAX) return INT_MAX;
        return static_cast<int>(v);
    }

    void SetUpDownRange(HWND hSpin, int low, int high) {
        SendMessageW(hSpin, UDM_SETRANGE32, (WPARAM)low, (LPARAM)high);
    }

    void CheckBoxSet(HWND hChk, bool on) {
        SendMessageW(hChk, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    bool CheckBoxGet(HWND hChk) {
        return (SendMessageW(hChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
    }
}

void ReservationSettingsTab::ApplyFont_(HWND h) const noexcept {
    if (!h || !m_hFont) return;
    SendMessageW(h, WM_SETFONT, (WPARAM)m_hFont, TRUE);
}

bool ReservationSettingsTab::Create(HWND hParent, const RECT& rcPage, HINSTANCE hInst, HFONT hFont,
    ConfigManager& cfg, int tunerCount, const std::wstring& title) {
    m_hParent = hParent;
    m_hInst = hInst;
    m_hFont = hFont;
    m_cfg = &cfg;
    m_tunerCount = (tunerCount < 1) ? 1 : tunerCount;

    static bool s_registered = false;
    if (!s_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &ReservationSettingsTab::PageWndProc_;
        wc.hInstance = m_hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); // ★灰色に統一
        wc.lpszClassName = L"ReservationSettingsTabPage";

        if (!RegisterClassExW(&wc)) {
            const DWORD err = GetLastError();
            // ★重要：既に登録済みはOK
            if (err != ERROR_CLASS_ALREADY_EXISTS) {
                return false;
            }
        }
        s_registered = true;
    }

    const int w = (rcPage.right - rcPage.left);
    const int h = (rcPage.bottom - rcPage.top);

    m_hPage = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        L"ReservationSettingsTabPage",
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        rcPage.left, rcPage.top, w, h,
        m_hParent,
        nullptr,
        m_hInst,
        this
    );
    if (!m_hPage) return false;

    ApplyFont_(m_hPage);

    if (!CreateControls_(title)) return false;

    LoadFromConfig_();
    ApplyToControls_();
    return true;
}

void ReservationSettingsTab::Move(const RECT& rcPage) {
    if (!m_hPage) return;
    const int w = (rcPage.right - rcPage.left);
    const int h = (rcPage.bottom - rcPage.top);
    MoveWindow(m_hPage, rcPage.left, rcPage.top, w, h, TRUE);
}

void ReservationSettingsTab::Show(bool visible) noexcept {
    if (!m_hPage) return;
    ShowWindow(m_hPage, visible ? SW_SHOW : SW_HIDE);
}

void ReservationSettingsTab::ApplyToConfig() {
    WriteToConfig_();
}

void ReservationSettingsTab::ReloadFromConfig() {
    LoadFromConfig_();
    ApplyToControls_();
}

void ReservationSettingsTab::UpdateTunerCount(int tunerCount) {
    m_tunerCount = (tunerCount < 1) ? 1 : tunerCount;
    RebuildTunerCombo_(m_tunerCount);
    if (m_preferredTuner >= m_tunerCount) m_preferredTuner = -1;
    ApplyToControls_();
}

bool ReservationSettingsTab::CreateControls_(const std::wstring& title) {
    RECT rc{};
    GetClientRect(m_hPage, &rc);

    const int pad = 12;
    int x = pad;
    int y = pad;

    const int pageW = (std::max)(360, (int)((rc.right - rc.left) - 24));
    const int lineH = 24;
    const int labelW = 300;
    const int editW = 90;

    m_hTitle = CreateWindowExW(
        0, L"STATIC", title.c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x + 4, y, pageW - 8, lineH,
        m_hPage, nullptr, m_hInst, nullptr
    );
    ApplyFont_(m_hTitle);

    y += 22;

    m_hSepLine = CreateWindowExW(
        0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
        x + 4, y, pageW - 8, 2,
        m_hPage, nullptr, m_hInst, nullptr
    );
    ApplyFont_(m_hSepLine);

    y += 14;

    // ---- StartAdvanceSec ----
    m_hStartAdvanceLabel = CreateWindowExW(
        0, L"STATIC", L"開始前に起動（秒） [0..600]",
        WS_CHILD | WS_VISIBLE,
        x + 12, y, labelW, lineH,
        m_hPage, nullptr, m_hInst, nullptr
    );
    ApplyFont_(m_hStartAdvanceLabel);

    m_hStartAdvanceEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"30",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
        x + 12 + labelW + 8, y, editW, lineH,
        m_hPage, (HMENU)(INT_PTR)IDC_RES_START_ADV_ED, m_hInst, nullptr
    );
    ApplyFont_(m_hStartAdvanceEdit);

    m_hStartAdvanceSpin = CreateWindowExW(
        0, UPDOWN_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_SETBUDDYINT,
        0, 0, 0, 0,
        m_hPage, (HMENU)(INT_PTR)IDC_RES_START_ADV_SPIN, m_hInst, nullptr
    );
    SendMessageW(m_hStartAdvanceSpin, UDM_SETBUDDY, (WPARAM)m_hStartAdvanceEdit, 0);
    SetUpDownRange(m_hStartAdvanceSpin, kStartAdvanceMinSec, kStartAdvanceMaxSec);

    y += 34;

    // ---- StartAdjustSec ----
    m_hStartAdjustLabel = CreateWindowExW(
        0, L"STATIC", L"開始時刻 微調整（秒） [-1..+1]",
        WS_CHILD | WS_VISIBLE,
        x + 12, y, labelW, lineH,
        m_hPage, nullptr, m_hInst, nullptr
    );
    ApplyFont_(m_hStartAdjustLabel);

    m_hStartAdjustEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"0",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        x + 12 + labelW + 8, y, editW, lineH,
        m_hPage, (HMENU)(INT_PTR)IDC_RES_START_ADJ_ED, m_hInst, nullptr
    );
    ApplyFont_(m_hStartAdjustEdit);

    m_hStartAdjustSpin = CreateWindowExW(
        0, UPDOWN_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_SETBUDDYINT,
        0, 0, 0, 0,
        m_hPage, (HMENU)(INT_PTR)IDC_RES_START_ADJ_SPIN, m_hInst, nullptr
    );
    SendMessageW(m_hStartAdjustSpin, UDM_SETBUDDY, (WPARAM)m_hStartAdjustEdit, 0);
    SetUpDownRange(m_hStartAdjustSpin, kStartAdjustMinSec, kStartAdjustMaxSec);

    y += 34;

    // ---- EndAdjustMin ----
    m_hEndAdjustLabel = CreateWindowExW(
        0, L"STATIC", L"終了時刻 調整（分） [-30..+180]",
        WS_CHILD | WS_VISIBLE,
        x + 12, y, labelW, lineH,
        m_hPage, nullptr, m_hInst, nullptr
    );
    ApplyFont_(m_hEndAdjustLabel);

    m_hEndAdjustEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"0",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        x + 12 + labelW + 8, y, editW, lineH,
        m_hPage, (HMENU)(INT_PTR)IDC_RES_END_ADJ_ED, m_hInst, nullptr
    );
    ApplyFont_(m_hEndAdjustEdit);

    m_hEndAdjustSpin = CreateWindowExW(
        0, UPDOWN_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_SETBUDDYINT,
        0, 0, 0, 0,
        m_hPage, (HMENU)(INT_PTR)IDC_RES_END_ADJ_SPIN, m_hInst, nullptr
    );
    SendMessageW(m_hEndAdjustSpin, UDM_SETBUDDY, (WPARAM)m_hEndAdjustEdit, 0);
    SetUpDownRange(m_hEndAdjustSpin, kEndAdjustMinMin, kEndAdjustMaxMin);

    y += 34;

    // ---- PreferredTuner ----
    m_hPreferredTunerLabel = CreateWindowExW(
        0, L"STATIC", L"優先チューナー",
        WS_CHILD | WS_VISIBLE,
        x + 12, y, labelW, lineH,
        m_hPage, nullptr, m_hInst, nullptr
    );
    ApplyFont_(m_hPreferredTunerLabel);

    m_hPreferredTunerCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        x + 12 + labelW + 8, y, 240, 200,
        m_hPage, (HMENU)(INT_PTR)IDC_RES_TUNER_COMBO, m_hInst, nullptr
    );
    ApplyFont_(m_hPreferredTunerCombo);
    RebuildTunerCombo_(m_tunerCount);

    y += 34;

    // ---- ConflictPolicy ----
    m_hConflictLabel = CreateWindowExW(
        0, L"STATIC", L"競合時ポリシー",
        WS_CHILD | WS_VISIBLE,
        x + 12, y, labelW, lineH,
        m_hPage, nullptr, m_hInst, nullptr
    );
    ApplyFont_(m_hConflictLabel);

    m_hConflictCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        x + 12 + labelW + 8, y, 360, 200,
        m_hPage, (HMENU)(INT_PTR)IDC_RES_CONFLICT_COMBO, m_hInst, nullptr
    );
    ApplyFont_(m_hConflictCombo);
    RebuildConflictCombo_();

    y += 34;

    // ---- EpgReserveButtonMode ----
    m_hEpgReserveButtonModeLabel = CreateWindowExW(
        0, L"STATIC", L"EPG予約ボタンの動作",
        WS_CHILD | WS_VISIBLE,
        x + 12, y, labelW, lineH,
        m_hPage, nullptr, m_hInst, nullptr
    );
    ApplyFont_(m_hEpgReserveButtonModeLabel);

    m_hEpgReserveButtonModeCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        x + 12 + labelW + 8, y, 360, 200,
        m_hPage, (HMENU)(INT_PTR)IDC_RES_EPG_BUTTON_MODE_COMBO, m_hInst, nullptr
    );
    ApplyFont_(m_hEpgReserveButtonModeCombo);
    RebuildEpgReserveButtonModeCombo_();

    y += 34;

    // ---- PauseEpgWhileRecording ----
    m_hPauseEpgCheck = CreateWindowExW(
        0, L"BUTTON", L"録画（予約）中はEPG更新を一時停止",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        x + 12, y, 480, lineH,
        m_hPage, (HMENU)(INT_PTR)IDC_RES_PAUSE_EPG_CHK, m_hInst, nullptr
    );
    ApplyFont_(m_hPauseEpgCheck);

    y += 42;

    // ---- Buttons ----
    m_hApplyBtn = CreateWindowExW(
        0, L"BUTTON", L"適用（config.iniへ保存）",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x + 12, y, 220, 28,
        m_hPage, (HMENU)(INT_PTR)IDC_RES_APPLY_BTN, m_hInst, nullptr
    );
    ApplyFont_(m_hApplyBtn);

    m_hReloadBtn = CreateWindowExW(
        0, L"BUTTON", L"再読込",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x + 12 + 240, y, 120, 28,
        m_hPage, (HMENU)(INT_PTR)IDC_RES_RELOAD_BTN, m_hInst, nullptr
    );
    ApplyFont_(m_hReloadBtn);

    return true;
}

void ReservationSettingsTab::RebuildTunerCombo_(int tunerCount) {
    if (!m_hPreferredTunerCombo) return;

    SendMessageW(m_hPreferredTunerCombo, CB_RESETCONTENT, 0, 0);

    LRESULT idx = SendMessageW(m_hPreferredTunerCombo, CB_ADDSTRING, 0, (LPARAM)L"Auto");
    SendMessageW(m_hPreferredTunerCombo, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)-1);

    const int n = (tunerCount < 1) ? 1 : tunerCount;
    for (int i = 0; i < n; ++i) {
        wchar_t item[32]{};
        _snwprintf_s(item, _countof(item), _TRUNCATE, L"%d", i);
        LRESULT ii = SendMessageW(m_hPreferredTunerCombo, CB_ADDSTRING, 0, (LPARAM)item);
        SendMessageW(m_hPreferredTunerCombo, CB_SETITEMDATA, (WPARAM)ii, (LPARAM)i);
    }

    SendMessageW(m_hPreferredTunerCombo, CB_SETCURSEL, 0, 0);
}

void ReservationSettingsTab::RebuildConflictCombo_() {
    if (!m_hConflictCombo) return;

    SendMessageW(m_hConflictCombo, CB_RESETCONTENT, 0, 0);

    auto addItem = [&](const wchar_t* text, int policyVal) {
        LRESULT idx = SendMessageW(m_hConflictCombo, CB_ADDSTRING, 0, (LPARAM)text);
        SendMessageW(m_hConflictCombo, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)policyVal);
        };

    addItem(L"空きチューナーへ自動で逃がす（推奨）", 0);
    addItem(L"優先チューナーを最優先（空きが無ければ次へ）", 1);
    addItem(L"競合したら失敗（安全・確実）", 2);

    SendMessageW(m_hConflictCombo, CB_SETCURSEL, 0, 0);
}

void ReservationSettingsTab::RebuildEpgReserveButtonModeCombo_() {
    if (!m_hEpgReserveButtonModeCombo) return;

    SendMessageW(m_hEpgReserveButtonModeCombo, CB_RESETCONTENT, 0, 0);

    auto addItem = [&](const wchar_t* text, int modeVal) {
        LRESULT idx = SendMessageW(m_hEpgReserveButtonModeCombo, CB_ADDSTRING, 0, (LPARAM)text);
        SendMessageW(m_hEpgReserveButtonModeCombo, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)modeVal);
        };

    addItem(L"押したときに「録画 / 視聴」を選ぶ", static_cast<int>(ConfigManager::EpgReserveButtonMode::Select));
    addItem(L"押したら「録画予約」", static_cast<int>(ConfigManager::EpgReserveButtonMode::Record));
    addItem(L"押したら「視聴予約」", static_cast<int>(ConfigManager::EpgReserveButtonMode::Watch));

    SendMessageW(m_hEpgReserveButtonModeCombo, CB_SETCURSEL, 0, 0);
}

void ReservationSettingsTab::LoadFromConfig_() {
    if (!m_cfg) return;

    m_startAdvanceSec = m_cfg->GetReservationStartAdvanceSec();
    m_startAdjustSec = m_cfg->GetReservationStartAdjustSec();
    m_endAdjustMin = m_cfg->GetReservationEndAdjustMin();
    m_preferredTuner = m_cfg->GetReservationPreferredTuner();
    m_conflictPolicy = m_cfg->GetReservationConflictPolicy();
    m_pauseEpgWhileRecording = m_cfg->GetReservationPauseEpgWhileRecording();
    m_epgReserveButtonMode = m_cfg->GetReservationEpgReserveButtonMode();

    m_startAdvanceSec = ClampInt(m_startAdvanceSec, kStartAdvanceMinSec, kStartAdvanceMaxSec);
    m_startAdjustSec = ClampInt(m_startAdjustSec, kStartAdjustMinSec, kStartAdjustMaxSec);
    m_endAdjustMin = ClampInt(m_endAdjustMin, kEndAdjustMinMin, kEndAdjustMaxMin);

    if (m_preferredTuner < -1) m_preferredTuner = -1;
    if (m_preferredTuner >= m_tunerCount) m_preferredTuner = -1;
}

void ReservationSettingsTab::ApplyToControls_() {
    if (!m_hPage) return;

    SetEditInt(m_hStartAdvanceEdit, m_startAdvanceSec);
    SetEditInt(m_hStartAdjustEdit, m_startAdjustSec);
    SetEditInt(m_hEndAdjustEdit, m_endAdjustMin);

    // PreferredTuner
    {
        int sel = 0;
        const LRESULT cnt = SendMessageW(m_hPreferredTunerCombo, CB_GETCOUNT, 0, 0);
        for (LRESULT i = 0; i < cnt; ++i) {
            const int data = (int)SendMessageW(m_hPreferredTunerCombo, CB_GETITEMDATA, (WPARAM)i, 0);
            if (data == m_preferredTuner) { sel = (int)i; break; }
        }
        SendMessageW(m_hPreferredTunerCombo, CB_SETCURSEL, (WPARAM)sel, 0);
    }

    // ConflictPolicy
    {
        const int target = (int)m_conflictPolicy;
        int sel = 0;
        const LRESULT cnt = SendMessageW(m_hConflictCombo, CB_GETCOUNT, 0, 0);
        for (LRESULT i = 0; i < cnt; ++i) {
            const int data = (int)SendMessageW(m_hConflictCombo, CB_GETITEMDATA, (WPARAM)i, 0);
            if (data == target) { sel = (int)i; break; }
        }
        SendMessageW(m_hConflictCombo, CB_SETCURSEL, (WPARAM)sel, 0);
    }

    // EpgReserveButtonMode
    {
        const int target = (int)m_epgReserveButtonMode;
        int sel = 0;
        const LRESULT cnt = SendMessageW(m_hEpgReserveButtonModeCombo, CB_GETCOUNT, 0, 0);
        for (LRESULT i = 0; i < cnt; ++i) {
            const int data = (int)SendMessageW(m_hEpgReserveButtonModeCombo, CB_GETITEMDATA, (WPARAM)i, 0);
            if (data == target) { sel = (int)i; break; }
        }
        SendMessageW(m_hEpgReserveButtonModeCombo, CB_SETCURSEL, (WPARAM)sel, 0);
    }

    CheckBoxSet(m_hPauseEpgCheck, m_pauseEpgWhileRecording);
}

void ReservationSettingsTab::ReadFromControls_() {
    m_startAdvanceSec = ClampInt(GetEditInt(m_hStartAdvanceEdit, 30), kStartAdvanceMinSec, kStartAdvanceMaxSec);
    m_startAdjustSec = ClampInt(GetEditInt(m_hStartAdjustEdit, 0), kStartAdjustMinSec, kStartAdjustMaxSec);
    m_endAdjustMin = ClampInt(GetEditInt(m_hEndAdjustEdit, 0), kEndAdjustMinMin, kEndAdjustMaxMin);

    {
        int preferred = -1;
        const LRESULT sel = SendMessageW(m_hPreferredTunerCombo, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) {
            preferred = (int)SendMessageW(m_hPreferredTunerCombo, CB_GETITEMDATA, (WPARAM)sel, 0);
        }
        if (preferred < -1) preferred = -1;
        if (preferred >= m_tunerCount) preferred = -1;
        m_preferredTuner = preferred;
    }

    {
        int cp = 0;
        const LRESULT sel = SendMessageW(m_hConflictCombo, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) {
            cp = (int)SendMessageW(m_hConflictCombo, CB_GETITEMDATA, (WPARAM)sel, 0);
        }
        cp = ClampInt(cp, 0, 2);
        m_conflictPolicy = static_cast<ConfigManager::ReservationConflictPolicy>(cp);
    }

    {
        int mode = static_cast<int>(ConfigManager::EpgReserveButtonMode::Select);
        const LRESULT sel = SendMessageW(m_hEpgReserveButtonModeCombo, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) {
            mode = (int)SendMessageW(m_hEpgReserveButtonModeCombo, CB_GETITEMDATA, (WPARAM)sel, 0);
        }
        mode = ClampInt(mode, 0, 2);
        m_epgReserveButtonMode = static_cast<ConfigManager::EpgReserveButtonMode>(mode);
    }

    m_pauseEpgWhileRecording = CheckBoxGet(m_hPauseEpgCheck);

    ApplyToControls_();
}

void ReservationSettingsTab::WriteToConfig_() {
    if (!m_cfg) return;

    ReadFromControls_();

    m_cfg->SetReservationStartAdvanceSec(m_startAdvanceSec);
    m_cfg->SetReservationStartAdjustSec(m_startAdjustSec);
    m_cfg->SetReservationEndAdjustMin(m_endAdjustMin);
    m_cfg->SetReservationPreferredTuner(m_preferredTuner);
    m_cfg->SetReservationConflictPolicy(m_conflictPolicy);
    m_cfg->SetReservationPauseEpgWhileRecording(m_pauseEpgWhileRecording);
    m_cfg->SetReservationEpgReserveButtonMode(m_epgReserveButtonMode);
}

LRESULT CALLBACK ReservationSettingsTab::PageWndProc_(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ReservationSettingsTab* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<ReservationSettingsTab*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    self = reinterpret_cast<ReservationSettingsTab*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    return self ? self->PageWndProcImpl_(hWnd, msg, wParam, lParam)
        : DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT ReservationSettingsTab::PageWndProcImpl_(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc{};
        GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);

        if (id == IDC_RES_APPLY_BTN && code == BN_CLICKED) {
            ApplyToConfig();
            if (m_cfg) {
                m_cfg->SaveConfig();
            }
            return 0;
        }
        if (id == IDC_RES_RELOAD_BTN && code == BN_CLICKED) {
            ReloadFromConfig();
            return 0;
        }
        break;
    }
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}