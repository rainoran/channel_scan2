#include "AribDecoderProbe.hpp"
#include "DebugManager.h"
#include <cstdio>
#include <cwchar>
#include <vector>
#include <string>
#include <string_view>

using std::wstring;


namespace arib {



	// ========== 追加: ログ出力ヘルパ ==========
    void AribDecoderProbe::EmitLog(const wstring& s) noexcept {
        log(s);
    }

    // ========== 軽量テキスト判定ヘルパ（UTF-8/ASCII） ==========
    static inline size_t utf8CharLen(uint8_t b) {
        if (b < 0x80) return 1;
        if ((b & 0xE0) == 0xC0) return 2;
        if ((b & 0xF0) == 0xE0) return 3;
        if ((b & 0xF8) == 0xF0) return 4;
        return 0;
    }
    static inline bool isValidUTF8Seq(const uint8_t* p, size_t n) {
        size_t len = utf8CharLen(p[0]);
        if (len == 0 || len > n) return false;
        for (size_t i = 1; i < len; ++i) if ((p[i] & 0xC0) != 0x80) return false;
        // 代表的な過長形式（簡易）
        if (len == 2 && (p[0] & 0x1E) == 0) return false;           // 2B overlong
        if (len == 3 && p[0] == 0xE0 && p[1] < 0xA0) return false;  // 3B overlong
        if (len == 4 && p[0] == 0xF0 && p[1] < 0x90) return false;  // 4B overlong
        return true;
    }
    static inline bool isAsciiByte(uint8_t b) {
        return (b >= 0x20 && b <= 0x7E) || b == 0x09 || b == 0x0A || b == 0x0D;
    }

    static inline bool isC0(uint8_t b) noexcept { return b <= 0x1F; }
    static inline bool isC1(uint8_t b) noexcept { return b >= 0x80 && b <= 0x9F; }

    // ========== ユーティリティ ==========
    void AribDecoderProbe::Reset() noexcept {
        m_summary = {};
        m_streamPos = 0;
        // ★ ARIB既定の初期値
        m_state.G[0] = CodeSetHint::JIS_CompatKanji1; // G0: 互換漢字1面(0x39)
        m_state.G[1] = CodeSetHint::Alnum;            // G1: 英数(0x4A)
        m_state.G[2] = CodeSetHint::Hiragana;         // G2: 平仮名(0x30)
        m_state.G[3] = CodeSetHint::Katakana;         // G3: 片仮名(0x31)
        m_state.GL = 0;                                // LS0(G0)
        m_state.GR = 2;                                // LS2R(G2)
        m_state.SS2 = m_state.SS3 = false;

        m_state.GInfo[0] = { CodeSetHint::JIS_CompatKanji1, true,  false, 0x39 };
        m_state.GInfo[1] = { CodeSetHint::Alnum,            false, false, 0x4A };
        m_state.GInfo[2] = { CodeSetHint::Hiragana,         false, false, 0x30 };
        m_state.GInfo[3] = { CodeSetHint::Katakana,         false, false, 0x31 };
    }

    void AribDecoderProbe::log(const wstring& s) noexcept {
        DebugLog(L"%ls", s.c_str());
    }


