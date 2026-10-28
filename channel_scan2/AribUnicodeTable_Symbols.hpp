// AribUnicodeTable_Symbols.hpp
#ifndef ARIB_UNICODE_TABLE_SYMBOLS_HPP
#define ARIB_UNICODE_TABLE_SYMBOLS_HPP
#pragma once

#include <cstdint>

namespace decoder {

    // detail::LookupSymbolString の前方宣言
    namespace detail {
        const char32_t* LookupSymbolString(uint16_t code, bool preferUnicode) noexcept;
    }
    // 追加記号 / 追加漢字の「フル文字列」版
    inline const char32_t* DecodeAribAdditionalSymbolString(
        uint8_t ku_ix,
        uint8_t ten_ix,
        bool preferUnicode = true) noexcept
    {
        if (ku_ix >= 94 || ten_ix >= 94)
            return nullptr;

        const uint16_t ku = static_cast<uint16_t>(ku_ix + 0x21);
        const uint16_t ten = static_cast<uint16_t>(ten_ix + 0x21);
        const uint16_t code = static_cast<uint16_t>((ku << 8) | ten);

        return detail::LookupSymbolString(code, preferUnicode);
    }

    namespace detail {

        // 追加記号・追加漢字用の範囲テーブル（char32_t 版）
        struct SymbolRange32 {
            uint16_t first;   // 開始コード (例: 0x7A21)
            uint16_t last;    // 終了コード (例: 0x7A48)
            const char32_t* const* table;   // 通常テーブル
            const char32_t* const* tableU;  // Unicode 版テーブル（あれば）
        };

        // ===== 各テーブル定義 =====

        // 90区 1〜40
        static const char32_t* const SymbolsTable_90_01[] = {
            U"\u26cc",     U"\u26cd",     U"\u2757",     U"\u26cf",     // 0x7A21 - 0x7A24  90/01 - 90/04
            U"\u26d0",     U"\u26d1",     nullptr,       U"\u26d2",     // 0x7A25 - 0x7A28  90/05 - 90/08
            U"\u26d5",     U"\u26d3",     U"\u26d4",     nullptr,       // 0x7A29 - 0x7A2C  90/09 - 90/12
            nullptr,       nullptr,       nullptr,       U"\U0001f17f", // 0x7A2D - 0x7A30  90/13 - 90/16
            U"\U0001f18a", nullptr,       nullptr,       U"\u26d6",     // 0x7A31 - 0x7A34  90/17 - 90/20
            U"\u26d7",     U"\u26d8",     U"\u26d9",     U"\u26da",     // 0x7A35 - 0x7A38  90/21 - 90/24
            U"\u26db",     U"\u26dc",     U"\u26dd",     U"\u26de",     // 0x7A39 - 0x7A3C  90/25 - 90/28
            U"\u26df",     U"\u26e0",     U"\u26e1",     U"\u2b55",     // 0x7A3D - 0x7A40  90/29 - 90/32
            U"\u3248",     U"\u3249",     U"\u324a",     U"\u324b",     // 0x7A41 - 0x7A44  90/33 - 90/36
            U"\u324c",     U"\u324d",     U"\u324e",     U"\u324f",     // 0x7A45 - 0x7A48  90/37 - 90/40
        };

        // 90区 45〜84 (通常テーブル)
        static const char32_t* const SymbolsTable_90_45[] = {
            U"10.",        U"11.",        U"12.",        U"[HV]",       // 0x7A4D - 0x7A50  90/45 - 90/48
            U"[SD]",       U"[Ｐ]",       U"[Ｗ]",       U"[MV]",       // 0x7A51 - 0x7A54  90/49 - 90/52
            U"[手]",       U"[字]",       U"[双]",       U"[デ]",       // 0x7A55 - 0x7A58  90/53 - 90/56
            U"[Ｓ]",       U"[二]",       U"[多]",       U"[解]",       // 0x7A59 - 0x7A5C  90/57 - 90/60
            U"[SS]",       U"[Ｂ]",       U"[Ｎ]",       U"■",         // 0x7A5D - 0x7A60  90/61 - 90/64
            U"●",          U"[天]",       U"[交]",       U"[映]",       // 0x7A61 - 0x7A64  90/65 - 90/68
            U"[無]",       U"[料]",       U"[年齢制限]", U"[前]",       // 0x7A65 - 0x7A68  90/69 - 90/72
            U"[後]",       U"[再]",       U"[新]",       U"[初]",       // 0x7A69 - 0x7A6C  90/73 - 90/76
            U"[終]",       U"[生]",       U"[販]",       U"[声]",       // 0x7A6D - 0x7A70  90/77 - 90/80
            U"[吹]",       U"[PPV]",      U"(秘)",       U"ほか",       // 0x7A71 - 0x7A74  90/81 - 90/84
        };

