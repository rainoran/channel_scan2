#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "SystemResourceMonitorTab.hpp"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "ConfigManager.h"

#pragma comment(lib, "Psapi.lib")

namespace
{
    constexpr wchar_t kClassName[] = L"TSCEPG_SystemResourceMonitorTab";

    constexpr int IDC_SRM_START_STOP = 9101;
    constexpr int IDC_SRM_SAMPLE = 9102;
    constexpr UINT_PTR IDT_SRM_SAMPLE = 9103;
    constexpr int IDC_SRM_CONSOLE_LOG = 9104;

    void SetFontIf_(HWND h, HFONT f)
    {
        if (h != nullptr && f != nullptr) {
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        }
    }

    int MaxInt_(int a, int b) noexcept
    {
        return (a > b) ? a : b;
    }

    std::string WideToUtf8_(const std::wstring& s)
    {
        if (s.empty()) {
            return {};
        }

        const int len =
            WideCharToMultiByte(
                CP_UTF8,
                0,
                s.data(),
                static_cast<int>(s.size()),
                nullptr,
                0,
                nullptr,
                nullptr);

        if (len <= 0) {
            return {};
        }

        std::string out(static_cast<std::size_t>(len), '\0');

        WideCharToMultiByte(
            CP_UTF8,
            0,
            s.data(),
            static_cast<int>(s.size()),
            out.data(),
            len,
            nullptr,
            nullptr);

        return out;
    }

    std::wstring NowText_()
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);

        wchar_t buf[64]{};

        swprintf_s(
            buf,
            L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
            static_cast<unsigned>(st.wYear),
            static_cast<unsigned>(st.wMonth),
            static_cast<unsigned>(st.wDay),
            static_cast<unsigned>(st.wHour),
            static_cast<unsigned>(st.wMinute),
            static_cast<unsigned>(st.wSecond),
            static_cast<unsigned>(st.wMilliseconds));

        return buf;
    }

    std::wstring DateText_()
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);

        wchar_t buf[32]{};

        swprintf_s(
            buf,
            L"%04u%02u%02u",
            static_cast<unsigned>(st.wYear),
            static_cast<unsigned>(st.wMonth),
            static_cast<unsigned>(st.wDay));

        return buf;
    }
}

bool SystemResourceMonitorTab::Create(
    HWND hParent,
    const RECT& rc,
    HINSTANCE hInst,
    HFONT hFont,
    std::function<void(std::wstring_view)>&& log)
{
    hInst_ = hInst;
    hFont_ = hFont;
    log_ = std::move(log);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &SystemResourceMonitorTab::WndProc;
    wc.hInstance = hInst_;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);

    RegisterClassExW(&wc);

    hWnd_ = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kClassName,
        L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
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

void SystemResourceMonitorTab::Show(bool show)
{
    if (hWnd_ != nullptr) {
        ShowWindow(hWnd_, show ? SW_SHOW : SW_HIDE);
    }
}

void SystemResourceMonitorTab::Move(const RECT& rc)
{
    if (hWnd_ != nullptr) {
        MoveWindow(
            hWnd_,
            rc.left,
            rc.top,
            rc.right - rc.left,
            rc.bottom - rc.top,
            TRUE);
    }
}

void SystemResourceMonitorTab::ApplyToConfig()
{
    SaveSettings_();
}

void SystemResourceMonitorTab::ReloadFromConfig()
{
    if (hWnd_ == nullptr) {
        return;
    }

    LoadSettings_();
    ApplyConsoleLogCheckState_();

    ResetBaseline_();
    OnManualSample_();
}

void SystemResourceMonitorTab::StopMonitor() noexcept
{
    if (hWnd_ != nullptr) {
        KillTimer(hWnd_, IDT_SRM_SAMPLE);
    }

    monitoring_ = false;

    if (hBtnStartStop_ != nullptr) {
        SetWindowTextW(hBtnStartStop_, L"監視開始");
    }
}

LRESULT CALLBACK SystemResourceMonitorTab::WndProc(
    HWND hWnd,
    UINT msg,
    WPARAM wp,
    LPARAM lp)
{
    SystemResourceMonitorTab* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<SystemResourceMonitorTab*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hWnd_ = hWnd;
    }
    else {
        self = reinterpret_cast<SystemResourceMonitorTab*>(
            GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->HandleMessage(msg, wp, lp);
    }

    return DefWindowProcW(hWnd, msg, wp, lp);
}

