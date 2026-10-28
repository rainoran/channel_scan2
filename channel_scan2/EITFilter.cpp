// EITFilter.cpp (C++20) - H(authoritative skeleton) / L(patch) 分離格納版
//
// 方針
// - ParseEITSection() は従来どおり「可能な限り解析」して ParsedEITPacket を作る
// - ParseAndCheckPacket() で
//    1) H（骨格）に必ず格納（ヘッダ情報のみ）
//    2) L（補完）に必要なら格納（4D/4E/54/D5/55等）
//    3) merged は H と同じ index を持ち、H更新やL更新で再構築
// - version が一致しない L は merged へ適用しない（混線防止）

#include "EITFilter.hpp"
//#include "TSSectionValidator.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <fstream>
#include <iomanip>
#include <system_error>
#include <string_view>
#include <string>
#include <filesystem>
#include <array>
#include <map>
#include <cstdint>
#include <exception>
#include <new>

#ifndef EIT_KEEP_RAW_DESCRIPTORS
#define EIT_KEEP_RAW_DESCRIPTORS 0
#endif

// ---------------------- グローバル ----------------------
static std::mutex g_ccTrack_mtx;
static std::mutex g_eitCountersFileMtx;
static void WriteU16Ascii(std::ofstream& ofs, std::string_view s);

namespace {

    // ---- (診断) 流量統計（merged基準） ----
    struct EventFlowDiag {
        std::uint64_t parsed_calls = 0;
        std::uint64_t added = 0;
        std::uint64_t updated = 0;
        std::uint64_t trimmed = 0;
        std::uint64_t max_stored = 0;
    };

    static std::mutex g_flowDiag_mtx;
    static std::unordered_map<const EITFilter*, EventFlowDiag> g_flowDiag;

    static void FlowDiag_OnParsed(const EITFilter* self, bool is_add, std::size_t stored_size)
    {
        std::lock_guard<std::mutex> lk(g_flowDiag_mtx);
        auto& d = g_flowDiag[self];
        ++d.parsed_calls;
        if (is_add) ++d.added;
        else        ++d.updated;
        if (stored_size > d.max_stored) d.max_stored = static_cast<std::uint64_t>(stored_size);
    }

    static void FlowDiag_OnTrim(const EITFilter* self, std::size_t removed)
    {
        if (removed == 0) return;
        std::lock_guard<std::mutex> lk(g_flowDiag_mtx);
        auto& d = g_flowDiag[self];
        d.trimmed += static_cast<std::uint64_t>(removed);
    }

    // -----------------------------
    // language_code (ISO-639) 'jpn'
    // -----------------------------
    static constexpr uint32_t kLangJpn =
        (static_cast<uint32_t>('j') << 16) |
        (static_cast<uint32_t>('p') << 8) |
        (static_cast<uint32_t>('n'));

    // -----------------------------
    // small helpers
    // -----------------------------
    static bool IsEmptyLikeLabel(const uint8_t* p, std::size_t n) noexcept
    {
        if (n == 0) return true;
        for (std::size_t i = 0; i < n; ++i) {
            const uint8_t c = p[i];
            if (c != 0x20 && c != 0x00) return false;
        }
        return true;
    }

    static void NormalizeExtendedItems(std::vector<EitExtendedItem>& items)
    {
        if (items.empty()) return;

        std::vector<EitExtendedItem> out;
        out.reserve(items.size());

        for (auto& it : items) {
            if (!out.empty()) {
                auto& last = out.back();

                // continuation (空ラベル)
                if (IsEmptyLikeLabel(it.item_description_bytes.data(), it.item_description_bytes.size())) {
                    last.item_text_bytes.insert(last.item_text_bytes.end(),
                        it.item_text_bytes.begin(), it.item_text_bytes.end());
                    continue;
                }

                // same label
                if (!it.item_description_bytes.empty() &&
                    last.item_description_bytes == it.item_description_bytes) {
                    last.item_text_bytes.insert(last.item_text_bytes.end(),
                        it.item_text_bytes.begin(), it.item_text_bytes.end());
                    continue;
                }
            }
            out.push_back(std::move(it));
        }

        items = std::move(out);
    }

    // PSI/SI CRC32 (0x04C11DB7, init 0xFFFFFFFF)
    static uint32_t ComputePsiCrc32(const uint8_t* data, std::size_t length) noexcept
    {
        uint32_t crc = 0xFFFFFFFFu;
        for (std::size_t i = 0; i < length; ++i) {
            crc ^= static_cast<uint32_t>(data[i]) << 24;
            for (int b = 0; b < 8; ++b) {
                if (crc & 0x80000000u) crc = (crc << 1) ^ 0x04C11DB7u;
                else                  crc <<= 1;
            }
        }
        return crc;
    }

    static uint8_t BcdToUint(uint8_t b) noexcept
    {
        return static_cast<uint8_t>(((b >> 4) & 0x0F) * 10 + (b & 0x0F));
    }

    static inline bool IsEITTableId(uint8_t tid) noexcept
    {
        if (tid == 0x4E || tid == 0x4F) return true;      // p/f
        if (tid >= 0x50 && tid <= 0x6F) return true;      // schedule
        return false;
    }

    // 1インスタンスごとの CC 追跡
    struct CcTrackState {
        bool valid = false;
        uint8_t last_cc = 0;
        std::size_t discontinuity_count = 0;
        std::size_t cc_error_count = 0;
    };
    static std::unordered_map<const EITFilter*, CcTrackState> g_ccTrack;

    // --- item merge helpers (patch merge) ---
    static uint64_t Fnv1a64(const uint8_t* p, std::size_t n) noexcept
    {
        uint64_t h = 1469598103934665603ull;
        for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    }

    static uint64_t HashItem(const EitExtendedItem& it) noexcept
    {
        uint64_t h = 0;
        if (!it.item_description_bytes.empty())
            h ^= Fnv1a64(it.item_description_bytes.data(), it.item_description_bytes.size());
        h ^= 0x9E3779B97F4A7C15ull;
        if (!it.item_text_bytes.empty())
            h ^= Fnv1a64(it.item_text_bytes.data(), it.item_text_bytes.size());
        return h;
    }

    static void MergeExtendedItems(std::vector<EitExtendedItem>& dst,
        const std::vector<EitExtendedItem>& src)
    {
        constexpr std::size_t kMaxItems = 32;
        if (src.empty() || dst.size() >= kMaxItems) return;

        std::unordered_set<uint64_t> seen;
        seen.reserve(dst.size() + src.size());
        for (const auto& d : dst) seen.insert(HashItem(d));

        for (const auto& s : src) {
            if (dst.size() >= kMaxItems) break;
            const uint64_t hs = HashItem(s);
            if (seen.insert(hs).second) dst.push_back(s);
        }
    }

    static void PreferLonger(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src)
    {
        if (src.size() > dst.size()) dst = src;
    }

    // ★ 4D 用: 空で上書きしない / 長い方を採用（ポインタ入力）
    static void PreferNonEmptyLonger(std::vector<uint8_t>& dst, const uint8_t* p, std::size_t n)
    {
        if (!p || n == 0) return;
        if (dst.empty() || n > dst.size()) {
            dst.assign(p, p + n);
        }
    }

    // ---- (優先2) 4E: イベント内で完結する断片をローカルで組み立てる ----
    struct ExtFragCollector {
        uint8_t last = 0;
        bool last_set = false;
        std::array<std::vector<uint8_t>, 16> frag;
        std::array<bool, 16> has{};

        void Reset(uint8_t new_last) {
            last = new_last;
            last_set = true;
            has.fill(false);
            for (auto& v : frag) v.clear();
        }

        bool Add(uint8_t no, uint8_t last_no, const uint8_t* p, std::size_t n) {
            if (no >= 16 || last_no >= 16) return false;
            if (no > last_no) return false;

            if (!last_set) {
                Reset(last_no);
            }
            else if (last != last_no) {
                Reset(last_no);
            }

            if (!has[no] || n > frag[no].size()) {
                frag[no].assign(p, p + n);
            }
            has[no] = true;
            return true;
        }

        bool CanAssemble() const {
            if (!last_set) return false;
            for (uint8_t i = 0; i <= last; ++i) {
                if (!has[i]) return false;
            }
            return true;
        }

        void Assemble(std::vector<uint8_t>& out) const {
            out.clear();
            if (!CanAssemble()) return;
            std::size_t total = 0;
            for (uint8_t i = 0; i <= last; ++i) total += frag[i].size();
            out.reserve(total);
            for (uint8_t i = 0; i <= last; ++i) {
                out.insert(out.end(), frag[i].begin(), frag[i].end());
            }
        }
    };

    // ---- 0x54 content_descriptor ----
    static void HandleContentDescriptor54(ParsedEITPacket& evt, const uint8_t* body, uint8_t dlen) noexcept
    {
        // 仕様上は2バイト単位の繰り返しになり得るが、
        // ここでは「最初の1件だけ」を既存挙動として踏襲する（最小修正）
        if (!body || dlen < 2) return;
        if (evt.has_genre) return;

        const uint8_t b0 = body[0];
        const uint8_t b1 = body[1];

        evt.content_nibble_level1 = static_cast<uint8_t>((b0 >> 4) & 0x0F);
        evt.content_nibble_level2 = static_cast<uint8_t>(b0 & 0x0F);
        evt.user_nibble1 = static_cast<uint8_t>((b1 >> 4) & 0x0F);
        evt.user_nibble2 = static_cast<uint8_t>(b1 & 0x0F);

        evt.has_genre = true;
    }

    // ---- 0x4E extended_event_descriptor ----
    template<class StatsT>
    static void HandleExtendedEventDescriptor4E(
        ParsedEITPacket& evt,
        EitEventTextAssembler& textAsm,
        StatsT& stats,
        EitEventTextKey& key, // 既存の key を使い回す（language_code を更新する）
        std::unordered_map<uint32_t, ExtFragCollector>& ext_by_lang,
        const uint8_t* body,
        uint8_t dlen)
    {
        if (!body || dlen < 6) return;

        ++stats.desc_4e_seen; // ★ m_stats ではなく stats

        const uint8_t descriptor_number = (body[0] >> 4) & 0x0F;
        const uint8_t last_descriptor_number = (body[0]) & 0x0F;

        const std::uint32_t lang =
            (static_cast<std::uint32_t>(body[1]) << 16) |
            (static_cast<std::uint32_t>(body[2]) << 8) |
            (static_cast<std::uint32_t>(body[3]));
        key.language_code = lang;

        const uint8_t length_of_items = body[4];
        std::size_t   pos_in_body = 5;

        if (pos_in_body + length_of_items > dlen) {
            ++stats.desc_4e_drop_items_bounds;
            return;
        }

        // items
        if (length_of_items > 0) {
            const std::size_t items_start = pos_in_body;
            const std::size_t items_end = items_start + length_of_items;
            std::size_t       items_pos = items_start;

            constexpr std::size_t kMaxItemsPerEvent = 32;

            while (items_pos < items_end && evt.extended_items.size() < kMaxItemsPerEvent) {
                if (items_pos + 1 > items_end) break;
                const uint8_t item_desc_len = body[items_pos++];
                if (items_pos + item_desc_len > items_end) break;
                const uint8_t* item_desc = body + items_pos;
                items_pos += item_desc_len;

                if (items_pos + 1 > items_end) break;
                const uint8_t item_len = body[items_pos++];
                if (items_pos + item_len > items_end) break;
                const uint8_t* item_text = body + items_pos;
                items_pos += item_len;

                if (IsEmptyLikeLabel(item_desc, item_desc_len)) {
                    if (!evt.extended_items.empty()) {
                        auto& last = evt.extended_items.back();
                        last.item_text_bytes.insert(last.item_text_bytes.end(), item_text, item_text + item_len);
                        ++stats.extitem_merged_continuation;
                    }
                    else {
                        ++stats.extitem_orphan_continuation;
                    }
                    continue;
                }

                if (item_desc_len > 0 && !evt.extended_items.empty()) {
                    auto& last = evt.extended_items.back();
                    if (last.item_description_bytes.size() == item_desc_len &&
                        std::equal(last.item_description_bytes.begin(), last.item_description_bytes.end(), item_desc)) {
                        last.item_text_bytes.insert(last.item_text_bytes.end(), item_text, item_text + item_len);
                        ++stats.extitem_merged_same_label;
                        continue;
                    }
                }

                EitExtendedItem item;
                item.item_description_bytes.assign(item_desc, item_desc + item_desc_len);
                item.item_text_bytes.assign(item_text, item_text + item_len);
                evt.extended_items.push_back(std::move(item));
            }

            pos_in_body = items_end;
        }

        // text_char
        if (pos_in_body >= dlen) {
            ++stats.desc_4e_drop_text_bounds;
            return;
        }

        std::uint8_t text_length = body[pos_in_body];
        ++pos_in_body;

        constexpr std::size_t kMaxExtTextLen = 255;
        if (text_length > kMaxExtTextLen) {
            return;
        }

        if (pos_in_body + text_length > dlen) {
            ++stats.desc_4e_drop_text_bounds;
            return;
        }

        ++stats.desc_4e_ok;

        const uint8_t* const text_char = body + pos_in_body;

        {
            auto& col = ext_by_lang[lang];
            if (!col.Add(descriptor_number, last_descriptor_number, text_char, text_length)) {
                ++stats.desc_4e_frag_reject;
                return;
            }
        }

        const bool complete = (descriptor_number == last_descriptor_number);
        if (complete) ++stats.asm_complete_true;

        textAsm.AddFragment(
            key,
            descriptor_number,
            last_descriptor_number,
            true,
            text_char,
            text_length
        );
    }


