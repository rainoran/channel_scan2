#include "ChannelScanTab.hpp"

#include <CommCtrl.h>
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <thread>
#include <atomic>

#pragma comment(lib, "Comctl32.lib")

#include "ConfigManager.h"
#include "bondriver_load.h"
#include "channel_scan_GUI.hpp"

namespace {

    static constexpr int IDC_CB_SPACE = 2001;
    static constexpr int IDC_BT_START = 2002;
    static constexpr int IDC_BT_STOP = 2003;
    static constexpr int IDC_ST_STATUS = 2004;
    static constexpr int IDC_GB_SERV = 2005;
    static constexpr int IDC_LV_SERV = 2006;

    static void SetFont(HWND h, HFONT f)
    {
        if (!h || !f) return;
        SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
    }

    static HMENU CtrlId(int id) noexcept
    {
        return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
    }

    static std::wstring GetExeDir()
    {
        wchar_t path[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(nullptr, path, _countof(path));
        if (n == 0 || n >= _countof(path)) return L".";
        std::filesystem::path p(path);
        return p.parent_path().wstring();
    }
    static std::wstring GetBaseDir()
    {
        try { return std::filesystem::current_path().wstring(); }
        catch (...) { return GetExeDir(); }
    }


    static std::wstring ReadTextFileAutoEncoding(const std::filesystem::path& path)
    {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) return {};

        std::string bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        if (bytes.empty()) return {};

        // UTF-8 BOM
        if (bytes.size() >= 3 &&
            (unsigned char)bytes[0] == 0xEF &&
            (unsigned char)bytes[1] == 0xBB &&
            (unsigned char)bytes[2] == 0xBF) {
            bytes.erase(0, 3);
        }

        auto toWide = [&](UINT cp, DWORD flags) -> std::wstring {
            if (bytes.empty()) return {};
            int need = MultiByteToWideChar(cp, flags, bytes.data(), (int)bytes.size(), nullptr, 0);
            if (need <= 0) return {};
            std::wstring out;
            out.resize((std::size_t)need);
            MultiByteToWideChar(cp, flags, bytes.data(), (int)bytes.size(), out.data(), need);
            return out;
            };

        // まず UTF-8 を試し、失敗したら CP_ACP
        std::wstring w = toWide(CP_UTF8, MB_ERR_INVALID_CHARS);
        if (!w.empty()) return w;
        return toWide(CP_ACP, 0);
    }

