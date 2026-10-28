// EitTextWriter.cpp
//
// 修正方針
// - 文字コードを「PCロケール依存(SJIS等)」にしない
// - 出力は常に UTF-8 (BOM付き) に統一する
//   ※ std::wofstream + locale("") を廃止し、std::ofstream に UTF-8 変換して書き込む
//
// 注意
// - 読み込み側 (EITTextDumpLoader) が UTF-8 を読める前提（以前 UTF-8 で動いていた想定）
// - Windows 想定：WideCharToMultiByte(CP_UTF8) を使用

#include "EitTextWriter.hpp"

#include <fstream>
#include <iomanip>
#include <locale>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <filesystem>
#include <algorithm>
#include <limits>
#include <cmath>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

#include "EITFilter.hpp"           // ParsedEITPacket
#include "AribDecodecallUnit.hpp"  // ARIB 文字デコード呼び出し用
#include "EITTextDumpLoader.hpp"   // ForEachEITTextEventInDump()

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
  // 非Windowsのフォールバック（必要なら）
#include <codecvt>
#endif

namespace {

    // ---------------------------
    // UTF-8 writer (no locale dependency)
    // ---------------------------
    static std::string WideToUtf8(std::wstring_view ws)
    {
        if (ws.empty()) return {};

#if defined(_WIN32)
        int need = ::WideCharToMultiByte(
            CP_UTF8, 0,
            ws.data(), static_cast<int>(ws.size()),
            nullptr, 0,
            nullptr, nullptr);
        if (need <= 0) return {};

        std::string out;
        out.resize(static_cast<std::size_t>(need));

        int written = ::WideCharToMultiByte(
            CP_UTF8, 0,
            ws.data(), static_cast<int>(ws.size()),
            out.data(), need,
            nullptr, nullptr);
        if (written <= 0) return {};
        if (written != need) out.resize(static_cast<std::size_t>(written));
        return out;
#else
        // deprecated だがフォールバック用途
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> cvt;
        return cvt.to_bytes(ws.data(), ws.data() + ws.size());
#endif
    }

    class Utf8FileWriter
    {
    public:
        Utf8FileWriter(const std::filesystem::path& path, std::ios::openmode mode)
            : ofs_(path, mode | std::ios::binary)
        {
        }

        bool good() const noexcept { return ofs_.good(); }

        void WriteBom()
        {
            static const unsigned char kBom[3] = { 0xEF, 0xBB, 0xBF };
            ofs_.write(reinterpret_cast<const char*>(kBom), 3);
        }

        void WriteW(std::wstring_view ws)
        {
            const std::string s = WideToUtf8(ws);
            if (!s.empty()) {
                ofs_.write(s.data(), static_cast<std::streamsize>(s.size()));
            }
        }

    private:
        std::ofstream ofs_;
    };

    // ---------------------------
    // low helpers
    // ---------------------------
    int BcdToInt(std::uint8_t b) noexcept
    {
        return ((b >> 4) & 0x0F) * 10 + (b & 0x0F);
    }

