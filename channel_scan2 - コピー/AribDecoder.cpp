#include "AribDecoder.hpp"
#include "AribUnicodeTable.hpp"
#include "DebugManager.h"
#include "AribUnicodeTable_Roman.hpp"
#include "AribUnicodeTable_Kana.hpp"
#include "AribUnicodeTable_Latin.hpp"
#include "AribUnicodeTable_Macro.hpp" 
#include "AribUnicodeTable_Symbols.hpp"

#include <atomic>
#include <string>
#include <vector>
#include <cwchar>
#include <locale>
#include <algorithm>
#include <iterator>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

// ------------------------------------------------------------
// AribDecoder ローカルログ抑制スイッチ
// 機能本体には影響させない
// ------------------------------------------------------------
// 
// 0:完全無効（従来経路のみ） 1:有効（従来経路＋追加ログ） 2:有効かつ詳細ログ（追加ログの内容も充実させる）
#ifndef ARIB_LOCAL_ENABLE_START_LOG
#define ARIB_LOCAL_ENABLE_START_LOG 0
#endif
// 0:完全無効（従来経路のみ） 1:有効（従来経路＋追加ログ） 2:有効かつ詳細ログ（追加ログの内容も充実させる）
#ifndef ARIB_LOCAL_ENABLE_PREDECODE_LOG
#define ARIB_LOCAL_ENABLE_PREDECODE_LOG 0
#endif
// 0:完全無効（従来経路のみ） 1:有効（従来経路＋追加ログ） 2:有効かつ詳細ログ（追加ログの内容も充実させる）
#ifndef ARIB_LOCAL_ENABLE_COUNTER_LOG
#define ARIB_LOCAL_ENABLE_COUNTER_LOG 0
#endif
// 0:完全無効（従来経路のみ） 1:有効（従来経路＋追加ログ） 2:有効かつ詳細ログ（追加ログの内容も充実させる）
#ifndef ARIB_LOCAL_ENABLE_GSTATE_INIT_LOG
#define ARIB_LOCAL_ENABLE_GSTATE_INIT_LOG 0
#endif
// 0:完全無効（従来経路のみ） 1:有効（従来経路＋追加ログ） 2:有効かつ詳細ログ（追加ログの内容も充実させる）
#ifndef ARIB_LOCAL_ENABLE_CHAR_LOG
#define ARIB_LOCAL_ENABLE_CHAR_LOG 0
#endif
// 0:完全無効（従来経路のみ） 1:有効（従来経路＋追加ログ） 2:有効かつ詳細ログ（追加ログの内容も充実させる）
#if ARIB_LOCAL_ENABLE_CHAR_LOG
#define ARIB_LOCAL_CHAR_LOGF(...) DebugLogFormat(__VA_ARGS__)
#else
#define ARIB_LOCAL_CHAR_LOGF(...) ((void)0)
#endif
// 0:完全無効（従来経路のみ） 1:有効（従来経路＋追加ログ） 2:有効かつ詳細ログ（追加ログの内容も充実させる）
#ifndef ARIB_LOCAL_ENABLE_G_LOG
#define ARIB_LOCAL_ENABLE_G_LOG 0
#endif

#ifndef ARIB_LOCAL_ENABLE_DEC_LOG
#define ARIB_LOCAL_ENABLE_DEC_LOG 0
#endif

#if ARIB_LOCAL_ENABLE_DEC_LOG
#define ARIB_LOCAL_DEC_LOGF(...) DebugLogFormat(__VA_ARGS__)
#else
#define ARIB_LOCAL_DEC_LOGF(...) ((void)0)
#endif

#ifndef ARIB_LOCAL_ENABLE_DEC_CP_LOG
#define ARIB_LOCAL_ENABLE_DEC_CP_LOG 0
#endif

#if ARIB_LOCAL_ENABLE_DEC_CP_LOG
#define ARIB_LOCAL_DEC_CP_LOGF(...) DebugLogFormat(__VA_ARGS__)
#else
#define ARIB_LOCAL_DEC_CP_LOGF(...) ((void)0)
#endif

// 0:完全無効（従来経路のみ） 1:有効（従来経路＋追加ログ） 2:有効かつ詳細ログ（追加ログの内容も充実させる）
#if ARIB_LOCAL_ENABLE_G_LOG
#define ARIB_LOCAL_G_LOGF(...) DebugLogFormat(__VA_ARGS__)
#else
#define ARIB_LOCAL_G_LOGF(...) ((void)0)
#endif
// 0:完全無効（従来経路のみ） 1:有効（従来経路＋追加ログ） 2:有効かつ詳細ログ（追加ログの内容も充実させる）
#ifndef ARIB_LOCAL_ENABLE_REP_LOG
#define ARIB_LOCAL_ENABLE_REP_LOG 0
#endif

#if ARIB_LOCAL_ENABLE_REP_LOG
#define ARIB_LOCAL_REP_LOGF(...) DebugLogFormat(__VA_ARGS__)
#else
#define ARIB_LOCAL_REP_LOGF(...) ((void)0)
#endif

#ifndef ARIB_LOCAL_ENABLE_DEC_DETAIL_LOG
#define ARIB_LOCAL_ENABLE_DEC_DETAIL_LOG 0
#endif

#if ARIB_LOCAL_ENABLE_DEC_DETAIL_LOG
#define ARIB_LOCAL_DEC_DETAIL_LOGF(...) DebugLogFormat(__VA_ARGS__)
#else
#define ARIB_LOCAL_DEC_DETAIL_LOGF(...) ((void)0)
#endif
// ログマクロ定義
#if ARIB_LOCAL_ENABLE_GSTATE_INIT_LOG
#define ARIB_LOCAL_GSTATE_INIT_LOGF(...) DebugLogFormat(__VA_ARGS__)
#else
#define ARIB_LOCAL_GSTATE_INIT_LOGF(...) ((void)0)
#endif
// ARIB Local ロゴ出力マクロ（必要に応じて内容を充実させる）
#if ARIB_LOCAL_ENABLE_START_LOG
#define ARIB_LOCAL_START_LOG(...)    DebugLog(__VA_ARGS__)
#define ARIB_LOCAL_START_LOGF(...)   DebugLogFormat(__VA_ARGS__)
#else
#define ARIB_LOCAL_START_LOG(...)    ((void)0)
#define ARIB_LOCAL_START_LOGF(...)   ((void)0)
#endif
// ARIB Local デコード前の状態出力マクロ（必要に応じて内容を充実させる）
#if ARIB_LOCAL_ENABLE_PREDECODE_LOG
#define ARIB_LOCAL_PREDECODE_LOG(...)  DebugLogFormat(__VA_ARGS__)
#else
#define ARIB_LOCAL_PREDECODE_LOG(...)  ((void)0)
#endif
// ARIB Local 各種カウンタ出力マクロ（必要に応じて内容を充実させる）
#if ARIB_LOCAL_ENABLE_COUNTER_LOG
#define ARIB_LOCAL_COUNTER_LOG(...)    DebugLog(__VA_ARGS__)
#define ARIB_LOCAL_COUNTER_LOGF(...)   DebugLogFormat(__VA_ARGS__)
#else
#define ARIB_LOCAL_COUNTER_LOG(...)    ((void)0)
#define ARIB_LOCAL_COUNTER_LOGF(...)   ((void)0)
#endif

// Gセット状態ログマクロ（必要に応じて内容を充実させる）
#ifndef ARIB_LOCAL_ENABLE_GSTATE_LOG
#define ARIB_LOCAL_ENABLE_GSTATE_LOG 0
#endif
// トレース用の状態ログマクロ（必要に応じて内容を充実させる）
#ifndef ARIB_LOCAL_ENABLE_TRACE_STATE_LOG
#define ARIB_LOCAL_ENABLE_TRACE_STATE_LOG 0
#endif
// Gセット状態ログマクロ
#if ARIB_LOCAL_ENABLE_GSTATE_LOG
#define ARIB_LOCAL_GSTATE_LOGF(...) DebugLogFormat(__VA_ARGS__)
#else
#define ARIB_LOCAL_GSTATE_LOGF(...) ((void)0)
#endif
// トレース用の状態ログマクロ
#if ARIB_LOCAL_ENABLE_TRACE_STATE_LOG
#define ARIB_LOCAL_TRACE_STATE_LOGF(...) DebugLogFormat(__VA_ARGS__)
#else
#define ARIB_LOCAL_TRACE_STATE_LOGF(...) ((void)0)
#endif

// AribDecoder.cpp のどこか（ファイル先頭付近）
static std::atomic_bool g_arib_trace_state_logged{ false };

// --- 追加: ビルドトグル（既定は無効＝従来経路のみ） ---
#if !defined(ARIBDECODER_ENABLE_KANJI_TABLE)
#define ARIBDECODER_ENABLE_KANJI_TABLE 1
#endif

#if !defined(ARIBDECODER_KANJI_STAGE)
#define ARIBDECODER_KANJI_STAGE 2  // 0:封印, 1:Plane1のみ, 2:Plane1+2
#endif

#ifndef ARIB_USE_YEN_FOR_BACKSLASH
#define ARIB_USE_YEN_FOR_BACKSLASH 1
#endif

#ifndef ARIB_USE_FULLWIDTH_SPACE
#define ARIB_USE_FULLWIDTH_SPACE 0
#endif

// ---- U+FFFD デバッグダンプ機能 ----
//未完成ですぐクラッシュするので常に0にする。
#ifndef ARIBDECODER_ENABLE_UFFFD_DUMP
#define ARIBDECODER_ENABLE_UFFFD_DUMP 0
#endif

#if ARIBDECODER_ENABLE_UFFFD_DUMP

// bytes[0..len) を HEX と ASCII で1行～複数行ログ出力する
static void DumpAribBytesHex(
    const uint8_t* bytes,
    std::size_t    len,
    const wchar_t* contextTag)
{
    if (!bytes || len == 0)
        return;

    // 最大ダンプ長は過剰ログを防ぐために制限しておく（適宜調整）
    constexpr std::size_t kMaxDumpBytes = 256;
    const std::size_t dumpLen = (len > kMaxDumpBytes) ? kMaxDumpBytes : len;

    DebugLogFormat(L"[ARIB][U+FFFD] context=%s, len=%zu (dump=%zu)",
        contextTag ? contextTag : L"(null)",
        len, dumpLen);

    // 16bytesずつ  HEX + ASCII 表示
    std::wstring line;
    line.reserve(128);

    for (std::size_t offset = 0; offset < dumpLen; offset += 16) {
        std::size_t chunkEnd = (offset + 16 <= dumpLen) ? offset + 16 : dumpLen;

        line.clear();
        line.append(L"  ");

        // HEX 部分
        for (std::size_t i = offset; i < chunkEnd; ++i) {
            wchar_t tmp[8];
            swprintf(tmp, 8, L"%02X ", static_cast<unsigned>(bytes[i]));
            line.append(tmp);
        }

        // 列を揃えるための空白パディング
        for (std::size_t i = chunkEnd; i < offset + 16; ++i) {
            line.append(L"   ");
        }

        line.append(L" | ");

        // ASCII 部分（制御コードや非ASCIIは '.' にする）
        for (std::size_t i = offset; i < chunkEnd; ++i) {
            wchar_t ch = (bytes[i] >= 0x20 && bytes[i] < 0x7F)
                ? static_cast<wchar_t>(bytes[i])
                : L'.';
            line.push_back(ch);
        }

        DebugLog(line.c_str());
    }

    if (len > dumpLen) {
        DebugLog(L"  ...(truncated)");
    }
}

#endif // ARIBDECODER_ENABLE_UFFFD_DUMP


namespace decoder {

    // 追加: 未実装 AdditionalSymbols のカウンタ
    static std::atomic<size_t> g_addsym_unknown_char_count{ 0 };

	// ---- 1バイト文字の全角変換ヘルパ ----
    static inline uint32_t ToFullwidthAscii(uint32_t cp) noexcept;

    // ---- translation unit local counters ----
    static std::atomic<size_t> g_dec_start{ 0 };
    static std::atomic<size_t> g_dec_ok{ 0 };
    static std::atomic<size_t> g_dec_err{ 0 };

    // ---- 未実装系のチェック用カウンタ ----
    // DRCS 関連
    static std::atomic<size_t> g_drcs_designate_count{ 0 };  // ESC で DRCS を G に割り当てた回数
    static std::atomic<size_t> g_drcs_char_count{ 0 };       // 実際に DRCS 文字を読もうとした回数

