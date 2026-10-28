#pragma once
#ifndef TIMESHIFT_PROGRAM_LIST_WINDOW_HPP
#define TIMESHIFT_PROGRAM_LIST_WINDOW_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <CommCtrl.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "TimeShift_ViewIndex.hpp"

class TimeShiftProgramListWindow final
{
public:
    using CopyCallback =
        std::function<void(const TimeShift_ViewIndex::ProgramEntry&)>;

    bool Create(
        HWND hOwner,
        HINSTANCE hInst,
        HFONT hFont,
        CopyCallback&& copyCallback,
        std::function<void(std::wstring_view)> log);

    void Show();
    void Hide();
    void Destroy();

    void ShowOrActivate(bool topMost);
    void SetTopMost(bool topMost);
    bool IsProgramListMinimized() const noexcept;

    bool IsCreated() const noexcept;
    bool IsVisible() const noexcept;

    void SetSnapshot(const TimeShift_ViewIndex::BuildResult& index);
    void SetBusy(bool busy, std::wstring_view text);

private:
    enum class ProgramFilter : int
    {
        Possible = 0,
        Impossible = 1,
        Partial = 2,
        Count = 3
    };

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    void OnCreate();
    void OnSize(int w, int h);
    void OnClose();
    void OnFilterChanged_();
    void OnProgramChanged_();
    void OnCopy_();

    void FillFilterCombo_();
    void FillProgramView_();
    void UpdateCopyButton_();
    void ApplyPendingSnapshotNow_();
    void SetInfo_(std::wstring_view text);

    void RebuildVisibleProgramIndexes_();

    void RebuildVisibleProgramIndexesScalar_();
    void RebuildVisibleProgramIndexesSse2_();
    void RebuildVisibleProgramIndexesSse42_();
    void RebuildVisibleProgramIndexesAvx2_();

    ProgramFilter GetFilter_() const noexcept;

    bool ShouldShowProgram_(
        const TimeShift_ViewIndex::ProgramEntry& program) const noexcept;

    static bool IsPossibleProgram_(
        const TimeShift_ViewIndex::ProgramEntry& program) noexcept;

    static bool IsPartialProgram_(
        const TimeShift_ViewIndex::ProgramEntry& program) noexcept;

    static const wchar_t* GetFilterText_(ProgramFilter filter) noexcept;

    static const wchar_t* MakeProgramStatusText_(
        const TimeShift_ViewIndex::ProgramEntry& program) noexcept;

    static HMENU CtrlId_(int id) noexcept;

    static std::wstring MakeChannelDisplayName_(int space, int channel);

private:
    HWND hOwner_ = nullptr;
    HWND hWnd_ = nullptr;
    HINSTANCE hInst_ = nullptr;
    HFONT hFont_ = nullptr;

    HWND hLblStatus_ = nullptr;
    HWND hCbFilter_ = nullptr;
    HWND hBtnCopy_ = nullptr;
    HWND hList_ = nullptr;
    HWND hInfo_ = nullptr;

    TimeShift_ViewIndex::BuildResult index_{};
    TimeShift_ViewIndex::BuildResult pendingIndex_{};
    bool hasPendingSnapshot_ = false;

    std::vector<std::size_t> visibleProgramIndexes_{};

    ProgramFilter filter_ = ProgramFilter::Possible;
    int selectedProgram_ = -1;

    bool busy_ = false;

    CopyCallback copyCallback_;
    std::function<void(std::wstring_view)> log_;
};

#endif // TIMESHIFT_PROGRAM_LIST_WINDOW_HPP