    static std::vector<std::wstring> ParseCsvLine(const std::wstring& line)
    {
        // 最低限のCSV（\"...\" 対応、\"\" -> \"）
        std::vector<std::wstring> out;
        std::wstring cur;
        cur.reserve(64);

        bool inQuote = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            const wchar_t ch = line[i];
            if (inQuote) {
                if (ch == L'\"') {
                    if (i + 1 < line.size() && line[i + 1] == L'\"') {
                        cur.push_back(L'\"');
                        ++i;
                    }
                    else {
                        inQuote = false;
                    }
                }
                else {
                    cur.push_back(ch);
                }
            }
            else {
                if (ch == L'\"') {
                    inQuote = true;
                }
                else if (ch == L',') {
                    out.emplace_back(std::move(cur));
                    cur.clear();
                }
                else if (ch == L'\r' || ch == L'\n') {
                    // skip
                }
                else {
                    cur.push_back(ch);
                }
            }
        }
        out.emplace_back(std::move(cur));
        return out;
    }

    static int ParseIntOr(const std::wstring& s, int fallback)
    {
        try {
            if (s.empty()) return fallback;
            size_t idx = 0;
            int v = std::stoi(s, &idx, 10);
            (void)idx;
            return v;
        }
        catch (...) {
            return fallback;
        }
    }

    static bool TryParseU16(const std::wstring& s, std::uint16_t& out)
    {
        try {
            if (s.empty()) return false;
            size_t idx = 0;
            unsigned long v = std::stoul(s, &idx, 10);
            if (v > 0xFFFFu) return false;
            out = static_cast<std::uint16_t>(v);
            return true;
        }
        catch (...) {
            return false;
        }
    }

    static std::wstring QuoteCsvField(std::wstring_view s)
    {
        bool needQuote = false;
        for (wchar_t ch : s) {
            if (ch == L',' || ch == L'\"' || ch == L'\n' || ch == L'\r') { needQuote = true; break; }
        }
        if (!needQuote) return std::wstring(s);

        std::wstring out;
        out.reserve(s.size() + 2);
        out.push_back(L'\"');
        for (wchar_t ch : s) {
            if (ch == L'\"') out.push_back(L'\"'); // escape
            out.push_back(ch);
        }
        out.push_back(L'\"');
        return out;
    }

    // services list columns
    enum ServiceCols : int {
        COL_ServiceName = 0, // checkbox is here
        COL_SID,
        COL_Type,
        COL_Space,
        COL_CH,
        COL_SignalDb,
        COL_PAT,
        COL_PMT,
        COL_SDT,
        COL_NIT,
        COL_Network,
        COL_ONID,
        COL_TSID,
        COL_RCK,
        COL_CA,
        COL__COUNT
    };

    static void InitServiceListColumns(HWND lv)
    {
        ListView_SetExtendedListViewStyle(lv,
            LVS_EX_FULLROWSELECT |
            LVS_EX_GRIDLINES |
            LVS_EX_CHECKBOXES |
            LVS_EX_DOUBLEBUFFER);

        struct Col { int w; const wchar_t* title; };
        const Col cols[] = {
            { 240, L"サービス名" },
            { 70,  L"SID" },
            { 55,  L"Type" },
            { 55,  L"Space" },
            { 55,  L"CH" },
            { 80,  L"信号(dB)" },
            { 55,  L"PAT" },
            { 55,  L"PMT" },
            { 55,  L"SDT" },
            { 55,  L"NIT" },
            { 180, L"Network" },
            { 70,  L"ONID" },
            { 70,  L"TSID" },
            { 60,  L"RC" },
            { 45,  L"CA" },
        };

        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        for (int i = 0; i < (int)(sizeof(cols) / sizeof(cols[0])); ++i) {
            c.iSubItem = i;
            c.cx = cols[i].w;
            c.pszText = const_cast<LPWSTR>(cols[i].title);
            ListView_InsertColumn(lv, i, &c);
        }
    }

    static std::wstring ResolveBonDriverPathForSpace_(int space)
    {
        auto& cfg = ConfigManager::GetInstance();

        std::wstring path =
            cfg.GetChannelScanBonDriverPathForSpace(space);

        if (path.empty()) {
            return L"";
        }

        try {
            std::filesystem::path p(path);

            if (p.is_absolute()) {
                return p.lexically_normal().wstring();
            }

            // config.ini / exe と同じ基準で相対パスを解決する。
            // ConfigManager 側で AppBase 解決を持っているので、直接 ini を読まない。
            return cfg.ResolvePathFromAppBase(path);
        }
        catch (...) {
            return path;
        }
    }

    static const wchar_t* GetSpaceBonDriverLabel_(int space) noexcept
    {
        switch (space) {
        case 0:
            return L"地上波";
        case 1:
            return L"BS";
        case 2:
            return L"CS";
        default:
            return L"不明space";
        }
    }

} // namespace

bool ChannelScanTab::Create(HWND hParent, const RECT& rc, HINSTANCE inst, HFONT font,
    std::function<GuiActions()>&& a,
    std::function<void(std::wstring_view)>&& l)
{
    hInst = inst;
    hFont = font;
    getActions = std::move(a);
    log = std::move(l);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ChannelScanTab::WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"TSCEPG_ChannelScanTab";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassExW(&wc)) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) return false;
    }

    hWnd = CreateWindowExW(
        0, wc.lpszClassName, L"",
        WS_CHILD | WS_VISIBLE,
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
        hParent, nullptr, hInst, this);

    return (hWnd != nullptr);
}

void ChannelScanTab::Show(bool show)
{
    if (!hWnd) return;
    ShowWindow(hWnd, show ? SW_SHOW : SW_HIDE);
}