    // ---- (優先3) table_id別の診断 ----
    struct TidDiagAgg {
        std::size_t ev = 0;
        std::size_t has4d_ok = 0;
        std::size_t has4e_seen = 0;
        std::size_t has4e_asm = 0;
        std::size_t only4d = 0;
        std::size_t only4e = 0;
        std::size_t both = 0;
        std::size_t drop4d_len = 0;
        std::size_t drop4d_bounds = 0;
        std::size_t desc_overrun = 0;
    };
    static std::mutex g_tidDiag_mtx;
    static std::unordered_map<const EITFilter*, std::array<TidDiagAgg, 256>> g_tidDiag;

    static void UpdateTidDiag(
        const EITFilter* self,
        uint8_t table_id,
        bool ev_has4d_ok,
        bool ev_has4e_seen,
        bool ev_has4e_assembled,
        bool ev_desc_overrun,
        bool ev_4d_drop_len,
        bool ev_4d_drop_bounds)
    {
        std::lock_guard<std::mutex> lk(g_tidDiag_mtx);
        auto& arr = g_tidDiag[self];
        auto& a = arr[table_id];

        ++a.ev;
        if (ev_has4d_ok) ++a.has4d_ok;
        if (ev_has4e_seen) ++a.has4e_seen;
        if (ev_has4e_assembled) ++a.has4e_asm;
        if (ev_desc_overrun) ++a.desc_overrun;
        if (ev_4d_drop_len) ++a.drop4d_len;
        if (ev_4d_drop_bounds) ++a.drop4d_bounds;

        const bool has4d = ev_has4d_ok;
        const bool has4e = ev_has4e_seen;
        if (has4d && has4e) ++a.both;
        else if (has4d)     ++a.only4d;
        else if (has4e)     ++a.only4e;
    }

    // ---- (優先1) TSV SERVICE行の列ズレ防止 ----
    static void WriteEmptyTabs(std::ofstream& ofs, std::size_t n)
    {
        for (std::size_t i = 0; i < n; ++i) ofs << "\t";
    }

    // -----------------------------
    // (改善5) イベント境界 resync
    // -----------------------------
    static inline bool IsValidBcdByte(uint8_t b) noexcept
    {
        return ((b >> 4) <= 9) && ((b & 0x0F) <= 9);
    }
    static inline bool IsValidHMS(uint8_t bh, uint8_t bm, uint8_t bs) noexcept
    {
        if (!IsValidBcdByte(bh) || !IsValidBcdByte(bm) || !IsValidBcdByte(bs)) return false;
        const uint8_t h = BcdToUint(bh);
        const uint8_t m = BcdToUint(bm);
        const uint8_t s = BcdToUint(bs);
        return (h <= 23) && (m <= 59) && (s <= 59);
    }

    static bool FindNextEventStart(
        const uint8_t* section,
        std::size_t payload_end,
        std::size_t from_pos,
        std::size_t& out_pos) noexcept
    {
        for (std::size_t p = from_pos; p + 12u <= payload_end; ++p) {
            if (!IsValidHMS(section[p + 7], section[p + 8], section[p + 9])) continue;

            const uint8_t running_free_len_hi = section[p + 10];
            const uint8_t len_lo = section[p + 11];
            const uint16_t loop_len =
                static_cast<uint16_t>(((running_free_len_hi & 0x0F) << 8) | len_lo);

            const std::size_t desc_start = p + 12u;
            const std::size_t desc_end = desc_start + static_cast<std::size_t>(loop_len);
            if (desc_end > payload_end) continue;

            out_pos = p;
            return true;
        }
        return false;
    }

} // unnamed namespace

// ---------------------- H/L ヘルパ実装 ----------------------
EITFilter::EventKey EITFilter::MakeKey(const ParsedEITPacket& p) noexcept
{
    EventKey k{};
    k.original_network_id = p.original_network_id;
    k.transport_stream_id = p.transport_stream_id;
    k.service_id = p.service_id;
    k.table_id = p.table_id;
    k.event_id = p.event_id;
    k.start_time_raw = p.start_time;
    return k;
}

EITFilter::SectionKey EITFilter::MakeSectionKey(const ParsedEITPacket& p) noexcept
{
    SectionKey k{};
    k.original_network_id = p.original_network_id;
    k.transport_stream_id = p.transport_stream_id;
    k.service_id = p.service_id;
    k.table_id = p.table_id;
    k.section_number = p.section_number;
    return k;
}

ParsedEITPacket EITFilter::MakeSkeletonH(const ParsedEITPacket& in)
{
    ParsedEITPacket h = in;

    // H は “骨格のみ”
    h.descriptors.clear();

    h.short_event_name_bytes.clear();
    h.short_text_bytes.clear();
    h.extended_text_bytes.clear();
    h.extended_items.clear();

    h.has_genre = false;
    h.content_nibble_level1 = 0;
    h.content_nibble_level2 = 0;
    h.user_nibble1 = 0;
    h.user_nibble2 = 0;

    h.has_series = false;
    h.series_id = 0;
    h.repeat_label = 0;
    h.program_pattern = 0;
    h.series_expire_valid = false;
    h.series_expire_mjd = 0;
    h.episode_number = 0;
    h.last_episode_number = 0;
    h.series_name_bytes.clear();

    h.has_parental_rating = false;
    h.parental_country.clear();
    h.parental_rating = 0;

    h.title_utf16.clear();
    h.short_text_utf16.clear();
    h.extended_text_utf16.clear();

    return h;
}

bool EITFilter::HasAnyPatchL(const ParsedEITPacket& in) noexcept
{
    if (!in.descriptors.empty()) return true;
    if (!in.short_event_name_bytes.empty()) return true;
    if (!in.short_text_bytes.empty()) return true;
    if (!in.extended_text_bytes.empty()) return true;
    if (!in.extended_items.empty()) return true;
    if (in.has_genre || in.has_series || in.has_parental_rating) return true;
    if (!in.title_utf16.empty() || !in.short_text_utf16.empty() || !in.extended_text_utf16.empty()) return true;
    if (!in.series_name_bytes.empty()) return true;
    return false;
}

ParsedEITPacket EITFilter::MakePatchL(const ParsedEITPacket& in)
{
    ParsedEITPacket l = in;

    // L は “補完中心” だが、同一キー判定に必要な骨格は残す
    // （start_time 等は key として必要）
    // 逆に “H専用の骨格” は消す必要はない（適用側で必要なフィールドだけ使う）

    return l;
}

void EITFilter::MergePatchL(ParsedEITPacket& dst, const ParsedEITPacket& src)
{
    // version が違うものは混ぜない（置き換え）
    if (dst.version != src.version) {
        dst = src;
        return;
    }

    // “より良い/長い” を優先（L側で素材を育てる）
    PreferLonger(dst.descriptors, src.descriptors);
    PreferLonger(dst.short_event_name_bytes, src.short_event_name_bytes);
    PreferLonger(dst.short_text_bytes, src.short_text_bytes);
    PreferLonger(dst.extended_text_bytes, src.extended_text_bytes);

    if (!src.extended_items.empty()) {
        if (dst.extended_items.empty()) dst.extended_items = src.extended_items;
        else MergeExtendedItems(dst.extended_items, src.extended_items);
        NormalizeExtendedItems(dst.extended_items);
    }

    if (src.has_genre && !dst.has_genre) {
        dst.has_genre = true;
        dst.content_nibble_level1 = src.content_nibble_level1;
        dst.content_nibble_level2 = src.content_nibble_level2;
        dst.user_nibble1 = src.user_nibble1;
        dst.user_nibble2 = src.user_nibble2;
    }

    if (src.has_series && !dst.has_series) {
        dst.has_series = true;
        dst.series_id = src.series_id;
        dst.repeat_label = src.repeat_label;
        dst.program_pattern = src.program_pattern;
        dst.series_expire_valid = src.series_expire_valid;
        dst.series_expire_mjd = src.series_expire_mjd;
        dst.episode_number = src.episode_number;
        dst.last_episode_number = src.last_episode_number;
        if (!src.series_name_bytes.empty()) dst.series_name_bytes = src.series_name_bytes;
    }
    else if (src.has_series && dst.has_series) {
        // シリーズ名だけ後から来るケース
        if (dst.series_name_bytes.empty() && !src.series_name_bytes.empty()) {
            dst.series_name_bytes = src.series_name_bytes;
        }
    }

    if (src.has_parental_rating && !dst.has_parental_rating) {
        dst.has_parental_rating = true;
        dst.parental_country = src.parental_country;
        dst.parental_rating = src.parental_rating;
    }

    if (dst.title_utf16.empty() && !src.title_utf16.empty()) dst.title_utf16 = src.title_utf16;
    if (dst.short_text_utf16.empty() && !src.short_text_utf16.empty()) dst.short_text_utf16 = src.short_text_utf16;
    if (dst.extended_text_utf16.empty() && !src.extended_text_utf16.empty()) dst.extended_text_utf16 = src.extended_text_utf16;
}

void EITFilter::ApplyPatchToMerged(ParsedEITPacket& merged, const ParsedEITPacket& patch)
{
    // merged は H から作るので、ここでは “埋める” が基本
    if (merged.descriptors.empty()) PreferLonger(merged.descriptors, patch.descriptors);

    if (merged.short_event_name_bytes.empty()) PreferLonger(merged.short_event_name_bytes, patch.short_event_name_bytes);
    if (merged.short_text_bytes.empty())       PreferLonger(merged.short_text_bytes, patch.short_text_bytes);
    if (merged.extended_text_bytes.empty())    PreferLonger(merged.extended_text_bytes, patch.extended_text_bytes);

    if (!patch.extended_items.empty()) {
        if (merged.extended_items.empty()) merged.extended_items = patch.extended_items;
        else MergeExtendedItems(merged.extended_items, patch.extended_items);
        NormalizeExtendedItems(merged.extended_items);
    }

    if (!merged.has_genre && patch.has_genre) {
        merged.has_genre = true;
        merged.content_nibble_level1 = patch.content_nibble_level1;
        merged.content_nibble_level2 = patch.content_nibble_level2;
        merged.user_nibble1 = patch.user_nibble1;
        merged.user_nibble2 = patch.user_nibble2;
    }

    if (!merged.has_series && patch.has_series) {
        merged.has_series = true;
        merged.series_id = patch.series_id;
        merged.repeat_label = patch.repeat_label;
        merged.program_pattern = patch.program_pattern;
        merged.series_expire_valid = patch.series_expire_valid;
        merged.series_expire_mjd = patch.series_expire_mjd;
        merged.episode_number = patch.episode_number;
        merged.last_episode_number = patch.last_episode_number;
        merged.series_name_bytes = patch.series_name_bytes;
    }
    else if (merged.has_series && merged.series_name_bytes.empty() && patch.has_series && !patch.series_name_bytes.empty()) {
        merged.series_name_bytes = patch.series_name_bytes;
    }

    if (!merged.has_parental_rating && patch.has_parental_rating) {
        merged.has_parental_rating = true;
        merged.parental_country = patch.parental_country;
        merged.parental_rating = patch.parental_rating;
    }

    if (merged.title_utf16.empty() && !patch.title_utf16.empty()) merged.title_utf16 = patch.title_utf16;
    if (merged.short_text_utf16.empty() && !patch.short_text_utf16.empty()) merged.short_text_utf16 = patch.short_text_utf16;
    if (merged.extended_text_utf16.empty() && !patch.extended_text_utf16.empty()) merged.extended_text_utf16 = patch.extended_text_utf16;
}

