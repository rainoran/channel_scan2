#pragma once
#ifndef DEBUG_MANAGER_H
#define DEBUG_MANAGER_H

#include "configmanager.h"
#include <string>
#include <string_view>
#include <cwchar>
#include <cstdarg>
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include "DebugMPMCLogBuffer.hpp"

#ifndef LIGHT_DEBUG_LOG
#define LIGHT_DEBUG_LOG(pid, label)                          \
    do {                                                     \
        static size_t _count_##label = 0;                    \
        if (++_count_##label % 1000 == 0) {                  \
            DebugLogFormat(L"[%-6s] PID=0x%04X", L#label, pid); \
        }                                                    \
    } while (0)
#endif

inline std::atomic_bool g_EnableSDTDebug{ false };
inline std::atomic_bool g_EnableNITDebug{ false };

#define SDT_DEBUG_LOG(fmt, ...) \
    do { \
        if (g_EnableSDTDebug.load(std::memory_order_relaxed)) { \
            DebugLog(fmt, __VA_ARGS__); \
        } \
    } while (0)

#define NIT_DEBUG_LOG(fmt, ...) \
    do { \
        if (g_EnableNITDebug.load(std::memory_order_relaxed)) { \
            DebugLog(fmt, __VA_ARGS__); \
        } \
    } while (0)

class DebugManager {
public:
    explicit DebugManager(ConfigManager& config);
    ~DebugManager();

    DebugManager(const DebugManager&) = delete;
    DebugManager& operator=(const DebugManager&) = delete;
    DebugManager(DebugManager&&) = delete;
    DebugManager& operator=(DebugManager&&) = delete;

    void ConfigureDebugMode();
    bool IsDebugEnabled() const;
    static DebugManager& GetInstance();

    void BufferedDebugLog(const std::wstring& message);
    void FlushBufferedLogs(bool force = false);

    void PushLog(const std::wstring& message);
    void Flush(bool force = false);

private:
    bool IsFileLogEnabled_() const;
    bool EnsureSinkStarted_();
    void StopSink_() noexcept;

private:
    ConfigManager& config;
    bool debugWriteToFile{ false };
    std::wstring startupLogFileName_;
    std::atomic_bool sinkStarted_{ false };

    DebugMPMCLogBuffer mpmcLogBuffer;
};

void DebugLog(const wchar_t* fmt, ...);
void DebugLogFormat(const wchar_t* format, ...);
void DebugLogUTF32(const std::u32string& u32str, const wchar_t* prefix = L"[UTF32]");

void CheckUTF8Convertible(const std::wstring& wstr, const wchar_t* context = L"[ENCODE CHECK]");
bool WriteSafe(std::wostream& os, const std::wstring& wstr, const wchar_t* context = L"[WRITE CHECK]");
void DebugLogToUTF16File(const std::wstring& message);
void FlushDebugLog();
void DebugLogFormatModern(std::wstring_view formatStr);

void DebugLogbeforedecode(std::wstring_view label, const uint8_t* data, std::size_t length, std::size_t previewBytes = 16) noexcept;
void DebugLogSafeString(const wchar_t* context, const std::wstring& s);

static std::wstring SanitizeForConsole(const std::wstring& src);

void DebugLogFormatSafe(const wchar_t* format, ...);

#endif // DEBUG_MANAGER_H