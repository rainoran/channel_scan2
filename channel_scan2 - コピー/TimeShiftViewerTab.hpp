#pragma once
#ifndef TIMESHIFT_VIEWER_TAB_HPP
#define TIMESHIFT_VIEWER_TAB_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <CommCtrl.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "AppWorkerPool.hpp"
#include "TimeShift_ViewIndex.hpp"
#include "TimeShiftProgramListWindow.hpp"

class TimeShiftViewerTab final
{
public:
    bool Create(
        HWND hParent,
        const RECT& rc,
        HINSTANCE hInst,
        HFONT hFont,
        std::function<void(std::wstring_view)>&& log,
        AppWorker::AppWorkerPool* workerPool = nullptr);

    void SetWorkerPool(AppWorker::AppWorkerPool* workerPool) noexcept;

    void Show(bool show);
    void Move(const RECT& rc);

    void ApplyToConfig();
    void ReloadFromConfig();

private:
    enum class SubPage : int
    {
        Program = 0,
        Status = 1,
        Detail = 2,
        Diagnostic = 3,
        Count = 4
    };

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    void OnCreate();
    void OnSize(int w, int h);
    void OnReload();

    void OnSubTabChanged();
    void OnChannelChanged();

    void OnRebuildProgramTable_();
    void RequestProgramTableRebuildIfNeeded_();
    void OnRebuildProgramTableDone_(LPARAM lp);
    void SetRebuildBusy_(bool busy, std::wstring_view text);

    void StartAutoRebuildTimer_(DWORD delayMs) noexcept;
    void StopAutoRebuildTimer_() noexcept;
    void OnAutoRebuildTimer_();

    void OnProgramWindowSwitch_();
    bool IsProgramWindowTopMostSwitchOn_() const noexcept;
    void SyncProgramListWindow_();

    void CopyProgramEntry_(
        const TimeShift_ViewIndex::ProgramEntry& program);

    std::vector<TimeShift_ViewIndex::ProgramSourceEntry>
        LoadProgramSourcesFromBeforeFolder_(
            const std::filesystem::path& beforeFolder) const;

    std::vector<TimeShift_ViewIndex::ProgramSourceEntry>
        LoadProgramSourcesFromEpgOut_(
            const std::filesystem::path& epgOutFolder) const;

    void CreateSubTabs_();
    void ShowSubPage_(SubPage page);
    RECT GetSubPageRect_() const;

    void RefreshCurrentSubPage_();
    void RefreshProgramTablePage_();
    void RefreshStorageStatusPage_();
    void RefreshDetailPage_();
    void RefreshDiagnosticPage_();

    void FillStatusView_();
    void FillChannels_();
    void FillSegments_();
    void FillDiagnostic_();

    void SetInfo_(std::wstring_view text);

    static HMENU CtrlId_(int id) noexcept;

    static std::wstring MakeChannelDisplayName_(int space, int channel);
    static std::wstring MakeUserStatusText_(const std::wstring& rawStatus);

    static std::wstring MakeRecordingRangeText_(
        const TimeShift_ViewIndex::ChannelEntry& ch);

    static std::wstring MakeSafeFileNamePart_(const std::wstring& text);

    std::filesystem::path MakeProgramCopyOutputPath_(
        const TimeShift_ViewIndex::ProgramEntry& program) const;

private:
    HINSTANCE hInst_ = nullptr;
    HWND hWnd_ = nullptr;
    HFONT hFont_ = nullptr;

    HWND hLblRoot_ = nullptr;
    HWND hBtnReload_ = nullptr;

    HWND hBtnOpenProgramWindow_ = nullptr;

    HWND hSubTab_ = nullptr;

    HWND hPageProgram_ = nullptr;
    HWND hPageStatus_ = nullptr;
    HWND hPageDetail_ = nullptr;
    HWND hPageDiagnostic_ = nullptr;

    HWND hProgramNotice_ = nullptr;

    HWND hListStatus_ = nullptr;

    HWND hListChannels_ = nullptr;
    HWND hListSegments_ = nullptr;

    HWND hInfo_ = nullptr;
    HWND hDiag_ = nullptr;

    std::function<void(std::wstring_view)> log_;

    AppWorker::AppWorkerPool* workerPool_ = nullptr;

    TimeShift_ViewIndex::BuildResult index_{};

    bool rebuildBusy_ = false;
    std::uint64_t rebuildRequestId_ = 0;

    bool hasLastAutoRebuildState_ = false;
    std::size_t lastAutoRebuildSegmentCount_ = 0;
    std::filesystem::file_time_type lastAutoRebuildEpgWrite_{};

    bool autoRebuildDeferredByEpgWrite_ = false;
    std::uint64_t autoRebuildEpgWriteSkipCount_ = 0;

    int selectedChannel_ = -1;

    SubPage currentPage_ = SubPage::Program;

    TimeShiftProgramListWindow programListWindow_{};
};

#endif // TIMESHIFT_VIEWER_TAB_HPP