    // ========== 表示用ヘルパ ==========
    const wchar_t* AribDecoderProbe::csName(CodeSetHint h) noexcept {
        switch (h) {
        case CodeSetHint::JIS0208:             return L"JIS0208";
        case CodeSetHint::JIS_CompatKanji1:     return L"JIS-Compat1";
        case CodeSetHint::JIS_CompatKanji2:     return L"JIS-Compat2";
        case CodeSetHint::Alnum:                return L"Alnum";
        case CodeSetHint::Hiragana:             return L"Hira";
        case CodeSetHint::Katakana:             return L"Kana";
        case CodeSetHint::PropAlnum:            return L"P-Alnum";
        case CodeSetHint::PropHiragana:         return L"P-Hira";
        case CodeSetHint::PropKatakana:         return L"P-Kana";
        case CodeSetHint::JISX0201_Katakana:    return L"JISX0201";
        case CodeSetHint::AdditionalSymbols:    return L"AddSym";
        case CodeSetHint::DRCS0_2B:             return L"DRCS0(2B)";
        case CodeSetHint::DRCS1_1B:             return L"DRCS1";
        case CodeSetHint::DRCS2_1B:             return L"DRCS2";
        case CodeSetHint::DRCS3_1B:             return L"DRCS3";
        case CodeSetHint::DRCS4_1B:             return L"DRCS4";
        case CodeSetHint::DRCS5_1B:             return L"DRCS5";
        case CodeSetHint::DRCS6_1B:             return L"DRCS6";
        case CodeSetHint::DRCS7_1B:             return L"DRCS7";
        case CodeSetHint::DRCS8_1B:             return L"DRCS8";
        case CodeSetHint::DRCS9_1B:             return L"DRCS9";
        case CodeSetHint::DRCS10_1B:            return L"DRCS10";
        case CodeSetHint::DRCS11_1B:            return L"DRCS11";
        case CodeSetHint::DRCS12_1B:            return L"DRCS12";
        case CodeSetHint::DRCS13_1B:            return L"DRCS13";
        case CodeSetHint::DRCS14_1B:            return L"DRCS14";
        case CodeSetHint::DRCS15_1B:            return L"DRCS15";
        case CodeSetHint::Unknown:              return L"Unknown";
        default:                                return L"?";
        }
    }


    // 2バイト/1バイトの近似判定（選別用途）
    size_t AribDecoderProbe::bytesPerChar(CodeSetHint h) noexcept {
        switch (h) {
        case CodeSetHint::JIS0208:
        case CodeSetHint::JIS_CompatKanji1:
        case CodeSetHint::JIS_CompatKanji2:
        case CodeSetHint::AdditionalSymbols:
        case CodeSetHint::DRCS0_2B:
            return 2;
        default:
            return 1;
        }
    }

    // 可視文字を消費し、Visibleイベントと1B/2Bカウンタを積む
    inline void consumeVisibleBytes(std::vector<ControlEvent>* outEvents,
        AnalyzeSummary& sum,
        size_t globalBase,
        const uint8_t* data,
        size_t& i,
        size_t length,
        int gIndex,
        CodeSetHint set,
        const wchar_t* howLabel)
    {
        size_t remain = length - i;
        size_t n = AribDecoderProbe::bytesPerChar(set);         // 1 or 2
        if (n == 2 && remain < 2) n = 1;      // 安全側（末尾保護）

        // イベント出力
        if (outEvents) {
            ControlEvent ev;
            ev.pos = globalBase + i;
            ev.type = ControlEventType::Visible;
            ev.b1 = data[i];
            ev.desc = std::wstring(L"VIS(") + howLabel + L"): " + AribDecoderProbe::csName(set) + (n == 2 ? L" 2B" : L" 1B");
            outEvents->push_back(ev);
        }

        // カウンタ
        if (n == 2) sum.count2B++; else sum.count1B++;
        sum.enc.arib += 0.5; // 軽くARIBに加点

        // 前進
        i += n;
        sum.totalBytes += n;
    }