        // 90区 45〜84 (Unicode テーブル)
        static const char32_t* const SymbolsTable_90_45_U[] = {
            U"\u2491",     U"\u2492",     U"\u2493",     U"\U0001f14a", // 0x7A4D - 0x7A50  90/45 - 90/48
            U"\U0001f14c", U"\U0001f13f", U"\U0001f146", U"\U0001f14b", // 0x7A51 - 0x7A54  90/49 - 90/52
            U"\U0001f210", U"\U0001f211", U"\U0001f212", U"\U0001f213", // 0x7A55 - 0x7A58  90/53 - 90/56
            U"\U0001f142", U"\U0001f214", U"\U0001f215", U"\U0001f216", // 0x7A59 - 0x7A5C  90/57 - 90/60
            U"\U0001f14d", U"\U0001f131", U"\U0001f13d", U"\u2b1b",     // 0x7A5D - 0x7A60  90/61 - 90/64
            U"\u2b24",     U"\U0001f217", U"\U0001f218", U"\U0001f219", // 0x7A61 - 0x7A64  90/65 - 90/68
            U"\U0001f21a", U"\U0001f21b", U"\u26bf",     U"\U0001f21c", // 0x7A65 - 0x7A68  90/69 - 90/72
            U"\U0001f21d", U"\U0001f21e", U"\U0001f21f", U"\U0001f220", // 0x7A69 - 0x7A6C  90/73 - 90/76
            U"\U0001f221", U"\U0001f222", U"\U0001f223", U"\U0001f224", // 0x7A6D - 0x7A70  90/77 - 90/80
            U"\U0001f225", U"\U0001f14e", U"\u3299",     U"\U0001f200", // 0x7A71 - 0x7A74  90/81 - 90/84
        };

        // 91区
        static const char32_t* const SymbolsTable_91[] = {
            U"\u26e3",     U"\u2b56",     U"\u2b57",     U"\u2b58",     // 0x7B21 - 0x7B24  91/01 - 91/04
            U"\u2b59",     U"\u2613",     U"\u328b",     U"\u3012",     // 0x7B25 - 0x7B28  91/05 - 91/08
            U"\u26e8",     U"\u3246",     U"\u3245",     U"\u26e9",     // 0x7B29 - 0x7B2C  91/09 - 91/12
            U"\u0fd6",     U"\u26ea",     U"\u26eb",     U"\u26ec",     // 0x7B2D - 0x7B30  91/13 - 91/16
            U"\u2668",     U"\u26ed",     U"\u26ee",     U"\u26ef",     // 0x7B31 - 0x7B34  91/17 - 91/20
            U"\u2693",     U"\u2708",     U"\u26f0",     U"\u26f1",     // 0x7B35 - 0x7B38  91/21 - 91/24
            U"\u26f2",     U"\u26f3",     U"\u26f4",     U"\u26f5",     // 0x7B39 - 0x7B3C  91/25 - 91/28
            U"\U0001f157", U"\u24b9",     U"\u24c8",     U"\u26f6",     // 0x7B3D - 0x7B40  91/29 - 91/32
            U"\U0001f15f", U"\U0001f18b", U"\U0001f18d", U"\U0001f18c", // 0x7B41 - 0x7B44  91/33 - 91/36
            U"\U0001f179", U"\u26f7",     U"\u26f8",     U"\u26f9",     // 0x7B45 - 0x7B48  91/37 - 91/40
            U"\u26fa",     U"\U0001f17b", U"\u260e",     U"\u26fb",     // 0x7B49 - 0x7B4C  91/41 - 91/44
            U"\u26fc",     U"\u26fd",     U"\u26fe",     U"\U0001f17c", // 0x7B4D - 0x7B50  91/45 - 91/48
            U"\u26ff",                                               // 0x7B51 - 0x7B51  91/49 - 91/49
        };

