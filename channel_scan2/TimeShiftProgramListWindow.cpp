#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "TimeShiftProgramListWindow.hpp"

#include <algorithm>

#include "SIMDData.hpp"
#include "DebugManager.h"

namespace
{
    constexpr wchar_t kClassName[] = L"TSCEPG_TimeShiftProgramListWindow";

    constexpr int IDC_TSPL_FILTER = 8001;
    constexpr int IDC_TSPL_COPY = 8002;
    constexpr int IDC_TSPL_LIST = 8003;

    void SetFontIf_(HWND h, HFONT f)
    {
        if (h != nullptr && f != nullptr) {
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        }
    }

    void AddColumn_(HWND hList, int index, const wchar_t* text, int width)
    {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<LPWSTR>(text);
        col.cx = width;
        col.iSubItem = index;

        ListView_InsertColumn(hList, index, &col);
    }

    void SetSubItemText_(HWND hList, int row, int col, const std::wstring& text)
    {
        ListView_SetItemText(
            hList,
            row,
            col,
            const_cast<LPWSTR>(text.c_str()));
    }

    int MaxInt_(int a, int b) noexcept
    {
        return (a > b) ? a : b;
    }

    struct TimeShiftProgramScalarCount_
    {
        std::size_t possible = 0;
        std::size_t impossible = 0;
        std::size_t partial = 0;
    };

    bool IsPossibleProgramScalar_(
        const TimeShift_ViewIndex::ProgramEntry& program) noexcept
    {
        return
            program.status != L"Partial" &&
            program.viewAllowed &&
            program.copyAllowed;
    }

    bool IsPartialProgramScalar_(
        const TimeShift_ViewIndex::ProgramEntry& program) noexcept
    {
        return program.status == L"Partial";
    }

    TimeShiftProgramScalarCount_ CountProgramScalar_(
        const std::vector<TimeShift_ViewIndex::ProgramEntry>& programs)
    {
        TimeShiftProgramScalarCount_ out{};

        for (const auto& p : programs) {
            const bool possible =
                IsPossibleProgramScalar_(p);

            const bool partial =
                IsPartialProgramScalar_(p);

            if (possible) {
                ++out.possible;
            }
            else if (partial) {
                ++out.partial;
            }
            else {
                ++out.impossible;
            }
        }

        return out;
    }

    std::wstring MakeJoinPaddingText_(
        const TimeShift_ViewIndex::ProgramEntry& p)
    {
        std::wstring s;

        s += L"前";
        s += std::to_wstring(p.joinPaddingBeforeCount);
        s += L"/後";
        s += std::to_wstring(p.joinPaddingAfterCount);

        if (!p.joinPlanStartText.empty() ||
            !p.joinPlanEndText.empty()) {
            s += L" ";
            s += p.joinPlanStartText;
            s += L"～";
            s += p.joinPlanEndText;
        }

        return s;
    }

    std::size_t ExpectedVisibleCountScalarByFilterValue_(
        const TimeShiftProgramScalarCount_& count,
        int filterValue) noexcept
    {
        // TimeShiftProgramListWindow::ProgramFilter は private のため、
        // 匿名 namespace 側では enum 名を直接参照しない。
        //
        // ProgramFilter:
        //   Possible   = 0
        //   Impossible = 1
        //   Partial    = 2

        switch (filterValue) {
        case 0:
            return count.possible;

        case 1:
            return count.impossible;

        case 2:
            return count.partial;

        default:
            return count.possible;
        }
    }
}

bool TimeShiftProgramListWindow::Create(
    HWND hOwner,
    HINSTANCE hInst,
    HFONT hFont,
    CopyCallback&& copyCallback,
    std::function<void(std::wstring_view)> log)
{
    hOwner_ = hOwner;
    hInst_ = hInst;
    hFont_ = hFont;
    copyCallback_ = std::move(copyCallback);
    log_ = std::move(log);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TimeShiftProgramListWindow::WndProc;
    wc.hInstance = hInst_;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (RegisterClassExW(&wc) == 0) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
    }

    hWnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,
        kClassName,
        L"タイムシフト番組一覧",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        980,
        640,
        hOwner_,
        nullptr,
        hInst_,
        this);

    return hWnd_ != nullptr;
}