    // ========== メイン処理 ==========
    void AribDecoderProbe::Feed(const uint8_t* data, size_t length, std::vector<ControlEvent>* outEvents) noexcept {
        if (!data || length == 0) return;

        size_t i = 0;
        while (i < length) {
            const uint8_t b = data[i];

            // ---- C0 ----
            if (isC0(b)) {
                ControlEvent ev;
                ev.pos = m_streamPos + i;
                ev.type = ControlEventType::C0;
                ev.b1 = b;

                switch (b) {
                case 0x07: ev.desc = L"C0:BEL"; break;
                case 0x08: ev.desc = L"C0:BS";  break;
                case 0x09: ev.desc = L"C0:HT";  break;
                case 0x0A: ev.desc = L"C0:LF";  break;
                case 0x0D: ev.desc = L"C0:CR";  break;
                case 0x0E: ev.desc = L"C0:SO (LS1)"; applyLockShift(0x0E); break; // Locking Shift 1
                case 0x0F: ev.desc = L"C0:SI (LS0)"; applyLockShift(0x0F); break; // Locking Shift 0

                case 0x19: // SS2
                    ev.type = ControlEventType::SS2;
                    ev.desc = L"C0:SS2 (next char uses G2)";
                    m_state.SS2 = true;
                    if (outEvents) outEvents->push_back(ev);
                    m_summary.countC0++;
                    m_summary.countSS2++;
                    m_summary.enc.arib += 1.5; // ★ 加点
                    ++i; ++m_summary.totalBytes;
                    continue;

                case 0x1B: { // ESC
                    m_summary.countESC++;
                    m_summary.enc.arib += 3.0; // ★ 強い証拠
                    size_t consumed = handleESC(&data[i + 1], length - (i + 1), m_streamPos + i, outEvents);
                    i += (1 + consumed);
                    m_summary.totalBytes += (1 + consumed);
                    continue;
                }

                case 0x1D: // SS3
                    ev.type = ControlEventType::SS3;
                    ev.desc = L"C0:SS3 (next char uses G3)";
                    m_state.SS3 = true;
                    if (outEvents) outEvents->push_back(ev);
                    m_summary.countC0++;
                    m_summary.countSS3++;
                    m_summary.enc.arib += 1.5; // ★ 加点
                    ++i; ++m_summary.totalBytes;
                    continue;

                default: {
                    wchar_t buf[32];
                    std::swprintf(buf, 32, L"C0:0x%02X", b);
                    ev.desc = buf;
                } break;
                }

                if (outEvents) outEvents->push_back(ev);
                m_summary.countC0++;
                if (b == 0x0E || b == 0x0F) m_summary.enc.arib += 1.5; // SO/SI
                i++;
                m_summary.totalBytes++;
                continue;
            }

            // ---- C1 ----
            if (isC1(b)) {
                ControlEvent ev;
                ev.pos = m_streamPos + i;
                ev.type = ControlEventType::C1;
                ev.b1 = b;

                if (b == 0x8E) { // SS2
                    ev.type = ControlEventType::SS2;
                    ev.desc = L"C1:SS2 (next char uses G2)";
                    m_state.SS2 = true;
                    m_summary.countSS2++;
                }
                else if (b == 0x8F) { // SS3
                    ev.type = ControlEventType::SS3;
                    ev.desc = L"C1:SS3 (next char uses G3)";
                    m_state.SS3 = true;
                    m_summary.countSS3++;
                }
                else if (b == 0x9B) {
                    ev.desc = L"C1:CSI";
                }
                else {
                    wchar_t buf[32];
                    std::swprintf(buf, 32, L"C1:0x%02X", b);
                    ev.desc = buf;
                }

                if (outEvents) outEvents->push_back(ev);
                m_summary.countC1++;
                if (b == 0x8E || b == 0x8F || b == 0x9B) m_summary.enc.arib += 1.0; // ★ 加点
                i++;
                m_summary.totalBytes++;
                continue;
            }

            
            // ---- 単発 SS2/SS3 の適用（次の1文字だけ G2/G3）----
            if (m_state.SS2 || m_state.SS3) {
                if (!isC0(b) && !isC1(b) && b != 0x1B) {
                    const int gx = m_state.SS2 ? 2 : 3; // SS2→G2, SS3→G3
                    consumeVisibleBytes(outEvents, m_summary, m_streamPos,
                        data, i, length, gx, m_state.G[gx],
                        m_state.SS2 ? L"SS2" : L"SS3");
                    m_state.SS2 = m_state.SS3 = false;
                    continue; // ← 重要：同じ文字を再処理しない
                }
                // 可視でなければ単発解除だけ
                m_state.SS2 = m_state.SS3 = false;
            }

            // ---- GL（0x20–0x7E を可視扱い：SP含む）----
            if (b >= 0x20 && b <= 0x7E) {
                const int gx = m_state.GL;
                consumeVisibleBytes(outEvents, m_summary, m_streamPos,
                    data, i, length, gx, m_state.G[gx], L"GL");
                continue;
            }

            // ---- GR（0xA1–0xFE）----
            if (b >= 0xA1 && b <= 0xFE) {
                const int gx = m_state.GR;
                consumeVisibleBytes(outEvents, m_summary, m_streamPos,
                    data, i, length, gx, m_state.G[gx], L"GR");
                continue;
            }



            // ---- UTF-8 / ASCII / Binary の軽量評価（可視バイトで）----
            if (i + 3 <= length && data[i] == 0xEF && data[i + 1] == 0xBB && data[i + 2] == 0xBF) {
                m_summary.enc.utf8 += 5.0; // BOMは強い証拠
            }
            size_t remain = length - i;
            if ((data[i] & 0x80) == 0) {
                if (isAsciiByte(data[i])) m_summary.enc.ascii += 0.5;
                else m_summary.enc.bin += 0.5; // 制御ノイズ寄り
            }
            else {
                if (isValidUTF8Seq(&data[i], remain)) m_summary.enc.utf8 += 1.0;
                else m_summary.enc.bin += 2.0;
            }

            // ---- その他（可視文字など）はスキップ（判別専用）----
            i++;
            m_summary.totalBytes++;
        }

        m_streamPos += length;
    }