void EITFilter::RebuildMergedAtIndex(std::size_t hIndex)
{
    if (hIndex >= m_eventsH.size()) return;

    // merged は H と同じ index
    m_eventsMerged[hIndex] = m_eventsH[hIndex];

    const auto key = MakeKey(m_eventsH[hIndex]);
    auto itL = m_eventIndexL.find(key);
    if (itL == m_eventIndexL.end()) {
        ++m_stats.merged_rebuild;
        return;
    }

    const auto& patch = m_eventsL[itL->second];

    // version が一致しない patch は適用しない
    if (patch.version != m_eventsH[hIndex].version) {
        ++m_stats.patch_skipped_version_mismatch;
        ++m_stats.merged_rebuild;
        return;
    }

    ApplyPatchToMerged(m_eventsMerged[hIndex], patch);
    ++m_stats.merged_rebuild;
}

// ---------------------- デストラクタ ----------------------
EITFilter::~EITFilter()
{
    {
        std::lock_guard<std::mutex> lk(g_ccTrack_mtx);
        g_ccTrack.erase(this);
    }
    {
        std::lock_guard<std::mutex> lk(g_tidDiag_mtx);
        g_tidDiag.erase(this);
    }
    {
        std::lock_guard<std::mutex> lk(g_flowDiag_mtx);
        g_flowDiag.erase(this);
    }
}

// ---------------------- TS パケット入力 ----------------------
void EITFilter::InputPacket(const std::array<uint8_t, TS_PACKET_SIZE>& pkt)
{
    ++m_tsPacketCount;

    const uint8_t* data = pkt.data();

    if (data[0] != 0x47) return;      // sync
    if (data[1] & 0x80) return;       // TEI

    const bool payload_unit_start = (data[1] & 0x40) != 0;
    const uint8_t adaptation_field_control = (data[3] >> 4) & 0x03;
    const uint8_t cc = static_cast<uint8_t>(data[3] & 0x0F);

    std::size_t pos = 4;

    // payload 無し
    if (adaptation_field_control == 0x00 || adaptation_field_control == 0x02) {
        return;
    }

    // adaptation_field + payload
    bool discontinuity_indicator = false;
    if (adaptation_field_control == 0x03) {
        if (pos >= TS_PACKET_SIZE) return;
        const uint8_t adaptation_length = data[pos];
        const std::size_t af_end = pos + 1u + static_cast<std::size_t>(adaptation_length);
        if (af_end > TS_PACKET_SIZE) return;

        if (adaptation_length >= 1) {
            const uint8_t flags = data[pos + 1];
            discontinuity_indicator = (flags & 0x80) != 0;
        }

        pos = af_end;
        if (pos == TS_PACKET_SIZE) return;
    }

    // ---- CC チェック（破損セクション連鎖を止める）----
    bool do_log = false;
    uint8_t got_cc = cc;
    uint8_t exp_cc = 0;
    std::size_t cc_err_count = 0;
    bool cc_ok = true;

    {
        std::lock_guard<std::mutex> lk(g_ccTrack_mtx);
        auto& st = g_ccTrack[this];

        if (discontinuity_indicator) {
            st.valid = false;
            ++st.discontinuity_count;

            sectionBuffer.clear();
            expectedSectionSize = 0;
        }
        else {
            if (st.valid) {
                exp_cc = static_cast<uint8_t>((st.last_cc + 1u) & 0x0F);
                if (cc != exp_cc) {
                    ++st.cc_error_count;
                    cc_err_count = st.cc_error_count;

                    sectionBuffer.clear();
                    expectedSectionSize = 0;

                    cc_ok = false;
                    do_log = ((cc_err_count & 0x1Fu) == 1u); // 1/32程度でログ
                }
            }

            st.last_cc = cc;
            st.valid = cc_ok;
        }
    }

    if (do_log) {
        DebugLogFormat(
            L"[EIT] continuity_counter discontinuity: got=%u expected=%u (count=%zu)",
            static_cast<unsigned>(got_cc),
            static_cast<unsigned>(exp_cc),
            static_cast<size_t>(cc_err_count)
        );
    }

    // ---- sectionBuffer確定補助 ----
    auto computeExpectedIfNeeded = [&]() {
        if (!sectionBuffer.empty() && sectionBuffer[0] == 0xFF) {
            std::size_t i = 0;
            while (i < sectionBuffer.size() && sectionBuffer[i] == 0xFF) ++i;
            sectionBuffer.erase(sectionBuffer.begin(), sectionBuffer.begin() + (std::ptrdiff_t)i);
            if (sectionBuffer.empty()) { expectedSectionSize = 0; return; }
            expectedSectionSize = 0;
        }

        if (expectedSectionSize == 0 && sectionBuffer.size() >= 3) {
            const uint16_t section_length =
                static_cast<uint16_t>(((sectionBuffer[1] & 0x0F) << 8) | sectionBuffer[2]);
            const std::size_t size = 3u + static_cast<std::size_t>(section_length);

            if (size == 0 || size > MAX_SECTION_SIZE || section_length > 4095) {
                ++m_sectionDropBySize;
                sectionBuffer.clear();
                expectedSectionSize = 0;
                return;
            }
            expectedSectionSize = size;
        }
        };

    auto tryFlush = [&]() {
        for (;;) {
            computeExpectedIfNeeded();

            if (expectedSectionSize == 0) {
                if (!sectionBuffer.empty() && sectionBuffer[0] == 0xFF) sectionBuffer.clear();
                break;
            }

            if (sectionBuffer.size() < expectedSectionSize) break;

            ++m_sectionCompleteCount;
            InputSection(sectionBuffer.data(), expectedSectionSize);

            if (sectionBuffer.size() == expectedSectionSize) {
                sectionBuffer.clear();
                expectedSectionSize = 0;
                break;
            }

            sectionBuffer.erase(
                sectionBuffer.begin(),
                sectionBuffer.begin() + static_cast<std::ptrdiff_t>(expectedSectionSize)
            );
            expectedSectionSize = 0;

            if (sectionBuffer.size() > MAX_SECTION_SIZE) {
                ++m_sectionDropBySize;
                sectionBuffer.clear();
                expectedSectionSize = 0;
                break;
            }

            while (!sectionBuffer.empty() && sectionBuffer[0] == 0xFF) {
                sectionBuffer.erase(sectionBuffer.begin());
            }
            if (sectionBuffer.empty()) break;
        }
        };

    // ---- PUSI: pointer_field処理 + 複数section拾い ----
    if (payload_unit_start) {
        ++m_sectionStartCount;

        if (pos >= TS_PACKET_SIZE) return;

        const uint8_t pointer_field = data[pos++];
        if (pos + static_cast<std::size_t>(pointer_field) > TS_PACKET_SIZE) {
            sectionBuffer.clear();
            expectedSectionSize = 0;
            return;
        }

        if (pointer_field == 0 && !sectionBuffer.empty()) {
            tryFlush();
            if (!sectionBuffer.empty()) {
                sectionBuffer.clear();
                expectedSectionSize = 0;
            }
        }

        if (pointer_field > 0) {
            if (!sectionBuffer.empty()) {
                if (sectionBuffer.size() + pointer_field > MAX_SECTION_SIZE) {
                    ++m_sectionDropBySize;
                    sectionBuffer.clear();
                    expectedSectionSize = 0;
                    return;
                }
                sectionBuffer.insert(sectionBuffer.end(), data + pos, data + pos + pointer_field);
                pos += pointer_field;
                tryFlush();
            }
            else {
                pos += pointer_field; // stuffing扱い
            }
        }

        if (pos >= TS_PACKET_SIZE) return;

        while (pos < TS_PACKET_SIZE) {
            const std::size_t remain = TS_PACKET_SIZE - pos;

            if (data[pos] == 0xFF) break;

            if (remain < 3) {
                sectionBuffer.assign(data + pos, data + TS_PACKET_SIZE);
                expectedSectionSize = 0;
                computeExpectedIfNeeded();
                break;
            }

            const uint16_t section_length =
                static_cast<uint16_t>(((data[pos + 1] & 0x0F) << 8) | data[pos + 2]);
            const std::size_t size = 3u + static_cast<std::size_t>(section_length);

            if (size == 0 || size > MAX_SECTION_SIZE || section_length > 4095) {
                ++m_sectionDropBySize;
                break;
            }

            if (remain >= size) {
                ++m_sectionCompleteCount;
                InputSection(data + pos, size);
                pos += size;
                continue;
            }

            sectionBuffer.assign(data + pos, data + TS_PACKET_SIZE);
            expectedSectionSize = size;
            break;
        }

        return;
    }

    // ---- 継続パケット ----
    if (sectionBuffer.empty() && expectedSectionSize == 0) return;
    if (pos >= TS_PACKET_SIZE) return;

    const std::size_t addLen = TS_PACKET_SIZE - pos;
    if (sectionBuffer.size() + addLen > MAX_SECTION_SIZE) {
        ++m_sectionDropBySize;
        sectionBuffer.clear();
        expectedSectionSize = 0;
        return;
    }

    sectionBuffer.insert(sectionBuffer.end(), data + pos, data + TS_PACKET_SIZE);
    tryFlush();
}

// ---------------------- Reset ----------------------
void EITFilter::Reset()
{
    m_stats = {};

    m_eventsH.clear();
    m_eventIndexH.clear();

    m_eventsL.clear();
    m_eventIndexL.clear();

    m_eventsMerged.clear();
    m_sectionSnapshots.clear();
    m_eventOwnerCounts.clear();

    data_valid = false;
    sectionBuffer.clear();
    expectedSectionSize = 0;
    m_textAssembler.Reset();
    m_segmentStates.clear();

    m_integrityDropCount = 0;
    m_errorDropCount = 0;

    m_tsPacketCount = 0;
    m_sectionStartCount = 0;
    m_sectionCompleteCount = 0;
    m_sectionDropBySize = 0;

    {
        std::lock_guard<std::mutex> lk(g_ccTrack_mtx);
        g_ccTrack.erase(this);
    }
    {
        std::lock_guard<std::mutex> lk(g_tidDiag_mtx);
        g_tidDiag.erase(this);
    }
    {
        std::lock_guard<std::mutex> lk(g_flowDiag_mtx);
        g_flowDiag.erase(this);
    }

    DebugLog(L"[EIT] フィルター状態をリセットしました。");
}