void TimeShiftProgramListWindow::Show()
{
    if (hWnd_ == nullptr) {
        return;
    }

    ShowWindow(hWnd_, SW_SHOW);
    SetForegroundWindow(hWnd_);
}

void TimeShiftProgramListWindow::ShowOrActivate(bool topMost)
{
    if (hWnd_ == nullptr) {
        return;
    }

    if (IsIconic(hWnd_) != FALSE) {
        ShowWindow(hWnd_, SW_RESTORE);
    }
    else if (IsWindowVisible(hWnd_) == FALSE) {
        ShowWindow(hWnd_, SW_SHOW);
    }
    else {
        ShowWindow(hWnd_, SW_SHOWNORMAL);
    }

    // topMost=true が渡ってきても、常時最前面にはしない。
    // 一瞬だけ前へ出したい場合も、最終状態は HWND_NOTOPMOST に戻す。
    if (topMost) {
        SetWindowPos(
            hWnd_,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE |
            SWP_NOSIZE |
            SWP_SHOWWINDOW);

        SetWindowPos(
            hWnd_,
            HWND_NOTOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE |
            SWP_NOSIZE |
            SWP_SHOWWINDOW);
    }
    else {
        SetWindowPos(
            hWnd_,
            HWND_TOP,
            0,
            0,
            0,
            0,
            SWP_NOMOVE |
            SWP_NOSIZE |
            SWP_SHOWWINDOW);
    }

    SetForegroundWindow(hWnd_);
    BringWindowToTop(hWnd_);
}

void TimeShiftProgramListWindow::SetTopMost(bool topMost)
{
    if (hWnd_ == nullptr) {
        return;
    }

    if (topMost) {
        // 常時最前面固定は使わない。
        // 互換用に true を受けても、前面へ一度出して通常ウィンドウへ戻す。
        SetWindowPos(
            hWnd_,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE |
            SWP_NOSIZE |
            SWP_SHOWWINDOW);

        SetWindowPos(
            hWnd_,
            HWND_NOTOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE |
            SWP_NOSIZE |
            SWP_SHOWWINDOW);

        return;
    }

    SetWindowPos(
        hWnd_,
        HWND_NOTOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE |
        SWP_NOSIZE |
        SWP_NOACTIVATE);
}

bool TimeShiftProgramListWindow::IsProgramListMinimized() const noexcept
{
    return hWnd_ != nullptr && ::IsIconic(hWnd_) != FALSE;
}

void TimeShiftProgramListWindow::Hide()
{
    if (hWnd_ != nullptr) {
        ShowWindow(hWnd_, SW_HIDE);
    }
}

void TimeShiftProgramListWindow::Destroy()
{
    if (hWnd_ != nullptr) {
        DestroyWindow(hWnd_);
        hWnd_ = nullptr;
    }
}

bool TimeShiftProgramListWindow::IsCreated() const noexcept
{
    return hWnd_ != nullptr;
}

bool TimeShiftProgramListWindow::IsVisible() const noexcept
{
    return hWnd_ != nullptr && IsWindowVisible(hWnd_) != FALSE;
}

void TimeShiftProgramListWindow::SetSnapshot(
    const TimeShift_ViewIndex::BuildResult& index)
{
    pendingIndex_ = index;
    hasPendingSnapshot_ = true;

    if (!busy_) {
        ApplyPendingSnapshotNow_();
        return;
    }

    SetInfo_(L"新しいタイムシフト番組一覧があります。\r\n再構築完了後に自動反映します。");
}

void TimeShiftProgramListWindow::SetBusy(
    bool busy,
    std::wstring_view text)
{
    busy_ = busy;

    if (hCbFilter_ != nullptr) {
        EnableWindow(hCbFilter_, busy ? FALSE : TRUE);
    }

    if (hBtnCopy_ != nullptr) {
        EnableWindow(hBtnCopy_, FALSE);
    }

    if (!busy_ && hasPendingSnapshot_) {
        ApplyPendingSnapshotNow_();
        return;
    }

    SetInfo_(text);
}