    // AdditionalSymbols 関連
    static std::atomic<size_t> g_addsym_1b_designate_count{ 0 }; // 1バイト AdditionalSymbols を G に割り当てた回数
    static std::atomic<size_t> g_addsym_2b_designate_count{ 0 }; // 2バイト AdditionalSymbols を G に割り当てた回数

    // 未対応 ESC など
    static std::atomic<size_t> g_unknown_esc_count{ 0 };     // 解釈していない ESC シーケンス

    // 置換文字(U+FFFD) を出した回数
    static std::atomic<size_t> g_replacement_char_count{ 0 };

    // --- U+FFFD の種類別カウンタ ---
    // 1B 系
    static std::atomic<size_t> g_rep_1b_InG94{ 0 };        // 1B: G94外
    static std::atomic<size_t> g_rep_1b_decodeFail{ 0 };   // 1B: SafeDecode1B などで cp=0

    // 2B 系
    static std::atomic<size_t> g_rep_2b_short{ 0 };        // 2B: 長さ不足
    static std::atomic<size_t> g_rep_2b_InG94{ 0 };        // 2B: G94外
    static std::atomic<size_t> g_rep_2b_decodeFail{ 0 };   // 2B: 漢字/追加記号テーブルで cp=0

    // ---- C1 制御コードの統計 ----
// 全 C1 の出現回数
    static std::atomic<size_t> g_c1_total{ 0 };
    // サイズ制御系 (SSZ/MSZ/NSZ/SZX)
    static std::atomic<size_t> g_c1_sizeCtl{ 0 };
    // マクロ (0x90–0x9F)
    static std::atomic<size_t> g_c1_macro{ 0 };
    // SS2/SS3
    static std::atomic<size_t> g_c1_ss2{ 0 };
    static std::atomic<size_t> g_c1_ss3{ 0 };
    // 上記どれにも分類されなかった C1
    static std::atomic<size_t> g_c1_unknown{ 0 };

    // DecodeCoreToU32 内の補助系
    static std::atomic<size_t> g_rep_char_nonZone{ 0 };    // GL/GR どちらでもないバイト
    static std::atomic<size_t> g_rep_exception{ 0 };       // 例外保険
    static std::atomic<size_t> g_rep_guard{ 0 };           // guard (= i が進まなかったとき)

    // ---- helpers ----
    static inline bool isC0(uint8_t b) noexcept { return b <= 0x1F; }
    static inline bool isC1(uint8_t b) noexcept { return b >= 0x80 && b <= 0x9F; }
    static inline std::wstring EscapePercents(const std::wstring& s);
    static inline std::wstring EscapePercents(std::wstring_view sv);
    static inline std::string  EscapePercentsA(const std::string& s);
    
    // ---- Forward declarations for safe decoders ----
    static inline uint32_t SafeDecode1B(AribDecoder::CodeSetHint set, uint8_t ix) noexcept;
    static inline uint32_t SafeDecode2B(AribDecoder::CodeSetHint set,
        uint8_t ku_ix, uint8_t ten_ix,
        int plane, uint8_t finalF) noexcept;

	// ---- ラテン拡張・特殊セット用カウンタ ----
    static std::atomic<size_t> g_latinext_unknown_count{ 0 };
    static std::atomic<size_t> g_latinspecial_unknown_count{ 0 };

    // C1 を正しく制御として処理できた
    inline std::atomic<uint64_t> g_c1_control_handled_count{ 0 };

    // 改行など、実際に文字を出力した C1
    inline std::atomic<uint64_t> g_c1_output_char_count{ 0 };

    // 仕様上は C1 だけど、まだ実装していない（＝スキップした）
    inline std::atomic<uint64_t> g_c1_unsupported_count{ 0 };

    // 本当に壊れている / ここに来るはずがない C1 → FFFD に落とした
    inline std::atomic<uint64_t> g_c1_invalid_count{ 0 };

    // ---- G94 用のローカルヘルパ（AdditionalSymbols 展開専用） ----
    static inline bool InG94_local(uint8_t b, bool isGR) noexcept
    {
        // クラス版 AribDecoder::InG94 と同じロジック
        return isGR ? (b >= 0xA1 && b <= 0xFE)
            : (b >= 0x21 && b <= 0x7E);
    }

    static inline uint8_t NormG94_local(uint8_t b, bool isGR) noexcept
    {
        // クラス版 AribDecoder::NormG94 と同じロジック
        return isGR ? static_cast<uint8_t>(b - 0x80) : b;
    }

    static inline uint8_t Idx94_local(uint8_t g94) noexcept
    {
        // クラス版 AribDecoder::Idx94 と同じロジック
        return static_cast<uint8_t>(g94 - 0x21); // 0..93
    }

    // ---- AdditionalSymbols: 1セル→複数コードポイント展開ヘルパ前方宣言 ----
    template <class OutputIt>
    static bool DecodeAdditionalSymbolCellToUTF32(
        AribDecoder::CodeSetHint usedSet,
        bool usedIsGR,
        int usedBytes,
        uint8_t raw1,
        uint8_t raw2,
        OutputIt out) noexcept;

    
   

#if ARIBDECODER_TRACE_GSETS
    static const wchar_t* csName(AribDecoder::CodeSetHint h) noexcept {
        using H = AribDecoder::CodeSetHint;
        switch (h) {
        case H::Alnum:               return L"Alnum(1B)";
        case H::Hiragana:            return L"Hiragana(1B)";
        case H::Katakana:            return L"Katakana(1B)";
        case H::JISX0201_Katakana:   return L"JISX0201-Kata(1B)";
        case H::PropAlnum:           return L"PropAlnum(1B)";
        case H::PropHiragana:        return L"PropHira(1B)";
        case H::PropKatakana:        return L"PropKata(1B)";
        case H::AdditionalSymbols:   return L"AddSymbols";
        case H::LatinExtension:      return L"LatinExt(1B)";      
        case H::LatinSpecial:        return L"LatinSpecial(1B)";  
        case H::JIS0208:             return L"JIS0208(2B)";
        case H::JIS_CompatKanji1:    return L"CompatKanji1(2B)";
        case H::JIS_CompatKanji2:    return L"CompatKanji2(2B)";
        case H::DRCS0_2B:            return L"DRCS0(2B)";
        default:                     return L"(unknown)";
        }
    }

    void AribDecoder::dumpGInit() noexcept {
        ARIB_LOCAL_GSTATE_INIT_LOGF(L"[ARIB][GState] INIT | GL=G%u GR=G%u | G0=%ls G1=%ls G2=%ls G3=%ls",
            (unsigned)m_state.GL, (unsigned)m_state.GR,
            csName(m_state.G[0]), csName(m_state.G[1]), csName(m_state.G[2]), csName(m_state.G[3]));
    }
    void AribDecoder::dumpGAssign(const wchar_t* tag, int gi) noexcept {
        ARIB_LOCAL_GSTATE_LOGF(L"[ARIB][GState] %ls: G%u <- %ls | GL=G%u GR=G%u",
            (tag ? tag : L"ASSIGN"), (unsigned)gi, csName(m_state.G[gi]),
            (unsigned)m_state.GL, (unsigned)m_state.GR);
    }

    static const wchar_t* width1or2(bool is2) { return is2 ? L"2B" : L"1B"; }

    void AribDecoder::dumpGLS(const wchar_t* tag) noexcept
    {
#if ARIBDECODER_TRACE_GSETS
        if (!m_traceG) return;

        const wchar_t* t = tag ? tag : L"LS";

        auto name = [this](int gi) -> const wchar_t* {
            return csName(m_state.G[gi]); // CodeSetHint→ラベル
            };
        auto bw = [this](int gi) -> const wchar_t* {
            return m_state.GInfo[gi].isTwoBytes ? L"2B" : L"1B";
            };
        auto fin = [this](int gi) -> unsigned {
            return static_cast<unsigned>(m_state.GInfo[gi].finalF);
            };

        // ★ SS 表示は SingleGL を優先（-1: 無し）
        const wchar_t* ss =
            (m_state.SingleGL == 2) ? L" | SS=G2" :
            (m_state.SingleGL == 3) ? L" | SS=G3" :
            (m_state.SS2 ? L" | SS=G2" : (m_state.SS3 ? L" | SS=G3" : L""));

        ARIB_LOCAL_GSTATE_LOGF(
            L"[ARIB][GState] %ls | GL=G%u GR=G%u | "
            L"G0=%ls(%ls,F=0x%02X) G1=%ls(%ls,F=0x%02X) G2=%ls(%ls,F=0x%02X) G3=%ls(%ls,F=0x%02X)%ls",
            t,
            static_cast<unsigned>(m_state.GL),
            static_cast<unsigned>(m_state.GR),

            name(0), bw(0), fin(0),
            name(1), bw(1), fin(1),
            name(2), bw(2), fin(2),
            name(3), bw(3), fin(3),

            ss
        );
#endif
    }






#endif // ARIBDECODER_TRACE_GSETS

    int AribDecoder::bytesPerChar(CodeSetHint h) noexcept {
        switch (h) {
        case CodeSetHint::Alnum:
        case CodeSetHint::Hiragana:
        case CodeSetHint::Katakana:
        case CodeSetHint::PropAlnum:
        case CodeSetHint::PropHiragana:
        case CodeSetHint::PropKatakana:
        case CodeSetHint::JISX0201_Katakana:
            return 1;
        case CodeSetHint::JIS0208:
        case CodeSetHint::JIS_CompatKanji1:
        case CodeSetHint::JIS_CompatKanji2:
        case CodeSetHint::DRCS0_2B:
            return 2;
        case CodeSetHint::AdditionalSymbols:
			return 2;
        default:
            return 1;
        }
    }

    // ============== Reset ==============
    void AribDecoder::Reset() noexcept {
        ARIB_LOCAL_START_LOG(L"[ARIB][START]Resetの実行\n");

		// Gセット初期化
        m_state.G[0] = CodeSetHint::JIS_CompatKanji1;
        m_state.G[1] = CodeSetHint::Alnum;
        m_state.G[2] = CodeSetHint::Hiragana;
        m_state.G[3] = CodeSetHint::Katakana;

        m_state.GInfo[0] = { CodeSetHint::JIS_CompatKanji1, true,  false, 'B' };
        m_state.GInfo[1] = { CodeSetHint::Alnum,            false, false, 'J' };
        m_state.GInfo[2] = { CodeSetHint::Hiragana,         false, false,  0 };
        m_state.GInfo[3] = { CodeSetHint::Katakana,         false, false,  1 };

        m_state.GL = 0;
        m_state.GR = 2;
        m_state.SS2 = m_state.SS3 = false;

#if ARIBDECODER_TRACE_GSETS
        dumpGInit();
#endif
    }

    void AribDecoder::ResetStateForString() noexcept {
        m_state.SingleGL = -1;
        m_state.SS2 = false;
        m_state.SS3 = false;
        m_state.GL = 0;
        m_state.GR = 2;
    }

    

    // ============== LS / Designate / ESC ==============
    void AribDecoder::applyLockShift(uint8_t code) noexcept
    {
        // ISO-2022 系列に準拠したロック/シフトの反映
        // C0: 0x0E(SO)=LS1, 0x0F(SI)=LS0, 0x19=SS2, 0x1D=SS3
        // C1: 0x6E=LS2, 0x6F=LS3, 0x7C=LS2R, 0x7D=LS3R, 0x7E=LS1R, 0x8E=SS2, 0x8F=SS3
        switch (code) {
            // --- Locking Shift (GL 側) ---
        case 0x0E: // LS1: GL=G1
            m_state.GL = 1;
            dumpGLS(L"LS1");
            return;
        case 0x0F: // LS0: GL=G0
            m_state.GL = 0;
            dumpGLS(L"LS0");
            return;
        case 0x6E: // LS2: GL=G2
            m_state.GL = 2;
            dumpGLS(L"LS2");
            return;
        case 0x6F: // LS3: GL=G3
            m_state.GL = 3;
            dumpGLS(L"LS3");
            return;

            // --- Locking Shift Right (GR 側) ---
        case 0x7C: // LS2R: GR=G2
            m_state.GR = 2;
            dumpGLS(L"LS2R");
            return;
        case 0x7D: // LS3R: GR=G3
            m_state.GR = 3;
            dumpGLS(L"LS3R");
            return;
        case 0x7E: // LS1R: GR=G1
            m_state.GR = 1;
            dumpGLS(L"LS1R");
            return;

            // --- Single Shift (次の1文字だけ GL=G2/G3 として扱う) ---
        case 0x19: // SS2 (C0)
        case 0x8E: // SS2 (C1)
            m_state.SS2 = true;
            m_state.SS3 = false;
            m_state.SingleGL = 2;   // 直後1文字のみ GL=G2 として解釈
            dumpGLS(L"SS2");
            return;
        case 0x1D: // SS3 (C0)
        case 0x8F: // SS3 (C1)
            m_state.SS2 = false;
            m_state.SS3 = true;
            m_state.SingleGL = 3;   // 直後1文字のみ GL=G3 として解釈
            dumpGLS(L"SS3");
            return;

        default:
            // 対象外
            return;
        }
    }