void ChannelScanTab::Move(const RECT& rc)
{
    if (!hWnd) return;
    MoveWindow(hWnd, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
}

LRESULT CALLBACK ChannelScanTab::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    ChannelScanTab* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<ChannelScanTab*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hWnd = h;
    }
    else {
        self = reinterpret_cast<ChannelScanTab*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    return self ? self->HandleMessage(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT ChannelScanTab::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        OnCreate();
        return 0;

    case WM_SIZE:
        OnSize(LOWORD(lp), HIWORD(lp));
        return 0;

        // ---- worker -> UI ----
    case WMU_CLEAR_RESULTS:
        ClearResults();
        return 0;

    case WMU_SET_RUNNING:
        SetRunning(wp ? true : false);
        return 0;

    case WMU_SET_STATUS:
    {
        std::unique_ptr<StatusPayload> p(reinterpret_cast<StatusPayload*>(lp));
        if (p) SetStatusText(p->text);
        return 0;
    }

    case WMU_ADD_ROW:
    {
        std::unique_ptr<RowPayload> p(reinterpret_cast<RowPayload*>(lp));
        if (p) {
            AddReceivableChannelRow(
                p->space, p->ch, p->chName,
                p->signalLevelDb,
                p->pat, p->pmt, p->sdt, p->nit
            );
        }
        return 0;
    }

    case WM_NOTIFY:
    {
        auto* hdr = reinterpret_cast<NMHDR*>(lp);
        if (!hdr) return 0;

        if (hdr->hwndFrom == lvServices && hdr->code == LVN_ITEMCHANGED) {
            if (!suppressServiceNotify) {
                auto* nmlv = reinterpret_cast<NMLISTVIEW*>(lp);
                if (nmlv) {
                    OnServiceListItemChanged(nmlv->iItem, nmlv->uOldState, nmlv->uNewState);
                }
            }
        }
        return 0;
    }

    case WM_COMMAND:
    {
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);

        if (id == IDC_CB_SPACE && code == CBN_SELCHANGE) {
            LoadServiceListForSelectedSpace();
            return 0;
        }

        if (id == IDC_BT_START) {
            if (isRunning) {
                return 0;
            }

            int sel = (int)SendMessageW(cbSpace, CB_GETCURSEL, 0, 0);
            if (sel < 0) sel = 0;

            ClearResults();
            SetStatusText(L"スキャンを開始します…");
            SetRunning(true);

            if (TryStartConnectedScan(sel)) {
                return 0;
            }

            if (TryStartSelfHostedScan(sel)) {
                if (log) log(L"[チャンネルスキャン] StartChannelScan 未接続のため、ChannelScanTab 自己完結モードで起動しました。");
                return 0;
            }

            SetRunning(false);
            SetStatusText(L"スキャン開始に失敗しました。");
            if (log) log(L"[チャンネルスキャン] スキャン開始に失敗しました。");
            return 0;
        }

        if (id == IDC_BT_STOP) {
            // 自己完結モードで動作中なら、まず自前停止を優先
            if (selfHostedScanActive.load(std::memory_order_acquire)) {
                RequestStopSelfHostedScan();
                return 0;
            }

            auto act = getActions ? getActions() : GuiActions{};
            if (act.StopTask) {
                act.StopTask();
                SetStatusText(L"停止要求を送信しました…");
                if (log) log(L"[チャンネルスキャン] StopTask に停止要求を送信しました。");
            }
            else {
                SetStatusText(L"停止経路が未接続です。");
                if (log) log(L"[チャンネルスキャン] 停止経路が未接続です。");
            }
            return 0;
        }

        return 0;
    }

    case WM_DESTROY:
        RequestStopSelfHostedScan();
        JoinSelfHostedScanIfNeeded();
        return 0;

    default:
        return DefWindowProcW(hWnd, msg, wp, lp);
    }
}

void ChannelScanTab::OnCreate()
{
    INITCOMMONCONTROLSEX icc{ sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    CreateWindowExW(0, L"STATIC", L"放送種別（space）：", WS_CHILD | WS_VISIBLE,
        10, 12, 140, 20, hWnd, nullptr, hInst, nullptr);

    cbSpace = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        160, 10, 240, 200, hWnd, CtrlId(IDC_CB_SPACE), hInst, nullptr);

    SendMessageW(cbSpace, CB_ADDSTRING, 0, (LPARAM)L"0: 地上波");
    SendMessageW(cbSpace, CB_ADDSTRING, 0, (LPARAM)L"1: BS");
    SendMessageW(cbSpace, CB_ADDSTRING, 0, (LPARAM)L"2: CS");
    SendMessageW(cbSpace, CB_SETCURSEL, 0, 0);

    btStart = CreateWindowExW(0, L"BUTTON", L"スキャン開始",
        WS_CHILD | WS_VISIBLE,
        10, 45, 140, 28, hWnd, CtrlId(IDC_BT_START), hInst, nullptr);

    btStop = CreateWindowExW(0, L"BUTTON", L"停止",
        WS_CHILD | WS_VISIBLE | WS_DISABLED,
        160, 45, 100, 28, hWnd, CtrlId(IDC_BT_STOP), hInst, nullptr);

    stStatus = CreateWindowExW(0, L"STATIC", L"待機中",
        WS_CHILD | WS_VISIBLE,
        10, 82, 760, 18, hWnd, CtrlId(IDC_ST_STATUS), hInst, nullptr);

    // ---- 1枠: サービス一覧（チェック=EPG対象） ----
    gbServices = CreateWindowExW(0, L"BUTTON", L"サービス一覧（チェック=EPG対象）",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        10, 105, 760, 420, hWnd, CtrlId(IDC_GB_SERV), hInst, nullptr);

    lvServices = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        20, 130, 740, 385, hWnd, CtrlId(IDC_LV_SERV), hInst, nullptr);

    InitServiceListColumns(lvServices);

    for (HWND c : { cbSpace, btStart, btStop, stStatus, gbServices, lvServices }) {
        if (c) SetFont(c, hFont);
    }

    // 起動直後に現在 space のサービス一覧を読み込む
    LoadServiceListForSelectedSpace();
}