LRESULT CALLBACK TimeShiftProgramListWindow::WndProc(
    HWND hWnd,
    UINT msg,
    WPARAM wp,
    LPARAM lp)
{
    TimeShiftProgramListWindow* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<TimeShiftProgramListWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hWnd_ = hWnd;
    }
    else {
        self = reinterpret_cast<TimeShiftProgramListWindow*>(
            GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->HandleMessage(msg, wp, lp);
    }

    return DefWindowProcW(hWnd, msg, wp, lp);
}

LRESULT TimeShiftProgramListWindow::HandleMessage(
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

    case WM_CLOSE:
        OnClose();
        return 0;

    case WM_DESTROY:
        hWnd_ = nullptr;
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_TSPL_FILTER &&
            HIWORD(wp) == CBN_SELCHANGE) {
            OnFilterChanged_();
            return 0;
        }

        if (LOWORD(wp) == IDC_TSPL_COPY) {
            OnCopy_();
            return 0;
        }

        

        break;

    case WM_NOTIFY:
    {
        const auto* nm = reinterpret_cast<const NMHDR*>(lp);
        if (nm != nullptr &&
            nm->hwndFrom == hList_ &&
            nm->code == LVN_ITEMCHANGED) {
            OnProgramChanged_();
            return 0;
        }

        break;
    }

    default:
        break;
    }

    return DefWindowProcW(hWnd_, msg, wp, lp);
}

void TimeShiftProgramListWindow::OnCreate()
{
    hLblStatus_ = CreateWindowExW(
        0,
        L"STATIC",
        L"タイムシフト番組一覧",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        10,
        10,
        360,
        22,
        hWnd_,
        nullptr,
        hInst_,
        nullptr);

    hCbFilter_ = CreateWindowExW(
        0,
        WC_COMBOBOXW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
        CBS_DROPDOWNLIST | CBS_HASSTRINGS,
        380,
        8,
        160,
        160,
        hWnd_,
        CtrlId_(IDC_TSPL_FILTER),
        hInst_,
        nullptr);

    hBtnCopy_ = CreateWindowExW(
        0,
        L"BUTTON",
        L"選択番組をコピー",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        550,
        7,
        150,
        26,
        hWnd_,
        CtrlId_(IDC_TSPL_COPY),
        hInst_,
        nullptr);

    hList_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE |
        LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        10,
        42,
        940,
        480,
        hWnd_,
        CtrlId_(IDC_TSPL_LIST),
        hInst_,
        nullptr);

    hInfo_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE |
        ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        10,
        530,
        940,
        70,
        hWnd_,
        nullptr,
        hInst_,
        nullptr);

    SetFontIf_(hLblStatus_, hFont_);
    SetFontIf_(hCbFilter_, hFont_);
    SetFontIf_(hBtnCopy_, hFont_);
    SetFontIf_(hList_, hFont_);
    SetFontIf_(hInfo_, hFont_);

    ListView_SetExtendedListViewStyle(
        hList_,
        LVS_EX_FULLROWSELECT |
        LVS_EX_GRIDLINES |
        LVS_EX_DOUBLEBUFFER);

    AddColumn_(hList_, 0, L"チャンネル", 130);
    AddColumn_(hList_, 1, L"開始", 150);
    AddColumn_(hList_, 2, L"終了", 150);
    AddColumn_(hList_, 3, L"番組名", 300);
    AddColumn_(hList_, 4, L"保存率", 80);
    AddColumn_(hList_, 5, L"状態", 90);
    AddColumn_(hList_, 6, L"結合保護", 260);
    AddColumn_(hList_, 7, L"理由", 420);

    FillFilterCombo_();
    FillProgramView_();
    UpdateCopyButton_();
}

