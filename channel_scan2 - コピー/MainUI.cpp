#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "MainUI.hpp"
#include <mutex>

#include "AutoReserveRuleStore.hpp"
#include "EpgHtmlPortal.hpp"
#include "EpgHtmlPortal_ReservationData.hpp"

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#include <algorithm>
#include <memory>
#include <filesystem>
#include <vector>
#include <utility>
#include <CommCtrl.h>
#pragma comment(lib, "Comctl32.lib")

#include <thread>
#include <atomic>
#include <windowsx.h>
#include <cstdint>
#include <exception>
#include <new>


#include <Shellapi.h>
#pragma comment(lib, "Shell32.lib")

#include "TaskKinds.hpp"
#include "TaskListManager.hpp"
#include "TimeKeeper.hpp"
#include "TaskSystemConnector.hpp"
#include "ConfigManager.h"
#include "SettingsTab.hpp"
#include "ChannelScanTab.hpp"
#include "EpgManualTab.hpp"
#include "bondriver_load.h"
#include "channel_scan_GUI.hpp"
#include "GuiDebugLogPipe.hpp"
#include "ReservationSettingsTab.hpp"
#include "TunerSettingsTab.hpp"
#include "PluginTab.hpp"
#include "NetworkSettingsTab.hpp"
#include "TimeShiftViewerTab.hpp"
#include "SystemResourceMonitorTab.hpp"
#include "DebugManager.h"


#include "Ts_Filter_Test.hpp"
#include "EpgBuild_GUI.hpp"
#include "TimeShift_Engine.hpp"
#include "AppWorkerPool.hpp"

// ------------------------------------------------------------
// globals
// ------------------------------------------------------------
int receivableCount_ = 0;

// 先に前方宣言
static int GetTodayYmd();
static int GetNowHHMM();

namespace
{
    static const wchar_t* MainUiSpaceLabel_(int space) noexcept
    {
        switch (space) {
        case 0:
            return L"地上波";
        case 1:
            return L"BS";
        case 2:
            return L"CS";
        default:
            return L"Unknown";
        }
    }

    static bool MainUiPathExistsNoThrow_(const std::wstring& path) noexcept
    {
        if (path.empty()) {
            return false;
        }

        try {
            std::error_code ec;
            return std::filesystem::exists(
                std::filesystem::path(path),
                ec) && !ec;
        }
        catch (...) {
            return false;
        }
    }

    static void MainUiDebugLogBonDriverConfigSnapshot_(
        int space,
        const wchar_t* reason,
        const wchar_t* selectedApi,
        const std::wstring& selectedPath)
    {
        ConfigManager& cfg =
            ConfigManager::GetInstance();

        const std::wstring legacyPath =
            cfg.GetBonDriverPath();

        const std::wstring terrestrialPath =
            cfg.GetBonDriverTerrestrialPath();

        const std::wstring satellitePath =
            cfg.GetBonDriverSatellitePath();

        const std::wstring defaultPath =
            cfg.GetBonDriverDefaultPath();

        const std::wstring pathForSpace =
            cfg.GetBonDriverPathForSpace(space);

        DebugLogFormat(
            L"[MainUI][BonDriver][LOAD-CHECK] reason=%ls space=%d %ls selectedApi=%ls selectedPath=%ls exists=%d",
            reason != nullptr ? reason : L"(null)",
            space,
            MainUiSpaceLabel_(space),
            selectedApi != nullptr ? selectedApi : L"(unknown)",
            selectedPath.empty() ? L"(empty)" : selectedPath.c_str(),
            MainUiPathExistsNoThrow_(selectedPath) ? 1 : 0);

        DebugLogFormat(
            L"[MainUI][BonDriver][CONFIG] Path=%ls exists=%d",
            legacyPath.empty() ? L"(empty)" : legacyPath.c_str(),
            MainUiPathExistsNoThrow_(legacyPath) ? 1 : 0);

        DebugLogFormat(
            L"[MainUI][BonDriver][CONFIG] Terrestrial=%ls exists=%d",
            terrestrialPath.empty() ? L"(empty)" : terrestrialPath.c_str(),
            MainUiPathExistsNoThrow_(terrestrialPath) ? 1 : 0);

        DebugLogFormat(
            L"[MainUI][BonDriver][CONFIG] Satellite=%ls exists=%d",
            satellitePath.empty() ? L"(empty)" : satellitePath.c_str(),
            MainUiPathExistsNoThrow_(satellitePath) ? 1 : 0);

        DebugLogFormat(
            L"[MainUI][BonDriver][CONFIG] Default=%ls exists=%d",
            defaultPath.empty() ? L"(empty)" : defaultPath.c_str(),
            MainUiPathExistsNoThrow_(defaultPath) ? 1 : 0);

        DebugLogFormat(
            L"[MainUI][BonDriver][CONFIG] ForSpace=%d %ls path=%ls exists=%d",
            space,
            MainUiSpaceLabel_(space),
            pathForSpace.empty() ? L"(empty)" : pathForSpace.c_str(),
            MainUiPathExistsNoThrow_(pathForSpace) ? 1 : 0);
    }

    static void MainUiDebugLogBonDriverSpaceMap_(
        const wchar_t* reason)
    {
        ConfigManager& cfg =
            ConfigManager::GetInstance();

        DebugLogFormat(
            L"[MainUI][BonDriver][SPACE-MAP] reason=%ls begin",
            reason != nullptr ? reason : L"(null)");

        for (int space = 0; space <= 2; ++space) {
            const std::wstring path =
                cfg.GetBonDriverPathForSpace(space);

            DebugLogFormat(
                L"[MainUI][BonDriver][SPACE-MAP] space=%d %ls path=%ls exists=%d",
                space,
                MainUiSpaceLabel_(space),
                path.empty() ? L"(empty)" : path.c_str(),
                MainUiPathExistsNoThrow_(path) ? 1 : 0);
        }

        DebugLogFormat(
            L"[MainUI][BonDriver][SPACE-MAP] reason=%ls end",
            reason != nullptr ? reason : L"(null)");
    }
}

MainUI::~MainUI() noexcept
{
    OutputDebugStringW(L"[MainUI::~MainUI] begin\n");

    StopTimeShift_();

    workerPool_.StopAndWait(true);

    ResetReservationTuners_();

    OutputDebugStringW(L"[WM_DESTROY] before delete tabSettings\n");
    delete tabSettings;     tabSettings = nullptr;
    OutputDebugStringW(L"[WM_DESTROY] after delete tabSettings\n");

    OutputDebugStringW(L"[WM_DESTROY] before delete tabTuners\n");
    delete tabTuners;       tabTuners = nullptr;
    OutputDebugStringW(L"[WM_DESTROY] after delete tabTuners\n");

    OutputDebugStringW(L"[WM_DESTROY] before delete tabChannelScan\n");
    delete tabChannelScan;  tabChannelScan = nullptr;
    OutputDebugStringW(L"[WM_DESTROY] after delete tabChannelScan\n");

    OutputDebugStringW(L"[WM_DESTROY] before delete tabEpg\n");
    delete tabEpg;          tabEpg = nullptr;
    OutputDebugStringW(L"[WM_DESTROY] after delete tabEpg\n");

    OutputDebugStringW(L"[WM_DESTROY] before delete tabReservation\n");
    delete tabReservation;  tabReservation = nullptr;
    OutputDebugStringW(L"[WM_DESTROY] after delete tabReservation\n");

    OutputDebugStringW(L"[WM_DESTROY] before delete tabNetwork\n");
    delete tabNetwork;      tabNetwork = nullptr;
    OutputDebugStringW(L"[WM_DESTROY] after delete tabNetwork\n");

    OutputDebugStringW(L"[WM_DESTROY] before delete tabPlugin\n");
    delete tabPlugin;       tabPlugin = nullptr;
    OutputDebugStringW(L"[WM_DESTROY] after delete tabPlugin\n");

    OutputDebugStringW(L"[WM_DESTROY] before delete tabTimeShiftViewer\n");
    delete tabTimeShiftViewer; tabTimeShiftViewer = nullptr;
    OutputDebugStringW(L"[WM_DESTROY] after delete tabTimeShiftViewer\n");

    OutputDebugStringW(L"[WM_DESTROY] before delete tabResourceMonitor\n");
    if (tabResourceMonitor != nullptr) {
        tabResourceMonitor->StopMonitor();
    }
    delete tabResourceMonitor;
    tabResourceMonitor = nullptr;
    OutputDebugStringW(L"[WM_DESTROY] after delete tabResourceMonitor\n");

    OutputDebugStringW(L"[WM_DESTROY] before delete tabDebug\n");
    delete tabDebug;        tabDebug = nullptr;
    OutputDebugStringW(L"[WM_DESTROY] after delete tabDebug\n");

    OutputDebugStringW(L"[MainUI::~MainUI] end\n");
}

namespace {
    std::thread g_scanThread;
    std::atomic_bool g_scanRunning{ false };
    std::atomic_bool g_scanCancel{ false };

    std::thread g_dbgFilterThread;
    std::atomic_bool g_dbgFilterRunning{ false };

    std::thread g_epgThread;
    std::atomic_bool g_epgRunning{ false };
    EpgBuildGUI::EpgBuild_GUI* g_epgRunner = nullptr;

    std::atomic_bool g_lastEpgWasFull{ false };
    std::atomic_bool g_lastEpgWasAuto{ false };

    enum class UiScanStatus : int {
        Ok = 0,
        Canceled = 1,
        BonNull = 2,
        Error = 3
    };

    static constexpr UINT_PTR TIMER_ID_TIMEKEEPER = 501;

    static constexpr UINT WM_APP_TRAY = WM_APP + 310;
    static constexpr UINT TRAY_ICON_ID = 1;
    static constexpr int  IDC_TRAY_RESTORE = 47001;
    static constexpr int  IDC_TRAY_EXIT = 47002;

    static bool g_trayAdded = false;
    static bool g_hiddenToTray = false;

    static void TrayAdd(HWND hWnd) {
        if (g_trayAdded) return;

        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hWnd;
        nid.uID = TRAY_ICON_ID;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = WM_APP_TRAY;
        nid.hIcon = (HICON)LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(nid.szTip, L"TSCEPG");

        if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            g_trayAdded = true;
        }
    }

    static void TrayRemove(HWND hWnd) {
        if (!g_trayAdded) return;

        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hWnd;
        nid.uID = TRAY_ICON_ID;
        Shell_NotifyIconW(NIM_DELETE, &nid);

        g_trayAdded = false;
        g_hiddenToTray = false;
    }

    static void TrayRestore(HWND hWnd) {
        TrayRemove(hWnd);
        ShowWindow(hWnd, SW_SHOW);
        ShowWindow(hWnd, SW_RESTORE);
        SetForegroundWindow(hWnd);
        g_hiddenToTray = false;
    }

    static void TrayShowMenu(HWND hWnd, POINT pt) {
        HMENU hMenu = CreatePopupMenu();
        if (!hMenu) return;

        InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDC_TRAY_RESTORE, L"復帰");
        InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDC_TRAY_EXIT, L"終了");

        SetForegroundWindow(hWnd);
        TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
        DestroyMenu(hMenu);
    }
}

// WM_APP
static constexpr UINT WM_APP_SCAN_LOG = WM_APP + 200;
static constexpr UINT WM_APP_SCAN_STATUS = WM_APP + 201;
static constexpr UINT WM_APP_SCAN_ADDROW = WM_APP + 202;
static constexpr UINT WM_APP_SCAN_DONE = WM_APP + 203;
static constexpr UINT WM_APP_SCAN_RUNNING = WM_APP + 204;