        // 92区 (通常)
        static const char32_t* const SymbolsTable_92[] = {
            U"→",         U"←",         U"↑",         U"↓",         // 0x7C21 - 0x7C24  92/01 - 92/04
            U"○",         U"●",         U"年",         U"月",         // 0x7C25 - 0x7C28  92/05 - 92/08
            U"日",         U"円",         U"㎡",         U"立方ｍ",     // 0x7C29 - 0x7C2C  92/09 - 92/12
            U"㎝",         U"平方㎝",     U"立方㎝",     U"０.",        // 0x7C2D - 0x7C30  92/13 - 92/16
            U"１.",        U"２.",        U"３.",        U"４.",        // 0x7C31 - 0x7C34  92/17 - 92/20
            U"５.",        U"６.",        U"７.",        U"８.",        // 0x7C35 - 0x7C38  92/21 - 92/24
            U"９.",        U"氏",         U"副",         U"元",         // 0x7C39 - 0x7C3C  92/25 - 92/28
            U"故",         U"前",         U"新",         U"０,",        // 0x7C3D - 0x7C40  92/29 - 92/32
            U"１,",        U"２,",        U"３,",        U"４,",        // 0x7C41 - 0x7C44  92/33 - 92/36
            U"５,",        U"６,",        U"７,",        U"８,",        // 0x7C45 - 0x7C48  92/37 - 92/40
            U"９,",        U"(社)",       U"(財)",       U"(有)",       // 0x7C49 - 0x7C4C  92/41 - 92/44
            U"(株)",       U"(代)",       U"(問)",       U"＞",         // 0x7C4D - 0x7C50  92/45 - 92/48
            U"＜",         U"【",         U"】",         U"◇",         // 0x7C51 - 0x7C54  92/49 - 92/52
            U"^2",         U"^3",         U"(CD)",       U"(vn)",       // 0x7C55 - 0x7C58  92/53 - 92/56
            U"(ob)",       U"(cb)",       U"(ce",        U"mb)",        // 0x7C59 - 0x7C5C  92/57 - 92/60
            U"(hp)",       U"(br)",       U"(p)",        U"(s)",        // 0x7C5D - 0x7C60  92/61 - 92/64
            U"(ms)",       U"(t)",        U"(bs)",       U"(b)",        // 0x7C61 - 0x7C64  92/65 - 92/68
            U"(tb)",       U"(tp)",       U"(ds)",       U"(ag)",       // 0x7C65 - 0x7C68  92/69 - 92/72
            U"(eg)",       U"(vo)",       U"(fl)",       U"(ke",        // 0x7C69 - 0x7C6C  92/73 - 92/76
            U"y)",         U"(sa",        U"x)",         U"(sy",        // 0x7C6D - 0x7C70  92/77 - 92/80
            U"n)",         U"(or",        U"g)",         U"(pe",        // 0x7C71 - 0x7C74  92/81 - 92/84
            U"r)",         U"(R)",        U"(C)",        U"(箏)",       // 0x7C75 - 0x7C78  92/85 - 92/88
            U"DJ",         U"[演]",       U"Fax",                     // 0x7C79 - 0x7C7B  92/89 - 92/91
        };

        // 92区 (Unicode)
        static const char32_t* const SymbolsTable_92_U[] = {
            U"\u27a1",     U"\u2b05",     U"\u2b06",     U"\u2b07",     // 0x7C21 - 0x7C24  92/01 - 92/04
            U"\u2b2f",     U"\u2b2e",     U"年",         U"月",         // 0x7C25 - 0x7C28  92/05 - 92/08
            U"日",         U"円",         U"㎡",         U"\u33a5",     // 0x7C29 - 0x7C2C  92/09 - 92/12
            U"㎝",         U"\u33a0",     U"\u33a4",     U"\U0001f100", // 0x7C2D - 0x7C30  92/13 - 92/16
            U"\u2488",     U"\u2489",     U"\u248a",     U"\u248b",     // 0x7C31 - 0x7C34  92/17 - 92/20
            U"\u248c",     U"\u248d",     U"\u248e",     U"\u248f",     // 0x7C35 - 0x7C38  92/21 - 92/24
            U"\u2490",     U"氏",         U"副",         U"元",         // 0x7C39 - 0x7C3C  92/25 - 92/28
            U"故",         U"前",         U"新",         U"\U0001f101", // 0x7C3D - 0x7C40  92/29 - 92/32
            U"\U0001f102", U"\U0001f103", U"\U0001f104", U"\U0001f105", // 0x7C41 - 0x7C44  92/33 - 92/36
            U"\U0001f106", U"\U0001f107", U"\U0001f108", U"\U0001f109", // 0x7C45 - 0x7C48  92/37 - 92/40
            U"\U0001f10a", U"\u3233",     U"\u3236",     U"\u3232",     // 0x7C49 - 0x7C4C  92/41 - 92/44
            U"\u3231",     U"\u3239",     U"\u3244",     U"\u25b6",     // 0x7C4D - 0x7C50  92/45 - 92/48
            U"\u25c0",     U"\u3016",     U"\u3017",     U"\u27d0",     // 0x7C51 - 0x7C54  92/49 - 92/52
            U"\u00b2",     U"\u00b3",     U"\U0001f12d", U"(vn)",       // 0x7C55 - 0x7C58  92/53 - 92/56
            U"(ob)",       U"(cb)",       U"(ce",        U"mb)",        // 0x7C59 - 0x7C5C  92/57 - 92/60
            U"(hp)",       U"(br)",       U"(p)",        U"(s)",        // 0x7C5D - 0x7C60  92/61 - 92/64
            U"(ms)",       U"(t)",        U"(bs)",       U"(b)",        // 0x7C61 - 0x7C64  92/65 - 92/68
            U"(tb)",       U"(tp)",       U"(ds)",       U"(ag)",       // 0x7C65 - 0x7C68  92/69 - 92/72
            U"(eg)",       U"(vo)",       U"(fl)",       U"(ke",        // 0x7C69 - 0x7C6C  92/73 - 92/76
            U"y)",         U"(sa",        U"x)",         U"(sy",        // 0x7C6D - 0x7C70  92/77 - 92/80
            U"n)",         U"(or",        U"g)",         U"(pe",        // 0x7C71 - 0x7C74  92/81 - 92/84
            U"r)",         U"\U0001f12c", U"\U0001f12b", U"\u3247",     // 0x7C75 - 0x7C78  92/85 - 92/88
            U"\U0001f190", U"\U0001f226", U"\u213b",                 // 0x7C79 - 0x7C7B  92/89 - 92/91
        };