    // ========== ESC/LS/Designate ==========
    size_t AribDecoderProbe::handleESC(const uint8_t* data, size_t length, size_t globalPos, std::vector<ControlEvent>* out) noexcept {
        // 戻り値は「0x1Bの次に消費したバイト数」
        if (length == 0) {
            ControlEvent ev;
            ev.pos = globalPos;
            ev.type = ControlEventType::ESC_Unknown;
            ev.b1 = 0x1B;
            ev.desc = L"ESC (trailing, no following byte)";
            if (out) out->push_back(ev);
            return 0;
        }

        const uint8_t a = data[0];

        // Locking Shift（ISO-2022系）
        if (a == 0x6E || a == 0x6F || a == 0x7E || a == 0x7D || a == 0x7C) {
            applyLockShift(a);
            ControlEvent ev;
            ev.pos = globalPos;
            ev.type = ControlEventType::ESC_LockShift;
            ev.b1 = 0x1B; ev.b2 = a;
            ev.desc = makeEscDesc_LockShift(a);
            if (out) out->push_back(ev);
            return 1;
        }

        // 1B指定: ESC ( / ) / * / +
        if (a == 0x28 || a == 0x29 || a == 0x2A || a == 0x2B) {
            size_t used = 0;
            if (length >= 2) {
                used = (data[1] == 0x20) ? ((length >= 3) ? 3 : 0) : 2; // DRCS: 0x20 F
            }
            if (used) applyDesignateSequence(a, data, used - 1); // 状態反映
            if (used >= 2) {
                const uint8_t final = (data[1] == 0x20 && used >= 3) ? data[2] : data[1];
                ControlEvent ev;
                ev.pos = globalPos; ev.type = ControlEventType::ESC_Designate;
                ev.b1 = 0x1B; ev.b2 = a; ev.b3 = final;
                ev.desc = makeEscDesc_Designate(a, final);
                if (out) out->push_back(ev);
            }
            return used ? used : 1;
        }

        if (a == 0x24) {
            // 形式B: ESC $ <intro> [0x20] F
            if (length >= 2 && (data[1] == 0x28 || data[1] == 0x29 || data[1] == 0x2A || data[1] == 0x2B)) {
                size_t used = 0;
                if (length >= 3) used = (data[2] == 0x20) ? ((length >= 4) ? 4 : 0) : 3;
                if (used) {
                    applyDesignateSequence(0x24, data + 1, used - 1); // 状態反映
                    uint8_t final = (used == 4) ? data[3] : data[2];
                    ControlEvent ev;
                    ev.pos = globalPos; ev.type = ControlEventType::ESC_Designate;
                    ev.b1 = 0x1B; ev.b2 = 0x24; ev.b3 = final;
                    ev.desc = std::wstring(L"ESC $ (x) designate: ") + makeEscDesc_Designate(data[1], final);
                    if (out) out->push_back(ev);
                    return used;
                }
                ControlEvent ev;
                ev.pos = globalPos; ev.type = ControlEventType::ESC_Unknown;
                ev.b1 = 0x1B; ev.b2 = 0x24;
                ev.desc = L"ESC $ (dangling 3B designate)";
                if (out) out->push_back(ev);
                return 1;
            }

            // 形式A: ESC $ [0x20] F → G0
            if (length >= 2) {
                size_t used = (data[1] == 0x20) ? ((length >= 3) ? 3 : 0) : 2;
                if (used) {
                    applyDesignateSequence(0x24, data + 1, used - 1);
                    uint8_t final = (used == 3) ? data[2] : data[1];
                    ControlEvent ev;
                    ev.pos = globalPos; ev.type = ControlEventType::ESC_Designate;
                    ev.b1 = 0x1B; ev.b2 = 0x24; ev.b3 = final;
                    wchar_t buf[64];
                    std::swprintf(buf, 64, L"ESC $ designate (2B): final=0x%02X", final);
                    ev.desc = buf;
                    if (out) out->push_back(ev);
                    return used;
                }
            }
            ControlEvent ev;
            ev.pos = globalPos; ev.type = ControlEventType::ESC_Unknown;
            ev.b1 = 0x1B; ev.b2 = 0x24;
            ev.desc = L"ESC $ (dangling)";
            if (out) out->push_back(ev);
            return 1;
        }

        // 未知ESC
        {
            ControlEvent ev;
            ev.pos = globalPos;
            ev.type = ControlEventType::ESC_Unknown;
            ev.b1 = 0x1B; ev.b2 = a;
            wchar_t buf[64];
            std::swprintf(buf, 64, L"ESC unknown: 0x%02X", a);
            ev.desc = buf;
            if (out) out->push_back(ev);
        }
        return 1;
    }