void TimeShiftProgramListWindow::OnSize(int w, int h)
{
    const int pad = 10;
    const int topH = 40;
    const int infoH = 82;

    if (hLblStatus_ != nullptr) {
        MoveWindow(hLblStatus_, pad, 10, 360, 22, TRUE);
    }

    if (hCbFilter_ != nullptr) {
        MoveWindow(hCbFilter_, 380, 8, 160, 160, TRUE);
    }

    if (hBtnCopy_ != nullptr) {
        MoveWindow(hBtnCopy_, 550, 7, 150, 26, TRUE);
    }

    const int listY = topH;
    const int listH = MaxInt_(80, h - topH - infoH - pad);

    if (hList_ != nullptr) {
        MoveWindow(
            hList_,
            pad,
            listY,
            MaxInt_(100, w - pad * 2),
            listH,
            TRUE);
    }

    if (hInfo_ != nullptr) {
        MoveWindow(
            hInfo_,
            pad,
            listY + listH + pad,
            MaxInt_(100, w - pad * 2),
            infoH - pad,
            TRUE);
    }
}

void TimeShiftProgramListWindow::OnClose()
{
    Hide();
}

void TimeShiftProgramListWindow::FillFilterCombo_()
{
    if (hCbFilter_ == nullptr) {
        return;
    }

    SendMessageW(hCbFilter_, CB_RESETCONTENT, 0, 0);

    for (int i = 0; i < static_cast<int>(ProgramFilter::Count); ++i) {
        const auto filter = static_cast<ProgramFilter>(i);

        const LRESULT idx =
            SendMessageW(
                hCbFilter_,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(GetFilterText_(filter)));

        if (idx >= 0) {
            SendMessageW(
                hCbFilter_,
                CB_SETITEMDATA,
                static_cast<WPARAM>(idx),
                static_cast<LPARAM>(i));
        }
    }

    SendMessageW(
        hCbFilter_,
        CB_SETCURSEL,
        static_cast<WPARAM>(ProgramFilter::Possible),
        0);

    filter_ = ProgramFilter::Possible;
}

TimeShiftProgramListWindow::ProgramFilter
TimeShiftProgramListWindow::GetFilter_() const noexcept
{
    if (hCbFilter_ == nullptr) {
        return filter_;
    }

    const LRESULT sel =
        SendMessageW(hCbFilter_, CB_GETCURSEL, 0, 0);

    if (sel < 0) {
        return filter_;
    }

    const LRESULT data =
        SendMessageW(
            hCbFilter_,
            CB_GETITEMDATA,
            static_cast<WPARAM>(sel),
            0);

    if (data < 0 ||
        data >= static_cast<LRESULT>(ProgramFilter::Count)) {
        return filter_;
    }

    return static_cast<ProgramFilter>(data);
}

bool TimeShiftProgramListWindow::IsPossibleProgram_(
    const TimeShift_ViewIndex::ProgramEntry& program) noexcept
{
    return
        program.status != L"Partial" &&
        program.viewAllowed &&
        program.copyAllowed;
}

bool TimeShiftProgramListWindow::IsPartialProgram_(
    const TimeShift_ViewIndex::ProgramEntry& program) noexcept
{
    return program.status == L"Partial";
}

bool TimeShiftProgramListWindow::ShouldShowProgram_(
    const TimeShift_ViewIndex::ProgramEntry& program) const noexcept
{
    const bool possible =
        IsPossibleProgram_(program);

    const bool partial =
        IsPartialProgram_(program);

    switch (filter_) {
    case ProgramFilter::Possible:
        return possible;

    case ProgramFilter::Impossible:
        return !possible && !partial;

    case ProgramFilter::Partial:
        return partial;

    default:
        return possible;
    }
}

