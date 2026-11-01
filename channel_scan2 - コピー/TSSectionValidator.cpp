#include "TSSectionValidator.hpp"
#include "DebugManager.h"

namespace {

    struct Stats {
        std::atomic<uint64_t> nullOrTooSmall{ 0 };
        std::atomic<uint64_t> pointerTooBig{ 0 };
        std::atomic<uint64_t> badSyntaxIndicator{ 0 };
        std::atomic<uint64_t> sectionTooLong{ 0 };
        std::atomic<uint64_t> sectionOverflow{ 0 };
    };

    inline Stats& GetStats() noexcept
    {
        static Stats s;
        return s;
    }

    // 詳細ログ何件まで出すか（0 なら完全サマリのみ）
    constexpr uint64_t VERBOSE_LIMIT_PER_KIND = 10;

    // 条件付きで std::wcerr を出すヘルパ
    inline void LogVerboseOnce(std::atomic<uint64_t>& counter,
        const wchar_t* msg)
    {
        const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= VERBOSE_LIMIT_PER_KIND) {
            std::wcerr << msg << L"\n";
        }
    }

    inline void LogLimited(std::atomic<uint64_t>& counter,
        const wchar_t* msg)
    {
        const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed) + 1;

        // 最初の数十回と、以降は 1000 件ごとに 1 回だけ出す
        if (n <= VERBOSE_LIMIT_PER_KIND || (n % 1000u) == 0u) {
            // DebugLog は printf 形式なので %ls 経由で安全に渡す
            DebugLog(L"%ls", msg);
        }
    }

    // PSI セクションの基本上限
    constexpr uint16_t SMALL_MAX_SECTION_LENGTH = 1021;  // PAT/PMT/SDT/NIT など
    constexpr uint16_t LARGE_MAX_SECTION_LENGTH = 4093;  // EIT などの長セクション用

    // table_id に応じた section_length の上限を返す
    inline uint16_t GetMaxSectionLengthForTable(uint8_t tableId) noexcept
    {
        // --- 小さい PSI: PAT / CAT / PMT / NIT / SDT / BAT など ---
        switch (tableId) {
        case 0x00: // PAT
        case 0x01: // CAT
        case 0x02: // PMT
        case 0x40: // NIT actual
        case 0x41: // NIT other
        case 0x42: // SDT actual
        case 0x46: // SDT other
        case 0x4A: // BAT
            return SMALL_MAX_SECTION_LENGTH;
        default:
            break;
        }

        // --- EIT 系 (present/following + schedule) ---
        // 0x4E,0x4F : present/following actual/other
        // 0x50–0x6F : schedule actual/other
        if (tableId >= 0x4E && tableId <= 0x6F) {
            return LARGE_MAX_SECTION_LENGTH;
        }

        // それ以外の未知/私用テーブルは「長い可能性あり」と見なして広めに許容
        return LARGE_MAX_SECTION_LENGTH;
    }

} // anonymous namespace

namespace TSValidator {

    // pointer_field 付き TS ペイロード用
    bool IsValidSection(const uint8_t* payload, size_t payloadSize)
    {
        auto& st = GetStats();

        if (payload == nullptr || payloadSize < 3) {
            LogVerboseOnce(st.nullOrTooSmall,
                L"[TSValidator] 無効: NULLまたはpayloadSize < 3");
            return false;
        }

        // payload[0] = pointer_field
        const uint8_t pointerField = payload[0];
        if (static_cast<size_t>(pointerField) + 3 > payloadSize) {
            std::wstringstream ss;
            ss << L"[TSValidator] 無効: pointerField + 3 > payloadSize ("
                << static_cast<int>(pointerField) << L" + 3 > " << payloadSize << L")";
            //LogVerboseOnce(st.pointerTooBig, ss.str().c_str());
            return false;
        }

        // pointer_field をスキップした位置が 「table_id から始まるセクション」
        const uint8_t* section = payload + 1 + pointerField;
        const size_t remain = payloadSize - static_cast<size_t>(section - payload);

        // 以降の PSI 部分の検証は Raw 版に任せる
        return IsValidRawSection(section, remain);
    }

    // table_id から始まる「生セクション」用
    bool IsValidRawSection(const uint8_t* section, size_t length)
    {
        auto& st = GetStats();

        if (section == nullptr || length < 3) {
            LogVerboseOnce(st.nullOrTooSmall,
                L"[TSValidator] 無効(Raw): NULLまたはlength < 3");
            return false;
        }

        // section[0] = table_id, section[1..2] に section_length + 各種フラグ
        const uint8_t tableId = section[0];
        const uint8_t b1 = section[1];

        // PSI 系 (PAT/PMT/SDT/NIT/EIT) は基本的に section_syntax_indicator=1
        // （TOT/TDT 等を扱う場合はここを見直す）
        if ((b1 & 0x80) == 0) {
            LogVerboseOnce(st.badSyntaxIndicator,
                L"[TSValidator] 無効(Raw): section_syntax_indicator が 0");
            return false;
        }

        const uint16_t sectionLength =
            static_cast<uint16_t>(((b1 & 0x0F) << 8) | section[2]);

        const uint16_t maxLen = GetMaxSectionLengthForTable(tableId);

        // 0 は異常扱い / テーブル別の上限を超えたら異常
        if (sectionLength == 0 || sectionLength > maxLen) {
            std::wstringstream ss;
            ss << L"[TSValidator] 無効(Raw): sectionLength 範囲外 (len="
                << sectionLength
                << L", table_id=0x"
                << std::hex << std::uppercase << static_cast<int>(tableId)
                << std::dec << L", max=" << maxLen << L")";

            LogVerboseOnce(st.sectionTooLong, ss.str().c_str());
            // 詳細な原因だけファイルに残したい場合は DebugLog にも流しておく
            DebugLog(ss.str().c_str());
            return false;
        }

        // table_id(1) + 長さフィールド(2) + sectionLength がバッファ内に収まるか
        if (static_cast<size_t>(3u + sectionLength) > length) {
            std::wstringstream ss;
            ss << L"[TSValidator] 無効(Raw): セクション長オーバー ("
                << (3u + sectionLength) << L" > " << length << L")";
            LogVerboseOnce(st.sectionOverflow, ss.str().c_str());
            return false;
        }

        return true;
    }