        // 93区 (通常)
        static const char32_t* const SymbolsTable_93[] = {
            U"(月)",       U"(火)",       U"(水)",       U"(木)",       // 0x7D21 - 0x7D24  93/01 - 93/04
            U"(金)",       U"(土)",       U"(日)",       U"(祝)",       // 0x7D25 - 0x7D28  93/05 - 93/08
            U"㍾",         U"㍽",         U"㍼",         U"㍻",         // 0x7D29 - 0x7D2C  93/09 - 93/12
            U"№",          U"℡",         U"(〒)",       U"○",          // 0x7D2D - 0x7D30  93/13 - 93/16
            U"〔本〕",     U"〔三〕",     U"〔二〕",     U"〔安〕",     // 0x7D31 - 0x7D34  93/17 - 93/20
            U"〔点〕",     U"〔打〕",     U"〔盗〕",     U"〔勝〕",     // 0x7D35 - 0x7D38  93/21 - 93/24
            U"〔敗〕",     U"〔Ｓ〕",     U"［投］",     U"［捕］",     // 0x7D39 - 0x7D3C  93/25 - 93/28
            U"［一］",     U"［二］",     U"［三］",     U"［遊］",     // 0x7D3D - 0x7D40  93/29 - 93/32
            U"［左］",     U"［中］",     U"［右］",     U"［指］",     // 0x7D41 - 0x7D44  93/33 - 93/36
            U"［走］",     U"［打］",     U"㍑",         U"㎏",         // 0x7D45 - 0x7D48  93/37 - 93/40
            U"Hz",         U"ha",         U"km",         U"平方km",     // 0x7D49 - 0x7D4C  93/41 - 93/44
            U"hPa",        nullptr,       nullptr,       U"1/2",        // 0x7D4D - 0x7D50  93/45 - 93/48
            U"0/3",        U"1/3",        U"2/3",        U"1/4",        // 0x7D51 - 0x7D54  93/49 - 93/52
            U"3/4",        U"1/5",        U"2/5",        U"3/5",        // 0x7D55 - 0x7D58  93/53 - 93/56
            U"4/5",        U"1/6",        U"5/6",        U"1/7",        // 0x7D59 - 0x7D5C  93/57 - 93/60
            U"1/8",        U"1/9",        U"1/10",       U"晴れ",       // 0x7D5D - 0x7D60  93/61 - 93/64
            U"曇り",       U"雨",         U"雪",         U"△",         // 0x7D61 - 0x7D64  93/65 - 93/68
            U"▲",          U"▽",         U"▼",          U"◆",          // 0x7D65 - 0x7D68  93/69 - 93/72
            U"・",         U"・",         U"・",         U"◇",         // 0x7D69 - 0x7D6C  93/73 - 93/76
            U"◎",          U"!!",         U"!?",         U"曇/晴",      // 0x7D6D - 0x7D70  93/77 - 93/80
            U"雨",         U"雨",         U"雪",         U"大雪",       // 0x7D71 - 0x7D74  93/81 - 93/84
            U"雷",         U"雷雨",       U"　",         U"・",         // 0x7D75 - 0x7D78  93/85 - 93/88
            U"・",         U"♪",         U"℡",                         // 0x7D79 - 0x7D7B  93/89 - 93/91
        };