    std::wstring AribDecoderProbe::makeEscDesc_LockShift(uint8_t code) {
        switch (code) {
        case 0x6E: return L"ESC n (LS2: GL←G2)";
        case 0x6F: return L"ESC o (LS3: GL←G3)";
        case 0x7E: return L"ESC ~ (LS1R: GR←G1)";
        case 0x7D: return L"ESC } (LS2R: GR←G2)";
        case 0x7C: return L"ESC | (LS3R: GR←G3)";
        default:   return L"ESC LockingShift (?)";
        }
    }

    std::wstring AribDecoderProbe::makeEscDesc_Designate(uint8_t introducer, uint8_t final) {
        const wchar_t intro = static_cast<wchar_t>(introducer);
        const wchar_t finCh = (final >= 0x20 && final <= 0x7E) ? static_cast<wchar_t>(final) : L'?';

        const wchar_t* name = L"(unknown set)";
        switch (final) {
        case 0x42: name = L"JIS X 0208 (Kanji)"; break;
        case 0x4A: name = L"JIS-Roman (Alnum)";  break;
        case 0x49: name = L"JIS X0201 Katakana"; break;
        case 0x30: name = L"Hiragana";           break;
        case 0x31: name = L"Katakana";           break;
        case 0x36: name = L"Prop Alnum";         break;
        case 0x37: name = L"Prop Hiragana";      break;
        case 0x38: name = L"Prop Katakana";      break;
        case 0x39: name = L"Compat Kanji-1";     break;
        case 0x3A: name = L"Compat Kanji-2";     break;
        case 0x3B: name = L"Additional Symbols"; break;
        }

        wchar_t buf[128];
        std::swprintf(buf, 128, L"ESC %lc %lc designate -> %ls (fin=0x%02X)", intro, finCh, name, final);
        return std::wstring(buf);
    }

