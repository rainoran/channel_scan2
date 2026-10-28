#pragma once
#ifndef EITTEXTWRITER_HPP
#define EITTEXTWRITER_HPP

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// ================================
// EIT Text Dump Writer (Header)
//  - ARIBデコード済みの文字列を保持する DumpEvent を定義
//  - 既存ダンプの読み込み→更新マージ→Snapshot 出力の宣言も提供
// ================================

// 拡張イベントの項目（1つ分）
struct EitDumpExtendedItem
{
    std::wstring item_description; // 項目名 (例: "出演者", "出演", "キャスト")
    std::wstring item_text;        // 項目内容
};

// EIT 1イベント分をテキストダンプに書き出すための構造体
// ・ARIBデコード済みの文字列を格納
// ・ジャンル/シリーズ/年齢制限など主要descriptorの情報も保持
struct EitDumpEvent
{
    std::uint8_t  table_id = 0;
    std::uint16_t service_id = 0;
    std::uint16_t event_id = 0;
    std::uint16_t transport_stream_id = 0;
    std::uint16_t original_network_id = 0;

    std::uint8_t  version_number = 0;
    std::uint8_t  section_number = 0;
    std::uint8_t  last_section_number = 0;

    // 開始時刻 (MJD + BCD hhmmss)
    std::uint16_t start_mjd = 0;
    std::uint8_t  start_time_bcd[3]{};   // hh, mm, ss (BCD)

    // 継続時間 (BCD hhmmss)
    std::uint8_t  duration_bcd[3]{};     // hh, mm, ss (BCD)

    std::uint8_t  running_status = 0;
    bool          free_ca_mode = false;

    std::wstring  short_event_name;  // [短形式タイトル]
    std::wstring  short_event_text;  // [短形式概要]

    // --- 0x4E: extended_event_descriptor ---
    std::wstring  extended_event_text;
    std::vector<EitDumpExtendedItem> extended_items;

    // --- 0x54: content_descriptor ---
    bool          has_genre = false;
    std::uint8_t  genre_primary = 0xFF;
    std::uint8_t  genre_secondary = 0xFF;
    std::uint8_t  user_nibble1 = 0xFF;
    std::uint8_t  user_nibble2 = 0xFF;

    // --- 0xD5: series_descriptor ---
    bool          has_series = false;
    std::uint16_t series_id = 0;
    std::uint8_t  repeat_label = 0;
    std::uint8_t  program_pattern = 0;
    bool          series_expire_valid = false;
    std::uint16_t series_expire_mjd = 0;
    std::uint16_t episode_number = 0;
    std::uint16_t last_episode_number = 0;
    std::wstring  series_name;

    // --- 0x55: parental_rating_descriptor ---
    bool          has_parental_rating = false;
    std::wstring  parental_country;
    std::uint8_t  parental_rating = 0;

    // 生の descriptor bytes（必要なら最後にそのままダンプする用）
    std::vector<std::uint8_t> descriptors;
};

// ------------------------------------------------------------
// 更新用オプション（Writer側で「更新マージ」して Snapshot 出力できるようにする）
// ------------------------------------------------------------
struct EitDumpUpdateOptions
{
    // 同一 logical（ONID/TSID/SID/EID）で start_time がズレた場合の差し替え許容
    std::uint32_t replaceStartTimeToleranceSec = 10u * 60u; // 10分

    // 差し替え時に duration が近いことを要求する（誤差し替え防止）
    bool requireDurationNearOnReplace = true;
    std::uint32_t replaceDurationToleranceSec = 60u;

    // 文字量/情報量が増えたら “改善版” とみなして反映
    bool allowUpgradeIfTextImproves = true;
    std::int64_t upgradeMinScoreGain = 32;

    // 拡張 items のマージ（ユニオン）
    bool mergeExtendedItems = true;
    std::size_t maxExtendedItems = 32;

    // start_time が不正(0等)の場合のキー緩和（既存互換用）
    bool relaxKeyWhenInvalidStartTime = true;
};

struct EitDumpUpdateStats
{
    std::uint64_t loadedExisting = 0;
    std::uint64_t inputNew = 0;

    std::uint64_t added = 0;
    std::uint64_t updated = 0;
    std::uint64_t replaced = 0;
    std::uint64_t unchanged = 0;

    std::uint64_t filledFields = 0;
    std::uint64_t mergedExtItems = 0;

    std::uint64_t finalCount = 0;
};

// ------------------------------------------------------------
// 既存：追記＋重複抑止（※更新反映には弱いので “初期用途” 想定）
// ------------------------------------------------------------
bool WriteEitDumpTextFile(
    const std::wstring& filePath,
    const std::vector<EitDumpEvent>& events,
    int space,
    int channel);

// ------------------------------------------------------------
// NEW：Snapshot（全件書き直し / 更新反映向け）
//  - filePath を trunc で開き、events 全件を出力する
// ------------------------------------------------------------
bool WriteEitDumpTextFileSnapshot(
    const std::wstring& filePath,
    const std::vector<EitDumpEvent>& events,
    int space,
    int channel);

// ParsedEITPacket からのラッパ。
// ★改良：既存 .epg を読み込み、新規と “更新マージ” した上で Snapshot 出力する
struct ParsedEITPacket;
namespace aribdecode { class AribDecodecallUnit; }

bool WriteEitDumpTextFileFromParsed(
    const std::wstring& filePath,
    const std::vector<ParsedEITPacket>& parsedEvents,
    int space,
    int channel,
    bool enableAribTrace
);

// NEW：更新マージのみ（Writerの外で使う用）
std::vector<EitDumpEvent> MergeEitDumpEventsForUpdate(
    const std::vector<EitDumpEvent>& existing,
    const std::vector<EitDumpEvent>& incoming,
    const EitDumpUpdateOptions& opt,
    EitDumpUpdateStats& outStats
);

#endif // EITTEXTWRITER_HPP
