#pragma once
#ifndef ARIBUNICODETABLE_HPP
#define ARIBUNICODETABLE_HPP

#include <cstddef>
#include <cstdint>
#include <climits> 
#include <vector>
#include <unordered_map>


namespace decoder {

    // 0..93 の Roman index を半角/全角にマップする
// widthMode: 0=半角ASCII, 1=全角ASCII（Fullwidth Forms）
    uint32_t DecodeAribAlnum(uint8_t ix, int widthMode) noexcept;

    // Proportional Alnum：当面は Alnum と同一挙動（将来差別化可）
    uint32_t DecodeAribPropAlnum(uint8_t ix, int widthMode) noexcept;

// ------------------------------
// ARIB Kanji Table（94区×94点）
// ------------------------------
//extern const uint32_t AribUnicodeTable_Kanji[94][94];
extern const uint32_t KanjiPlane1Table[94][94];
extern const uint32_t KanjiPlane2Table1[15][94];
extern const uint32_t KanjiPlane2Table2[17][94];

// ------------------------------
// Kanji（2バイトコード）→ UTF変換関数
// ------------------------------
size_t KanjiToUTF32(uint16_t Code, char32_t* pUTF32, size_t Length);
size_t KanjiToUTF16(uint16_t Code, char16_t* pUTF16, size_t Length);
size_t KanjiToUTF8(uint16_t Code, char* pUTF8, size_t Length);

// ------------------------------
// 記号・英数字・ひらがな・カタカナ 変換関数
// ※引数は (区, 点) = (ku, ten) - 0x21オフセット後の値
// ------------------------------
//uint32_t DecodeAribSymbol(uint8_t ku, uint8_t ten);
uint32_t DecodeAribAlnum(uint8_t ku, uint8_t ten);
uint32_t DecodeAribHiragana(uint8_t ku, uint8_t ten);
uint32_t DecodeAribKatakana(uint8_t ku, uint8_t ten);

// JIS X 0213 Plane 1 / 2 対応（Plane 2 を追加する場合も拡張しやすい構成）
uint32_t GetJISX0213Unicode(int Plane, uint16_t Code);
size_t JISX0213PlaneToUTF32(int Plane, uint16_t Code, char32_t* pUTF32, size_t Length);
size_t JISX0213PlaneToUTF16(int Plane, uint16_t Code, char16_t* pUTF16, size_t Length);
size_t JISX0213PlaneToUTF8(int Plane, uint16_t Code, char* pUTF8, size_t Length);


// --- 追加: JIS X 0213 Plane 1/2 の UTF-16 / UTF-8 変換 ---
size_t JISX0213PlaneToUTF16(int Plane, uint16_t Code, char16_t* pUTF16, size_t Length);
size_t JISX0213PlaneToUTF8(int Plane, uint16_t Code, char* pUTF8, size_t Length);

uint32_t DecodeAribPropAlnum(uint8_t ku, uint8_t ten);
uint32_t DecodeAribPropHiragana(uint8_t ku, uint8_t ten);
uint32_t DecodeAribPropKatakana(uint8_t ku, uint8_t ten);

struct DRCSGlyph {
    uint16_t width = 0;
    uint16_t height = 0;
    std::vector<uint8_t> bitmap; // 1bpp など運用に応じて解釈
};

// set: 0..15, ku/ten: 0..93 (区点)
// 戻り値: 登録成功
bool RegisterDRCSGlyph(int set, uint8_t ku, uint8_t ten, const DRCSGlyph& glyph);

// 文字として解決できる場合は Unicode を返す（なければ 0 を返す）
// 既定は 0（未解決）→ デコーダ側で U+FFFD にフォールバック
uint32_t ResolveDRCSUnicode(int set, uint8_t ku, uint8_t ten);

// set: 0..15, ku/ten: 0..93 (区点)
// 対応する Unicode コードポイントを登録する（0 は無効）
bool RegisterDRCSUnicodeMapping(int set, uint8_t ku, uint8_t ten, uint32_t codePoint);

// ビットマップを取得（見つからなければ false）
bool GetDRCSGlyph(int set, uint8_t ku, uint8_t ten, DRCSGlyph* out);

// --- 任意: wchar_t ラッパ（ビルド環境が Unicode wchar のときだけ有効）---
#if defined(LIBISDB_WCHAR) && defined(LIBISDB_WCHAR_IS_UNICODE)
inline size_t JISX0213PlaneToWChar(int Plane, uint16_t Code, wchar_t* pWChar, size_t Length)
{
#if WCHAR_MAX == 0xFFFF
    return JISX0213PlaneToUTF16(Plane, Code, reinterpret_cast<char16_t*>(pWChar), Length);
#elif WCHAR_MAX == 0xFFFFFFFF
    return JISX0213PlaneToUTF32(Plane, Code, reinterpret_cast<char32_t*>(pWChar), Length);
#else
    static_assert(false, "Unsupported wchar_t size");
#endif
}
#endif
} // namespace decoder
#endif // ARIBUNICODETABLE_HPP