    void AribDecoder::applyDesignateSequence(uint8_t first, const uint8_t* seq, std::size_t n) noexcept
    {
        if (n == 0)
            return;

        // 判定：
        //   twoBytes = "$" で導入された（= 2バイトG）
        //   oneByte DRCS = "<intro> 20 F" （n>=2 && seq[0]==0x20）
        //   twoByte DRCS = "24 <intro> 20 F"（n>=3 && seq[1]==0x20）
        const bool twoBytes = (first == 0x24);
        const bool oneDRCS = (!twoBytes && n >= 2 && seq[0] == 0x20);
        const bool twoDRCS = (twoBytes && n >= 3 && seq[1] == 0x20);

        // ---- G スロット決定 ----
        int gi = -1;
        if (twoBytes) {
            if (n >= 2) {
                // ESC 24 <intro> F / ESC 24 <intro> 20 F
                gi = AribDecoder::gIndexFromIntroducer(seq[0]); // <intro>
            }
            else {
                // ★ ESC 24 F （= G0 2バイト集合を直接指定）を G0 に割り当てる
                //   例: ESC 24 3B → G0 <- AdditionalSymbols
                gi = 0;
            }
        }
        else {
            // 1B の場合は first 自体が intro
            gi = AribDecoder::gIndexFromIntroducer(first);
        }

        if (gi < 0) {
            if (m_traceG) {
                const uint8_t intro = (twoBytes && n >= 1) ? seq[0] : first;
                ARIB_LOCAL_G_LOGF(
                    L"[ARIB][G] Designate: invalid introducer (first=0x%02X, intro=0x%02X, n=%zu)",
                    (unsigned)first, (unsigned)intro, n
                );
            }
            return;
        }

        // F 終端（最後の 1 バイト）
        const uint8_t fin = seq[n - 1];

        // ---- DRCS（最小限：DRCS-0 2B のみ対応）----
        if (oneDRCS || twoDRCS) {
            // DRCS-0: F=0x40('@') → ここでは 2 バイト集合として扱う
            const bool is2B = twoDRCS || (fin == 0x40); // DRCS-0 は 2B
            const bool isDR = true;

            g_drcs_designate_count.fetch_add(1, std::memory_order_relaxed);

            CodeSetHint hint = CodeSetHint::DRCS0_2B;   // 現状は DRCS-0 のみ
            m_state.GInfo[gi] = { hint, is2B, isDR, fin };
            m_state.G[gi] = hint;

#if ARIBDECODER_TRACE_GSETS
            dumpGAssign(is2B ? L"DESIGNATE(DRCS,2B)" : L"DESIGNATE(DRCS,1B)", gi);
#endif
            if (m_traceG) {
                ARIB_LOCAL_G_LOGF(
                    L"[ARIB][G] Designate: G%u <- DRCS (F=0x%02X, twoByte=%d)",
                    (unsigned)gi, (unsigned)fin, is2B ? 1 : 0
                );
            }
            return;
        }

        // ---- 通常（1B / 2B）----
        if (twoBytes) {
            // 2B 集合
            CodeSetHint hint = CodeSetHint::JIS0208;

            // 表に基づき拡張（互換漢字1/2、追加記号など）
            if (fin == 'B' || fin == '@' || fin == '9')
                hint = CodeSetHint::JIS_CompatKanji1;   // 漢字/互換面1
            else if (fin == 'D' || fin == ':')
                hint = CodeSetHint::JIS_CompatKanji2;   // 互換面2
            else if (fin == ';')
                hint = CodeSetHint::AdditionalSymbols;  // 追加シンボル（G94 2B）

            // 2B AdditionalSymbols 指定を検知
            if (hint == CodeSetHint::AdditionalSymbols) {
                g_addsym_2b_designate_count.fetch_add(1, std::memory_order_relaxed);
            }

            m_state.GInfo[gi] = { hint, true, false, fin };
            m_state.G[gi] = hint;

#if ARIBDECODER_TRACE_GSETS
            dumpGAssign(L"DESIGNATE(2B)", gi);
#endif
            if (m_traceG) {
                ARIB_LOCAL_G_LOGF(
                    L"[ARIB][G] Designate: G%u <- %ls (final=0x%02X, twoByte=1)",
                    (unsigned)gi, csName(m_state.G[gi]), (unsigned)fin
                );
            }
            return;
        }

        // 1B 集合
        CodeSetHint hint = CodeSetHint::AdditionalSymbols; // デフォルトは「謎 1B = AdditionalSymbols 扱い」
        switch (fin) {
        case 'J':
        case 'B':
            hint = CodeSetHint::Alnum;             // 英数
            break;
        case '0':
            hint = CodeSetHint::Hiragana;          // ひらがな
            break;
        case '1':
            hint = CodeSetHint::Katakana;          // カタカナ
            break;
        case '6':
            hint = CodeSetHint::PropAlnum;         // プロポ英数
            break;
        case '7':
            hint = CodeSetHint::PropHiragana;      // プロポひら
            break;
        case '8':
            hint = CodeSetHint::PropKatakana;      // プロポカナ
            break;
        case 'I':
            hint = CodeSetHint::JISX0201_Katakana; // JIS X0201 カナ
            break;
        default:
            // AdditionalSymbols のまま → ログ用にカウント
            ++g_addsym_1b_designate_count;
            break;
        }

        // --- ラテン拡張／特殊（ABNT 系）を後段で判定 ---
#if ARIBDECODER_ENABLE_LATIN_SETS
        if (hint == CodeSetHint::AdditionalSymbols) {
            if (ARIBDECODER_LATINEXT_FINAL != 0x00 &&
                fin == ARIBDECODER_LATINEXT_FINAL) {
                hint = CodeSetHint::LatinExtension;
            }
            else if (ARIBDECODER_LATINSPECIAL_FINAL != 0x00 &&
                fin == ARIBDECODER_LATINSPECIAL_FINAL) {
                hint = CodeSetHint::LatinSpecial;
            }
        }
#endif

        // 1B AdditionalSymbols 指定を検知
        if (hint == CodeSetHint::AdditionalSymbols) {
            //g_addsym_1b_designate_count.fetch_add(1, std::memory_order_relaxed);
        }

        m_state.GInfo[gi] = { hint, false, false, fin };
        m_state.G[gi] = hint;

#if ARIBDECODER_TRACE_GSETS
        dumpGAssign(L"DESIGNATE(1B)", gi);
#endif
        if (m_traceG) {
            ARIB_LOCAL_G_LOGF(
                L"[ARIB][G] Designate: G%u <- %ls (final=0x%02X, twoByte=0)",
                (unsigned)gi, csName(m_state.G[gi]), (unsigned)fin
            );
        }
    }







    std::size_t AribDecoder::handleESC(const uint8_t* data, std::size_t length,
        std::size_t /*globalPos*/, std::vector<ControlEvent>* out) noexcept
    {
        if (length == 0) return 0;
        const uint8_t a = data[0];

        // --- Locking Shift (GL) ---
        if (a == 'n' || a == 'o') { // 0x6E/0x6F
            m_state.GL = (a == 'n' ? 2 : 3); // LS2 / LS3
#if ARIBDECODER_TRACE_GSETS
            dumpGLS(L"LS2/LS3");
#endif
            if (m_traceG) {
                ARIB_LOCAL_G_LOGF(L"[ARIB][G] LS(GL) | GL=G%u GR=G%u (ESC=0x%02X)",
                    (unsigned)m_state.GL, (unsigned)m_state.GR, (unsigned)a);
            }
            if (out) { ControlEvent ev{ ControlEvent::LSx, 0, {0x1B,a,0,0}, 2 }; out->push_back(ev); }
            return 1;
        }

        // --- Locking Shift Right (GR) ---
        if (a == '|' || a == '}' || a == '~') { // 0x7C / 0x7D / 0x7E
            // 0x7C='|'→LS3R→GR=G3, 0x7D='}'→LS2R→GR=G2, 0x7E='~'→LS1R→GR=G1
            m_state.GR = (a == '|' ? 3 : (a == '}' ? 2 : 1));
#if ARIBDECODER_TRACE_GSETS
            dumpGLS(L"LSxR");
#endif
            if (m_traceG) {
                ARIB_LOCAL_G_LOGF(L"[ARIB][G] LS(GR) | GL=G%u GR=G%u (ESC=0x%02X)",
                    (unsigned)m_state.GL, (unsigned)m_state.GR, (unsigned)a);
            }
            if (out) { ControlEvent ev{ ControlEvent::LSx, 0, {0x1B,a,0,0}, 2 }; out->push_back(ev); }
            return 1;
        }

        // --- Designate（1B / 2B / DRCS）---
        // 形式:
        //   1B: ESC <intro(28/29/2A/2B)> [0x20] F
        //   2B: ESC 0x24 <intro(28/29/2A/2B)> [0x20] F
        if (a == 0x24 || a == 0x28 || a == 0x29 || a == 0x2A || a == 0x2B) {
            // two-byte? (= first==0x24)
            if (a == 0x24) {

                // ★ 拡張: ESC 24 F（イントロ無し → G0 に 2バイト集合を指定）
                if (length >= 2) {
                    const uint8_t b1 = data[1];
                    const bool hasIntro =
                        (b1 == 0x28 || b1 == 0x29 || b1 == 0x2A || b1 == 0x2B); // '(',')','*','+'

                    if (!hasIntro) {
                        // ESC 24 F 形式: G0 を 2バイト集合(F)に指定
                        // 例: ESC 24 3B → G0 = AdditionalSymbols(2B, F=';')
                        applyDesignateSequence(a, &data[1], 1);
                        if (out) {
                            ControlEvent ev{
                                ControlEvent::Designate,
                                0,
                                { 0x1B, a, b1, 0 },
                                2  // ESC の後ろ 2 バイト消費
                            };
                            out->push_back(ev);
                        }
                        return 2;
                    }
                }

                // 2B通常: ESC 24 <intro> F         → 最低3バイト必要
                // 2B DRCS: ESC 24 <intro> 20 F     → 最低4バイト必要
                if (length >= 4 && data[2] == 0x20) {
                    // pass n=3 bytes "<intro,0x20,F>"
                    applyDesignateSequence(a, &data[1], 3);
                    if (out) {
                        ControlEvent ev{
                            ControlEvent::Designate,
                            0,
                            { 0x1B, a, data[1], data[3] },
                            4
                        };
                        out->push_back(ev);
                    }
                    return 4;
                }
                else if (length >= 3) {
                    // pass n=2 bytes "<intro,F>"
                    applyDesignateSequence(a, &data[1], 2);
                    if (out) {
                        ControlEvent ev{
                            ControlEvent::Designate,
                            0,
                            { 0x1B, a, data[1], data[2] },
                            3
                        };
                        out->push_back(ev);
                    }
                    return 3;
                }
                else {
                    g_unknown_esc_count.fetch_add(1, std::memory_order_relaxed);  // ★追加
                    // 不完全
                    if (out) {
                        ControlEvent ev{
                            ControlEvent::ESC,
                            0,
                            { 0x1B, a, 0, 0 },
                            2
                        };
                        out->push_back(ev);
                    }
                    return 1;
                }
            }
            else {
                // 1B通常: ESC <intro> F            → 最低2バイト
                // 1B DRCS: ESC <intro> 20 F        → 最低3バイト
                if (length >= 3 && data[1] == 0x20) {
                    // pass n=2 bytes "<0x20,F>" ただし first=intro として渡す
                    applyDesignateSequence(a, &data[1], 2);
                    if (out) {
                        ControlEvent ev{
                            ControlEvent::Designate,
                            0,
                            { 0x1B, a, 0x20, data[2] },
                            3
                        };
                        out->push_back(ev);
                    }
                    return 3;
                }
                else if (length >= 2) {
                    // pass n=1 bytes "<F>"
                    applyDesignateSequence(a, &data[1], 1);
                    if (out) {
                        ControlEvent ev{
                            ControlEvent::Designate,
                            0,
                            { 0x1B, a, data[1], 0 },
                            2
                        };
                        out->push_back(ev);
                    }
                    return 2;
                }
                else {
                    if (out) {
                        ControlEvent ev{
                            ControlEvent::ESC,
                            0,
                            { 0x1B, a, 0, 0 },
                            2
                        };
                        out->push_back(ev);
                    }
                    return 1;
                }
            }
        }


        // unknown ESC
        if (out) { ControlEvent ev{ ControlEvent::ESC, 0, {0x1B,a,0,0}, 2 }; out->push_back(ev); }
        return 1;
    }