    std::uint8_t ToBCD(std::uint8_t v) noexcept
    {
        return static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10));
    }

    bool IsValidStartRaw40(std::uint64_t raw40) noexcept
    {
        return raw40 != 0 && raw40 != 0xFFFFFFFFFFull;
    }

    // EitDumpEvent から MJD+BCD(時刻) 40bit を生成
    std::uint64_t MakeRawMjdBcd40(const EitDumpEvent& ev) noexcept
    {
        std::uint64_t v = 0;
        v |= (static_cast<std::uint64_t>(ev.start_mjd >> 8) & 0xFFu) << 32;
        v |= (static_cast<std::uint64_t>(ev.start_mjd & 0xFFu)) << 24;
        v |= (static_cast<std::uint64_t>(ev.start_time_bcd[0])) << 16;
        v |= (static_cast<std::uint64_t>(ev.start_time_bcd[1])) << 8;
        v |= (static_cast<std::uint64_t>(ev.start_time_bcd[2]));
        return (v & 0xFFFFFFFFFFull);
    }

    std::wstring ToHex40_NoComma(std::uint64_t v)
    {
        std::wstring s(10, L'0');
        v &= 0xFFFFFFFFFFull;
        for (int i = 9; i >= 0; --i) {
            unsigned digit = static_cast<unsigned>(v & 0xFu);
            s[i] = (digit < 10)
                ? static_cast<wchar_t>(L'0' + digit)
                : static_cast<wchar_t>(L'A' + (digit - 10));
            v >>= 4;
        }
        return s;
    }

    std::wstring NormalizeHex40(std::wstring hex)
    {
        // "0x" prefix remove
        if (hex.size() >= 2 && hex[0] == L'0' && (hex[1] == L'x' || hex[1] == L'X')) {
            hex = hex.substr(2);
        }
        // trim
        while (!hex.empty() && (hex[0] == L' ' || hex[0] == L'\t')) hex.erase(hex.begin());
        while (!hex.empty() && (hex.back() == L' ' || hex.back() == L'\t' || hex.back() == L'\r' || hex.back() == L'\n')) hex.pop_back();

        // upper
        for (auto& c : hex) {
            if (c >= L'a' && c <= L'f') c = (wchar_t)(c - (L'a' - L'A'));
        }
        // keep last 10
        if (hex.size() > 10) hex = hex.substr(hex.size() - 10);
        // left pad to 10
        if (hex.size() < 10) hex = std::wstring(10 - hex.size(), L'0') + hex;
        return hex;
    }

    int DurationToHhmmss(const EitDumpEvent& ev)
    {
        const int h = BcdToInt(ev.duration_bcd[0]);
        const int m = BcdToInt(ev.duration_bcd[1]);
        const int s = BcdToInt(ev.duration_bcd[2]);
        return h * 10000 + m * 100 + s;
    }

    std::uint32_t DurationSecOf(const EitDumpEvent& ev) noexcept
    {
        const int h = BcdToInt(ev.duration_bcd[0]);
        const int m = BcdToInt(ev.duration_bcd[1]);
        const int s = BcdToInt(ev.duration_bcd[2]);
        if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) return 0;
        const std::uint32_t sec = (std::uint32_t)h * 3600u + (std::uint32_t)m * 60u + (std::uint32_t)s;
        if (sec == 0 || sec > 24u * 3600u) return 0;
        return sec;
    }

    bool DurationNear(const EitDumpEvent& a, const EitDumpEvent& b, std::uint32_t tolSec) noexcept
    {
        const auto da = DurationSecOf(a);
        const auto db = DurationSecOf(b);
        if (da == 0 || db == 0) return false;
        const auto diff = (da > db) ? (da - db) : (db - da);
        return diff <= tolSec;
    }

    std::optional<std::int64_t> StartTimeDeltaSecRough(const EitDumpEvent& a, const EitDumpEvent& b) noexcept
    {
        const auto ra = MakeRawMjdBcd40(a);
        const auto rb = MakeRawMjdBcd40(b);
        if (!IsValidStartRaw40(ra) || !IsValidStartRaw40(rb)) return std::nullopt;

        const std::int64_t dayDiff = (std::int64_t)a.start_mjd - (std::int64_t)b.start_mjd;

        const int hA = BcdToInt(a.start_time_bcd[0]);
        const int mA = BcdToInt(a.start_time_bcd[1]);
        const int sA = BcdToInt(a.start_time_bcd[2]);

        const int hB = BcdToInt(b.start_time_bcd[0]);
        const int mB = BcdToInt(b.start_time_bcd[1]);
        const int sB = BcdToInt(b.start_time_bcd[2]);

        if (hA > 23 || mA > 59 || sA > 59) return std::nullopt;
        if (hB > 23 || mB > 59 || sB > 59) return std::nullopt;

        const std::int64_t secA = (std::int64_t)hA * 3600 + (std::int64_t)mA * 60 + sA;
        const std::int64_t secB = (std::int64_t)hB * 3600 + (std::int64_t)mB * 60 + sB;

        return dayDiff * 86400 + (secA - secB);
    }

    std::int64_t CalcTextScore(const EitDumpEvent& e) noexcept
    {
        std::int64_t score = 0;

        if (!e.short_event_name.empty()) { score += 1000; score += (std::int64_t)e.short_event_name.size(); }
        if (!e.short_event_text.empty()) { score += 400;  score += (std::int64_t)e.short_event_text.size(); }
        if (!e.extended_event_text.empty()) { score += 800; score += (std::int64_t)e.extended_event_text.size(); }

        if (!e.extended_items.empty()) {
            score += (std::int64_t)e.extended_items.size() * 500;
            for (const auto& it : e.extended_items) {
                score += (std::int64_t)it.item_description.size();
                score += (std::int64_t)it.item_text.size() * 2;
            }
        }

        if (e.has_genre) score += 120;
        if (e.has_series) score += 120;
        if (e.has_parental_rating) score += 40;

        score += (std::int64_t)e.descriptors.size();
        return score;
    }

    static std::uint64_t HashItem64(const EitDumpExtendedItem& it) noexcept
    {
        // FNV1a簡易（wstringをUTF-16バイトとして扱う）
        auto fnv1a = [](const wchar_t* p, std::size_t n) noexcept -> std::uint64_t {
            std::uint64_t h = 1469598103934665603ull;
            const std::uint8_t* b = reinterpret_cast<const std::uint8_t*>(p);
            const std::size_t bytes = n * sizeof(wchar_t);
            for (std::size_t i = 0; i < bytes; ++i) { h ^= b[i]; h *= 1099511628211ull; }
            return h;
            };

        std::uint64_t h = 0;
        if (!it.item_description.empty()) h ^= fnv1a(it.item_description.data(), it.item_description.size());
        h ^= 0x9E3779B97F4A7C15ull;
        if (!it.item_text.empty()) h ^= fnv1a(it.item_text.data(), it.item_text.size());
        return h;
    }

    template <typename T>
    void WriteDecNoComma(std::wostream& os, T value)
    {
        os << std::to_wstring(static_cast<unsigned long long>(value));
    }

    // ---------------------------
    // TextDumpLoader -> DumpEvent（既存再利用用）
    // ---------------------------
    bool ConvertTextEventToDumpEvent(const EITTextEvent& src, EitDumpEvent& dst)
    {
        if (!(src.parsed.headerTableId &&
            src.parsed.headerServiceId &&
            src.parsed.headerEventId &&
            src.parsed.headerTsid &&
            src.parsed.headerOnid))
        {
            return false;
        }

        EitDumpEvent ev{};

        ev.table_id = src.tableId;
        ev.service_id = src.serviceId;
        ev.event_id = src.eventId;
        ev.transport_stream_id = src.tsid;
        ev.original_network_id = src.onid;
        ev.version_number = src.version;
        ev.section_number = src.sectionNumber;
        ev.last_section_number = src.lastSectionNumber;
        ev.running_status = src.runningStatus;
        ev.free_ca_mode = src.freeCA;

        ev.short_event_name = src.shortTitle;
        ev.short_event_text = src.shortSummary;

        if (!src.extendedItems.empty()) {
            ev.extended_items.reserve(src.extendedItems.size());
            for (const auto& p : src.extendedItems) {
                EitDumpExtendedItem item;
                item.item_description = p.first;
                item.item_text = p.second;
                ev.extended_items.emplace_back(std::move(item));
            }
        }

        if (src.parsed.genre) {
            ev.has_genre = true;
            ev.genre_primary = src.contentNibbleLevel1;
            ev.genre_secondary = src.contentNibbleLevel2;
            ev.user_nibble1 = src.userNibble1;
            ev.user_nibble2 = src.userNibble2;
        }

        // Start raw hex から start_mjd/bcd を復元
        if (src.parsed.startTime && !src.startTimeRawHex.empty()) {
            try {
                std::wstring hex = NormalizeHex40(src.startTimeRawHex);
                unsigned long long raw40 = std::stoull(hex, nullptr, 16);
                raw40 &= 0xFFFFFFFFFFull;

                ev.start_mjd = static_cast<std::uint16_t>((raw40 >> 24) & 0xFFFFu);
                ev.start_time_bcd[0] = static_cast<std::uint8_t>((raw40 >> 16) & 0xFFu);
                ev.start_time_bcd[1] = static_cast<std::uint8_t>((raw40 >> 8) & 0xFFu);
                ev.start_time_bcd[2] = static_cast<std::uint8_t>(raw40 & 0xFFu);
            }
            catch (...) {}
        }

        // Duration "HH:MM:SS"
        if (src.parsed.duration && !src.durationHHMMSS.empty()) {
            int h = 0, m = 0, s = 0;
#if defined(_MSC_VER)
            if (swscanf_s(src.durationHHMMSS.c_str(), L"%d:%d:%d", &h, &m, &s) == 3)
#else
            if (std::swscanf(src.durationHHMMSS.c_str(), L"%d:%d:%d", &h, &m, &s) == 3)
#endif
            {
                auto clamp2 = [](int v) -> std::uint8_t {
                    if (v < 0) v = 0;
                    if (v > 99) v = 99;
                    return static_cast<std::uint8_t>(v);
                    };
                ev.duration_bcd[0] = ToBCD(clamp2(h));
                ev.duration_bcd[1] = ToBCD(clamp2(m));
                ev.duration_bcd[2] = ToBCD(clamp2(s));
            }
        }

        // series / parental / descriptors はテキストダンプからは復元しない（現仕様）
        dst = std::move(ev);
        return true;
    }

    // ---------------------------
    // Parsed -> DumpEvent（あなたの現状ロジックに合わせる）
    // ---------------------------
    EitDumpEvent MakeDumpEventFromParsed(
        const ParsedEITPacket& src,
        aribdecode::AribDecodecallUnit& decoder
    )
    {
        EitDumpEvent ev{};

        ev.table_id = src.table_id;
        ev.service_id = src.service_id;
        ev.event_id = src.event_id;
        ev.transport_stream_id = src.transport_stream_id;
        ev.original_network_id = src.original_network_id;

        ev.version_number = src.version;
        ev.section_number = src.section_number;
        ev.last_section_number = src.last_section_number;

        // start_time (40bit)
        {
            const std::uint64_t t = (src.start_time & 0xFFFFFFFFFFull);
            ev.start_mjd = static_cast<std::uint16_t>((t >> 24) & 0xFFFFu);
            ev.start_time_bcd[0] = static_cast<std::uint8_t>((t >> 16) & 0xFFu);
            ev.start_time_bcd[1] = static_cast<std::uint8_t>((t >> 8) & 0xFFu);
            ev.start_time_bcd[2] = static_cast<std::uint8_t>(t & 0xFFu);
        }

        // duration_hhmmss -> bcd
        {
            const std::uint32_t d = src.duration_hhmmss;
            const std::uint8_t h = static_cast<std::uint8_t>((d / 10000u) % 100u);
            const std::uint8_t m = static_cast<std::uint8_t>((d / 100u) % 100u);
            const std::uint8_t s = static_cast<std::uint8_t>(d % 100u);
            ev.duration_bcd[0] = ToBCD(h);
            ev.duration_bcd[1] = ToBCD(m);
            ev.duration_bcd[2] = ToBCD(s);
        }

        ev.running_status = src.running_status;
        ev.free_ca_mode = src.free_ca_mode;

        ev.short_event_name = decoder.DecodeToWString(src.short_event_name_bytes);
        ev.short_event_text = decoder.DecodeToWString(src.short_text_bytes);

        if (!src.extended_text_bytes.empty()) {
            ev.extended_event_text = decoder.DecodeToWString(src.extended_text_bytes);
        }

        if (!src.extended_items.empty()) {
            ev.extended_items.reserve(src.extended_items.size());
            for (const auto& it : src.extended_items) {
                EitDumpExtendedItem dumpItem;
                dumpItem.item_description = decoder.DecodeToWString(it.item_description_bytes);
                dumpItem.item_text = decoder.DecodeToWString(it.item_text_bytes);
                ev.extended_items.emplace_back(std::move(dumpItem));
            }
        }

        // descriptor 生バイト列は必要なら保持
        ev.descriptors = src.descriptors;

        // genre/series/parental は Parsed 側に既にある前提ならそれを反映
        if (src.has_genre) {
            ev.has_genre = true;
            ev.genre_primary = src.content_nibble_level1;
            ev.genre_secondary = src.content_nibble_level2;
            ev.user_nibble1 = src.user_nibble1;
            ev.user_nibble2 = src.user_nibble2;
        }
        if (src.has_series) {
            ev.has_series = true;
            ev.series_id = src.series_id;
            ev.repeat_label = src.repeat_label;
            ev.program_pattern = src.program_pattern;
            ev.series_expire_valid = src.series_expire_valid;
            ev.series_expire_mjd = src.series_expire_mjd;
            ev.episode_number = src.episode_number;
            ev.last_episode_number = src.last_episode_number;
            ev.series_name = decoder.DecodeToWString(src.series_name_bytes);
        }
        if (src.has_parental_rating) {
            ev.has_parental_rating = true;
            ev.parental_country = src.parental_country;
            ev.parental_rating = src.parental_rating;
        }

        return ev;
    }

    // ---------------------------
    // 更新マージ用 key
    // ---------------------------
    struct LogicalKey
    {
        std::uint16_t onid = 0;
        std::uint16_t tsid = 0;
        std::uint16_t sid = 0;
        std::uint16_t eid = 0;

        bool operator==(const LogicalKey& o) const noexcept
        {
            return onid == o.onid && tsid == o.tsid && sid == o.sid && eid == o.eid;
        }
    };
    struct LogicalKeyHash
    {
        std::size_t operator()(const LogicalKey& k) const noexcept
        {
            std::uint64_t x = 0;
            x ^= (std::uint64_t)k.onid << 48;
            x ^= (std::uint64_t)k.tsid << 32;
            x ^= (std::uint64_t)k.sid << 16;
            x ^= (std::uint64_t)k.eid;
            x ^= (x >> 33);
            x *= 0xff51afd7ed558ccdULL;
            x ^= (x >> 33);
            return (std::size_t)x;
        }
    };

    struct StrongKey
    {
        LogicalKey lk{};
        std::uint64_t start_raw = 0; // 40bit

        bool operator==(const StrongKey& o) const noexcept
        {
            return lk == o.lk && start_raw == o.start_raw;
        }
    };
    struct StrongKeyHash
    {
        std::size_t operator()(const StrongKey& k) const noexcept
        {
            std::size_t h1 = LogicalKeyHash{}(k.lk);
            std::size_t h2 = std::hash<std::uint64_t>{}(k.start_raw);
            // size_t is 32-bit on Win32 and 64-bit on x64.
            // Retain the original low bits of the hash on either target.
            constexpr std::size_t kMix =
                static_cast<std::size_t>(0x9e3779b97f4a7c15ULL);
            return h1 ^ (h2 + kMix + (h1 << 6) + (h1 >> 2));
        }
    };

    LogicalKey MakeLogicalKey(const EitDumpEvent& e) noexcept
    {
        return LogicalKey{ e.original_network_id, e.transport_stream_id, e.service_id, e.event_id };
    }

    std::uint64_t NormalizeStartRaw40(const EitDumpEvent& e, const EitDumpUpdateOptions& opt) noexcept
    {
        const auto raw = MakeRawMjdBcd40(e);
        if (opt.relaxKeyWhenInvalidStartTime && !IsValidStartRaw40(raw)) return 0;
        return raw;
    }

    // dst を “保持”、src を “新規” として不足埋め＋改善反映
    std::uint64_t MergePreferBetter(EitDumpEvent& dst, const EitDumpEvent& src, const EitDumpUpdateOptions& opt, std::uint64_t& outMergedItems)
    {
        outMergedItems = 0;
        std::uint64_t filled = 0;

        // 欠損埋め（start/duration）
        if (dst.start_mjd == 0 && src.start_mjd != 0) { dst.start_mjd = src.start_mjd; ++filled; }
        if (dst.start_time_bcd[0] == 0 && src.start_time_bcd[0] != 0) { dst.start_time_bcd[0] = src.start_time_bcd[0]; ++filled; }
        if (dst.start_time_bcd[1] == 0 && src.start_time_bcd[1] != 0) { dst.start_time_bcd[1] = src.start_time_bcd[1]; ++filled; }
        if (dst.start_time_bcd[2] == 0 && src.start_time_bcd[2] != 0) { dst.start_time_bcd[2] = src.start_time_bcd[2]; ++filled; }

        if (DurationSecOf(dst) == 0 && DurationSecOf(src) != 0) {
            dst.duration_bcd[0] = src.duration_bcd[0];
            dst.duration_bcd[1] = src.duration_bcd[1];
            dst.duration_bcd[2] = src.duration_bcd[2];
            ++filled;
        }

        // 欠損埋め（テキスト）
        if (dst.short_event_name.empty() && !src.short_event_name.empty()) { dst.short_event_name = src.short_event_name; ++filled; }
        if (dst.short_event_text.empty() && !src.short_event_text.empty()) { dst.short_event_text = src.short_event_text; ++filled; }
        if (dst.extended_event_text.empty() && !src.extended_event_text.empty()) { dst.extended_event_text = src.extended_event_text; ++filled; }

        // descriptor系
        if (!dst.has_genre && src.has_genre) {
            dst.has_genre = true;
            dst.genre_primary = src.genre_primary;
            dst.genre_secondary = src.genre_secondary;
            dst.user_nibble1 = src.user_nibble1;
            dst.user_nibble2 = src.user_nibble2;
            ++filled;
        }
        if (!dst.has_series && src.has_series) {
            dst.has_series = true;
            dst.series_id = src.series_id;
            dst.repeat_label = src.repeat_label;
            dst.program_pattern = src.program_pattern;
            dst.series_expire_valid = src.series_expire_valid;
            dst.series_expire_mjd = src.series_expire_mjd;
            dst.episode_number = src.episode_number;
            dst.last_episode_number = src.last_episode_number;
            dst.series_name = src.series_name;
            ++filled;
        }
        if (!dst.has_parental_rating && src.has_parental_rating) {
            dst.has_parental_rating = true;
            dst.parental_country = src.parental_country;
            dst.parental_rating = src.parental_rating;
            ++filled;
        }

        // 改善版テキスト反映（スコアが増えたら置換）
        if (opt.allowUpgradeIfTextImproves) {
            const auto s0 = CalcTextScore(dst);
            const auto s1 = CalcTextScore(src);
            if (s1 >= s0 + opt.upgradeMinScoreGain) {
                if (src.short_event_name.size() > dst.short_event_name.size()) { dst.short_event_name = src.short_event_name; ++filled; }
                if (src.short_event_text.size() > dst.short_event_text.size()) { dst.short_event_text = src.short_event_text; ++filled; }
                if (src.extended_event_text.size() > dst.extended_event_text.size()) { dst.extended_event_text = src.extended_event_text; ++filled; }
            }
        }

        // 拡張itemsはユニオンマージ
        if (opt.mergeExtendedItems && !src.extended_items.empty()) {
            if (dst.extended_items.empty()) {
                dst.extended_items = src.extended_items;
                ++filled;
            }
            else {
                std::unordered_set<std::uint64_t> seen;
                seen.reserve(dst.extended_items.size() + src.extended_items.size());
                for (const auto& it : dst.extended_items) seen.insert(HashItem64(it));

                for (const auto& it : src.extended_items) {
                    if (dst.extended_items.size() >= opt.maxExtendedItems) break;
                    const auto h = HashItem64(it);
                    if (seen.insert(h).second) {
                        dst.extended_items.push_back(it);
                        ++outMergedItems;
                    }
                }
            }
        }

        // descriptors は空だけ埋める
        if (dst.descriptors.empty() && !src.descriptors.empty()) { dst.descriptors = src.descriptors; ++filled; }

        // version は上げる（整合用）
        if (src.version_number > dst.version_number) { dst.version_number = src.version_number; ++filled; }

        return filled;
    }

    // 既存 logical の start 群から差し替え候補を探す
    std::optional<std::uint64_t> FindReplaceCandidateStart(
        const std::vector<std::uint64_t>& starts,
        const std::unordered_map<StrongKey, EitDumpEvent, StrongKeyHash>& store,
        const LogicalKey& lk,
        const EitDumpEvent& incoming,
        const EitDumpUpdateOptions& opt)
    {
        // incoming の start が有効でないと精度低いので基本やらない
        const auto inRaw = MakeRawMjdBcd40(incoming);
        if (!IsValidStartRaw40(inRaw)) return std::nullopt;

        std::optional<std::uint64_t> best;
        std::int64_t bestAbs = (std::numeric_limits<std::int64_t>::max)();

        for (auto st : starts) {
            StrongKey sk{ lk, st };
            auto it = store.find(sk);
            if (it == store.end()) continue;

            const auto& cur = it->second;

            auto d = StartTimeDeltaSecRough(incoming, cur);
            if (!d) continue;

            const auto absd = (std::int64_t)std::llabs(*d);
            if ((std::uint32_t)absd > opt.replaceStartTimeToleranceSec) continue;

            if (opt.requireDurationNearOnReplace) {
                if (!DurationNear(incoming, cur, opt.replaceDurationToleranceSec)) continue;
            }

            if (absd < bestAbs) {
                bestAbs = absd;
                best = st;
            }
        }

        return best;
    }

    void SortDumpEvents(std::vector<EitDumpEvent>& v)
    {
        std::sort(v.begin(), v.end(), [](const EitDumpEvent& a, const EitDumpEvent& b) {
            if (a.original_network_id != b.original_network_id) return a.original_network_id < b.original_network_id;
            if (a.transport_stream_id != b.transport_stream_id) return a.transport_stream_id < b.transport_stream_id;
            if (a.service_id != b.service_id) return a.service_id < b.service_id;
            const auto ra = MakeRawMjdBcd40(a);
            const auto rb = MakeRawMjdBcd40(b);
            if (ra != rb) return ra < rb;
            if (a.event_id != b.event_id) return a.event_id < b.event_id;
            return a.table_id < b.table_id;
            });
    }

    // 1イベント出力（wstring）を組み立てる
    std::wstring BuildEventDumpText(const EitDumpEvent& ev)
    {
        std::wostringstream oss;

        oss << L"TableID: 0x"
            << std::hex << std::uppercase << std::setw(2) << std::setfill(L'0')
            << static_cast<int>(ev.table_id)
            << std::dec << std::setfill(L' ');

        oss << L", ServiceID: "; WriteDecNoComma(oss, ev.service_id);
        oss << L", EventID: ";   WriteDecNoComma(oss, ev.event_id);
        oss << L", TSID: ";      WriteDecNoComma(oss, ev.transport_stream_id);
        oss << L", ONID: ";      WriteDecNoComma(oss, ev.original_network_id);
        oss << L"\n";

        oss << L"  Version: " << static_cast<int>(ev.version_number)
            << L", Section: " << static_cast<int>(ev.section_number)
            << L"/" << static_cast<int>(ev.last_section_number) << L"\n";

        const std::wstring raw40Hex = ToHex40_NoComma(MakeRawMjdBcd40(ev));
        oss << L"  StartTime(raw MJD+BCD40bit): 0x" << raw40Hex << L"\n";

        const int dur = DurationToHhmmss(ev);
        const int h = (dur / 10000) % 100;
        const int m = (dur / 100) % 100;
        const int s = dur % 100;

        oss << L"  Duration(HH:MM:SS): "
            << std::setw(2) << std::setfill(L'0') << h << L":"
            << std::setw(2) << std::setfill(L'0') << m << L":"
            << std::setw(2) << std::setfill(L'0') << s
            << std::setfill(L' ')
            << L", RunningStatus: " << static_cast<int>(ev.running_status)
            << L", FreeCA: " << (ev.free_ca_mode ? 1 : 0) << L"\n";

        if (!ev.short_event_name.empty()) oss << L"  [短形式タイトル] " << ev.short_event_name << L"\n";
        if (!ev.short_event_text.empty()) oss << L"  [短形式概要] " << ev.short_event_text << L"\n";

        if (!ev.extended_event_text.empty()) oss << L"  [拡張テキスト] " << ev.extended_event_text << L"\n";

        if (!ev.extended_items.empty()) {
            oss << L"  [拡張items]\n";
            int idx = 0;
            for (const auto& item : ev.extended_items) {
                oss << L"    (" << idx++ << L") " << item.item_description << L": " << item.item_text << L"\n";
            }
        }

        if (ev.has_genre) {
            oss << L"  [ジャンル] primary=0x"
                << std::hex << static_cast<int>(ev.genre_primary)
                << L", secondary=0x" << static_cast<int>(ev.genre_secondary)
                << std::dec
                << L" (user1=" << static_cast<int>(ev.user_nibble1)
                << L", user2=" << static_cast<int>(ev.user_nibble2) << L")\n";
        }

        if (ev.has_series) {
            oss << L"  [シリーズ] SeriesID="; WriteDecNoComma(oss, ev.series_id);
            oss << L", Episode=" << static_cast<unsigned>(ev.episode_number)
                << L"/" << static_cast<unsigned>(ev.last_episode_number)
                << L", RepeatLabel=" << static_cast<unsigned>(ev.repeat_label)
                << L", Pattern=" << static_cast<unsigned>(ev.program_pattern);
            if (ev.series_expire_valid) oss << L", ExpireMJD=" << ev.series_expire_mjd;
            oss << L"\n";
            if (!ev.series_name.empty()) oss << L"    SeriesName: " << ev.series_name << L"\n";
        }

        if (ev.has_parental_rating) {
            oss << L"  [年齢制限] Country=" << ev.parental_country
                << L", RatingCode=" << static_cast<unsigned>(ev.parental_rating) << L"\n";
        }

        if (!ev.descriptors.empty()) {
            oss << L"  DescriptorBytes: " << ev.descriptors.size() << L"\n";
            oss << L"  DescriptorsHex:";
            for (auto b : ev.descriptors) {
                oss << L" "
                    << std::hex << std::uppercase << std::setw(2) << std::setfill(L'0')
                    << static_cast<int>(b)
                    << std::dec << std::setfill(L' ');
            }
            oss << L"\n";
        }

        oss << L"\n";
        return oss.str();
    }

} // namespace

