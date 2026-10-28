#include "AribDecodecallUnit.hpp"

#include <fstream>
#include <algorithm>

#include <Windows.h>
#include "DebugManager.h"

// ------------------------------------------------------------
// AribDecodecallUnit local debug log suppression route
// ------------------------------------------------------------
// 0: 完全抑制
// 1: 出力許可
//
// [ARIB][RETURN] は enableProbeLog=true のときだけ出るが、
// EPG大量処理中に増える可能性があるので初期値は 0 にする。
#ifndef ARIB_DECODECALL_ENABLE_RETURN_LOG
#define ARIB_DECODECALL_ENABLE_RETURN_LOG 0
#endif

// ファイル読み込み失敗は異常検知なので初期値は残す。
// 完全に静かにしたい場合は 0 にする。
#ifndef ARIB_DECODECALL_ENABLE_FILE_ERROR_LOG
#define ARIB_DECODECALL_ENABLE_FILE_ERROR_LOG 1
#endif

#if ARIB_DECODECALL_ENABLE_RETURN_LOG
#define ARIB_DECODECALL_RETURN_LOG(fmt, ...) \
    do { DebugLogFormat((fmt), __VA_ARGS__); } while (0)
#else
#define ARIB_DECODECALL_RETURN_LOG(fmt, ...) \
    do { (void)0; } while (0)
#endif

#if ARIB_DECODECALL_ENABLE_FILE_ERROR_LOG
#define ARIB_DECODECALL_FILE_ERROR_LOG(fmt, ...) \
    do { DebugLogFormat((fmt), __VA_ARGS__); } while (0)
#else
#define ARIB_DECODECALL_FILE_ERROR_LOG(fmt, ...) \
    do { (void)0; } while (0)
#endif

namespace aribdecode {

    AribDecodecallUnit::AribDecodecallUnit() = default;
    AribDecodecallUnit::~AribDecodecallUnit() = default;

    void AribDecodecallUnit::SetAutoResetBeforeDecode(bool enable) noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_autoResetBeforeDecode = enable;
    }

    bool AribDecodecallUnit::GetAutoResetBeforeDecode() const noexcept
    {
        // const関数でもmutexは触りたいが、ここは読み取りのみで割り切り
        // （厳密にやるなら atomic<bool> にする）
        return m_autoResetBeforeDecode;
    }

    std::wstring AribDecodecallUnit::DecodeToWString(const uint8_t* data, std::size_t length)
    {
        std::wstring out;
        if (!DecodeInternal(data, length, out)) {
            return L"";
        }
        return out;
    }

    std::wstring AribDecodecallUnit::DecodeToWString(const std::vector<uint8_t>& bytes)
    {
        if (bytes.empty()) return L"";
        return DecodeToWString(bytes.data(), bytes.size());
    }

#if ARIB_DECODECALL_HAS_SPAN
    std::wstring AribDecodecallUnit::DecodeToWString(std::span<const uint8_t> bytes)
    {
        if (bytes.empty()) return L"";
        return DecodeToWString(bytes.data(), bytes.size());
    }
#endif

    std::wstring AribDecodecallUnit::DecodeToWStringForWin32(const uint8_t* data, std::size_t length,
        bool enableProbeLog)
    {
        std::wstring out;
        if (!DecodeInternal(data, length, out)) {
            return L"";
        }

        ReturnProcessFlags flags = ReturnProcessFlags::normalize_win32;
        if (enableProbeLog) flags = flags | ReturnProcessFlags::probe_log;

        ApplyReturnProcess(out, flags, L"DecodeToWStringForWin32");
        return out;
    }

    std::wstring AribDecodecallUnit::DecodeToWStringForWin32(const std::vector<uint8_t>& bytes,
        bool enableProbeLog)
    {
        if (bytes.empty()) return L"";
        return DecodeToWStringForWin32(bytes.data(), bytes.size(), enableProbeLog);
    }

