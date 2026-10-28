#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "AppSimdSse42.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#if defined(_M_X64) || defined(_M_AMD64) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || \
    defined(__SSE2__)
#define APP_SIMD_SSE42_COMPILE_ENABLED_ 1
#else
#define APP_SIMD_SSE42_COMPILE_ENABLED_ 0
#endif

#if APP_SIMD_SSE42_COMPILE_ENABLED_
#include <emmintrin.h>
#endif

namespace
{
    bool IsFixedMarkerAt_(
        const wchar_t* data,
        std::size_t size,
        std::size_t pos,
        std::wstring_view marker) noexcept
    {
        if (data == nullptr) {
            return false;
        }

        if (marker.empty()) {
            return false;
        }

        if (pos > size) {
            return false;
        }

        if (marker.size() > size - pos) {
            return false;
        }

        for (std::size_t i = 0; i < marker.size(); ++i) {
            if (data[pos + i] != marker[i]) {
                return false;
            }
        }

        return true;
    }

    bool IsTimeShiftEpgTableIdPrefixAt_(
        const std::wstring& text,
        std::size_t pos) noexcept
    {
        if (pos + 8u > text.size()) {
            return false;
        }

        return
            text[pos + 0u] == L'T' &&
            text[pos + 1u] == L'a' &&
            text[pos + 2u] == L'b' &&
            text[pos + 3u] == L'l' &&
            text[pos + 4u] == L'e' &&
            text[pos + 5u] == L'I' &&
            text[pos + 6u] == L'D' &&
            text[pos + 7u] == L':';
    }

    bool IsTimeShiftEpgTableIdLineHeadAt_(
        const std::wstring& text,
        std::size_t pos) noexcept
    {
        if (!IsTimeShiftEpgTableIdPrefixAt_(text, pos)) {
            return false;
        }

        return
            pos == 0u ||
            text[pos - 1u] == L'\n' ||
            text[pos - 1u] == L'\r';
    }

#if APP_SIMD_SSE42_COMPILE_ENABLED_

    bool CollectFixedTextOffsetsSse42Utf16_(
        const std::wstring& text,
        std::wstring_view marker,
        std::vector<std::size_t>& outOffsets) noexcept
    {
        try {
            std::vector<std::size_t> offsets;

            if constexpr (sizeof(wchar_t) != 2u) {
                outOffsets.clear();
                return false;
            }
            else {
                const std::size_t n =
                    text.size();

                const std::size_t markerSize =
                    marker.size();

                if (n == 0u || markerSize == 0u) {
                    outOffsets.swap(offsets);
                    return true;
                }

                if (n < markerSize) {
                    outOffsets.swap(offsets);
                    return true;
                }

                const wchar_t* const p =
                    text.data();

                const wchar_t firstChar =
                    marker.front();

                const __m128i needle =
                    _mm_set1_epi16(
                        static_cast<short>(
                            static_cast<std::uint16_t>(firstChar)));

                constexpr std::size_t kLanes = 8u;

                std::size_t i = 0;
                std::size_t nextAllowed = 0;

                for (; i + kLanes <= n; i += kLanes) {
                    const __m128i block =
                        _mm_loadu_si128(
                            reinterpret_cast<const __m128i*>(p + i));

                    const __m128i cmp =
                        _mm_cmpeq_epi16(
                            block,
                            needle);

                    const unsigned int mask =
                        static_cast<unsigned int>(
                            _mm_movemask_epi8(cmp));

                    if (mask == 0u) {
                        continue;
                    }

                    for (unsigned int lane = 0u; lane < 8u; ++lane) {
                        const unsigned int laneMask =
                            0x3u << (lane * 2u);

                        if ((mask & laneMask) != laneMask) {
                            continue;
                        }

                        const std::size_t pos =
                            i + static_cast<std::size_t>(lane);

                        if (pos < nextAllowed) {
                            continue;
                        }

                        if (IsFixedMarkerAt_(
                            p,
                            n,
                            pos,
                            marker)) {
                            offsets.push_back(pos);
                            nextAllowed = pos + markerSize;
                        }
                    }
                }

                for (; i < n; ++i) {
                    if (i < nextAllowed) {
                        continue;
                    }

                    if (p[i] != firstChar) {
                        continue;
                    }

                    if (IsFixedMarkerAt_(
                        p,
                        n,
                        i,
                        marker)) {
                        offsets.push_back(i);
                        nextAllowed = i + markerSize;
                    }
                }

                outOffsets.swap(offsets);
                return true;
            }
        }
        catch (...) {
            outOffsets.clear();
            return false;
        }
    }

