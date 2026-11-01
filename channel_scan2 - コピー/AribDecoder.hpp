#pragma once
#ifndef DECODER_ARIB_DECODER_HPP
#define DECODER_ARIB_DECODER_HPP

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <span>     // C++20+
#include <algorithm>
#include <atomic>

// --- Gセットトレース出力レベル ---
// 0: ログ無効 / 1: INIT・DESIGNATE・LS/LSR を出す / 2: + READ(毎文字)も出す
#ifndef ARIBDECODER_TRACE_GSETS
#define ARIBDECODER_TRACE_GSETS 1
#endif

// --- 汎用ログAPI（どのテーブル/フィルタでも使用可） ---
#ifndef ARIBDECODER_LOG_DETAILS
#define ARIBDECODER_LOG_DETAILS 1  // 1にすると詳細ログも出力
#endif

// --- 1文字ごとのデコードトレース ---
// 0: 無効
// 1: 有効（DecodeToUTF16 / MapCharFromGSet 内で毎文字ログ）
#ifndef ARIBDECODER_TRACE_CHARS
#define ARIBDECODER_TRACE_CHARS 0
#endif

// --- �(U+FFFD) が出たときの詳細ログ ---
// 0: 無効 / 1: 有効（1文字ごとに状況を1行出力）
#ifndef ARIBDECODER_TRACE_REPLACEMENTS
#define ARIBDECODER_TRACE_REPLACEMENTS 1
#endif

// 0x20 を GL 側で「強制スペース」として扱う簡易パッチ
// 1: 有効（現状の挙動） / 0: 無効（従来通り、Gセットに従って処理）
#ifndef ARIB_TREAT_0x20_AS_SPACE
#define ARIB_TREAT_0x20_AS_SPACE 1
#endif
// 追加記号の Unicode 変換で、可能な限り Unicode のシンボルを優先するかどうか
#ifndef ARIBDECODER_PREFER_UNICODE_SYMBOLS
#define ARIBDECODER_PREFER_UNICODE_SYMBOLS 0
#endif

// DRCS 関連のログ出力
#ifndef ARIBDECODER_TRACE_DRCS
#define ARIBDECODER_TRACE_DRCS 1   // 必要に応じて 0 にすれば DRCS ログを止められる
#endif



// === ラテン拡張セット Designate 用（ABNT NBR 15606-1 などを想定） ===
//
// ※デフォルトは無効（0）としておく。
//   実際に使うときは ABNT NBR 15606-1 の Table 8 を確認して
//   正しい Final Byte (F) を設定してください。
#ifndef ARIBDECODER_ENABLE_LATIN_SETS
#define ARIBDECODER_ENABLE_LATIN_SETS 0
#endif

#ifndef ARIBDECODER_LATINEXT_FINAL
#define ARIBDECODER_LATINEXT_FINAL 0x00    // 0x00: 無効
#endif

#ifndef ARIBDECODER_LATINSPECIAL_FINAL
#define ARIBDECODER_LATINSPECIAL_FINAL 0x00 // 0x00: 無効
#endif

namespace decoder {

    
    struct DecodeContextInfo {
        std::wstring label;       // "EIT-DEC[4D-name]" みたいなやつ
        uint16_t service_id = 0;
        uint16_t event_id = 0;
    };

    // ------------------------------------------------------------
    // AribDecoder: 単独ユニット（Probe機能を内包）
    //   - SDT/NIT等のARIB文字列をデコード
    //   - ESC系列(Designate/LS/LSR/SS)の選別ロジックを内包
    //   - DebugManager準拠の軽量ログをオプション出力
    // ------------------------------------------------------------
    class AribDecoder {
    public:
        // 制御イベント（必要最低限）
        struct ControlEvent {
            enum Kind : uint8_t { ESC = 0, Designate, LSx } kind{};
            std::size_t posGlobal{};   // 入力全体での位置（任意）
            uint8_t     bytes[4]{};    // 抜粋
            uint8_t     size{};        // bytesに格納したサイズ
        };

        // 文字集合ヒント（最小集合。詳細はcppに委譲）
        enum class CodeSetHint : uint8_t {
            Alnum, Hiragana, Katakana, JISX0201_Katakana,
            PropAlnum, PropHiragana, PropKatakana,
            AdditionalSymbols,
            LatinExtension,        // ラテン拡張 (1B)
            LatinSpecial,          // ラテン特殊 (1B)
            JIS0208,               // generic 2B
            JIS_CompatKanji1,      // Kanji plane1
            JIS_CompatKanji2,      // Kanji plane2
            DRCS0_2B,              // DRCS(2B)
        };

        // Gスロットに割当てられた集合のメタ
        struct GSetInfo {
            CodeSetHint hint{ CodeSetHint::JIS_CompatKanji1 };
            bool        isTwoBytes{ true };
            bool        isDRCS{ false };
            uint8_t     finalF{ 'B' }; // designateのF
        };