void EITFilter::ResetService(uint16_t original_network_id,
    uint16_t transport_stream_id,
    uint16_t service_id)
{
    // H / merged
    if (!m_eventsH.empty()) {
        auto end_it = std::remove_if(
            m_eventsH.begin(), m_eventsH.end(),
            [=](const ParsedEITPacket& ev) {
                return ev.original_network_id == original_network_id &&
                    ev.transport_stream_id == transport_stream_id &&
                    ev.service_id == service_id;
            });
        const std::size_t newSize = static_cast<std::size_t>(std::distance(m_eventsH.begin(), end_it));
        m_eventsH.erase(end_it, m_eventsH.end());
        if (m_eventsMerged.size() > newSize) m_eventsMerged.resize(newSize);
    }

    // L
    if (!m_eventsL.empty()) {
        const auto end_it = std::remove_if(
            m_eventsL.begin(), m_eventsL.end(),
            [=](const ParsedEITPacket& ev) {
                return ev.original_network_id == original_network_id &&
                    ev.transport_stream_id == transport_stream_id &&
                    ev.service_id == service_id;
            });
        m_eventsL.erase(end_it, m_eventsL.end());
    }

    // segment states
    if (!m_segmentStates.empty()) {
        for (auto it = m_segmentStates.begin(); it != m_segmentStates.end(); ) {
            const SegmentKey& key = it->first;
            if (key.original_network_id == original_network_id &&
                key.transport_stream_id == transport_stream_id &&
                key.service_id == service_id) {
                it = m_segmentStates.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    // section 所有情報
    if (!m_sectionSnapshots.empty()) {
        for (auto it = m_sectionSnapshots.begin(); it != m_sectionSnapshots.end(); ) {
            const SectionKey& key = it->first;
            if (key.original_network_id == original_network_id &&
                key.transport_stream_id == transport_stream_id &&
                key.service_id == service_id) {
                it = m_sectionSnapshots.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    m_eventOwnerCounts.clear();
    for (const auto& sectionPair : m_sectionSnapshots) {
        for (const auto& eventKey : sectionPair.second.event_keys) {
            ++m_eventOwnerCounts[eventKey];
        }
    }

    // rebuild indexes
    m_eventIndexH.clear();
    m_eventIndexH.reserve(m_eventsH.size());
    for (std::size_t i = 0; i < m_eventsH.size(); ++i) {
        m_eventIndexH.emplace(MakeKey(m_eventsH[i]), i);
    }

    m_eventIndexL.clear();
    m_eventIndexL.reserve(m_eventsL.size());
    for (std::size_t i = 0; i < m_eventsL.size(); ++i) {
        m_eventIndexL.emplace(MakeKey(m_eventsL[i]), i);
    }

    // merged rebuild (Hが残っている範囲だけ)
    m_eventsMerged.resize(m_eventsH.size());
    for (std::size_t i = 0; i < m_eventsH.size(); ++i) {
        RebuildMergedAtIndex(i);
    }

    if (m_eventsMerged.empty()) data_valid = false;

    m_textAssembler.Reset();
}

void EITFilter::DumpIncompleteSegments(double threshold) const
{
    DebugLogFormat(L"[EIT] DumpIncompleteSegments(threshold=%.3f)", threshold);

    for (const auto& kv : m_segmentStates) {
        const SegmentKey& key = kv.first;
        const SegmentState& st = kv.second;

        const uint8_t maxSec = st.last_section_number;
        const std::size_t total = static_cast<std::size_t>(maxSec) + 1u;
        if (total == 0) continue;

        std::size_t recv = 0;
        for (std::size_t i = 0; i < total; ++i) {
            if (st.received_sections[i]) ++recv;
        }

        const double ratio = static_cast<double>(recv) / static_cast<double>(total);
        if (ratio >= threshold) continue;

        std::wstring missing;
        for (std::size_t i = 0; i < total; ++i) {
            if (!st.received_sections[i]) {
                if (!missing.empty()) missing.append(L",");
                missing.append(std::to_wstring(i));
            }
        }

        DebugLogFormat(
            L"[EIT][INCOMPLETE] ONID=0x%04X TSID=0x%04X SID=0x%04X TID=0x%02X "
            L"ratio=%.3f (%zu/%zu) missing={%ls} ver=%u vchg=%zu dup=%zu",
            key.original_network_id,
            key.transport_stream_id,
            key.service_id,
            key.table_id,
            ratio,
            recv,
            total,
            missing.c_str(),
            static_cast<unsigned>(st.version_number),
            static_cast<size_t>(st.version_changes),
            static_cast<size_t>(st.duplicate_same_crc)
        );
    }
}

// ---------------------- 公開: セクション入力 ----------------------
void EITFilter::InputSection(const uint8_t* section, std::size_t length)
{
    if (!section || length < 3) {
        DebugLog(L"[EIT] セクションポインタまたは長さが不正です。");
        return;
    }

    const uint8_t  table_id = section[0];
    const uint16_t section_length =
        static_cast<uint16_t>(((section[1] & 0x0Fu) << 8) | section[2]);

    if ((section[1] & 0x80u) == 0) return; // section_syntax_indicator
    if (!IsEITTableId(table_id)) return;

    if (section_length < 5 || section_length > 4095) {
        ++m_stats.section_len_range_drop;
        DebugLogFormat(
            L"[EIT] section_length 範囲外: table_id=0x%02X secLen=%u bufLen=%zu",
            table_id,
            static_cast<unsigned>(section_length),
            static_cast<std::size_t>(length)
        );
        return;
    }

    const std::size_t logicalLength = 3u + static_cast<std::size_t>(section_length);
    if (logicalLength > length) {
        ++m_stats.section_len_mismatch_drop;
        DebugLogFormat(
            L"[EIT] section_length > buffer: table_id=0x%02X secLen=%u logical=%zu buf=%zu",
            table_id,
            static_cast<unsigned>(section_length),
            logicalLength,
            length
        );
        return;
    }

    try {
        ParseEITSection(section, logicalLength);
    }
    catch (const std::bad_alloc&) {
        ++m_errorDropCount;
        DebugLogFormat(L"[EIT][ERROR] ParseEITSection bad_alloc: table_id=0x%02X logical=%zu",
            table_id, logicalLength);
        TrimStoredEventsIfNeeded();
    }
    catch (const std::exception&) {
        ++m_errorDropCount;
        DebugLogFormat(L"[EIT][ERROR] ParseEITSection exception: table_id=0x%02X logical=%zu",
            table_id, logicalLength);
    }
    catch (...) {
        ++m_errorDropCount;
        DebugLogFormat(L"[EIT][ERROR] ParseEITSection unknown exception: table_id=0x%02X logical=%zu",
            table_id, logicalLength);
    }
}

// ---------------------- 内部: EIT セクション解析 ----------------------
void EITFilter::ParseEITSection(const uint8_t* section, std::size_t length)
{
    if (length < 18) {
        DebugLogFormat(L"[EIT] セクション長が短すぎます: %zu", length);
        return;
    }
    ++m_stats.section_total;

    const uint8_t table_id = section[0];
    if (!IsEITTableId(table_id)) return;
    ++m_stats.section_eit;

    const uint16_t section_length =
        static_cast<uint16_t>((section[1] & 0x0F) << 8 | section[2]);
    const std::size_t section_total_length = 3u + static_cast<std::size_t>(section_length);

    if (section_total_length != length) {
        ++m_stats.section_len_mismatch_drop;
        DebugLogFormat(
            L"[EIT] section_length の不一致: ヘッダー=%zu 実長=%zu",
            section_total_length, length
        );
        return;
    }

    if (section_total_length < 4) return;
    const std::size_t payload_end = section_total_length - 4;

    // CRC32
    uint32_t crc_in_section = 0;
    {
        const std::size_t crc_pos = payload_end;
        crc_in_section =
            (static_cast<uint32_t>(section[crc_pos]) << 24) |
            (static_cast<uint32_t>(section[crc_pos + 1]) << 16) |
            (static_cast<uint32_t>(section[crc_pos + 2]) << 8) |
            (static_cast<uint32_t>(section[crc_pos + 3]));

        const uint32_t crc_calc = ComputePsiCrc32(section, payload_end);
        if (crc_calc != crc_in_section) {
            ++m_stats.section_crc_fail;
            DebugLogFormat(
                L"[EIT] CRC32 不一致: table_id=0x%02X calc=0x%08X sec=0x%08X",
                table_id, crc_calc, crc_in_section
            );
            return;
        }
    }

    const uint16_t service_id =
        static_cast<uint16_t>((section[3] << 8) | section[4]);
    const uint8_t ver_cni = section[5];
    const uint8_t version = static_cast<uint8_t>((ver_cni >> 1) & 0x1F);
    const bool current_next = (ver_cni & 0x01) != 0;

    const uint8_t section_number = section[6];
    const uint8_t last_section_number = section[7];

    const uint16_t transport_stream_id =
        static_cast<uint16_t>((section[8] << 8) | section[9]);
    const uint16_t original_network_id =
        static_cast<uint16_t>((section[10] << 8) | section[11]);

    const uint8_t segment_last_section_number = section[12];
    const uint8_t last_table_id = section[13];

    if (!current_next) {
        ++m_stats.section_current_next_drop;
        return;
    }

    const SectionKey sectionKey{
        original_network_id,
        transport_stream_id,
        service_id,
        table_id,
        section_number
    };

    // 同一section・同一version・同一CRCは解析前に除外する。
    // versionが違う場合の採否と旧イベント削除は、全イベント解析後に
    // CommitSectionSnapshot() が一括して行う。
    if (const auto it = m_sectionSnapshots.find(sectionKey);
        it != m_sectionSnapshots.end() &&
        it->second.version == version &&
        it->second.crc == crc_in_section) {
        SegmentKey segmentKey{ original_network_id, transport_stream_id, service_id, table_id };
        auto& st = m_segmentStates[segmentKey];
        ++st.duplicate_same_crc;
        ++m_stats.section_duplicate_same_crc;
        return;
    }

    std::size_t pos = 14;
    bool sectionParseComplete = true;
    std::vector<ParsedEITPacket> parsedEvents;
    parsedEvents.reserve(16);

    while (pos + 12u <= payload_end) {
        const uint16_t event_id =
            static_cast<uint16_t>((section[pos] << 8) | section[pos + 1]);

        std::uint64_t start_time_raw = 0;
        for (int i = 0; i < 5; ++i) {
            start_time_raw = (start_time_raw << 8) | section[pos + 2 + i];
        }

        const uint8_t d_h = BcdToUint(section[pos + 7]);
        const uint8_t d_m = BcdToUint(section[pos + 8]);
        const uint8_t d_s = BcdToUint(section[pos + 9]);
        const std::uint32_t duration_hhmmss =
            static_cast<std::uint32_t>(d_h) * 10000u +
            static_cast<std::uint32_t>(d_m) * 100u +
            static_cast<std::uint32_t>(d_s);

        const uint8_t running_free_len_hi = section[pos + 10];
        const uint8_t len_lo = section[pos + 11];

        const uint8_t running_status = static_cast<uint8_t>((running_free_len_hi >> 5) & 0x07);
        const bool    free_ca_mode = ((running_free_len_hi & 0x10) != 0);

        const uint16_t descriptors_loop_length =
            static_cast<uint16_t>(((running_free_len_hi & 0x0F) << 8) | len_lo);

        const std::size_t descriptors_start = pos + 12u;
        const std::size_t descriptors_end = descriptors_start + descriptors_loop_length;

        if (descriptors_start > payload_end) {
            sectionParseComplete = false;
            break;
        }

        if (descriptors_end > payload_end) {
            DebugLogFormat(
                L"[EIT] descriptors_loop_length overrun: ev_id=%u pos=%zu end=%zu payload_end=%zu -> try resync",
                static_cast<unsigned>(event_id),
                pos,
                descriptors_end,
                payload_end
            );
            ++m_stats.desc_overrun_break;

            std::size_t newPos = 0;
            if (FindNextEventStart(section, payload_end, pos + 1, newPos)) {
                pos = newPos;
                continue;
            }
            sectionParseComplete = false;
            break;
        }

        ParsedEITPacket evt{};
        evt.table_id = table_id;
        evt.service_id = service_id;
        evt.event_id = event_id;
        evt.version = version;
        evt.current_next = current_next;
        evt.section_number = section_number;
        evt.last_section_number = last_section_number;
        evt.transport_stream_id = transport_stream_id;
        evt.original_network_id = original_network_id;
        evt.start_time = start_time_raw;
        evt.duration_hhmmss = duration_hhmmss;
        evt.duration = static_cast<std::uint32_t>(d_h) * 3600u
            + static_cast<std::uint32_t>(d_m) * 60u
            + static_cast<std::uint32_t>(d_s);
        evt.running_status = running_status;
        evt.free_ca_mode = free_ca_mode;

        // ---- table_id理由診断用: イベントフラグ ----
        bool ev_has4d_ok = false;
        bool ev_has4e_seen = false;
        bool ev_has4e_assembled = false;
        bool ev_desc_overrun = false;
        bool ev_4d_drop_len = false;
        bool ev_4d_drop_bounds = false;

        // ★ 4D 候補（日本語優先・空で上書きしない）
        std::vector<uint8_t> name_jpn, text_jpn;
        std::vector<uint8_t> name_any, text_any;

        // ---- 4Eローカル収集: language_code -> collector ----
        std::unordered_map<uint32_t, ExtFragCollector> ext_by_lang;
        ext_by_lang.reserve(2);

        if (descriptors_loop_length > 0) {
            EitEventTextKey key{};
            key.original_network_id = original_network_id;
            key.transport_stream_id = transport_stream_id;
            key.service_id = service_id;
            key.event_id = event_id;
            key.table_id = table_id;
            key.start_time_raw = start_time_raw;
            key.language_code = 0;

            std::size_t desc_pos = descriptors_start;
            while (desc_pos + 2u <= descriptors_end) {
                const uint8_t tag = section[desc_pos];
                const uint8_t dlen = section[desc_pos + 1];

                if (dlen == 0) {
                    ++m_stats.desc_len0_skip;
                    desc_pos += 2u;
                    continue;
                }

                if (desc_pos + 2u + dlen > descriptors_end) {
                    ++m_stats.desc_overrun_break;
                    ev_desc_overrun = true;

                    bool recovered = false;
                    for (std::size_t p = desc_pos + 1; p + 2 <= descriptors_end; ++p) {
                        const uint8_t t = section[p];
                        const uint8_t l = section[p + 1];
                        if (p + 2u + l <= descriptors_end) {
                            if (t == 0x4D || t == 0x4E || t == 0x54 || t == 0x55 || t == 0xD5) {
                                desc_pos = p;
                                recovered = true;
                                break;
                            }
                        }
                    }
                    if (!recovered) break;
                    continue;
                }

                const uint8_t* const body = section + desc_pos + 2u;

                // 0x4D short_event_descriptor
                if (tag == 0x4D && dlen >= 5) {
                    ++m_stats.desc_4d_seen;

                    const std::uint32_t lang =
                        (static_cast<std::uint32_t>(body[0]) << 16) |
                        (static_cast<std::uint32_t>(body[1]) << 8) |
                        (static_cast<std::uint32_t>(body[2]));
                    key.language_code = lang;

                    const uint8_t event_name_length = body[3];

                    constexpr std::size_t kMaxEventNameLen = 255;
                    if (event_name_length > kMaxEventNameLen) {
                        ++m_stats.desc_4d_drop_len;
                        ev_4d_drop_len = true;
                        goto next_desc;
                    }

                    if (4u + event_name_length > dlen) {
                        ++m_stats.desc_4d_drop_bounds;
                        ev_4d_drop_bounds = true;
                        goto next_desc;
                    }

                    const uint8_t* const event_name_char = body + 4;

                    const bool is_jpn = (lang == kLangJpn);
                    if (is_jpn) PreferNonEmptyLonger(name_jpn, event_name_char, event_name_length);
                    else        PreferNonEmptyLonger(name_any, event_name_char, event_name_length);

                    ++m_stats.desc_4d_ok;
                    ev_has4d_ok = true;

                    const std::size_t text_length_offset = 4u + static_cast<std::size_t>(event_name_length);
                    if (text_length_offset >= dlen) goto next_desc;

                    const uint8_t text_length = body[text_length_offset];

                    constexpr std::size_t kMaxShortTextLen = 255;
                    if (text_length > kMaxShortTextLen) {
                        ++m_stats.desc_4d_drop_len;
                        ev_4d_drop_len = true;
                        goto next_desc;
                    }

                    if (text_length_offset + 1u + static_cast<std::size_t>(text_length) > dlen) {
                        ++m_stats.desc_4d_drop_bounds;
                        ev_4d_drop_bounds = true;
                        goto next_desc;
                    }

                    const uint8_t* const text_char = body + text_length_offset + 1u;

                    if (is_jpn) PreferNonEmptyLonger(text_jpn, text_char, text_length);
                    else        PreferNonEmptyLonger(text_any, text_char, text_length);
                }
                // 0x4E extended_event_descriptor
                else if (tag == 0x4E && dlen >= 6) {
                    ev_has4e_seen = true;

                    HandleExtendedEventDescriptor4E(
                        evt,
                        m_textAssembler,
                        m_stats,
                        key,
                        ext_by_lang,
                        body,
                        dlen
                    );
                }
                // 0x54 content_descriptor
                else if (tag == 0x54 && dlen >= 2) {
                    HandleContentDescriptor54(evt, body, dlen);
                }
                // 0xD5 series_descriptor
                else if (tag == 0xD5 && dlen >= 7 && !evt.has_series) {
                    const uint8_t* p = body;
                    std::size_t    remain = dlen;

                    if (remain < 2) goto next_desc;
                    evt.series_id = static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
                    p += 2; remain -= 2;

                    if (remain < 1) goto next_desc;
                    const uint8_t rep_prog_exp = p[0];
                    ++p; --remain;

                    evt.repeat_label = static_cast<uint8_t>((rep_prog_exp >> 4) & 0x0F);
                    evt.program_pattern = static_cast<uint8_t>((rep_prog_exp >> 1) & 0x07);
                    const bool expire_flag = (rep_prog_exp & 0x01) != 0;

                    if (remain < 2) goto next_desc;
                    evt.series_expire_mjd = static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
                    p += 2; remain -= 2;
                    evt.series_expire_valid = expire_flag && (evt.series_expire_mjd != 0xFFFFu);

                    if (remain < 2) goto next_desc;
                    const uint16_t eps_last = static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
                    p += 2; remain -= 2;

                    evt.episode_number = static_cast<uint16_t>((eps_last >> 4) & 0x0FFF);
                    evt.last_episode_number = static_cast<uint16_t>(eps_last & 0x0FFF);

                    if (remain >= 1) {
                        const uint8_t name_len = p[0];
                        ++p; --remain;

                        if (name_len <= remain) {
                            evt.series_name_bytes.assign(p, p + name_len);
                        }
                    }

                    evt.has_series = true;
                }
                // 0x55 parental_rating_descriptor
                else if (tag == 0x55 && dlen >= 4 && !evt.has_parental_rating) {
                    const char c1 = static_cast<char>(body[0]);
                    const char c2 = static_cast<char>(body[1]);
                    const char c3 = static_cast<char>(body[2]);
                    const uint8_t rating = body[3];

                    evt.parental_country.clear();
                    evt.parental_country.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c1)));
                    evt.parental_country.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c2)));
                    evt.parental_country.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c3)));

                    evt.parental_rating = rating;
                    evt.has_parental_rating = true;
                }

            next_desc:
                desc_pos += 2u + dlen;
            }
        }

        // ★ 4D 最終決定：jpn優先、無ければany
        if (!name_jpn.empty()) evt.short_event_name_bytes = std::move(name_jpn);
        else                   evt.short_event_name_bytes = std::move(name_any);

        if (!text_jpn.empty()) evt.short_text_bytes = std::move(text_jpn);
        else                   evt.short_text_bytes = std::move(text_any);

        // descriptor 生データ保持はメモリ負荷が大きいので既定OFF。
        // 必要な解析済み情報は 4D/4E/54/D5/55 で個別保持する。
