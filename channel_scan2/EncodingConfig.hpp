#pragma once
#pragma once
#ifndef ENCODING_CONFIG_HPP
#define ENCODING_CONFIG_HPP

#include <string>
#include <algorithm>
#include <cwctype>

enum class TextEncoding {
    Utf8,
    ShiftJis,
};

struct EncodingConfig {
    TextEncoding logEncoding = TextEncoding::Utf8;
};

// グローバル設定インスタンス
extern EncodingConfig g_EncodingConfig;



// "utf8" / "sjis" 等の文字列 → TextEncoding
TextEncoding ParseLogEncoding(const std::wstring& value);


#endif // ENCODING_CONFIG_HPP