    // ============== Mapping ==============
    

    uint32_t AribDecoder::MapCharFromGSet(
        CodeSetHint set,
        bool        isGR,
        const uint8_t* data,
        std::size_t& i,
        std::size_t    len) noexcept
    {
        ARIB_LOCAL_START_LOG(L"[ARIB][START]MapCharFromGSetの実行\n");
        // どの G スロットを使っているかを “状態” から決める
        const int gi = isGR ? static_cast<int>(m_state.GR)
            : static_cast<int>(m_state.GL);

        // Designate の結果（1B/2B）を優先する
        const bool twoBytes = m_state.GInfo[gi].isTwoBytes;
        // ★ bytesPerChar(set) を使う案はコメントアウトで保持
        // const bool twoBytes = (bytesPerChar(set) == 2);

        // 安全に先読みするヘルパ
        auto peek = [&](std::size_t k) -> uint8_t {
            return (i + k < len) ? data[i + k] : 0;
            };

        // ===================== 1バイト集合 =====================
        if (!twoBytes) {
            if (i >= len) {
                // 1バイト足りない → guard 的な扱い
                return 0xFFFDu;
            }

            const uint8_t b = peek(0);

            // ゾーン外（GL/GR 毎の 0x21..0x7E 以外）
            if (!InG94(b, isGR)) {
                // ★ 種類別カウンタ（1B / InG94）
                g_replacement_char_count.fetch_add(1, std::memory_order_relaxed);
                g_rep_1b_InG94.fetch_add(1, std::memory_order_relaxed);

#if ARIBDECODER_TRACE_REPLACEMENTS
                if (m_traceG) {
                    const int gi2 = isGR ? (int)m_state.GR : (int)m_state.GL;
                    const GSetInfo& ginfo2 = m_state.GInfo[gi2];
                    ARIB_LOCAL_REP_LOGF(
                        L"[ARIB-REP] stage=Map1B-InG94 zone=%ls G%u=%ls(%ls,F=0x%02X) raw=%02X",
                        isGR ? L"GR" : L"GL",
                        (unsigned)gi2,
                        csName(set),
                        ginfo2.isTwoBytes ? L"2B" : L"1B",
                        (unsigned)ginfo2.finalF,
                        (unsigned)b
                    );
                }
#endif
                ++i;
                return 0xFFFDu;
            }

            const uint8_t g94 = NormG94(b, isGR); // → 0x21..0x7E
            const uint8_t ix = Idx94(g94);       // → 0..93

            // ★ 1バイト集合の基礎デコード（0=失敗）
            uint32_t cp = SafeDecode1B(set, ix);

            if (!cp) {
                // ★ SafeDecode1B で失敗した場合のみ ここで U+FFFD 決定
                g_replacement_char_count.fetch_add(1, std::memory_order_relaxed);
                g_rep_1b_decodeFail.fetch_add(1, std::memory_order_relaxed);

#if ARIBDECODER_TRACE_REPLACEMENTS
                if (m_traceG) {
                    const int gi2 = isGR ? (int)m_state.GR : (int)m_state.GL;
                    const GSetInfo& ginfo2 = m_state.GInfo[gi2];
                    ARIB_LOCAL_REP_LOGF(
                        L"[ARIB-REP] stage=Map1B-cp0 zone=%ls G%u=%ls(%ls,F=0x%02X) ix=%u raw=%02X",
                        isGR ? L"GR" : L"GL",
                        (unsigned)gi2,
                        csName(set),
                        ginfo2.isTwoBytes ? L"2B" : L"1B",
                        (unsigned)ginfo2.finalF,
                        (unsigned)ix,
                        (unsigned)b
                    );
                }
#endif
                ++i;
                return 0xFFFDu;
            }

            // ここからは cp != 0 が保証されている。
            // セットに応じて幅などを調整（AdditionalSymbols は SafeDecode1B の結果をそのまま）
            switch (set) {
            case CodeSetHint::Alnum:
                cp = DecodeAribAlnum(ix, /*widthMode=*/0);         // 全角
                break;
            case CodeSetHint::PropAlnum:
                cp = DecodeAribPropAlnum(ix, /*widthMode=*/0);     // 半角
                break;

            case CodeSetHint::Hiragana:
                cp = DecodeAribHiragana(ix, 0);
                break;
            case CodeSetHint::Katakana:
            case CodeSetHint::JISX0201_Katakana:
                cp = DecodeAribKatakana(ix, 0);
                break;
            case CodeSetHint::PropHiragana:
                cp = DecodeAribPropHiragana(ix, 0);
                break;
            case CodeSetHint::PropKatakana:
                cp = DecodeAribPropKatakana(ix, 0);
                break;

            case CodeSetHint::AdditionalSymbols:
                // ★ AdditionalSymbols は SafeDecode1B の結果をそのまま使う
                break;

            default:
                break;
            }

            ++i;

#if (ARIBDECODER_TRACE_GSETS >= 2) || ARIBDECODER_TRACE_CHARS
            if (m_traceG) {
                ARIB_LOCAL_CHAR_LOGF(
                    L"[ARIB-CHAR][1B] set=%ls GR=%d b=0x%02X g94=0x%02X ix=%u -> U+%04X('%lc')",
                    csName(set),
                    isGR ? 1 : 0,
                    (unsigned)b,
                    (unsigned)g94,
                    (unsigned)ix,
                    (unsigned)cp,
                    (wchar_t)((cp && cp <= 0xFFFF) ? cp : 0xFFFD)
                );
            }
#endif
            // ★ ここでは cp==0 にはならない設計にしたので、最後の if(!cp) は削除済み
            return cp;
        }

        // ===================== 2バイト集合 =====================
        // b1,b2 のどちらかが足りない（short）
        if (i + 1 >= len) {
            // ★ 種類別カウンタ（2B / short）
            g_replacement_char_count.fetch_add(1, std::memory_order_relaxed);
            g_rep_2b_short.fetch_add(1, std::memory_order_relaxed);

#if ARIBDECODER_TRACE_REPLACEMENTS
            if (m_traceG) {
                uint8_t raw = (i < len) ? data[i] : 0;
                ARIB_LOCAL_REP_LOGF(
                    L"[ARIB-REP] stage=Map2B-short pos=%zu raw=%02X (GL=G%u GR=G%u)",
                    (size_t)i,
                    (unsigned)raw,
                    (unsigned)m_state.GL,
                    (unsigned)m_state.GR
                );
            }
#endif
            // ここでは b1 分だけ消費（次の呼び出しで再評価する）
            ++i;
            return 0xFFFDu;
        }

        const uint8_t b1 = peek(0);
        const uint8_t b2 = peek(1);

        // ゾーン外（いずれかのバイトが InG94 でない）
        if (!InG94(b1, isGR) || !InG94(b2, isGR)) {
            // ★ 種類別カウンタ（2B / InG94）
            g_replacement_char_count.fetch_add(1, std::memory_order_relaxed);
            g_rep_2b_InG94.fetch_add(1, std::memory_order_relaxed);

#if ARIBDECODER_TRACE_REPLACEMENTS
            if (m_traceG) {
                const int gi2 = isGR ? (int)m_state.GR : (int)m_state.GL;
                const GSetInfo& ginfo2 = m_state.GInfo[gi2];
                ARIB_LOCAL_REP_LOGF(
                    L"[ARIB-REP] stage=Map2B-InG94 zone=%ls G%u=%ls(%ls,F=0x%02X) raw=%02X %02X",
                    isGR ? L"GR" : L"GL",
                    (unsigned)gi2,
                    csName(set),
                    ginfo2.isTwoBytes ? L"2B" : L"1B",
                    (unsigned)ginfo2.finalF,
                    (unsigned)b1,
                    (unsigned)b2
                );
            }
#endif
            i += 2;
            return 0xFFFDu;
        }

        // GL/GR の差を吸収して 0x21..0x7E に正規化
        const uint8_t ku = NormG94(b1, isGR);
        const uint8_t ten = NormG94(b2, isGR);

        // index（記号/DRCS 向け）
        const uint8_t ku_ix = Idx94(ku);   // 0..93
        const uint8_t ten_ix = Idx94(ten);  // 0..93

        uint32_t cp = 0;

        switch (set) {
        case CodeSetHint::JIS0208:
        case CodeSetHint::JIS_CompatKanji1:
        case CodeSetHint::JIS_CompatKanji2:
        {
            // まずは「追加記号テーブル」で解決を試みる
            char32_t sym = DecodeAribAdditionalSymbol(
                ku_ix, ten_ix,
                /*preferUnicode=*/(ARIBDECODER_PREFER_UNICODE_SYMBOLS != 0)
            );
            if (sym != U'\uFFFD') {
                cp = static_cast<uint32_t>(sym);
                break; // シンボルで解決できたので漢字テーブルには行かない
            }

            // ここまで来たら純粋な漢字として扱う
            const int gi2 = isGR ? static_cast<int>(m_state.GR)
                : static_cast<int>(m_state.GL);
            const int plane = DecideKanjiPlane(m_state.G[gi2], m_state.GInfo[gi2].finalF);

#if ARIBDECODER_ENABLE_KANJI_TABLE
#  if ARIBDECODER_KANJI_STAGE >= 1
            if (plane == 1) {
                const uint16_t code = static_cast<uint16_t>((ku << 8) | ten);
                cp = GetJISX0213Unicode(1, code);
            }
#  endif
#  if ARIBDECODER_KANJI_STAGE >= 2
            if (plane == 2) {
                const uint16_t code = static_cast<uint16_t>((ku << 8) | ten);
                cp = GetJISX0213Unicode(2, code);
            }
#  endif
#else
            cp = 0; // Kanji 経路は封印
#endif
            break;
        }

        case CodeSetHint::AdditionalSymbols:
        {
            cp = static_cast<uint32_t>(
                DecodeAribAdditionalSymbol(
                    ku_ix,
                    ten_ix,
                    /*preferUnicode=*/(ARIBDECODER_PREFER_UNICODE_SYMBOLS != 0)));

            if (!cp || cp == 0xFFFDu) {
                g_addsym_unknown_char_count.fetch_add(1, std::memory_order_relaxed);
#if ARIBDECODER_LOG_DETAILS
                DebugLogFormat(
                    L"[ARIB][SYM] unknown AdditionalSymbols ku=%u ten=%u (ix=%u,%u) bytes=(%02X,%02X)",
                    (unsigned)ku, (unsigned)ten,
                    (unsigned)ku_ix, (unsigned)ten_ix,
                    (unsigned)b1, (unsigned)b2
                );
#endif
                cp = 0; // 後段の decodeFail 処理に任せる
            }
            break;
        }

        case CodeSetHint::DRCS0_2B:
            // ★ DRCS 文字を読もうとした回数だけカウント
            g_drcs_char_count.fetch_add(1, std::memory_order_relaxed);
            cp = ResolveDRCSUnicode(0, ku_ix, ten_ix);
            break;

        default:
            break;
        }

        // cp==0 ⇒ 漢字テーブル/追加記号/DRCS いずれでも解決できなかった
        if (cp == 0) {
            g_replacement_char_count.fetch_add(1, std::memory_order_relaxed);
            g_rep_2b_decodeFail.fetch_add(1, std::memory_order_relaxed);

#if ARIBDECODER_TRACE_REPLACEMENTS
            if (m_traceG) {
                const int gi2 = isGR ? (int)m_state.GR : (int)m_state.GL;
                const GSetInfo& ginfo2 = m_state.GInfo[gi2];
                ARIB_LOCAL_REP_LOGF(
                    L"[ARIB-REP] stage=Map2B-cp0 zone=%ls G%u=%ls(%ls,F=0x%02X) ku=%u ten=%u raw=%02X %02X",
                    isGR ? L"GR" : L"GL",
                    (unsigned)gi2,
                    csName(set),
                    ginfo2.isTwoBytes ? L"2B" : L"1B",
                    (unsigned)ginfo2.finalF,
                    (unsigned)ku_ix,
                    (unsigned)ten_ix,
                    (unsigned)b1,
                    (unsigned)b2
                );
            }
#endif
            i += 2;
            return 0xFFFDu;
        }

        i += 2;

#if (ARIBDECODER_TRACE_GSETS >= 2) || ARIBDECODER_TRACE_CHARS
        if (m_traceG) {
            bool canShowChar = (cp != 0)
                && (cp <= 0xFFFF)
                && !(cp >= 0xD800 && cp <= 0xDFFF); // サロゲートは除外

            if (canShowChar) {
                ARIB_LOCAL_CHAR_LOGF(
                    L"[ARIB-CHAR][2B] set=%ls GR=%d b1=0x%02X b2=0x%02X ku=%u ten=%u (ix=%u,%u) -> U+%04X('%lc')",
                    csName(set),
                    isGR ? 1 : 0,
                    (unsigned)b1, (unsigned)b2,
                    (unsigned)ku, (unsigned)ten,
                    (unsigned)ku_ix, (unsigned)ten_ix,
                    (unsigned)cp,
                    (wchar_t)cp
                );
            }
            else {
                ARIB_LOCAL_CHAR_LOGF(
                    L"[ARIB-CHAR][2B] set=%ls GR=%d b1=0x%02X b2=0x%02X ku=%u ten=%u (ix=%u,%u) -> U+%06X",
                    csName(set),
                    isGR ? 1 : 0,
                    (unsigned)b1, (unsigned)b2,
                    (unsigned)ku, (unsigned)ten,
                    (unsigned)ku_ix, (unsigned)ten_ix,
                    (unsigned)cp
                );
            }
        }
#endif

        return cp;
    }



