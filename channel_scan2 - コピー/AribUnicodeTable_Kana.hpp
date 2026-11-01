// AribUnicodeTable_Kana.hpp
#pragma once
#include <cstddef>
#include <iterator> // std::size

namespace decoder {

    // ARIB カタカナ（G94相当の並び。※不足は 0 返しでU+FFFDに落とす）
    static const wchar_t AribKataKanaTable[] = {
        /*L'　',*/ L'ァ', L'ア', L'ィ', L'イ', L'ゥ', L'ウ', L'ェ', L'エ', L'ォ', L'オ', L'カ', L'ガ', L'キ', L'ギ', L'ク',
        L'グ', L'ケ', L'ゲ', L'コ', L'ゴ', L'サ', L'ザ', L'シ', L'ジ', L'ス', L'ズ', L'セ', L'ゼ', L'ソ', L'ゾ', L'タ',
        L'ダ', L'チ', L'ヂ', L'ッ', L'ツ', L'ヅ', L'テ', L'デ', L'ト', L'ド', L'ナ', L'ニ', L'ヌ', L'ネ', L'ノ', L'ハ',
        L'バ', L'パ', L'ヒ', L'ビ', L'ピ', L'フ', L'ブ', L'プ', L'ヘ', L'ベ', L'ペ', L'ホ', L'ボ', L'ポ', L'マ', L'ミ',
        L'ム', L'メ', L'モ', L'ャ', L'ヤ', L'ュ', L'ユ', L'ョ', L'ヨ', L'ラ', L'リ', L'ル', L'レ', L'ロ', L'ヮ', L'ワ',
        L'ヰ', L'ヱ', L'ヲ', L'ン', L'ヴ', L'ヵ', L'ヶ', L'ヽ', L'ヾ', L'ー', L'。', L'「', L'」', L'、', L'・', L'　',
    };

    // 参考：JIS X 0201（G96系の半角カナ相当の並び・サブセット）
    // ※使う場合は G96 デコード側で別ルートにしてください
    static const wchar_t AribJISKataKanaTable[] = {
        /*L'　',*/ L'。', L'「', L'」', L'、', L'・', L'ヲ', L'ァ', L'ィ', L'ゥ', L'ェ', L'ォ', L'ャ', L'ュ', L'ョ', L'ッ',
        L'ー', L'ア', L'イ', L'ウ', L'エ', L'オ', L'カ', L'キ', L'ク', L'ケ', L'コ', L'サ', L'シ', L'ス', L'セ', L'ソ',
        L'タ', L'チ', L'ツ', L'テ', L'ト', L'ナ', L'ニ', L'ヌ', L'ネ', L'ノ', L'ハ', L'ヒ', L'フ', L'ヘ', L'ホ', L'マ',
        L'ミ', L'ム', L'メ', L'モ', L'ヤ', L'ユ', L'ヨ', L'ラ', L'リ', L'ル', L'レ', L'ロ', L'ワ', L'ン', L'゛', L'゜',
    };

    static const wchar_t AribHiraGanaTable[] = {
        /*L'　',*/ L'ぁ', L'あ', L'ぃ', L'い', L'ぅ', L'う', L'ぇ', L'え', L'ぉ', L'お', L'か', L'が', L'き', L'ぎ', L'く',
        L'ぐ', L'け', L'げ', L'こ', L'ご', L'さ', L'ざ', L'し', L'じ', L'す', L'ず', L'せ', L'ぜ', L'そ', L'ぞ', L'た',
        L'だ', L'ち', L'ぢ', L'っ', L'つ', L'づ', L'て', L'で', L'と', L'ど', L'な', L'に', L'ぬ', L'ね', L'の', L'は',
        L'ば', L'ぱ', L'ひ', L'び', L'ぴ', L'ふ', L'ぶ', L'ぷ', L'へ', L'べ', L'ぺ', L'ほ', L'ぼ', L'ぽ', L'ま', L'み',
        L'む', L'め', L'も', L'ゃ', L'や', L'ゅ', L'ゆ', L'ょ', L'よ', L'ら', L'り', L'る', L'れ', L'ろ', L'ゎ', L'わ',
        L'ゐ', L'ゑ', L'を', L'ん', L'　', L'　', L'　', L'ゝ', L'ゞ', L'ー', L'。', L'「', L'」', L'、', L'・', L'　',
    };

    // ===== 1B/G94 デコード（ix: 0..93）。配列外・未配置は 0 を返す =====
    inline uint32_t DecodeAribKatakana(uint8_t ix, int) {
        const std::size_t n = std::size(AribKataKanaTable);
        if (ix >= n) return 0;
        const wchar_t ch = AribKataKanaTable[ix];
        return ch ? static_cast<uint32_t>(ch) : 0;
    }
    inline uint32_t DecodeAribHiragana(uint8_t ix, int) {
        const std::size_t n = std::size(AribHiraGanaTable);
        if (ix >= n) return 0;
        const wchar_t ch = AribHiraGanaTable[ix];
        return ch ? static_cast<uint32_t>(ch) : 0;
    }

} // namespace decoder
