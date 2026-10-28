#ifndef EPG_BUILDER_HPP
#define EPG_BUILDER_HPP

#include <cstdint>
#include <vector>
#include <map>

#include "EpgModel.hpp"
#include "FilterManager.hpp"
#include "EITEventSortUnit.hpp"
#include "AribDecodecallUnit.hpp"
#include "DebugManager.h"

#ifdef ENABLE_EPG_EIT_DUMP_DEBUG
extern bool g_EnableEitDumpDebug;
void SetEitDumpDebug(bool enable);
#else
// マクロ無効時はダミー
inline void SetEitDumpDebug(bool) {}
#endif

// -------------------------------------------------------------------------
// EPG 構築時のデコードオプション。
// まずは [拡張items]（extended_text）の扱いをフラグで切り替えられるようにする。
// -------------------------------------------------------------------------
struct EpgBuildOptions
{
    // 短形式タイトル／短形式概要をデコードするかどうか。
    // 現状は常に true 運用の想定だが、将来のために用意しておく。
    bool decodeShortText = true;

    // 拡張テキスト（[拡張items]）をデコードするかどうか。
    // デフォルト false = EPG構築では extended_text を無視する。
    bool decodeExtendedText = false;
};

// -------------------------------------------------------------------------
// FilterManager が持っている EIT イベント列（ParsedEITPacket 群）と
// AribDecodecallUnit（ARIB → std::wstring デコーダ）を使って
// メモリ上の EPG テーブル（EpgTable）を構築するユニット。
// -------------------------------------------------------------------------

// FilterManager 内部の EIT 結果から EPG テーブルを構築する。
// - EITEventSortUnit::BuildUniqueSortedEvents() で重複排除＆ソート
// - start_time (MJD+BCD) を年/月/日/時/分に展開（ローカル時刻として格納）
// - タイトル／概要／拡張テキストは AribDecodecallUnit で std::wstring に変換
//
// ※現段階では SDT 由来の service_name はまだ埋めていない。
//   ServiceInfo の定義と合わせて、後で service_name を補完する予定。
EpgTable BuildEpgTableFromFilters(
    const FilterManager& fm,
    aribdecode::AribDecodecallUnit& decoder,
    const EpgBuildOptions& options = EpgBuildOptions{});

#endif // EPG_BUILDER_HPP
