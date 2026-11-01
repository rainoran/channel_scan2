#pragma once
#ifndef EPG_MANUAL_TAB_HPP
#define EPG_MANUAL_TAB_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <functional>
#include <string>

#include "GuiActions.hpp"
#include "ConfigManager.h"

class EpgManualTab
{
public:
    bool Create(HWND hParent, const RECT& rc, HINSTANCE hInst, HFONT hFont,
        std::function<GuiActions()>&& getActions,
        std::function<void(std::wstring_view)>&& log);

    void Show(bool show);
    void Move(const RECT& rc);

    void NotifyActivated();
    void NotifyReservationDataChanged();
    void ApplyToConfig() {}
    void ReloadFromConfig();

    void SetBusyState(bool busy);
    bool IsBusy() const noexcept { return busy_; }

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    void OnCreate();
    void OnApplyDailyFullTime();
    void RefreshTaskListView_();
    void RebuildTaskListColumns_();
    void LayoutControls_(int clientWidth, int clientHeight);
    void ApplyBusyStateToControls_();

    int GetHour_() const;
    int GetMinute_() const;
    void SetHourMinute_(int hhmm);

    void ReloadHtmlRenderRouteSelection_();
    void SaveHtmlRenderRouteSelection_();
    ConfigManager::EpgHtmlRenderRoute GetSelectedHtmlRenderRoute_() const noexcept;

private:
    HINSTANCE hInst_ = nullptr;
    HWND hWnd_ = nullptr;
    HFONT hFont_ = nullptr;

    std::function<GuiActions()> getActions_;
    std::function<void(std::wstring_view)> log_;

    bool busy_ = false;

    HWND stTitle_ = nullptr;
    HWND btFullScan_ = nullptr;
    HWND btRealtime_ = nullptr;
    HWND btBuild_ = nullptr;
    HWND btStop_ = nullptr;

    HWND stDailyFull_ = nullptr;
    HWND edHour_ = nullptr;
    HWND udHour_ = nullptr;
    HWND stColon_ = nullptr;
    HWND edMin_ = nullptr;
    HWND udMin_ = nullptr;
    HWND btSetDailyFull_ = nullptr;

    HWND stHtmlRenderRoute_ = nullptr;
    HWND cbHtmlRenderRoute_ = nullptr;

    HWND stTaskList_ = nullptr;
    HWND btRefreshTasks_ = nullptr;
    HWND lvTasks_ = nullptr;
};

#endif // EPG_MANUAL_TAB_HPP