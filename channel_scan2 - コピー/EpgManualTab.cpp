#include "EpgManualTab.hpp"

#include <CommCtrl.h>
#pragma comment(lib, "Comctl32.lib")

#include "ConfigManager.h"
#include "TaskKinds.hpp"
#include "TaskListManager.hpp"
#include "SystemInfoManager.hpp"

#include <algorithm>
#include <filesystem>
#include <Shellapi.h>
#include <cwchar>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "Shell32.lib")

static constexpr int IDC_BT_FULLSCAN = 3001;
static constexpr int IDC_BT_REALTIME = 3002;
static constexpr int IDC_BT_BUILD = 3003;
static constexpr int IDC_BT_STOP = 3004;
static constexpr int IDC_ED_HOUR = 3050;
static constexpr int IDC_UD_HOUR = 3051;
static constexpr int IDC_ED_MIN = 3052;
static constexpr int IDC_UD_MIN = 3053;
static constexpr int IDC_BT_SET_DAILYFULL = 3055;
static constexpr int IDC_BT_REFRESH_TASKS = 3056;
static constexpr int IDC_LV_TASKS = 3057;
static constexpr int IDC_ST_HTML_RENDER = 3058;
static constexpr int IDC_CB_HTML_RENDER = 3059;

namespace
{
    std::filesystem::path GetExeDirectoryPath_()
    {
        wchar_t exePath[MAX_PATH]{};
        const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            return std::filesystem::current_path();
        }
        return std::filesystem::path(exePath).parent_path();
    }

    std::wstring GetDefaultTasksIniPath_()
    {
        return (GetExeDirectoryPath_() / L"tasks.ini").wstring();
    }

    std::wstring GetDefaultEpgIndexHtmlPath_()
    {
        return (GetExeDirectoryPath_() / L"epg_out" / L"index.html").wstring();
    }

    bool OpenPathWithShell_(const std::filesystem::path& targetPath)
    {
        const std::wstring filePath = targetPath.wstring();
        const std::wstring workDir = targetPath.parent_path().wstring();

        const HINSTANCE h = ShellExecuteW(
            nullptr,
            L"open",
            filePath.c_str(),
            nullptr,
            workDir.empty() ? nullptr : workDir.c_str(),
            SW_SHOWNORMAL);

        return reinterpret_cast<INT_PTR>(h) > 32;
    }

    std::wstring ReadIniStringLike_(
        const std::wstring& iniPath,
        const wchar_t* section,
        const wchar_t* key,
        const wchar_t* defValue = L"")
    {
        wchar_t buf[512]{};

        ::GetPrivateProfileStringW(
            section,
            key,
            defValue ? defValue : L"",
            buf,
            static_cast<DWORD>(std::size(buf)),
            iniPath.c_str());

        return std::wstring(buf);
    }

    int ReadIniIntLike_(
        const std::wstring& iniPath,
        const wchar_t* section,
        const wchar_t* key,
        int defValue)
    {
        return ::GetPrivateProfileIntW(
            section,
            key,
            defValue,
            iniPath.c_str());
    }

    bool ReadIniBoolLike_(
        const std::wstring& iniPath,
        const wchar_t* section,
        const wchar_t* key,
        bool defValue)
    {
        const int defInt = defValue ? 1 : 0;
        const int v = ReadIniIntLike_(iniPath, section, key, defInt);
        return v != 0;
    }

    std::wstring NormalizeBrowserHost_(std::wstring host)
    {
        if (host.empty()) {
            return L"127.0.0.1";
        }

        if (host == L"0.0.0.0" || host == L"::" || host == L"*") {
            return L"127.0.0.1";
        }

        if (host == L"localhost") {
            return L"127.0.0.1";
        }

        return host;
    }

    std::wstring ResolveEpgPortalHttpIndexUrl_()
    {
        const std::wstring iniPath = ConfigManager::GetInstance().GetIniPath();
        if (iniPath.empty()) {
            return {};
        }

        const bool enableServer =
            ReadIniBoolLike_(iniPath, L"Network", L"EnableServer", false);
        const bool autoStartServer =
            ReadIniBoolLike_(iniPath, L"Network", L"AutoStartServer", false);

        if (!enableServer || !autoStartServer) {
            return {};
        }

        std::wstring host =
            ReadIniStringLike_(iniPath, L"Network", L"BindAddress", L"127.0.0.1");
        host = NormalizeBrowserHost_(host);

        int port = ReadIniIntLike_(iniPath, L"Network", L"HttpPort", 8080);
        if (port <= 0 || port > 65535) {
            port = 8080;
        }

        // 現在の EpgPortalReservationApiServer は HTTP サーバーとして運用する。
        // EnableHttps/TlsMode は将来対応用としてここでは使わない。
        std::wstring url = L"http://";
        url += host;
        url += L":";
        url += std::to_wstring(port);
        url += L"/index.html";

        return url;
    }

    bool OpenUrlWithShell_(const std::wstring& url)
    {
        if (url.empty()) {
            return false;
        }

        const HINSTANCE h = ::ShellExecuteW(
            nullptr,
            L"open",
            url.c_str(),
            nullptr,
            nullptr,
            SW_SHOWNORMAL);

        return reinterpret_cast<INT_PTR>(h) > 32;
    }

    std::wstring FormatYmdHHMMTextForEpgGuard_(int ymd, int hhmm)
    {
        if (ymd <= 0 || hhmm < 0) {
            return L"-";
        }

        wchar_t buf[64]{};
        swprintf_s(
            buf,
            L"%08d %02d:%02d",
            ymd,
            hhmm / 100,
            hhmm % 100);

        return std::wstring(buf);
    }

    bool CheckManualEpgStartOrLog_(
        const GuiActions& act,
        const std::function<void(std::wstring_view)>& log,
        int estimatedMinutes,
        int guardMinutes,
        const wchar_t* label)
    {
        if (!act.CheckManualEpgStart) {
            // 未接続時は旧動作維持。
            // ただしログには残す。
            if (log) {
                std::wstring msg = L"[EPG] 手動EPG開始チェックが未接続です。旧動作で開始します: ";
                msg += label ? label : L"(unknown)";
                log(msg);
            }
            return true;
        }

        const auto check =
            act.CheckManualEpgStart(
                estimatedMinutes,
                guardMinutes);

        if (check.canStart) {
            return true;
        }

        std::wstring msg;
        msg.reserve(512);

        msg += L"[EPG] ";
        msg += label ? label : L"手動EPG取得";
        msg += L"を開始しません。";
        msg += check.reason.empty()
            ? L"番組予約と衝突する可能性があります。"
            : check.reason;

        if (!check.conflictName.empty()) {
            msg += L" conflict=";
            msg += check.conflictName;
        }

        if (check.conflictYmd > 0 && check.conflictHHMM >= 0) {
            msg += L" due=";
            msg += FormatYmdHHMMTextForEpgGuard_(
                check.conflictYmd,
                check.conflictHHMM);
        }

        if (log) {
            log(msg);
        }

        SystemInfoLogWarn(
            L"%ls",
            msg.c_str());

        return false;
    }


    int CalcTomorrowYmd()
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);

        FILETIME ft{};
        if (!SystemTimeToFileTime(&st, &ft)) {
            return static_cast<int>(st.wYear) * 10000 + static_cast<int>(st.wMonth) * 100 + static_cast<int>(st.wDay);
        }

        ULARGE_INTEGER ui{};
        ui.LowPart = ft.dwLowDateTime;
        ui.HighPart = ft.dwHighDateTime;

        constexpr ULONGLONG OneDay100ns = 24ULL * 60ULL * 60ULL * 10ULL * 1000ULL * 1000ULL;
        ui.QuadPart += OneDay100ns;

        FILETIME ft2{};
        ft2.dwLowDateTime = ui.LowPart;
        ft2.dwHighDateTime = ui.HighPart;

        SYSTEMTIME st2{};
        if (!FileTimeToSystemTime(&ft2, &st2)) {
            return static_cast<int>(st.wYear) * 10000 + static_cast<int>(st.wMonth) * 100 + static_cast<int>(st.wDay);
        }
        return static_cast<int>(st2.wYear) * 10000 + static_cast<int>(st2.wMonth) * 100 + static_cast<int>(st2.wDay);
    }

    bool AddLocalMinutesYmdHhmm_(int baseYmd, int baseHHMM, int deltaMinutes, int& outYmd, int& outHHMM)
    {
        SYSTEMTIME local{};
        local.wYear = static_cast<WORD>(baseYmd / 10000);
        local.wMonth = static_cast<WORD>((baseYmd / 100) % 100);
        local.wDay = static_cast<WORD>(baseYmd % 100);
        local.wHour = static_cast<WORD>(baseHHMM / 100);
        local.wMinute = static_cast<WORD>(baseHHMM % 100);
        local.wSecond = 0;
        local.wMilliseconds = 0;

        SYSTEMTIME utc{};
        if (!::TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) {
            return false;
        }

        FILETIME ft{};
        if (!::SystemTimeToFileTime(&utc, &ft)) {
            return false;
        }

        ULARGE_INTEGER ui{};
        ui.LowPart = ft.dwLowDateTime;
        ui.HighPart = ft.dwHighDateTime;

        constexpr LONGLONG kMinute100ns = 60LL * 1000LL * 1000LL * 10LL;
        LONGLONG v = static_cast<LONGLONG>(ui.QuadPart);
        v += static_cast<LONGLONG>(deltaMinutes) * kMinute100ns;
        if (v < 0) {
            v = 0;
        }

        ui.QuadPart = static_cast<ULONGLONG>(v);
        ft.dwLowDateTime = ui.LowPart;
        ft.dwHighDateTime = ui.HighPart;

        if (!::FileTimeToSystemTime(&ft, &utc)) {
            return false;
        }

        SYSTEMTIME shiftedLocal{};
        if (!::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &shiftedLocal)) {
            return false;
        }

        outYmd = static_cast<int>(shiftedLocal.wYear) * 10000
            + static_cast<int>(shiftedLocal.wMonth) * 100
            + static_cast<int>(shiftedLocal.wDay);

        outHHMM = static_cast<int>(shiftedLocal.wHour) * 100
            + static_cast<int>(shiftedLocal.wMinute);
        return true;
    }

    void SetFont_(HWND h, HFONT f)
    {
        if (!h || !f) return;
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    }

    HMENU CtrlId_(int id) noexcept
    {
        return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
    }

    std::wstring FormatYmdText_(int ymd)
    {
        if (ymd <= 0) {
            return L"-";
        }

        const int year = ymd / 10000;
        const int month = (ymd / 100) % 100;
        const int day = ymd % 100;

        wchar_t buf[32]{};
        swprintf_s(buf, L"%04d-%02d-%02d", year, month, day);
        return std::wstring(buf);
    }

    std::wstring FormatHhmmText_(int hhmm)
    {
        if (hhmm <= 0) {
            return L"-";
        }

        const int hour = hhmm / 100;
        const int minute = hhmm % 100;

        wchar_t buf[16]{};
        swprintf_s(buf, L"%02d:%02d", hour, minute);
        return std::wstring(buf);
    }

    std::wstring FormatStartDateText_(const TaskListManager::TaskEntry& t)
    {
        switch (t.schedule.type) {
        case TaskListManager::ScheduleType::Daily:
            return L"毎日";

        case TaskListManager::ScheduleType::Hourly:
            return L"毎時";

        case TaskListManager::ScheduleType::Interval:
            return L"間隔";

        case TaskListManager::ScheduleType::OneShot:
            return FormatYmdText_(t.schedule.dueYmd);

        default:
            return L"-";
        }
    }

    std::wstring FormatStartTimeText_(const TaskListManager::TaskEntry& t)
    {
        wchar_t buf[32]{};

        switch (t.schedule.type) {
        case TaskListManager::ScheduleType::Daily:
            swprintf_s(buf, L"%02d:%02d", t.schedule.dailyHour, t.schedule.dailyMinute);
            return std::wstring(buf);

        case TaskListManager::ScheduleType::Hourly:
            swprintf_s(buf, L"%02d分", t.schedule.hourlyAtMinute);
            return std::wstring(buf);

        case TaskListManager::ScheduleType::Interval:
            swprintf_s(buf, L"%d分ごと", t.schedule.intervalMinutes);
            return std::wstring(buf);

        case TaskListManager::ScheduleType::OneShot:
            return FormatHhmmText_(t.schedule.dueHHMM);

        default:
            return L"-";
        }
    }

    std::wstring FormatEndDateText_(const TaskListManager::TaskEntry& t)
    {
        if (t.display.endYmd > 0) {
            return FormatYmdText_(t.display.endYmd);
        }

        if (t.schedule.useTimeWindow && t.schedule.windowEndYmd > 0) {
            return FormatYmdText_(t.schedule.windowEndYmd);
        }

        return L"-";
    }

    std::wstring FormatEndTimeText_(const TaskListManager::TaskEntry& t)
    {
        if (t.display.endHHMM > 0) {
            return FormatHhmmText_(t.display.endHHMM);
        }

        if (t.schedule.useTimeWindow && t.schedule.windowEndHHMM > 0) {
            return FormatHhmmText_(t.schedule.windowEndHHMM);
        }

        return L"-";
    }

    int InsertTaskColumn_(HWND lv, int index, const wchar_t* text, int width)
    {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
        col.fmt = LVCFMT_LEFT;
        col.cx = width;
        col.pszText = const_cast<LPWSTR>(text);
        col.iSubItem = index;
        return ListView_InsertColumn(lv, index, &col);
    }

    void SetTaskCell_(HWND lv, int itemIndex, int subItem, const std::wstring& text)
    {
        ListView_SetItemText(lv, itemIndex, subItem, const_cast<LPWSTR>(text.c_str()));
    }

    const wchar_t* ToRouteDisplayName_(ConfigManager::EpgHtmlRenderRoute route) noexcept
    {
        switch (route) {
        case ConfigManager::EpgHtmlRenderRoute::ProgramPageWriter:
            return L"ProgramPageWriter（予約スイッチ）";
        case ConfigManager::EpgHtmlRenderRoute::Plugin:
            return L"プラグイン";
        case ConfigManager::EpgHtmlRenderRoute::Auto:
            return L"自動";
        case ConfigManager::EpgHtmlRenderRoute::Lite:
            return L"軽量版（EpgHtmlLite）";
        case ConfigManager::EpgHtmlRenderRoute::Legacy:
        default:
            return L"旧番組表";
        }
    }

    bool IsProgramReservationTaskKind_(const TaskListManager::TaskEntry& t) noexcept
    {
        return
            t.request.kind == TaskKinds::RecPrepare ||
            t.request.kind == TaskKinds::RecStart ||
            t.request.kind == TaskKinds::RecStop;
    }

    bool IsTaskListDisplayRepresentative_(const TaskListManager::TaskEntry& t) noexcept
    {
        // 番組予約は内部的に RecPrepare / RecStart / RecStop の3タスクで持つ。
        // UI表示では RecStart だけを代表行として見せる。
        if (IsProgramReservationTaskKind_(t)) {
            return t.request.kind == TaskKinds::RecStart;
        }

        // EPG定時取得などはそのまま表示。
        return true;
    }
}