        // 93区 (Unicode)
        static const char32_t* const SymbolsTable_93_U[] = {
            U"\u322a",     U"\u322b",     U"\u322c",     U"\u322d",     // 0x7D21 - 0x7D24  93/01 - 93/04
            U"\u322e",     U"\u322f",     U"\u3230",     U"\u3237",     // 0x7D25 - 0x7D28  93/05 - 93/08
            U"㍾",         U"㍽",         U"㍼",         U"㍻",         // 0x7D29 - 0x7D2C  93/09 - 93/12
            U"№",          U"℡",         U"\u3036",     U"\u26be",     // 0x7D2D - 0x7D30  93/13 - 93/16
            U"\U0001f240", U"\U0001f241", U"\U0001f242", U"\U0001f243", // 0x7D31 - 0x7D34  93/17 - 93/20
            U"\U0001f244", U"\U0001f245", U"\U0001f246", U"\U0001f247", // 0x7D35 - 0x7D38  93/21 - 93/24
            U"\U0001f248", U"\U0001f12a", U"\U0001f227", U"\U0001f228", // 0x7D39 - 0x7D3C  93/25 - 93/28
            U"\U0001f229", U"\U0001f214", U"\U0001f22a", U"\U0001f22b", // 0x7D3D - 0x7D40  93/29 - 93/32
            U"\U0001f22c", U"\U0001f22d", U"\U0001f22e", U"\U0001f22f", // 0x7D41 - 0x7D44  93/33 - 93/36
            U"\U0001f230", U"\U0001f231", U"\u2113",     U"㎏",         // 0x7D45 - 0x7D48  93/37 - 93/40
            U"\u3390",     U"\u33ca",     U"\u339e",     U"\u33a2",     // 0x7D49 - 0x7D4C  93/41 - 93/44
            U"\u3371",     nullptr,       nullptr,       U"\u00bd",     // 0x7D4D - 0x7D50  93/45 - 93/48
            U"\u2189",     U"\u2153",     U"\u2154",     U"\u00bc",     // 0x7D51 - 0x7D54  93/49 - 93/52
            U"\u00be",     U"\u2155",     U"\u2156",     U"\u2157",     // 0x7D55 - 0x7D58  93/53 - 93/56
            U"\u2158",     U"\u2159",     U"\u215a",     U"\u2150",     // 0x7D59 - 0x7D5C  93/57 - 93/60
            U"\u215b",     U"\u2151",     U"\u2152",     U"\u2600",     // 0x7D5D - 0x7D60  93/61 - 93/64
            U"\u2601",     U"\u2602",     U"\u26c4",     U"\u2616",     // 0x7D61 - 0x7D64  93/65 - 93/68
            U"\u2617",     U"\u26c9",     U"\u26ca",     U"\u2666",     // 0x7D65 - 0x7D68  93/69 - 93/72
            U"\u2665",     U"\u2663",     U"\u2660",     U"\u26cb",     // 0x7D69 - 0x7D6C  93/73 - 93/76
            U"\u2a00",     U"\u203c",     U"\u2049",     U"\u26c5",     // 0x7D6D - 0x7D70  93/77 - 93/80
            U"\u2614",     U"\u26c6",     U"\u2603",     U"\u26c7",     // 0x7D71 - 0x7D74  93/81 - 93/84
            U"\u26a1",     U"\u26c8",     U"　",         U"\u269e",     // 0x7D75 - 0x7D78  93/85 - 93/88
            U"\u269f",     U"\u266c",     U"\u260e",                         // 0x7D79 - 0x7D7B  93/89 - 93/91
        };

        // 94区 (通常)
        static const char32_t* const SymbolsTable_94[] = {
            U"Ⅰ",         U"Ⅱ",         U"Ⅲ",         U"Ⅳ",         // 0x7E21 - 0x7E24  94/01 - 94/04
            U"Ⅴ",         U"Ⅵ",         U"Ⅶ",         U"Ⅷ",         // 0x7E25 - 0x7E28  94/05 - 94/08
            U"Ⅸ",         U"Ⅹ",         U"XI",        U"XⅡ",        // 0x7E29 - 0x7E2C  94/09 - 94/12
            U"⑰",         U"⑱",         U"⑲",         U"⑳",         // 0x7E2D - 0x7E30  94/13 - 94/16
            U"(1)",       U"(2)",       U"(3)",       U"(4)",        // 0x7E31 - 0x7E34  94/17 - 94/20
            U"(5)",       U"(6)",       U"(7)",       U"(8)",        // 0x7E35 - 0x7E38  94/21 - 94/24
            U"(9)",       U"(10)",      U"(11)",      U"(12)",       // 0x7E39 - 0x7E3C  94/25 - 94/28
            U"(21)",      U"(22)",      U"(23)",      U"(24)",       // 0x7E3D - 0x7E40  94/29 - 94/32
            U"(A)",       U"(B)",       U"(C)",       U"(D)",        // 0x7E41 - 0x7E44  94/33 - 94/36
            U"(E)",       U"(F)",       U"(G)",       U"(H)",        // 0x7E45 - 0x7E48  94/37 - 94/40
            U"(I)",       U"(J)",       U"(K)",       U"(L)",        // 0x7E49 - 0x7E4C  94/41 - 94/44
            U"(M)",       U"(N)",       U"(O)",       U"(P)",        // 0x7E4D - 0x7E50  94/45 - 94/48
            U"(Q)",       U"(R)",       U"(S)",       U"(T)",        // 0x7E51 - 0x7E54  94/49 - 94/52
            U"(U)",       U"(V)",       U"(W)",       U"(X)",        // 0x7E55 - 0x7E58  94/53 - 94/56
            U"(Y)",       U"(Z)",       U"(25)",      U"(26)",       // 0x7E59 - 0x7E5C  94/57 - 94/60
            U"(27)",      U"(28)",      U"(29)",      U"(30)",       // 0x7E5D - 0x7E60  94/61 - 94/64
            U"①",         U"②",         U"③",         U"④",         // 0x7E61 - 0x7E64  94/65 - 94/68
            U"⑤",         U"⑥",         U"⑦",         U"⑧",         // 0x7E65 - 0x7E68  94/69 - 94/72
            U"⑨",         U"⑩",         U"⑪",         U"⑫",         // 0x7E69 - 0x7E6C  94/73 - 94/76
            U"⑬",         U"⑭",         U"⑮",         U"⑯",         // 0x7E6D - 0x7E70  94/77 - 94/80
            U"①",         U"②",         U"③",         U"④",         // 0x7E71 - 0x7E74  94/81 - 94/84
            U"⑤",         U"⑥",         U"⑦",         U"⑧",         // 0x7E75 - 0x7E78  94/85 - 94/88
            U"⑨",         U"⑩",         U"⑪",         U"⑫",         // 0x7E79 - 0x7E7C  94/89 - 94/92
            U"(31)",                                                // 0x7E7D - 0x7E7D  94/93 - 94/93
        };

