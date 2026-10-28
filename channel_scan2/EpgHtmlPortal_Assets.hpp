#pragma once
#ifndef EPG_HTML_PORTAL_ASSETS_HPP
#define EPG_HTML_PORTAL_ASSETS_HPP
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "EpgHtmlPortal.hpp"

namespace EpgHtmlPortal_Assets
{
    struct AssetFileEntry
    {
        std::filesystem::path templateRelativePath;
        std::filesystem::path outputRelativePath;
        bool required = true;
    };

    // 既定テンプレート探索ルート:
    // 1. <exeDir>\templates\EpgHtmlPortal
    // 2. <exeDir>\templates
    // 3. <exeDir>
    std::vector<std::filesystem::path> BuildDefaultTemplateSearchRoots();

    // 指定相対パスのテンプレートファイルを探索
    std::filesystem::path FindTemplateAssetPath(
        const std::filesystem::path& relativePath,
        const std::vector<std::filesystem::path>& searchRoots);

    // 単一 asset をコピー
    bool CopyTemplateAssetToOutput(
        const std::filesystem::path& templatePath,
        const std::filesystem::path& outputPath,
        bool overwriteIfExists = true);

    // 相対指定の asset を探索してコピー
    bool CopyNamedAssetToOutput(
        const std::filesystem::path& templateRelativePath,
        const std::filesystem::path& outputPath,
        const std::vector<std::filesystem::path>& searchRoots,
        bool overwriteIfExists = true);

    // runtime 用アセット群をまとめて配置
    bool EnsurePortalAssets(
        const EpgHtmlPortal::WriteOptions& opt,
        const std::vector<AssetFileEntry>& assets,
        const std::vector<std::filesystem::path>& searchRoots = {});

    // 既定構成の asset 一覧
    std::vector<AssetFileEntry> BuildDefaultPortalAssetList();

    // 既定構成で portal asset を配置
    bool EnsureDefaultPortalAssets(const EpgHtmlPortal::WriteOptions& opt);
}

#endif // EPG_HTML_PORTAL_ASSETS_HPP