    void AribDecoderProbe::applyLockShift(uint8_t code) noexcept {
        switch (code) {
        case 0x0E: m_state.GL = 1; break; // SO → GL←G1
        case 0x0F: m_state.GL = 0; break; // SI → GL←G0
        case 0x6E: m_state.GL = 2; break; // ESC n → GL←G2
        case 0x6F: m_state.GL = 3; break; // ESC o → GL←G3
        case 0x7E: m_state.GR = 1; break; // ESC ~ → GR←G1
        case 0x7D: m_state.GR = 2; break; // ESC } → GR←G2
        case 0x7C: m_state.GR = 3; break; // ESC | → GR←G3
        }
    }

    static CodeSetHint mapFinal(uint8_t fin, bool twoB, bool drcs) {
        if (drcs) {
            if (twoB) return CodeSetHint::DRCS0_2B; // 代表扱い
            switch (fin) {
            case 0x41: return CodeSetHint::DRCS1_1B;
            case 0x42: return CodeSetHint::DRCS2_1B;
            case 0x43: return CodeSetHint::DRCS3_1B;
            case 0x44: return CodeSetHint::DRCS4_1B;
            case 0x45: return CodeSetHint::DRCS5_1B;
            case 0x46: return CodeSetHint::DRCS6_1B;
            case 0x47: return CodeSetHint::DRCS7_1B;
            case 0x48: return CodeSetHint::DRCS8_1B;
            case 0x49: return CodeSetHint::DRCS9_1B;
            case 0x4A: return CodeSetHint::DRCS10_1B;
            case 0x4B: return CodeSetHint::DRCS11_1B;
            case 0x4C: return CodeSetHint::DRCS12_1B;
            case 0x4D: return CodeSetHint::DRCS13_1B;
            case 0x4E: return CodeSetHint::DRCS14_1B;
            case 0x4F: return CodeSetHint::DRCS15_1B;
            default:   return CodeSetHint::DRCS0_2B;
            }
        }
        switch (fin) {
        case 0x42: return CodeSetHint::JIS0208;            // 漢字
        case 0x4A: return CodeSetHint::Alnum;              // 英数(Roman)
        case 0x30: return CodeSetHint::Hiragana;
        case 0x31: return CodeSetHint::Katakana;
        case 0x36: return CodeSetHint::PropAlnum;
        case 0x37: return CodeSetHint::PropHiragana;
        case 0x38: return CodeSetHint::PropKatakana;
        case 0x49: return CodeSetHint::JISX0201_Katakana;
        case 0x39: return CodeSetHint::JIS_CompatKanji1;   // 互換漢字1面
        case 0x3A: return CodeSetHint::JIS_CompatKanji2;   // 互換漢字2面
        case 0x3B: return CodeSetHint::AdditionalSymbols;  // 追加記号
        default:   return CodeSetHint::Unknown;
        }
    }
    CodeSetHint AribDecoderProbe::hintFromFinal(uint8_t fin, bool twoB, bool drcs) {
        return mapFinal(fin, twoB, drcs);
    }

    size_t AribDecoderProbe::pickLenForInvoke(int gIndex, const CodeSetHint G[4]) noexcept {
        if (gIndex < 0 || gIndex > 3) return 1;
        return bytesPerChar(G[gIndex]);
    }