static constexpr UINT WM_APP_EPG_LOG = WM_APP + 220;
static constexpr UINT WM_APP_EPG_OVERALL = WM_APP + 221;
static constexpr UINT WM_APP_EPG_CHSTART = WM_APP + 222;
static constexpr UINT WM_APP_EPG_CYCLE = WM_APP + 223;
static constexpr UINT WM_APP_EPG_DONE = WM_APP + 224;
static constexpr UINT WM_APP_SCHEDULED_EPG_FULL = WM_APP + 225;
static constexpr UINT WM_APP_SCHEDULED_EPG_LIGHT = WM_APP + 226;

static constexpr UINT WM_APP_RESERVATION_DATA_CHANGED = WM_APP + 240;

struct ScanRowMsg {
    int space = 0;
    int ch = 0;
    float levelDb = 0.0f;
    std::size_t pat = 0, pmt = 0, sdt = 0, nit = 0;
    std::wstring name;
};

struct EpgOverallMsg { std::size_t done = 0; std::size_t total = 0; };
struct EpgChStartMsg { int space = 0; int ch = 0; std::wstring name; };
struct EpgCycleMsg {
    int space = 0; int ch = 0; int cycle = 0;
    std::size_t rawEvents = 0; std::size_t storeSize = 0; std::size_t processedPackets = 0;
};

struct EpgDoneMsg {
    EpgBuildGUI::Status status = EpgBuildGUI::Status::InvalidArgument;
    std::size_t targetChannels = 0;
    std::size_t processedChannels = 0;
    std::wstring mergedEpgPath;
    std::wstring htmlPath;

    bool runReservationMaintenanceAfterBuild = false;
};

static bool IsValidWnd(HWND h) noexcept { return (h != nullptr) && (::IsWindow(h) != FALSE); }

static constexpr int IDC_MAIN_SAVE = 4001;
static constexpr int IDC_MAIN_CANCEL = 4002;
static constexpr int IDC_MAIN_EXIT = 4003;

static void SetDefaultGuiFont(HWND h, HFONT f)
{
    if (!h || !f) return;
    SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
}

static void DebugLogWideChunks_(
    const wchar_t* prefix,
    std::wstring_view text)
{
    constexpr std::size_t kChunkChars = 900;

    if (prefix == nullptr) {
        prefix = L"";
    }

    if (text.empty()) {
        DebugLog(L"%ls", prefix);
        return;
    }

    for (std::size_t pos = 0; pos < text.size(); pos += kChunkChars) {
        const std::size_t remain = text.size() - pos;
        const std::size_t take =
            remain < kChunkChars ? remain : kChunkChars;

        std::wstring chunk(text.substr(pos, take));

        DebugLog(
            L"%ls%ls",
            prefix,
            chunk.c_str());
    }
}

static HMENU CtrlId(int id) noexcept
{
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

static std::wstring GetDefaultTasksIniPath()
{
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path p(exePath);
    return (p.parent_path() / L"tasks.ini").wstring();
}

static int CalcTomorrowYmd()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);

    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) {
        return (int)st.wYear * 10000 + (int)st.wMonth * 100 + (int)st.wDay;
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
        return (int)st.wYear * 10000 + (int)st.wMonth * 100 + (int)st.wDay;
    }
    return (int)st2.wYear * 10000 + (int)st2.wMonth * 100 + (int)st2.wDay;
}

static bool IsExpiredOneShot_yyyyMMdd_hhmm(int dueYmd, int dueHHMM, int lateToleranceMin)
{
    if (lateToleranceMin < 0) return false;

    SYSTEMTIME nowSt{};
    GetLocalTime(&nowSt);

    SYSTEMTIME dueSt{};
    dueSt.wYear = (WORD)(dueYmd / 10000);
    dueSt.wMonth = (WORD)((dueYmd / 100) % 100);
    dueSt.wDay = (WORD)(dueYmd % 100);
    dueSt.wHour = (WORD)(dueHHMM / 100);
    dueSt.wMinute = (WORD)(dueHHMM % 100);
    dueSt.wSecond = 0;
    dueSt.wMilliseconds = 0;

    FILETIME ftNow{}, ftDue{};
    if (!SystemTimeToFileTime(&nowSt, &ftNow)) return false;
    if (!SystemTimeToFileTime(&dueSt, &ftDue)) return false;

    ULARGE_INTEGER uNow{}, uDue{};
    uNow.LowPart = ftNow.dwLowDateTime; uNow.HighPart = ftNow.dwHighDateTime;
    uDue.LowPart = ftDue.dwLowDateTime; uDue.HighPart = ftDue.dwHighDateTime;

    const ULONGLONG tol100ns = (ULONGLONG)lateToleranceMin * 60ULL * 10ULL * 1000ULL * 1000ULL;
    return uNow.QuadPart > (uDue.QuadPart + tol100ns);
}

static void NormalizeEpgFullOneShot(TaskListManager& tl, const std::wstring& tasksIniPath)
{
    auto one = tl.Find(TaskKinds::EpgFull, 1);
    if (!one.has_value()) return;
    if (!one->enabled) return;
    if (one->schedule.type != TaskListManager::ScheduleType::OneShot) return;

    const int dueYmd = one->schedule.dueYmd;
    const int dueHHMM = one->schedule.dueHHMM;
    const int tolMin = one->schedule.lateToleranceMinutes;

    if (!IsExpiredOneShot_yyyyMMdd_hhmm(dueYmd, dueHHMM, tolMin)) return;

    const int hhmm = ConfigManager::GetInstance().GetEpgDailyFullHHMM();
    const int nowHHMM = GetNowHHMM();
    const int todayYmd = GetTodayYmd();
    const int nextYmd = (nowHHMM <= hhmm) ? todayYmd : CalcTomorrowYmd();

    TaskListManager::TaskEntry updated = *one;
    updated.name = L"EPG Full OneShot (Reissued)";
    updated.schedule.dueYmd = nextYmd;
    updated.schedule.dueHHMM = hhmm;

    tl.UpsertTask(updated);
    (void)tl.SaveToIni(tasksIniPath);
}

static int GetTodayYmd()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return (int)st.wYear * 10000 + (int)st.wMonth * 100 + (int)st.wDay;
}

static int GetNowHHMM()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return (int)st.wHour * 100 + (int)st.wMinute;
}

static std::wstring LoadBonDriverPathFromIni(const std::wstring& iniFullPath)
{
    wchar_t buf[1024]{};
    GetPrivateProfileStringW(L"BonDriver", L"Path", L"", buf, (DWORD)_countof(buf), iniFullPath.c_str());
    std::wstring v = buf;
    if (v.empty()) return L"";

    std::filesystem::path p(v);
    if (p.is_absolute()) return p.wstring();

    std::filesystem::path base(iniFullPath);
    return (base.parent_path() / p).lexically_normal().wstring();
}

static int LoadIniInt(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, int defv)
{
    return (int)GetPrivateProfileIntW(sec, key, defv, ini.c_str());
}

namespace {
    template<class OptT>
    void TryBindStatusCallbacks(OptT& opt, HWND hwnd)
    {
        if constexpr (requires(OptT o) { o.onStatusText = std::function<void(const std::wstring&)>{}; }) {
            opt.onStatusText = [hwnd](const std::wstring& s) {
                PostMessageW(hwnd, WM_APP_SCAN_STATUS, 0, (LPARAM)new std::wstring(s));
                };
        }
        if constexpr (requires(OptT o) { o.onRunningState = std::function<void(bool)>{}; }) {
            opt.onRunningState = [hwnd](bool running) {
                PostMessageW(hwnd, WM_APP_SCAN_RUNNING, running ? 1 : 0, 0);
                };
        }
    }
}

bool MainUI::CreateAndShow(HINSTANCE inst, int nCmdShow)
{
    hInst = inst;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_TAB_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MainUI::WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"TSCEPG_MainWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

    if (!RegisterClassExW(&wc)) return false;

    auto& cfg = ConfigManager::GetInstance();
    const int x = cfg.GetUiWindowX();
    const int y = cfg.GetUiWindowY();
    const int w = cfg.GetUiWindowW();
    const int h = cfg.GetUiWindowH();

    hWnd = CreateWindowExW(
        0, wc.lpszClassName, L"TSCEPG GUI Console",
        WS_OVERLAPPEDWINDOW,
        x, y, w, h,
        nullptr, nullptr, hInst, this);

    if (!hWnd) return false;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    return true;
}

void MainUI::AppendLog(std::wstring_view text)
{
    if (!hLog) return;

    const int len = GetWindowTextLengthW(hLog);
    SendMessageW(hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);

    std::wstring line(text);
    if (!line.empty() && line.back() != L'\n') line += L"\r\n";
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
}

/*
LRESULT CALLBACK MainUI::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    


    MainUI* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<MainUI*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hWnd = h;
    }
    else {
        self = reinterpret_cast<MainUI*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    return self ? self->HandleMessage(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}
*/
LRESULT CALLBACK MainUI::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCDESTROY) {
        OutputDebugStringW(L"[WndProc] WM_NCDESTROY enter\n");
    }

    MainUI* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<MainUI*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hWnd = h;
    }
    else {
        self = reinterpret_cast<MainUI*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }

    LRESULT result = self ? self->HandleMessage(msg, wp, lp)
        : DefWindowProcW(h, msg, wp, lp);

    if (msg == WM_NCDESTROY) {
        OutputDebugStringW(L"[WndProc] WM_NCDESTROY after HandleMessage\n");
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        if (self && self->hWnd == h) {
            self->hWnd = nullptr;
        }
        OutputDebugStringW(L"[WndProc] WM_NCDESTROY leave\n");
    }

    return result;
}

