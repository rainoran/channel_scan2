#include "EpgHtmlPortal_Template.hpp"

#include <utility>

#include "DebugManager.h"
#include "EpgHtmlPortal_Common.hpp"

namespace EpgHtmlPortal_Template
{
    namespace
    {
        void LogTemplateLoadFailed_(const std::filesystem::path& path)
        {
            DebugLogFormat(
                L"[EPG-PORTAL][TEMPLATE][ERROR] template load failed: %ls",
                path.wstring().c_str());
        }

        void LogTemplateRenderFailed_(const std::wstring& key)
        {
            DebugLogFormat(
                L"[EPG-PORTAL][TEMPLATE][ERROR] replace failed: %ls",
                key.c_str());
        }

        void LogTemplateSaveFailed_(const std::filesystem::path& path)
        {
            DebugLogFormat(
                L"[EPG-PORTAL][TEMPLATE][ERROR] rendered output save failed: %ls",
                path.wstring().c_str());
        }
    }

    bool ReplaceAll(
        std::wstring& text,
        std::wstring_view key,
        std::wstring_view value)
    {
        if (key.empty()) {
            return false;
        }

        bool replaced = false;
        std::size_t pos = 0;

        while ((pos = text.find(key, pos)) != std::wstring::npos) {
            text.replace(pos, key.size(), value.data(), value.size());
            pos += value.size();
            replaced = true;
        }

        return replaced;
    }

    bool LoadTemplateText(
        const std::filesystem::path& templatePath,
        std::wstring& outText)
    {
        outText.clear();

        if (!EpgHtmlPortal_Common::ReadTextFileToWide(templatePath, outText)) {
            LogTemplateLoadFailed_(templatePath);
            return false;
        }

        return true;
    }

    bool RenderTemplateText(
        std::wstring_view templateText,
        std::span<const ReplaceEntry> replacements,
        std::wstring& outText)
    {
        try {
            outText.assign(templateText.data(), templateText.size());

            for (const auto& entry : replacements) {
                if (entry.key.empty()) {
                    continue;
                }

                if (!ReplaceAll(outText, entry.key, entry.value)) {
                    // プレースホルダ未使用は失敗扱いにしない
                    DebugLogFormat(
                        L"[EPG-PORTAL][TEMPLATE] placeholder not found: %ls",
                        entry.key.c_str());
                }
            }

            return true;
        }
        catch (...) {
            DebugLog(L"[EPG-PORTAL][TEMPLATE][ERROR] exception in RenderTemplateText");
            return false;
        }
    }

    bool RenderTemplateFile(
        const std::filesystem::path& templatePath,
        std::span<const ReplaceEntry> replacements,
        std::wstring& outText)
    {
        std::wstring templateText;
        if (!LoadTemplateText(templatePath, templateText)) {
            return false;
        }

        if (!RenderTemplateText(templateText, replacements, outText)) {
            DebugLogFormat(
                L"[EPG-PORTAL][TEMPLATE][ERROR] render failed: %ls",
                templatePath.wstring().c_str());
            return false;
        }

        return true;
    }

    bool SaveRenderedText(
        const std::filesystem::path& outputPath,
        std::wstring_view text)
    {
        if (!EpgHtmlPortal_Common::WriteUtf8BomTextFile(outputPath, text)) {
            LogTemplateSaveFailed_(outputPath);
            return false;
        }
        return true;
    }

    bool RenderTemplateFileToOutput(
        const std::filesystem::path& templatePath,
        const std::filesystem::path& outputPath,
        std::span<const ReplaceEntry> replacements)
    {
        std::wstring rendered;
        if (!RenderTemplateFile(templatePath, replacements, rendered)) {
            return false;
        }

        if (!SaveRenderedText(outputPath, rendered)) {
            return false;
        }

        DebugLogFormat(
            L"[EPG-PORTAL][TEMPLATE] rendered: %ls -> %ls",
            templatePath.wstring().c_str(),
            outputPath.wstring().c_str());

        return true;
    }

    std::wstring MakePlaceholder(std::wstring_view name)
    {
        std::wstring out;
        out.reserve(name.size() + 4);
        out += L"{{";
        out.append(name.data(), name.size());
        out += L"}}";
        return out;
    }
}