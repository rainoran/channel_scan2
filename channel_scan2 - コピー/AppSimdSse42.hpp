#pragma once
#ifndef APP_SIMD_SSE42_HPP
#define APP_SIMD_SSE42_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace AppSimdSse42
{
    // TimeShift用:
    // EPG dump の UTF-16 text から、行頭 "TableID:" の開始位置を収集する。
    //
    // 現段階:
    //   SSE4.2専用ユニットの接続確認用。
    //   実装は安全性優先のSSE128比較カーネル。
    bool CollectTimeShiftEpgTableBlockOffsets(
        const std::wstring& text,
        std::vector<std::size_t>& outOffsets) noexcept;

    // EpgHtmlPortal_SearchData 用:
    // 固定文字列 marker の開始位置を収集する。
    //
    // 現段階:
    //   SSE4.2経路として呼ばれるが、EPG検索では結果一致を優先し、
    //   まずSSE128比較カーネルで実装する。
    bool CollectEpgSearchFixedTextOffsets(
        const std::wstring& text,
        std::wstring_view marker,
        std::vector<std::size_t>& outOffsets) noexcept;

    // EpgHtmlPortal_SearchData 用:
    // merged EPG text 内の "TableID" 開始位置を収集する。
    //
    // 既存の検索DB側Scalar scanと合わせるため、
    // 行頭限定や ':' 限定はしない。
    bool CollectEpgSearchTableIdOffsets(
        const std::wstring& text,
        std::vector<std::size_t>& outOffsets) noexcept;
}

#endif // APP_SIMD_SSE42_HPP