#if EIT_KEEP_RAW_DESCRIPTORS
        if (descriptors_loop_length > 0) {
            evt.descriptors.assign(section + descriptors_start, section + descriptors_end);
        }
#else
        evt.descriptors.clear();
#endif

        // ---- 4E assemble ----
        if (ev_has4e_seen) {
            ++m_stats.asm_try;

            std::vector<uint8_t> longBytes;
            bool assembled = false;

            auto itJ = ext_by_lang.find(kLangJpn);
            if (itJ != ext_by_lang.end() && itJ->second.CanAssemble()) {
                itJ->second.Assemble(longBytes);
                assembled = true;
            }
            else {
                for (auto& kv : ext_by_lang) {
                    if (kv.second.CanAssemble()) {
                        kv.second.Assemble(longBytes);
                        assembled = true;
                        break;
                    }
                }
            }

            if (!assembled) {
                std::vector<uint8_t> asmBytes;
                EitEventTextKey key{};
                key.original_network_id = original_network_id;
                key.transport_stream_id = transport_stream_id;
                key.service_id = service_id;
                key.event_id = event_id;
                key.table_id = table_id;
                key.start_time_raw = start_time_raw;
                key.language_code = (itJ != ext_by_lang.end()) ? kLangJpn : 0;

                if (m_textAssembler.AssembleAndErase(key, asmBytes)) {
                    longBytes = std::move(asmBytes);
                    assembled = true;
                }
                else {
                    if (!ext_by_lang.empty()) {
                        for (const auto& kv : ext_by_lang) {
                            key.language_code = kv.first;
                            if (m_textAssembler.AssembleAndErase(key, asmBytes)) {
                                longBytes = std::move(asmBytes);
                                assembled = true;
                                break;
                            }
                        }
                    }
                }
            }

            if (assembled && !longBytes.empty()) {
                ++m_stats.asm_success;
                evt.extended_text_bytes = std::move(longBytes);
                ev_has4e_assembled = true;
            }
            else {
                ++m_stats.asm_incomplete;
            }
        }

        NormalizeExtendedItems(evt.extended_items);

        UpdateTidDiag(
            this,
            table_id,
            ev_has4d_ok,
            ev_has4e_seen,
            ev_has4e_assembled,
            ev_desc_overrun,
            ev_4d_drop_len,
            ev_4d_drop_bounds
        );

        parsedEvents.push_back(std::move(evt));

        pos = descriptors_end;
    }

    if (!sectionParseComplete) {
        DebugLogFormat(
            L"[EIT] section解析未完了のため旧スナップショットを維持: "
            L"table_id=0x%02X section=%u version=%u",
            static_cast<unsigned>(table_id),
            static_cast<unsigned>(section_number),
            static_cast<unsigned>(version));
        return;
    }

    if (!CommitSectionSnapshot(sectionKey, version, crc_in_section, parsedEvents)) {
        DebugLogFormat(
            L"[EIT] sectionスナップショット差し替え失敗: "
            L"table_id=0x%02X section=%u version=%u events=%zu",
            static_cast<unsigned>(table_id),
            static_cast<unsigned>(section_number),
            static_cast<unsigned>(version),
            static_cast<std::size_t>(parsedEvents.size()));
        return;
    }

    SegmentKey segmentKey{ original_network_id, transport_stream_id, service_id, table_id };
    auto& st = m_segmentStates[segmentKey];
    if (!st.version_valid) {
        st.version_valid = true;
        st.version_number = version;
    }
    else if (st.version_number != version) {
        ++st.version_changes;
        st.version_number = version;
    }

    st.last_section_number = last_section_number;
    st.segment_last_section_number = segment_last_section_number;
    st.last_table_id = last_table_id;

    if (section_number < st.received_sections.size()) {
        if (!st.received_sections[section_number]) {
            ++st.received_unique;
        }
        st.received_sections[section_number] = true;
        st.section_crc[section_number] = crc_in_section;
    }
}

// ---------------------- イベント単位のチェック＆格納（H/L分離） ----------------------
bool EITFilter::ParseAndCheckPacket(const ParsedEITPacket& packet)
{
    if (!PerformIntegrityCheck(packet)) {
        ++m_integrityDropCount;
        DebugLogFormat(L"[EIT] event_id=%u の整合性チェックに失敗しました", packet.event_id);
        return false;
    }

    if (!PerformErrorCheck(packet)) {
        ++m_errorDropCount;
        DebugLogFormat(L"[EIT] event_id=%u のエラーチェックに失敗しました", packet.event_id);
        return false;
    }

    const bool stored = StoreCheckedPacket(packet);
    if (stored) {
        TrimStoredEventsIfNeeded();
    }
    return stored;
}