LRESULT SystemResourceMonitorTab::HandleMessage(
    UINT msg,
    WPARAM wp,
    LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        OnCreate();
        return 0;

    case WM_SIZE:
        OnSize(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_TIMER:
        if (wp == IDT_SRM_SAMPLE) {
            OnTimer_();
            return 0;
        }
        break;

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_SRM_START_STOP) {
            OnStartStop_();
            return 0;
        }

        if (LOWORD(wp) == IDC_SRM_SAMPLE) {
            OnManualSample_();
            return 0;
        }

        if (LOWORD(wp) == IDC_SRM_CONSOLE_LOG) {
            consoleLogEnabled_ =
                SendMessageW(
                    hChkConsoleLog_,
                    BM_GETCHECK,
                    0,
                    0) == BST_CHECKED;

            SaveSettings_();

            SetInfo_(
                consoleLogEnabled_
                ? L"console.log へのアイドリング監視ログ出力を有効にしました。"
                : L"console.log へのアイドリング監視ログ出力を停止しました。画面表示の監視は継続します。");

            return 0;
        }
        break;

    case WM_DESTROY:
        StopMonitor();
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hWnd_, msg, wp, lp);
}

void SystemResourceMonitorTab::OnCreate()
{
    logicalProcessors_ = GetLogicalProcessorCount_();

    if (logicalProcessors_ == 0) {
        logicalProcessors_ = 1;
    }

    LoadSettings_();

    hLblStatus_ = CreateWindowExW(
        0,
        L"STATIC",
        L"アイドリング監視",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        10,
        10,
        360,
        24,
        hWnd_,
        nullptr,
        hInst_,
        nullptr);

    hBtnStartStop_ = CreateWindowExW(
        0,
        L"BUTTON",
        L"監視開始",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        380,
        8,
        100,
        28,
        hWnd_,
        CtrlId_(IDC_SRM_START_STOP),
        hInst_,
        nullptr);

    hBtnSample_ = CreateWindowExW(
        0,
        L"BUTTON",
        L"今すぐ測定",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        490,
        8,
        110,
        28,
        hWnd_,
        CtrlId_(IDC_SRM_SAMPLE),
        hInst_,
        nullptr);

    hChkConsoleLog_ = CreateWindowExW(
        0,
        L"BUTTON",
        L"console.logへ出力",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        610,
        10,
        180,
        24,
        hWnd_,
        CtrlId_(IDC_SRM_CONSOLE_LOG),
        hInst_,
        nullptr);

    hInfo_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE |
        ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        10,
        44,
        760,
        360,
        hWnd_,
        nullptr,
        hInst_,
        nullptr);

    SetFontIf_(hLblStatus_, hFont_);
    SetFontIf_(hBtnStartStop_, hFont_);
    SetFontIf_(hBtnSample_, hFont_);
    SetFontIf_(hChkConsoleLog_, hFont_);
    SetFontIf_(hInfo_, hFont_);

    ApplyConsoleLogCheckState_();

    ResetBaseline_();
    OnManualSample_();
}

void SystemResourceMonitorTab::OnSize(int w, int h)
{
    const int pad = 10;
    const int topH = 42;

    if (hLblStatus_ != nullptr) {
        MoveWindow(hLblStatus_, pad, 10, 360, 24, TRUE);
    }

    if (hBtnStartStop_ != nullptr) {
        MoveWindow(hBtnStartStop_, 380, 8, 100, 28, TRUE);
    }

    if (hBtnSample_ != nullptr) {
        MoveWindow(hBtnSample_, 490, 8, 110, 28, TRUE);
    }

    if (hChkConsoleLog_ != nullptr) {
        MoveWindow(hChkConsoleLog_, 610, 10, 180, 24, TRUE);
    }

    if (hInfo_ != nullptr) {
        MoveWindow(
            hInfo_,
            pad,
            topH,
            MaxInt_(100, w - pad * 2),
            MaxInt_(100, h - topH - pad),
            TRUE);
    }
}

void SystemResourceMonitorTab::OnStartStop_()
{
    if (monitoring_) {
        StopMonitor();
        SetInfo_(L"アイドリング監視を停止しました。");
        return;
    }

    StartMonitor_();
}

