#pragma once
#ifndef TS_FILTER_TEST_H
#define TS_FILTER_TEST_H

#include "IBonDriver2.h"
#include "FilterManager.hpp"

// FilterManagerMode を前方宣言（FilterManager.hpp で定義済み）
enum class FilterManagerMode;

// メインのフィルター動作検証
void RunFilterProcessingTest(IBonDriver2* bonDriver, FilterManagerMode mode);
void RunEpgDataTest(IBonDriver2* bon, FilterManagerMode mode);

// ★ 互換用ラッパー（既存コードを壊さない）
inline void RunFilterProcessingTest(IBonDriver2* bonDriver) {
    RunFilterProcessingTest(bonDriver, FilterManagerMode::ChannelScan);
}
inline void RunEpgDataTest(IBonDriver2* bon) {
    RunEpgDataTest(bon, FilterManagerMode::EpgFullScan);
}

// 番組表構築テスト（更新機能なし／あり）
void RunEpgBuildTest(IBonDriver2* bon, bool enableUpdate);

// ★ channellist_spaceX.ch を使って EIT を取得するテスト
void RunEpgDataTest_FromChannelList(IBonDriver2* bon, FilterManagerMode mode, int space);

// ★ デフォルトで EpgFullScan モードのラッパー
inline void RunEpgDataTest_FromChannelList(IBonDriver2* bon, int space) {
    RunEpgDataTest_FromChannelList(bon, FilterManagerMode::EpgFullScan, space);
}

// ===== ★追加：TDT/TOT テスト =====
// PID 0x0014（TDT=0x70 / TOT=0x73）を一定時間受信して、最終時刻を出力する。
// space は地上波なら 0 を想定。captureSec は受信秒数。
void RunTdtTotTest(IBonDriver2* bon, int space, int captureSec);

// 互換ラッパ（地上波 space=0 を 8秒）
inline void RunTdtTotTest(IBonDriver2* bon) {
    RunTdtTotTest(bon, 0, 8);
}

// EPGダンプパースチェック
void RunEpgDumpParseCheck();

// EPGソートデバッグテスト
void RunEpgSortDebugTest();

// EPG HTML 生成テスト
void RunEpgHtmlTest();

// EPG構築テスト（更新機能付き／なし、FilterManagerMode指定版）
void RunEpgBuildTest2(IBonDriver2* bon, FilterManagerMode mode, bool enableUpdate);

// CDTフィルタ動作テスト
void RunCdtTest(IBonDriver2* bon, int space, int captureSec);

// CAT/BIT フィルターテスト（簡易）
void RunCatBitTest(IBonDriver2* bon, int space, int captureSec);

// RST フィルターテスト
void RunRstTest(IBonDriver2* bon, int space, int captureSec);

// TSDT/DIT/SIT フィルタースキャンテスト
void RunTsdtDitSitScanTest(IBonDriver2* bon);
#endif // TS_FILTER_TEST_H