        // デコーダ内部状態（ProbeState）
        struct ProbeState {
            // 現在のG割当
            CodeSetHint G[4]{
                CodeSetHint::JIS_CompatKanji1, // G0: Kanji plane1
                CodeSetHint::Alnum,            // G1: Alnum(1B)
                CodeSetHint::Hiragana,         // G2: Hiragana(1B)
                CodeSetHint::Katakana          // G3: Katakana(1B)
            };
            GSetInfo    GInfo[4]{
                {CodeSetHint::JIS_CompatKanji1,true,false,'B'},
                {CodeSetHint::Alnum,false,false,'J'},
                {CodeSetHint::Hiragana,false,false,'0'},
                {CodeSetHint::Katakana,false,false,'1'}
            };

            // GL/GR選択（0..3）
            uint8_t GL{ 0 }; // 既定: GL=G0
            uint8_t GR{ 2 }; // 既定: GR=G2

            // 一時シフト（次の1文字だけ G2/G3 をGL扱い）
            bool SS2{ false };
            bool SS3{ false };

            int  SingleGL{ -1 };
        };

        // UTF-32 出力（内部の本体デコーダ）
        std::u32string DecodeToUTF32(const uint8_t* data, size_t length);

        // UTF-16 出力（公開API）
        std::wstring Decode(const uint8_t* data, size_t length);

        // デコード前プレビュー（SDT以外でも使える汎用）
        void LogBeforeDecode(std::wstring_view label,
            const uint8_t* data,
            size_t length,
            size_t previewBytes = 16) noexcept;

        // 復号結果の要約ログ（sidは任意）
        void LogDecodedSummary(std::wstring_view what,
            const std::u16string& u16,
            const uint16_t* opt_service_id = nullptr) noexcept;

#if ARIBDECODER_LOG_DETAILS
        // 詳細ログ（コードポイントや構成比）
        void LogDecodedDetails(std::wstring_view what,
            const std::u16string& u16,
            const uint16_t* opt_service_id = nullptr) noexcept;
#endif

		// Gセット状態ログ
        DecodeContextInfo m_ctx;

    public:

		// --- デコードコンテキスト情報の設定 ---
        void SetDecodeContext(std::wstring label, uint16_t sid = 0, uint16_t eid = 0) noexcept {
            m_ctx.label = std::move(label);
            m_ctx.service_id = sid;
            m_ctx.event_id = eid;
        }

        // --- ライフサイクル ---
        AribDecoder() = default;                 // ※デストラクタは規約により未宣言
        AribDecoder(const AribDecoder&) = delete;
        AribDecoder& operator=(const AribDecoder&) = delete;

        // --- 操作API ---
        void Reset() noexcept;
        // probe風ワンショット解析（必要ならイベント収集）
        void AnalyzeOnce(const uint8_t* data, std::size_t length,
            std::vector<ControlEvent>* outEvents = nullptr) noexcept;

        // 文字列デコード
        std::u16string DecodeToUTF16(const uint8_t* data, std::size_t length) noexcept;
        std::string    DecodeToUTF8(const uint8_t* data, std::size_t length) noexcept;

        // デコード統計（ログ出力）
        static void DumpDecodeCounters() noexcept;
        

        // （必要に応じて）現在の状態を取得
        const ProbeState& GetState() const noexcept { return m_state; }

        // 文字列デコード開始時に一時シフト(SS2/SS3)だけをクリアする
        inline void BeginString() noexcept { ResetStateForString(); }

        // Gセットの詳細ログを有効/無効化
        void SetGTrace(bool enable) noexcept { m_traceG = enable; }

        static void ForceGTrace(bool enable) noexcept { s_forceTraceG.store(enable, std::memory_order_relaxed); }
        static bool IsForceGTrace() noexcept { return s_forceTraceG.load(std::memory_order_relaxed); }
        //bool IsGTraceEnabled() const noexcept { return m_traceG; }
        bool IsGTraceEnabled() const noexcept { return m_traceG || IsForceGTrace(); }

        // フィルター向け：ARIBテキスト→UTF-16（失敗時は false）
        bool DecodeTextUTF16(const uint8_t* data, size_t len, std::wstring& out)  noexcept;

        // --- ARIB プリセットマクロの適用 ---
    // Macro[0..15] に定義済みの ESC シーケンスを、現在のデコーダ状態に適用する。
    // 出力文字列は生成せず、G セット割り当てや LS/LSR などの状態だけを更新する。
        void ApplyMacroPreset(std::uint8_t macro_index) noexcept;

        // --- UTF-16 ヘルパ（コードポイント ⇔ UTF-16） ---
       // cp: U+0000〜U+10FFFF を想定（それ以外は U+FFFD にフォールバック）
        static inline void AppendUtf16(std::u16string& out, uint32_t cp) noexcept
        {
            if (cp <= 0xFFFFu) {
                out.push_back(static_cast<char16_t>(cp));
            }
            else if (cp <= 0x10FFFFu) {
                cp -= 0x10000u;
                const char16_t high = static_cast<char16_t>(0xD800u + (cp >> 10));
                const char16_t low = static_cast<char16_t>(0xDC00u + (cp & 0x3FFu));
                out.push_back(high);
                out.push_back(low);
            }
            else {
                out.push_back(u'\uFFFD');
            }
        }

