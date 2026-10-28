#pragma once
#pragma once
#ifndef EPG_HTML_WRITER_HPP
#define EPG_HTML_WRITER_HPP

#include <string>
#include <vector>

#include "EpgModel.hpp"      // EpgTable, EpgEvent, MakeServiceKey
#include "channellist.hpp"   // ChannelListEntry
#include "EncodingConfig.hpp" 
#include "DebugManager.h"


// ★ イベント詳細デバッグのオン/オフスイッチ
//   0: 無効（通常運用）
//   1: 有効（対象イベントを詳細ログ出力）
#ifndef EPG_HTML_DEBUG_VERBOSE
#define EPG_HTML_DEBUG_VERBOSE 1
#endif

// ★ 拡張テキスト([拡張items] / extended_text) を HTML に出すかどうか
//   0: 出さない（現状方針）
//   1: short_text に続けて extended_text も出力する
#ifndef EPG_HTML_ENABLE_EXTENDED_TEXT
#define EPG_HTML_ENABLE_EXTENDED_TEXT 0
#endif

// channellist.ch から読み込んだチャンネルリストと、
// メモリ上の EPG テーブルを元に、簡易な HTML 番組表を出力する。
// - チャンネルの表示順は channellist.ch の並び（物理チャンネル順）
// - 各チャンネルごとに <h2> + <table> で「時間順の番組一覧」を出力
// - 時刻は「YYYY/MM/DD HH:MM - HH:MM」形式で表示
//
// 戻り値: 成功時 true / 失敗時 false
bool WriteEpgHtmlSimple(
    const std::wstring& filePath,
    const EpgTable& epg,
    const std::vector<ChannelListEntry>& channels);

// 新 API：エンコード指定版
bool WriteEpgHtmlSimple(
    const std::wstring& filePath,
    const EpgTable& epg,
    const std::vector<ChannelListEntry>& channels,
    TextEncoding encoding);

#endif // EPG_HTML_WRITER_HPP