LRESULT MainUI::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        OnCreate();
        return 0;

    case WM_SIZE:
        OnSize(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_SYSCOMMAND:
    {
        const WPARAM cmd = (wp & 0xFFF0);
        if (cmd == SC_MINIMIZE) {
            if (ConfigManager::GetInstance().GetUiMinimizeToTray()) {
                TrayAdd(hWnd);
                ShowWindow(hWnd, SW_HIDE);
                g_hiddenToTray = true;
                return 0;
            }
        }
        return DefWindowProcW(hWnd, msg, wp, lp);
    }

    case WM_APP_TRAY:
    {
        const LPARAM ev = lp;
        if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) {
            TrayRestore(hWnd);
            return 0;
        }
        if (ev == WM_RBUTTONUP) {
            POINT pt{};
            GetCursorPos(&pt);
            TrayShowMenu(hWnd, pt);
            return 0;
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_ID_TIMEKEEPER) {
            taskSystem_.OnTick();
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wp, lp);

    case WM_NOTIFY:
    {
        const auto* nm = reinterpret_cast<const NMHDR*>(lp);
        if (nm && nm->hwndFrom == hTab && nm->code == TCN_SELCHANGE) {
            // EPG実行中はタブ切替を許可する。
            // チャンネルスキャン等の強いロック中だけ、従来通りタブ切替を戻す。
            const bool epgBusy = g_epgRunning.load(std::memory_order_acquire);

            if (scanUiLocked && !epgBusy) {
                if (IsValidWnd(hTab)) {
                    TabCtrl_SetCurSel(hTab, currentTab);
                }
                return 0;
            }

            OnTabChanged();
            return 0;
        }
        return 0;
    }

    case WM_COMMAND:
    {
        const int id = LOWORD(wp);

        if (id == IDC_TRAY_RESTORE) {
            TrayRestore(hWnd);
            return 0;
        }
        if (id == IDC_TRAY_EXIT) {
            ExitWithPrompt();
            return 0;
        }

        if (id == IDC_MAIN_SAVE || id == IDC_MAIN_CANCEL || id == IDC_MAIN_EXIT) {
            if (scanUiLocked) {
                AppendLog(L"[GUI] 実行中のため操作できません。");
                return 0;
            }
            OnMainButton(id);
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wp, lp);
    }

    case WM_CLOSE:
        if (scanUiLocked) {
            MessageBoxW(hWnd, L"実行中は終了できません。\n完了後に終了してください。", L"TSCEPG", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        ExitWithPrompt();
        return 0;

    case WM_DESTROY:
    {
        OutputDebugStringW(L"[WM_DESTROY] begin\n");

        TrayRemove(hWnd);
        OutputDebugStringW(L"[WM_DESTROY] after TrayRemove\n");

        OutputDebugStringW(L"[WM_DESTROY] before StopTimeShift_()\n");
        StopTimeShift_();
        OutputDebugStringW(L"[WM_DESTROY] after StopTimeShift_()\n");

        OutputDebugStringW(L"[WM_DESTROY] before workerPool_.StopAndWait(true)\n");
        workerPool_.StopAndWait(true);
        OutputDebugStringW(L"[WM_DESTROY] after workerPool_.StopAndWait(true)\n");

        if (tabResourceMonitor != nullptr) {
            OutputDebugStringW(L"[WM_DESTROY] before tabResourceMonitor->StopMonitor()\n");
            tabResourceMonitor->StopMonitor();
            OutputDebugStringW(L"[WM_DESTROY] after tabResourceMonitor->StopMonitor()\n");
        }

        OutputDebugStringW(L"[WM_DESTROY] before taskSystem_.Stop(true)\n");
        taskSystem_.Stop(true);
        OutputDebugStringW(L"[WM_DESTROY] after taskSystem_.Stop(true)\n");

        OutputDebugStringW(L"[WM_DESTROY] before g_scanCancel.store(true)\n");
        g_scanCancel.store(true);
        OutputDebugStringW(L"[WM_DESTROY] after g_scanCancel.store(true)\n");

        if (g_epgRunner) {
            OutputDebugStringW(L"[WM_DESTROY] before g_epgRunner->RequestCancel()\n");
            g_epgRunner->RequestCancel();
            OutputDebugStringW(L"[WM_DESTROY] after g_epgRunner->RequestCancel()\n");
        }
        else {
            OutputDebugStringW(L"[WM_DESTROY] g_epgRunner == nullptr\n");
        }

        if (g_scanThread.joinable()) {
            OutputDebugStringW(L"[WM_DESTROY] before g_scanThread.join()\n");
            g_scanThread.join();
            OutputDebugStringW(L"[WM_DESTROY] after g_scanThread.join()\n");
        }
        else {
            OutputDebugStringW(L"[WM_DESTROY] g_scanThread not joinable\n");
        }
        g_scanRunning.store(false);
        OutputDebugStringW(L"[WM_DESTROY] after g_scanRunning.store(false)\n");

        g_scanCancel.store(false);
        OutputDebugStringW(L"[WM_DESTROY] after g_scanCancel.store(false)\n");

        if (g_epgThread.joinable()) {
            OutputDebugStringW(L"[WM_DESTROY] before g_epgThread.join()\n");
            g_epgThread.join();
            OutputDebugStringW(L"[WM_DESTROY] after g_epgThread.join()\n");
        }
        else {
            OutputDebugStringW(L"[WM_DESTROY] g_epgThread not joinable\n");
        }
        g_epgRunning.store(false);
        OutputDebugStringW(L"[WM_DESTROY] after g_epgRunning.store(false)\n");

        // BonDriverを参照する全workerが終了してから、残存Leaseを解放する。
        ResetReservationTuners_();

        if (g_epgRunner) {
            OutputDebugStringW(L"[WM_DESTROY] before g_epgRunner->Reset()\n");
            g_epgRunner->Reset();
            OutputDebugStringW(L"[WM_DESTROY] after g_epgRunner->Reset()\n");

            OutputDebugStringW(L"[WM_DESTROY] before delete g_epgRunner\n");
            delete g_epgRunner;
            g_epgRunner = nullptr;
            OutputDebugStringW(L"[WM_DESTROY] after delete g_epgRunner\n");
        }
        else {
            OutputDebugStringW(L"[WM_DESTROY] skip g_epgRunner reset/delete (nullptr)\n");
        }

        OutputDebugStringW(L"[WM_DESTROY] before tunerHandle_.Reset()\n");
        tunerHandle_.Reset();
        OutputDebugStringW(L"[WM_DESTROY] after tunerHandle_.Reset()\n");

        if (g_dbgFilterThread.joinable()) {
            OutputDebugStringW(L"[WM_DESTROY] before g_dbgFilterThread.join()\n");
            g_dbgFilterThread.join();
            OutputDebugStringW(L"[WM_DESTROY] after g_dbgFilterThread.join()\n");
        }
        else {
            OutputDebugStringW(L"[WM_DESTROY] g_dbgFilterThread not joinable\n");
        }
        g_dbgFilterRunning.store(false);
        OutputDebugStringW(L"[WM_DESTROY] after g_dbgFilterRunning.store(false)\n");

        if (saveConfigOnDestroy) {
            OutputDebugStringW(L"[WM_DESTROY] before saveConfigOnDestroy block\n");

            RECT rc{};
            GetWindowRect(hWnd, &rc);

            auto& cfg = ConfigManager::GetInstance();
            cfg.SetUiLastTabIndex(currentTab);
            cfg.SetUiWindowRect(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
            cfg.SaveConfig();



            if (tabNetwork) {
                tabNetwork->ApplyToConfig();
            }

            OutputDebugStringW(L"[WM_DESTROY] after saveConfigOnDestroy block\n");
        }
        else {
            OutputDebugStringW(L"[WM_DESTROY] saveConfigOnDestroy == false\n");
        }


        OutputDebugStringW(L"[WM_DESTROY] before registeredPages_.clear()\n");
        registeredPages_.clear();
        OutputDebugStringW(L"[WM_DESTROY] after registeredPages_.clear()\n");

        

        if (hFont) {
            OutputDebugStringW(L"[WM_DESTROY] before DeleteObject(hFont)\n");
            DeleteObject(hFont);
            hFont = nullptr;
            OutputDebugStringW(L"[WM_DESTROY] after DeleteObject(hFont)\n");
        }
        else {
            OutputDebugStringW(L"[WM_DESTROY] hFont == nullptr\n");
        }

        OutputDebugStringW(L"[WM_DESTROY] before actions reset\n");
        actions = GuiActions{};
        OutputDebugStringW(L"[WM_DESTROY] after actions reset\n");

        OutputDebugStringW(L"[WM_DESTROY] before PostQuitMessage(0)\n");
        PostQuitMessage(0);
        OutputDebugStringW(L"[WM_DESTROY] after PostQuitMessage(0)\n");

        return 0;
    }
        case WM_APP_EPG_LOG:
        {
            std::unique_ptr<std::wstring> p(reinterpret_cast<std::wstring*>(lp));
            if (p) {
                AppendLog(*p);
            }
            return 0;
        }

        case WM_APP_EPG_OVERALL:
        {
            std::unique_ptr<EpgOverallMsg> p(reinterpret_cast<EpgOverallMsg*>(lp));
            if (p) {
                wchar_t buf[128]{};
                swprintf_s(buf, L"[EPG] overall %zu / %zu", p->done, p->total);
                AppendLog(buf);
                SetMainStatusText(buf);
            }
            return 0;
        }

        case WM_APP_EPG_CHSTART:
        {
            std::unique_ptr<EpgChStartMsg> p(reinterpret_cast<EpgChStartMsg*>(lp));
            if (p) {
                std::wstring msg = L"[EPG] channel start: space=" + std::to_wstring(p->space)
                    + L" ch=" + std::to_wstring(p->ch) + L" " + p->name;
                AppendLog(msg);
                SetMainStatusText(msg);
            }
            return 0;
        }

        case WM_APP_EPG_CYCLE:
        {
            std::unique_ptr<EpgCycleMsg> p(reinterpret_cast<EpgCycleMsg*>(lp));
            if (p) {
                wchar_t buf[256]{};
                swprintf_s(buf,
                    L"[EPG] cycle: space=%d ch=%d cycle=%d raw=%zu store=%zu processed=%zu",
                    p->space, p->ch, p->cycle,
                    p->rawEvents, p->storeSize, p->processedPackets);
                AppendLog(buf);
                SetMainStatusText(buf);
            }
            return 0;
        }

        case WM_APP_EPG_DONE:
        {
            std::unique_ptr<EpgDoneMsg> p(reinterpret_cast<EpgDoneMsg*>(lp));

            SetEpgTabBusyState_(false);
            SetScanUiLocked(false);

            if (!p) {
                AppendLog(L"[EPG] 完了通知が不正です。");
                SetMainStatusText(L"EPG 完了通知異常");
                return 0;
            }

            switch (p->status) {
            case EpgBuildGUI::Status::Ok:
            {
                AppendLog(L"[EPG] 完了しました。");
                if (!p->htmlPath.empty()) {
                    AppendLog(std::wstring(L"[EPG] HTML: ") + p->htmlPath);
                }

                if (p->runReservationMaintenanceAfterBuild) {
                    AppendLog(L"[Reservation] EPG更新後の予約タスク再同期を開始します。");

                    const auto sync =
                        taskSystem_.ReloadTasksAfterExternalEpgUpdate(false);

                    if (sync.ok) {
                        AppendLog(
                            L"[Reservation] EPG更新後の予約タスク再同期が完了しました。");

                        if (!sync.tasksIniPath.empty()) {
                            AppendLog(
                                std::wstring(L"[Reservation] tasks.ini: ") +
                                sync.tasksIniPath);
                        }
                    }
                    else {
                        AppendLog(
                            std::wstring(L"[Reservation] EPG更新後の予約タスク再同期に失敗しました。") +
                            L" result=" +
                            sync.message);

                        if (!sync.tasksIniPath.empty()) {
                            AppendLog(
                                std::wstring(L"[Reservation] tasks.ini: ") +
                                sync.tasksIniPath);
                        }
                    }
                }

                SetMainStatusText(L"EPG 完了");
                break;
            }

            case EpgBuildGUI::Status::Cancelled:
                AppendLog(L"[EPG] 中断しました。");
                SetMainStatusText(L"EPG 中断");
                break;

            case EpgBuildGUI::Status::ChannelListMissing:
                AppendLog(L"[EPG] channellist がありません。先にチャンネルスキャンが必要です。");
                SetMainStatusText(L"channellist 不足");
                break;

            default:
                AppendLog(L"[EPG] 失敗しました。");
                SetMainStatusText(L"EPG 失敗");
                break;
            }

            return 0;
        }

        case WM_APP_RESERVATION_DATA_CHANGED:
        {
            if (tabEpg) {
                tabEpg->NotifyReservationDataChanged();
            }

            if (tabReservation) {
                tabReservation->ReloadFromConfig();
            }

            SetMainStatusText(L"予約データ更新を反映しました。");
            return 0;
        }

        case WM_APP_SCHEDULED_EPG_FULL:
        {
            AppendLog(L"[Task] スケジュールされた EPGフルスキャンを開始します。");
            SetMainStatusText(L"スケジュールEPGフルスキャン開始");

            // 定時フル更新後だけ、終了済み予約の整理を許可する。
            StartEpgTask_(false, true);
            return 0;
        }

        case WM_APP_SCHEDULED_EPG_LIGHT:
        {
            AppendLog(L"[Task] スケジュールされた リアルタイム更新を開始します。");
            SetMainStatusText(L"スケジュールEPG軽量更新開始");

            StartEpgTask_(true, false);
            return 0;
        }

    default:
        return DefWindowProcW(hWnd, msg, wp, lp);
    }
}

void MainUI::RegisterPage(RegisteredPage page)
{
    registeredPages_.push_back(std::move(page));
}

void MainUI::RegisterCorePages()
{
    RegisterPage({
        L"設定",
        true,
        false,
        [this](HWND parent, const RECT& rc) -> bool {
            if (!tabSettings) {
                tabSettings = new SettingsTab();
            }
            if (!tabSettings) {
                return false;
            }

            return tabSettings->Create(
                parent,
                rc,
                hInst,
                hFont,
                [this]() -> GuiActions& { return actions; },
                [this](std::wstring_view s) { AppendLog(s); });
        },
        [this](bool show) {
            if (tabSettings) tabSettings->Show(show);
        },
        [this](const RECT& rc) {
            if (tabSettings) tabSettings->Move(rc);
        },
        [this]() {
            if (tabSettings) tabSettings->ApplyToConfig();
        },
        [this]() {
            if (tabSettings) tabSettings->ReloadFromConfig();
        },
        {}
        });

    RegisterPage({
        L"チューナー設定",
        true,
        false,
        [this](HWND parent, const RECT& rc) -> bool {
            if (!tabTuners) {
                tabTuners = new TunerSettingsTab();
            }
            if (!tabTuners) {
                return false;
            }

            auto& cfg = ConfigManager::GetInstance();
            return tabTuners->Create(parent, rc, hInst, hFont, cfg);
        },
        [this](bool show) {
            if (tabTuners) tabTuners->Show(show);
        },
        [this](const RECT& rc) {
            if (tabTuners) tabTuners->Move(rc);
        },
        [this]() {
            if (tabTuners) tabTuners->ApplyToConfig();
        },
        [this]() {
            if (tabTuners) tabTuners->ReloadFromConfig();
        },
        [this]() {
            if (tabTuners) {
                tabTuners->UpdateTunerCount(ConfigManager::GetInstance().GetTunersCount());
            }
        }
        });

    RegisterPage({
        L"チャンネルスキャン",
        true,
        false,
        [this](HWND parent, const RECT& rc) -> bool {
            if (!tabChannelScan) {
                tabChannelScan = new ChannelScanTab();
            }
            if (!tabChannelScan) {
                return false;
            }

            return tabChannelScan->Create(
                parent,
                rc,
                hInst,
                hFont,
                [this]() -> GuiActions& { return actions; },
                [this](std::wstring_view s) { AppendLog(s); });
        },
        [this](bool show) {
            if (tabChannelScan) tabChannelScan->Show(show);
        },
        [this](const RECT& rc) {
            if (tabChannelScan) tabChannelScan->Move(rc);
        },
        [this]() {
            if (tabChannelScan) tabChannelScan->ApplyToConfig();
        },
        [this]() {
            if (tabChannelScan) tabChannelScan->ReloadFromConfig();
        },
        {}
        });

    RegisterPage({
        L"EPG手動",
        true,
        false,
        [this](HWND parent, const RECT& rc) -> bool {
            if (!tabEpg) {
                tabEpg = new EpgManualTab();
            }
            if (!tabEpg) {
                return false;
            }

            return tabEpg->Create(
                parent,
                rc,
                hInst,
                hFont,
                [this]() -> GuiActions& { return actions; },
                [this](std::wstring_view s) { AppendLog(s); });
        },
        [this](bool show) {
            if (tabEpg) tabEpg->Show(show);
        },
        [this](const RECT& rc) {
            if (tabEpg) tabEpg->Move(rc);
        },
        [this]() {
            if (tabEpg) tabEpg->ApplyToConfig();
        },
        [this]() {
            if (tabEpg) tabEpg->ReloadFromConfig();
        },
        {}
        });

    RegisterPage({
        L"予約設定",
        true,
        false,
        [this](HWND parent, const RECT& rc) -> bool {
            if (!tabReservation) {
                tabReservation = new ReservationSettingsTab();
            }
            if (!tabReservation) {
                return false;
            }

            auto& cfg = ConfigManager::GetInstance();
            const int tunerCount = cfg.GetTunersCount();
            return tabReservation->Create(parent, rc, hInst, hFont, cfg, tunerCount);
        },
        [this](bool show) {
            if (tabReservation) tabReservation->Show(show);
        },
        [this](const RECT& rc) {
            if (tabReservation) tabReservation->Move(rc);
        },
        [this]() {
            if (tabReservation) tabReservation->ApplyToConfig();
        },
        [this]() {
            if (tabReservation) tabReservation->ReloadFromConfig();
        },
        [this]() {
            if (tabReservation) {
                tabReservation->UpdateTunerCount(ConfigManager::GetInstance().GetTunersCount());
            }
        }
        });

    RegisterPage({
    L"タイムシフト",
    true,
    false,
    [this](HWND parent, const RECT& rc) -> bool {
        if (!tabTimeShiftViewer) {
            tabTimeShiftViewer = new TimeShiftViewerTab();
        }
        if (!tabTimeShiftViewer) {
            return false;
        }

        return tabTimeShiftViewer->Create(
            parent,
            rc,
            hInst,
            hFont,
            [this](std::wstring_view s)
            {
                AppendLog(s);

                DebugLogWideChunks_(
                    L"[TimeShiftViewer][GUI] ",
                    s);
            },
            &workerPool_);
    },
    [this](bool show) {
        if (tabTimeShiftViewer) tabTimeShiftViewer->Show(show);
    },
    [this](const RECT& rc) {
        if (tabTimeShiftViewer) tabTimeShiftViewer->Move(rc);
    },
    [this]() {
        if (tabTimeShiftViewer) tabTimeShiftViewer->ApplyToConfig();
    },
    [this]() {
        if (tabTimeShiftViewer) tabTimeShiftViewer->ReloadFromConfig();
    },
    [this]() {
        if (tabTimeShiftViewer) {
            tabTimeShiftViewer->ReloadFromConfig();
        }
    }
        });

    RegisterPage({
    L"端末監視",
    true,
    false,
    [this](HWND parent, const RECT& rc) -> bool {
        if (!tabResourceMonitor) {
            tabResourceMonitor = new SystemResourceMonitorTab();
        }
        if (!tabResourceMonitor) {
            return false;
        }

        return tabResourceMonitor->Create(
            parent,
            rc,
            hInst,
            hFont,
            [this](std::wstring_view s)
            {
                // 端末監視タブは debug_*.log へミラーしない。
                // 観測値本体は console.log に出す。
                AppendLog(s);
            });
    },
    [this](bool show) {
        if (tabResourceMonitor) tabResourceMonitor->Show(show);
    },
    [this](const RECT& rc) {
        if (tabResourceMonitor) tabResourceMonitor->Move(rc);
    },
    [this]() {
        if (tabResourceMonitor) tabResourceMonitor->ApplyToConfig();
    },
    [this]() {
        if (tabResourceMonitor) tabResourceMonitor->ReloadFromConfig();
    },
    [this]() {
        if (tabResourceMonitor) {
            tabResourceMonitor->ReloadFromConfig();
        }
    }
        });

    RegisterPage({
    L"ネットワーク",
    true,
    false,
    [this](HWND parent, const RECT& rc) -> bool {
        if (!tabNetwork) {
            tabNetwork = new NetworkSettingsTab();
        }
        if (!tabNetwork) {
            return false;
        }

        auto& cfg = ConfigManager::GetInstance();
        return tabNetwork->Create(parent, rc, hInst, hFont, cfg);
    },
    [this](bool show) {
        if (tabNetwork) tabNetwork->Show(show);
    },
    [this](const RECT& rc) {
        if (tabNetwork) tabNetwork->Move(rc);
    },
    [this]() {
        if (tabNetwork) tabNetwork->ApplyToConfig();
    },
    [this]() {
        if (tabNetwork) tabNetwork->ReloadFromConfig();
    },
    {}
        });

    RegisterPage({
    L"プラグイン",
    true,
    false,
    [this](HWND parent, const RECT& rc) -> bool {
        if (!tabPlugin) {
            tabPlugin = new PluginTab();
        }
        if (!tabPlugin) {
            return false;
        }

        return tabPlugin->Create(
            parent,
            rc,
            hInst,
            hFont,
            [this](std::wstring_view s) { AppendLog(s); });
    },
    [this](bool show) {
        if (tabPlugin) tabPlugin->Show(show);
    },
    [this](const RECT& rc) {
        if (tabPlugin) tabPlugin->Move(rc);
    },
    [this]() {
        if (tabPlugin) tabPlugin->ApplyToConfig();
    },
    [this]() {
        if (tabPlugin) tabPlugin->ReloadFromConfig();
    },
    {}
        });

    RegisterPage({
        L"デバッグ",
        true,
        false,
        [this](HWND parent, const RECT& rc) -> bool {
            if (!tabDebug) {
                tabDebug = new GuiDebugLogPipeTab();
            }
            if (!tabDebug) {
                return false;
            }

            return tabDebug->Create(
                parent,
                rc,
                hInst,
                hFont,
                [this]() -> GuiActions& { return actions; },
                [this](std::wstring_view s) { AppendLog(s); });
        },
        [this](bool show) {
            if (tabDebug) tabDebug->Show(show);
        },
        [this](const RECT& rc) {
            if (tabDebug) tabDebug->Move(rc);
        },
        [this]() {
            if (tabDebug) tabDebug->ApplyToConfig();
        },
        [this]() {
            if (tabDebug) tabDebug->ReloadFromConfig();
        },
        {}
        });
}

void MainUI::RegisterPluginPages()
{
    // 将来のプラグインDLL自動認識用
    // いまは空実装
}

void MainUI::BuildPageRegistry()
{
    registeredPages_.clear();
    RegisterCorePages();
    RegisterPluginPages();
}

bool MainUI::CreateRegisteredPages()
{
    if (!IsValidWnd(hPageHost)) {
        AppendLog(L"[GUI] hPageHost が無効です。");
        return false;
    }

    RECT rcLocal{};
    GetClientRect(hPageHost, &rcLocal);

    const int pageW = rcLocal.right - rcLocal.left;
    const int pageH = rcLocal.bottom - rcLocal.top;
    if (pageW <= 0 || pageH <= 0) {
        AppendLog(L"[GUI] hPageHost のサイズが未確定です。");
        return false;
    }

    bool anyCreated = false;

    for (auto& page : registeredPages_) {
        if (page.created) {
            anyCreated = true;
            continue;
        }

        if (!page.create) {
            AppendLog(L"[GUI] ページ生成関数が未登録: " + page.title);
            continue;
        }

        const bool ok = page.create(hPageHost, rcLocal);
        page.created = ok;

        if (ok) {
            anyCreated = true;
            AppendLog(L"[GUI] ページ生成成功: " + page.title);
        }
        else {
            AppendLog(L"[GUI] ページ生成失敗: " + page.title);
        }
    }

    pagesCreated_ = anyCreated;
    return anyCreated;
}

void MainUI::MoveRegisteredPages(const RECT& rcLocal)
{
    if (!pagesCreated_) {
        return;
    }

    for (auto& page : registeredPages_) {
        if (!page.created) {
            continue;
        }
        if (page.move) {
            page.move(rcLocal);
        }
    }
}


void MainUI::SyncDependentTabsFromConfig()
{
    auto& cfg = ConfigManager::GetInstance();
    const int tunerCount = cfg.GetTunersCount();

    if (tabTuners) {
        tabTuners->UpdateTunerCount(tunerCount);
    }
    if (tabReservation) {
        tabReservation->UpdateTunerCount(tunerCount);
    }
}

void MainUI::OnCreate()
{
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    hFont = CreateFontIndirectW(&ncm.lfMessageFont);

    hTab = CreateWindowExW(
        0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, 100, 100,
        hWnd, nullptr, hInst, nullptr);

    if (!hTab) {
        MessageBoxW(hWnd, L"TabControl の作成に失敗しました。\n初期化を中断します。", L"channel_scan", MB_OK | MB_ICONERROR);
        DestroyWindow(hWnd);
        return;
    }

    SetDefaultGuiFont(hTab, hFont);

    // 重要:
    // hPageHost は hWnd の子ではなく hTab の子にする
    hPageHost = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 100, 100,
        hTab, nullptr, hInst, nullptr);

    if (!hPageHost) {
        MessageBoxW(hWnd, L"ページホストの作成に失敗しました。\n初期化を中断します。", L"channel_scan", MB_OK | MB_ICONERROR);
        DestroyWindow(hWnd);
        return;
    }

    hBtnSave = CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE, 0, 0, 80, 28, hWnd, CtrlId(IDC_MAIN_SAVE), hInst, nullptr);
    hBtnCancel = CreateWindowExW(0, L"BUTTON", L"キャンセル", WS_CHILD | WS_VISIBLE, 0, 0, 110, 28, hWnd, CtrlId(IDC_MAIN_CANCEL), hInst, nullptr);
    hBtnExit = CreateWindowExW(0, L"BUTTON", L"終了", WS_CHILD | WS_VISIBLE, 0, 0, 80, 28, hWnd, CtrlId(IDC_MAIN_EXIT), hInst, nullptr);

    SetDefaultGuiFont(hBtnSave, hFont);
    SetDefaultGuiFont(hBtnCancel, hFont);
    SetDefaultGuiFont(hBtnExit, hFont);

    hStatus = CreateWindowExW(0, L"STATIC", L"Ready.", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 100, 28, hWnd, nullptr, hInst, nullptr);
    SetDefaultGuiFont(hStatus, hFont);

    hLog = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        0, 0, 100, 100, hWnd, nullptr, hInst, nullptr);
    SetDefaultGuiFont(hLog, hFont);

    InitGuiActions_();

    {
        AppWorker::WorkerPoolOptions workerOpt{};
        workerOpt.workerCount = 0;
        workerOpt.reserveCoreForUi = true;
        workerOpt.backgroundMode = true;
        workerOpt.threadNamePrefix = L"TSCEPG_Worker";

        if (workerPool_.Start(workerOpt)) {
            AppendLog(L"[Worker] AppWorkerPool を開始しました。");
        }
        else {
            AppendLog(L"[Worker] AppWorkerPool の開始に失敗しました。");
        }
    }

    BuildPageRegistry();

    TCITEMW item{};
    item.mask = TCIF_TEXT;
    for (auto& page : registeredPages_) {
        item.pszText = const_cast<LPWSTR>(page.title.c_str());
        TabCtrl_InsertItem(hTab, TabCtrl_GetItemCount(hTab), &item);
    }

    auto& cfg = ConfigManager::GetInstance();
    currentTab = std::clamp(cfg.GetUiLastTabIndex(), 0, std::max(0, static_cast<int>(registeredPages_.size()) - 1));
    if (IsValidWnd(hTab)) {
        TabCtrl_SetCurSel(hTab, currentTab);
    }

    pagesCreated_ = false;

    RECT rcClient{};
    GetClientRect(hWnd, &rcClient);
    OnSize(rcClient.right - rcClient.left, rcClient.bottom - rcClient.top);

    AppendLog(L"[GUI] MainUI created.");

    {
        auto& cfg = ConfigManager::GetInstance();
        const std::wstring iniPath = cfg.GetIniPath();
        const std::wstring tasksIni = GetDefaultTasksIniPath();

        TaskSystemConnector::Options opt{};
        opt.tunerCount = std::max(1, cfg.GetTunersCount());
        opt.enableUtilityWorker = true;
        opt.maxQueueSize = 256;

        opt.enableAuto =
            (cfg.GetEpgAutoMode() == ConfigManager::EpgAutoMode::Task);

        opt.bootGraceSeconds = cfg.GetEpgBootGraceSec();
        opt.catchUpOnStart = cfg.GetEpgCatchUpOnStart();
        opt.tickSeconds = cfg.GetEpgTickSeconds();
        opt.idleRequiredSeconds = cfg.GetEpgIdleRequiredSec();
        opt.enqueueRetryBackoffSeconds = 30;

        // EpgManualTab から保存される EPG 予約は RequiredTuners=0 なので、
        // まずは utility worker 経由で安全に起動させる。
        opt.epgRequiredTuners = 0;
        opt.tasksIniPath = tasksIni;

        reservationExecutionManager_.EnsureDefaultConfig(cfg);
        reservationExecutionManager_.ConfigureFromConfig(cfg);
        reservationExecutionManager_.SetTaskSystem(&taskSystem_);

        TaskSystemConnector::Callbacks cb{};
        cb.Log = [hwnd = hWnd](const std::wstring& s) {
            if (!IsValidWnd(hwnd)) return;
            auto* p = new std::wstring(s);
            if (!PostMessageW(hwnd, WM_APP_EPG_LOG, 0, reinterpret_cast<LPARAM>(p))) {
                delete p;
            }
            };

        cb.CanRun = []() -> bool {
            return !g_scanRunning.load(std::memory_order_acquire)
                && !g_epgRunning.load(std::memory_order_acquire);
            };

        cb.OnEpgFull = [hwnd = hWnd](int /*tunerIndex*/, IBonDriver2* /*bon*/) {
            if (IsValidWnd(hwnd)) {
                PostMessageW(hwnd, WM_APP_SCHEDULED_EPG_FULL, 0, 0);
            }
            };

        cb.OnEpgLight = [hwnd = hWnd](int /*tunerIndex*/, IBonDriver2* /*bon*/) {
            if (IsValidWnd(hwnd)) {
                PostMessageW(hwnd, WM_APP_SCHEDULED_EPG_LIGHT, 0, 0);
            }
            };

        cb.OnRecPrepare = [this](
            int tunerIndex,
            IBonDriver2* /*bon*/,
            const TimeKeeper::TaskRequest& req)
            {
                TunerPortManager::Lease lease{};
                if (!AcquireReservationTuner_(tunerIndex, req, lease)) {
                    DebugLogFormat(
                        L"[MainUI][ReservationTuner][PREPARE] acquire failed tuner=%d inst=%u",
                        tunerIndex,
                        static_cast<unsigned>(req.instance));
                    return;
                }

                wchar_t buf[256]{};
                swprintf_s(
                    buf,
                    L"[MainUI] OnRecPrepare -> ReservationExecution tuner=%d kind=%u inst=%u bon=%p\n",
                    lease.tunerIndex,
                    static_cast<unsigned>(req.kind),
                    static_cast<unsigned>(req.instance),
                    lease.bon);
                OutputDebugStringW(buf);

                reservationExecutionManager_.OnRecPrepare(
                    lease.tunerIndex,
                    lease.bon,
                    req);
            };

        cb.OnRecStart = [this](
            int tunerIndex,
            IBonDriver2* /*bon*/,
            const TimeKeeper::TaskRequest& req)
            {
                TunerPortManager::Lease lease{};
                if (!GetReservationTunerLease_(req.instance, lease) &&
                    !AcquireReservationTuner_(tunerIndex, req, lease)) {
                    DebugLogFormat(
                        L"[MainUI][ReservationTuner][START] lease unavailable tuner=%d inst=%u",
                        tunerIndex,
                        static_cast<unsigned>(req.instance));
                    return;
                }

                wchar_t buf[256]{};
                swprintf_s(
                    buf,
                    L"[MainUI] OnRecStart -> ReservationExecution tuner=%d kind=%u inst=%u bon=%p\n",
                    lease.tunerIndex,
                    static_cast<unsigned>(req.kind),
                    static_cast<unsigned>(req.instance),
                    lease.bon);
                OutputDebugStringW(buf);

                reservationExecutionManager_.OnRecStart(
                    lease.tunerIndex,
                    lease.bon,
                    req);
            };

        cb.OnRecStop = [this](
            int tunerIndex,
            IBonDriver2* /*bon*/,
            const TimeKeeper::TaskRequest& req)
            {
                TunerPortManager::Lease lease{};
                const bool hasLease =
                    GetReservationTunerLease_(req.instance, lease);

                const int actualTunerIndex =
                    hasLease ? lease.tunerIndex : tunerIndex;
                IBonDriver2* actualBon =
                    hasLease ? lease.bon : nullptr;

                wchar_t buf[256]{};
                swprintf_s(
                    buf,
                    L"[MainUI] OnRecStop -> ReservationExecution tuner=%d kind=%u inst=%u bon=%p\n",
                    actualTunerIndex,
                    static_cast<unsigned>(req.kind),
                    static_cast<unsigned>(req.instance),
                    actualBon);
                OutputDebugStringW(buf);

                reservationExecutionManager_.OnRecStop(
                    actualTunerIndex,
                    actualBon,
                    req);

                // OnRecStop内でRawTsRecorderのworker joinとEIT flushが
                // 完了した後にBonDriverを返却する。
                ReleaseReservationTuner_(req.instance);
            };

        cb.OnReservationDataChanged = [hwnd = hWnd]() {
            if (IsValidWnd(hwnd)) {
                PostMessageW(hwnd, WM_APP_RESERVATION_DATA_CHANGED, 0, 0);
            }
            };

        if (!taskSystem_.Initialize(hWnd, TIMER_ID_TIMEKEEPER, opt, cb)) {
            AppendLog(L"[Task] TaskSystemConnector の初期化に失敗しました。");
            SetMainStatusText(L"TaskSystem 初期化失敗");
        }
        else {
            AppendLog(L"[Task] TaskSystemConnector を初期化しました。");

            

            EnsureReservationTunersLoaded_();

            ReloadAndApplyTasks_();

            if (!taskSystem_.Start()) {
                AppendLog(L"[Task] TaskSystemConnector の開始に失敗しました。");
                SetMainStatusText(L"TaskSystem 開始失敗");
            }
            else {
                AppendLog(L"[Task] TaskSystemConnector を開始しました。");
            }
        }

        
    }
    StartTimeShiftFromConfig_();
}