void SystemResourceMonitorTab::OnManualSample_()
{
    const Sample sample = MakeSample_();

    UpdateView_(sample);
    AppendSampleLog_(sample);
}

void SystemResourceMonitorTab::OnTimer_()
{
    const Sample sample = MakeSample_();

    UpdateView_(sample);
    AppendSampleLog_(sample);
}

void SystemResourceMonitorTab::LoadSettings_() noexcept
{
    try {
        auto& cfg = ConfigManager::GetInstance();

        intervalMs_ =
            static_cast<DWORD>(
                cfg.GetIdleMonitorSampleMs());

        consoleLogEnabled_ =
            cfg.GetIdleMonitorConsoleLog();

        idleSystemCpuThresholdPct_ =
            static_cast<DWORD>(
                cfg.GetIdleMonitorSystemCpuPct());

        idleProcessCpuThresholdPct_ =
            static_cast<DWORD>(
                cfg.GetIdleMonitorProcessCpuPct());

        idleReadKbThreshold_ =
            static_cast<DWORD>(
                cfg.GetIdleMonitorReadKBps());

        idleWriteKbThreshold_ =
            static_cast<DWORD>(
                cfg.GetIdleMonitorWriteKBps());

        idleMemoryLoadThresholdPct_ =
            static_cast<DWORD>(
                cfg.GetIdleMonitorMemoryLoadPct());
    }
    catch (...) {
        intervalMs_ = 5000;
        consoleLogEnabled_ = true;
        idleSystemCpuThresholdPct_ = 20;
        idleProcessCpuThresholdPct_ = 5;
        idleReadKbThreshold_ = 2048;
        idleWriteKbThreshold_ = 4096;
        idleMemoryLoadThresholdPct_ = 90;
    }
}

void SystemResourceMonitorTab::SaveSettings_() noexcept
{
    try {
        auto& cfg = ConfigManager::GetInstance();

        cfg.SetIdleMonitorSampleMs(
            static_cast<int>(intervalMs_));

        cfg.SetIdleMonitorConsoleLog(consoleLogEnabled_);

        cfg.SetIdleMonitorSystemCpuPct(
            static_cast<int>(idleSystemCpuThresholdPct_));

        cfg.SetIdleMonitorProcessCpuPct(
            static_cast<int>(idleProcessCpuThresholdPct_));

        cfg.SetIdleMonitorReadKBps(
            static_cast<int>(idleReadKbThreshold_));

        cfg.SetIdleMonitorWriteKBps(
            static_cast<int>(idleWriteKbThreshold_));

        cfg.SetIdleMonitorMemoryLoadPct(
            static_cast<int>(idleMemoryLoadThresholdPct_));

        cfg.SaveConfig();
    }
    catch (...) {
    }
}

void SystemResourceMonitorTab::ApplyConsoleLogCheckState_() noexcept
{
    if (hChkConsoleLog_ == nullptr) {
        return;
    }

    SendMessageW(
        hChkConsoleLog_,
        BM_SETCHECK,
        consoleLogEnabled_ ? BST_CHECKED : BST_UNCHECKED,
        0);
}

void SystemResourceMonitorTab::StartMonitor_()
{
    if (hWnd_ == nullptr) {
        return;
    }

    StopMonitor();

    ApplyConsoleLogCheckState_();
    ResetBaseline_();

    monitoring_ = true;

    SetTimer(
        hWnd_,
        IDT_SRM_SAMPLE,
        intervalMs_,
        nullptr);

    if (hBtnStartStop_ != nullptr) {
        SetWindowTextW(hBtnStartStop_, L"監視停止");
    }

    SetInfo_(L"アイドリング監視を開始しました。");
}

void SystemResourceMonitorTab::ResetBaseline_()
{
    lastTickMs_ = GetTickCount64();
    lastCpu_ = GetSystemCpuTimes_();
    lastProcessCpu100ns_ = GetProcessCpuTime100ns_();
    lastIo_ = GetProcessIoSnapshot_();
}