// ------------------------------------------------------------
// NEW：更新マージ（Writer外でも使える）
// ------------------------------------------------------------
std::vector<EitDumpEvent> MergeEitDumpEventsForUpdate(
    const std::vector<EitDumpEvent>& existing,
    const std::vector<EitDumpEvent>& incoming,
    const EitDumpUpdateOptions& opt,
    EitDumpUpdateStats& outStats
)
{
    outStats = {};
    outStats.loadedExisting = (std::uint64_t)existing.size();
    outStats.inputNew = (std::uint64_t)incoming.size();

    std::unordered_map<StrongKey, EitDumpEvent, StrongKeyHash> store;
    store.reserve(existing.size() + incoming.size() + 64);

    std::unordered_map<LogicalKey, std::vector<std::uint64_t>, LogicalKeyHash> index;
    index.reserve(existing.size() + 64);

    auto indexAdd = [&](const LogicalKey& lk, std::uint64_t start) {
        auto& v = index[lk];
        if (std::find(v.begin(), v.end(), start) == v.end()) v.push_back(start);
        };
    auto indexRemove = [&](const LogicalKey& lk, std::uint64_t start) {
        auto it = index.find(lk);
        if (it == index.end()) return;
        auto& v = it->second;
        v.erase(std::remove(v.begin(), v.end(), start), v.end());
        if (v.empty()) index.erase(it);
        };

    // 1) 既存を投入（強キー重複は “良い方” を残す）
    for (const auto& e : existing) {
        const auto lk = MakeLogicalKey(e);
        const auto st = NormalizeStartRaw40(e, opt);
        StrongKey sk{ lk, st };

        auto it = store.find(sk);
        if (it == store.end()) {
            store.emplace(sk, e);
            indexAdd(lk, st);
        }
        else {
            const auto s0 = CalcTextScore(it->second);
            const auto s1 = CalcTextScore(e);
            if (s1 > s0) it->second = e;
        }
    }

    // 2) 新規を適用（更新/差し替え/追加）
    for (const auto& e : incoming) {
        const auto lk = MakeLogicalKey(e);
        const auto st = NormalizeStartRaw40(e, opt);
        StrongKey sk{ lk, st };

        // strong一致：更新
        if (auto it = store.find(sk); it != store.end()) {
            std::uint64_t mergedItems = 0;
            const auto filled = MergePreferBetter(it->second, e, opt, mergedItems);
            outStats.filledFields += filled;
            outStats.mergedExtItems += mergedItems;

            if (filled > 0 || mergedItems > 0) outStats.updated++;
            else outStats.unchanged++;
            continue;
        }

        // strong不一致：logical一致の差し替え候補を探す
        if (auto itIdx = index.find(lk); itIdx != index.end()) {
            const auto rep = FindReplaceCandidateStart(itIdx->second, store, lk, e, opt);
            if (rep) {
                StrongKey oldKey{ lk, *rep };
                auto itOld = store.find(oldKey);
                EitDumpEvent oldEv{};
                if (itOld != store.end()) {
                    oldEv = itOld->second;
                    store.erase(itOld);
                }
                indexRemove(lk, *rep);

                EitDumpEvent combined = e;
                {
                    std::uint64_t mergedItems = 0;
                    std::uint64_t filled = 0;
                    filled += MergePreferBetter(combined, oldEv, opt, mergedItems);
                    outStats.filledFields += filled;
                    outStats.mergedExtItems += mergedItems;
                }

                store.emplace(sk, std::move(combined));
                indexAdd(lk, st);

                outStats.replaced++;
                continue;
            }
        }

        // 新規追加
        store.emplace(sk, e);
        indexAdd(lk, st);
        outStats.added++;
    }

    // 3) 出力ベクタ
    std::vector<EitDumpEvent> out;
    out.reserve(store.size());
    for (auto& kv : store) out.push_back(std::move(kv.second));

    SortDumpEvents(out);

    outStats.finalCount = (std::uint64_t)out.size();
    return out;
}