void ChannelScanTab::OnSize(int w, int h)
{
    const int margin = 10;
    const int statusY = 82;
    const int topY = 105;

    if (stStatus) MoveWindow(stStatus, margin, statusY, w - margin * 2, 18, TRUE);

    // groupbox を下まで
    const int gbH = (std::max)(80, h - topY - margin);
    if (gbServices) MoveWindow(gbServices, margin, topY, w - margin * 2, gbH, TRUE);

    // listview を groupbox 内に配置
    const int lvX = margin + 10;
    const int lvY = topY + 25;
    const int lvW = (std::max)(60, w - (margin * 2) - 20);
    const int lvH = (std::max)(40, h - lvY - margin);
    if (lvServices) MoveWindow(lvServices, lvX, lvY, lvW, lvH, TRUE);
}

void ChannelScanTab::ClearResults()
{
    // スキャン統計のみクリア（サービス一覧は残す）
    channelStats.clear();

    if (!lvServices) return;

    const int n = ListView_GetItemCount(lvServices);
    for (int i = 0; i < n; ++i) {
        ListView_SetItemText(lvServices, i, COL_SignalDb, const_cast<LPWSTR>(L""));
        ListView_SetItemText(lvServices, i, COL_PAT, const_cast<LPWSTR>(L""));
        ListView_SetItemText(lvServices, i, COL_PMT, const_cast<LPWSTR>(L""));
        ListView_SetItemText(lvServices, i, COL_SDT, const_cast<LPWSTR>(L""));
        ListView_SetItemText(lvServices, i, COL_NIT, const_cast<LPWSTR>(L""));
    }
}

void ChannelScanTab::SetRunning(bool running)
{
    const bool wasRunning = isRunning;
    isRunning = running;

    if (btStart) EnableWindow(btStart, running ? FALSE : TRUE);
    if (btStop)  EnableWindow(btStop, running ? TRUE : FALSE);
    if (cbSpace) EnableWindow(cbSpace, running ? FALSE : TRUE);

    // スキャン終了時に、最新の ch ファイルを読み直して反映
    if (wasRunning && !running) {
        LoadServiceListForSelectedSpace();
    }
}

void ChannelScanTab::SetStatusText(std::wstring_view s)
{
    if (!stStatus) return;
    std::wstring tmp(s);
    SetWindowTextW(stStatus, tmp.c_str());
}

void ChannelScanTab::ApplyToConfig()
{
    if (serviceRows.empty()) {
        LoadServiceListForSelectedSpace();
    }

    // GUIの「保存」では、現在表示中spaceのサービス設定を完全スナップショットとして保存する。
    // これにより「無効化したものだけがconfig.iniに残る」状態を避ける。
    SaveAllServiceRowsEnabledToIni(true);
}

void ChannelScanTab::ReloadFromConfig()
{
    LoadServiceListForSelectedSpace();
}

int ChannelScanTab::GetSelectedSpaceIndex() const
{
    if (!cbSpace) {
        return 0;
    }

    int sel = static_cast<int>(SendMessageW(cbSpace, CB_GETCURSEL, 0, 0));
    if (sel < 0) {
        sel = 0;
    }

    return sel;
}

bool ChannelScanTab::TryStartConnectedScan(int space)
{
    auto act = getActions ? getActions() : GuiActions{};
    if (!act.StartChannelScan) {
        return false;
    }

    act.StartChannelScan(space);
    if (log) log(L"[チャンネルスキャン] 接続済み StartChannelScan を呼び出しました。");
    return true;
}