bool SystemResourceMonitorTab::IsIdleSample_(
    const Sample& sample) const noexcept
{
    if (sample.elapsedMs == 0) {
        return false;
    }

    if (sample.systemCpuPct >
        static_cast<double>(idleSystemCpuThresholdPct_)) {
        return false;
    }

    if (sample.processCpuPct >
        static_cast<double>(idleProcessCpuThresholdPct_)) {
        return false;
    }

    if (sample.processReadKbPerSec >
        static_cast<std::uint64_t>(idleReadKbThreshold_)) {
        return false;
    }

    if (sample.processWriteKbPerSec >
        static_cast<std::uint64_t>(idleWriteKbThreshold_)) {
        return false;
    }

    if (sample.memoryLoadPct > idleMemoryLoadThresholdPct_) {
        return false;
    }

    return true;
}

std::wstring SystemResourceMonitorTab::MakeIdleReason_(
    const Sample& sample) const
{
    if (sample.idle) {
        return L"IDLE: CPU / App CPU / I/O / Memory が閾値以下です。";
    }

    std::wstring reason = L"BUSY:";

    bool any = false;

    const auto addReason =
        [&reason, &any](const wchar_t* text)
        {
            if (any) {
                reason += L",";
            }

            reason += L" ";
            reason += text;
            any = true;
        };

    if (sample.systemCpuPct >
        static_cast<double>(idleSystemCpuThresholdPct_)) {
        addReason(L"System CPU");
    }

    if (sample.processCpuPct >
        static_cast<double>(idleProcessCpuThresholdPct_)) {
        addReason(L"App CPU");
    }

    if (sample.processReadKbPerSec >
        static_cast<std::uint64_t>(idleReadKbThreshold_)) {
        addReason(L"Read I/O");
    }

    if (sample.processWriteKbPerSec >
        static_cast<std::uint64_t>(idleWriteKbThreshold_)) {
        addReason(L"Write I/O");
    }

    if (sample.memoryLoadPct > idleMemoryLoadThresholdPct_) {
        addReason(L"Memory");
    }

    if (!any) {
        reason += L" baseline更新直後、または測定間隔不足";
    }

    return reason;
}

SystemResourceMonitorTab::Sample SystemResourceMonitorTab::MakeSample_()
{
    Sample out{};

    const ULONGLONG nowTick = GetTickCount64();
    const CpuTimes nowCpu = GetSystemCpuTimes_();
    const std::uint64_t nowProcCpu = GetProcessCpuTime100ns_();
    const IoSnapshot nowIo = GetProcessIoSnapshot_();

    out.elapsedMs =
        nowTick >= lastTickMs_
        ? static_cast<std::uint64_t>(nowTick - lastTickMs_)
        : 0;

    if (out.elapsedMs > 0 &&
        lastCpu_.valid &&
        nowCpu.valid) {
        const std::uint64_t idleDelta =
            nowCpu.idle - lastCpu_.idle;

        const std::uint64_t kernelDelta =
            nowCpu.kernel - lastCpu_.kernel;

        const std::uint64_t userDelta =
            nowCpu.user - lastCpu_.user;

        const std::uint64_t totalDelta =
            kernelDelta + userDelta;

        if (totalDelta > 0 && totalDelta >= idleDelta) {
            out.systemCpuPct =
                100.0 *
                static_cast<double>(totalDelta - idleDelta) /
                static_cast<double>(totalDelta);
        }
    }

    if (out.elapsedMs > 0 &&
        nowProcCpu >= lastProcessCpu100ns_) {
        const std::uint64_t delta100ns =
            nowProcCpu - lastProcessCpu100ns_;

        const double elapsed100ns =
            static_cast<double>(out.elapsedMs) * 10000.0;

        const double denom =
            elapsed100ns *
            static_cast<double>(std::max<DWORD>(1, logicalProcessors_));

        if (denom > 0.0) {
            out.processCpuPct =
                100.0 *
                static_cast<double>(delta100ns) /
                denom;
        }
    }

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);

    if (GlobalMemoryStatusEx(&mem) != FALSE) {
        out.memoryLoadPct = mem.dwMemoryLoad;
        out.totalPhysMb =
            static_cast<std::uint64_t>(mem.ullTotalPhys / (1024ull * 1024ull));
        out.availPhysMb =
            static_cast<std::uint64_t>(mem.ullAvailPhys / (1024ull * 1024ull));
    }

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);

    if (GetProcessMemoryInfo(
        GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
        sizeof(pmc)) != FALSE) {
        out.workingSetMb =
            static_cast<std::uint64_t>(
                pmc.WorkingSetSize / (1024ull * 1024ull));

        out.privateUsageMb =
            static_cast<std::uint64_t>(
                pmc.PrivateUsage / (1024ull * 1024ull));
    }

    if (out.elapsedMs > 0 &&
        lastIo_.valid &&
        nowIo.valid) {
        const std::uint64_t readDelta =
            nowIo.readBytes >= lastIo_.readBytes
            ? nowIo.readBytes - lastIo_.readBytes
            : 0;

        const std::uint64_t writeDelta =
            nowIo.writeBytes >= lastIo_.writeBytes
            ? nowIo.writeBytes - lastIo_.writeBytes
            : 0;

        out.processReadKbPerSec =
            readDelta * 1000ull / out.elapsedMs / 1024ull;

        out.processWriteKbPerSec =
            writeDelta * 1000ull / out.elapsedMs / 1024ull;
    }

    ProbeDrive_(
        GetExeDir_(),
        out.exeDrive,
        out.exeDriveTotalGb,
        out.exeDriveFreeGb,
        out.exeDriveFreePct);

    ProbeDrive_(
        GetTimeShiftRoot_(),
        out.timeShiftDrive,
        out.timeShiftDriveTotalGb,
        out.timeShiftDriveFreeGb,
        out.timeShiftDriveFreePct);

    out.idle = IsIdleSample_(out);
    out.idleReason = MakeIdleReason_(out);

    lastTickMs_ = nowTick;
    lastCpu_ = nowCpu;
    lastProcessCpu100ns_ = nowProcCpu;
    lastIo_ = nowIo;

    return out;
}