bool EpgManualTab::Create(HWND hParent, const RECT& rc, HINSTANCE inst, HFONT font,
    std::function<GuiActions()>&& a,
    std::function<void(std::wstring_view)>&& l)
{
    hInst_ = inst;
    hFont_ = font;
    getActions_ = std::move(a);
    log_ = std::move(l);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &EpgManualTab::WndProc;
    wc.hInstance = hInst_;
    wc.lpszClassName = L"TSCEPG_EpgManualTab";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (!RegisterClassExW(&wc)) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) return false;
    }

    hWnd_ = CreateWindowExW(
        0, wc.lpszClassName, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
        hParent, nullptr, hInst_, this);

    return (hWnd_ != nullptr);
}

void EpgManualTab::Show(bool show)
{
    if (!hWnd_) return;
    ShowWindow(hWnd_, show ? SW_SHOW : SW_HIDE);

    if (show) {
        RefreshTaskListView_();
        ApplyBusyStateToControls_();
    }
}

void EpgManualTab::Move(const RECT& rc)
{
    if (!hWnd_) return;
    MoveWindow(hWnd_, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
}

void EpgManualTab::NotifyActivated()
{
    RefreshTaskListView_();
    ApplyBusyStateToControls_();
}

void EpgManualTab::NotifyReservationDataChanged()
{
    RefreshTaskListView_();
}

void EpgManualTab::ReloadFromConfig()
{
    if (!hWnd_) return;

    const int hhmm = ConfigManager::GetInstance().GetEpgDailyFullHHMM();
    SetHourMinute_(hhmm);
    ReloadHtmlRenderRouteSelection_();
    RefreshTaskListView_();
    ApplyBusyStateToControls_();
}

void EpgManualTab::SetBusyState(bool busy)
{
    if (busy_ == busy) {
        return;
    }

    busy_ = busy;
    ApplyBusyStateToControls_();

    if (log_) {
        log_(busy_
            ? L"[EPG] 実行中UIへ切り替えました。開始・設定変更はロックし、閲覧系は許可します。"
            : L"[EPG] 通常UIへ戻しました。");
    }
}

void EpgManualTab::ApplyBusyStateToControls_()
{
    if (!hWnd_) return;

    const BOOL enableOperation = busy_ ? FALSE : TRUE;
    const BOOL enableViewOnly = TRUE;
    const BOOL enableStop = TRUE;

    // EPG実行を増やす/設定を変える操作はロック
    if (btFullScan_)        EnableWindow(btFullScan_, enableOperation);
    if (btRealtime_)        EnableWindow(btRealtime_, enableOperation);
    if (btSetDailyFull_)    EnableWindow(btSetDailyFull_, enableOperation);

    if (edHour_)            EnableWindow(edHour_, enableOperation);
    if (udHour_)            EnableWindow(udHour_, enableOperation);
    if (edMin_)             EnableWindow(edMin_, enableOperation);
    if (udMin_)             EnableWindow(udMin_, enableOperation);

    if (cbHtmlRenderRoute_) EnableWindow(cbHtmlRenderRoute_, enableOperation);

    // 閲覧・確認系は busy 中でも許可
    if (btBuild_)           EnableWindow(btBuild_, enableViewOnly);
    if (btRefreshTasks_)    EnableWindow(btRefreshTasks_, enableViewOnly);
    if (lvTasks_)           EnableWindow(lvTasks_, enableViewOnly);

    // 停止は常に許可
    if (btStop_)            EnableWindow(btStop_, enableStop);

    if (busy_ && btStop_) {
        SetFocus(btStop_);
    }
}
LRESULT CALLBACK EpgManualTab::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    EpgManualTab* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<EpgManualTab*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hWnd_ = h;
    }
    else {
        self = reinterpret_cast<EpgManualTab*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }

    return self ? self->HandleMessage(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT EpgManualTab::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        OnCreate();
        return 0;

    case WM_SIZE:
        LayoutControls_(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_COMMAND:
    {
        const int id = LOWORD(wp);
        auto act = getActions_ ? getActions_() : GuiActions{};

        if (id == IDC_BT_FULLSCAN) {
            if (busy_) {
                if (log_) log_(L"[EPG] 実行中のためフルスキャン開始は無効です。");
                return 0;
            }

            if (!CheckManualEpgStartOrLog_(
                act,
                log_,
                90,  // 初回/フル取得の想定: 90分
                30,  // 予約前の余裕: 30分
                L"EPGフルスキャン")) {
                return 0;
            }

            if (act.StartEpgFullScan) {
                act.StartEpgFullScan();
                if (log_) log_(L"[EPG] フルスキャンを開始します。");
            }
            else if (log_) {
                log_(L"[EPG] StartEpgFullScan が未接続です。");
            }
        }
        else if (id == IDC_BT_REALTIME) {
            if (busy_) {
                if (log_) log_(L"[EPG] 実行中のためリアルタイム更新開始は無効です。");
                return 0;
            }

            if (!CheckManualEpgStartOrLog_(
                act,
                log_,
                90,  // 第1工程では安全側で固定
                30,
                L"リアルタイムEPG更新")) {
                return 0;
            }

            if (act.StartRealtimeEpgUpdate) {
                act.StartRealtimeEpgUpdate();
                if (log_) log_(L"[EPG] リアルタイム更新を開始します。");
            }
            else if (log_) {
                log_(L"[EPG] StartRealtimeEpgUpdate が未接続です。");
            }
        }
        else if (id == IDC_CB_HTML_RENDER && HIWORD(wp) == CBN_SELCHANGE) {
            if (busy_) {
                ReloadHtmlRenderRouteSelection_();
                if (log_) log_(L"[EPG] 実行中のため番組表デザイン変更は無効です。");
                return 0;
            }

            SaveHtmlRenderRouteSelection_();
        }
        else if (id == IDC_BT_BUILD) {
            if (busy_ && log_) {
                log_(L"[EPG] 実行中ですが、閲覧用としてEPG表示を開きます。");
            }

            const std::filesystem::path indexPath = GetDefaultEpgIndexHtmlPath_();

            if (!std::filesystem::exists(indexPath)) {
                if (log_) {
                    std::wstring msg = L"[EPG] index.html が見つかりません: ";
                    msg += indexPath.wstring();
                    log_(msg);
                }
                return 0;
            }

            const std::wstring portalUrl = ResolveEpgPortalHttpIndexUrl_();

            if (!portalUrl.empty()) {
                if (OpenUrlWithShell_(portalUrl)) {
                    if (log_) {
                        std::wstring msg = L"[EPG] EPG表示をサーバー経由で開きました: ";
                        msg += portalUrl;
                        log_(msg);
                    }
                }
                else {
                    if (log_) {
                        std::wstring msg = L"[EPG] サーバー経由のEPG表示を開けませんでした。ローカルファイルへフォールバックします: ";
                        msg += portalUrl;
                        log_(msg);
                    }

                    if (OpenPathWithShell_(indexPath)) {
                        if (log_) {
                            std::wstring msg = L"[EPG] EPG表示をローカルファイルで開きました: ";
                            msg += indexPath.wstring();
                            log_(msg);
                        }
                    }
                    else if (log_) {
                        std::wstring msg = L"[EPG] EPG表示を開けませんでした: ";
                        msg += indexPath.wstring();
                        log_(msg);
                    }
                }
            }
            else {
                if (OpenPathWithShell_(indexPath)) {
                    if (log_) {
                        std::wstring msg = L"[EPG] EPG表示をローカルファイルで開きました: ";
                        msg += indexPath.wstring();
                        log_(msg);
                    }
                }
                else {
                    if (log_) {
                        std::wstring msg = L"[EPG] EPG表示を開けませんでした: ";
                        msg += indexPath.wstring();
                        log_(msg);
                    }
                }
            }
        }
        else if (id == IDC_BT_STOP) {
            if (act.StopTask) {
                act.StopTask();
                if (log_) log_(L"[EPG] 停止要求を送信しました。");
            }
            else if (log_) {
                log_(L"[EPG] StopTask が未接続です。");
            }
        }
        else if (id == IDC_BT_SET_DAILYFULL) {
            if (busy_) {
                if (log_) log_(L"[EPG] 実行中のため時間設定変更は無効です。");
                return 0;
            }
            OnApplyDailyFullTime();
        }
        else if (id == IDC_BT_REFRESH_TASKS) {
            RefreshTaskListView_();

            if (log_) {
                log_(busy_
                    ? L"[EPG] 実行中ですが、表示専用としてタスクリストを再読み込みしました。"
                    : L"[EPG] タスクリストを再読み込みしました。");
            }
        }
        return 0;
    }

    default:
        return DefWindowProcW(hWnd_, msg, wp, lp);
    }
}

