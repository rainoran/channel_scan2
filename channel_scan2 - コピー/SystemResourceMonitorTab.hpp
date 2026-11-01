#pragma once
#ifndef SYSTEM_RESOURCE_MONITOR_TAB_HPP
#define SYSTEM_RESOURCE_MONITOR_TAB_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

class SystemResourceMonitorTab final
{
public:
    bool Create(
        HWND hParent,
        const RECT& rc,
        HINSTANCE hInst,
        HFONT hFont,
        std::function<void(std::wstring_view)>&& log);

    void Show(bool show);
    void Move(const RECT& rc);

    void ApplyToConfig();
    void ReloadFromConfig();

    void StopMonitor() noexcept;

private:
    struct CpuTimes
    {
        std::uint64_t idle = 0;
        std::uint64_t kernel = 0;
        std::uint64_t user = 0;
        bool valid = false;
    };

    struct IoSnapshot
    {
        std::uint64_t readBytes = 0;
        std::uint64_t writeBytes = 0;
        bool valid = false;
    };

    struct Sample
    {
        std::uint64_t elapsedMs = 0;

        double systemCpuPct = 0.0;
        double processCpuPct = 0.0;

        DWORD memoryLoadPct = 0;
        std::uint64_t totalPhysMb = 0;
        std::uint64_t availPhysMb = 0;

        std::uint64_t workingSetMb = 0;
        std::uint64_t privateUsageMb = 0;

        std::uint64_t processReadKbPerSec = 0;
        std::uint64_t processWriteKbPerSec = 0;

        std::wstring exeDrive;
        std::uint64_t exeDriveTotalGb = 0;
        std::uint64_t exeDriveFreeGb = 0;
        DWORD exeDriveFreePct = 0;

        std::wstring timeShiftDrive;
        std::uint64_t timeShiftDriveTotalGb = 0;
        std::uint64_t timeShiftDriveFreeGb = 0;
        DWORD timeShiftDriveFreePct = 0;

        bool idle = false;
        std::wstring idleReason;
    };

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    void OnCreate();
    void OnSize(int w, int h);
    void OnStartStop_();
    void OnManualSample_();
    void OnTimer_();

    void StartMonitor_();
    void ResetBaseline_();

    void LoadSettings_() noexcept;
    void SaveSettings_() noexcept;
    void ApplyConsoleLogCheckState_() noexcept;

    bool IsIdleSample_(const Sample& sample) const noexcept;
    std::wstring MakeIdleReason_(const Sample& sample) const;

    Sample MakeSample_();
    void UpdateView_(const Sample& sample);
    void AppendSampleLog_(const Sample& sample);
    void SetInfo_(std::wstring_view text);

    std::filesystem::path GetExeDir_() const;
    std::filesystem::path GetTimeShiftRoot_() const;
    std::filesystem::path MakeLogPath_() const;

    static HMENU CtrlId_(int id) noexcept;

    static std::uint64_t FileTimeToUInt64_(const FILETIME& ft) noexcept;
    static CpuTimes GetSystemCpuTimes_() noexcept;
    static std::uint64_t GetProcessCpuTime100ns_() noexcept;
    static IoSnapshot GetProcessIoSnapshot_() noexcept;
    static DWORD GetLogicalProcessorCount_() noexcept;

    static std::wstring FormatDouble_(double value);
    static std::wstring MakeDriveRoot_(const std::filesystem::path& path);

    static void ProbeDrive_(
        const std::filesystem::path& path,
        std::wstring& driveRoot,
        std::uint64_t& totalGb,
        std::uint64_t& freeGb,
        DWORD& freePct);

private:
    HINSTANCE hInst_ = nullptr;
    HWND hWnd_ = nullptr;
    HFONT hFont_ = nullptr;

    HWND hLblStatus_ = nullptr;
    HWND hBtnStartStop_ = nullptr;
    HWND hBtnSample_ = nullptr;
    HWND hChkConsoleLog_ = nullptr;
    HWND hInfo_ = nullptr;

    std::function<void(std::wstring_view)> log_;

    bool monitoring_ = false;
    bool consoleLogEnabled_ = true;

    DWORD intervalMs_ = 5000;
    DWORD logicalProcessors_ = 1;

    DWORD idleSystemCpuThresholdPct_ = 20;
    DWORD idleProcessCpuThresholdPct_ = 5;
    DWORD idleReadKbThreshold_ = 2048;
    DWORD idleWriteKbThreshold_ = 4096;
    DWORD idleMemoryLoadThresholdPct_ = 90;

    ULONGLONG lastTickMs_ = 0;
    CpuTimes lastCpu_{};
    std::uint64_t lastProcessCpu100ns_ = 0;
    IoSnapshot lastIo_{};
};

#endif // SYSTEM_RESOURCE_MONITOR_TAB_HPP