void SystemResourceMonitorTab::UpdateView_(const Sample& sample)
{
    std::wstring text;

    text += L"アイドリング監視\r\n";
    text += L"\r\n[Idle]\r\n";
    text += L"State: ";
    text += sample.idle ? L"IDLE" : L"BUSY";
    text += L"\r\nReason: ";
    text += sample.idleReason;

    text += L"\r\nThreshold: systemCpu<=";
    text += std::to_wstring(idleSystemCpuThresholdPct_);
    text += L"% appCpu<=";
    text += std::to_wstring(idleProcessCpuThresholdPct_);
    text += L"% read<=";
    text += std::to_wstring(idleReadKbThreshold_);
    text += L"KB/s write<=";
    text += std::to_wstring(idleWriteKbThreshold_);
    text += L"KB/s memory<=";
    text += std::to_wstring(idleMemoryLoadThresholdPct_);
    text += L"%\r\n";
    text += L"時刻: ";
    text += NowText_();

    text += L"\r\n間隔(ms): ";
    text += std::to_wstring(sample.elapsedMs);

    text += L"\r\n\r\n[CPU]\r\n";
    text += L"System CPU: ";
    text += FormatDouble_(sample.systemCpuPct);
    text += L" %\r\n";

    text += L"App CPU: ";
    text += FormatDouble_(sample.processCpuPct);
    text += L" %\r\n";

    text += L"Logical processors: ";
    text += std::to_wstring(logicalProcessors_);

    text += L"\r\n\r\n[Memory]\r\n";
    text += L"Memory load: ";
    text += std::to_wstring(sample.memoryLoadPct);
    text += L" %\r\n";

    text += L"Total physical: ";
    text += std::to_wstring(sample.totalPhysMb);
    text += L" MB\r\n";

    text += L"Available physical: ";
    text += std::to_wstring(sample.availPhysMb);
    text += L" MB\r\n";

    text += L"App WorkingSet: ";
    text += std::to_wstring(sample.workingSetMb);
    text += L" MB\r\n";

    text += L"App PrivateUsage: ";
    text += std::to_wstring(sample.privateUsageMb);
    text += L" MB\r\n";

    text += L"\r\n[App I/O]\r\n";
    text += L"Read: ";
    text += std::to_wstring(sample.processReadKbPerSec);
    text += L" KB/s\r\n";

    text += L"Write: ";
    text += std::to_wstring(sample.processWriteKbPerSec);
    text += L" KB/s\r\n";

    text += L"\r\n[Drive]\r\n";
    text += L"Exe drive: ";
    text += sample.exeDrive;
    text += L" free ";
    text += std::to_wstring(sample.exeDriveFreeGb);
    text += L" / ";
    text += std::to_wstring(sample.exeDriveTotalGb);
    text += L" GB (";
    text += std::to_wstring(sample.exeDriveFreePct);
    text += L"%)\r\n";

    text += L"TimeShift drive: ";
    text += sample.timeShiftDrive;
    text += L" free ";
    text += std::to_wstring(sample.timeShiftDriveFreeGb);
    text += L" / ";
    text += std::to_wstring(sample.timeShiftDriveTotalGb);
    text += L" GB (";
    text += std::to_wstring(sample.timeShiftDriveFreePct);
    text += L"%)\r\n";

    text += L"\r\nconsole.log output: ";
    text += consoleLogEnabled_ ? L"ON" : L"OFF";

    text += L"\r\nLog: ";
    text += MakeLogPath_().wstring();

    SetInfo_(text);
}