void TimeShiftProgramListWindow::RebuildVisibleProgramIndexes_()
{
    visibleProgramIndexes_.clear();

    const TimeShiftProgramScalarCount_ scalarCount =
        CountProgramScalar_(index_.programs);

    const int filterValue =
        static_cast<int>(filter_);

    const std::size_t expectedVisible =
        ExpectedVisibleCountScalarByFilterValue_(
            scalarCount,
            filterValue);

    const SIMDData::SelectedPath selected =
        SIMDData::SelectPathFromConfig();

    switch (selected.actual) {
    case SIMDData::ExecPath::Avx2:
        RebuildVisibleProgramIndexesAvx2_();
        break;

    case SIMDData::ExecPath::Sse42:
        RebuildVisibleProgramIndexesSse42_();
        break;

    case SIMDData::ExecPath::Sse2:
        RebuildVisibleProgramIndexesSse2_();
        break;

    case SIMDData::ExecPath::Scalar:
    default:
        RebuildVisibleProgramIndexesScalar_();
        break;
    }

    const bool sumOk =
        scalarCount.possible +
        scalarCount.impossible +
        scalarCount.partial ==
        index_.programs.size();

    const bool visibleOk =
        expectedVisible == visibleProgramIndexes_.size();

    DebugLog(
        L"[TimeShiftProgramListWindow][ScalarCheck] programs=%zu possible=%zu impossible=%zu partial=%zu sumOk=%d filter=%ls expectedVisible=%zu actualVisible=%zu visibleOk=%d",
        index_.programs.size(),
        scalarCount.possible,
        scalarCount.impossible,
        scalarCount.partial,
        sumOk ? 1 : 0,
        GetFilterText_(filter_),
        expectedVisible,
        visibleProgramIndexes_.size(),
        visibleOk ? 1 : 0);

    DebugLog(
        L"[TimeShiftProgramListWindow][SIMD] filter=%ls requested=%ls actual=%ls fallback=%d programs=%zu visible=%zu reason=%ls",
        GetFilterText_(filter_),
        SIMDData::ToString(selected.requested),
        SIMDData::ToString(selected.actual),
        selected.fallback ? 1 : 0,
        index_.programs.size(),
        visibleProgramIndexes_.size(),
        selected.reason.c_str());
}

void TimeShiftProgramListWindow::RebuildVisibleProgramIndexesScalar_()
{
    visibleProgramIndexes_.clear();
    visibleProgramIndexes_.reserve(index_.programs.size());

    for (std::size_t i = 0; i < index_.programs.size(); ++i) {
        if (ShouldShowProgram_(index_.programs[i])) {
            visibleProgramIndexes_.push_back(i);
        }
    }
}

void TimeShiftProgramListWindow::RebuildVisibleProgramIndexesSse2_()
{
    // 入口だけ先に作る。
    // 実SIMD化前は Scalar と同じ結果にする。
    RebuildVisibleProgramIndexesScalar_();
}

void TimeShiftProgramListWindow::RebuildVisibleProgramIndexesSse42_()
{
    // 入口だけ先に作る。
    // 後で ProgramEntry の状態列を SIMDData 側へ渡して一括判定する。
    RebuildVisibleProgramIndexesScalar_();
}

void TimeShiftProgramListWindow::RebuildVisibleProgramIndexesAvx2_()
{
    // 入口だけ先に作る。
    // AVX2本体は別cpp / /arch:AVX2 側に分離する予定。
    RebuildVisibleProgramIndexesScalar_();
}

