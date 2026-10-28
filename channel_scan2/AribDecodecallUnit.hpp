#pragma once
#ifndef ARIB_DECODECALL_UNIT_HPP
#define ARIB_DECODECALL_UNIT_HPP

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <mutex>

#include "AribDecoder.hpp"

#if defined(__cpp_lib_span) && (__cpp_lib_span >= 202002L)
#include <span>
#define ARIB_DECODECALL_HAS_SPAN 1
#else
#define ARIB_DECODECALL_HAS_SPAN 0
#endif

namespace aribdecode {

    enum class ReturnProcessFlags : uint32_t
    {
        none = 0,
        normalize_win32 = 1u << 0,
        probe_log = 1u << 1,
    };

    constexpr ReturnProcessFlags operator|(ReturnProcessFlags a, ReturnProcessFlags b) noexcept
    {
        return static_cast<ReturnProcessFlags>(
            static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    constexpr ReturnProcessFlags operator&(ReturnProcessFlags a, ReturnProcessFlags b) noexcept
    {
        return static_cast<ReturnProcessFlags>(
            static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }
    constexpr bool HasFlag(ReturnProcessFlags v, ReturnProcessFlags f) noexcept
    {
        return (static_cast<uint32_t>(v) & static_cast<uint32_t>(f)) != 0;
    }

    struct ReturnProbeResult
    {
        bool   hasEmbeddedNull = false;
        bool   hasControlChar = false;
        bool   hasBadSurrogate = false;
        size_t size = 0;
        size_t cstrLen = 0;
        size_t firstNullPos = static_cast<size_t>(-1);
    };

    class AribDecodecallUnit final
    {
    public:
        AribDecodecallUnit();
        ~AribDecodecallUnit();

        AribDecodecallUnit(const AribDecodecallUnit&) = delete;
        AribDecodecallUnit& operator=(const AribDecodecallUnit&) = delete;
        AribDecodecallUnit(AribDecodecallUnit&&) = delete;
        AribDecodecallUnit& operator=(AribDecodecallUnit&&) = delete;

        // ★追加：毎回 Reset するか（既定 true）
        void SetAutoResetBeforeDecode(bool enable) noexcept;
        bool GetAutoResetBeforeDecode() const noexcept;

        // RAW デコード（整形なし）
        std::wstring DecodeToWString(const uint8_t* data, std::size_t length);
        std::wstring DecodeToWString(const std::vector<uint8_t>& bytes);

#if ARIB_DECODECALL_HAS_SPAN
        std::wstring DecodeToWString(std::span<const uint8_t> bytes);
#endif

        // Win32向け返却（整形込み）
        std::wstring DecodeToWStringForWin32(const uint8_t* data, std::size_t length,
            bool enableProbeLog = true);
        std::wstring DecodeToWStringForWin32(const std::vector<uint8_t>& bytes,
            bool enableProbeLog = true);

#if ARIB_DECODECALL_HAS_SPAN
        std::wstring DecodeToWStringForWin32(std::span<const uint8_t> bytes,
            bool enableProbeLog = true);
#endif

        // 連結デコード
        std::wstring DecodeConcatToWString(const std::vector<std::vector<uint8_t>>& chunks);

#if ARIB_DECODECALL_HAS_SPAN
        std::wstring DecodeConcatToWString(std::span<const std::vector<uint8_t>> chunks);
#endif

        // ファイル → デコード
        std::wstring DecodeFileToWString(const std::wstring& filePath);
        std::wstring DecodeFileToWStringForWin32(const std::wstring& filePath,
            bool enableProbeLog = true);

        // 返却整形
        static void NormalizeForWin32(std::wstring& s, wchar_t nulReplace = L' ');
        static ReturnProbeResult InspectReturn(std::wstring_view s) noexcept;

        static std::string ToUtf8(std::wstring_view w);
        static bool WriteUtf8File(const std::filesystem::path& path,
            std::wstring_view w,
            bool withBom = false);

        // テスト用
        void EnableTraceForTest(bool enable) noexcept;

        // 手動リセット
        void ResetDecoder() noexcept;

    private:
        bool DecodeInternal(const uint8_t* data, std::size_t length, std::wstring& out);

        void ApplyReturnProcess(std::wstring& s,
            ReturnProcessFlags flags,
            std::wstring_view tagForLog);

    private:
        decoder::AribDecoder m_decoder;
        std::mutex m_mutex;

        // ★追加：true=毎回Reset / false=状態保持（BeginStringのみ）
        bool m_autoResetBeforeDecode = true;
    };

} // namespace aribdecode

#endif // ARIB_DECODECALL_UNIT_HPP