void SystemResourceMonitorTab::AppendSampleLog_(const Sample& sample)
{
    if (!consoleLogEnabled_) {
        return;
    }

    if (sample.elapsedMs < 1000) {
        return;
    }

    const std::filesystem::path path = MakeLogPath_();

    std::error_code ec;

    const bool exists =
        std::filesystem::exists(path, ec);

    std::ofstream ofs(
        path,
        std::ios::binary | std::ios::app);

    if (!ofs.is_open()) {
        return;
    }

    if (!exists) {
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        ofs.write(reinterpret_cast<const char*>(bom), 3);
    }

    std::wstring line;

    line += L"[IdleMonitor]";
    line += L" time=";
    line += NowText_();

    line += L" idle=";
    line += sample.idle ? L"1" : L"0";

    line += L" state=";
    line += sample.idle ? L"IDLE" : L"BUSY";

    line += L" reason=\"";
    line += sample.idleReason;
    line += L"\"";

    line += L" elapsedMs=";
    line += std::to_wstring(sample.elapsedMs);

    line += L" systemCpuPct=";
    line += FormatDouble_(sample.systemCpuPct);

    line += L" processCpuPct=";
    line += FormatDouble_(sample.processCpuPct);

    line += L" memoryLoadPct=";
    line += std::to_wstring(sample.memoryLoadPct);

    line += L" workingSetMb=";
    line += std::to_wstring(sample.workingSetMb);

    line += L" privateUsageMb=";
    line += std::to_wstring(sample.privateUsageMb);

    line += L" readKBps=";
    line += std::to_wstring(sample.processReadKbPerSec);

    line += L" writeKBps=";
    line += std::to_wstring(sample.processWriteKbPerSec);

    line += L" exeDrive=\"";
    line += sample.exeDrive;
    line += L"\"";

    line += L" exeFreeGB=";
    line += std::to_wstring(sample.exeDriveFreeGb);

    line += L" exeTotalGB=";
    line += std::to_wstring(sample.exeDriveTotalGb);

    line += L" timeShiftDrive=\"";
    line += sample.timeShiftDrive;
    line += L"\"";

    line += L" timeShiftFreeGB=";
    line += std::to_wstring(sample.timeShiftDriveFreeGb);

    line += L" timeShiftTotalGB=";
    line += std::to_wstring(sample.timeShiftDriveTotalGb);

    line += L"\r\n";

    const std::string u8 = WideToUtf8_(line);

    ofs.write(
        u8.data(),
        static_cast<std::streamsize>(u8.size()));
}

void SystemResourceMonitorTab::SetInfo_(std::wstring_view text)
{
    if (hInfo_ == nullptr) {
        return;
    }

    std::wstring s(text);
    SetWindowTextW(hInfo_, s.c_str());
}

std::filesystem::path SystemResourceMonitorTab::GetExeDir_() const
{
    wchar_t path[MAX_PATH]{};

    const DWORD n =
        GetModuleFileNameW(
            nullptr,
            path,
            MAX_PATH);

    if (n == 0 || n >= MAX_PATH) {
        return L".";
    }

    return std::filesystem::path(path).parent_path();
}

