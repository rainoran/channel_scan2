#pragma once
#ifndef ARIB_DECODER_PROBE_HPP
#define ARIB_DECODER_PROBE_HPP

// ▼ 追加: 軽量化スイッチと分岐ヒント
#if !defined(ARIB_PROBE_LOG)
#  ifndef NDEBUG
#    define ARIB_PROBE_LOG 1
#  else
#    define ARIB_PROBE_LOG 0
#  endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define ARIB_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define ARIB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define ARIB_LIKELY(x)   (x)
#  define ARIB_UNLIKELY(x) (x)
#endif

#include <cstdint>
#include <cstddef>
#include <vector>
#include <functional>
#include <string>
#include <string_view>

namespace arib {

    // 検出イベントの種別
    enum class ControlEventType {
        C0,             // 0x00-0x1F
        C1,             // 0x80-0x9F
        ESC_Unknown,    // 不明なESCシーケンス
        ESC_Designate,  // 例: ESC $ B, ESC ( J, ESC ) I, ESC * B, ESC + ...
        ESC_LockShift,  // 例: ESC n, ESC o, ESC ~, ESC }, ESC |
        SS2,            // 0x8E
        SS3,            // 0x8F
        Visible,        // ★ 可視文字をどの集合で何バイト消費したか
    };

    // 検出イベント
    struct ControlEvent {
        size_t pos = 0;            // 入力ストリーム上の位置（Feed開始からのオフセット）
        ControlEventType type{};
        uint8_t b1 = 0;            // 先頭制御バイト（例: 0x1B, 0x8E, 0x8F, etc.）
        uint8_t b2 = 0;            // 2バイト目（存在すれば）
        uint8_t b3 = 0;            // 3バイト目（存在すれば）
        std::wstring desc;         // 人間可読の簡易説明
    };

	// 符号化方式の推定結果（候補）
    enum class TextEncodingGuess { Unknown, ARIB, UTF8, ASCII, Binary };

	// 符号化方式の推定結果
    struct EncodingScore {
        double arib = 0.0;
        double utf8 = 0.0;
        double ascii = 0.0;
        double bin = 0.0;
        TextEncodingGuess winner = TextEncodingGuess::Unknown;
        double confidence = 0.0;
    };

    // 解析の集計結果（必要最低限）
    struct AnalyzeSummary {
        size_t totalBytes = 0;
        size_t countC0 = 0;
        size_t countC1 = 0;
        size_t countESC = 0;
        size_t countSS2 = 0;
        size_t countSS3 = 0;
        size_t count1B = 0;
        size_t count2B = 0;

        size_t used1B[4] = { 0,0,0,0 };
        size_t used2B[4] = { 0,0,0,0 };
        size_t designated[4] = { 0,0,0,0 };

        EncodingScore enc;
    };

    // 追加: 符号集合ヒント
    enum class CodeSetHint {
        Unknown,
        JIS0208,             // 0x42
        JIS_CompatKanji1,    // 0x39
        JIS_CompatKanji2,    // 0x3A
        Alnum,               // 0x4A（英数/Roman）
        Hiragana,            // 0x30
        Katakana,            // 0x31
        PropAlnum,           // 0x36
        PropHiragana,        // 0x37
        PropKatakana,        // 0x38
        JISX0201_Katakana,   // 0x49
        AdditionalSymbols,   // 0x3B
        DRCS0_2B,            // 2バイトDRCS
        DRCS1_1B, DRCS2_1B, DRCS3_1B, DRCS4_1B, DRCS5_1B,
        DRCS6_1B, DRCS7_1B, DRCS8_1B, DRCS9_1B, DRCS10_1B,
        DRCS11_1B, DRCS12_1B, DRCS13_1B, DRCS14_1B, DRCS15_1B,
    };

    struct GSetInfo {
        CodeSetHint hint = CodeSetHint::Unknown; // 論理名（推論）
        bool twoBytes = false;                   // 指示で2バイト集合なら true
        bool drcs = false;                       // DRCS 指定か
        uint8_t final = 0;                       // ESC 終端（F）値（0x30..0x7E）
    };


    // 追加: プローブ状態
    struct ProbeState {
        CodeSetHint G[4] = { CodeSetHint::Unknown, CodeSetHint::Unknown,
                            CodeSetHint::Unknown, CodeSetHint::Unknown }; // G0..G3
        GSetInfo    GInfo[4];
        int  GL = 0;   // 0..3 = G0..G3
        int  GR = 2;   // 初期はLS2R(G2)
        bool SS2 = false; // 単発指定フラグ
        bool SS3 = false;
    };



    // ▼ 追加: ログマクロ（Releaseで完全に消える）
#if ARIB_PROBE_LOG
#  define PROBE_LOG(self, WSTR_OR_VIEW) (self)->EmitLog((WSTR_OR_VIEW))
#else
#  define PROBE_LOG(self, WSTR_OR_VIEW) ((void)0)
#endif

