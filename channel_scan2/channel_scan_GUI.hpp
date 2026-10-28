#pragma once
#ifndef CHANNEL_SCAN_GUI_HPP
#define CHANNEL_SCAN_GUI_HPP

#include <Windows.h>
#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "IBonDriver2.h"
#include "FilterManager.hpp"
#include "GuiServiceSnapshot.hpp" // ★採用する前提

// FilterManagerMode を前方宣言（FilterManager.hpp で定義済み）
enum class FilterManagerMode;

// GUI呼び出し用オプション（最小）
struct ChannelScanGUIOptions
{
    // ---- Space 指定 ----
    int space = 0;                         // 単一 space の既定値
    bool scanAllSpaces = false;            // true: space を総当たり
    int maxSpaceToTry = 16;                // scanAllSpaces 時の探索上限
    std::vector<int> spacesToScan;         // これが非空なら scanAllSpaces/space より優先

    // ---- 受信/解析 ----
    int captureSec = 18;
    std::size_t ringBufferSize = 8192;

    std::size_t packetReserve = 4096;
    int sleepOnFailMs = 1;

    // GUI から直接 ON したい場合（config.ini の設定とも OR 結合される）
    bool enableTsDump = false;
    bool enableRingDump = false;

    bool enablePromptInConsole = false; // GUIからは false 推奨（wcin待ち回避）

    // ★追加：開始時に config.ini を必ず再読込（GUIで設定変更した直後に反映できる）
    bool reloadConfigAtStart = true;

    // ★追加：固定化した設定スナップショットを起動時に必ずログ出し
    bool dumpFixedConfigSnapshot = true;

    // ★追加：GUIへ “寿命固定済みのサービス一覧” を渡す（必要なときだけ設定）
    std::function<void(int space, int ch, const std::vector<GuiSnapshot::ServiceRow>&)> onSdtSnapshot;

    // ★追加：ステータス視覚化（チャンネル番号＋経過時間）
    std::function<void(const std::wstring& statusText)> onStatusText;

    // ★追加：処理開始/終了（タブロックなどに使う）
    std::function<void(bool running)> onRunningState;

    // ★追加：Win32表示向けにサービス名等を正規化する（埋め込みNUL対策）
    bool normalizeStringsForWin32 = true;

    // 追加: 外部停止要求
   // nullptr の場合は停止要求なし
    std::atomic_bool* cancelRequested = nullptr;
};

// RunFilterProcessingTest をほぼそのまま移植した本体
void channel_scan_GUI(IBonDriver2* bon, FilterManagerMode mode, const ChannelScanGUIOptions& opt);

// 互換：オプション省略（プロンプト無し / ダンプ無し）
void channel_scan_GUI(IBonDriver2* bon, FilterManagerMode mode);

// GUI から呼ぶ入口（オプション無し）
void channel_scan_start(IBonDriver2* bon, FilterManagerMode mode);

// GUI から呼ぶ入口（オプション有り）
void channel_scan_start(IBonDriver2* bon, FilterManagerMode mode, const ChannelScanGUIOptions& opt);

// 互換：mode省略
inline void channel_scan_start(IBonDriver2* bon)
{
    channel_scan_start(bon, FilterManagerMode::ChannelScan);
}

#endif // CHANNEL_SCAN_GUI_HPP
