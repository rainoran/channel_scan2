// AribUnicodeTable_Latin.hpp
#pragma once
#ifndef DECODER_ARIB_UNICODE_TABLE_LATIN_HPP
#define DECODER_ARIB_UNICODE_TABLE_LATIN_HPP
#include <cstdint>

namespace decoder {


    
    // 内部実装用：Latin 拡張/特殊テーブル
    namespace detail {
    // ラテン拡張 / ラテン特殊 用のコードポイント型（UTF-32）
    using LatinChar = char32_t;

    // ラテン文字拡張コード変換テーブル
    // index = Code - 0x20 (Code が 0x20..0x7F の範囲)
    static constexpr LatinChar LatinExtensionTable[96] = {
        // 0x20..0x2F
        U' ',       U'\u00A1', U'\u00A2', U'\u00A3',
        U'\u20AC',  U'\u00A5', U'\u0160', U'\u00A7',
        U'\u0161',  U'\u00A9', U'\u00AA', U'\u00AB',
        U'\u00AC',  U'\u00FF', U'\u00AE', U'\u00AF',

        // 0x30..0x3F
        U'\u00B0',  U'\u00B1', U'\u00B2', U'\u00B3',
        U'\u017D',  U'\u03BC', U'\u00B6', U'\u00B7',
        U'\u017E',  U'\u00B9', U'\u00BA', U'\u00BB',
        U'\u0152',  U'\u0153', U'\u0178', U'\u00BF',

        // 0x40..0x4F
        U'\u00C0',  U'\u00C1', U'\u00C2', U'\u00C3',
        U'\u00C4',  U'\u00C5', U'\u00C6', U'\u00C7',
        U'\u00C8',  U'\u00C9', U'\u00CA', U'\u00CB',
        U'\u00CC',  U'\u00CD', U'\u00CE', U'\u00CF',

        // 0x50..0x5F
        U'\u00D0',  U'\u00D1', U'\u00D2', U'\u00D3',
        U'\u00D4',  U'\u00D5', U'\u00D6', U'\u00D7',
        U'\u00D8',  U'\u00D9', U'\u00DA', U'\u00DB',
        U'\u00DC',  U'\u00DD', U'\u00DE', U'\u00DF',

        // 0x60..0x6F
        U'\u00E0',  U'\u00E1', U'\u00E2', U'\u00E3',
        U'\u00E4',  U'\u00E5', U'\u00E6', U'\u00E7',
        U'\u00E8',  U'\u00E9', U'\u00EA', U'\u00EB',
        U'\u00EC',  U'\u00ED', U'\u00EE', U'\u00EF',

        // 0x70..0x7F
        U'\u00F0',  U'\u00F1', U'\u00F2', U'\u00F3',
        U'\u00F4',  U'\u00F5', U'\u00F6', U'\u00F7',
        U'\u00F8',  U'\u00F9', U'\u00FA', U'\u00FB',
        U'\u00FC',  U'\u00FD', U'\u00FE', U' '


    };

    // ラテン文字特殊コード変換テーブル
    // index = Code - 0x20 (Code が 0x20..0x4F の範囲)
    static constexpr LatinChar LatinSpecialTable[48] = {
        // 0x20..0x2F
        U' ',       U'\u266A', U' ',      U' ',
        U' ',       U' ',      U' ',      U' ',
        U' ',       U' ',      U' ',      U' ',
        U' ',       U' ',      U' ',      U' ',

        // 0x30..0x3F
        U'\u00A4',  U'\u00A6', U'\u00A8', U'\u00B4',
        U'\u00B8',  U'\u00BC', U'\u00BD', U'\u00BE',
        U' ',       U' ',      U' ',      U' ',
        U' ',       U' ',      U' ',      U' ',

        // 0x40..0x4F
        U'\u2026',  U'\u2588', U'\u2018', U'\u2019',
        U'\u201C',  U'\u201D', U'\u2022', U'\u2122',
        U'\u215B',  U'\u215C', U'\u215D', U'\u215E',
        U' ',       U' ',      U' ',      U' '
    };

	} // namespace detail

     // ラテン文字拡張 (Latin Extension)
    inline std::uint32_t DecodeLatinExtension(std::uint8_t code) noexcept
    {
        if (code < 0x20 || code >= 0x80) {
            return 0;
        }

        const std::uint8_t index = static_cast<std::uint8_t>(code - 0x20);
        return static_cast<std::uint32_t>(detail::LatinExtensionTable[index]);
    }

    // ラテン特殊 (Latin Special)
    inline std::uint32_t DecodeLatinSpecial(std::uint8_t code) noexcept
    {
        if (code < 0x20 || code >= 0x50) {
            return 0;
        }

        const std::uint8_t index = static_cast<std::uint8_t>(code - 0x20);
        return static_cast<std::uint32_t>(detail::LatinSpecialTable[index]);
    }

} // namespace decoder

#endif // DECODER_ARIB_UNICODE_TABLE_LATIN_HPP