bool ChannelScanTab::TryStartSelfHostedScan(int space)
{
    if (selfHostedScanActive.load(std::memory_order_acquire)) {
        return false;
    }

    JoinSelfHostedScanIfNeeded();

    selfHostedStopRequested.store(false, std::memory_order_release);
    selfHostedScanActive.store(true, std::memory_order_release);

    selfHostedScanThread = std::thread([this, space]() {
        BonDriverLoader localLoader;
        bool runningFalsePosted = false;

        auto finish = [&]() noexcept {
            localLoader.Unload();
            selfHostedStopRequested.store(false, std::memory_order_release);
            selfHostedScanActive.store(false, std::memory_order_release);

            if (!runningFalsePosted) {
                PostSetRunning(false);
            }
            };

        try {
            auto& cfg = ConfigManager::GetInstance();
            cfg.LoadConfig();

            const std::wstring bonPath =
                ResolveBonDriverPathForSpace_(space);

            if (bonPath.empty()) {
                std::wstring status = L"BonDriver のパスが未設定です。space=";
                status += std::to_wstring(space);
                status += L" ";
                status += GetSpaceBonDriverLabel_(space);

                PostSetStatusText(status);

                if (log) {
                    std::wstring msg = L"[チャンネルスキャン] ";
                    msg += status;
                    msg += L"。";
                    log(msg);
                }

                finish();
                return;
            }

            if (log) {
                std::wstring msg = L"[チャンネルスキャン] 自己完結モード開始: space=";
                msg += std::to_wstring(space);
                msg += L" ";
                msg += GetSpaceBonDriverLabel_(space);
                msg += L" BonDriver=";
                msg += bonPath;
                log(msg);
            }

            if (!localLoader.LoadBonDriver(bonPath)) {
                std::wstring status = L"BonDriver のロードに失敗しました。space=";
                status += std::to_wstring(space);
                status += L" ";
                status += GetSpaceBonDriverLabel_(space);

                PostSetStatusText(status);

                if (log) {
                    std::wstring msg = L"[チャンネルスキャン] ";
                    msg += status;
                    msg += L" BonDriver=";
                    msg += bonPath;
                    log(msg);
                }

                finish();
                return;
            }

            IBonDriver2* bon = localLoader.GetBonDriver();
            if (!bon) {
                std::wstring status = L"IBonDriver2 の取得に失敗しました。space=";
                status += std::to_wstring(space);
                status += L" ";
                status += GetSpaceBonDriverLabel_(space);

                PostSetStatusText(status);

                if (log) {
                    std::wstring msg = L"[チャンネルスキャン] ";
                    msg += status;
                    msg += L" BonDriver=";
                    msg += bonPath;
                    log(msg);
                }

                finish();
                return;
            }

            ChannelScanGUIOptions opt{};
            opt.space = space;
            opt.scanAllSpaces = false;
            opt.enablePromptInConsole = false;
            opt.reloadConfigAtStart = true;
            opt.dumpFixedConfigSnapshot = true;

            // ここが停止ボタンとの接続点
            opt.cancelRequested = &selfHostedStopRequested;

            opt.onStatusText = [this](const std::wstring& s) {
                PostSetStatusText(s);
                };

            opt.onRunningState = [this, &runningFalsePosted](bool running) {
                if (!running) {
                    runningFalsePosted = true;
                }
                PostSetRunning(running);
                };

            opt.onSdtSnapshot = [](int, int, const std::vector<GuiSnapshot::ServiceRow>&) {
                };

            channel_scan_start(bon, FilterManagerMode::ChannelScan, opt);
        }
        catch (...) {
            PostSetStatusText(L"自己完結スキャン中に例外が発生しました。");
            if (log) log(L"[チャンネルスキャン] 自己完結スキャン中に例外が発生しました。");
        }

        finish();
        });

    return true;
}

void ChannelScanTab::RequestStopSelfHostedScan()
{
    if (!selfHostedScanActive.load(std::memory_order_acquire)) {
        if (log) log(L"[チャンネルスキャン] 自己完結スキャンは動作していません。");
        return;
    }

    selfHostedStopRequested.store(true, std::memory_order_release);
    SetStatusText(L"停止要求を送信しました…");

    if (log) {
        log(L"[チャンネルスキャン] 自己完結モードへ停止要求を送信しました。");
    }
}

void ChannelScanTab::JoinSelfHostedScanIfNeeded()
{
    if (selfHostedScanThread.joinable()) {
        selfHostedScanThread.join();
    }
    selfHostedScanActive.store(false);
    selfHostedStopRequested.store(false);
}

