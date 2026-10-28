#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "EpgHtmlLite.hpp"
#include "EpgHtmlLite_SplitGuide.hpp"
#include "DebugManager.h"

#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <new>
#include <system_error>

namespace EpgHtmlLite
{
    bool Generate(const Context& ctx, Result* outResult)
    {
        Result result{};

        const auto diagStart =
            std::chrono::steady_clock::now();

        const std::size_t entryCount =
            ctx.sharedData != nullptr
            ? ctx.sharedData->entries.size()
            : 0;

        const std::wstring outputDirText =
            ctx.outputDir.wstring();

        DebugLogFormat(
            L"[EPG][DIAG][LITE][GENERATE-BEGIN] space=%d entries=%zu futureDays=%d pastKeepDays=%d outputDir=%ls merged=%ls chlist=%ls",
            ctx.space,
            entryCount,
            ctx.futureDays,
            ctx.pastKeepDays,
            outputDirText.c_str(),
            ctx.mergedEpgPath.c_str(),
            ctx.channelListPath.c_str());

        try {
            if (ctx.outputDir.empty()) {
                DebugLog(L"[EPG][DIAG][LITE][GENERATE-END] ok=0 reason=empty-outputDir");

                if (outResult) {
                    *outResult = result;
                }
                return false;
            }

            // Lite生成中に取得側の所有物を借用し続けない。
            // entries内はwstringを含むため、ここで完全な独立コピーになる。
            SharedData detachedData{};
            if (ctx.sharedData != nullptr) {
                detachedData = *ctx.sharedData;
            }

            // SplitGuide以下にはローカルの独立データだけを渡す。
            Context detachedCtx = ctx;
            detachedCtx.sharedData = &detachedData;
            detachedCtx.releaseInputResources = {};

            // コピー後なら、呼び出し元はBonDriver等を安全に手動解放できる。
            // HTML生成より先に必ず実行する。
            if (ctx.releaseInputResources) {
                bool released = false;
                try {
                    released = ctx.releaseInputResources();
                }
                catch (...) {
                    DebugLog(
                        L"[EPG][DIAG][LITE][GENERATE-END] ok=0 reason=input-release-exception");

                    if (outResult) {
                        *outResult = result;
                    }
                    return false;
                }

                if (!released) {
                    DebugLog(
                        L"[EPG][DIAG][LITE][GENERATE-END] ok=0 reason=input-release-failed");

                    if (outResult) {
                        *outResult = result;
                    }
                    return false;
                }

                DebugLog(
                    L"[EPG][DIAG][LITE][INPUT-RESOURCES-RELEASED]");
            }

            std::error_code ec;
            std::filesystem::create_directories(detachedCtx.outputDir, ec);
            if (ec) {
                DebugLogFormat(
                    L"[EPG][DIAG][LITE][GENERATE-END] ok=0 reason=create-dir-failed ec=%d",
                    ec.value());

                if (outResult) {
                    *outResult = result;
                }
                return false;
            }

            DebugLog(L"[EPG][DIAG][LITE][SPLIT-BEGIN]");

            if (!SplitGuide::Generate(detachedCtx, result)) {
                const auto elapsedMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - diagStart).count();

                DebugLogFormat(
                    L"[EPG][DIAG][LITE][GENERATE-END] ok=0 reason=split-failed elapsedMs=%lld files=%zu",
                    static_cast<long long>(elapsedMs),
                    result.generatedFiles.size());

                if (outResult) {
                    *outResult = result;
                }
                return false;
            }

            DebugLogFormat(
                L"[EPG][DIAG][LITE][SPLIT-END] files=%zu index=%ls",
                result.generatedFiles.size(),
                result.indexPath.c_str());

            result.success = true;

            if (outResult) {
                *outResult = result;
            }

            const auto elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - diagStart).count();

            DebugLogFormat(
                L"[EPG][DIAG][LITE][GENERATE-END] ok=1 elapsedMs=%lld entries=%zu files=%zu index=%ls",
                static_cast<long long>(elapsedMs),
                entryCount,
                result.generatedFiles.size(),
                result.indexPath.c_str());

            return true;
        }
        catch (const std::bad_alloc&) {
            DebugLog(L"[EPG][DIAG][LITE][GENERATE-END] ok=0 exception=bad_alloc");
        }
        catch (const std::exception& e) {
            DebugLogFormat(
                L"[EPG][DIAG][LITE][GENERATE-END] ok=0 exception=std what=%S",
                e.what());
        }
        catch (...) {
            DebugLog(L"[EPG][DIAG][LITE][GENERATE-END] ok=0 exception=unknown");
        }

        if (outResult) {
            *outResult = result;
        }
        return false;
    }
}
