#pragma once
#ifndef EIT_HTML_PROGRAM_TABLE_HPP
#define EIT_HTML_PROGRAM_TABLE_HPP

#include <string>
#include <chrono>

// 前段のローダーユニット
#include "EITTextDumpLoader.hpp"

// 旧版との互換用 (サービスリストを使わない単純版)
// epgDumpPath   : EpgTest_space0_ch15_EIT.epg など（UTF-8 テキスト）
// htmlOutputPath: 出力する HTML ファイルパス
bool GenerateEITHtmlProgramTable(
    const std::wstring& epgDumpPath,
    const std::wstring& htmlOutputPath);

// ★ 新: channellist_space0.ch を使ってサービスごとに整理して出力する版
//
// epgDumpPath      : EITTextDumpLoader で出力した .epg テキスト
// channelListPath  : channellist_space0.ch など（サービスリスト）
// htmlOutputPath   : 出力する HTML ファイルパス
bool GenerateEITHtmlProgramTable(
    const std::wstring& epgDumpPath,
    const std::wstring& channelListPath,
    const std::wstring& htmlOutputPath);

// 既存の簡易テスト (サービスリストを使わない)
void RunEITHtmlProgramTableTest(const std::wstring& epgDumpPath);

// ★ 新: サービスリスト付きテスト
void RunEITHtmlProgramTableTest_WithServiceList(
    const std::wstring& epgDumpPath,
    const std::wstring& channelListPath);

// ★ 追加: サービス＋時間順ソート結果を DebugLog に吐くだけのテスト
bool DumpEpgSortedByServiceAndTime(
    const std::wstring& epgDumpPath,
    const std::wstring& channelListPath);

#endif // EIT_HTML_PROGRAM_TABLE_HPP