#if ARIB_DECODECALL_HAS_SPAN
    std::wstring AribDecodecallUnit::DecodeToWStringForWin32(std::span<const uint8_t> bytes,
        bool enableProbeLog)
    {
        if (bytes.empty()) return L"";
        return DecodeToWStringForWin32(bytes.data(), bytes.size(), enableProbeLog);
    }
#endif

    std::wstring AribDecodecallUnit::DecodeConcatToWString(const std::vector<std::vector<uint8_t>>& chunks)
    {
        std::size_t total = 0;
        for (const auto& c : chunks) total += c.size();
        if (total == 0) return L"";

        std::vector<uint8_t> merged;
        merged.reserve(total);
        for (const auto& c : chunks) merged.insert(merged.end(), c.begin(), c.end());

        return DecodeToWString(merged);
    }

#if ARIB_DECODECALL_HAS_SPAN
    std::wstring AribDecodecallUnit::DecodeConcatToWString(std::span<const std::vector<uint8_t>> chunks)
    {
        std::size_t total = 0;
        for (const auto& c : chunks) total += c.size();
        if (total == 0) return L"";

        std::vector<uint8_t> merged;
        merged.reserve(total);
        for (const auto& c : chunks) merged.insert(merged.end(), c.begin(), c.end());

        return DecodeToWString(merged);
    }
