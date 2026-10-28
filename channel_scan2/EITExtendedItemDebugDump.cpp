#include "EITExtendedItemDebugDump.hpp"

#if EIT_EXTITEM_DUMP_ENABLE

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <chrono>
#include <ctime>
#include <string_view>

namespace eitdebug
{
    namespace
    {
        std::mutex g_mutex;
        FILE* g_fp = nullptr;

        constexpr const wchar_t* kFileName = L"EIT_ExtendedItemsDump.log";

        void OpenExtendedItemDumpLocked()
        {
            if (g_fp != nullptr) {
                return;
            }

            FILE* fp = nullptr;
            const errno_t err = _wfopen_s(&fp, kFileName, L"a, ccs=UTF-8");
            if (err != 0 || fp == nullptr) {
                return;
            }

            g_fp = fp;

            const auto now = std::chrono::system_clock::now();
            const std::time_t tt = std::chrono::system_clock::to_time_t(now);

            std::tm lt{};
            localtime_s(&lt, &tt);

            std::fwprintf(
                g_fp,
                L"\n===== EIT ExtendedItems Dump START %04d-%02d-%02d %02d:%02d:%02d =====\n",
                lt.tm_year + 1900,
                lt.tm_mon + 1,
                lt.tm_mday,
                lt.tm_hour,
                lt.tm_min,
                lt.tm_sec);

            std::fflush(g_fp);
        }

        void HexDumpLine(
            const std::uint8_t* bytes,
            std::size_t offset,
            std::size_t count)
        {
            if (g_fp == nullptr || bytes == nullptr) {
                return;
            }

            std::fwprintf(g_fp, L"  %04zu : ", offset);

            for (std::size_t i = 0; i < count; ++i) {
                std::fwprintf(
                    g_fp,
                    L"%02X ",
                    static_cast<unsigned>(bytes[offset + i]));
            }

            for (std::size_t i = count; i < 16; ++i) {
                std::fwprintf(g_fp, L"   ");
            }

            std::fwprintf(g_fp, L" | ");

            for (std::size_t i = 0; i < count; ++i) {
                const std::uint8_t b = bytes[offset + i];
                const wchar_t w =
                    (b >= 0x20 && b < 0x7F)
                    ? static_cast<wchar_t>(b)
                    : L'.';

                std::fwprintf(g_fp, L"%lc", w);
            }

            std::fwprintf(g_fp, L"\n");
        }
    }

    void DumpExtendedItemRaw(
        std::uint16_t serviceId,
        std::uint16_t eventId,
        std::wstring_view label,
        const std::uint8_t* bytes,
        std::size_t length)
    {
        if (bytes == nullptr || length == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(g_mutex);

        if (g_fp == nullptr) {
            OpenExtendedItemDumpLocked();
            if (g_fp == nullptr) {
                return;
            }
        }

        std::fwprintf(
            g_fp,
            L"[EXTITEM] sid=0x%04X eid=0x%04X len=%zu label=\"%.*ls\"\n",
            static_cast<unsigned>(serviceId),
            static_cast<unsigned>(eventId),
            length,
            static_cast<int>(label.size()),
            label.data());

        constexpr std::size_t kMaxDumpBytes = 512;
        const std::size_t dumpLen =
            (length > kMaxDumpBytes) ? kMaxDumpBytes : length;

        for (std::size_t offset = 0; offset < dumpLen; offset += 16) {
            const std::size_t chunk =
                (offset + 16 <= dumpLen)
                ? 16
                : (dumpLen - offset);

            HexDumpLine(bytes, offset, chunk);
        }

        if (length > dumpLen) {
            std::fwprintf(
                g_fp,
                L"  ...(truncated, total=%zu)\n",
                length);
        }

        std::fflush(g_fp);
    }

    void CloseExtendedItemDump()
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (g_fp != nullptr) {
            std::fwprintf(g_fp, L"===== EIT ExtendedItems Dump END =====\n");
            std::fclose(g_fp);
            g_fp = nullptr;
        }
    }
}

#endif // EIT_EXTITEM_DUMP_ENABLE