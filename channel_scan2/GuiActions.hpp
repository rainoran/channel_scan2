#pragma once
#ifndef GUI_ACTIONS_HPP
#define GUI_ACTIONS_HPP

#include <cstdint>
#include <functional>
#include <string>

struct GuiActions
{
    // 後から core 側で set する（未接続なら empty）
    std::function<void(int space)> StartChannelScan;
    std::function<void()> StopTask;

    std::function<void()> StartEpgFullScan;
    std::function<void()> StartRealtimeEpgUpdate;
    std::function<void()> BuildEpg;
    std::function<void()> ReloadAndApplyTasks; // tasks.ini を再ロードして ApplyTaskList() する

    // ★追加：デバッグタブから RunFilterProcessingTest() を呼ぶ
    std::function<void()> RunChannelScanDebugFilterTest;

    // 手動EPG取得開始前チェック結果。
    // 実際の判定本体は TaskSystemConnector::CanStartManualEpgAcquisition() 側に置く。
    struct ManualEpgStartCheckResult
    {
        bool canStart = false;

        std::wstring reason;
        std::wstring conflictName;

        std::uint32_t conflictKind = 0;
        std::uint32_t conflictInstance = 0;

        int conflictYmd = 0;
        int conflictHHMM = -1;
    };

    // 手動EPG取得を開始してよいか確認する。
    //
    // estimatedMinutes:
    //   EPG取得にかかる想定時間。
    //
    // guardMinutes:
    //   番組予約前の安全余裕。
    //
    // 第1工程では、
    //   フルEPG / 手動EPG更新ともに 90 + 30 = 120分
    // で運用する。
    std::function<ManualEpgStartCheckResult(
        int estimatedMinutes,
        int guardMinutes)> CheckManualEpgStart;
};

#endif // GUI_ACTIONS_HPP