void EpgManualTab::OnCreate()
{
    stTitle_ = CreateWindowExW(0, L"STATIC", L"EPG 手動実行：", WS_CHILD | WS_VISIBLE,
        10, 12, 240, 20, hWnd_, nullptr, hInst_, nullptr);

    btFullScan_ = CreateWindowExW(0, L"BUTTON", L"EPGフルスキャン",
        WS_CHILD | WS_VISIBLE,
        10, 45, 160, 30, hWnd_, CtrlId_(IDC_BT_FULLSCAN), hInst_, nullptr);

    btRealtime_ = CreateWindowExW(0, L"BUTTON", L"リアルタイム更新",
        WS_CHILD | WS_VISIBLE,
        180, 45, 160, 30, hWnd_, CtrlId_(IDC_BT_REALTIME), hInst_, nullptr);

    btBuild_ = CreateWindowExW(0, L"BUTTON", L"EPG表示",
        WS_CHILD | WS_VISIBLE,
        10, 85, 160, 30, hWnd_, CtrlId_(IDC_BT_BUILD), hInst_, nullptr);

    btStop_ = CreateWindowExW(0, L"BUTTON", L"停止",
        WS_CHILD | WS_VISIBLE,
        180, 85, 160, 30, hWnd_, CtrlId_(IDC_BT_STOP), hInst_, nullptr);

    stDailyFull_ = CreateWindowExW(0, L"STATIC", L"フルスキャン定時：", WS_CHILD | WS_VISIBLE,
        10, 135, 140, 20, hWnd_, nullptr, hInst_, nullptr);

    edHour_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"03",
        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
        150, 132, 40, 24, hWnd_, CtrlId_(IDC_ED_HOUR), hInst_, nullptr);

    udHour_ = CreateWindowExW(0, UPDOWN_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_SETBUDDYINT | UDS_ARROWKEYS,
        0, 0, 0, 0, hWnd_, CtrlId_(IDC_UD_HOUR), hInst_, nullptr);

    stColon_ = CreateWindowExW(0, L"STATIC", L":", WS_CHILD | WS_VISIBLE,
        195, 135, 10, 20, hWnd_, nullptr, hInst_, nullptr);

    edMin_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"30",
        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
        210, 132, 40, 24, hWnd_, CtrlId_(IDC_ED_MIN), hInst_, nullptr);

    udMin_ = CreateWindowExW(0, UPDOWN_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_SETBUDDYINT | UDS_ARROWKEYS,
        0, 0, 0, 0, hWnd_, CtrlId_(IDC_UD_MIN), hInst_, nullptr);

    btSetDailyFull_ = CreateWindowExW(0, L"BUTTON", L"時間設定",
        WS_CHILD | WS_VISIBLE,
        270, 130, 90, 26, hWnd_, CtrlId_(IDC_BT_SET_DAILYFULL), hInst_, nullptr);

    stHtmlRenderRoute_ = CreateWindowExW(0, L"STATIC", L"番組表デザイン：", WS_CHILD | WS_VISIBLE,
        10, 172, 140, 20, hWnd_, CtrlId_(IDC_ST_HTML_RENDER), hInst_, nullptr);

    cbHtmlRenderRoute_ = CreateWindowExW(0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
        150, 168, 210, 220, hWnd_, CtrlId_(IDC_CB_HTML_RENDER), hInst_, nullptr);

    // アイテムデータに ConfigManager::EpgHtmlRenderRoute の値をセットしていく
    {
        const int i0 = static_cast<int>(SendMessageW(
            cbHtmlRenderRoute_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"旧番組表")));
        SendMessageW(
            cbHtmlRenderRoute_, CB_SETITEMDATA, i0,
            static_cast<LPARAM>(ConfigManager::EpgHtmlRenderRoute::Legacy));

        const int i1 = static_cast<int>(SendMessageW(
            cbHtmlRenderRoute_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"ProgramPageWriter(予約スイッチ)")));
        SendMessageW(
            cbHtmlRenderRoute_, CB_SETITEMDATA, i1,
            static_cast<LPARAM>(ConfigManager::EpgHtmlRenderRoute::ProgramPageWriter));

        const int i2 = static_cast<int>(SendMessageW(
            cbHtmlRenderRoute_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"軽量版（EpgHtmlLite）")));
        SendMessageW(
            cbHtmlRenderRoute_, CB_SETITEMDATA, i2,
            static_cast<LPARAM>(ConfigManager::EpgHtmlRenderRoute::Lite));

        const int i3 = static_cast<int>(SendMessageW(
            cbHtmlRenderRoute_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"プラグイン")));
        SendMessageW(
            cbHtmlRenderRoute_, CB_SETITEMDATA, i3,
            static_cast<LPARAM>(ConfigManager::EpgHtmlRenderRoute::Plugin));

        const int i4 = static_cast<int>(SendMessageW(
            cbHtmlRenderRoute_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"自動")));
        SendMessageW(
            cbHtmlRenderRoute_, CB_SETITEMDATA, i4,
            static_cast<LPARAM>(ConfigManager::EpgHtmlRenderRoute::Auto));
    }

    stTaskList_ = CreateWindowExW(0, L"STATIC", L"EPG予約タスクリスト：", WS_CHILD | WS_VISIBLE,
        10, 212, 180, 20, hWnd_, nullptr, hInst_, nullptr);

    btRefreshTasks_ = CreateWindowExW(0, L"BUTTON", L"一覧更新",
        WS_CHILD | WS_VISIBLE,
        200, 207, 90, 26, hWnd_, CtrlId_(IDC_BT_REFRESH_TASKS), hInst_, nullptr);

    lvTasks_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        10, 237, 760, 260, hWnd_, CtrlId_(IDC_LV_TASKS), hInst_, nullptr);

    for (HWND c : {
        stTitle_, btFullScan_, btRealtime_, btBuild_, btStop_,
            stDailyFull_, edHour_, udHour_, stColon_, edMin_, udMin_, btSetDailyFull_,
            stHtmlRenderRoute_, cbHtmlRenderRoute_,
            stTaskList_, btRefreshTasks_, lvTasks_
    }) {
        SetFont_(c, hFont_);
    }

    SendMessageW(udHour_, UDM_SETBUDDY, reinterpret_cast<WPARAM>(edHour_), 0);
    SendMessageW(udHour_, UDM_SETRANGE32, 0, 23);

    SendMessageW(udMin_, UDM_SETBUDDY, reinterpret_cast<WPARAM>(edMin_), 0);
    SendMessageW(udMin_, UDM_SETRANGE32, 0, 59);

    ListView_SetExtendedListViewStyle(lvTasks_,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);

    RebuildTaskListColumns_();

    const auto& cfg = ConfigManager::GetInstance();
    SetHourMinute_(cfg.GetEpgDailyFullHHMM());
    ReloadHtmlRenderRouteSelection_();
    RefreshTaskListView_();
    ApplyBusyStateToControls_();
}

