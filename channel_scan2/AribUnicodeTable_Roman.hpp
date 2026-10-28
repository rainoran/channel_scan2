#pragma once
#include <cstdint>

// --- ビルドトグル（既定は無効＝従来経路のみ） ---
#ifndef ARIB_USE_YEN_FOR_BACKSLASH
#define ARIB_USE_YEN_FOR_BACKSLASH 1
#endif
#ifndef ARIB_USE_FULLWIDTH_SPACE
#define ARIB_USE_FULLWIDTH_SPACE 1
#endif


namespace decoder {
    // 94 個 (0x21..0x7E → index 0..93)
    static const wchar_t RomanHalf94[94] = {
        L'!', L'"', L'#', L'$', L'%', L'&', L'\'', L'(', L')', L'*',
        L'+', L',', L'-', L'.', L'/', L'0', L'1', L'2', L'3', L'4',
        L'5', L'6', L'7', L'8', L'9', L':', L';', L'<', L'=', L'>',
        L'?', L'@', L'A', L'B', L'C', L'D', L'E', L'F', L'G', L'H',
        L'I', L'J', L'K', L'L', L'M', L'N', L'O', L'P', L'Q', L'R',
        L'S', L'T', L'U', L'V', L'W', L'X', L'Y', L'Z', L'[', L'\\',
        L']', L'^', L'_', L'`', L'a', L'b', L'c', L'd', L'e', L'f',
        L'g', L'h', L'i', L'j', L'k', L'l', L'm', L'n', L'o', L'p',
        L'q', L'r', L's', L't', L'u', L'v', L'w', L'x', L'y', L'z',
        L'{', L'|', L'}', L'~'
    };

    // 全角版（Unicode の全角英数字・記号）
    static const wchar_t RomanFull94[94] = {
        L'！', L'＂', L'＃', L'＄', L'％', L'＆', L'＇', L'（', L'）', L'＊',
        L'＋', L'，', L'－', L'．', L'／', L'０', L'１', L'２', L'３', L'４',
        L'５', L'６', L'７', L'８', L'９', L'：', L'；', L'＜', L'＝', L'＞',
        L'？', L'＠', L'Ａ', L'Ｂ', L'Ｃ', L'Ｄ', L'Ｅ', L'Ｆ', L'Ｇ', L'Ｈ',
        L'Ｉ', L'Ｊ', L'Ｋ', L'Ｌ', L'Ｍ', L'Ｎ', L'Ｏ', L'Ｐ', L'Ｑ', L'Ｒ',
        L'Ｓ', L'Ｔ', L'Ｕ', L'Ｖ', L'Ｗ', L'Ｘ', L'Ｙ', L'Ｚ', L'［', L'＼',
        L'］', L'＾', L'＿', L'｀', L'ａ', L'ｂ', L'ｃ', L'ｄ', L'ｅ', L'ｆ',
        L'ｇ', L'ｈ', L'ｉ', L'ｊ', L'ｋ', L'ｌ', L'ｍ', L'ｎ', L'ｏ', L'ｐ',
        L'ｑ', L'ｒ', L'ｓ', L'ｔ', L'ｕ', L'ｖ', L'ｗ', L'ｘ', L'ｙ', L'ｚ',
        L'｛', L'｜', L'｝', L'～'
    };

    // G94 index(0..93) → 英数（半角/全角切替）
    inline uint32_t DecodeAribAlnum(uint8_t ix, int widthMode) noexcept {
        if (ix >= 94) return 0;
        wchar_t w = (widthMode == 1) ? RomanFull94[ix] : RomanHalf94[ix];

#if ARIB_USE_YEN_FOR_BACKSLASH
        // 半角バックスラッシュは円記号に寄せる
        if (widthMode == 0 && w == L'\\') w = L'¥';
        // 全角は RomanFull94 が『＼(U+FF3C)』だが、『￥(U+FFE5)』に寄せたい場合はここで切替
        // if (widthMode == 1 && w == L'＼') w = L'￥';
#endif
        return static_cast<uint32_t>(w);
    }

    // いまは Alnum と同一（将来プロポ用に分けるならここで分岐）
    inline uint32_t DecodeAribPropAlnum(uint8_t ix, int widthMode) noexcept {
        return DecodeAribAlnum(ix, widthMode);
    }


} // namespace decoder