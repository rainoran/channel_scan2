#pragma once
#ifndef APP_SIMD_SSE2_HPP
#define APP_SIMD_SSE2_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace AppSimdSse2
{
    // TimeShift用:
    // EPG dump の UTF-16 text から、行頭 "TableID:" の開始位置を収集する。
    //
    // SSE2専用。
    // x64ではSSE2は標準前提。
    //
    // 注意:
    //   呼び出し側でCPU SSE2対応確認済みの場合だけ呼ぶ。
    //   false の場合は呼び出し側で Scalar へ戻す。
    bool CollectTimeShiftEpgTableBlockOffsets(
        const std::wstring& text,
        std::vector<std::size_t>& outOffsets) noexcept;

    // EpgHtmlPortal_SearchData 用:
    // 固定文字列 marker の開始位置を収集する。
    //
    // LoadMergedEvents側のScalar検索と結果を合わせるため、
    // markerヒット後は marker.size() 分だけ進める非重複検索にする。
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

#endif // APP_SIMD_SSE2_HPP