void ChannelScanTab::AddReceivableChannelRow(
    int space, int ch, std::wstring_view /*chName*/,
    float signalLevelDb,
    std::size_t pat, std::size_t pmt, std::size_t sdt, std::size_t nit)
{
    const std::uint32_t key = MakeChannelKey(space, ch);

    ChannelStat st{};
    st.signalLevelDb = signalLevelDb;
    st.pat = pat; st.pmt = pmt; st.sdt = sdt; st.nit = nit;
    st.has = true;
    channelStats[key] = st;

    // 現在の一覧に同じ space/ch のサービス行があるなら反映
    if (!lvServices) return;

    auto it = channelToRowIndices.find(key);
    if (it == channelToRowIndices.end()) return;

    wchar_t sigBuf[64]{};
    _snwprintf_s(sigBuf, _countof(sigBuf), _TRUNCATE, L"%.1f", signalLevelDb);

    const std::wstring wPat = std::to_wstring(pat);
    const std::wstring wPmt = std::to_wstring(pmt);
    const std::wstring wSdt = std::to_wstring(sdt);
    const std::wstring wNit = std::to_wstring(nit);

    for (int idx : it->second) {
        if (idx < 0) continue;
        ListView_SetItemText(lvServices, idx, COL_SignalDb, sigBuf);
        ListView_SetItemText(lvServices, idx, COL_PAT, const_cast<LPWSTR>(wPat.c_str()));
        ListView_SetItemText(lvServices, idx, COL_PMT, const_cast<LPWSTR>(wPmt.c_str()));
        ListView_SetItemText(lvServices, idx, COL_SDT, const_cast<LPWSTR>(wSdt.c_str()));
        ListView_SetItemText(lvServices, idx, COL_NIT, const_cast<LPWSTR>(wNit.c_str()));
    }
}

// ---------------------- services list ----------------------

void ChannelScanTab::ClearServiceList()
{
    if (!lvServices) return;

    suppressServiceNotify = true;
    ListView_DeleteAllItems(lvServices);
    suppressServiceNotify = false;

    serviceRows.clear();
    channelToRowIndices.clear();
}

void ChannelScanTab::LoadServiceListForSelectedSpace()
{
    if (!cbSpace) return;
    int sel = (int)SendMessageW(cbSpace, CB_GETCURSEL, 0, 0);
    if (sel < 0) sel = 0;
    LoadServiceList(sel);
}

bool ChannelScanTab::ReadServiceEnabledFromIni(int space, std::uint16_t onid, std::uint16_t tsid, std::uint16_t sid, bool fallbackDefault) const
{
    const std::wstring iniPath = ConfigManager::GetInstance().GetIniPath();
    const std::wstring section = L"EpgServiceEnable_space" + std::to_wstring(space);
    const std::wstring key = std::to_wstring(onid) + L"-" + std::to_wstring(tsid) + L"-" + std::to_wstring(sid);

    wchar_t buf[1024]{};
    const DWORD n = GetPrivateProfileStringW(section.c_str(), key.c_str(), L"", buf, _countof(buf), iniPath.c_str());
    if (n == 0) return fallbackDefault;

    // value: enabled,... （enabled だけでも許容）
    std::wstring v(buf);
    int enabled = 0;
    try {
        size_t idx = 0;
        enabled = std::stoi(v, &idx, 10);
    }
    catch (...) {
        return fallbackDefault;
    }
    return enabled != 0;
}

void ChannelScanTab::WriteServiceEnabledToIni(int space,
    std::uint16_t onid, std::uint16_t tsid, std::uint16_t sid,
    bool enabled,
    int ch, int serviceType, int freeCA, int remoteKeyId,
    const std::wstring& serviceName,
    const std::wstring& provider,
    const std::wstring& networkName)
{
    const std::wstring iniPath = ConfigManager::GetInstance().GetIniPath();
    const std::wstring section = L"EpgServiceEnable_space" + std::to_wstring(space);
    const std::wstring key = std::to_wstring(onid) + L"-" + std::to_wstring(tsid) + L"-" + std::to_wstring(sid);

    // value: enabled,"ServiceName",space,ch,type,sid,onid,tsid,CA,"Provider","NetworkName",RC
    std::wstring value;
    value.reserve(512);
    value += (enabled ? L"1" : L"0");
    value += L",";
    value += QuoteCsvField(serviceName);
    value += L",";
    value += std::to_wstring(space);
    value += L",";
    value += std::to_wstring(ch);
    value += L",";
    value += std::to_wstring(serviceType);
    value += L",";
    value += std::to_wstring(sid);
    value += L",";
    value += std::to_wstring(onid);
    value += L",";
    value += std::to_wstring(tsid);
    value += L",";
    value += std::to_wstring(freeCA);
    value += L",";
    value += QuoteCsvField(provider);
    value += L",";
    value += QuoteCsvField(networkName);
    value += L",";
    value += std::to_wstring(remoteKeyId);

    WritePrivateProfileStringW(section.c_str(), key.c_str(), value.c_str(), iniPath.c_str());
}

bool ChannelScanTab::MakeInitialServiceEnabledDefault(const ServiceRow& r) const
{
    // 通常テレビサービス以外は初期無効。
    // 例:
    //   1   = デジタルTV
    //   161 = 臨時サービス
    //   192 = ワンセグ/携帯向け
    if (r.serviceType != 1) {
        return false;
    }

    return ConfigManager::GetInstance().GetEpgServiceDefaultEnable(
        r.space,
        true);
}

