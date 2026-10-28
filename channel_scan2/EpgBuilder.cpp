#include "EpgBuilder.hpp"
#include "DebugManager.h"  // ★ デバッグログ用

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>

// ★ 詳細デバッグログ（1イベントごとの中身確認）を有効にするか
//   0: 無効（通常運用） / 1: 有効（先頭数イベントだけ詳細ログ）
#if !defined(EPG_BUILDER_DEBUG_VERBOSE)
#define EPG_BUILDER_DEBUG_VERBOSE 1
#endif

// 無名名前空間にローカルヘルパをまとめる
namespace {

    // 1byte BCD → 0〜99
    inline int BcdToInt(std::uint8_t b) noexcept
    {
        return ((b >> 4) & 0x0F) * 10 + (b & 0x0F);
    }

    // MJD → 年月日（グレゴリオ暦）変換
    inline void DecodeMJD(int mjd, int& year, int& month, int& day) noexcept
    {
        // TOTFilter と同等の定番アルゴリズム
        int jd = mjd + 2400001 + 68569;
        int c = 4 * jd / 146097;
        jd = jd - (146097 * c + 3) / 4;
        int e = 4000 * (jd + 1) / 1461001;
        jd = jd - 1461 * e / 4 + 31;
        int f = 80 * jd / 2447;
        day = jd - 2447 * f / 80;
        jd = f / 11;
        month = f + 2 - 12 * jd;
        year = 100 * (c - 49) + e + jd;
    }

    struct DecodedDateTime {
        int year = 0;
        int month = 0;
        int day = 0;
        int hour = 0;
        int min = 0;
        int sec = 0;
        bool valid = false;
    };

    // ParsedEITPacket::start_time (40bit 生データ) → 日付＋時刻
    inline DecodedDateTime DecodeEitStartTime(std::uint64_t start_time_raw) noexcept
    {
        DecodedDateTime dt{};

        // 未定義時刻（全ビット1など）は 0 初期値のまま返す
        if (start_time_raw == 0 || start_time_raw == 0xFFFFFFFFFFull) {
            return dt;  // dt.valid == false のまま
        }

        std::array<std::uint8_t, 5> buf{};

        // 上位から順に 5byte 抽出
        for (int i = 4; i >= 0; --i) {
            buf[i] = static_cast<std::uint8_t>(start_time_raw & 0xFFu);
            start_time_raw >>= 8;
        }

        const int mjd = (static_cast<int>(buf[0]) << 8) | buf[1];
        DecodeMJD(mjd, dt.year, dt.month, dt.day);

        dt.hour = BcdToInt(buf[2]);
        dt.min = BcdToInt(buf[3]);
        dt.sec = BcdToInt(buf[4]);

        // ★ 有効フラグを立てる
        dt.valid = true;
        return dt;
    }

    // ParsedEITPacket::descriptors から EpgEvent の
    // ジャンル / シリーズ / 年齢制限を埋める
    void FillGenreSeriesParentalFromDescriptors(
        const std::vector<std::uint8_t>& descBuf,
        aribdecode::AribDecodecallUnit& decoder,
        EpgEvent& out
    )
    {
        std::size_t pos = 0;
        while (pos + 2 <= descBuf.size()) {
            const std::uint8_t tag = descBuf[pos];
            const std::uint8_t len = descBuf[pos + 1];

            if (pos + 2u + len > descBuf.size()) {
                break; // 破損防止
            }

            const std::uint8_t* body = descBuf.data() + pos + 2;
            const std::size_t   dlen = len;

            // --- 0x54: content_descriptor → ジャンル ---
            if (tag == 0x54 && !out.has_genre && dlen >= 2) {
                const std::uint8_t* p = body;
                const std::uint8_t* endp = body + dlen;

                while (p + 1 < endp) {
                    const std::uint8_t c = p[0];
                    const std::uint8_t u = p[1];

                    const std::uint8_t level1 = static_cast<std::uint8_t>((c >> 4) & 0x0F);
                    const std::uint8_t level2 = static_cast<std::uint8_t>(c & 0x0F);
                    const std::uint8_t user1 = static_cast<std::uint8_t>((u >> 4) & 0x0F);
                    const std::uint8_t user2 = static_cast<std::uint8_t>(u & 0x0F);

                    out.has_genre = true;
                    out.genre_primary = level1;
                    out.genre_secondary = level2;
                    out.user_nibble1 = user1;
                    out.user_nibble2 = user2;
                    break; // 最初のペアだけ採用
                }
            }

            // --- 0xD5: series_descriptor → シリーズ情報 ---
            if (tag == 0xD5 && !out.has_series && dlen >= 5) {
                const std::uint8_t* p = body;
                std::size_t         remain = dlen;

                if (remain >= 2) {
                    out.series_id = static_cast<std::uint16_t>(
                        (static_cast<std::uint16_t>(p[0]) << 8) | p[1]
                        );
                    p += 2;
                    remain -= 2;
                }

                if (remain >= 1) {
                    const std::uint8_t b = *p++;
                    --remain;

                    // repeat_label / program_pattern / expire_flag
                    const bool series_expire_valid = (b & 0x01) != 0;

                    if (series_expire_valid && remain >= 2) {
                        // expire MJD はいまの EpgEvent では保持しないので読み飛ばし
                        p += 2;
                        remain -= 2;
                    }
                }

                if (remain >= 2) {
                    out.episode_number = p[0];
                    out.last_episode_number = p[1];
                    p += 2;
                    remain -= 2;
                }

                if (remain >= 1) {
                    std::uint8_t nameLen = *p++;
                    --remain;
                    if (nameLen > remain) {
                        nameLen = static_cast<std::uint8_t>(remain);
                    }

                    if (nameLen > 0) {
                        std::vector<std::uint8_t> nameBytes(p, p + nameLen);
                        out.series_name = decoder.DecodeToWString(nameBytes);
                    }
                }

                out.has_series = true;
            }

            // --- 0x55: parental_rating_descriptor → 年齢制限 ---
            if (tag == 0x55 && !out.has_parental_rating && dlen >= 4) {
                const std::uint8_t* p = body;
                const std::size_t   remain = dlen;

                if (remain >= 4) {
                    // country_code(3byte) + rating 1byte
                    const std::uint8_t rating = p[3];

                    out.has_parental_rating = true;
                    out.parental_rating = rating;
                }
            }

            pos += 2u + dlen;
        }
    }

} // namespace