    // ============== Decode (UTF-16 / UTF-8) ==============
    std::u16string AribDecoder::DecodeToUTF16(const uint8_t* data, std::size_t length) noexcept
    {
        // コアで UTF-32 までデコード
        const std::u32string u32 = DecodeCoreToU32(data, length);
        // 既存ヘルパで UTF-16 に「下げる」
        return U32ToUTF16(u32);
    }


    
	// UTF-32 デコード版
    std::u32string AribDecoder::DecodeCoreToU32(const uint8_t* data, std::size_t length) noexcept {
        ARIB_LOCAL_START_LOG(L"[ARIB][START]DecodeCoreToU32の実行\n");
        g_dec_start.fetch_add(1, std::memory_order_relaxed);

        std::u32string out;
        out.reserve(length);
        if (!data || length == 0) { g_dec_err.fetch_add(1, std::memory_order_relaxed); return out; }

        // ★ 文字列開始ごとに SS をクリア（安全）
        Reset();               // G0..G3, GInfo, GL/GR まで全部初期状態へ
        ResetStateForString(); // SingleGL/SS2/SS3 用のクリア（Reset の中に含めてもOK）

        bool pickLogged = false;  // ★ この呼び出し中に「1回だけ」拾いログを出す
        bool loggedAddSym = false; // AdditionalSymbols 用ログを一度だけ出すフラグ

        std::size_t i = 0;
        std::size_t guard = 0; // ★ 無限ループ防止

        while (i < length && guard++ < length * 4 + 16) {
            const std::size_t before = i; // ★ 進捗監視

            // i が範囲内であることは while 条件で保証
            uint8_t b = data[i];

            try {
                if (isC0(b)) {
                    if (b == 0x0A) out.push_back(u'\n');
                    else if (b == 0x09) out.push_back(u'\t');
                    else if (b == 0x1B) { // ESC
                        std::size_t used = handleESC(&data[i + 1], length - (i + 1), 0, nullptr);
                        if (used > length - (i + 1)) used = length - (i + 1); // ★ クランプ
                        i += 1 + used;
                        continue;
                    }
                    else if (b == 0x0E) { m_state.GL = 1; }  // LS1
                    else if (b == 0x0F) { m_state.GL = 0; }  // LS0
                    else if (b == 0x19) { m_state.SS2 = true; } // SS2
                    else if (b == 0x1D) { m_state.SS3 = true; } // SS3
                    ++i;
                    continue;
                }

                // 既存:
                if (isC1(b)) {
                    // ★ まず総数をインクリメント
                    g_c1_total.fetch_add(1, std::memory_order_relaxed);

                    // ---- 0x80〜0x89 のうち、仕様上はある程度意味が決まっているもの ----
                    // ここでは「動作としては捨てるが、制御としては認識した」ものを
                    // g_c1_control_handled_count にまとめてカウントしておく
                    switch (b) {
                    case 0x80: // PAD
                    case 0x81: // HOP
                    case 0x82: // BPH
                    case 0x83: // NBH
                    case 0x84: // IND
                    case 0x86: // SSA
                    case 0x87: // ESA
                    case 0x88: // HTS
                    case 0x89: // HTJ
                        g_c1_control_handled_count.fetch_add(1, std::memory_order_relaxed);
                        // 今のところ挙動は「何もしない」で OK
                        ++i;
                        continue;

                    case 0x85: // NEL (改行)
                        g_c1_control_handled_count.fetch_add(1, std::memory_order_relaxed);
                        g_c1_output_char_count.fetch_add(1, std::memory_order_relaxed);
                        out.push_back(u'\n');
                        ++i;
                        continue;

                    default:
                        break; // 0x8A以降は既存ロジックへ
                    }
                    // --- サイズ系（NSZ等）は無視 ---
                    auto isSizeCtl = [](uint8_t x) noexcept {
                        // SSZ 0x8A, MSZ 0x8B, NSZ 0x8C, SZX 0x8D
                        return (x == 0x8A || x == 0x8B || x == 0x8C || x == 0x8D);
                        };
                    if (isSizeCtl(b)) {

                        // ここまでのどれにも該当しない C1 はログだけ取って無視
                        g_c1_sizeCtl.fetch_add(1, std::memory_order_relaxed);
                        g_c1_control_handled_count.fetch_add(1, std::memory_order_relaxed);
#if ARIBDECODER_TRACE_GSETS
                        if (m_traceG) DebugLogFormat(L"[ARIB][SIZE] C1=0x%02X detected (ignored)", (unsigned)b);
#endif
                        ++i;
                        continue;
                    }

					// --- マクロプリセット ---
                    if (b >= 0x90 && b <= 0x9F) {
                        g_c1_macro.fetch_add(1, std::memory_order_relaxed);
                        g_c1_control_handled_count.fetch_add(1, std::memory_order_relaxed);

                        const uint8_t macroIndex = static_cast<uint8_t>(b - 0x90);
                        ApplyMacroPreset(macroIndex);
                        ++i;
                        continue;
                    }

					// --- SS2 / SS3 ---
                    if (b == 0x8E) {
                        m_state.SS2 = true;
                        g_c1_ss2.fetch_add(1, std::memory_order_relaxed);
                        g_c1_control_handled_count.fetch_add(1, std::memory_order_relaxed);
                        ++i;
                        continue;
                    }
                    if (b == 0x8F) {
                        m_state.SS3 = true;
                        g_c1_ss3.fetch_add(1, std::memory_order_relaxed);
                        g_c1_control_handled_count.fetch_add(1, std::memory_order_relaxed);
                        ++i;
                        continue;
                    }
                    // ---- ここまでどれにも該当しなかった C1 ----
                    g_c1_unknown.fetch_add(1, std::memory_order_relaxed);
                    g_c1_unsupported_count.fetch_add(1, std::memory_order_relaxed);
#if ARIBDECODER_TRACE_GSETS
                    if (m_traceG) {
                        DebugLogFormat(L"[ARIB][C1] Unhandled C1=0x%02X (ignored)", (unsigned)b);
                    }
#endif
                    ++i;
                    continue;
                }

                // --- ★ SP (space) の特別扱い: 制御コード側で処理 ---
                if (b == 0x20) {
                    // GL の 0x20 は常にスペース（LibISDB と同じ発想）
                    ++i;
                    // 好みで：全角 or 半角。EPG 的には全角が自然。
                    out.push_back(u'\u3000');   // 全角空白 "　"
                    continue;
                }
                if (b == 0xA0) {
                    // GR 側 SP は半角スペース扱い（LibISDB と同じ）
                    ++i;
                    out.push_back(u' ');
                    continue;
                }

                bool restore = false;
                int savedGL = m_state.GL, savedGR = m_state.GR;
                if (m_state.SS2) { m_state.SS2 = false; m_state.GL = 2; restore = true; }
                if (m_state.SS3) { m_state.SS3 = false; m_state.GL = 3; restore = true; }

                // ★ 最初の可視文字に達した時点で「今どこを使うか」を1行ログ（1回だけ）
                if (!pickLogged && m_traceG) {
                    const bool isGRzone = (b >= 0xA1 && b <= 0xFE);
                    const int  gi = isGRzone ? (int)m_state.GR : (int)m_state.GL;
                    const auto set = m_state.G[gi];
                    const bool twoB = m_state.GInfo[gi].isTwoBytes;

                    // 既存の G セット状況ログ
                    DebugLogFormat(
                        L"[ARIB][Pick] zone=%ls G%u=%ls(%ls,F=0x%02X) first=0x%02X",
                        isGRzone ? L"GR" : L"GL",
                        (unsigned)gi,
                        csName(set),
                        twoB ? L"2B" : L"1B",
                        (unsigned)m_state.GInfo[gi].finalF,
                        (unsigned)b
                    );

                    // ★ 追加: AdditionalSymbols に入ったことを一度だけログ
                    if (set == CodeSetHint::AdditionalSymbols && !loggedAddSym) {
                        DebugLogFormat(
                            L"[ARIB-ADD-SYM-ENTRY] zone=%ls G%u=AdditionalSymbols(%ls,F=0x%02X)",
                            isGRzone ? L"GR" : L"GL",
                            (unsigned)gi,
                            twoB ? L"2B" : L"1B",
                            (unsigned)m_state.GInfo[gi].finalF
                        );
                        loggedAddSym = true;
                    }

                    pickLogged = true;
                }

                // ★ ここから 1文字分の「生バイト」と「使ったGセット」を記録しておく
                const std::size_t charPos = i;
                uint8_t raw1 = data[i];
                uint8_t raw2 = (i + 1 < length) ? data[i + 1] : 0;
                CodeSetHint usedSet = CodeSetHint::Alnum;
                bool        usedIsGR = false;
                int         usedBytes = 0;


                // ★ 文字マッピング実行
                //char16_t ch = 0;
                uint32_t cp = 0;
                if (b >= 0x21 && b <= 0x7E) {
                    usedSet = m_state.G[m_state.GL];
                    usedIsGR = false;
                    cp = MapCharFromGSet(usedSet, false, data, i, length);
                    usedBytes = static_cast<int>(i - charPos);   // 1 または 2
                }
                else if (b >= 0xA1 && b <= 0xFE) {
                    usedSet = m_state.G[m_state.GR];
                    usedIsGR = true;
                    cp = MapCharFromGSet(usedSet, true, data, i, length);
                    usedBytes = static_cast<int>(i - charPos);   // 1 or 2
                }
                else {
                    // GL/GR どちらでもない領域（0x7F 等）はそのまま置換扱い
                    ++i;
                    cp = 0xFFFDu;
                    usedSet = m_state.G[m_state.GL];
                    usedIsGR = false;
                    usedBytes = 1;

					// ★ 種類別カウンタ
                    g_replacement_char_count.fetch_add(1, std::memory_order_relaxed);
                    g_rep_char_nonZone.fetch_add(1, std::memory_order_relaxed);
                }

                if (restore) { m_state.GL = savedGL; m_state.GR = savedGR; }

                // ★ AdditionalSymbols のときは 1セル→複数コードポイント展開を優先
                if (DecodeAdditionalSymbolCellToUTF32(
                    usedSet,
                    usedIsGR,
                    usedBytes,
                    raw1,
                    raw2,
                    std::back_inserter(out))) {
                    // ここに来た時点で out には必要なコードポイントがすべて書き込まれている。
                    // cp は使わず、この文字の処理は完了として次へ進む。
                    continue;
                }

#if ARIBDECODER_TRACE_REPLACEMENTS
                // ★ 「�」(U+FFFD) が出たときだけ状況ログを吐く
                if (cp == 0xFFFDu) {
                    //g_replacement_char_count.fetch_add(1, std::memory_order_relaxed);
                    if (m_traceG) {
#if ARIBDECODER_TRACE_GSETS
                        const uint8_t g_index = usedIsGR ? m_state.GR : m_state.GL;
                        const GSetInfo& ginfo = m_state.GInfo[g_index];
                        ARIB_LOCAL_REP_LOGF(
                            L"[ARIB-REP] stage=MapChar pos=%zu zone=%ls G%u=%ls(%ls,F=0x%02X) usedBytes=%d raw=%02X %02X",
                            charPos,
                            usedIsGR ? L"GR" : L"GL",
                            (unsigned)g_index,
                            csName(usedSet),
                            ginfo.isTwoBytes ? L"2B" : L"1B",
                            (unsigned)ginfo.finalF,
                            usedBytes,
                            (unsigned)raw1,
                            (unsigned)raw2
                        );
#else
                        ARIB_LOCAL_REP_LOGF(
                            L"[ARIB-REP] stage=MapChar pos=%zu zone=%ls usedBytes=%d raw=%02X %02X",
                            charPos,
                            usedIsGR ? L"GR" : L"GL",
                            usedBytes,
                            (unsigned)raw1,
                            (unsigned)raw2
                        );
#endif
                    }
                }
#endif

				// ★ UTF-32 への書き込み
                out.push_back(static_cast<char32_t>(cp));
            }
            catch (...) {
				// ★ 例外保険：ここに来ることは想定していないが、一応置換文字を出して進める
                g_replacement_char_count.fetch_add(1, std::memory_order_relaxed);
                g_rep_exception.fetch_add(1, std::memory_order_relaxed);

#if ARIBDECODER_TRACE_REPLACEMENTS
                
                if (m_traceG) {
                    ARIB_LOCAL_REP_LOGF(
                        L"[ARIB-REP] stage=exception pos=%zu raw=%02X (GL=G%u GR=G%u)",
                        (size_t)i,
                        (unsigned)b,
                        (unsigned)m_state.GL,
                        (unsigned)m_state.GR
                    );
                }
#endif
                // ★ 例外保険：状態を立て直して前進させる
                ++i;
                out.push_back(u'\uFFFD');
                ResetStateForString();
            }

            // ★ 前進保証：i が進んでいなければ強制的に1バイト捨てて置換文字を出す
            if (i == before) {

				// ★ 無限ループ防止カウンタ
                g_replacement_char_count.fetch_add(1, std::memory_order_relaxed);
                g_rep_guard.fetch_add(1, std::memory_order_relaxed);

#if ARIBDECODER_TRACE_REPLACEMENTS
                
                if (m_traceG) {
                    uint8_t raw = (i < length) ? data[i] : 0;
                    ARIB_LOCAL_REP_LOGF(
                        L"[ARIB-REP] stage=guard pos=%zu raw=%02X (GL=G%u GR=G%u)",
                        (size_t)i,
                        (unsigned)raw,
                        (unsigned)m_state.GL,
                        (unsigned)m_state.GR
                    );
                }
#endif

                ++i;
                out.push_back(u'\uFFFD');
            }
        }

        g_dec_ok.fetch_add(1, std::memory_order_relaxed);
        return out;
    }