        // 94区 (Unicode)
        static const char32_t* const SymbolsTable_94_U[] = {
            U"Ⅰ",              U"Ⅱ",              U"Ⅲ",              U"Ⅳ",              // 0x7E21 - 0x7E24  94/01 - 94/04
            U"Ⅴ",              U"Ⅵ",              U"Ⅶ",              U"Ⅷ",              // 0x7E25 - 0x7E28  94/05 - 94/08
            U"Ⅸ",              U"Ⅹ",              U"\u216a",         U"\u216b",         // 0x7E29 - 0x7E2C  94/09 - 94/12
            U"⑰",              U"⑱",              U"⑲",              U"⑳",              // 0x7E2D - 0x7E30  94/13 - 94/16
            U"\u2474",         U"\u2475",         U"\u2476",         U"\u2477",         // 0x7E31 - 0x7E34  94/17 - 94/20
            U"\u2478",         U"\u2479",         U"\u247a",         U"\u247b",         // 0x7E35 - 0x7E38  94/21 - 94/24
            U"\u247c",         U"\u247d",         U"\u247e",         U"\u247f",         // 0x7E39 - 0x7E3C  94/25 - 94/28
            U"\u3251",         U"\u3252",         U"\u3253",         U"\u3254",         // 0x7E3D - 0x7E40  94/29 - 94/32
            U"\U0001f110",     U"\U0001f111",     U"\U0001f112",     U"\U0001f113",     // 0x7E41 - 0x7E44  94/33 - 94/36
            U"\U0001f114",     U"\U0001f115",     U"\U0001f116",     U"\U0001f117",     // 0x7E45 - 0x7E48  94/37 - 94/40
            U"\U0001f118",     U"\U0001f119",     U"\U0001f11a",     U"\U0001f11b",     // 0x7E49 - 0x7E4C  94/41 - 94/44
            U"\U0001f11c",     U"\U0001f11d",     U"\U0001f11e",     U"\U0001f11f",     // 0x7E4D - 0x7E50  94/45 - 94/48
            U"\U0001f120",     U"\U0001f121",     U"\U0001f122",     U"\U0001f123",     // 0x7E51 - 0x7E54  94/49 - 94/52
            U"\U0001f124",     U"\U0001f125",     U"\U0001f126",     U"\U0001f127",     // 0x7E55 - 0x7E58  94/53 - 94/56
            U"\U0001f128",     U"\U0001f129",     U"\u3255",         U"\u3256",         // 0x7E59 - 0x7E5C  94/57 - 94/60
            U"\u3257",         U"\u3258",         U"\u3259",         U"\u325a",         // 0x7E5D - 0x7E60  94/61 - 94/64
            U"①",              U"②",              U"③",              U"④",              // 0x7E61 - 0x7E64  94/65 - 94/68
            U"⑤",              U"⑥",              U"⑦",              U"⑧",              // 0x7E65 - 0x7E68  94/69 - 94/72
            U"⑨",              U"⑩",              U"⑪",              U"⑫",              // 0x7E69 - 0x7E6C  94/73 - 94/76
            U"⑬",              U"⑭",              U"⑮",              U"⑯",              // 0x7E6D - 0x7E70  94/77 - 94/80
            U"\u2776",         U"\u2777",         U"\u2778",         U"\u2779",         // 0x7E71 - 0x7E74  94/81 - 94/84
            U"\u277a",         U"\u277b",         U"\u277c",         U"\u277d",         // 0x7E75 - 0x7E78  94/85 - 94/88
            U"\u277e",         U"\u277f",         U"\u24eb",         U"\u24ec",         // 0x7E79 - 0x7E7C  94/89 - 94/92
            U"\u325b",                                                                // 0x7E7D - 0x7E7D  94/93 - 94/93
        };