// ------------------------------------------------------------
// 追記型（既存互換）※UTF-8固定
// ------------------------------------------------------------
bool WriteEitDumpTextFile(
    const std::wstring& filePath,
    const std::vector<EitDumpEvent>& events,
    int space,
    int channel)
{
    namespace fs = std::filesystem;
    const fs::path path(filePath);

    // 既存ファイルから “既に書かれているイベント” を作る
    struct EventKey
    {
        std::uint16_t service_id = 0;
        std::uint16_t event_id = 0;
        std::wstring  start_raw_hex;  // 10桁HEX(正規化)

        bool operator==(const EventKey& rhs) const noexcept
        {
            return service_id == rhs.service_id && event_id == rhs.event_id && start_raw_hex == rhs.start_raw_hex;
        }
    };
    struct EventKeyHash
    {
        std::size_t operator()(const EventKey& k) const noexcept
        {
            std::size_t h1 = std::hash<std::uint16_t>{}(k.service_id);
            std::size_t h2 = std::hash<std::uint16_t>{}(k.event_id);
            std::size_t h3 = std::hash<std::wstring>{}(k.start_raw_hex);
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };

    std::unordered_set<EventKey, EventKeyHash> existingKeys;
    bool fileHasContent = false;
    bool needBom = true;

    try {
        if (fs::exists(path)) {
            const auto sz = fs::file_size(path);
            fileHasContent = (sz > 0);
            needBom = (sz == 0);
            if (sz > 0) {
                ForEachEITTextEventInDump(
                    filePath,
                    [&](const EITTextEvent& ev) -> bool
                    {
                        if (!ev.HasHeader()) return true;

                        EventKey key;
                        key.service_id = ev.serviceId;
                        key.event_id = ev.eventId;
                        key.start_raw_hex = NormalizeHex40(ev.startTimeRawHex); // ★正規化
                        existingKeys.insert(std::move(key));
                        return true;
                    });
            }
        }
    }
    catch (...) {
        // 読み込み失敗は追記動作に影響させない
    }

    Utf8FileWriter writer(path, std::ios::app);
    if (!writer.good()) return false;

    if (needBom) writer.WriteBom();

    if (!fileHasContent) {
        std::wostringstream h;
        h << L"# EIT Dump (space=" << space << L", ch=" << channel << L")\n";
        writer.WriteW(h.str());
    }

    for (const auto& ev : events) {
        EventKey key;
        key.service_id = ev.service_id;
        key.event_id = ev.event_id;
        key.start_raw_hex = ToHex40_NoComma(MakeRawMjdBcd40(ev));

        if (existingKeys.find(key) != existingKeys.end()) continue;
        existingKeys.insert(key);

        writer.WriteW(BuildEventDumpText(ev));
    }

    return true;
}

// ------------------------------------------------------------
// NEW：Snapshot（全件書き直し）※UTF-8固定
// ------------------------------------------------------------
bool WriteEitDumpTextFileSnapshot(
    const std::wstring& filePath,
    const std::vector<EitDumpEvent>& events,
    int space,
    int channel)
{
    namespace fs = std::filesystem;
    const fs::path path(filePath);

    Utf8FileWriter writer(path, std::ios::trunc);
    if (!writer.good()) return false;

    // UTF-8 BOM + header
    writer.WriteBom();

    {
        std::wostringstream h;
        h << L"# EIT Dump (space=" << space << L", ch=" << channel << L")\n\n";
        writer.WriteW(h.str());
    }

    for (const auto& ev : events) {
        writer.WriteW(BuildEventDumpText(ev));
    }

    return true;
}

// ------------------------------------------------------------
// 改良版：Parsed -> 既存読み込み -> 更新マージ -> Snapshot 出力
// ------------------------------------------------------------
bool WriteEitDumpTextFileFromParsed(
    const std::wstring& filePath,
    const std::vector<ParsedEITPacket>& parsedEvents,
    int space,
    int channel,
    bool enableAribTrace)
{
    // 1) 既存読み込み（DumpEvent化）
    std::vector<EitDumpEvent> existing;
    existing.reserve(4096);

    try {
        ForEachEITTextEventInDump(
            filePath,
            [&](const EITTextEvent& txtEv) -> bool
            {
                EitDumpEvent ev{};
                if (!ConvertTextEventToDumpEvent(txtEv, ev)) return true;
                existing.emplace_back(std::move(ev));
                return true;
            });
    }
    catch (...) {
        // 無ければ空として扱う
    }

    // 2) 新規 Parsed を DumpEventへ
    aribdecode::AribDecodecallUnit decoder;
    decoder.EnableTraceForTest(enableAribTrace);

    std::vector<EitDumpEvent> incoming;
    incoming.reserve(parsedEvents.size());

    for (const auto& p : parsedEvents) {
        incoming.emplace_back(MakeDumpEventFromParsed(p, decoder));
    }

    // 3) 更新マージ
    EitDumpUpdateOptions uopt{};
    EitDumpUpdateStats ust{};
    auto merged = MergeEitDumpEventsForUpdate(existing, incoming, uopt, ust);

    // 4) Snapshot 出力（更新反映の肝）
    return WriteEitDumpTextFileSnapshot(filePath, merged, space, channel);
}