void EpgManualTab::RebuildTaskListColumns_()
{
    if (!lvTasks_) return;

    while (ListView_DeleteColumn(lvTasks_, 0)) {}

    InsertTaskColumn_(lvTasks_, 0, L"実行名ラベル", 180);
    InsertTaskColumn_(lvTasks_, 1, L"開始日", 110);
    InsertTaskColumn_(lvTasks_, 2, L"開始時刻", 90);
    InsertTaskColumn_(lvTasks_, 3, L"終了日", 110);
    InsertTaskColumn_(lvTasks_, 4, L"終了時刻", 90);
    InsertTaskColumn_(lvTasks_, 5, L"対象", 160);
    InsertTaskColumn_(lvTasks_, 6, L"種別", 90);
    InsertTaskColumn_(lvTasks_, 7, L"状態", 70);
}

void EpgManualTab::LayoutControls_(int clientWidth, int clientHeight)
{
    if (!hWnd_) return;

    const int right = (clientWidth > 10) ? clientWidth : 10;
    const int bottom = (clientHeight > 10) ? clientHeight : 10;

    MoveWindow(btRefreshTasks_, right - 110, 207, 100, 26, TRUE);

    const int listX = 10;
    const int listY = 237;
    const int listW = (right - 20 > 100) ? (right - 20) : 100;
    const int listH = (bottom - listY - 10 > 120) ? (bottom - listY - 10) : 120;
    MoveWindow(lvTasks_, listX, listY, listW, listH, TRUE);

    const int widths[] = {
        listW * 22 / 100,
        listW * 12 / 100,
        listW * 10 / 100,
        listW * 12 / 100,
        listW * 10 / 100,
        listW * 16 / 100,
        listW * 10 / 100,
        listW * 8 / 100
    };

    for (int i = 0; i < 8; ++i) {
        ListView_SetColumnWidth(lvTasks_, i, widths[i]);
    }
}

