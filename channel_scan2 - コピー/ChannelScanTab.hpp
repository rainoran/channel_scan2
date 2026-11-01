#pragma once
#ifndef CHANNEL_SCAN_TAB_HPP
#define CHANNEL_SCAN_TAB_HPP

#include <Windows.h>
#include <string>
#include <string_view>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <thread>
#include <atomic>

#include "GuiActions.hpp"

// ChannelScanTab
// - 上部: space選択 + Start/Stop + ステータス
// - 下部: 「サービス一覧（チェック=EPG対象）」を 1枠(ListView)に統合表示
// - space切替で channellist_spaceN.ch を読み直す
// - チェック変更は config.ini に保存（ConfigManager の iniPath を利用）
class ChannelScanTab
{
public:
    bool Create(HWND hParent, const RECT& rc, HINSTANCE hInst, HFONT hFont,
        std::function<GuiActions()>&& getActions,
        std::function<void(std::wstring_view)>&& log);

    void Show(bool show);
    void Move(const RECT& rc);

    void ApplyToConfig();
    void ReloadFromConfig();

    void ClearResults();
    void SetRunning(bool running);
    void SetStatusText(std::wstring_view s);

    void AddReceivableChannelRow(
        int space, int ch, std::wstring_view chName,
        float signalLevelDb,
        std::size_t pat, std::size_t pmt, std::size_t sdt, std::size_t nit);

    void PostClearResults();
    void PostSetRunning(bool running);
    void PostSetStatusText(std::wstring text);
    void PostAddReceivableChannelRow(
        int space, int ch, std::wstring chName,
        float signalLevelDb,
        std::size_t pat, std::size_t pmt, std::size_t sdt, std::size_t nit);

    HWND GetHwnd() const noexcept { return hWnd; }

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    struct ServiceRow;

    void OnCreate();
    void OnSize(int w, int h);

    void LoadServiceListForSelectedSpace();
    void LoadServiceList(int space);
    void ClearServiceList();

    void OnServiceListItemChanged(int item, UINT oldState, UINT newState);

    bool ReadServiceEnabledFromIni(int space, std::uint16_t onid, std::uint16_t tsid, std::uint16_t sid, bool fallbackDefault) const;
    void WriteServiceEnabledToIni(int space,
        std::uint16_t onid, std::uint16_t tsid, std::uint16_t sid,
        bool enabled,
        int ch, int serviceType, int freeCA, int remoteKeyId,
        const std::wstring& serviceName,
        const std::wstring& provider,
        const std::wstring& networkName);

    int GetSelectedSpaceIndex() const;
    bool MakeInitialServiceEnabledDefault(const ServiceRow& r) const;
    void SaveServiceRowEnabledToIni(const ServiceRow& r);
    void SaveAllServiceRowsEnabledToIni(bool rewriteSpaceSection);

    bool TryStartConnectedScan(int space);
    bool TryStartSelfHostedScan(int space);
    void RequestStopSelfHostedScan();
    void JoinSelfHostedScanIfNeeded();

    static std::uint32_t MakeChannelKey(int space, int ch) noexcept {
        return (static_cast<std::uint32_t>(space) << 16) | (static_cast<std::uint32_t>(ch) & 0xFFFFu);
    }

private:
    enum : UINT {
        WMU_CLEAR_RESULTS = WM_APP + 600,
        WMU_SET_RUNNING = WM_APP + 601,
        WMU_SET_STATUS = WM_APP + 602,
        WMU_ADD_ROW = WM_APP + 603,
    };

    struct StatusPayload {
        std::wstring text;
    };

    struct RowPayload {
        int space = 0;
        int ch = 0;
        std::wstring chName;
        float signalLevelDb = 0.0f;
        std::size_t pat = 0, pmt = 0, sdt = 0, nit = 0;
    };

    struct ChannelStat {
        float signalLevelDb = 0.0f;
        std::size_t pat = 0, pmt = 0, sdt = 0, nit = 0;
        bool has = false;
    };

    struct ServiceRow {
        bool enabled = true;

        std::wstring serviceName;
        int space = 0;
        int ch = 0;
        int serviceType = 0;

        std::uint16_t serviceId = 0;
        std::uint16_t networkId = 0;
        std::uint16_t tsid = 0;
        std::uint16_t onid = 0;

        int freeCA = 0;
        std::wstring provider;
        std::wstring networkName;
        int remoteControlKeyId = 0;
    };

private:
    HINSTANCE hInst = nullptr;
    HWND hWnd = nullptr;
    HFONT hFont = nullptr;

    std::function<GuiActions()> getActions;
    std::function<void(std::wstring_view)> log;

    HWND cbSpace = nullptr;
    HWND btStart = nullptr;
    HWND btStop = nullptr;
    HWND stStatus = nullptr;
    HWND gbServices = nullptr;
    HWND lvServices = nullptr;

    bool isRunning = false;
    bool suppressServiceNotify = false;

    std::vector<ServiceRow> serviceRows;
    std::unordered_map<std::uint32_t, std::vector<int>> channelToRowIndices;
    std::unordered_map<std::uint32_t, ChannelStat> channelStats;

    // MainUI 依存が切れてもタブ単体で起動できる保険経路
    std::thread selfHostedScanThread;
    std::atomic_bool selfHostedScanActive{ false };
    std::atomic_bool selfHostedStopRequested{ false };
};

#endif // CHANNEL_SCAN_TAB_HPP