// PerformIntegrityCheck / PerformErrorCheck 済みイベントの格納本体。
// section単位コミットから複数イベントを一括反映するときにも使用する。
bool EITFilter::StoreCheckedPacket(const ParsedEITPacket& packet)
{
#if EIT_EXTITEM_DUMP_ENABLE
    if (!packet.extended_text_bytes.empty()) {
        eitdebug::DumpExtendedItemRaw(
            packet.service_id,
            packet.event_id,
            L"[拡張items:extended_text]",
            packet.extended_text_bytes.data(),
            packet.extended_text_bytes.size()
        );
    }
#endif

    const EventKey key = MakeKey(packet);
    bool h_added = false;

    try {
        // 1) H（骨格）格納：必ず
        {
            ParsedEITPacket h = MakeSkeletonH(packet);

            auto itH = m_eventIndexH.find(key);
            if (itH == m_eventIndexH.end()) {
                h_added = true;
                m_eventsH.push_back(std::move(h));
                m_eventIndexH.emplace(key, m_eventsH.size() - 1);

                // merged も同じ index で増やす
                m_eventsMerged.push_back(m_eventsH.back());
                ++m_stats.h_add;
            }
            else {
                auto& dstH = m_eventsH[itH->second];

                // 骨格の更新（上書きOK）
                dstH.version = h.version;
                dstH.current_next = h.current_next;
                dstH.section_number = h.section_number;
                dstH.last_section_number = h.last_section_number;

                dstH.start_time = h.start_time;
                dstH.duration_hhmmss = h.duration_hhmmss;
                dstH.duration = h.duration;

                dstH.running_status = h.running_status;
                dstH.free_ca_mode = h.free_ca_mode;

                ++m_stats.h_update;
            }
        }

        // 2) L（補完）格納：必要なら
        if (HasAnyPatchL(packet)) {
            ParsedEITPacket l = MakePatchL(packet);

            auto itL = m_eventIndexL.find(key);
            if (itL == m_eventIndexL.end()) {
                m_eventsL.push_back(std::move(l));
                m_eventIndexL.emplace(key, m_eventsL.size() - 1);
                ++m_stats.l_add;
            }
            else {
                auto& dstL = m_eventsL[itL->second];
                MergePatchL(dstL, l);
                ++m_stats.l_update;
            }
        }

        // 3) merged 再構築（H index を基準に）
        auto itH = m_eventIndexH.find(key);
        if (itH != m_eventIndexH.end()) {
            RebuildMergedAtIndex(itH->second);
        }

        data_valid = true;

        FlowDiag_OnParsed(this, h_added, m_eventsMerged.size());
        return true;
    }
    catch (const std::bad_alloc&) {
        ++m_errorDropCount;
        DebugLogFormat(L"[EIT][ERROR] ParseAndCheckPacket bad_alloc: event_id=%u H=%zu L=%zu M=%zu",
            packet.event_id,
            static_cast<std::size_t>(m_eventsH.size()),
            static_cast<std::size_t>(m_eventsL.size()),
            static_cast<std::size_t>(m_eventsMerged.size()));
        TrimStoredEventsIfNeeded();
        return false;
    }
    catch (const std::exception&) {
        ++m_errorDropCount;
        DebugLogFormat(L"[EIT][ERROR] ParseAndCheckPacket exception: event_id=%u", packet.event_id);
        return false;
    }
    catch (...) {
        ++m_errorDropCount;
        DebugLogFormat(L"[EIT][ERROR] ParseAndCheckPacket unknown exception: event_id=%u", packet.event_id);
        return false;
    }
}

bool EITFilter::CommitSectionSnapshot(
    const SectionKey& sectionKey,
    uint8_t version,
    uint32_t crc,
    const std::vector<ParsedEITPacket>& parsedEvents)
{
    const SectionSignature incomingSignature{ version, crc };
    auto oldIt = m_sectionSnapshots.find(sectionKey);

    if (oldIt != m_sectionSnapshots.end()) {
        const SectionSnapshot& oldSnapshot = oldIt->second;

        if (oldSnapshot.version == version && oldSnapshot.crc == crc) {
            ++m_stats.section_duplicate_same_crc;
            return true;
        }

        // Versionの数値大小では判定しない。
        // 一度退役した「同じversion・同じCRC」のsectionが再到着した場合だけ、
        // 受信バッファに残った旧sectionの再送と判断して巻き戻りを防ぐ。
        if (std::find(
            oldSnapshot.retired_signatures.begin(),
            oldSnapshot.retired_signatures.end(),
            incomingSignature) != oldSnapshot.retired_signatures.end()) {
            ++m_stats.section_snapshot_stale_replay_drop;
            DebugLogFormat(
                L"[EIT] 退役済みsection再受信を破棄: "
                L"table_id=0x%02X section=%u version=%u crc=0x%08X",
                static_cast<unsigned>(sectionKey.table_id),
                static_cast<unsigned>(sectionKey.section_number),
                static_cast<unsigned>(version),
                static_cast<unsigned>(crc));
            return true;
        }
    }

    // 旧sectionを消す前に、新sectionの全イベントを検査する。
    // 空sectionは正常なスナップショットとして許可する。
    for (const auto& event : parsedEvents) {
        if (!PerformIntegrityCheck(event)) {
            ++m_integrityDropCount;
            return false;
        }
        if (!PerformErrorCheck(event)) {
            ++m_errorDropCount;
            return false;
        }
        if (!(MakeSectionKey(event) == sectionKey) ||
            event.version != version) {
            ++m_errorDropCount;
            DebugLogFormat(
                L"[EIT] section内イベントの所属情報が不一致: "
                L"table_id=0x%02X section=%u event_id=%u",
                static_cast<unsigned>(sectionKey.table_id),
                static_cast<unsigned>(sectionKey.section_number),
                static_cast<unsigned>(event.event_id));
            return false;
        }
    }

    std::vector<EventKey> incomingKeys;
    incomingKeys.reserve(parsedEvents.size());
    std::unordered_set<EventKey, EventKeyHash> incomingKeySet;
    incomingKeySet.reserve(parsedEvents.size() * 2u + 1u);

    for (const auto& event : parsedEvents) {
        const EventKey eventKey = MakeKey(event);
        if (incomingKeySet.insert(eventKey).second) {
            incomingKeys.push_back(eventKey);
        }
    }

    // 新イベントを先に格納する。格納失敗時に旧sectionを先に失わない。
    for (const auto& event : parsedEvents) {
        if (!StoreCheckedPacket(event)) {
            return false;
        }
    }

    std::unordered_set<EventKey, EventKeyHash> oldKeySet;
    std::unordered_set<EventKey, EventKeyHash> removeKeys;
    SectionSnapshot nextSnapshot{};
    nextSnapshot.version = version;
    nextSnapshot.crc = crc;
    nextSnapshot.event_keys = incomingKeys;

    if (oldIt == m_sectionSnapshots.end()) {
        for (const auto& eventKey : incomingKeys) {
            ++m_eventOwnerCounts[eventKey];
        }
        ++m_stats.section_snapshot_add;
    }
    else {
        const SectionSnapshot& oldSnapshot = oldIt->second;
        oldKeySet.reserve(oldSnapshot.event_keys.size() * 2u + 1u);
        for (const auto& eventKey : oldSnapshot.event_keys) {
            oldKeySet.insert(eventKey);
        }

        nextSnapshot.retired_signatures = oldSnapshot.retired_signatures;
        const SectionSignature oldSignature{
            oldSnapshot.version,
            oldSnapshot.crc
        };
        if (!(oldSignature == incomingSignature)) {
            nextSnapshot.retired_signatures.push_back(oldSignature);
            constexpr std::size_t kMaxRetiredSectionSignatures = 4;
            if (nextSnapshot.retired_signatures.size() >
                kMaxRetiredSectionSignatures) {
                const auto removeCount =
                    nextSnapshot.retired_signatures.size() -
                    kMaxRetiredSectionSignatures;
                nextSnapshot.retired_signatures.erase(
                    nextSnapshot.retired_signatures.begin(),
                    nextSnapshot.retired_signatures.begin() +
                    static_cast<std::ptrdiff_t>(removeCount));
            }
        }

        // 旧sectionから消えたEventKeyの所有数を減らす。
        for (const auto& eventKey : oldSnapshot.event_keys) {
            if (incomingKeySet.find(eventKey) != incomingKeySet.end()) {
                continue;
            }

            auto ownerIt = m_eventOwnerCounts.find(eventKey);
            if (ownerIt == m_eventOwnerCounts.end() || ownerIt->second <= 1u) {
                if (ownerIt != m_eventOwnerCounts.end()) {
                    m_eventOwnerCounts.erase(ownerIt);
                }
                removeKeys.insert(eventKey);
            }
            else {
                --ownerIt->second;
            }
        }

        // 新sectionで追加されたEventKeyの所有数を増やす。
        for (const auto& eventKey : incomingKeys) {
            if (oldKeySet.find(eventKey) == oldKeySet.end()) {
                ++m_eventOwnerCounts[eventKey];
            }
        }

        ++m_stats.section_snapshot_replace;
    }

    m_sectionSnapshots[sectionKey] = std::move(nextSnapshot);

    if (!removeKeys.empty()) {
        m_stats.section_snapshot_events_removed += removeKeys.size();
        RemoveEventsByKeySet(removeKeys);
    }

    TrimStoredEventsIfNeeded();
    data_valid = !m_eventsMerged.empty();
    return true;
}

void EITFilter::RemoveEventsByKeySet(
    const std::unordered_set<EventKey, EventKeyHash>& removeKeys)
{
    if (removeKeys.empty()) return;

    auto hEnd = std::remove_if(
        m_eventsH.begin(),
        m_eventsH.end(),
        [&](const ParsedEITPacket& event) {
            return removeKeys.find(MakeKey(event)) != removeKeys.end();
        });
    m_eventsH.erase(hEnd, m_eventsH.end());

    auto lEnd = std::remove_if(
        m_eventsL.begin(),
        m_eventsL.end(),
        [&](const ParsedEITPacket& event) {
            return removeKeys.find(MakeKey(event)) != removeKeys.end();
        });
    m_eventsL.erase(lEnd, m_eventsL.end());

    RebuildEventIndexesAndMerged();
}

void EITFilter::RebuildEventIndexesAndMerged()
{
    m_eventIndexH.clear();
    m_eventIndexH.reserve(m_eventsH.size());
    for (std::size_t i = 0; i < m_eventsH.size(); ++i) {
        m_eventIndexH.emplace(MakeKey(m_eventsH[i]), i);
    }

    m_eventIndexL.clear();
    m_eventIndexL.reserve(m_eventsL.size());
    for (std::size_t i = 0; i < m_eventsL.size(); ++i) {
        m_eventIndexL.emplace(MakeKey(m_eventsL[i]), i);
    }

    m_eventsMerged.clear();
    m_eventsMerged.resize(m_eventsH.size());
    for (std::size_t i = 0; i < m_eventsH.size(); ++i) {
        RebuildMergedAtIndex(i);
    }
}

// ---------------------- 整合性チェック ----------------------
bool EITFilter::PerformIntegrityCheck(const ParsedEITPacket& packet)
{
    if (!IsEITTableId(packet.table_id)) {
        DebugLog(L"[EIT] table_id が不正です (期待値: 0x4E-0x6F)");
        return false;
    }

    if (!packet.current_next) return false;

    if (packet.section_number > packet.last_section_number) {
        DebugLog(L"[EIT] section_number が last_section_number の範囲外です");
        return false;
    }

    if (packet.service_id == 0) {
        DebugLog(L"[EIT] 注意: service_id==0 のイベントを検出しました");
    }
    if (packet.event_id == 0xFFFF) {
        DebugLog(L"[EIT] 注意: event_id==0xFFFF のイベントを検出しました");
    }

    return true;
}