void TimeShiftProgramListWindow::FillProgramView_()
{
    if (hList_ == nullptr) {
        return;
    }

    ListView_DeleteAllItems(hList_);

    visibleProgramIndexes_.clear();
    selectedProgram_ = -1;

    if (busy_) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = 0;
        item.pszText = const_cast<LPWSTR>(L"再構築中");
        ListView_InsertItem(hList_, &item);
        UpdateCopyButton_();
        return;
    }

    if (!index_.ok) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = 0;
        item.pszText = const_cast<LPWSTR>(L"インデックス作成失敗");
        ListView_InsertItem(hList_, &item);
        SetSubItemText_(hList_, 0, 7, index_.reason);
        UpdateCopyButton_();
        return;
    }

    if (index_.programs.empty()) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = 0;
        item.pszText = const_cast<LPWSTR>(L"番組データ未構築");

        ListView_InsertItem(hList_, &item);

        SetSubItemText_(
            hList_,
            0,
            3,
            L"保存セグメントはありますが、EPGとの照合前なので番組一覧はまだありません。");

        SetSubItemText_(
            hList_,
            0,
            6,
            L"結合保護情報はありません。");

        SetSubItemText_(
            hList_,
            0,
            7,
            L"タイムシフトタブの更新、または一覧窓表示時に自動で .epg と保存セグメントを照合します。");

        SetInfo_(
            L"番組データが未構築です。\r\n"
            L"TimeShift_ViewIndex::Build(root) だけでは programs は作られません。\r\n"
            L"タブ更新または一覧窓表示時の自動再構築で、.epg と保存セグメントを照合します。");

        UpdateCopyButton_();
        return;
    }

    filter_ = GetFilter_();

    RebuildVisibleProgramIndexes_();

    if (visibleProgramIndexes_.empty()) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = 0;
        item.pszText = const_cast<LPWSTR>(L"該当番組なし");
        ListView_InsertItem(hList_, &item);

        std::wstring msg = L"表示条件: ";
        msg += GetFilterText_(filter_);
        SetSubItemText_(hList_, 0, 3, msg);

        UpdateCopyButton_();
        return;
    }

    int row = 0;

    for (const std::size_t programIndex : visibleProgramIndexes_) {
        const auto& p = index_.programs[programIndex];

        const std::wstring channelName =
            !p.serviceName.empty()
            ? p.serviceName
            : MakeChannelDisplayName_(p.space, p.channel);

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(channelName.c_str());

        ListView_InsertItem(hList_, &item);

        SetSubItemText_(hList_, row, 1, p.startText);
        SetSubItemText_(hList_, row, 2, p.endText);
        SetSubItemText_(hList_, row, 3, p.title);

        std::wstring coverage = std::to_wstring(p.coveragePermille);
        coverage += L"/1000";

        SetSubItemText_(hList_, row, 4, coverage);
        SetSubItemText_(hList_, row, 5, MakeProgramStatusText_(p));
        SetSubItemText_(hList_, row, 6, MakeJoinPaddingText_(p));
        SetSubItemText_(hList_, row, 7, p.reason);

        ++row;
    }

    if (!visibleProgramIndexes_.empty()) {
        selectedProgram_ =
            static_cast<int>(visibleProgramIndexes_.front());

        ListView_SetItemState(
            hList_,
            0,
            LVIS_SELECTED | LVIS_FOCUSED,
            LVIS_SELECTED | LVIS_FOCUSED);
    }

    std::wstring info = L"表示件数: ";
    info += std::to_wstring(visibleProgramIndexes_.size());
    info += L" / ";
    info += std::to_wstring(index_.programs.size());
    info += L"  条件: ";
    info += GetFilterText_(filter_);
    SetInfo_(info);

    UpdateCopyButton_();
}

void TimeShiftProgramListWindow::OnFilterChanged_()
{
    filter_ = GetFilter_();
    FillProgramView_();
}

void TimeShiftProgramListWindow::OnProgramChanged_()
{
    const int row = ListView_GetNextItem(
        hList_,
        -1,
        LVNI_SELECTED);

    if (row < 0 ||
        row >= static_cast<int>(visibleProgramIndexes_.size())) {
        selectedProgram_ = -1;
        UpdateCopyButton_();
        return;
    }

    selectedProgram_ =
        static_cast<int>(
            visibleProgramIndexes_[static_cast<std::size_t>(row)]);

    UpdateCopyButton_();

    if (selectedProgram_ < 0 ||
        selectedProgram_ >= static_cast<int>(index_.programs.size())) {
        return;
    }

    const auto& p =
        index_.programs[static_cast<std::size_t>(selectedProgram_)];

    std::wstring info;

    info += L"番組名: ";
    info += p.title;

    info += L"\r\n開始: ";
    info += p.startText;
    info += L"  終了: ";
    info += p.endText;

    info += L"\r\n状態: ";
    info += MakeProgramStatusText_(p);

    info += L"  保存率: ";
    info += std::to_wstring(p.coveragePermille);
    info += L"/1000";

    info += L"  本体秒数: ";
    info += std::to_wstring(p.durationSec);
    info += L"秒";

    info += L"  保存済み: ";
    info += std::to_wstring(p.coveredSec);
    info += L"秒";

    info += L"\r\n結合保護: ";
    info += MakeJoinPaddingText_(p);

    info += L"\r\n結合セグメント数: ";
    info += std::to_wstring(p.segmentCount);

    info += L"  結合サイズ: ";
    info += TimeShift_ViewIndex::FormatBytes(p.totalBytes);

    info += L"\r\n欠損秒数: ";
    info += std::to_wstring(p.missingSec);

    info += L"  最大欠損: ";
    info += std::to_wstring(p.maxGapSec);
    info += L"秒";

    if (!p.firstMissingStartText.empty() ||
        !p.firstMissingEndText.empty()) {
        info += L"\r\n最初の欠損: ";
        info += p.firstMissingStartText;
        info += L"～";
        info += p.firstMissingEndText;
    }

    info += L"\r\n境界確認: 前";
    info += std::to_wstring(p.boundaryProbeBeforeCount);
    info += L"/後";
    info += std::to_wstring(p.boundaryProbeAfterCount);

    info += L"  edgeGap=";
    info += p.boundaryProbeMayBeEdgeGap ? L"1" : L"0";

    if (!p.reason.empty()) {
        info += L"\r\n理由: ";
        info += p.reason;
    }

    SetInfo_(info);
}