    std::string AribDecoder::DecodeToUTF8(const uint8_t* data, std::size_t length) noexcept
    {
        const std::u32string u32 = DecodeCoreToU32(data, length);
        return U32ToUTF8(u32);
    }


    std::u32string AribDecoder::DecodeToUTF32(const uint8_t* data, size_t length)
    {
        return DecodeCoreToU32(data, length);
    }




    std::wstring AribDecoder::Decode(const uint8_t* data, size_t length)
    {
        const std::u16string u16 = DecodeToUTF16(data, length);
        return std::wstring(u16.begin(), u16.end());
    }


   
    // ============== Lightweight probe (void版) ==============
    void AribDecoder::AnalyzeOnce(const uint8_t* data, std::size_t length,
        std::vector<ControlEvent>* outEvents) noexcept
    {
        ARIB_LOCAL_START_LOG(L"[ARIB][START]AnalyzeOnceの実行\n");
        if (!data || length == 0) return;

        std::size_t i = 0;
        while (i < length) {
            uint8_t b = data[i];
            if (isC0(b)) {
                if (b == 0x0E) { m_state.GL = 1; if (outEvents) outEvents->push_back({ ControlEvent::LSx, i, {b,0,0,0}, 1 }); }
                else if (b == 0x0F) { m_state.GL = 0; if (outEvents) outEvents->push_back({ ControlEvent::LSx, i, {b,0,0,0}, 1 }); }
                else if (b == 0x19) { m_state.SS2 = true; }
                else if (b == 0x1D) { m_state.SS3 = true; }
                else if (b == 0x1B) {
                    std::size_t used = handleESC(&data[i + 1], length - (i + 1), i, outEvents);
                    i += 1 + used; continue;
                }
                ++i; continue;
            }
            if (isC1(b)) {
                if (b == 0x8E) m_state.SS2 = true;
                else if (b == 0x8F) m_state.SS3 = true;
                ++i; continue;
            }
            // 可視は状態進行のみ（詳細は Decode で）
            ++i;
        }
    }

    
	// ============== Decode to wstring (UTF-16) ==============
    bool AribDecoder::DecodeTextUTF16(const uint8_t* bytes, std::size_t len, std::wstring& wout) noexcept
    {
        ARIB_LOCAL_START_LOG(L"[ARIB][START]DecodeTextUTF16の実行\n");
        // DecodeTextUTF16 の冒頭あたり
        if (!g_arib_trace_state_logged.exchange(true)) {
            DebugLogFormat(
                L"[ARIB][TRACE-STATE] m_traceG=%d (IsGTraceEnabled=%d)",
                m_traceG ? 1 : 0,
                IsGTraceEnabled() ? 1 : 0
            );
        }
        wout.clear();
        if (!bytes) return false;

        // 既存の安全化は DecodeToUTF16 内で ResetStateForString() 済み
        const std::u16string u16 = DecodeToUTF16(bytes, len);

        // 簡易判定（段階導入）：文字があり、U+FFFD の比率が高すぎない
        std::size_t rep = 0;
        for (char16_t ch : u16) if (ch == u'\uFFFD') ++rep;
        const bool ok = (!u16.empty()) && (rep * 2 < u16.size());

#if ARIBDECODER_ENABLE_UFFFD_DUMP
        // ★この文字列の中に U+FFFD が一つでも含まれていたら、
        //   元の ARIB バイト列を HEX + ASCII でダンプ
        if (rep > 0 && bytes && len > 0) {
            DumpAribBytesHex(bytes, len, L"DecodeTextUTF16");
        }
#endif

        wout.assign(u16.begin(), u16.end());
        return ok;
    }


    // ============== counters dump ==============
    void AribDecoder::DumpDecodeCounters() noexcept {
        ARIB_LOCAL_COUNTER_LOGF(L"[AribDecoder] Decode Start Count: %zu", g_dec_start.load(std::memory_order_relaxed));
        ARIB_LOCAL_COUNTER_LOGF(L"[AribDecoder] Decode Success Count: %zu", g_dec_ok.load(std::memory_order_relaxed));
        ARIB_LOCAL_COUNTER_LOGF(L"[AribDecoder] Decode Error Count: %zu", g_dec_err.load(std::memory_order_relaxed));

        // ★未実装系チェック用
        ARIB_LOCAL_COUNTER_LOGF(L"[AribDecoder] DRCS designate: %zu, DRCS chars: %zu",
            g_drcs_designate_count.load(std::memory_order_relaxed),
            g_drcs_char_count.load(std::memory_order_relaxed));

        ARIB_LOCAL_COUNTER_LOGF(L"[AribDecoder] AdditionalSymbols designate: 1B=%zu, 2B=%zu",
            g_addsym_1b_designate_count.load(std::memory_order_relaxed),
            g_addsym_2b_designate_count.load(std::memory_order_relaxed));

        ARIB_LOCAL_COUNTER_LOGF(L"[AribDecoder] Unknown ESC count: %zu",
            g_unknown_esc_count.load(std::memory_order_relaxed));

        ARIB_LOCAL_COUNTER_LOGF(L"[AribDecoder] AdditionalSymbols Unknown Char Count: %zu",
            g_addsym_unknown_char_count.load(std::memory_order_relaxed));

        //  置換文字の発生回数
        ARIB_LOCAL_COUNTER_LOGF(L"[AribDecoder] Replacement char (U+FFFD) Count: %zu",
            g_replacement_char_count.load(std::memory_order_relaxed));

		// LatinExt / LatinSpecial 未実装文字数
        ARIB_LOCAL_COUNTER_LOGF(L"[AribDecoder] LatinExt unknown: %zu, LatinSpecial unknown: %zu",
            g_latinext_unknown_count.load(std::memory_order_relaxed),
            g_latinspecial_unknown_count.load(std::memory_order_relaxed));

		//  置換文字の発生回数内訳
        ARIB_LOCAL_COUNTER_LOGF(L"[AribDecoder] Replacement char (U+FFFD) Count: %zu",
            g_replacement_char_count.load(std::memory_order_relaxed));

        
        ARIB_LOCAL_COUNTER_LOGF(L"[AribDecoder]  U+FFFD breakdown:");
		//  1B / 2B / その他
        DebugLogFormat(L"  1B: InG94=%zu, decodeFail=%zu",
            g_rep_1b_InG94.load(std::memory_order_relaxed),
            g_rep_1b_decodeFail.load(std::memory_order_relaxed));
        DebugLogFormat(L"  2B: short=%zu, InG94=%zu, decodeFail=%zu",
            g_rep_2b_short.load(std::memory_order_relaxed),
            g_rep_2b_InG94.load(std::memory_order_relaxed),
            g_rep_2b_decodeFail.load(std::memory_order_relaxed));
        DebugLogFormat(L"  misc: nonZone=%zu, exception=%zu, guard=%zu",
            g_rep_char_nonZone.load(std::memory_order_relaxed),
            g_rep_exception.load(std::memory_order_relaxed),
            g_rep_guard.load(std::memory_order_relaxed));

		// C1 基本統計
        DebugLogFormat(
            L"[AribDecoder] C1: total=%zu, sizeCtl=%zu, macro=%zu, SS2=%zu, SS3=%zu, unknown=%zu",
            g_c1_total.load(std::memory_order_relaxed),
            g_c1_sizeCtl.load(std::memory_order_relaxed),
            g_c1_macro.load(std::memory_order_relaxed),
            g_c1_ss2.load(std::memory_order_relaxed),
            g_c1_ss3.load(std::memory_order_relaxed),
            g_c1_unknown.load(std::memory_order_relaxed));

		// C1 詳細内訳
        DebugLogFormat(
            L"[AribDecoder] C1 detail: handled=%llu, output=%llu, unsupported=%llu, invalid=%llu",
            static_cast<unsigned long long>(g_c1_control_handled_count.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_c1_output_char_count.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_c1_unsupported_count.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_c1_invalid_count.load(std::memory_order_relaxed)));
    }

	// ============== Character Table Access ==============
	//文字テーブル呼び出し関数群

    // ---- G94/G96 判定 ----
    bool AribDecoder::InG94(uint8_t b, bool isGR) noexcept {
        return isGR ? (b >= 0xA1 && b <= 0xFE)
            : (b >= 0x21 && b <= 0x7E);
    }
    bool AribDecoder::InG96(uint8_t b, bool isGR) noexcept {
        return isGR ? (b >= 0xA0 && b <= 0xFF)
            : (b >= 0x20 && b <= 0x7F);
    }

    // ---- 正規化（GL/GR → 共通領域へ）----
    uint8_t AribDecoder::NormG94(uint8_t b, bool isGR) noexcept {
        // GR: 0xA1..0xFE → 0x21..0x7E
        return isGR ? static_cast<uint8_t>(b - 0x80) : b;
    }
    uint8_t AribDecoder::NormG96(uint8_t b, bool isGR) noexcept {
        // GR: 0xA0..0xFF → 0x20..0x7F
        return isGR ? static_cast<uint8_t>(b - 0x80) : b;
    }