// ---------------------- エラーチェック ----------------------
bool EITFilter::PerformErrorCheck(const ParsedEITPacket& packet)
{
    if (packet.start_time == 0) {
        DebugLogFormat(L"[EIT] 注意: 開始時刻がすべて 0 (event_id=%u)", packet.event_id);
    }

    if (packet.duration_hhmmss > 235959) {
        DebugLogFormat(L"[EIT] 注意: duration(HHMMSS) 異常: %u (event_id=%u)",
            packet.duration_hhmmss, packet.event_id);
    }

    // raw descriptor は通常保持しない設計にするため、空であっても異常扱いしない。

    return true;
}

// ---------------------- 内部: イベント数制御（H/L/Merged 同期） ----------------------
void EITFilter::TrimStoredEventsIfNeeded()
{
    if (m_eventsH.size() > MAX_STORED_EVENTS) {
        const std::size_t removeCount = m_eventsH.size() / 2u;
        FlowDiag_OnTrim(this, removeCount);

        // 削除キー収集（L側も掃除する）
        std::unordered_set<EventKey, EventKeyHash> removed;
        removed.reserve(removeCount * 2);

        for (std::size_t i = 0; i < removeCount; ++i) {
            removed.insert(MakeKey(m_eventsH[i]));
        }

        DebugLogFormat(
            L"[EIT] H over limit -> remove %zu (total=%zu)",
            removeCount, m_eventsH.size()
        );

        m_eventsH.erase(m_eventsH.begin(),
            m_eventsH.begin() + static_cast<std::ptrdiff_t>(removeCount));
        m_eventsMerged.erase(m_eventsMerged.begin(),
            m_eventsMerged.begin() + static_cast<std::ptrdiff_t>(removeCount));

        // rebuild H index
        m_eventIndexH.clear();
        m_eventIndexH.reserve(m_eventsH.size());
        for (std::size_t i = 0; i < m_eventsH.size(); ++i) {
            m_eventIndexH.emplace(MakeKey(m_eventsH[i]), i);
        }

        // remove L that corresponds to removed H
        if (!m_eventsL.empty()) {
            auto end_it = std::remove_if(
                m_eventsL.begin(), m_eventsL.end(),
                [&](const ParsedEITPacket& p) {
                    return removed.find(MakeKey(p)) != removed.end();
                });
            m_eventsL.erase(end_it, m_eventsL.end());

            // rebuild L index
            m_eventIndexL.clear();
            m_eventIndexL.reserve(m_eventsL.size());
            for (std::size_t i = 0; i < m_eventsL.size(); ++i) {
                m_eventIndexL.emplace(MakeKey(m_eventsL[i]), i);
            }
        }
    }

    // L が単独で膨らみ続けるのも防止（必要なら）
    if (m_eventsL.size() > MAX_PATCH_EVENTS) {
        const std::size_t removeCount = m_eventsL.size() / 2u;

        DebugLogFormat(
            L"[EIT] L over limit -> remove %zu (total=%zu)",
            removeCount, m_eventsL.size()
        );

        m_eventsL.erase(m_eventsL.begin(),
            m_eventsL.begin() + static_cast<std::ptrdiff_t>(removeCount));

        m_eventIndexL.clear();
        m_eventIndexL.reserve(m_eventsL.size());
        for (std::size_t i = 0; i < m_eventsL.size(); ++i) {
            m_eventIndexL.emplace(MakeKey(m_eventsL[i]), i);
        }

        // merged 再構築（L掃除で適用対象が消えることがある）
        for (std::size_t i = 0; i < m_eventsH.size(); ++i) {
            RebuildMergedAtIndex(i);
        }
    }
}

// ---------------------- デバッグ: カウンター出力 ----------------------
void EITFilter::DumpDebugCounters() const
{
    DebugLogFormat(L"[EIT] TS packets into filter: %zu", (size_t)m_tsPacketCount);
    DebugLogFormat(L"[EIT] Section starts (PUSI=1): %zu", (size_t)m_sectionStartCount);
    DebugLogFormat(L"[EIT] Completed sections (InputSection): %zu", (size_t)m_sectionCompleteCount);
    DebugLogFormat(L"[EIT] Section drops by size: %zu", (size_t)m_sectionDropBySize);

    DebugLogFormat(L"[EIT] Stored H=%zu, L=%zu, Merged=%zu",
        (size_t)m_eventsH.size(), (size_t)m_eventsL.size(), (size_t)m_eventsMerged.size());

    DebugLogFormat(
        L"[EIT] Section snapshots: active=%zu add=%zu replace=%zu "
        L"staleReplayDrop=%zu removedEvents=%zu",
        static_cast<std::size_t>(m_sectionSnapshots.size()),
        static_cast<std::size_t>(m_stats.section_snapshot_add),
        static_cast<std::size_t>(m_stats.section_snapshot_replace),
        static_cast<std::size_t>(m_stats.section_snapshot_stale_replay_drop),
        static_cast<std::size_t>(m_stats.section_snapshot_events_removed));

    DebugLogFormat(L"[EIT] Integrity drops: %zu", (size_t)m_integrityDropCount);
    DebugLogFormat(L"[EIT] Error drops: %zu", (size_t)m_errorDropCount);

    DebugLogFormat(L"[EIT][STAT] section_total=%zu eit=%zu crc_fail=%zu len_range_drop=%zu len_mismatch_drop=%zu cur_next_drop=%zu dup_same_crc=%zu",
        (size_t)m_stats.section_total, (size_t)m_stats.section_eit,
        (size_t)m_stats.section_crc_fail, (size_t)m_stats.section_len_range_drop,
        (size_t)m_stats.section_len_mismatch_drop, (size_t)m_stats.section_current_next_drop,
        (size_t)m_stats.section_duplicate_same_crc);

    DebugLogFormat(L"[EIT][STAT] desc_len0_skip=%zu desc_overrun_break=%zu 4D seen/ok=%zu/%zu (drop_len=%zu drop_bounds=%zu) 4E seen/ok=%zu/%zu (drop_items=%zu drop_text=%zu frag_reject=%zu)",
        (size_t)m_stats.desc_len0_skip, (size_t)m_stats.desc_overrun_break,
        (size_t)m_stats.desc_4d_seen, (size_t)m_stats.desc_4d_ok,
        (size_t)m_stats.desc_4d_drop_len, (size_t)m_stats.desc_4d_drop_bounds,
        (size_t)m_stats.desc_4e_seen, (size_t)m_stats.desc_4e_ok,
        (size_t)m_stats.desc_4e_drop_items_bounds, (size_t)m_stats.desc_4e_drop_text_bounds,
        (size_t)m_stats.desc_4e_frag_reject);

    DebugLogFormat(L"[EIT][STAT] asm try=%zu success=%zu incomplete=%zu fail=%zu complete_true=%zu",
        (size_t)m_stats.asm_try, (size_t)m_stats.asm_success,
        (size_t)m_stats.asm_incomplete, (size_t)m_stats.asm_fail,
        (size_t)m_stats.asm_complete_true);

    DebugLogFormat(L"[EIT][HL] h_add=%zu h_update=%zu l_add=%zu l_update=%zu merged_rebuild=%zu patch_skip(ver_mismatch)=%zu",
        (size_t)m_stats.h_add, (size_t)m_stats.h_update,
        (size_t)m_stats.l_add, (size_t)m_stats.l_update,
        (size_t)m_stats.merged_rebuild,
        (size_t)m_stats.patch_skipped_version_mismatch);

    {
        std::lock_guard<std::mutex> lk(g_ccTrack_mtx);
        auto it = g_ccTrack.find(this);
        if (it != g_ccTrack.end()) {
            DebugLogFormat(
                L"[EIT] CC discontinuity=%zu, CC errors=%zu",
                (size_t)it->second.discontinuity_count,
                (size_t)it->second.cc_error_count
            );
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_flowDiag_mtx);
        auto it = g_flowDiag.find(this);
        if (it != g_flowDiag.end()) {
            const auto& d = it->second;
            DebugLogFormat(
                L"[EIT][FLOW] parsed_calls=%llu added=%llu updated=%llu trimmed=%llu max_stored=%llu",
                (unsigned long long)d.parsed_calls,
                (unsigned long long)d.added,
                (unsigned long long)d.updated,
                (unsigned long long)d.trimmed,
                (unsigned long long)d.max_stored
            );
        }
    }

    // table_id別診断
    {
        std::lock_guard<std::mutex> lk(g_tidDiag_mtx);
        auto it = g_tidDiag.find(this);
        if (it != g_tidDiag.end()) {
            const auto& arr = it->second;

            for (uint8_t tid = 0x50; tid <= 0x6F; ++tid) {
                const auto& a = arr[tid];
                if (a.ev == 0 && a.has4d_ok == 0 && a.has4e_seen == 0) continue;

                DebugLogFormat(
                    L"[EIT][TID] tid=0x%02X ev=%zu 4D_ok=%zu 4E_seen=%zu 4E_asm=%zu both=%zu only4E=%zu only4D=%zu drop4D(len/bnd)=%zu/%zu overrun=%zu",
                    tid, a.ev, a.has4d_ok, a.has4e_seen, a.has4e_asm,
                    a.both, a.only4e, a.only4d, a.drop4d_len, a.drop4d_bounds, a.desc_overrun
                );
            }
        }
    }
}

// ---------------------- coverage ----------------------
double EITFilter::GetSectionCoverageRatio(uint16_t onid, uint16_t tsid, uint16_t sid, uint8_t table_id) const
{
    auto isScheduleTid = [](uint8_t tid) noexcept -> bool {
        return (tid >= 0x50 && tid <= 0x5F);
        };

    SegmentKey key{ onid, tsid, sid, table_id };
    auto it = m_segmentStates.find(key);
    if (it == m_segmentStates.end()) return 0.0;

    const SegmentState& st = it->second;

    int lastSeen = -1;
    for (int i = 0; i < 256; ++i) {
        if (st.received_sections[i]) lastSeen = i;
    }
    if (lastSeen < 0) return 0.0;

    const std::size_t expectedLast =
        (std::max<std::size_t>)(static_cast<std::size_t>(st.last_section_number),
            static_cast<std::size_t>(lastSeen));

    if (isScheduleTid(table_id) && expectedLast >= 8 && (expectedLast % 8) == 0 && st.segment_last_section_number <= 7) {
        const int expectedSeg = static_cast<int>((expectedLast >> 3) + 1u);
        if (expectedSeg <= 0) return 0.0;

        const int segLast = static_cast<int>(st.segment_last_section_number);

        int completeSeg = 0;
        for (int seg = 0; seg < expectedSeg; ++seg) {
            const int base = seg * 8;
            bool ok = true;
            for (int off = 0; off <= segLast; ++off) {
                const int idx = base + off;
                if (idx < 0 || idx >= 256 || !st.received_sections[idx]) { ok = false; break; }
            }
            if (ok) ++completeSeg;
        }
        return static_cast<double>(completeSeg) / static_cast<double>(expectedSeg);
    }

    const std::size_t total = expectedLast + 1u;
    std::size_t recv = 0;
    for (std::size_t i = 0; i < total; ++i) {
        if (st.received_sections[i]) ++recv;
    }
    return (total == 0) ? 0.0 : (static_cast<double>(recv) / static_cast<double>(total));
}

bool EITFilter::IsEpgSegmentComplete(
    uint16_t original_network_id,
    uint16_t transport_stream_id,
    uint16_t service_id,
    uint8_t  table_id,
    double   threshold
) const
{
    return GetSectionCoverageRatio(original_network_id, transport_stream_id, service_id, table_id) >= threshold;
}

// ---------------------- TSV helper ----------------------
static void WriteUtf8Bom(std::ofstream& ofs)
{
    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    ofs.write(reinterpret_cast<const char*>(bom), 3);
}

static bool NeedWriteHeader(const std::filesystem::path& p)
{
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return true;
    const auto sz = std::filesystem::file_size(p, ec);
    return ec || (sz < 4);
}