        // 追加漢字テーブル1 (0x7521-0x757E)
        static const char32_t* const KanjiTable1[] = {
            U"\u3402",     U"\U00020158", U"\u4efd",     U"\u4eff",     // 0x7521 - 0x7524
            U"\u4f9a",     U"\u4fc9",     U"\u509c",     U"\u511e",     // 0x7525 - 0x7528
            U"\u51bc",     U"\u351f",     U"\u5307",     U"\u5361",     // 0x7529 - 0x752C
            U"\u536c",     U"\u8a79",     U"\U00020bb7", U"\u544d",     // 0x752D - 0x7530
            U"\u5496",     U"\u549c",     U"\u54a9",     U"\u550e",     // 0x7531 - 0x7534
            U"\u554a",     U"\u5672",     U"\u56e4",     U"\u5733",     // 0x7535 - 0x7538
            U"\u5734",     U"\ufa10",     U"\u5880",     U"\u59e4",     // 0x7539 - 0x753C
            U"\u5a23",     U"\u5a55",     U"\u5bec",     U"\ufa11",     // 0x753D - 0x7540
            U"\u37e2",     U"\u5eac",     U"\u5f34",     U"\u5f45",     // 0x7541 - 0x7544
            U"\u5fb7",     U"\u6017",     U"\ufa6b",     U"\u6130",     // 0x7545 - 0x7548
            U"\u6624",     U"\u66c8",     U"\u66d9",     U"\u66fa",     // 0x7549 - 0x754C
            U"\u66fb",     U"\u6852",     U"\u9fc4",     U"\u6911",     // 0x754D - 0x7550
            U"\u693b",     U"\u6a45",     U"\u6a91",     U"\u6adb",     // 0x7551 - 0x7554
            U"\U000233cc", U"\U000233fe", U"\U000235c4", U"\u6bf1",     // 0x7555 - 0x7558
            U"\u6ce0",     U"\u6d2e",     U"\ufa45",     U"\u6dbf",     // 0x7559 - 0x755C
            U"\u6dca",     U"\u6df8",     U"\ufa46",     U"\u6f5e",     // 0x755D - 0x7560
            U"\u6ff9",     U"\u7064",     U"\ufa6c",     U"\U000242ee", // 0x7561 - 0x7564
            U"\u7147",     U"\u71c1",     U"\u7200",     U"\u739f",     // 0x7565 - 0x7568
            U"\u73a8",     U"\u73c9",     U"\u73d6",     U"\u741b",     // 0x7569 - 0x756C
            U"\u7421",     U"\ufa4a",     U"\u7426",     U"\u742a",     // 0x756D - 0x7570
            U"\u742c",     U"\u7439",     U"\u744b",     U"\u3eda",     // 0x7571 - 0x7574
            U"\u7575",     U"\u7581",     U"\u7772",     U"\u4093",     // 0x7575 - 0x7578
            U"\u78c8",     U"\u78e0",     U"\u7947",     U"\u79ae",     // 0x7579 - 0x757C
            U"\u9fc6",     U"\u4103",                                  // 0x757D - 0x757E
        };

        // 追加漢字テーブル2 (0x7621-0x764B)
        static const char32_t* const KanjiTable2[] = {
            U"\u9fc5",     U"\u79da",     U"\u7a1e",     U"\u7b7f",     // 0x7621 - 0x7624
            U"\u7c31",     U"\u4264",     U"\u7d8b",     U"\u7fa1",     // 0x7625 - 0x7628
            U"\u8118",     U"\u813a",     U"\ufa6d",     U"\u82ae",     // 0x7629 - 0x762C
            U"\u845b",     U"\u84dc",     U"\u84ec",     U"\u8559",     // 0x762D - 0x7630
            U"\u85ce",     U"\u8755",     U"\u87ec",     U"\u880b",     // 0x7631 - 0x7634
            U"\u88f5",     U"\u89d2",     U"\u8af6",     U"\u8dce",     // 0x7635 - 0x7638
            U"\u8fbb",     U"\u8ff6",     U"\u90dd",     U"\u9127",     // 0x7639 - 0x763C
            U"\u912d",     U"\u91b2",     U"\u9233",     U"\u9288",     // 0x763D - 0x7640
            U"\u9321",     U"\u9348",     U"\u9592",     U"\u96de",     // 0x7641 - 0x7644
            U"\u9903",     U"\u9940",     U"\u9ad9",     U"\u9bd6",     // 0x7645 - 0x7648
            U"\u9dd7",     U"\u9eb4",     U"\u9eb5",                     // 0x7649 - 0x764B
        };