void EpgManualTab::RefreshTaskListView_()
{
    if (!lvTasks_) return;

    ListView_DeleteAllItems(lvTasks_);

    TaskListManager tl;
    const std::wstring tasksIni = GetDefaultTasksIniPath_();
    if (!tl.LoadFromIni(tasksIni)) {
        tl.EnsureDefaultEpgTasks(1);
    }

    const auto snapshot = tl.GetSnapshot();

    // 通常は RecStart を番組予約の代表行にする。
    // 旧版で RecStart だけが開始時刻に削除済みの予約については、
    // RecStop を代替表示して終了時刻まで一覧から消えないようにする。
    std::unordered_set<std::uint32_t> programStartInstances;
    programStartInstances.reserve(snapshot.size());

    for (const auto& t : snapshot) {
        if (t.request.kind == TaskKinds::RecStart) {
            programStartInstances.insert(t.request.instance);
        }
    }

    for (const auto& t : snapshot) {
        bool displayRepresentative = IsTaskListDisplayRepresentative_(t);
        bool usingStopFallback = false;

        if (!displayRepresentative &&
            t.request.kind == TaskKinds::RecStop &&
            programStartInstances.find(t.request.instance) ==
            programStartInstances.end()) {
            displayRepresentative = true;
            usingStopFallback = true;
        }

        if (!displayRepresentative) {
            continue;
        }

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = ListView_GetItemCount(lvTasks_);

        const std::wstring label = !t.display.label.empty()
            ? t.display.label
            : t.name;

        item.pszText = const_cast<LPWSTR>(label.c_str());

        const int row = ListView_InsertItem(lvTasks_, &item);
        if (row < 0) {
            continue;
        }

        std::wstring startDateText = FormatStartDateText_(t);
        std::wstring startTimeText = FormatStartTimeText_(t);
        std::wstring endDateText = FormatEndDateText_(t);
        std::wstring endTimeText = FormatEndTimeText_(t);

        if (usingStopFallback) {
            if (!t.reserve.startDate.empty()) {
                startDateText = t.reserve.startDate;
            }
            if (!t.reserve.startTime.empty()) {
                startTimeText = t.reserve.startTime.substr(
                    0,
                    (std::min<std::size_t>)(5, t.reserve.startTime.size()));
            }
            if (!t.reserve.endDate.empty()) {
                endDateText = t.reserve.endDate;
            }
            if (!t.reserve.endTime.empty()) {
                endTimeText = t.reserve.endTime.substr(
                    0,
                    (std::min<std::size_t>)(5, t.reserve.endTime.size()));
            }
        }

        SetTaskCell_(lvTasks_, row, 1, startDateText);
        SetTaskCell_(lvTasks_, row, 2, startTimeText);
        SetTaskCell_(lvTasks_, row, 3, endDateText);
        SetTaskCell_(lvTasks_, row, 4, endTimeText);
        SetTaskCell_(lvTasks_, row, 5, t.display.target.empty() ? L"-" : t.display.target);
        SetTaskCell_(lvTasks_, row, 6, t.display.category.empty() ? L"-" : t.display.category);
        SetTaskCell_(lvTasks_, row, 7, t.enabled ? L"有効" : L"無効");
    }

    RECT rc{};
    GetClientRect(hWnd_, &rc);
    LayoutControls_(rc.right - rc.left, rc.bottom - rc.top);
}

