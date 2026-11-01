#include "EncodingConfig.hpp"

EncodingConfig g_EncodingConfig{};  // ここが「本体」

// 既に実装済みならそのままでOK
TextEncoding ParseLogEncoding(const std::wstring& value)
{
    std::wstring lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(::towlower(ch)); });

    if (lower == L"utf8" || lower == L"utf-8") {
        return TextEncoding::Utf8;
    }
    if (lower == L"sjis" || lower == L"shift_jis" || lower == L"shift-jis") {
        return TextEncoding::ShiftJis;
    }
    return TextEncoding::Utf8;
}