#endif

    std::wstring AribDecodecallUnit::DecodeFileToWString(const std::wstring& filePath)
    {
        const std::filesystem::path p(filePath);
        std::ifstream ifs(p, std::ios::binary);
        if (!ifs) {
            ARIB_DECODECALL_FILE_ERROR_LOG(L"[ARIB][DecodeFile] open failed: %ls", filePath.c_str());
            return L"";
        }

        ifs.seekg(0, std::ios::end);
        const std::streampos endPos = ifs.tellg();
        if (endPos <= 0) return L"";
        ifs.seekg(0, std::ios::beg);

        const auto size = static_cast<std::size_t>(endPos);
        std::vector<uint8_t> buffer(size);

        if (!ifs.read(reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()))) {
            ARIB_DECODECALL_FILE_ERROR_LOG(L"[ARIB][DecodeFile] read failed: %ls", filePath.c_str());
            return L"";
        }

        return DecodeToWString(buffer);
    }

    std::wstring AribDecodecallUnit::DecodeFileToWStringForWin32(const std::wstring& filePath,
        bool enableProbeLog)
    {
        const std::wstring raw = DecodeFileToWString(filePath);
        if (raw.empty()) return raw;

        std::wstring out = raw;
        ReturnProcessFlags flags = ReturnProcessFlags::normalize_win32;
        if (enableProbeLog) flags = flags | ReturnProcessFlags::probe_log;

        ApplyReturnProcess(out, flags, L"DecodeFileToWStringForWin32");
        return out;
    }

    bool AribDecodecallUnit::DecodeInternal(const uint8_t* data, std::size_t length, std::wstring& out)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!data || length == 0) {
            out.clear();
            return true;
        }

        // ★ここがポイント：
        // - autoReset=true なら従来通り Reset
        // - autoReset=false なら状態保持しつつ「文字列開始だけ」整える
        if (m_autoResetBeforeDecode) {
            m_decoder.Reset();
        }
        else {
            m_decoder.BeginString();
        }

        std::wstring decoded;
        const bool ok = m_decoder.DecodeTextUTF16(data, length, decoded);
        out.swap(decoded);
        return ok;
    }

    void AribDecodecallUnit::EnableTraceForTest(bool enable) noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_decoder.SetGTrace(enable);
    }

    void AribDecodecallUnit::ResetDecoder() noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_decoder.Reset();
    }

    void AribDecodecallUnit::ApplyReturnProcess(std::wstring& s,
        ReturnProcessFlags flags,
        std::wstring_view tagForLog)
    {
        if (HasFlag(flags, ReturnProcessFlags::probe_log)) {
            const auto r = InspectReturn(s);
            if (r.hasEmbeddedNull || r.hasControlChar || r.hasBadSurrogate || (r.cstrLen < r.size)) {
                ARIB_DECODECALL_FILE_ERROR_LOG(
                    L"[ARIB][RETURN] %ls size=%zu cstrLen=%zu null=%d ctrl=%d surrogate=%d nullPos=%zu",
                    std::wstring(tagForLog).c_str(),
                    r.size, r.cstrLen,
                    r.hasEmbeddedNull ? 1 : 0,
                    r.hasControlChar ? 1 : 0,
                    r.hasBadSurrogate ? 1 : 0,
                    (r.firstNullPos == static_cast<size_t>(-1)) ? 0 : r.firstNullPos
                );
            }
        }

        if (HasFlag(flags, ReturnProcessFlags::normalize_win32)) {
            NormalizeForWin32(s, L' ');
        }
    }

    void AribDecodecallUnit::NormalizeForWin32(std::wstring& s, wchar_t nulReplace)
    {
        for (auto& ch : s) {
            if (ch == L'\0') {
                ch = nulReplace;
                continue;
            }
            if (ch < 0x20) {
                if (ch == L'\n' || ch == L'\r' || ch == L'\t') {
                    continue;
                }
                ch = L' ';
            }
        }
    }

    static bool IsHighSurrogate(wchar_t ch) noexcept { return (ch >= 0xD800 && ch <= 0xDBFF); }
    static bool IsLowSurrogate(wchar_t ch) noexcept { return (ch >= 0xDC00 && ch <= 0xDFFF); }

    ReturnProbeResult AribDecodecallUnit::InspectReturn(std::wstring_view s) noexcept
    {
        ReturnProbeResult r{};
        r.size = s.size();

        const size_t limit = s.size();
        size_t cLen = 0;
        for (; cLen < limit; ++cLen) {
            if (s[cLen] == L'\0') break;
        }
        r.cstrLen = cLen;

        const auto nullPos = s.find(L'\0');
        if (nullPos != std::wstring_view::npos) {
            r.hasEmbeddedNull = true;
            r.firstNullPos = nullPos;
        }

        for (wchar_t ch : s) {
            if (ch < 0x20) {
                if (ch != L'\n' && ch != L'\r' && ch != L'\t') {
                    r.hasControlChar = true;
                    break;
                }
            }
        }

        for (size_t i = 0; i < s.size(); ++i) {
            const wchar_t ch = s[i];
            if (IsHighSurrogate(ch)) {
                if (i + 1 >= s.size() || !IsLowSurrogate(s[i + 1])) {
                    r.hasBadSurrogate = true;
                    break;
                }
                ++i;
            }
            else if (IsLowSurrogate(ch)) {
                r.hasBadSurrogate = true;
                break;
            }
        }

        return r;
    }

    std::string AribDecodecallUnit::ToUtf8(std::wstring_view w)
    {
        if (w.empty()) return {};

        const int needed = ::WideCharToMultiByte(
            CP_UTF8, 0,
            w.data(), static_cast<int>(w.size()),
            nullptr, 0, nullptr, nullptr);

        if (needed <= 0) return {};

        std::string out(static_cast<size_t>(needed), '\0');
        ::WideCharToMultiByte(
            CP_UTF8, 0,
            w.data(), static_cast<int>(w.size()),
            out.data(), needed, nullptr, nullptr);

        return out;
    }

    bool AribDecodecallUnit::WriteUtf8File(const std::filesystem::path& path,
        std::wstring_view w,
        bool withBom)
    {
        std::ofstream ofs(path, std::ios::binary);
        if (!ofs) return false;

        if (withBom) {
            const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
            ofs.write(reinterpret_cast<const char*>(bom), 3);
        }

        const std::string u8 = ToUtf8(w);
        if (!u8.empty()) {
            ofs.write(u8.data(), static_cast<std::streamsize>(u8.size()));
        }
        return static_cast<bool>(ofs);
    }

} // namespace aribdecode
