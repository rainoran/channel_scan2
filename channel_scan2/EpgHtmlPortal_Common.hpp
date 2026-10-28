#pragma once
#ifndef EPG_HTML_PORTAL_COMMON_HPP
#define EPG_HTML_PORTAL_COMMON_HPP
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <algorithm>
#include <vector>

namespace EpgHtmlPortal_Common
{
    std::wstring TrimW(std::wstring s);
    std::wstring ToLowerAsciiW(std::wstring s);

    std::string WideToUtf8(std::wstring_view ws);
    std::wstring EscapeHtmlW(std::wstring_view s);

    std::wstring ToJsBool(bool v);
    // UTF-8バイト列を wchar_t へ1バイト拡張してしまった文字化けを復元する。
    // 例: L"ãã¬ç1" -> L"テレ玉1"
    std::wstring RepairUtf8ByteWidenedMojibake(std::wstring_view s);
    std::wstring ToJsStringLiteral(std::wstring_view s);

    bool ReadAllBytes(const std::filesystem::path& path, std::string& outBytes);
    bool ReadTextFileToWide(const std::filesystem::path& path, std::wstring& outText);

    bool WriteUtf8BomTextFile(const std::filesystem::path& path, std::wstring_view body);

    bool TryDecodeMultiByte(unsigned int codePage, const std::string& bytes, std::wstring& out);
    std::wstring DecodeUtf16LeBytes(const std::string& bytes, std::size_t offset = 0);
    std::wstring DecodeUtf16BeBytes(const std::string& bytes, std::size_t offset = 0);
    std::wstring DecodeTextBytesToWide(const std::string& bytes);

    std::filesystem::path GetExeDirectoryPath();
    std::filesystem::path ResolveAuxiliaryPath(
        const std::filesystem::path& baseDir,
        const std::filesystem::path& filePath);

    bool EnsureParentDirectory(const std::filesystem::path& path);
}

#endif // EPG_HTML_PORTAL_COMMON_HPP