    bool CollectTimeShiftTableIdLineHeadSse42Utf16_(
        const std::wstring& text,
        std::vector<std::size_t>& outOffsets) noexcept
    {
        try {
            std::vector<std::size_t> offsets;

            if constexpr (sizeof(wchar_t) != 2u) {
                outOffsets.clear();
                return false;
            }
            else {
                const std::size_t n =
                    text.size();

                if (n < 8u) {
                    outOffsets.swap(offsets);
                    return true;
                }

                const wchar_t* const p =
                    text.data();

                const __m128i needle =
                    _mm_set1_epi16(
                        static_cast<short>(
                            static_cast<std::uint16_t>(L'T')));

                constexpr std::size_t kLanes = 8u;

                std::size_t i = 0;

                for (; i + kLanes <= n; i += kLanes) {
                    const __m128i block =
                        _mm_loadu_si128(
                            reinterpret_cast<const __m128i*>(p + i));

                    const __m128i cmp =
                        _mm_cmpeq_epi16(
                            block,
                            needle);

                    const unsigned int mask =
                        static_cast<unsigned int>(
                            _mm_movemask_epi8(cmp));

                    if (mask == 0u) {
                        continue;
                    }

                    for (unsigned int lane = 0u; lane < 8u; ++lane) {
                        const unsigned int laneMask =
                            0x3u << (lane * 2u);

                        if ((mask & laneMask) != laneMask) {
                            continue;
                        }

                        const std::size_t pos =
                            i + static_cast<std::size_t>(lane);

                        if (IsTimeShiftEpgTableIdLineHeadAt_(text, pos)) {
                            offsets.push_back(pos);
                        }
                    }
                }

                for (; i < n; ++i) {
                    if (p[i] != L'T') {
                        continue;
                    }

                    if (IsTimeShiftEpgTableIdLineHeadAt_(text, i)) {
                        offsets.push_back(i);
                    }
                }

                outOffsets.swap(offsets);
                return true;
            }
        }
        catch (...) {
            outOffsets.clear();
            return false;
        }
    }

#endif
}

namespace AppSimdSse42
{
    bool CollectTimeShiftEpgTableBlockOffsets(
        const std::wstring& text,
        std::vector<std::size_t>& outOffsets) noexcept
    {
#if APP_SIMD_SSE42_COMPILE_ENABLED_
        return CollectTimeShiftTableIdLineHeadSse42Utf16_(
            text,
            outOffsets);
#else
        (void)text;

        outOffsets.clear();
        return false;
#endif
    }

    bool CollectEpgSearchFixedTextOffsets(
        const std::wstring& text,
        std::wstring_view marker,
        std::vector<std::size_t>& outOffsets) noexcept
    {
#if APP_SIMD_SSE42_COMPILE_ENABLED_
        return CollectFixedTextOffsetsSse42Utf16_(
            text,
            marker,
            outOffsets);
#else
        (void)text;
        (void)marker;

        outOffsets.clear();
        return false;
#endif
    }

    bool CollectEpgSearchTableIdOffsets(
        const std::wstring& text,
        std::vector<std::size_t>& outOffsets) noexcept
    {
#if APP_SIMD_SSE42_COMPILE_ENABLED_
        return CollectFixedTextOffsetsSse42Utf16_(
            text,
            L"TableID",
            outOffsets);
#else
        (void)text;

        outOffsets.clear();
        return false;
#endif
    }
}