        // UTF-16 文字列から 1 コードポイントを取り出すヘルパ
        // ・i は参照で渡し、消費した UTF-16 コード単位ぶんだけ進む
        // ・不正サロゲート列などは U+FFFD を返す
        static inline uint32_t NextCodePointFromUtf16(const std::u16string& u16, std::size_t& i) noexcept
        {
            if (i >= u16.size()) {
                return 0xFFFDu;
            }

            const char16_t u = u16[i++];

            // 上位サロゲート?
            if (u >= 0xD800 && u <= 0xDBFF) {
                if (i < u16.size()) {
                    const char16_t low = u16[i];
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        ++i;
                        uint32_t cp = ((static_cast<uint32_t>(u) - 0xD800u) << 10)
                            | (static_cast<uint32_t>(low) - 0xDC00u);
                        return cp + 0x10000u;
                    }
                }
                // 不正な上位サロゲート（後続が無い／下位サロゲートでない）
                return 0xFFFDu;
            }

            // 単独の下位サロゲートも不正
            if (u >= 0xDC00 && u <= 0xDFFF) {
                return 0xFFFDu;
            }

            // 通常の BMP
            return static_cast<uint32_t>(u);
        }

		// UTF-32 出力（公開API）
        std::u32string DecodeToU32(const uint8_t* data, std::size_t length) noexcept;

		// デコード統計のリセット
        static void ResetDecodeCounters() noexcept;
    private:

        // コア: ARIB → UTF-32
        std::u32string DecodeCoreToU32(const uint8_t* data, std::size_t length) noexcept;
        // --- ESC/Designate・LS/SS処理 ---
        // first: '$','(',' )','*','+',' ' のいずれか
        // seq:   1段導入→{F} / 2段導入→{intro,F}
        void applyDesignateSequence(uint8_t first, const uint8_t* seq, std::size_t n) noexcept;

        // ESC処理：戻り値は消費バイト数
        std::size_t handleESC(const uint8_t* data, std::size_t length,
            std::size_t globalPos,
            std::vector<ControlEvent>* out) noexcept;

        // ★ 追加：文字列処理ごとの一時シフト状態をクリア
        void ResetStateForString() noexcept;

        // ★ 追加：LS/SS を1バイトコードから反映（0x0E/0x0F/0x8E/0x8F）
        void applyLockShift(uint8_t code) noexcept;

        // --- 実デコード補助 ---
        // 指定Gセットから1文字を取り出し、UTF-16の1文字（または置換）へ
        uint32_t MapCharFromGSet(CodeSetHint set, bool isGR,
            const uint8_t* data, std::size_t& i, std::size_t length) noexcept;

        static int bytesPerChar(CodeSetHint set) noexcept;

        // '('=G0, ')'=G1, '*'=G2, '+'=G3 を返す（該当なしは -1）
        static inline int gIndexFromIntroducer(uint8_t intro) noexcept {
            switch (intro) {
            case 0x28: return 0; // '('
            case 0x29: return 1; // ')'
            case 0x2A: return 2; // '*'
            case 0x2B: return 3; // '+'
            default:   return -1;
            }
        }

        bool m_traceG = false; // 既定はOFF

        // ログ用ユーティリティ
        static std::wstring MakeLogSafe_(const std::u16string& u16);
        static void Summarize_(const std::u16string& u16, size_t& rep, bool& ok) noexcept;

#if ARIBDECODER_TRACE_GSETS
        // DebugManager準拠ログ（軽量）
        void dumpGInit() noexcept;
        void dumpGAssign(const wchar_t* tag, int gi) noexcept;
        void dumpGLS(const wchar_t* tag) noexcept;
        // 互換：既存呼び出しが残っている場合に備えて
        void dumpGState(const wchar_t* reason) noexcept { dumpGLS(reason); }
#endif

        // ---- G94/G96 判定・正規化・インデックス化 ----
        static inline bool InG94(uint8_t b, bool isGR) noexcept;
        static inline bool InG96(uint8_t b, bool isGR) noexcept;

        static inline uint8_t NormG94(uint8_t b, bool isGR) noexcept; // → 0x21..0x7E
        static inline uint8_t NormG96(uint8_t b, bool isGR) noexcept; // → 0x20..0x7F

        static inline uint8_t Idx94(uint8_t g94 /*0x21..0x7E*/) noexcept; // 0..93
        static inline uint8_t Idx96(uint8_t g96 /*0x20..0x7F*/) noexcept; // 0..95

        // ---- Kanji 面の決定（ヒント優先、必要なら finalF で厳密化）----
        static inline int DecideKanjiPlane(CodeSetHint set, uint8_t finalF) noexcept;

		// --- 文字列変換ヘルパ ---
        static std::u16string U32ToUTF16(const std::u32string& src) noexcept;
        static std::string    U32ToUTF8(const std::u32string& src) noexcept;

        static inline std::atomic_bool s_forceTraceG{ false };
    private:
        ProbeState m_state{};
    };

} // namespace decoder

#endif // DECODER_ARIB_DECODER_HPP