void ChannelScanTab::SaveServiceRowEnabledToIni(const ServiceRow& r)
{
    WriteServiceEnabledToIni(
        r.space,
        r.onid,
        r.tsid,
        r.serviceId,
        r.enabled,
        r.ch,
        r.serviceType,
        r.freeCA,
        r.remoteControlKeyId,
        r.serviceName,
        r.provider,
        r.networkName);
}

void ChannelScanTab::SaveAllServiceRowsEnabledToIni(bool rewriteSpaceSection)
{
    if (serviceRows.empty()) {
        SetStatusText(L"EPG対象サービス設定: 保存対象がありません。");
        return;
    }

    const int space = serviceRows.front().space;
    const std::wstring iniPath =
        ConfigManager::GetInstance().GetIniPath();

    const std::wstring section =
        L"EpgServiceEnable_space" + std::to_wstring(space);

    if (rewriteSpaceSection) {
        // 古い残骸キーを消してから、現在の一覧を全件保存する。
        // これで「前回の無効化行だけ残る」事故を防ぐ。
        WritePrivateProfileStringW(
            section.c_str(),
            nullptr,
            nullptr,
            iniPath.c_str());
    }

    int enabledCount = 0;
    int disabledCount = 0;

    for (const auto& r : serviceRows) {
        SaveServiceRowEnabledToIni(r);

        if (r.enabled) {
            ++enabledCount;
        }
        else {
            ++disabledCount;
        }
    }

    // ini キャッシュの反映を促す。
    WritePrivateProfileStringW(
        nullptr,
        nullptr,
        nullptr,
        iniPath.c_str());

    SetStatusText(
        L"EPG対象サービス設定を保存しました: space=" +
        std::to_wstring(space) +
        L" / 有効=" + std::to_wstring(enabledCount) +
        L" / 無効=" + std::to_wstring(disabledCount));
}