void MainUI::OnSize(int w, int h)
{
    if (!IsValidWnd(hTab) || !IsValidWnd(hPageHost) || !hLog) {
        return;
    }

    const int barH = 40;
    const int logH = std::clamp(h / 3, 120, 260);
    const int tabH = std::max<int>(0, h - logH - barH);

    MoveWindow(hTab, 0, 0, w, tabH, TRUE);

    const int pad = 10;
    const int btnH = 28;
    const int y = tabH + (barH - btnH) / 2;

    const int exitW = 80;
    const int cancelW = 110;
    const int saveW = 80;

    int x = w - pad - exitW;
    MoveWindow(hBtnExit, x, y, exitW, btnH, TRUE);
    x -= (pad + cancelW);
    MoveWindow(hBtnCancel, x, y, cancelW, btnH, TRUE);
    x -= (pad + saveW);
    MoveWindow(hBtnSave, x, y, saveW, btnH, TRUE);

    if (hStatus) {
        const int statusX = pad;
        const int statusW = std::max<int>(0, x - (pad * 2));
        MoveWindow(hStatus, statusX, y, statusW, btnH, TRUE);
    }

    MoveWindow(hLog, 0, tabH + barH, w, logH, TRUE);

    // hPageHost は hTab の子なので、hTab 基準座標で配置
    RECT rcPageLocal = GetTabPageRect();
    MoveWindow(
        hPageHost,
        rcPageLocal.left,
        rcPageLocal.top,
        rcPageLocal.right - rcPageLocal.left,
        rcPageLocal.bottom - rcPageLocal.top,
        TRUE);

    SetWindowPos(
        hPageHost,
        HWND_TOP,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    RECT rcLocal{};
    GetClientRect(hPageHost, &rcLocal);

    if (!pagesCreated_) {
        const int pageW = rcLocal.right - rcLocal.left;
        const int pageH = rcLocal.bottom - rcLocal.top;
        if (pageW > 0 && pageH > 0) {
            if (CreateRegisteredPages()) {
                MoveRegisteredPages(rcLocal);
                ShowCurrentRegisteredPage();
            }
        }
        return;
    }

    MoveRegisteredPages(rcLocal);
}

void MainUI::ShowCurrentRegisteredPage()
{
    if (!pagesCreated_) {
        return;
    }

    const int maxIndex = static_cast<int>(registeredPages_.size()) - 1;
    currentTab = std::clamp(currentTab, 0, std::max(0, maxIndex));

    for (int i = 0; i < static_cast<int>(registeredPages_.size()); ++i) {
        auto& page = registeredPages_[i];

        if (!page.created) {
            if (page.show) {
                page.show(false);
            }
            continue;
        }

        const bool visible = (i == currentTab);

        if (visible && page.onSelected) {
            page.onSelected();
        }

        if (page.show) {
            page.show(visible);
        }
    }

    if (IsValidWnd(hPageHost)) {
        SetWindowPos(
            hPageHost,
            HWND_TOP,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        InvalidateRect(hPageHost, nullptr, TRUE);
        UpdateWindow(hPageHost);
    }
}

RECT MainUI::GetTabPageRect() const
{
    if (!IsValidWnd(hTab)) {
        RECT z{};
        return z;
    }

    RECT rcTab{};
    GetClientRect(hTab, &rcTab);

    RECT rcPage = rcTab;
    TabCtrl_AdjustRect(hTab, FALSE, &rcPage);

    // 重要:
    // hTab の子として使うので、MapWindowPoints はしない
    return rcPage;
}

void MainUI::OnTabChanged()
{
    if (!IsValidWnd(hTab)) {
        currentTab = 0;
        return;
    }

    const int sel = TabCtrl_GetCurSel(hTab);
    currentTab = (sel < 0) ? 0 : sel;

    ShowCurrentRegisteredPage();
    AppendLog(L"[GUI] Tab changed.");
}

void MainUI::OnMainButton(int id)
{
    switch (id)
    {
    case IDC_MAIN_SAVE:   SaveAllTabsToConfigAndFile(); return;
    case IDC_MAIN_CANCEL: ExitWithoutSave(); return;
    case IDC_MAIN_EXIT:   ExitWithPrompt(); return;
    }
}

void MainUI::SaveAllTabsToConfigAndFile()
{
    auto& cfg = ConfigManager::GetInstance();

    for (auto& page : registeredPages_) {
        if (!page.created) {
            continue;
        }
        if (page.title == L"設定" && page.apply) {
            page.apply();
            break;
        }
    }

    SyncDependentTabsFromConfig();

    for (auto& page : registeredPages_) {
        if (!page.created) {
            continue;
        }
        if (page.title == L"設定") {
            continue;
        }
        if (page.apply) {
            page.apply();
        }
    }

    cfg.SaveConfig();

    reservationExecutionManager_.ConfigureFromConfig(cfg);

    // 使用中のTimeShift Leaseを先に返却してからポート設定を作り直す。
    StopTimeShift_();
    EnsureReservationTunersLoaded_();

    // ConfigManager::SaveConfig() が全体再生成型でも
    // [Network] が消えないよう最後に再反映
    if (tabNetwork) {
        tabNetwork->ApplyToConfig();
    }
    StartTimeShiftFromConfig_();

    AppendLog(L"[GUI] すべての設定を保存しました。");
}

void MainUI::ExitWithoutSave()
{
    saveConfigOnDestroy = false;
    ConfigManager::GetInstance().LoadConfig();
    AppendLog(L"[GUI] 保存せず終了します。");
    DestroyWindow(hWnd);
}

void MainUI::ExitWithPrompt()
{
    if (g_hiddenToTray) {
        TrayRestore(hWnd);
    }

    const int r = MessageBoxW(
        hWnd,
        L"変更を保存して終了しますか？\n\n"
        L"はい：保存して終了\n"
        L"いいえ：保存せず終了\n"
        L"キャンセル：戻る",
        L"終了確認",
        MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON1);

    if (r == IDYES) {
        saveConfigOnDestroy = true;
        SaveAllTabsToConfigAndFile();
        DestroyWindow(hWnd);
        return;
    }
    if (r == IDNO) {
        ExitWithoutSave();
        return;
    }
}

void MainUI::SetScanUiLocked(bool locked)
{
    scanUiLocked = locked;

    // hTab は無効化しない。
// EPG実行中はタブ切替を許可し、EPGタブ内の開始/設定操作だけをロックする。
// チャンネルスキャンなどの強いロック時は WM_NOTIFY 側でタブ切替を戻す。

    if (hBtnSave)   EnableWindow(hBtnSave, locked ? FALSE : TRUE);
    if (hBtnCancel) EnableWindow(hBtnCancel, locked ? FALSE : TRUE);
    if (hBtnExit)   EnableWindow(hBtnExit, locked ? FALSE : TRUE);

    if (!locked) {
        if (IsValidWnd(hTab)) {
            TabCtrl_SetCurSel(hTab, currentTab);
            ShowCurrentRegisteredPage();
        }
    }
}

void MainUI::SetMainStatusText(std::wstring_view text)
{
    if (!hStatus) return;
    std::wstring s(text);
    SetWindowTextW(hStatus, s.c_str());
}

void MainUI::SetEpgTabBusyState_(bool busy)
{
    if (tabEpg) {
        tabEpg->SetBusyState(busy);
    }
}

void MainUI::StartTimeShiftFromConfig_()
{
    auto& cfg = ConfigManager::GetInstance();

    TimeShift_Engine::Settings settings{};

    settings.managerSettings.rootFolder =
        cfg.GetTimeShiftRootFolder();

    settings.managerSettings.maxBytesPerChannel =
        static_cast<std::uint64_t>(cfg.GetTimeShiftMaxBytesGB()) *
        1024ull *
        1024ull *
        1024ull;

    settings.managerSettings.highWatermarkPercent = 80;
    settings.managerSettings.lowWatermarkPercent = 70;

    settings.managerSettings.segmentSeconds = 60;
    settings.managerSettings.segmentMaxBytes =
        256ull * 1024ull * 1024ull;

    settings.managerSettings.writerQueueBytes =
        64ull * 1024ull * 1024ull;

    settings.managerSettings.dropOldestWhenFull = true;

    settings.waitForSignalLock = true;
    settings.signalLockTimeoutMs = 5000;
    settings.settleSleepMs = 300;
    settings.discardInitialBlocks = 3;
    settings.idleSleepMs = 5;
    settings.noDataTimeoutMs = 30000;
    settings.statusLogIntervalMs = 30000;

    timeShiftEngine_.SetTunerPortManager(&tunerPortManager_);
    timeShiftEngine_.Configure(settings);

    AppendLog(
        std::wstring(L"[TimeShift] 起動チェックを開始します。root=") +
        cfg.GetTimeShiftRootFolder() +
        L" maxGB=" +
        std::to_wstring(cfg.GetTimeShiftMaxBytesGB()));

    const bool ok = timeShiftEngine_.StartFromConfig(cfg);

    if (!ok) {
        AppendLog(L"[TimeShift] 起動対象の開始に失敗しました。");
        SetMainStatusText(L"TimeShift 開始失敗");
        return;
    }

    const auto statuses = timeShiftEngine_.GetStatuses();

    if (statuses.empty()) {
        AppendLog(L"[TimeShift] 起動対象はありません。用途=タイムシフト専用のチューナーが未設定です。");
        return;
    }

    for (const auto& st : statuses) {
        std::wstring msg = L"[TimeShift] status tuner=";
        msg += std::to_wstring(st.tunerIndex);
        msg += L" running=";
        msg += st.workerRunning ? L"1" : L"0";
        msg += L" failed=";
        msg += st.failed ? L"1" : L"0";
        msg += L" space=";
        msg += std::to_wstring(st.space);
        msg += L" ch=";
        msg += std::to_wstring(st.channel);

        if (!st.serviceName.empty()) {
            msg += L" name=";
            msg += st.serviceName;
        }

        if (!st.reason.empty()) {
            msg += L" reason=";
            msg += st.reason;
        }

        AppendLog(msg);
    }

        SetMainStatusText(L"TimeShift 起動チェック完了");

        if (tabTimeShiftViewer != nullptr) {
            AppendLog(L"[TimeShift] Viewer側の番組一覧キャッシュ更新を要求します。");
            DebugLog(L"[TimeShift] Viewer側の番組一覧キャッシュ更新を要求します。");

            tabTimeShiftViewer->ReloadFromConfig();

            DebugLog(L"[TimeShift] Viewer ReloadFromConfig returned");
        }
        else {
            AppendLog(L"[TimeShift] Viewerタブが未作成のため、番組一覧キャッシュ更新を保留しました。");
            DebugLog(L"[TimeShift] Viewerタブが未作成のため、番組一覧キャッシュ更新を保留しました。");
        }

}

void MainUI::StopTimeShift_() noexcept
{
    OutputDebugStringW(L"[TimeShift] Stop begin\n");

    try {
        timeShiftEngine_.StopAll();
    }
    catch (...) {
    }

    OutputDebugStringW(L"[TimeShift] Stop end\n");
}

void MainUI::InitGuiActions_()
{
    actions.StartEpgFullScan = [this]() {
        StartEpgTask_(false, true);
        };

    actions.StartRealtimeEpgUpdate = [this]() {
        StartEpgTask_(true);
        };

    actions.StopTask = [this]() {
        StopActiveTask_();
        };

    actions.ReloadAndApplyTasks = [this]() {
        ReloadAndApplyTasks_();
        };

    actions.CheckManualEpgStart =
        [this](int estimatedMinutes, int guardMinutes) -> GuiActions::ManualEpgStartCheckResult
        {
            GuiActions::ManualEpgStartCheckResult out{};

            if (!taskSystem_.IsInitialized()) {
                // 初期化前は旧動作維持。
                // 通常は OnCreate 完了後に押されるため、ここにはほぼ来ない。
                out.canStart = true;
                out.reason = L"TaskSystem 未初期化のため手動EPG開始チェックをスキップしました。";

                AppendLog(L"[EPG] 手動EPG開始チェックをスキップしました。TaskSystem 未初期化。");
                return out;
            }

            const auto r =
                taskSystem_.CanStartManualEpgAcquisition(
                    estimatedMinutes,
                    guardMinutes);

            out.canStart = r.canStart;
            out.reason = r.reason;
            out.conflictName = r.conflictName;
            out.conflictKind = r.conflictKind;
            out.conflictInstance = r.conflictInstance;
            out.conflictYmd = r.conflictYmd;
            out.conflictHHMM = r.conflictHHMM;

            if (!out.canStart) {
                SetMainStatusText(L"番組予約が近いため手動EPG取得を開始しません。");

                std::wstring msg = L"[EPG] 手動EPG開始チェックで停止しました。";
                if (!out.reason.empty()) {
                    msg += L" reason=";
                    msg += out.reason;
                }
                if (!out.conflictName.empty()) {
                    msg += L" conflict=";
                    msg += out.conflictName;
                }

                AppendLog(msg);
            }

            return out;
        };

    actions.BuildEpg = [this]() {
        AppendLog(L"[EPG] このボタンは今後『EPG呼び出し』へ差し替え予定です。");
        SetMainStatusText(L"EPG呼び出しボタンへ変更予定です。");
        };
}

void MainUI::StartEpgTask_(
    bool realtime,
    bool runReservationMaintenanceAfterBuild)
{
    if (g_scanRunning.load(std::memory_order_acquire)) {
        AppendLog(L"[EPG] チャンネルスキャン実行中のため開始できません。");
        SetMainStatusText(L"チャンネルスキャン実行中です。");
        return;
    }

    if (g_epgRunning.load(std::memory_order_acquire)) {
        AppendLog(L"[EPG] すでに EPG タスクが実行中です。");
        SetMainStatusText(L"EPG タスク実行中です。");
        return;
    }

    if (g_epgThread.joinable()) {
        g_epgThread.join();
    }
    if (g_epgRunner) {
        delete g_epgRunner;
        g_epgRunner = nullptr;
    }

    auto& cfg = ConfigManager::GetInstance();
    cfg.LoadConfig();

    MainUiDebugLogBonDriverSpaceMap_(
        realtime ? L"EPG-Realtime" : L"EPG-Full");

    const int epgSpace =
        LoadIniInt(
            cfg.GetIniPath(),
            L"ChannelScan",
            L"ChannelScanFixedDefaultSpace",
            0);

    std::wstring bonPath =
        cfg.GetBonDriverPathForSpace(epgSpace);

    MainUiDebugLogBonDriverConfigSnapshot_(
        epgSpace,
        realtime ? L"EPG-Realtime" : L"EPG-Full",
        L"GetBonDriverPathForSpace",
        bonPath);

    if (bonPath.empty()) {
        AppendLog(L"[EPG] BonDriver のパスが未設定です。");
        SetMainStatusText(L"BonDriver 未設定");

        DebugLogFormat(
            L"[MainUI][BonDriver][ERROR] EPG BonDriver path empty. space=%d %ls",
            epgSpace,
            MainUiSpaceLabel_(epgSpace));

        return;
    }

    TunerPortManager::AcquireRequest epgAcquire{};
    epgAcquire.purpose =
        TunerPortManager::Purpose::EpgCapture;
    epgAcquire.preferredTunerIndex = -1;
    epgAcquire.ownerInstance = 0;
    epgAcquire.allowFallback = true;

    TunerPortManager::Lease epgLease{};
    if (!tunerPortManager_.Acquire(epgAcquire, epgLease)) {
        AppendLog(L"[EPG] 利用可能なチューナーポートがありません。");
        SetMainStatusText(L"EPG チューナー取得失敗");

        DebugLogFormat(
            L"[MainUI][TunerPort][EPG-ACQUIRE-FAILED] space=%d %ls configuredPath=%ls",
            epgSpace,
            MainUiSpaceLabel_(epgSpace),
            bonPath.c_str());
        return;
    }

    IBonDriver2* bon = epgLease.bon;

    auto* runner = new EpgBuildGUI::EpgBuild_GUI();
    g_epgRunner = runner;

    EpgBuildGUI::Options opt{};
    opt.space = epgSpace;
    opt.bonDriverPathForLog = bonPath;
    opt.configIniPath = cfg.GetIniPath();
    opt.useEnabledServicesFromConfig = true;
    opt.outputDir = L"epg_out";
    opt.basePrefix = realtime ? L"EpgRealtime" : L"EpgFull";
    opt.htmlTitle = realtime ? L"Realtime EPG" : L"EPG";
    opt.enableEitDump = true;
    opt.enableUpdateMerge = true;
    opt.enableAribTrace = cfg.GetAribEnableTrace() || cfg.GetAribTraceG() || cfg.GetAribTraceChars();

    // 手動フルスキャンとリアルタイム更新で負荷を分ける
    opt.captureSecondsPerCycle = realtime ? 30 : 120;
    opt.maxCycles = realtime ? 1 : 2;

    {
        const int ringSize = LoadIniInt(
            cfg.GetIniPath(),
            L"ChannelScan",
            L"ChannelScanFixedRingBufferSizeBytes",
            32 * 1024 * 1024);
        if (ringSize > 0) {
            opt.ringBufferSize = static_cast<std::size_t>(ringSize);
        }
    }

    EpgBuildGUI::Callbacks cb{};
    cb.onLog = [hwnd = hWnd](std::wstring_view s) {
        if (!IsValidWnd(hwnd)) return;
        auto* p = new std::wstring(s);
        if (!PostMessageW(hwnd, WM_APP_EPG_LOG, 0, reinterpret_cast<LPARAM>(p))) {
            delete p;
        }
        };
    cb.onOverallProgress = [hwnd = hWnd](std::size_t done, std::size_t total) {
        if (!IsValidWnd(hwnd)) return;
        auto* p = new EpgOverallMsg{};
        p->done = done;
        p->total = total;
        if (!PostMessageW(hwnd, WM_APP_EPG_OVERALL, 0, reinterpret_cast<LPARAM>(p))) {
            delete p;
        }
        };
    cb.onChannelStart = [hwnd = hWnd](int space, int ch, std::wstring_view name) {
        if (!IsValidWnd(hwnd)) return;
        auto* p = new EpgChStartMsg{};
        p->space = space;
        p->ch = ch;
        p->name.assign(name.begin(), name.end());
        if (!PostMessageW(hwnd, WM_APP_EPG_CHSTART, 0, reinterpret_cast<LPARAM>(p))) {
            delete p;
        }
        };
    cb.onCycleProgress = [hwnd = hWnd](int space, int ch, int cycle,
        std::size_t rawEvents, std::size_t storeSize, std::size_t processedPackets) {
            if (!IsValidWnd(hwnd)) return;
            auto* p = new EpgCycleMsg{};
            p->space = space;
            p->ch = ch;
            p->cycle = cycle;
            p->rawEvents = rawEvents;
            p->storeSize = storeSize;
            p->processedPackets = processedPackets;
            if (!PostMessageW(hwnd, WM_APP_EPG_CYCLE, 0, reinterpret_cast<LPARAM>(p))) {
                delete p;
            }
        };

    cb.onAutoReserveRegisterRequest =
        [this, hwnd = hWnd](
            const EpgHtmlPortal::WriteOptions& portalOpt,
            const std::wstring& reason)
        {
            try {
                const std::wstring reasonText =
                    reason.empty()
                    ? std::wstring(L"epg-build-final")
                    : reason;

                if (!taskSystem_.IsInitialized()) {
                    DebugLogFormat(
                        L"[MainUI][AUTO-RESERVE][REGISTER-SKIP] TaskSystem not initialized reason=%ls",
                        reasonText.c_str());
                    return;
                }

                AutoReserve::CurrentSearchRegisterOptions opt =
                    AutoReserve::MakeCurrentSearchRegisterOptions(
                        AutoReserve::ApplyTrigger::EpgUpdate,
                        0);

                opt.reason = reasonText;

                // 録画専用の自動予約として固定。
                // MakeCurrentSearchRegisterOptions() の既定値でもほぼ同じだが、
                // ここで明示しておく。
                opt.dryRun = false;
                opt.allowRegister = true;
                // EPG再構築後の自動録画登録では auto ルールだけ対象にする。
// Portal版の candidate ルールは「候補」として正しい書式なので、
// Lite側で勝手に録画予約へ昇格させない。
                opt.includeCandidateRules = false;
                opt.includeAutoRules = true;
                opt.skipDuplicate = true;
                opt.protectUserBlockedReservation = true;
                opt.maxRegisterCount = 0;

                opt.reserveMode = L"record";
                opt.reserveSource = L"auto_reserve_epg_update";

                opt.forceCreatedReservationEnabled = true;
                opt.requiredTuners = 1;
                opt.preferredTunerIndex = -1;
                opt.refreshReservationDataJs = true;

                const AutoReserve::CurrentSearchRegisterResult r =
                    AutoReserve::RunCurrentSearchAndRegisterRecordingTasks(
                        &taskSystem_,
                        portalOpt,
                        opt);

                DebugLogFormat(
                    L"[MainUI][AUTO-RESERVE][EPG-BUILD-FINAL][REGISTER] reason=%ls ok=%d searchAttempted=%d searchOk=%d matched=%zu candidate=%zu auto=%zu target=%zu registered=%zu dryRun=%zu dup=%zu userBlocked=%zu invalid=%zu limit=%zu action=%zu failed=%zu reservationJs=%d message=%ls",
                    reasonText.c_str(),
                    r.ok ? 1 : 0,
                    r.searchAttempted ? 1 : 0,
                    r.searchOk ? 1 : 0,
                    r.matchedCount,
                    r.candidateCount,
                    r.autoRegisterCount,
                    r.registerTarget,
                    r.registered,
                    r.dryRunSkipped,
                    r.duplicateSkipped,
                    r.userBlockedSkipped,
                    r.invalidSkipped,
                    r.limitSkipped,
                    r.actionSkipped,
                    r.failed,
                    r.reservationJsOk ? 1 : 0,
                    r.message.c_str());

                if ((r.registered != 0 || r.reservationJsOk) &&
                    ::IsWindow(hwnd)) {
                    PostMessageW(
                        hwnd,
                        WM_APP_RESERVATION_DATA_CHANGED,
                        0,
                        0);
                }
            }
            catch (const std::bad_alloc&) {
                DebugLog(L"[MainUI][AUTO-RESERVE][EPG-BUILD-FINAL][ERROR] bad_alloc");
            }
            catch (const std::exception& e) {
                DebugLogFormat(
                    L"[MainUI][AUTO-RESERVE][EPG-BUILD-FINAL][ERROR] exception what=%S",
                    e.what());
            }
            catch (...) {
                DebugLog(L"[MainUI][AUTO-RESERVE][EPG-BUILD-FINAL][ERROR] unknown");
            }
        };

    cb.onAutoReserveMatchResult =
        [this, hwnd = hWnd](
            const AutoReserve::RuleMatchResult& matchResult,
            const EpgHtmlPortal::WriteOptions&,
            const std::wstring& reason)
        {
            // onAutoReserveRegisterRequest が未接続/失敗した場合だけ使われる保険ルート。
            // 通常は RunCurrentSearchAndRegisterRecordingTasks() 側が使われる。
            if (!taskSystem_.IsInitialized()) {
                DebugLogFormat(
                    L"[MainUI][AUTO-RESERVE][FALLBACK-SKIP] TaskSystem not initialized reason=%ls",
                    reason.c_str());
                return;
            }

            const auto r =
                taskSystem_.RegisterAutoReserveMatchesAsRecordingFixed(
                    matchResult);

            DebugLogFormat(
                L"[MainUI][AUTO-RESERVE][FALLBACK-REGISTER] reason=%ls ok=%d input=%zu target=%zu registered=%zu dup=%zu userBlocked=%zu invalid=%zu limit=%zu action=%zu failed=%zu message=%ls",
                reason.c_str(),
                r.ok ? 1 : 0,
                r.inputMatches,
                r.targetPrograms,
                r.registered,
                r.duplicateSkipped,
                r.userBlockedSkipped,
                r.invalidSkipped,
                r.limitSkipped,
                r.actionSkipped,
                r.failed,
                r.message.c_str());

            if (r.registered != 0 && ::IsWindow(hwnd)) {
                PostMessageW(
                    hwnd,
                    WM_APP_RESERVATION_DATA_CHANGED,
                    0,
                    0);
            }
        };

    g_lastEpgWasFull.store(!realtime, std::memory_order_release);
    g_lastEpgWasAuto.store(false, std::memory_order_release);
    g_epgRunning.store(true, std::memory_order_release);

    SetEpgTabBusyState_(true);
    SetScanUiLocked(true);
    SetMainStatusText(realtime ? L"リアルタイム更新を開始します…" : L"EPGフルスキャンを開始します…");
    AppendLog(realtime ? L"[EPG] リアルタイム更新を開始します。" : L"[EPG] EPGフルスキャンを開始します。");

    const auto mode = realtime
        ? FilterManagerMode::RealtimeEpgUpdate
        : FilterManagerMode::EpgFullScan;

    try {
        g_epgThread = std::thread([this, bon, mode, opt, cb, runner,
            runReservationMaintenanceAfterBuild, epgLease]() mutable {
        EpgBuildGUI::Result result{};
        try {
            result = runner->Run(bon, mode, opt, &cb);
        }
        catch (...) {
            result.status = EpgBuildGUI::Status::InvalidArgument;
            if (IsValidWnd(hWnd)) {
                auto* p = new std::wstring(L"[EPG] 例外が発生しました。");
                if (!PostMessageW(hWnd, WM_APP_EPG_LOG, 0, reinterpret_cast<LPARAM>(p))) {
                    delete p;
                }
            }
        }

        // Run()が受信workerをすべて停止した後、HTML完了通知より先に
        // EPG用ポートを完全解放する。
        (void)tunerPortManager_.Release(epgLease);

        g_epgRunning.store(false, std::memory_order_release);

        if (IsValidWnd(hWnd)) {
            auto* done = new EpgDoneMsg{};
            done->status = result.status;
            done->targetChannels = result.targetChannels;
            done->processedChannels = result.processedChannels;
            done->mergedEpgPath = result.mergedEpgPath;
            done->htmlPath = result.htmlPath;
            done->runReservationMaintenanceAfterBuild =
                runReservationMaintenanceAfterBuild;

            if (!PostMessageW(hWnd, WM_APP_EPG_DONE, 0, reinterpret_cast<LPARAM>(done))) {
                delete done;
            }
        }
        });
    }
    catch (...) {
        g_epgRunning.store(false, std::memory_order_release);
        (void)tunerPortManager_.Release(epgLease);
        delete runner;
        g_epgRunner = nullptr;
        SetEpgTabBusyState_(false);
        SetScanUiLocked(false);
        AppendLog(L"[EPG] 実行スレッドの開始に失敗しました。");
        SetMainStatusText(L"EPG スレッド開始失敗");
    }
}

void MainUI::StopActiveTask_()
{
    if (g_epgRunning.load(std::memory_order_acquire)) {
        if (g_epgRunner) {
            g_epgRunner->RequestCancel();
        }
        AppendLog(L"[EPG] 停止要求を送信しました。");
        SetMainStatusText(L"EPG 停止要求を送信しました。");
        return;
    }

    if (g_scanRunning.load(std::memory_order_acquire)) {
        g_scanCancel.store(true, std::memory_order_release);
        AppendLog(L"[チャンネルスキャン] 停止要求を送信しました。");
        SetMainStatusText(L"チャンネルスキャン停止要求を送信しました。");
        return;
    }

    AppendLog(L"[GUI] 停止対象のタスクはありません。");
    SetMainStatusText(L"停止対象なし");
}

void MainUI::ReloadAndApplyTasks_()
{
    if (!taskSystem_.IsInitialized()) {
        AppendLog(L"[Task] taskSystem_ が未初期化のため、タスクを反映できません。");
        SetMainStatusText(L"TaskSystem 未初期化");
        return;
    }

    auto& cfg = ConfigManager::GetInstance();
    const std::wstring iniPath = cfg.GetIniPath();
    const std::wstring tasksIni = GetDefaultTasksIniPath();

    TaskListManager tl;
    if (!tl.LoadFromIni(tasksIni)) {
        // tasks.ini が無い場合は既定枠を作る
        tl.EnsureDefaultEpgTasks(0);
        if (!tl.SaveToIni(tasksIni)) {
            AppendLog(L"[Task] tasks.ini の作成に失敗しました。");
            SetMainStatusText(L"tasks.ini 作成失敗");
            return;
        }
    }

    NormalizeEpgFullOneShot(tl, tasksIni);

    // 設定値の反映
    taskSystem_.SetEnableAuto(
        cfg.GetEpgAutoMode() == ConfigManager::EpgAutoMode::Task);

    taskSystem_.SetBootGraceSeconds(cfg.GetEpgBootGraceSec());
    taskSystem_.SetTickSeconds(cfg.GetEpgTickSeconds());
    taskSystem_.SetIdleRequiredSeconds(cfg.GetEpgIdleRequiredSec());

    const bool wasRunning = taskSystem_.IsRunning();
    if (wasRunning) {
        taskSystem_.Stop(false);
    }

    EnsureReservationTunersLoaded_();

    if (!taskSystem_.LoadTasksFromIni(tasksIni)) {
        AppendLog(L"[Task] tasks.ini の再読込に失敗しました。");
        SetMainStatusText(L"tasks.ini 再読込失敗");
        return;
    }

    if (!taskSystem_.ApplyTaskList()) {
        AppendLog(L"[Task] TaskSystemConnector への反映に失敗しました。");
        SetMainStatusText(L"タスク反映失敗");
        return;
    }

    if (wasRunning) {
        if (!taskSystem_.Start()) {
            AppendLog(L"[Task] タスク監視の再開に失敗しました。");
            SetMainStatusText(L"タスク監視再開失敗");
            return;
        }
    }

    AppendLog(L"[Task] tasks.ini を再読込し、TimeKeeper / TaskSystemConnector へ反映しました。");
    SetMainStatusText(L"タスク反映完了");
}

bool MainUI::EnsureReservationTunersLoaded_()
{
    if (!taskSystem_.IsInitialized()) {
        AppendLog(L"[TunerPortManager] TaskSystem 未初期化のため設定を適用できません。");
        return false;
    }

    auto& cfg = ConfigManager::GetInstance();
    cfg.LoadConfig();

    const int tunerCount =
        (std::max)(1, cfg.GetTunersCount());

    // 設定変更中に予約Leaseが残っている場合は、使用中ポートを
    // 作り直さない。通常はタスク停止・録画停止後に再適用する。
    if (tunerPortManager_.HasActiveLease()) {
        AppendLog(L"[TunerPortManager] 使用中ポートがあるため再設定を保留しました。");
        return true;
    }

    std::vector<TunerPortManager::PortConfig> ports;
    ports.reserve(static_cast<std::size_t>(tunerCount));

    for (int tunerIndex = 0; tunerIndex < tunerCount; ++tunerIndex) {
        TunerPortManager::PortConfig port{};
        port.tunerIndex = tunerIndex;
        port.enabled = cfg.GetTunerEnabled(tunerIndex);
        port.allowParallel = true;
        port.priority = tunerCount - tunerIndex;
        port.name = L"Tuner " + std::to_wstring(tunerIndex);
        port.bonDriverPath =
            cfg.GetBonDriverPathForTunerOrDefault(tunerIndex);

        if (cfg.GetTunerUseRole(tunerIndex) ==
            ConfigManager::TunerUseRole::TimeShiftOnly) {
            port.allowedPurposes =
                TunerPortManager::ToMask(
                    TunerPortManager::Purpose::TimeShift);
        }
        else {
            port.allowedPurposes =
                TunerPortManager::ToMask(
                    TunerPortManager::Purpose::ChannelScan) |
                TunerPortManager::ToMask(
                    TunerPortManager::Purpose::EpgCapture) |
                TunerPortManager::ToMask(
                    TunerPortManager::Purpose::ReservationRecord);
        }

        // Runnerはタスク番号の配送だけを行う。BonDriverはPrepare時に
        // TunerPortManagerから取得するため常駐登録しない。
        taskSystem_.SetTuner(tunerIndex, nullptr);
        taskSystem_.SetTunerAvailable(
            tunerIndex,
            port.enabled &&
            (port.allowedPurposes &
                TunerPortManager::ToMask(
                    TunerPortManager::Purpose::ReservationRecord)) != 0);

        ports.push_back(std::move(port));
    }

    if (!tunerPortManager_.Configure(ports)) {
        AppendLog(L"[TunerPortManager] ポート設定に失敗しました。");
        return false;
    }

    AppendLog(L"[TunerPortManager] 設定を適用しました。BonDriverは未ロードです。");
    return true;
}

void MainUI::ResetReservationTuners_() noexcept
{
    std::vector<TunerPortManager::Lease> leases;

    {
        std::lock_guard<std::mutex> lock(reservationLeaseMutex_);
        leases.reserve(reservationTunerLeases_.size());
        for (auto& item : reservationTunerLeases_) {
            leases.push_back(item.second);
        }
        reservationTunerLeases_.clear();
    }

    for (auto& lease : leases) {
        (void)tunerPortManager_.Release(lease);
    }

    tunerPortManager_.StopAll();

    DebugLog(L"[TunerPortManager] reset reservation leases");
}

bool MainUI::AcquireReservationTuner_(
    int requestedTunerIndex,
    const TimeKeeper::TaskRequest& req,
    TunerPortManager::Lease& outLease) noexcept
{
    outLease.Reset();

    {
        std::lock_guard<std::mutex> lock(reservationLeaseMutex_);
        const auto it = reservationTunerLeases_.find(req.instance);
        if (it != reservationTunerLeases_.end() && it->second.IsValid()) {
            outLease = it->second;
            return true;
        }
    }

    TunerPortManager::AcquireRequest acquire{};
    acquire.purpose =
        TunerPortManager::Purpose::ReservationRecord;
    acquire.preferredTunerIndex = requestedTunerIndex;
    acquire.ownerInstance = req.instance;
    acquire.allowFallback = true;

    TunerPortManager::Lease newLease{};
    if (!tunerPortManager_.Acquire(acquire, newLease)) {
        return false;
    }

    TunerPortManager::Lease duplicate{};
    {
        std::lock_guard<std::mutex> lock(reservationLeaseMutex_);
        const auto [it, inserted] =
            reservationTunerLeases_.emplace(req.instance, newLease);

        if (!inserted) {
            // 同一instanceのPrepareが競合した場合は、先に登録された
            // Leaseを採用し、後から取得したポートを返却する。
            duplicate = newLease;
            outLease = it->second;
        }
        else {
            outLease = it->second;
        }
    }

    if (duplicate.IsValid()) {
        (void)tunerPortManager_.Release(duplicate);
    }

    return outLease.IsValid();
}

bool MainUI::GetReservationTunerLease_(
    std::uint32_t instance,
    TunerPortManager::Lease& outLease) noexcept
{
    outLease.Reset();

    std::lock_guard<std::mutex> lock(reservationLeaseMutex_);
    const auto it = reservationTunerLeases_.find(instance);
    if (it == reservationTunerLeases_.end() || !it->second.IsValid()) {
        return false;
    }

    outLease = it->second;
    return true;
}

void MainUI::ReleaseReservationTuner_(
    std::uint32_t instance) noexcept
{
    TunerPortManager::Lease lease{};

    {
        std::lock_guard<std::mutex> lock(reservationLeaseMutex_);
        const auto it = reservationTunerLeases_.find(instance);
        if (it == reservationTunerLeases_.end()) {
            return;
        }

        lease = it->second;
        reservationTunerLeases_.erase(it);
    }

    (void)tunerPortManager_.Release(lease);
}