    // ★ ここから追加：EIT 専用の「完成済みセクション」ヘッダ検証
    bool IsValidEITSectionHeader(const uint8_t* section, size_t length)
    {
        auto& st = GetStats();

        // 先頭 3 バイトすらないのは論外
        if (!section || length < 3) {
            LogLimited(st.nullOrTooSmall,
                L"[TSValidator] 無効(EIT-Raw): section 先頭が不正 (null/len<3)");
            return false;
        }

        const uint8_t table_id = section[0];

        // EIT の table_id 範囲：0x4E,0x4F,0x50-0x6F
        const bool isEIT =
            (table_id == 0x4E) ||
            (table_id == 0x4F) ||
            (table_id >= 0x50 && table_id <= 0x6F);

        if (!isEIT) {
            // 「EIT 用としては対象外」なので、ここは統計を増やさずに素直に false
            return false;
        }

        const uint8_t b1 = section[1];
        const uint8_t b2 = section[2];

        // section_syntax_indicator / '0' / reserved のチェック
        const bool section_syntax_indicator = (b1 & 0x80u) != 0;
        const bool zeroBit = (b1 & 0x40u) == 0;
        const uint8_t reservedBits = (b1 & 0x30u) >> 4;

        if (!section_syntax_indicator || !zeroBit || reservedBits != 0x03u) {
            LogLimited(st.badSyntaxIndicator,
                L"[TSValidator] 無効(EIT-Raw): section_syntax_indicator / 0bit / reserved 異常");
            return false;
        }

        const uint16_t section_length =
            static_cast<uint16_t>(((b1 & 0x0Fu) << 8) | b2);

        // EIT は PSI 共通で 12bit 長さ (最大 4095)
        if (section_length > 4095u) {
            LogLimited(
                st.sectionTooLong,
                (std::wstring(L"[TSValidator] 無効(EIT-Raw): sectionLength 範囲外 (") +
                    std::to_wstring(section_length) + L")").c_str()
            );
            return false;
        }

        // 最低限「ヘッダ + CRC」が入っている長さか？（ざっくり 15 バイト以上）
        //   section_length は「この 2 バイトのあとから CRC まで」を含む
        constexpr uint16_t MIN_EIT_SECTION_LENGTH = 15u;
        if (section_length < MIN_EIT_SECTION_LENGTH) {
            LogLimited(
                st.sectionOverflow,
                (std::wstring(L"[TSValidator] 無効(EIT-Raw): sectionLength が短すぎる (") +
                    std::to_wstring(section_length) + L")").c_str()
            );
            return false;
        }

        // 実際に手元にあるバッファ長と突き合わせ
        const size_t needed = static_cast<size_t>(3u) +
            static_cast<size_t>(section_length);

        if (needed > length) {
            LogLimited(
                st.sectionOverflow,
                (std::wstring(L"[TSValidator] 無効(EIT-Raw): sectionLength 超過 (必要=") +
                    std::to_wstring(needed) + L", 実長=" +
                    std::to_wstring(length) + L")").c_str()
            );
            return false;
        }

        // ここで true なら「EIT のセクションヘッダとしては一応成立」
        return true;
    }

    void ResetStats() noexcept
    {
        auto& s = GetStats();
        s.nullOrTooSmall.store(0, std::memory_order_relaxed);
        s.pointerTooBig.store(0, std::memory_order_relaxed);
        s.badSyntaxIndicator.store(0, std::memory_order_relaxed);
        s.sectionTooLong.store(0, std::memory_order_relaxed);
        s.sectionOverflow.store(0, std::memory_order_relaxed);
    }

    void DumpStatsToDebugLog()
    {
        auto& s = GetStats();

        DebugLogFormat(
            L"[TSValidator] summary: null/payload<3=%llu, pointer+3>size=%llu, syntax=%llu, sectionLenInvalid=%llu, lenOverflow=%llu",
            static_cast<unsigned long long>(s.nullOrTooSmall.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(s.pointerTooBig.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(s.badSyntaxIndicator.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(s.sectionTooLong.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(s.sectionOverflow.load(std::memory_order_relaxed))
        );
    }

} // namespace TSValidator