        // ===== ここで kSymbolRanges32 を定義 =====
        inline constexpr SymbolRange32 kSymbolRanges32[] = {
            // 追加漢字 (KanjiTable1/2)
            { 0x7521, 0x757E, KanjiTable1, nullptr },            // KanjiTable1
            { 0x7621, 0x764B, KanjiTable2, nullptr },            // KanjiTable2

            // 追加記号 (90〜94区)
            { 0x7A21, 0x7A48, SymbolsTable_90_01,     nullptr },            // 90/01-40
            { 0x7A4D, 0x7A74, SymbolsTable_90_45,     SymbolsTable_90_45_U },
            { 0x7B21, 0x7B51, SymbolsTable_91,        nullptr },            // 91区
            { 0x7C21, 0x7C7B, SymbolsTable_92,        SymbolsTable_92_U },  // 92区
            { 0x7D21, 0x7D7B, SymbolsTable_93,        SymbolsTable_93_U },  // 93区
            { 0x7E21, 0x7E7D, SymbolsTable_94,        SymbolsTable_94_U },  // 94区
        };

        // ===== ルックアップ関数 =====

        // code (0x7A21 など) から char32_t 文字列へのポインタを返す
        inline const char32_t* LookupSymbolString(uint16_t code, bool preferUnicode) noexcept
        {
            for (const auto& r : kSymbolRanges32) {
                if (code < r.first || code > r.last)
                    continue;

                const auto idx = static_cast<uint16_t>(code - r.first);

                const char32_t* const* tbl = nullptr;
                if (preferUnicode) {
                    // Unicode 優先: U テーブルがあればそちら、無ければ通常テーブル
                    tbl = r.tableU ? r.tableU : r.table;
                }
                else {
                    // 非 Unicode 優先: 通常テーブルがあればそちら、無ければ U テーブル
                    tbl = r.table ? r.table : r.tableU;
                }

                if (!tbl)
                    return nullptr;

                return tbl[idx]; // nullptr の可能性あり（TOFU 用）
            }
            return nullptr;
        }

        // 「とりあえず 1コードポイントだけ欲しい」用ヘルパ
// 使う側が 1文字しか受け取れない場合専用（互換用）
        inline char32_t LookupSymbolCodepoint(uint16_t code, bool preferUnicode) noexcept
        {
            const char32_t* p = LookupSymbolString(code, preferUnicode);
            if (!p || *p == U'\0')
                return U'\uFFFD'; // TOFU（未定義）

            // ★ ここから先、余計な解釈は一切しない
            // 複数文字のシンボルは先頭 1 文字だけ返す
            return *p;
        }




    } // namespace detail

    // ARIB 追加記号 / 追加漢字 (G2/G3 Additional Symbols, 追加漢字)
    // ku_ix, ten_ix は 0..93 のインデックス（0x21 を足して実コードにする）
    inline char32_t DecodeAribAdditionalSymbol(uint8_t ku_ix,
        uint8_t ten_ix,
        bool preferUnicode = true) noexcept
    {
        if (ku_ix >= 94 || ten_ix >= 94)
            return U'\uFFFD';

        const uint16_t ku = static_cast<uint16_t>(ku_ix + 0x21);
        const uint16_t ten = static_cast<uint16_t>(ten_ix + 0x21);
        const uint16_t code = static_cast<uint16_t>((ku << 8) | ten);

        return detail::LookupSymbolCodepoint(code, preferUnicode);
    }

    // 出力先は char32_t を受け取る OutputIt（例: std::back_inserter(std::u32string)）
    template <class OutputIt>
    inline bool DecodeAdditionalSymbolTo(
        uint8_t ku,        // ARIB の「区」(1〜94 を想定)
        uint8_t ten,       // ARIB の「点」(1〜94 を想定)
        bool    preferUnicode,
        OutputIt out) noexcept
    {
        // 範囲チェック
        if (ku < 1 || ku > 94 || ten < 1 || ten > 94)
            return false;

        // 0-based のインデックスへ変換
        uint8_t ku_ix = static_cast<uint8_t>(ku - 1);
        uint8_t ten_ix = static_cast<uint8_t>(ten - 1);

        // 1) フル文字列版（複数コードポイント対応）を優先
        if (const char32_t* s = DecodeAribAdditionalSymbolString(ku_ix, ten_ix, preferUnicode)) {
            if (*s != U'\0') {
                for (const char32_t* p = s; *p != U'\0'; ++p) {
                    *out++ = *p;   // 1 ARIB文字 → char32_t列を全部書き込む
                }
                return true;
            }
        }

        // 2) フォールバック：単一コードポイント版
        char32_t cp = DecodeAribAdditionalSymbol(ku_ix, ten_ix, preferUnicode);
        if (cp == U'\uFFFD' || cp == U'\0') {
            return false;
        }

        *out++ = cp;
        return true;
    }

} // namespace decoder

#endif // ARIB_UNICODE_TABLE_SYMBOLS_HPP