void ChannelScanTab::LoadServiceList(int space)
{
    if (!lvServices) return;

    ClearServiceList();

    // ch ファイルパス（exe dir）
    const std::filesystem::path filePath =
        std::filesystem::path(GetBaseDir()) / (L"channellist_space" + std::to_wstring(space) + L".ch");

    const std::wstring text = ReadTextFileAutoEncoding(filePath);
    if (text.empty()) {
        SetStatusText(std::wstring(L"サービス一覧: ファイルが見つからない / 空です: ") + filePath.wstring());
        return;
    }

    suppressServiceNotify = true;

    std::wstringstream wss(text);
    std::wstring line;

    int rowIndex = 0;
    while (std::getline(wss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;

        // BOM (U+FEFF) が先頭に入るケース
        if (!line.empty() && line[0] == 0xFEFF) line.erase(0, 1);

        // comment
        if (!line.empty() && line[0] == L';') continue;

        const auto f = ParseCsvLine(line);
        if (f.size() < 12) continue;

        ServiceRow r{};
        r.serviceName = f[0];
        r.space = ParseIntOr(f[1], space);
        r.ch = ParseIntOr(f[2], 0);
        r.serviceType = ParseIntOr(f[3], 0);
        (void)TryParseU16(f[4], r.serviceId);
        (void)TryParseU16(f[5], r.networkId);
        (void)TryParseU16(f[6], r.tsid);
        (void)TryParseU16(f[7], r.onid);
        r.freeCA = ParseIntOr(f[8], 0);
        r.provider = f[9];
        r.networkName = f[10];
        r.remoteControlKeyId = ParseIntOr(f[11], 0);

        // 念のため space が一致しない行は除外
        if (r.space != space) continue;

        // enabled は ini の個別設定を最優先。
// 個別設定が無い場合だけ、serviceType と DefaultEnable から初期値を決める。
        const bool initialEnabled =
            MakeInitialServiceEnabledDefault(r);

        r.enabled = ReadServiceEnabledFromIni(
            space,
            r.onid,
            r.tsid,
            r.serviceId,
            initialEnabled);

        serviceRows.push_back(r);

        // ListView へ
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = rowIndex;
        item.pszText = const_cast<LPWSTR>(serviceRows.back().serviceName.c_str());
        const int idx = ListView_InsertItem(lvServices, &item);
        if (idx < 0) continue;

        auto setText = [&](int col, const std::wstring& s) {
            ListView_SetItemText(lvServices, idx, col, const_cast<LPWSTR>(s.c_str()));
            };

        setText(COL_SID, std::to_wstring(serviceRows.back().serviceId));
        setText(COL_Type, std::to_wstring(serviceRows.back().serviceType));
        setText(COL_Space, std::to_wstring(serviceRows.back().space));
        setText(COL_CH, std::to_wstring(serviceRows.back().ch));

        setText(COL_Network, serviceRows.back().networkName);
        setText(COL_ONID, std::to_wstring(serviceRows.back().onid));
        setText(COL_TSID, std::to_wstring(serviceRows.back().tsid));
        setText(COL_RCK, std::to_wstring(serviceRows.back().remoteControlKeyId));
        setText(COL_CA, std::to_wstring(serviceRows.back().freeCA));

        // scan stats がすでにあれば反映
        const std::uint32_t ckey = MakeChannelKey(space, serviceRows.back().ch);
        channelToRowIndices[ckey].push_back(idx);

        auto stIt = channelStats.find(ckey);
        if (stIt != channelStats.end() && stIt->second.has) {
            wchar_t sigBuf[64]{};
            _snwprintf_s(sigBuf, _countof(sigBuf), _TRUNCATE, L"%.1f", stIt->second.signalLevelDb);
            ListView_SetItemText(lvServices, idx, COL_SignalDb, sigBuf);

            const std::wstring wPat = std::to_wstring(stIt->second.pat);
            const std::wstring wPmt = std::to_wstring(stIt->second.pmt);
            const std::wstring wSdt = std::to_wstring(stIt->second.sdt);
            const std::wstring wNit = std::to_wstring(stIt->second.nit);
            setText(COL_PAT, wPat);
            setText(COL_PMT, wPmt);
            setText(COL_SDT, wSdt);
            setText(COL_NIT, wNit);
        }

        ListView_SetCheckState(lvServices, idx, serviceRows.back().enabled ? TRUE : FALSE);

        ++rowIndex;
    }

    suppressServiceNotify = false;

    int enabledCount = 0;
    int disabledCount = 0;

    for (const auto& r : serviceRows) {
        if (r.enabled) {
            ++enabledCount;
        }
        else {
            ++disabledCount;
        }
    }

    SetStatusText(
        L"サービス一覧を読み込み: " +
        filePath.wstring() +
        L" / 有効=" + std::to_wstring(enabledCount) +
        L" / 無効=" + std::to_wstring(disabledCount));
}

void ChannelScanTab::OnServiceListItemChanged(int item, UINT oldState, UINT newState)
{
    if (!lvServices) return;
    if (item < 0 || (std::size_t)item >= serviceRows.size()) return;

    const UINT oldImg = (oldState & LVIS_STATEIMAGEMASK);
    const UINT newImg = (newState & LVIS_STATEIMAGEMASK);
    if (oldImg == newImg) return; // checkbox 変化ではない

    const bool enabled = (ListView_GetCheckState(lvServices, item) != FALSE);
    serviceRows[(std::size_t)item].enabled = enabled;

    // ユーザー操作によるチェック変更は、その1行だけ即保存する。
    const auto& r = serviceRows[static_cast<std::size_t>(item)];
    SaveServiceRowEnabledToIni(r);

    SetStatusText(
        L"EPG対象サービスを更新: " +
        r.serviceName +
        L" = " +
        (enabled ? L"有効" : L"無効"));
}

// ---------------------- PostXXX（スレッドセーフ） ----------------------

void ChannelScanTab::PostClearResults()
{
    if (!hWnd) return;
    PostMessageW(hWnd, WMU_CLEAR_RESULTS, 0, 0);
}

void ChannelScanTab::PostSetRunning(bool running)
{
    if (!hWnd) return;
    PostMessageW(hWnd, WMU_SET_RUNNING, running ? 1 : 0, 0);
}

void ChannelScanTab::PostSetStatusText(std::wstring text)
{
    if (!hWnd) return;
    auto* p = new StatusPayload{};
    p->text = std::move(text);
    if (!PostMessageW(hWnd, WMU_SET_STATUS, 0, (LPARAM)p)) {
        delete p;
    }
}

void ChannelScanTab::PostAddReceivableChannelRow(
    int space, int ch, std::wstring chName,
    float signalLevelDb,
    std::size_t pat, std::size_t pmt, std::size_t sdt, std::size_t nit)
{
    if (!hWnd) return;

    auto* p = new RowPayload{};
    p->space = space;
    p->ch = ch;
    p->chName = std::move(chName);
    p->signalLevelDb = signalLevelDb;
    p->pat = pat; p->pmt = pmt; p->sdt = sdt; p->nit = nit;

    if (!PostMessageW(hWnd, WMU_ADD_ROW, 0, (LPARAM)p)) {
        delete p;
    }
}
