#pragma once
#ifndef EPG_MODEL_HPP
#define EPG_MODEL_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <map>

struct EpgEvent
{
    // 識別用
    uint16_t original_network_id = 0;
    uint16_t transport_stream_id = 0;
    uint16_t service_id = 0;
    uint16_t event_id = 0;

    // 時刻（ローカル時刻にしておく）
    int year = 0, month = 0, day = 0;
    int start_hour = 0, start_minute = 0;
    int duration_minutes = 0;

    // 表示用文字列（全部 std::wstring）
    std::wstring service_name;    // channellist から引く
    std::wstring event_name;      // short_event_name
    std::wstring short_text;      // short_event_text
    std::wstring extended_text;   // extended_event_text

    // --- ここから追加 ---

    // 0x54: content_descriptor → ジャンル
    bool     has_genre = false;
    uint8_t  genre_primary = 0xFF; // content_nibble_level1
    uint8_t  genre_secondary = 0xFF; // content_nibble_level2
    uint8_t  user_nibble1 = 0xFF; // user_nibble1
    uint8_t  user_nibble2 = 0xFF; // user_nibble2

    // 0xD5: series_descriptor → シリーズ情報
    bool     has_series = false;
    uint16_t series_id = 0;
    uint8_t  episode_number = 0;  // 話数
    uint8_t  last_episode_number = 0;  // 総話数
    std::wstring series_name;          // シリーズ名（デコード済み）

    // 0x55: parental_rating_descriptor → 年齢制限
    bool     has_parental_rating = false;
    uint8_t  parental_rating = 0;  // 生の rating コード（録画フィルタ用）
};


/// サービスごとの番組表
struct EpgServiceSchedule
{
    std::wstring service_name;
    std::vector<EpgEvent> events; // 開始時刻順に並べる
};

/// (ONID/TSID/SID) → schedule
using EpgTable = std::map<uint64_t, EpgServiceSchedule>;

inline uint64_t MakeServiceKey(
    uint16_t onid, uint16_t tsid, uint16_t sid)
{
    return (static_cast<uint64_t>(onid) << 32)
        | (static_cast<uint64_t>(tsid) << 16)
        | static_cast<uint64_t>(sid);
}

struct GenreEntry
{
    uint8_t primary;
    uint8_t secondary;
    const wchar_t* label;
};

// ★暫定版ジャンルテーブル（必要に応じてここを拡張する）
inline constexpr GenreEntry kGenreTable[] = {
    // 0x0x ニュース／報道
    {0x0, 0x0, L"ニュース／報道(定時・総合)"},
    {0x0, 0x1, L"ニュース／報道(天気)"},
    {0x0, 0x2, L"ニュース／報道(特集・ドキュメンタリー)"},

    // 0x1x スポーツ
    {0x1, 0x0, L"スポーツ(ニュース・中継)"},
    {0x1, 0x1, L"スポーツ(野球)"},
    {0x1, 0x2, L"スポーツ(サッカー)"},
    {0x1, 0x3, L"スポーツ(ゴルフ)"},
    {0x1, 0x4, L"スポーツ(その他競技)"},

    // 0x2x 情報／ワイドショー
    {0x2, 0x0, L"情報／ワイドショー(総合)"},
    {0x2, 0x1, L"情報／ワイドショー(天気・気象)"},
    {0x2, 0x2, L"情報／ワイドショー(経済・市況)"},

    // 0x3x ドラマ
    {0x3, 0x0, L"ドラマ(国内ドラマ)"},
    {0x3, 0x1, L"ドラマ(海外ドラマ)"},

    // 0x7x アニメ／特撮
    {0x7, 0x0, L"アニメ／特撮(国内)"},
    {0x7, 0x1, L"アニメ／特撮(海外)"},
};


// --- 追加: ジャンル / 年齢制限のラベル化ヘルパ ---

inline std::wstring GetGenrePrimaryLabel(uint8_t primary)
{
    switch (primary) {
    case 0x0:  return L"ニュース／報道";
    case 0x1:  return L"スポーツ";
    case 0x2:  return L"情報／ワイドショー";
    case 0x3:  return L"ドラマ";
    case 0x4:  return L"音楽";
    case 0x5:  return L"バラエティ";
    case 0x6:  return L"映画";
    case 0x7:  return L"アニメ／特撮";
    case 0x8:  return L"ドキュメンタリー／教養";
    case 0x9:  return L"劇場／公演";
    case 0xA:  return L"趣味／教育";
    case 0xB:  return L"福祉";
    default:   return L"その他";
    }
}

inline std::wstring GetGenreLabel(const EpgEvent& ev)
{
    if (!ev.has_genre) {
        return L""; // ジャンル情報なし
    }

    // まず primary+secondary の組み合わせでテーブルを検索
    for (const auto& g : kGenreTable) {
        if (g.primary == ev.genre_primary && g.secondary == ev.genre_secondary) {
            return std::wstring(g.label);
        }
    }

    // 見つからなければ primary のみのラベルにフォールバック
    return GetGenrePrimaryLabel(ev.genre_primary);
}

inline std::wstring GetParentalRatingLabel(const EpgEvent& ev)
{
    if (!ev.has_parental_rating) {
        return L""; // 情報なし
    }

    switch (ev.parental_rating) {
    case 0x00:
        return L"年齢制限なし";
    default:
        // 詳細なマッピングは後で詰める前提で、まずはコード表示
        return L"レーティングコード " + std::to_wstring(ev.parental_rating);
    }
}

#endif // EPG_MODEL_HPP