    // ---- 0-based index 化 ----
    uint8_t AribDecoder::Idx94(uint8_t g94) noexcept {
        // 前提: g94 ∈ [0x21..0x7E]
        return static_cast<uint8_t>(g94 - 0x21); // 0..93
    }
    uint8_t AribDecoder::Idx96(uint8_t g96) noexcept {
        // 前提: g96 ∈ [0x20..0x7F]
        return static_cast<uint8_t>(g96 - 0x20); // 0..95
    }

    // ---- Kanji 面決定（まずはヒント優先。将来 finalF で厳密化）----
    int AribDecoder::DecideKanjiPlane(CodeSetHint set, uint8_t finalF) noexcept {
        ARIB_LOCAL_START_LOG(L"[ARIB][START]DecideKanjiPlaneの実行\n");
        // デフォルト：ヒント（互換漢字1→P1、互換漢字2→P2）
        int byHint = 0;
        switch (set) {
        case CodeSetHint::JIS0208:
        case CodeSetHint::JIS_CompatKanji1: byHint = 1; break; // Plane1
        case CodeSetHint::JIS_CompatKanji2: byHint = 2; break; // Plane2
        default:                            byHint = 0; break;
        }

        // Fで上書き（※ARIB慣用）
        // 'B','@','9' → Plane1 / 'D',':' → Plane2 / それ以外はヒント優先
        switch (finalF) {
        case 'B': case '@': case '9': return 1;
        case 'D': case ':':           return 2;
        default:                      return byHint;
        }
    }


    void AribDecoder::LogBeforeDecode(std::wstring_view label,
        const uint8_t* data, size_t length, size_t previewBytes) noexcept
    {
        ARIB_LOCAL_START_LOG(L"[ARIB][START]LogBeforeDecodeの実行\n");
        // 例: 先頭バイトをASCII化（非表示は '.' に）
        std::wstring ascii;
        ascii.reserve(previewBytes);
        for (size_t i = 0; i < std::min<size_t>(length, previewBytes); ++i) {
            wchar_t w = (data[i] >= 0x20 && data[i] <= 0x7E) ? (wchar_t)data[i] : L'.';
            ascii.push_back(w);
        }
        const std::wstring safeAscii = EscapePercents(ascii); // ←★ここ重要★

        // 例: 先頭のHEXも作る
        wchar_t hexbuf[512]{};
        size_t hn = 0;
        for (size_t i = 0; i < std::min<size_t>(length, previewBytes) && hn + 3 < std::size(hexbuf); ++i) {
            hn += swprintf(hexbuf + hn, std::size(hexbuf) - hn, L"%02X ", (unsigned)data[i]);
        }

        const std::wstring safeLabel = EscapePercents(std::wstring(label));
        // 必ず「固定フォーマット」+「引数」
        DebugLogFormat(
            L"[PRE-DECODE] %.*ls len=%zu head=%ls ascii=\"%ls\"",
            (int)label.size(), label.data(),
            length, hexbuf, safeAscii.c_str()
        );
    }


    std::wstring AribDecoder::MakeLogSafe_(const std::u16string& u16)
    {
        ARIB_LOCAL_START_LOG(L"[ARIB][START]MakeLogSafe_の実行\n");
        std::wstring out;
        out.reserve(u16.size() + 8);
        for (char16_t ch : u16) {
            wchar_t w = static_cast<wchar_t>(ch);
            if (w < 0x20 || w == 0x7F) w = L'.'; // 制御類は可視化
            // ★ ここで必ず '%' を "%%" に
            if (w == L'%') { out.push_back(L'%'); out.push_back(L'%'); }
            else { out.push_back(w); }
        }
        return out;
    }


    void AribDecoder::Summarize_(const std::u16string& u16, size_t& rep, bool& ok) noexcept
    {
        rep = 0;
        for (char16_t ch : u16) if (ch == u'\uFFFD') ++rep;
        ok = (!u16.empty()) && (rep * 2 < u16.size());
    }

    void AribDecoder::LogDecodedSummary(std::wstring_view what,
        const std::u16string& u16,
        const uint16_t* opt_service_id) noexcept
    {
        ARIB_LOCAL_START_LOG(L"[ARIB][START]LogDecodedSummaryの実行\n");
        size_t rep = 0; bool ok = false;
        Summarize_(u16, rep, ok);
        const std::wstring wsafe = MakeLogSafe_(u16);
        

        if (opt_service_id) {
            ARIB_LOCAL_DEC_LOGF(
                L"[ARIB][DEC] %.*ls: sid=0x%04X ok=%d len=%zu rep=%zu text=\"%ls\"",
                (int)what.size(), what.data(),
                *opt_service_id, ok ? 1 : 0,
                (size_t)u16.size(), rep, wsafe.c_str()
            );
        }
        else {
            ARIB_LOCAL_DEC_LOGF(
                L"[ARIB][DEC] %.*ls: ok=%d len=%zu rep=%zu text=\"%ls\"",
                (int)what.size(), what.data(),
                ok ? 1 : 0,
                (size_t)u16.size(), rep, wsafe.c_str()
            );
        }
    }

#if ARIBDECODER_LOG_DETAILS
    static std::wstring DumpCodepoints_(const std::u16string& u16, size_t maxDump = 96)
    {
        std::wostringstream oss; oss << L"[";
        size_t printed = 0;
        for (size_t i = 0; i < u16.size(); ++i) {
            if (printed >= maxDump) { oss << L"...(+" << (u16.size() - i) << L")"; break; }
            const uint16_t cp = static_cast<uint16_t>(u16[i]);
            if (printed) oss << L' ';
            oss << L"U+" << std::uppercase << std::hex
                << std::setw(4) << std::setfill(L'0') << static_cast<unsigned>(cp)
                << std::nouppercase << std::dec;
            ++printed;
        }
        oss << L"]";
        return oss.str();
    }

    struct U16ClassStats_ { size_t ascii = 0, hira = 0, kata = 0, kanji = 0, pua = 0, symbol = 0, replaced = 0, other = 0; };
    static U16ClassStats_ Classify_(const std::u16string& u16)
    {
        U16ClassStats_ s{};
        for (char16_t ch : u16) {
            uint16_t u = static_cast<uint16_t>(ch);
            if (u == 0xFFFD) { ++s.replaced; continue; }
            if (u >= 0x20 && u <= 0x7E) { ++s.ascii;   continue; }
            if (u >= 0x3040 && u <= 0x309F) { ++s.hira;    continue; }
            if ((u >= 0x30A0 && u <= 0x30FF) || u == 0x30FC) { ++s.kata; continue; }
            if (u >= 0x4E00 && u <= 0x9FFF) { ++s.kanji;   continue; }
            if (u >= 0xE000 && u <= 0xF8FF) { ++s.pua;     continue; }
            if ((u >= 0x2000 && u <= 0x206F) || (u >= 0x3000 && u <= 0x303F)) { ++s.symbol; continue; }
            ++s.other;
        }
        return s;
    }

    void AribDecoder::LogDecodedDetails(std::wstring_view what,
        const std::u16string& u16,
        const uint16_t* opt_service_id) noexcept
    {
        ARIB_LOCAL_START_LOG(L"[ARIB][START]LogDecodedDetailsの実行\n");
        const auto st = Classify_(u16);
        if (opt_service_id) {
            ARIB_LOCAL_DEC_DETAIL_LOGF(L"[ARIB][DEC-DETAIL] %ls sid=0x%04X | ascii=%zu hira=%zu kata=%zu kanji=%zu pua=%zu symbol=%zu other=%zu rep=%zu",
                what.data(), *opt_service_id,
                st.ascii, st.hira, st.kata, st.kanji, st.pua, st.symbol, st.other, st.replaced);
        }
        else {
            ARIB_LOCAL_DEC_DETAIL_LOGF(L"[ARIB][DEC-DETAIL] %ls | ascii=%zu hira=%zu kata=%zu kanji=%zu pua=%zu symbol=%zu other=%zu rep=%zu",
                what.data(),
                st.ascii, st.hira, st.kata, st.kanji, st.pua, st.symbol, st.other, st.replaced);
        }
        const auto dump = DumpCodepoints_(u16, 96);
        ARIB_LOCAL_DEC_CP_LOGF(L"[ARIB][DEC-CP] %ls %ls", what.data(), dump.c_str());
    }
#endif // ARIBDECODER_LOG_DETAILS


    static inline uint32_t ToFullwidthAscii(uint32_t cp) noexcept {
        if (cp >= 0x21 && cp <= 0x7E) {
#if ARIB_USE_YEN_FOR_BACKSLASH
            if (cp == 0x5C) return 0xFFE5; // FULLWIDTH YEN SIGN
#else
            if (cp == 0x5C) return 0xFF3C; // FULLWIDTH REVERSE SOLIDUS
#endif
            return cp + 0xFEE0;
        }
#if ARIB_USE_FULLWIDTH_SPACE
        if (cp == 0x20) return 0x3000;     // IDEOGRAPHIC SPACE（保険）
#endif
        return cp;
    }

    // ---- Kanji テーブル直参照ヘルパ（ku/ten は 0-based index を前提）----
    static inline uint32_t GetKanjiUnicodeFromTables_(int plane, uint8_t ku_ix, uint8_t ten_ix) noexcept
    {
        // 安全ガード
        if (ten_ix >= 94) return 0;

        if (plane == 1) {
            // Plane1: 94×94（ku_ix: 0..93）
            if (ku_ix < 94) {
                return KanjiPlane1Table[ku_ix][ten_ix];
            }
            return 0;
        }

        if (plane == 2) {
            // Plane2 は 94×94 を 2分割保持：
            //   前半 15行 → KanjiPlane2Table1[15][94]  … ku_ix = 0..14
            //   後半 17行 → KanjiPlane2Table2[17][94] … ku_ix = 15..31
            if (ku_ix < 15) {
                return KanjiPlane2Table1[ku_ix][ten_ix];
            }
            // 後半ブロック
            uint8_t ku2 = static_cast<uint8_t>(ku_ix - 15); // 0..16
            if (ku2 < 17) {
                return KanjiPlane2Table2[ku2][ten_ix];
            }
            return 0;
        }

        return 0;
    }

    // どこか共通ユーティリティに
    static inline std::wstring EscapePercents(const std::wstring& s) {
        std::wstring out;
        out.reserve(s.size());
        for (wchar_t c : s) {
            if (c == L'%') { out.push_back(L'%'); out.push_back(L'%'); }
            else { out.push_back(c); }
        }
        return out;
    }
    static inline std::wstring EscapePercents(std::wstring_view sv) {
        return EscapePercents(std::wstring(sv));
    }
    static inline std::string EscapePercentsA(const std::string& s) {
        std::string out; out.reserve(s.size());
        for (char c : s) out += (c == '%') ? "%%" : std::string(1, c);
        return out;
    }

    