int EpgManualTab::GetHour_() const
{
    if (!edHour_) return 3;
    wchar_t b[16]{};
    GetWindowTextW(edHour_, b, _countof(b));
    const int v = _wtoi(b);
    return (v < 0) ? 0 : (v > 23 ? 23 : v);
}

int EpgManualTab::GetMinute_() const
{
    if (!edMin_) return 30;
    wchar_t b[16]{};
    GetWindowTextW(edMin_, b, _countof(b));
    const int v = _wtoi(b);
    return (v < 0) ? 0 : (v > 59 ? 59 : v);
}

void EpgManualTab::SetHourMinute_(int hhmm)
{
    int h = hhmm / 100;
    int m = hhmm % 100;
    if (h < 0 || h > 23) h = 3;
    if (m < 0 || m > 59) m = 30;

    wchar_t hb[8]{}, mb[8]{};
    swprintf_s(hb, L"%02d", h);
    swprintf_s(mb, L"%02d", m);
    SetWindowTextW(edHour_, hb);
    SetWindowTextW(edMin_, mb);

    SendMessageW(udHour_, UDM_SETPOS32, 0, h);
    SendMessageW(udMin_, UDM_SETPOS32, 0, m);
}

void EpgManualTab::OnApplyDailyFullTime()
{
    int oneShotDueYmd = 0;
    int oneShotEndYmd = 0;
    int oneShotEndHHMM = 0;

    const int h = GetHour_();
    const int m = GetMinute_();
    const int hhmm = h * 100 + m;

    {
        auto& cfg = ConfigManager::GetInstance();
        cfg.SetEpgDailyFullHHMM(hhmm);
        cfg.SaveConfig();
    }

    const std::wstring tasksIni = GetDefaultTasksIniPath_();
    TaskListManager tl;

    const bool loaded = tl.LoadFromIni(tasksIni);
    if (!loaded) {
        if (log_) log_(L"[TaskList] tasks.ini load failed. create defaults then upsert.");
        tl.EnsureDefaultEpgTasks(1);
    }
    else {
        tl.EnsureDefaultEpgTasks(1);
    }

    // 毎日定時の EPG 取得予約
    {
        auto t = tl.Find(TaskKinds::EpgFull, 0).value_or(TaskListManager::TaskEntry{});
        t.name = L"EPG Full Update (Daily)";
        t.display.label = L"EPG取得予約";
        t.display.category = L"EPG";
        t.display.target = L"EPG対象サービス";
        t.display.note = L"毎日時刻テンプレート。実行は次回OneShotのみ";
        t.display.endYmd = 0;
        t.display.endHHMM = 0;

        // Daily は実行対象ではなく、次回OneShotを作るための時刻テンプレート。
        // 実際のEPG取得は instance=1 のOneShotのみで行う。
        t.enabled = false;

        t.request.kind = TaskKinds::EpgFull;
        t.request.instance = 0;
        t.request.priority = TimeKeeper::TaskPriority::Soft;
        t.request.requiredTuners = 1;

        t.schedule.type = TaskListManager::ScheduleType::Daily;
        t.schedule.dailyHour = h;
        t.schedule.dailyMinute = m;
        t.schedule.hourlyAtMinute = 0;
        t.schedule.intervalMinutes = 0;
        t.schedule.dueYmd = 0;
        t.schedule.dueHHMM = 0;
        t.schedule.lateToleranceMinutes = -1;

        // Daily は従来どおり
        t.schedule.useTimeWindow = false;
        t.schedule.windowStartYmd = 0;
        t.schedule.windowStartHHMM = 0;
        t.schedule.windowEndYmd = 0;
        t.schedule.windowEndHHMM = 0;

        tl.UpsertTask(t);
    }

    // 次回起動用の単発予約: 開始から30分の範囲
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);

        const int nowHHMM = static_cast<int>(st.wHour) * 100 + static_cast<int>(st.wMinute);
        const int todayYmd = static_cast<int>(st.wYear) * 10000
            + static_cast<int>(st.wMonth) * 100
            + static_cast<int>(st.wDay);

        const int dueYmd = (nowHHMM <= hhmm) ? todayYmd : CalcTomorrowYmd();
        oneShotDueYmd = dueYmd;

        if (!AddLocalMinutesYmdHhmm_(dueYmd, hhmm, 30, oneShotEndYmd, oneShotEndHHMM)) {
            oneShotEndYmd = dueYmd;
            oneShotEndHHMM = hhmm;
        }

        auto t = tl.Find(TaskKinds::EpgFull, 1).value_or(TaskListManager::TaskEntry{});
        t.name = L"EPG Full OneShot (From UI)";
        t.display.label = L"EPG取得予約(次回)";
        t.display.category = L"EPG";
        t.display.target = L"EPG対象サービス";
        t.display.note = L"開始時刻から30分以内に起動";
        t.display.endYmd = oneShotEndYmd;
        t.display.endHHMM = oneShotEndHHMM;
        t.enabled = true;

        t.request.kind = TaskKinds::EpgFull;
        t.request.instance = 1;
        t.request.priority = TimeKeeper::TaskPriority::Soft;
        t.request.requiredTuners = 1;

        t.schedule.type = TaskListManager::ScheduleType::OneShot;
        t.schedule.dailyHour = 0;
        t.schedule.dailyMinute = 0;
        t.schedule.hourlyAtMinute = 0;
        t.schedule.intervalMinutes = 0;
        t.schedule.dueYmd = dueYmd;
        t.schedule.dueHHMM = hhmm;
        t.schedule.lateToleranceMinutes = 60;

        t.schedule.useTimeWindow = true;
        t.schedule.windowStartYmd = dueYmd;
        t.schedule.windowStartHHMM = hhmm;
        t.schedule.windowEndYmd = oneShotEndYmd;
        t.schedule.windowEndHHMM = oneShotEndHHMM;

        tl.UpsertTask(t);
    }

    const bool saved = tl.SaveToIni(tasksIni);
    if (!saved) {
        if (log_) log_(L"[TaskList] tasks.ini save failed.");
        SystemInfoLogError(L"EPG予約の登録に失敗しました。tasks.ini の保存に失敗しました。");
    }
    else {
        SystemInfoLogInfo(
            L"EPG自動更新テンプレートを保存しました。name=%ls schedule=毎日 %02d:%02d kind=%u instance=%u enabled=0",
            L"EPG Full Update (Daily)",
            h, m,
            static_cast<unsigned>(TaskKinds::EpgFull),
            0u);

        SystemInfoLogInfo(
            L"EPG予約を登録しました。name=%ls schedule=%08d %02d:%02d - %08d %02d:%02d kind=%u instance=%u enabled=1",
            L"EPG Full OneShot (From UI)",
            oneShotDueYmd,
            h, m,
            oneShotEndYmd,
            oneShotEndHHMM / 100, oneShotEndHHMM % 100,
            static_cast<unsigned>(TaskKinds::EpgFull),
            1u);
    }

    {
        auto act = getActions_ ? getActions_() : GuiActions{};
        if (act.ReloadAndApplyTasks) {
            act.ReloadAndApplyTasks();
        }
        else {
            if (log_) log_(L"[TaskList] ReloadAndApplyTasks is not connected.");
            SystemInfoLogWarn(L"EPG予約の反映に失敗しました。ReloadAndApplyTasks が未接続です。");
        }
    }

    RefreshTaskListView_();

    if (log_) {
        wchar_t msg[256]{};
        swprintf_s(msg, L"[EPG] 定時を %02d:%02d に設定（次回予約は30分範囲で tasks.ini へ反映）", h, m);
        log_(msg);
    }
}

