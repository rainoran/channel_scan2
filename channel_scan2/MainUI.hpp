#pragma once
#ifndef MAIN_UI_HPP
#define MAIN_UI_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <CommCtrl.h>
#include <string>
#include <string_view>
#include <functional>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "GuiActions.hpp"
#include "TaskSystemConnector.hpp"
#include "TunerTaskGate.hpp"
#include "Reservation_ExecutionManager.hpp"
#include "bondriver_load.h"
#include "AppWorkerPool.hpp"
#include "TunerPortManager.hpp"

#include "TimeShift_Engine.hpp"

class SettingsTab;
class ChannelScanTab;
class EpgManualTab;
class GuiDebugLogPipeTab;
class ReservationSettingsTab;
class TunerSettingsTab;
class PluginTab;
class NetworkSettingsTab;
class TimeShiftViewerTab;
class SystemResourceMonitorTab;
class BonDriverLoader;
class IBonDriver2;

class MainUI
{
public:
    MainUI() = default;
    ~MainUI() noexcept;

    bool CreateAndShow(HINSTANCE hInst, int nCmdShow);
    void SetActions(const GuiActions& a) { actions = a; }

    void AppendLog(std::wstring_view text);

private:
    struct RegisteredPage
    {
        std::wstring title;
        bool isCore = true;
        bool created = false;

        std::function<bool(HWND, const RECT&)> create;
        std::function<void(bool)> show;
        std::function<void(const RECT&)> move;
        std::function<void()> apply;
        std::function<void()> reload;
        std::function<void()> onSelected;
    };

	// ウィンドウプロシージャとメッセージハンドラー
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

	// メッセージハンドラー
    void OnCreate();
    void OnSize(int w, int h);
    void OnTabChanged();

	// メインボタン（保存/キャンセル/終了）押下時の処理。id は CtrlId() で指定した値。
    void OnMainButton(int id);
    void SaveAllTabsToConfigAndFile();
    void ExitWithPrompt();
    void ExitWithoutSave();

	// タブコントロールのクライアント領域を取得する。タブの位置とサイズを考慮している。
    RECT GetTabPageRect() const;

    // 実行中など、保存・終了などの危険なUI操作をロックする必要があるときに呼ぶ。
// hTab 自体は無効化しない。
// EPG実行中のタブ切替は許可し、チャンネルスキャン等の強いロック時だけ
// WM_NOTIFY 側でタブ切替を戻す。
    void SetScanUiLocked(bool locked);
    void SetMainStatusText(std::wstring_view text);

	// ページの登録と管理
    void RegisterPage(RegisteredPage page);
    void RegisterCorePages();
    void RegisterPluginPages();
    void BuildPageRegistry();

	// 登録されたページをすべて作成する。失敗しても途中まで作成されたページはそのまま残る。
    bool CreateRegisteredPages();
    void MoveRegisteredPages(const RECT& rcLocal);
    void ShowCurrentRegisteredPage();
    void SyncDependentTabsFromConfig();

	// EPG自動取得タスク関連
    void InitGuiActions_();
    void StartEpgTask_(bool realtime, bool runReservationMaintenanceAfterBuild = false);
    void StopActiveTask_();
    void ReloadAndApplyTasks_();
    bool EnsureReservationTunersLoaded_();
    void ResetReservationTuners_() noexcept;
    bool AcquireReservationTuner_(
        int requestedTunerIndex,
        const TimeKeeper::TaskRequest& req,
        TunerPortManager::Lease& outLease) noexcept;
    bool GetReservationTunerLease_(
        std::uint32_t instance,
        TunerPortManager::Lease& outLease) noexcept;
    void ReleaseReservationTuner_(std::uint32_t instance) noexcept;

    void StartTimeShiftFromConfig_();
    void StopTimeShift_() noexcept;

private:
    HINSTANCE hInst = nullptr;
    HWND hWnd = nullptr;
    HWND hTab = nullptr;
    HWND hPageHost = nullptr;
    HWND hLog = nullptr;

    HWND hBtnSave = nullptr;
    HWND hBtnCancel = nullptr;
    HWND hBtnExit = nullptr;
    HWND hStatus = nullptr;

    HFONT hFont = nullptr;
    int currentTab = 0;

    SettingsTab* tabSettings = nullptr;
    TunerSettingsTab* tabTuners = nullptr;
    ChannelScanTab* tabChannelScan = nullptr;
    EpgManualTab* tabEpg = nullptr;
    TimeShiftViewerTab* tabTimeShiftViewer = nullptr;
    SystemResourceMonitorTab* tabResourceMonitor = nullptr;
    ReservationSettingsTab* tabReservation = nullptr;
	PluginTab* tabPlugin = nullptr;
    GuiDebugLogPipeTab* tabDebug = nullptr;
    NetworkSettingsTab* tabNetwork = nullptr;


    std::vector<RegisteredPage> registeredPages_;

    GuiActions actions{};

    bool saveConfigOnDestroy = true;
    bool scanUiLocked = false;
    bool pagesCreated_ = false;

    TaskSystemConnector taskSystem_{};
    Reservation_ExecutionManager reservationExecutionManager_{};
    TunerTaskHandle tunerHandle_{};

    TimeShift_Engine timeShiftEngine_{};
    AppWorker::AppWorkerPool workerPool_{};

    TunerPortManager tunerPortManager_{};
    std::mutex reservationLeaseMutex_{};
    std::unordered_map<std::uint32_t, TunerPortManager::Lease>
        reservationTunerLeases_{};

    void SetEpgTabBusyState_(bool busy);
};

#endif // MAIN_UI_HPP