    // ---- 安全な1B/2Bデコード用ラッパー（範囲外なら 0 を返す）----
    static inline uint32_t SafeDecode1B(decoder::AribDecoder::CodeSetHint set, uint8_t ix) noexcept
    {
        // G94 の index は 0..93 のみ有効
        if (ix >= 94)
            return 0;

        switch (set) {
            // 1バイト G94 集合（「デコードできるかどうか」の判定も兼ねてそのまま呼ぶ）
        case decoder::AribDecoder::CodeSetHint::Alnum:
            return DecodeAribAlnum(ix, /*widthMode=*/0);

        case decoder::AribDecoder::CodeSetHint::PropAlnum:
            return DecodeAribPropAlnum(ix, /*widthMode=*/0);

        case decoder::AribDecoder::CodeSetHint::Hiragana:
            return DecodeAribHiragana(ix, 0);

        case decoder::AribDecoder::CodeSetHint::Katakana:
        case decoder::AribDecoder::CodeSetHint::JISX0201_Katakana:
            return DecodeAribKatakana(ix, 0);

        case decoder::AribDecoder::CodeSetHint::PropHiragana:
            return DecodeAribPropHiragana(ix, 0);

        case decoder::AribDecoder::CodeSetHint::PropKatakana:
            return DecodeAribPropKatakana(ix, 0);

            // 1バイト AdditionalSymbols
        case decoder::AribDecoder::CodeSetHint::AdditionalSymbols:
        {
            // ix: 0..93 を「90区 1～94点」に対応させる
            const uint8_t ku_ix = static_cast<uint8_t>((90 - 1)); // 90区 → index 89
            const uint8_t ten_ix = ix;                            // 0..93

#if ARIBDECODER_PREFER_UNICODE_SYMBOLS
            uint32_t cp = static_cast<uint32_t>(
                DecodeAribAdditionalSymbol(ku_ix, ten_ix, /*preferUnicode=*/true));
#else
            uint32_t cp = static_cast<uint32_t>(
                DecodeAribAdditionalSymbol(ku_ix, ten_ix, /*preferUnicode=*/false));
#endif
            // 未定義 or 置換文字なら「失敗」とみなして 0 を返す
            if (!cp || cp == 0xFFFDu) {
                g_addsym_unknown_char_count.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            return cp;
        }

        // ラテン拡張テーブル（1バイト）
        case decoder::AribDecoder::CodeSetHint::LatinExtension:
        {
            const uint8_t code = static_cast<uint8_t>(0x21 + ix); // 0x21..0x7E
            uint32_t cp = DecodeLatinExtension(code);
            if (!cp) {
                g_latinext_unknown_count.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            return DecodeLatinExtension(code);
        }
        case decoder::AribDecoder::CodeSetHint::LatinSpecial:
        {
            const uint8_t code = static_cast<uint8_t>(0x21 + ix);
            uint32_t cp = DecodeLatinSpecial(code);
            if (!cp) {
                g_latinspecial_unknown_count.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            return DecodeLatinSpecial(code);
        }

        default:
            return 0;
        }
    }


    static inline uint32_t SafeDecode2B(decoder::AribDecoder::CodeSetHint set,
        uint8_t ku_ix, uint8_t ten_ix,
        int plane, uint8_t /*finalF*/) noexcept
    {
        // index は 0..93 × 0..93
        if (ku_ix >= 94 || ten_ix >= 94)
            return 0;

        // ---- 2B AdditionalSymbols セット ----
        if (set == decoder::AribDecoder::CodeSetHint::AdditionalSymbols) {
#if ARIBDECODER_PREFER_UNICODE_SYMBOLS
            uint32_t cp = static_cast<uint32_t>(
                DecodeAribAdditionalSymbol(ku_ix, ten_ix, /*preferUnicode=*/true));
#else
            uint32_t cp = static_cast<uint32_t>(
                DecodeAribAdditionalSymbol(ku_ix, ten_ix, /*preferUnicode=*/false));
#endif
            if (!cp || cp == 0xFFFDu) {
                g_addsym_unknown_char_count.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            return cp;
        }

        // ---- JIS 系 2B (漢字集合) ----
        if (set == decoder::AribDecoder::CodeSetHint::JIS0208 ||
            set == decoder::AribDecoder::CodeSetHint::JIS_CompatKanji1 ||
            set == decoder::AribDecoder::CodeSetHint::JIS_CompatKanji2)
        {
#if ARIBDECODER_ENABLE_KANJI_TABLE
            // まず「追加記号が紛れ込んでいないか」を追加シンボルテーブルで確認
            // （90～94区にある [生] や ★ などの対策。90～94区以外なら DecodeAribAdditionalSymbol 側で U+FFFD を返すだけ）
#if ARIBDECODER_PREFER_UNICODE_SYMBOLS
            char32_t sym = DecodeAribAdditionalSymbol(ku_ix, ten_ix, /*preferUnicode=*/true);
#else
            char32_t sym = DecodeAribAdditionalSymbol(ku_ix, ten_ix, /*preferUnicode=*/false);
#endif
            if (sym != U'\uFFFD' && sym != 0) {
                // 追加記号として定義されている → こちらを優先
                return static_cast<uint32_t>(sym);
            }

            // ここまで来たら「純粋な漢字」として JIS X 0213 テーブルを参照
            if (plane == 1 || plane == 2) {
                const uint16_t code =
                    static_cast<uint16_t>(((ku_ix + 0x21) << 8) | (ten_ix + 0x21));
                return GetJISX0213Unicode(plane, code);
            }
#endif // ARIBDECODER_ENABLE_KANJI_TABLE

            return 0;
        }

        // ---- DRCS ----
        if (set == decoder::AribDecoder::CodeSetHint::DRCS0_2B) {
            g_drcs_char_count.fetch_add(1, std::memory_order_relaxed);

            uint32_t cp = ResolveDRCSUnicode(0, ku_ix, ten_ix);

#if ARIBDECODER_TRACE_DRCS
            if (cp == 0) {
                // ここで 0 を返したものは、後段で U+FFFD に置き換えられる候補
                DebugLogFormat(
                    L"[ARIB-DRCS] ku=%d ten=%d -> unresolved (cp=0, will fallback to U+FFFD)",
                    ku_ix + 1,  // 区/点は 1-origin 表記の方が JIS テーブルと見比べやすい
                    ten_ix + 1
                );
            }
            else {
                DebugLogFormat(
                    L"[ARIB-DRCS] ku=%d ten=%d -> U+%04X",
                    ku_ix + 1,
                    ten_ix + 1,
                    cp
                );
            }
#endif

            return cp;  // 0 のときは後段で U+FFFD にフォールバック
        }

        // その他のセットはここでは扱わない
        return 0;
    }

    // ---- 追加記号 1セル→複数コードポイント展開用ヘルパ ----
    template <class OutputIt>
    static bool DecodeAdditionalSymbolCellToUTF32(
        AribDecoder::CodeSetHint usedSet,
        bool usedIsGR,
        int usedBytes,
        uint8_t raw1,
        uint8_t raw2,
        OutputIt out) noexcept
    {
        using decoder::AribDecoder;

        if (usedSet != AribDecoder::CodeSetHint::AdditionalSymbols)
            return false;

#if ARIBDECODER_PREFER_UNICODE_SYMBOLS
        constexpr bool preferUnicode = true;
#else
        constexpr bool preferUnicode = false;
#endif

        // --- 1バイト AdditionalSymbols (G94, 区は 90 固定) ---
        if (usedBytes == 1) {
            if (!InG94_local(raw1, usedIsGR))
                return false;

            const uint8_t g94 = NormG94_local(raw1, usedIsGR); // 0x21..0x7E
            const uint8_t tenIx = Idx94_local(g94);              // 0..93

            const uint8_t ku = 90;                                     // 区番号 90
            const uint8_t ten = static_cast<uint8_t>(tenIx + 1);        // 点番号 1..94

            // ku, ten は 1..94 で渡す
            return DecodeAdditionalSymbolTo(ku, ten, preferUnicode, out);
        }

        // --- 2バイト AdditionalSymbols (G94×G94) ---
        if (usedBytes == 2) {
            if (!InG94_local(raw1, usedIsGR) ||
                !InG94_local(raw2, usedIsGR))
                return false;

            const uint8_t ku_g94 = NormG94_local(raw1, usedIsGR); // 0x21..0x7E
            const uint8_t ten_g94 = NormG94_local(raw2, usedIsGR);

            const uint8_t kuIx = Idx94_local(ku_g94);   // 0..93
            const uint8_t tenIx = Idx94_local(ten_g94);  // 0..93

            const uint8_t ku = static_cast<uint8_t>(kuIx + 1);   // 1..94
            const uint8_t ten = static_cast<uint8_t>(tenIx + 1);

            return DecodeAdditionalSymbolTo(ku, ten, preferUnicode, out);
        }

        return false;
    }


    void AribDecoder::ApplyMacroPreset(std::uint8_t macro_index) noexcept
    {
        ARIB_LOCAL_START_LOG(L"[ARIB][START]ApplyMacroPresetの実行\n");
        if (macro_index >= 16) {
            return;
        }

        const uint8_t* seq = decoder::Macro[macro_index];
        constexpr std::size_t kMaxLen = 20;

        std::size_t pos = 0;
        while (pos < kMaxLen) {
            uint8_t b = seq[pos];
            if (b == 0x00) {
                // 0 終端扱い
                break;
            }

            if (b == 0x1B) {
                // ESC シーケンス → 既存の handleESC をそのまま利用
                ++pos;
                if (pos >= kMaxLen) break;

                const std::size_t used = handleESC(&seq[pos], kMaxLen - pos, 0, nullptr);
                // handleESC 内部で G セット・LS/LSR が更新される
                pos += used;
            }
            else if (b == 0x0E) {
                // LS1 (GL=G1)
                m_state.GL = 1;
                ++pos;
            }
            else if (b == 0x0F) {
                // LS0 (GL=G0)
                m_state.GL = 0;
                ++pos;
            }
            else {
                // 想定外のバイトはスキップ（安全側）
                ++pos;
            }
        }

#if ARIBDECODER_TRACE_GSETS
        if (m_traceG) {
            DebugLogFormat(L"[ARIB][MACRO] ApplyMacroPreset(%u): GL=G%u GR=G%u",
                (unsigned)macro_index,
                (unsigned)m_state.GL,
                (unsigned)m_state.GR);
        }
#endif
    }

	// ==== UTF-32 関連ユーティリティ ====
    std::u16string AribDecoder::U32ToUTF16(const std::u32string& src) noexcept
    {
        std::u16string dst;
        dst.reserve(src.size());

        for (char32_t cp32 : src) {
            uint32_t cp = static_cast<uint32_t>(cp32);

            if (cp <= 0xFFFFu) {
                // サロゲート領域を特別扱いしたければここでチェック
                dst.push_back(static_cast<char16_t>(cp));
            }
            else if (cp <= 0x10FFFFu) {
                cp -= 0x10000u;
                char16_t high = static_cast<char16_t>(0xD800u + (cp >> 10));
                char16_t low = static_cast<char16_t>(0xDC00u + (cp & 0x3FF));
                dst.push_back(high);
                dst.push_back(low);
            }
            else {
                // 不正値 → U+FFFD
                dst.push_back(static_cast<char16_t>(0xFFFDu));
            }
        }
        return dst;
    }

    std::string AribDecoder::U32ToUTF8(const std::u32string& src) noexcept
    {
        std::string dst;
        dst.reserve(src.size() * 4);

        for (char32_t cp32 : src) {
            uint32_t cp = static_cast<uint32_t>(cp32);

            if (cp <= 0x7Fu) {
                dst.push_back(static_cast<char>(cp));
            }
            else if (cp <= 0x7FFu) {
                dst.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else if (cp <= 0xFFFFu) {
                dst.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else if (cp <= 0x10FFFFu) {
                dst.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                dst.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else {
                // 不正値 → U+FFFD
                dst.append("\xEF\xBF\xBD", 3); // U+FFFD
            }
        }

        return dst;
    }

    std::u32string AribDecoder::DecodeToU32(const uint8_t* data, std::size_t length) noexcept
    {
        return DecodeCoreToU32(data, length);
    }

    void AribDecoder::ResetDecodeCounters() noexcept
    {
        ARIB_LOCAL_START_LOG(L"[ARIB][START]ResetDecodeCountersの実行\n");

        g_dec_start.store(0, std::memory_order_relaxed);
        g_dec_ok.store(0, std::memory_order_relaxed);
        g_dec_err.store(0, std::memory_order_relaxed);

        g_drcs_designate_count.store(0, std::memory_order_relaxed);
        g_drcs_char_count.store(0, std::memory_order_relaxed);

        g_addsym_1b_designate_count.store(0, std::memory_order_relaxed);
        g_addsym_2b_designate_count.store(0, std::memory_order_relaxed);
        g_addsym_unknown_char_count.store(0, std::memory_order_relaxed);

        g_unknown_esc_count.store(0, std::memory_order_relaxed);

        g_replacement_char_count.store(0, std::memory_order_relaxed);

        g_rep_1b_InG94.store(0, std::memory_order_relaxed);
        g_rep_1b_decodeFail.store(0, std::memory_order_relaxed);

        g_rep_2b_short.store(0, std::memory_order_relaxed);
        g_rep_2b_InG94.store(0, std::memory_order_relaxed);
        g_rep_2b_decodeFail.store(0, std::memory_order_relaxed);

        g_rep_char_nonZone.store(0, std::memory_order_relaxed);
        g_rep_exception.store(0, std::memory_order_relaxed);
        g_rep_guard.store(0, std::memory_order_relaxed);

        g_latinext_unknown_count.store(0, std::memory_order_relaxed);
        g_latinspecial_unknown_count.store(0, std::memory_order_relaxed);
    }


} // namespace decoder
