#pragma once
#ifndef EPG_HTML_PORTAL_TEMPLATE_HPP
#define EPG_HTML_PORTAL_TEMPLATE_HPP
#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace EpgHtmlPortal_Template
{
    struct ReplaceEntry
    {
        std::wstring key;
        std::wstring value;
    };

    // テンプレート文字列内の key を value にすべて置換する
    bool ReplaceAll(
        std::wstring& text,
        std::wstring_view key,
        std::wstring_view value);

    // テンプレートファイルを UTF 系テキストとして読み込む
    bool LoadTemplateText(
        const std::filesystem::path& templatePath,
        std::wstring& outText);

    // テンプレート文字列へ複数置換を適用する
    bool RenderTemplateText(
        std::wstring_view templateText,
        std::span<const ReplaceEntry> replacements,
        std::wstring& outText);

    // テンプレートファイルを読み込み、複数置換を適用する
    bool RenderTemplateFile(
        const std::filesystem::path& templatePath,
        std::span<const ReplaceEntry> replacements,
        std::wstring& outText);

    // 置換後の文字列を UTF-8 BOM 付きで保存する
    bool SaveRenderedText(
        const std::filesystem::path& outputPath,
        std::wstring_view text);

    // テンプレート読込 → 置換 → 保存まで一括で行う
    bool RenderTemplateFileToOutput(
        const std::filesystem::path& templatePath,
        const std::filesystem::path& outputPath,
        std::span<const ReplaceEntry> replacements);

    // 簡易的なプレースホルダ形式 {{NAME}} を作る補助
    std::wstring MakePlaceholder(std::wstring_view name);
}

#endif // EPG_HTML_PORTAL_TEMPLATE_HPP