    void AribDecoderProbe::applyDesignateSequence(uint8_t first, const uint8_t* seq, size_t n) noexcept
    {
        if (!seq || n == 0) return;

        auto setG = [&](int gx, CodeSetHint h, bool twoB, bool isDRCS, uint8_t fin) {
            if (gx < 0 || gx > 3) return;

            if (h != CodeSetHint::Unknown) {
                m_state.G[gx] = h;
                m_state.GInfo[gx].hint = h;
            }
            else {
                m_state.GInfo[gx].hint = CodeSetHint::Unknown;
            }
            m_state.GInfo[gx].twoBytes = twoB;
            m_state.GInfo[gx].drcs = isDRCS;
            m_state.GInfo[gx].final = fin;
            m_summary.designated[gx]++;
            };

        // ---- 1バイトGセット: ESC '(' / ')' / '*' / '+' ----
        if (first == 0x28 || first == 0x29 || first == 0x2A || first == 0x2B) {
            const bool twoB = false;  // ★ 1バイト指定
            bool isDRCS = false;
            uint8_t fin = 0;

            if (seq[0] == 0x20) { // DRCS(1B): 0x20 F
                if (n < 2) return;
                isDRCS = true;
                fin = seq[1];
            }
            else {
                fin = seq[0];
            }

            const int gx =
                (first == 0x28) ? 0 :
                (first == 0x29) ? 1 :
                (first == 0x2A) ? 2 : 3;

            setG(gx, hintFromFinal(fin, twoB, isDRCS), twoB, isDRCS, fin);
            return;
        }

        // ---- 2バイトGセット: ESC '$' ... ----
        if (first == 0x24) {
            if (n < 1) return;

            // 形式B: ESC $ '('|')'|'*'|'+' [0x20] F
            if (seq[0] == 0x28 || seq[0] == 0x29 || seq[0] == 0x2A || seq[0] == 0x2B) {
                const bool twoB = true;  // ★ 2バイト指定（形式B）
                bool isDRCS = false;
                uint8_t fin = 0;

                if (n >= 3 && seq[1] == 0x20) { // DRCS(2B)
                    fin = seq[2];
                    isDRCS = true;
                }
                else {
                    if (n < 2) return;
                    fin = seq[1];
                }

                const int gx =
                    (seq[0] == 0x28) ? 0 :
                    (seq[0] == 0x29) ? 1 :
                    (seq[0] == 0x2A) ? 2 : 3;

                setG(gx, hintFromFinal(fin, twoB, isDRCS), twoB, isDRCS, fin);
                return;
            }

            // 形式A: ESC $ [0x20] F → G0
            {
                const bool twoB = true;  // ★ 2バイト指定（形式A）
                bool isDRCS = false;
                uint8_t fin = 0;

                if (seq[0] == 0x20) {
                    if (n < 2) return;
                    isDRCS = true;
                    fin = seq[1];
                }
                else {
                    fin = seq[0];
                }
                const int gx = 0; // ★ 形式Aは G0 固定
                setG(gx, hintFromFinal(fin, twoB, isDRCS), twoB, isDRCS, fin);
                return;
            }
        }

        // その他: 何もしない
    }


    // 受け取った先頭 maxBytes の Hex/ASCII を出す簡易プレビュー
static void logPayloadPreview(AribDecoderProbe* self, const uint8_t* p, size_t n, size_t maxBytes = 32) {
    if (!self || !p || n == 0) return;
    const size_t m = (n < maxBytes ? n : maxBytes);

    std::wstring hex; hex.reserve(m * 3 + 32);
    for (size_t i=0; i<m; ++i) {
        wchar_t b[4]; std::swprintf(b, 4, L"%02X", p[i]);
        hex += b; if (i+1<m) hex += L' ';
    }
    std::wstring asc; asc.reserve(m + 32);
    for (size_t i=0; i<m; ++i) {
        asc += (p[i] >= 0x20 && p[i] <= 0x7E) ? static_cast<wchar_t>(p[i]) : L'.';
    }
    wchar_t head[128];
    std::swprintf(head, 128, L"[ARIB-PROBE][preview] len=%zu show=%zu", n, m);

    self->EmitLog(head);
    self->EmitLog(std::wstring(L"[ARIB-PROBE][hex] ") + hex);
    self->EmitLog(std::wstring(L"[ARIB-PROBE][asc] ") + asc);
}

