#include "EpgHtmlPortal_Assets.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

#include "DebugManager.h"
#include "EpgHtmlPortal_Common.hpp"

namespace EpgHtmlPortal_Assets
{
    namespace
    {
        bool EnsureOutputParentDir_(const std::filesystem::path& outputPath)
        {
            const std::filesystem::path parent = outputPath.parent_path();
            if (parent.empty()) {
                return true;
            }

            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                DebugLogFormat(
                    L"[EPG-PORTAL][ASSET][ERROR] create directory failed: %ls",
                    parent.wstring().c_str());
                return false;
            }

            return true;
        }

        bool PathsEquivalentSafe_(
            const std::filesystem::path& lhs,
            const std::filesystem::path& rhs)
        {
            std::error_code ec;
            if (std::filesystem::exists(lhs, ec) && !ec &&
                std::filesystem::exists(rhs, ec) && !ec) {
                std::error_code ecEq;
                const bool eq = std::filesystem::equivalent(lhs, rhs, ecEq);
                if (!ecEq) {
                    return eq;
                }
            }
            return false;
        }

        void AddUniqueRoot_(
            std::vector<std::filesystem::path>& roots,
            const std::filesystem::path& p)
        {
            if (p.empty()) {
                return;
            }

            if (std::find(roots.begin(), roots.end(), p) == roots.end()) {
                roots.push_back(p);
            }
        }
    }

    std::vector<std::filesystem::path> BuildDefaultTemplateSearchRoots()
    {
        std::vector<std::filesystem::path> roots;

        const std::filesystem::path exeDir = EpgHtmlPortal_Common::GetExeDirectoryPath();
        if (!exeDir.empty()) {
            AddUniqueRoot_(roots, exeDir / L"templates" / L"EpgHtmlPortal");
            AddUniqueRoot_(roots, exeDir / L"templates");
            AddUniqueRoot_(roots, exeDir);
        }

        std::error_code ec;
        const std::filesystem::path cwd = std::filesystem::current_path(ec);
        if (!ec && !cwd.empty()) {
            AddUniqueRoot_(roots, cwd / L"templates" / L"EpgHtmlPortal");
            AddUniqueRoot_(roots, cwd / L"templates");
            AddUniqueRoot_(roots, cwd);
        }

        return roots;
    }

    std::filesystem::path FindTemplateAssetPath(
        const std::filesystem::path& relativePath,
        const std::vector<std::filesystem::path>& searchRoots)
    {
        if (relativePath.empty()) {
            return {};
        }

        if (relativePath.is_absolute()) {
            if (std::filesystem::exists(relativePath)) {
                return relativePath;
            }
            return {};
        }

        for (const auto& root : searchRoots) {
            if (root.empty()) {
                continue;
            }

            const std::filesystem::path candidate = root / relativePath;
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec) && !ec) {
                return candidate;
            }
        }

        return {};
    }

    bool CopyTemplateAssetToOutput(
        const std::filesystem::path& templatePath,
        const std::filesystem::path& outputPath,
        bool overwriteIfExists)
    {
        if (templatePath.empty() || outputPath.empty()) {
            DebugLog(L"[EPG-PORTAL][ASSET][ERROR] templatePath/outputPath is empty");
            return false;
        }

        std::error_code ecExists;
        if (!std::filesystem::exists(templatePath, ecExists) || ecExists) {
            DebugLogFormat(
                L"[EPG-PORTAL][ASSET][ERROR] template asset not found: %ls",
                templatePath.wstring().c_str());
            return false;
        }

        if (!EnsureOutputParentDir_(outputPath)) {
            return false;
        }

        if (PathsEquivalentSafe_(templatePath, outputPath)) {
            DebugLogFormat(
                L"[EPG-PORTAL][ASSET] skip copy (same file): %ls",
                outputPath.wstring().c_str());
            return true;
        }

        const auto copyOpt = overwriteIfExists
            ? std::filesystem::copy_options::overwrite_existing
            : std::filesystem::copy_options::skip_existing;

        std::error_code ecCopy;
        std::filesystem::copy_file(templatePath, outputPath, copyOpt, ecCopy);
        if (ecCopy) {
            DebugLogFormat(
                L"[EPG-PORTAL][ASSET][ERROR] copy failed: %ls -> %ls",
                templatePath.wstring().c_str(),
                outputPath.wstring().c_str());
            return false;
        }

        DebugLogFormat(
            L"[EPG-PORTAL][ASSET] copied: %ls -> %ls",
            templatePath.wstring().c_str(),
            outputPath.wstring().c_str());
        return true;
    }

    bool CopyNamedAssetToOutput(
        const std::filesystem::path& templateRelativePath,
        const std::filesystem::path& outputPath,
        const std::vector<std::filesystem::path>& searchRoots,
        bool overwriteIfExists)
    {
        const std::filesystem::path found = FindTemplateAssetPath(
            templateRelativePath,
            searchRoots);

        if (found.empty()) {
            DebugLogFormat(
                L"[EPG-PORTAL][ASSET][ERROR] asset not found in search roots: %ls",
                templateRelativePath.wstring().c_str());
            return false;
        }

        return CopyTemplateAssetToOutput(found, outputPath, overwriteIfExists);
    }

    bool EnsurePortalAssets(
        const EpgHtmlPortal::WriteOptions& opt,
        const std::vector<AssetFileEntry>& assets,
        const std::vector<std::filesystem::path>& searchRoots)
    {
        if (opt.outputDir.empty()) {
            DebugLog(L"[EPG-PORTAL][ASSET][ERROR] outputDir is empty");
            return false;
        }

        const std::vector<std::filesystem::path> roots =
            searchRoots.empty() ? BuildDefaultTemplateSearchRoots() : searchRoots;

        bool ok = true;

        for (const auto& asset : assets) {
            if (asset.templateRelativePath.empty() || asset.outputRelativePath.empty()) {
                DebugLog(L"[EPG-PORTAL][ASSET][WARN] skipped empty asset entry");
                if (asset.required) {
                    ok = false;
                }
                continue;
            }

            const std::filesystem::path outputPath = opt.outputDir / asset.outputRelativePath;
            const bool copied = CopyNamedAssetToOutput(
                asset.templateRelativePath,
                outputPath,
                roots,
                true);

            if (!copied) {
                if (asset.required) {
                    ok = false;
                }
                else {
                    DebugLogFormat(
                        L"[EPG-PORTAL][ASSET][WARN] optional asset missing: %ls",
                        asset.templateRelativePath.wstring().c_str());
                }
            }
        }

        return ok;
    }

    std::vector<AssetFileEntry> BuildDefaultPortalAssetList()
    {
        std::vector<AssetFileEntry> assets;
        assets.reserve(1);

        // runtime.js は静的 asset ではなく dynamic 側で生成する
        assets.push_back({
            std::filesystem::path(L"portal.css"),
            std::filesystem::path(L"portal.css"),
            true
            });

        return assets;
    }

    bool EnsureDefaultPortalAssets(const EpgHtmlPortal::WriteOptions& opt)
    {
        return EnsurePortalAssets(
            opt,
            BuildDefaultPortalAssetList(),
            BuildDefaultTemplateSearchRoots());
    }
}