// -----------------------------------------------------------------------------
// 本体
// -----------------------------------------------------------------------------

EpgTable BuildEpgTableFromFilters(
    const FilterManager& fm,
    aribdecode::AribDecodecallUnit& decoder,
    const EpgBuildOptions& options)
{
    EpgTable epg;

    // FilterManager から「最終的な EIT イベント列」を取得
    const std::vector<ParsedEITPacket>& rawEvents = fm.GetEITEvents();
    if (rawEvents.empty()) {
        DebugLog(L"[EPG] BuildEpgTableFromFilters: rawEvents は空です。");
        return epg;
    }

    DebugLog(L"[EPG] BuildEpgTableFromFilters: rawEvents.size = %zu", rawEvents.size());

    // EITEventSortUnit で重複排除＋開始時刻順ソート
    std::vector<ParsedEITPacket> events =
        EITEventSortUnit::BuildUniqueSortedEvents(rawEvents);

    DebugLog(L"[EPG] BuildEpgTableFromFilters: uniqueSortedEvents.size = %zu", events.size());

#if EPG_BUILDER_DEBUG_VERBOSE
    // 先頭数件だけ rawEvents の中身をざっくり確認
    const std::size_t maxDebugEvents = 3;
    std::size_t dbgIdx = 0;
    for (const auto& ev : events) {
        if (dbgIdx >= maxDebugEvents) break;

        DebugLog(
            L"[EPG][DBG-RAW] #%zu: TID=0x%02X ONID=0x%04X TSID=0x%04X SID=0x%04X EID=0x%04X "
            L"start_time_raw=0x%010llX duration=%u shortLen=%zu shortTextLen=%zu extLen=%zu descLen=%zu",
            dbgIdx,
            ev.table_id,
            ev.original_network_id,
            ev.transport_stream_id,
            ev.service_id,
            ev.event_id,
            static_cast<unsigned long long>(ev.start_time),
            static_cast<unsigned>(ev.duration),
            ev.short_event_name_bytes.size(),
            ev.short_text_bytes.size(),
            ev.extended_text_bytes.size(),
            ev.descriptors.size()
        );

        ++dbgIdx;
    }
#endif

    // 各イベントを EpgEvent に変換して EpgTable に積む
    std::size_t convertedCount = 0;

    for (const auto& ev : events) {
        EpgEvent out{};

        // --- 識別情報 ---
        out.original_network_id = ev.original_network_id;
        out.transport_stream_id = ev.transport_stream_id;
        out.service_id = ev.service_id;
        out.event_id = ev.event_id;

        // --- 開始日時 ---
        const DecodedDateTime dt = DecodeEitStartTime(ev.start_time);
        if (!dt.valid) {
            // 開始時刻未定義のイベントは EPGから除外
#if EPG_BUILDER_DEBUG_VERBOSE
            DebugLog(
                L"[EPG][DBG-SKIP] ONID=0x%04X TSID=0x%04X SID=0x%04X EID=0x%04X: start_time 未定義のため除外",
                ev.original_network_id,
                ev.transport_stream_id,
                ev.service_id,
                ev.event_id
            );
#endif
            continue;
        }

        out.year = dt.year;
        out.month = dt.month;
        out.day = dt.day;
        out.start_hour = dt.hour;
        out.start_minute = dt.min;

        // --- 継続時間（秒 → 分・切り捨て）---
        if (ev.duration != 0xFFFFFFFFu) {
            out.duration_minutes = static_cast<int>(ev.duration / 60u);
        }
        else {
            // duration 未定義（予約等）の場合は 0 にしておく
            out.duration_minutes = 0;
        }

        // --- 文字列（ARIB バイトがあれば優先、無ければ UTF-16 フィールドを利用）---
        auto DecodeOrUseUtf16 = [&](const std::vector<std::uint8_t>& bytes,
            const std::wstring& fallback) -> std::wstring
            {
                if (!bytes.empty()) {
                    return decoder.DecodeToWString(bytes);
                }
                return fallback;
            };

        // --- 短形式タイトル／短形式概要 ---
        if (options.decodeShortText) {
            // 短形式タイトル
            out.event_name = DecodeOrUseUtf16(
                ev.short_event_name_bytes,
                ev.title_utf16
            );

            // 短形式概要
            out.short_text = DecodeOrUseUtf16(
                ev.short_text_bytes,
                ev.short_text_utf16
            );
        }
        else {
            out.event_name.clear();
            out.short_text.clear();
        }

        // --- 拡張テキスト（[拡張items]） ---
        // 現状は options.decodeExtendedText が true のときだけデコードし、
        // それ以外は EPG構築上は無視する（extended_text は空文字列）。
        if (options.decodeExtendedText) {
            out.extended_text = DecodeOrUseUtf16(
                ev.extended_text_bytes,
                ev.extended_text_utf16
            );
        }
        else {
            out.extended_text.clear();
        }


        // --- ジャンル／シリーズ／年齢制限など descriptor 由来の情報 ---
        FillGenreSeriesParentalFromDescriptors(ev.descriptors, decoder, out);

#if EPG_BUILDER_DEBUG_VERBOSE
        // 先頭数件だけ「デコード後」の中身を確認
        if (convertedCount < 3) {
            DebugLog(
                L"[EPG][DBG-DEC] #%zu: ONID=0x%04X TSID=0x%04X SID=0x%04X EID=0x%04X "
                L"date=%04d/%02d/%02d %02d:%02d dur=%dmin title=\"%s\" shortLen=%zu extLen=%zu "
                L"genre=%d-%d user=(%d,%d) series=%d(%d/%d) rating=0x%02X",
                convertedCount,
                out.original_network_id,
                out.transport_stream_id,
                out.service_id,
                out.event_id,
                out.year, out.month, out.day, out.start_hour, out.start_minute,
                out.duration_minutes,
                out.event_name.c_str(),
                out.short_text.size(),
                out.extended_text.size(),
                out.has_genre ? out.genre_primary : -1,
                out.has_genre ? out.genre_secondary : -1,
                out.has_genre ? out.user_nibble1 : -1,
                out.has_genre ? out.user_nibble2 : -1,
                out.has_series ? out.series_id : 0,
                out.has_series ? out.episode_number : 0,
                out.has_series ? out.last_episode_number : 0,
                out.has_parental_rating ? out.parental_rating : 0
            );
        }
#endif

        // --- サービスキーでグルーピング ---
        const std::uint64_t key = MakeServiceKey(
            out.original_network_id,
            out.transport_stream_id,
            out.service_id
        );

        EpgServiceSchedule& sched = epg[key];

        // サービス名は SDT 側で補完する前提なのでここでは触らない
        // sched.service_name は別ユニットで埋める

        // イベントをスケジュールに追加
        sched.events.push_back(std::move(out));
        ++convertedCount;
    }

    // 念のため、サービスごとに開始時刻でソート
    for (auto& kv : epg) {
        auto& sched = kv.second;
        std::sort(
            sched.events.begin(),
            sched.events.end(),
            [](const EpgEvent& a, const EpgEvent& b) {
                if (a.year != b.year)        return a.year < b.year;
                if (a.month != b.month)       return a.month < b.month;
                if (a.day != b.day)         return a.day < b.day;
                if (a.start_hour != b.start_hour)  return a.start_hour < b.start_hour;
                return a.start_minute < b.start_minute;
            }
        );
    }

    // ★ デバッグ: サービス数と合計イベント数を出力
    std::size_t totalEvents = 0;
    for (const auto& kv : epg) {
        totalEvents += kv.second.events.size();
    }
    DebugLog(L"[EPG] BuildEpgTableFromFilters: EpgTable 構築完了: サービス数=%zu, 合計イベント数=%zu",
        epg.size(), totalEvents);

#if EPG_BUILDER_DEBUG_VERBOSE
    // ★ 先頭数サービスだけざっくり内容確認ログ
    std::size_t serviceIndex = 0;
    for (const auto& kv : epg) {
        const auto& sched = kv.second;
        if (sched.events.empty()) {
            continue;
        }
        const auto& e0 = sched.events.front();
        DebugLog(
            L"[EPG] サービス[%zu]: ONID=0x%04X TSID=0x%04X SID=0x%04X, イベント数=%zu, "
            L"最初の番組: %04d/%02d/%02d %02d:%02d \"%s\"",
            serviceIndex,
            e0.original_network_id,
            e0.transport_stream_id,
            e0.service_id,
            sched.events.size(),
            e0.year, e0.month, e0.day, e0.start_hour, e0.start_minute,
            e0.event_name.c_str()
        );
        if (++serviceIndex >= 5) {
            break; // 多すぎるので5サービスまで
        }
    }
#endif

    return epg;
}