static void WriteTsvHeader(std::ofstream& ofs)
{
    ofs <<
        "ROW\tspace\tchannel\tonid\ttsid\tsid\t"
        "ts_packets\tsec_starts\tsec_complete\tsec_drop_by_size\t"
        "stored_events\tintegrity_drop\terror_drop\t"
        "sec_total\tsec_eit\tcrc_fail\tlen_range_drop\tlen_mismatch_drop\tcur_next_drop\tdup_same_crc\t"
        "desc_len0_skip\tdesc_overrun_break\t4d_seen\t4d_ok\t4d_drop_len\t4d_drop_bounds\t"
        "4e_seen\t4e_ok\t4e_drop_items\t4e_drop_text\t"
        "asm_try\tasm_success\tasm_incomplete\tasm_fail\t"
        "cc_discontinuity\tcc_errors\t"
        "A_cov_avg\tA_cov_min\tA_cov_max\tA_tables\t"
        "O_cov_avg\tO_cov_min\tO_cov_max\tO_tables\t"
        "seg_ver\tseg_vchg\tseg_dup\tseg_recv_unique\tseg_last_sec\tseg_last_table_id"
        "\r\n";
}

struct ServiceKey3 {
    uint16_t onid = 0;
    uint16_t tsid = 0;
    uint16_t sid = 0;

    bool operator<(const ServiceKey3& r) const noexcept {
        if (onid != r.onid) return onid < r.onid;
        if (tsid != r.tsid) return tsid < r.tsid;
        return sid < r.sid;
    }
};

static bool IsScheduleTid(uint8_t tid) noexcept {
    return (tid >= 0x50 && tid <= 0x6F);
}

static void CalcCoverageSummaryForService(
    const std::map<SegmentKey, SegmentState>& segs,
    uint16_t onid, uint16_t tsid, uint16_t sid,
    uint8_t tid_begin, uint8_t tid_end,
    double& out_avg, double& out_min, double& out_max, std::size_t& out_tables
)
{
    out_avg = 0.0;
    out_min = 1.0;
    out_max = 0.0;
    out_tables = 0;

    double sum = 0.0;

    for (uint8_t tid = tid_begin; tid <= tid_end; ++tid) {
        SegmentKey k{ onid, tsid, sid, tid };
        auto it = segs.find(k);
        if (it == segs.end()) continue;

        const SegmentState& st = it->second;

        int lastSeen = -1;
        for (int i = 0; i < 256; ++i) {
            if (st.received_sections[i]) lastSeen = i;
        }
        if (lastSeen < 0) continue;

        const std::size_t expectedLast =
            (std::max<std::size_t>)(static_cast<std::size_t>(st.last_section_number),
                static_cast<std::size_t>(lastSeen));

        double r = 0.0;

        if (IsScheduleTid(tid) && expectedLast >= 8 && (expectedLast % 8) == 0 && st.segment_last_section_number <= 7) {
            const int expectedSeg = static_cast<int>((expectedLast >> 3) + 1u);
            const int segLast = static_cast<int>(st.segment_last_section_number);

            int completeSeg = 0;
            for (int seg = 0; seg < expectedSeg; ++seg) {
                const int base = seg * 8;
                bool ok = true;
                for (int off = 0; off <= segLast; ++off) {
                    const int idx = base + off;
                    if (idx < 0 || idx >= 256 || !st.received_sections[idx]) { ok = false; break; }
                }
                if (ok) ++completeSeg;
            }
            r = (expectedSeg <= 0) ? 0.0 : (static_cast<double>(completeSeg) / static_cast<double>(expectedSeg));
        }
        else {
            const std::size_t total = expectedLast + 1u;
            std::size_t recv = 0;
            for (std::size_t i = 0; i < total; ++i) {
                if (st.received_sections[i]) ++recv;
            }
            r = (total == 0) ? 0.0 : (static_cast<double>(recv) / static_cast<double>(total));
        }

        sum += r;
        out_min = (std::min)(out_min, r);
        out_max = (std::max)(out_max, r);
        ++out_tables;
    }

    if (out_tables == 0) {
        out_avg = 0.0;
        out_min = 0.0;
        out_max = 0.0;
        return;
    }
    out_avg = sum / static_cast<double>(out_tables);
}

// ---------------------- TSV: append all channels ----------------------
bool EITFilter::AppendCountersTsv_AllChannels(
    const std::filesystem::path& tsvPath,
    int space,
    int channel
) const
{
    std::lock_guard<std::mutex> filelk(g_eitCountersFileMtx);

    const bool writeHeader = NeedWriteHeader(tsvPath);

    std::ofstream ofs(tsvPath, std::ios::binary | std::ios::app);
    if (!ofs) return false;

    if (writeHeader) {
        WriteUtf8Bom(ofs);
        WriteTsvHeader(ofs);
    }

    // CC統計
    std::size_t cc_disc = 0, cc_err = 0;
    {
        std::lock_guard<std::mutex> lk(g_ccTrack_mtx);
        auto it = g_ccTrack.find(this);
        if (it != g_ccTrack.end()) {
            cc_disc = it->second.discontinuity_count;
            cc_err = it->second.cc_error_count;
        }
    }

    // CHANNEL行
    ofs << "CHANNEL" << "\t"
        << space << "\t" << channel << "\t"
        << "" << "\t" << "" << "\t" << "" << "\t"
        << (std::size_t)m_tsPacketCount << "\t"
        << (std::size_t)m_sectionStartCount << "\t"
        << (std::size_t)m_sectionCompleteCount << "\t"
        << (std::size_t)m_sectionDropBySize << "\t"
        << (std::size_t)m_eventsMerged.size() << "\t"
        << (std::size_t)m_integrityDropCount << "\t"
        << (std::size_t)m_errorDropCount << "\t"
        << (std::size_t)m_stats.section_total << "\t"
        << (std::size_t)m_stats.section_eit << "\t"
        << (std::size_t)m_stats.section_crc_fail << "\t"
        << (std::size_t)m_stats.section_len_range_drop << "\t"
        << (std::size_t)m_stats.section_len_mismatch_drop << "\t"
        << (std::size_t)m_stats.section_current_next_drop << "\t"
        << (std::size_t)m_stats.section_duplicate_same_crc << "\t"
        << (std::size_t)m_stats.desc_len0_skip << "\t"
        << (std::size_t)m_stats.desc_overrun_break << "\t"
        << (std::size_t)m_stats.desc_4d_seen << "\t"
        << (std::size_t)m_stats.desc_4d_ok << "\t"
        << (std::size_t)m_stats.desc_4d_drop_len << "\t"
        << (std::size_t)m_stats.desc_4d_drop_bounds << "\t"
        << (std::size_t)m_stats.desc_4e_seen << "\t"
        << (std::size_t)m_stats.desc_4e_ok << "\t"
        << (std::size_t)m_stats.desc_4e_drop_items_bounds << "\t"
        << (std::size_t)m_stats.desc_4e_drop_text_bounds << "\t"
        << (std::size_t)m_stats.asm_try << "\t"
        << (std::size_t)m_stats.asm_success << "\t"
        << (std::size_t)m_stats.asm_incomplete << "\t"
        << (std::size_t)m_stats.asm_fail << "\t"
        << cc_disc << "\t"
        << cc_err << "\t"
        << "" << "\t" << "" << "\t" << "" << "\t" << "" << "\t"
        << "" << "\t" << "" << "\t" << "" << "\t" << "" << "\t"
        << "" << "\t" << "" << "\t" << "" << "\t" << "" << "\t" << "" << "\t" << ""
        << "\r\n";

    // SERVICE一覧生成
    std::map<ServiceKey3, bool> services;
    for (const auto& kv : m_segmentStates) {
        const SegmentKey& k = kv.first;
        services[{ k.original_network_id, k.transport_stream_id, k.service_id }] = true;
    }

    for (const auto& sv : services) {
        const auto& sk = sv.first;

        double a_avg = 0, a_min = 0, a_max = 0; std::size_t a_tbl = 0;
        double o_avg = 0, o_min = 0, o_max = 0; std::size_t o_tbl = 0;

        CalcCoverageSummaryForService(m_segmentStates, sk.onid, sk.tsid, sk.sid, 0x50, 0x5F, a_avg, a_min, a_max, a_tbl);
        CalcCoverageSummaryForService(m_segmentStates, sk.onid, sk.tsid, sk.sid, 0x60, 0x6F, o_avg, o_min, o_max, o_tbl);

        // 代表segment
        uint8_t rep_tid = 0x50;
        SegmentKey repKey{ sk.onid, sk.tsid, sk.sid, rep_tid };
        auto itRep = m_segmentStates.find(repKey);
        if (itRep == m_segmentStates.end()) {
            for (uint8_t tid = 0x50; tid <= 0x6F; ++tid) {
                SegmentKey k{ sk.onid, sk.tsid, sk.sid, tid };
                auto it = m_segmentStates.find(k);
                if (it != m_segmentStates.end()) { itRep = it; rep_tid = tid; break; }
            }
        }

        unsigned seg_ver = 0;
        std::size_t seg_vchg = 0, seg_dup = 0, seg_recv_unique = 0;
        unsigned seg_last_sec = 0, seg_last_tid = 0;

        if (itRep != m_segmentStates.end()) {
            const SegmentState& st = itRep->second;
            seg_ver = static_cast<unsigned>(st.version_number);
            seg_vchg = static_cast<std::size_t>(st.version_changes);
            seg_dup = static_cast<std::size_t>(st.duplicate_same_crc);
            seg_recv_unique = static_cast<std::size_t>(st.received_unique);
            seg_last_sec = static_cast<unsigned>(st.last_section_number);
            seg_last_tid = static_cast<unsigned>(st.last_table_id);
        }

        // SERVICE行（列数固定）
        ofs << "SERVICE" << "\t"
            << space << "\t" << channel << "\t"
            << sk.onid << "\t" << sk.tsid << "\t" << sk.sid;

        // columns 7..36 (ts_packets..cc_errors) を空にする：30列
        WriteEmptyTabs(ofs, 30);

        // columns 37..50
        ofs << std::fixed << std::setprecision(6)
            << a_avg << "\t" << a_min << "\t" << a_max << "\t" << a_tbl << "\t"
            << o_avg << "\t" << o_min << "\t" << o_max << "\t" << o_tbl << "\t"
            << seg_ver << "\t" << seg_vchg << "\t" << seg_dup << "\t" << seg_recv_unique << "\t"
            << seg_last_sec << "\t" << seg_last_tid
            << "\r\n";
    }

    return true;
}

// ---------------------- UTF-16 TSV header ----------------------
static void WriteU16(std::ofstream& ofs, std::u16string_view s)
{
    ofs.write(reinterpret_cast<const char*>(s.data()),
        static_cast<std::streamsize>(s.size() * sizeof(char16_t)));
}

static void WriteU16Ascii(std::ofstream& ofs, std::string_view s)
{
    std::u16string tmp;
    tmp.reserve(s.size());
    for (unsigned char c : s) tmp.push_back(static_cast<char16_t>(c));
    WriteU16(ofs, tmp);
}

void EITFilter::WriteTsvHeaderU16(std::ofstream& ofs)
{
    WriteU16Ascii(ofs,
        "ROW\tspace\tchannel\tonid\ttsid\tsid\t"
        "ts_packets\tsec_starts\tsec_complete\tsec_drop_by_size\t"
        "stored_events\tintegrity_drop\terror_drop\t"
        "sec_total\tsec_eit\tcrc_fail\tlen_range_drop\t"
        "len_mismatch_drop\tcur_next_drop\tdup_same_crc\t"
        "desc_len0_skip\tdesc_overrun_break\t4d_seen\t4d_ok\t"
        "4d_drop_len\t4d_drop_bounds\t"
        "4e_seen\t4e_ok\t4e_drop_items\t4e_drop_text\t"
        "asm_try\tasm_success\tasm_incomplete\tasm_fail\t"
        "cc_discontinuity\tcc_errors\t"
        "A_cov_avg\tA_cov_min\tA_cov_max\tA_tables\t"
        "O_cov_avg\tO_cov_min\tO_cov_max\tO_tables\t"
        "seg_ver\tseg_vchg\tseg_dup\tseg_recv_unique\t"
        "seg_last_sec\tseg_last_table_id\r\n"
    );
}