void TimeShiftProgramListWindow::OnCopy_()
{
    if (selectedProgram_ < 0 ||
        selectedProgram_ >= static_cast<int>(index_.programs.size())) {
        SetInfo_(L"コピー対象の番組が選択されていません。");
        return;
    }

    const auto& p =
        index_.programs[static_cast<std::size_t>(selectedProgram_)];

    if (!p.copyAllowed) {
        SetInfo_(L"この番組は100%保存済みではないためコピーできません。");
        return;
    }

    if (copyCallback_) {
        copyCallback_(p);
    }
}

void TimeShiftProgramListWindow::UpdateCopyButton_()
{
    bool canCopy = false;

    if (!busy_ &&
        selectedProgram_ >= 0 &&
        selectedProgram_ < static_cast<int>(index_.programs.size())) {
        const auto& p =
            index_.programs[static_cast<std::size_t>(selectedProgram_)];

        canCopy = p.copyAllowed;
    }

    if (hBtnCopy_ != nullptr) {
        EnableWindow(hBtnCopy_, canCopy ? TRUE : FALSE);
    }
}

void TimeShiftProgramListWindow::SetInfo_(std::wstring_view text)
{
    if (hInfo_ == nullptr) {
        return;
    }

    std::wstring s(text);
    SetWindowTextW(hInfo_, s.c_str());
}

const wchar_t* TimeShiftProgramListWindow::GetFilterText_(
    ProgramFilter filter) noexcept
{
    switch (filter) {
    case ProgramFilter::Possible:
        return L"可能";

    case ProgramFilter::Impossible:
        return L"不可";

    case ProgramFilter::Partial:
        return L"欠損あり";

    default:
        return L"可能";
    }
}

const wchar_t* TimeShiftProgramListWindow::MakeProgramStatusText_(
    const TimeShift_ViewIndex::ProgramEntry& program) noexcept
{
    if (IsPartialProgram_(program)) {
        return L"欠損あり";
    }

    if (IsPossibleProgram_(program)) {
        return L"可能";
    }

    return L"不可";
}

HMENU TimeShiftProgramListWindow::CtrlId_(int id) noexcept
{
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

std::wstring TimeShiftProgramListWindow::MakeChannelDisplayName_(
    int space,
    int channel)
{
    std::wstring name = L"地デジ ch";
    name += std::to_wstring(channel);

    if (space != 0) {
        name = L"space";
        name += std::to_wstring(space);
        name += L" ch";
        name += std::to_wstring(channel);
    }

    return name;
}

void TimeShiftProgramListWindow::ApplyPendingSnapshotNow_()
{
    if (!hasPendingSnapshot_) {
        FillProgramView_();
        UpdateCopyButton_();
        return;
    }

    index_ = std::move(pendingIndex_);
    pendingIndex_ = TimeShift_ViewIndex::BuildResult{};
    hasPendingSnapshot_ = false;
    selectedProgram_ = -1;

    FillProgramView_();
    UpdateCopyButton_();

    std::wstring info = L"タイムシフト番組一覧を更新しました。";
    info += L"\r\nprograms=";
    info += std::to_wstring(index_.programs.size());
    info += L" segments=";
    info += std::to_wstring(index_.segments.size());

    SetInfo_(info);
}