std::filesystem::path SystemResourceMonitorTab::GetTimeShiftRoot_() const
{
    try {
        const std::filesystem::path root =
            ConfigManager::GetInstance().GetTimeShiftRootFolder();

        if (!root.empty()) {
            return root;
        }
    }
    catch (...) {
    }

    return GetExeDir_();
}

std::filesystem::path SystemResourceMonitorTab::MakeLogPath_() const
{
    // 端末監視タブの観測ログは debug_*.log ではなく、
    // 実行ファイル横の console.log に集約する。
    return GetExeDir_() / L"console.log";
}

HMENU SystemResourceMonitorTab::CtrlId_(int id) noexcept
{
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

std::uint64_t SystemResourceMonitorTab::FileTimeToUInt64_(
    const FILETIME& ft) noexcept
{
    ULARGE_INTEGER ui{};
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return ui.QuadPart;
}

SystemResourceMonitorTab::CpuTimes
SystemResourceMonitorTab::GetSystemCpuTimes_() noexcept
{
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};

    CpuTimes out{};

    if (GetSystemTimes(&idle, &kernel, &user) == FALSE) {
        return out;
    }

    out.idle = FileTimeToUInt64_(idle);
    out.kernel = FileTimeToUInt64_(kernel);
    out.user = FileTimeToUInt64_(user);
    out.valid = true;

    return out;
}

std::uint64_t SystemResourceMonitorTab::GetProcessCpuTime100ns_() noexcept
{
    FILETIME createTime{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};

    if (GetProcessTimes(
        GetCurrentProcess(),
        &createTime,
        &exitTime,
        &kernelTime,
        &userTime) == FALSE) {
        return 0;
    }

    return
        FileTimeToUInt64_(kernelTime) +
        FileTimeToUInt64_(userTime);
}

SystemResourceMonitorTab::IoSnapshot
SystemResourceMonitorTab::GetProcessIoSnapshot_() noexcept
{
    IO_COUNTERS c{};

    IoSnapshot out{};

    if (GetProcessIoCounters(GetCurrentProcess(), &c) == FALSE) {
        return out;
    }

    out.readBytes =
        static_cast<std::uint64_t>(c.ReadTransferCount);

    out.writeBytes =
        static_cast<std::uint64_t>(c.WriteTransferCount);

    out.valid = true;

    return out;
}

DWORD SystemResourceMonitorTab::GetLogicalProcessorCount_() noexcept
{
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);

    return si.dwNumberOfProcessors > 0
        ? si.dwNumberOfProcessors
        : 1;
}

std::wstring SystemResourceMonitorTab::FormatDouble_(double value)
{
    wchar_t buf[32]{};
    swprintf_s(buf, L"%.1f", value);
    return buf;
}

std::wstring SystemResourceMonitorTab::MakeDriveRoot_(
    const std::filesystem::path& path)
{
    std::error_code ec;

    std::filesystem::path abs =
        std::filesystem::absolute(path, ec);

    if (ec) {
        abs = path;
    }

    std::wstring s = abs.wstring();

    if (s.size() >= 2 && s[1] == L':') {
        std::wstring root = s.substr(0, 2);
        root += L"\\";
        return root;
    }

    return s;
}

void SystemResourceMonitorTab::ProbeDrive_(
    const std::filesystem::path& path,
    std::wstring& driveRoot,
    std::uint64_t& totalGb,
    std::uint64_t& freeGb,
    DWORD& freePct)
{
    driveRoot.clear();
    totalGb = 0;
    freeGb = 0;
    freePct = 0;

    driveRoot = MakeDriveRoot_(path);

    ULARGE_INTEGER freeAvailable{};
    ULARGE_INTEGER totalBytes{};
    ULARGE_INTEGER totalFree{};

    if (GetDiskFreeSpaceExW(
        driveRoot.c_str(),
        &freeAvailable,
        &totalBytes,
        &totalFree) == FALSE) {
        return;
    }

    totalGb =
        static_cast<std::uint64_t>(
            totalBytes.QuadPart / (1024ull * 1024ull * 1024ull));

    freeGb =
        static_cast<std::uint64_t>(
            totalFree.QuadPart / (1024ull * 1024ull * 1024ull));

    if (totalBytes.QuadPart > 0) {
        freePct =
            static_cast<DWORD>(
                totalFree.QuadPart * 100ull / totalBytes.QuadPart);
    }
}