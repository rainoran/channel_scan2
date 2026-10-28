#pragma once
#ifndef APP_SIMD_AVX2_HPP
#define APP_SIMD_AVX2_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace AppSimdAvx2
{
    // TimeShift用:
    // EPG dump の UTF-16 text から、行頭 "TableID:" の開始位置を収集する。
    //
    // 戻り値:
    //   true  = AVX2実装で収集成功
    //   false = このビルド単位でAVX2が使えない、または例外発生
    //
    // 注意:
    //   呼び出し側でCPU AVX2対応確認済みの場合だけ呼ぶ。
    //   false の場合は呼び出し側で Scalar へ戻す。
    bool CollectTimeShiftEpgTableBlockOffsets(
        const std::wstring& text,
        std::vector<std::size_t>& outOffsets) noexcept;

    // EpgHtmlPortal_SearchData 用:
    // 固定文字列 marker の開始位置を収集する。
    // 現在は UTF-16 / wchar_t=2byte / Windows 前提。
    bool CollectEpgSearchFixedTextOffsets(
        const std::wstring& text,
        std::wstring_view marker,
        std::vector<std::size_t>& outOffsets) noexcept;

    // EpgHtmlPortal_SearchData 用:
    // merged EPG text 内の "TableID" 開始位置を収集する。
    // LoadMergedEvents 側の既存 scalar scan と合わせるため、行頭限定や ':' 限定はしない。
    bool CollectEpgSearchTableIdOffsets(
        const std::wstring& text,
        std::vector<std::size_t>& outOffsets) noexcept;
}

#endif // APP_SIMD_AVX2_HPP