    // ========== 1バッファ解析 ==========
    AnalyzeSummary AribDecoderProbe::AnalyzeOnce(const uint8_t* data, size_t length, std::vector<ControlEvent>* outEvents) noexcept {


        Reset();

        logPayloadPreview(this, data, length, 32);

        Feed(data, length, outEvents);

        std::wstring s;
        s.reserve(256); // 任意
        s += L"[ARIB-PROBE][state] G0="; s += csName(m_state.G[0]);
        s += L" G1="; s += csName(m_state.G[1]);
        s += L" G2="; s += csName(m_state.G[2]);
        s += L" G3="; s += csName(m_state.G[3]);
        s += L" | GL=G"; s += std::to_wstring(m_state.GL);
        s += L" GR=G";  s += std::to_wstring(m_state.GR);
        s += (m_state.SS2 ? L" | SS2=on" : L" | SS2=off");
        s += (m_state.SS3 ? L" | SS3=on" : L" | SS3=off");
        
        log(s);

        // ★ 符号方式の最終推定（ARIB優先）
        bool has_strong_arib = (m_summary.countESC > 0) || (m_summary.countSS2 > 0) || (m_summary.countSS3 > 0);
        auto& e = m_summary.enc;

        double sum = e.arib + e.utf8 + e.ascii + e.bin + 1e-9;
        double best = e.arib; TextEncodingGuess w = TextEncodingGuess::ARIB;

        if (!has_strong_arib) {
            if (e.utf8 > best) { best = e.utf8;  w = TextEncodingGuess::UTF8; }
            if (e.ascii > best) { best = e.ascii; w = TextEncodingGuess::ASCII; }
            if (e.bin > best) { best = e.bin;   w = TextEncodingGuess::Binary; }
        }
        e.winner = w; e.confidence = best / sum;

        const wchar_t* wname = L"?";
        switch (w) {
        case TextEncodingGuess::ARIB:   wname = L"ARIB";   break;
        case TextEncodingGuess::UTF8:   wname = L"UTF-8";  break;
        case TextEncodingGuess::ASCII:  wname = L"ASCII";  break;
        case TextEncodingGuess::Binary: wname = L"Binary"; break;
        default: break;
        }

        {
            const auto& st = m_state;
            wchar_t sbuf[256];
            std::swprintf(sbuf, 256,
                L"[ARIB-PROBE][summary] 1B=%zu 2B=%zu ESC=%zu SS2=%zu SS3=%zu | G0=%ls G1=%ls G2=%ls G3=%ls | GL=G%d GR=G%d",
                m_summary.count1B, m_summary.count2B, m_summary.countESC, m_summary.countSS2, m_summary.countSS3,
                csName(st.G[0]), csName(st.G[1]), csName(st.G[2]), csName(st.G[3]),
                st.GL, st.GR);
            log(sbuf);
        }
        wchar_t b2[192];
        std::swprintf(b2, 192,
            L"[ARIB-PROBE][encoding] guess=%ls conf=%.2f  (arib=%.1f utf8=%.1f ascii=%.1f bin=%.1f)",
            wname, e.confidence, e.arib, e.utf8, e.ascii, e.bin);
        log(b2);
        {
            const auto& st = m_state;
            wchar_t sbuf[256];
            std::swprintf(sbuf, 256,
                L"[ARIB-PROBE][summary] 1B=%zu 2B=%zu ESC=%zu SS2=%zu SS3=%zu | G0=%ls G1=%ls G2=%ls G3=%ls | GL=G%d GR=G%d",
                m_summary.count1B, m_summary.count2B, m_summary.countESC, m_summary.countSS2, m_summary.countSS3,
                csName(st.G[0]), csName(st.G[1]), csName(st.G[2]), csName(st.G[3]),
                st.GL, st.GR);
            log(sbuf);
        }
        return m_summary;
    }

} // namespace arib