    // 判定専用デコーダ（ステートマシン）
    // - 文字デコードは行わない
    // - ESCシーケンス/制御コードの検出・ログ出力のみ行う
    class AribDecoderProbe {
    public:
        using Logger = std::function<void(std::wstring_view)>;

        AribDecoderProbe() = default;

        // ロガーを差し替え（既定は無出力）
        void SetLogger(Logger logger) { m_logger = std::move(logger); }

        // 内部状態を初期化（GL/GRやG0～G3の概念は保持だけ・利用はしない）
        void Reset() noexcept;

        // ストリームを増分投入して判定（イベントは追記、サマリを更新）
        void Feed(const uint8_t* data, size_t length, std::vector<ControlEvent>* outEvents = nullptr) noexcept;


        // 1バッファだけを解析するユーティリティ（内部状態は維持）
        AnalyzeSummary AnalyzeOnce(const uint8_t* data, size_t length, std::vector<ControlEvent>* outEvents = nullptr) noexcept;

        // 現在までの集計値を取得
        const AnalyzeSummary& GetSummary() const noexcept { return m_summary; }

        // 追加: 状態取得と文字列境界用の単発解除
        const ProbeState& GetState() const noexcept { return m_state; }
        const GSetInfo& GetGInfo(int g) const noexcept { return m_state.GInfo[g]; }
        void ResetStateForString() noexcept { m_state.SS2 = m_state.SS3 = false; }
		
        // 追加: ログ出力ヘルパ
        void EmitLog(const std::wstring& s) noexcept;

        // Feed などで使う補助（宣言を .hpp に）
        static const wchar_t* csName(CodeSetHint h) noexcept;
        static size_t bytesPerChar(CodeSetHint h) noexcept;
        static size_t pickLenForInvoke(int gIndex, const CodeSetHint G[4]) noexcept;

        


    private:
        enum class EscState {
            None,
            AfterESC,      // 0x1B 読了直後
            ESC_DOLLAR,    // ESC 0x24 ($) 読了
            ESC_LPAREN,    // ESC 0x28 (  読了
            ESC_RPAREN,    // ESC 0x29 )  読了
            ESC_ASTER,     // ESC 0x2A *  読了
            ESC_PLUS,      // ESC 0x2B +  読了
        };

        // ヘルパ
        void log(const std::wstring& s) noexcept;
        static bool isC0(uint8_t b) { return b <= 0x1F; }
        static bool isC1(uint8_t b) { return (b >= 0x80 && b <= 0x9F); }
        // 追加（軽量な範囲判定・全て inline）
        static inline bool inGL_Vis(uint8_t b) noexcept { return (uint8_t)(b - 0x21) < 0x5E; } // 0x21..0x7E
        static inline bool inGL_Vis1(uint8_t b) noexcept { return (uint8_t)(b - 0x20) < 0x5F; } // 0x20..0x7E (space含む)
        static inline bool inGR_Vis(uint8_t b) noexcept { return (uint8_t)(b - 0xA1) < 0x5E; } // 0xA1..0xFE

        // 追加: 指示/呼び出しの適用ヘルパ
        void applyLockShift(uint8_t code) noexcept;
        void applyDesignateSequence(uint8_t first, const uint8_t* seq, size_t n) noexcept;

        // ESCシーケンス判定
        // 戻り: 消費した追加バイト数（先頭の0x1Bは呼び出し側で消費済みとする）
        size_t handleESC(const uint8_t* data, size_t length, size_t globalPos, std::vector<ControlEvent>* out) noexcept;

        // Locking Shift / Designate の説明文字列を簡易生成
        static std::wstring makeEscDesc_LockShift(uint8_t code);
        static std::wstring makeEscDesc_Designate(uint8_t introducer, uint8_t final);
        // 追加: ヒント生成
        static CodeSetHint hintFromFinal(uint8_t fin, bool twoBytes, bool isDRCS);

        // ▼ 追加: 実使用時の確定長（2B/1B）をGInfo優先で決める
        static inline size_t pickLenForInvoke(const GSetInfo& gi, bool isGR,
            const uint8_t* data, size_t i, size_t len) noexcept {
            if (ARIB_UNLIKELY(i >= len)) return 0;
            if (gi.twoBytes) {
                if (ARIB_UNLIKELY(i + 1 >= len)) return 1; // 末尾保護
                if (!isGR) {
                    if (inGL_Vis(data[i]) && inGL_Vis(data[i + 1])) return 2;
                }
                else {
                    if (inGR_Vis(data[i]) && inGR_Vis(data[i + 1])) return 2;
                }
                return 1; // 範囲不正は安全側で1B
            }
            // 1B集合
            return (!isGR ? (inGL_Vis1(data[i]) ? 1 : 1) : (inGR_Vis(data[i]) ? 1 : 1));
        }

    private:
        Logger m_logger = [](std::wstring_view) {}; // 既定は無出力
        AnalyzeSummary m_summary{};
        size_t m_streamPos = 0; // Feedの累積オフセット（イベント位置表示用）
        // 追加: プローブ状態
        ProbeState m_state{};

    };

} // namespace arib

#endif // ARIB_DECODER_PROBE_HPP