ConfigManager::EpgHtmlRenderRoute EpgManualTab::GetSelectedHtmlRenderRoute_() const noexcept
{
    if (!cbHtmlRenderRoute_) {
        return ConfigManager::EpgHtmlRenderRoute::Legacy;
    }

    const LRESULT idx = SendMessageW(cbHtmlRenderRoute_, CB_GETCURSEL, 0, 0);
    if (idx == CB_ERR) {
        return ConfigManager::EpgHtmlRenderRoute::Legacy;
    }

    const LRESULT data = SendMessageW(cbHtmlRenderRoute_, CB_GETITEMDATA, static_cast<WPARAM>(idx), 0);
    if (data == CB_ERR) {
        return ConfigManager::EpgHtmlRenderRoute::Legacy;
    }

    return static_cast<ConfigManager::EpgHtmlRenderRoute>(static_cast<int>(data));
}

void EpgManualTab::ReloadHtmlRenderRouteSelection_()
{
    if (!cbHtmlRenderRoute_) return;

    const auto route = ConfigManager::GetInstance().GetEpgHtmlRenderRoute();
    const int count = static_cast<int>(SendMessageW(cbHtmlRenderRoute_, CB_GETCOUNT, 0, 0));

    for (int i = 0; i < count; ++i) {
        const LRESULT data = SendMessageW(cbHtmlRenderRoute_, CB_GETITEMDATA, static_cast<WPARAM>(i), 0);
        if (data != CB_ERR && static_cast<int>(data) == static_cast<int>(route)) {
            SendMessageW(cbHtmlRenderRoute_, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
            return;
        }
    }

    SendMessageW(cbHtmlRenderRoute_, CB_SETCURSEL, 0, 0);
}

void EpgManualTab::SaveHtmlRenderRouteSelection_()
{
    const auto route = GetSelectedHtmlRenderRoute_();

    auto& cfg = ConfigManager::GetInstance();
    cfg.SetEpgHtmlRenderRoute(route);
    cfg.SaveConfig();

    if (log_) {
        std::wstring msg = L"[EPG] 番組表デザインを保存しました: ";
        msg += ToRouteDisplayName_(route);
        log_(msg);
    }
}
