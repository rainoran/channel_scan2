#include "DebugManager.h"
#include <iostream>
#include <cstdarg>
#include <fstream>
#include <ctime>
#include <Windows.h>
#include <cwchar>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <string_view>
#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

    enum class HotLogKind : std::uint8_t {
        None = 0,
        AribStart,
        AribGState,
        MpmcPushSuccess,
        UnmatchedPid,
        EitNoDescriptor
    };

    struct ReentryGuard final {
        static thread_local bool s_inLog;
        bool entered{ false };

        ReentryGuard() noexcept {
            if (!s_inLog) {
                s_inLog = true;
                entered = true;
            }
        }

        ~ReentryGuard() {
            if (entered) {
                s_inLog = false;
            }
        }
    };

    thread_local bool ReentryGuard::s_inLog = false;

    static std::atomic_uint64_t g_aribStartSeen{ 0 };
    static std::atomic_uint64_t g_aribStartDropped{ 0 };
    static std::atomic_uint64_t g_aribGStateSeen{ 0 };
    static std::atomic_uint64_t g_aribGStateDropped{ 0 };
    static std::atomic_uint64_t g_mpmcPushSeen{ 0 };
    static std::atomic_uint64_t g_mpmcPushDropped{ 0 };
    static std::atomic_uint64_t g_unmatchedPidSeen{ 0 };
    static std::atomic_uint64_t g_unmatchedPidDropped{ 0 };
    static std::atomic_uint64_t g_eitNoDescSeen{ 0 };
    static std::atomic_uint64_t g_eitNoDescDropped{ 0 };

    static bool IsLogSinkEnabled() noexcept
    {
        return static_cast<bool>(ConfigManager::GetInstance().GetDebugWriteToFile());
    }

    static std::wstring NormalizeLogLine_(const std::wstring& src)
    {
        std::wstring out = src;
        if (out.empty() || (out.back() != L'\n' && out.back() != L'\r')) {
            out.push_back(L'\n');
        }
        return out;
    }

    static std::wstring BuildLogTimestamp_() noexcept
    {
        wchar_t timestamp[64]{};
        std::time_t now = std::time(nullptr);
        std::tm local_tm{};
        localtime_s(&local_tm, &now);
        std::wcsftime(timestamp, _countof(timestamp), L"%Y-%m-%d %H:%M:%S", &local_tm);
        return std::wstring(timestamp);
    }

    static std::wstring BuildStartupLogFileName_() noexcept
    {
        try {
            wchar_t stamp[64]{};
            std::time_t now = std::time(nullptr);
            std::tm local_tm{};
            localtime_s(&local_tm, &now);
            std::wcsftime(stamp, _countof(stamp), L"debug_%Y%m%d_%H%M%S.log", &local_tm);
            return std::wstring(stamp);
        }
        catch (...) {
            return L"debug_session.log";
        }
    }

    static HotLogKind DetectHotLogKind_(std::wstring_view s) noexcept
    {
        if (s.find(L"[ARIB][START]") != std::wstring_view::npos) {
            return HotLogKind::AribStart;
        }
        if (s.find(L"[ARIB][GState]") != std::wstring_view::npos) {
            return HotLogKind::AribGState;
        }
        if (s.find(L"[MPMC] Push成功") != std::wstring_view::npos) {
            return HotLogKind::MpmcPushSuccess;
        }
        if (s.find(L"[OTHER] Unmatched PID=") != std::wstring_view::npos) {
            return HotLogKind::UnmatchedPid;
        }
        if (s.find(L"[EIT] 注意: event_id") != std::wstring_view::npos &&
            s.find(L"descriptor がありません") != std::wstring_view::npos) {
            return HotLogKind::EitNoDescriptor;
        }
        return HotLogKind::None;
    }

    static bool ApplyHotLogSampling_(std::wstring& normalized) noexcept
    {
        const HotLogKind kind = DetectHotLogKind_(normalized);
        if (kind == HotLogKind::None) {
            return true;
        }

        std::atomic_uint64_t* seen = nullptr;
        std::atomic_uint64_t* dropped = nullptr;
        std::uint64_t sampleEvery = 1;
        const wchar_t* label = L"";

        switch (kind) {
        case HotLogKind::AribStart:
            seen = &g_aribStartSeen;
            dropped = &g_aribStartDropped;
            sampleEvery = 256;
            label = L"[ARIB][START]";
            break;

        case HotLogKind::AribGState:
            seen = &g_aribGStateSeen;
            dropped = &g_aribGStateDropped;
            sampleEvery = 128;
            label = L"[ARIB][GState]";
            break;

        case HotLogKind::MpmcPushSuccess:
            seen = &g_mpmcPushSeen;
            dropped = &g_mpmcPushDropped;
            sampleEvery = 1024;
            label = L"[MPMC] Push成功";
            break;

        case HotLogKind::UnmatchedPid:
            seen = &g_unmatchedPidSeen;
            dropped = &g_unmatchedPidDropped;
            sampleEvery = 256;
            label = L"[OTHER] Unmatched PID";
            break;

        case HotLogKind::EitNoDescriptor:
            seen = &g_eitNoDescSeen;
            dropped = &g_eitNoDescDropped;
            sampleEvery = 256;
            label = L"[EIT] descriptor missing";
            break;

        default:
            return true;
        }

        const std::uint64_t current = seen->fetch_add(1, std::memory_order_relaxed) + 1;
        if ((current % sampleEvery) != 0) {
            dropped->fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const std::uint64_t suppressed = dropped->exchange(0, std::memory_order_relaxed);
        if (suppressed > 0) {
            std::wostringstream oss;
            oss << L"[DEBUG] " << BuildLogTimestamp_()
                << L" - [DebugManager] suppressed "
                << static_cast<unsigned long long>(suppressed)
                << L" hot logs for " << label << L"\n";
            normalized = oss.str() + normalized;
        }

        return true;
    }

} // namespace

extern DebugMPMCLogBuffer g_DebugBuffer;

bool DebugManager::IsFileLogEnabled_() const
{
    return debugWriteToFile || static_cast<bool>(ConfigManager::GetInstance().GetDebugWriteToFile());
}

bool DebugManager::EnsureSinkStarted_()
{
    if (!IsFileLogEnabled_()) {
        return false;
    }

    bool expected = false;
    if (sinkStarted_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        mpmcLogBuffer.SetIdleWait(std::chrono::milliseconds(10));
        mpmcLogBuffer.PrepareOutput();
        mpmcLogBuffer.Start();
    }

    return true;
}

void DebugManager::StopSink_() noexcept
{
    bool expected = true;
    if (sinkStarted_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        mpmcLogBuffer.Flush();
        mpmcLogBuffer.Stop();
    }
}

DebugManager::DebugManager(ConfigManager& cfg)
    : config(cfg)
    , debugWriteToFile(false)
    , startupLogFileName_(BuildStartupLogFileName_())
    , sinkStarted_(false)
    , mpmcLogBuffer(4096, startupLogFileName_)
{
    debugWriteToFile = config.GetDebugWriteToFile();
    g_EnableSDTDebug.store(config.GetEnableSDTDebug(), std::memory_order_relaxed);
    g_EnableNITDebug.store(config.GetEnableNITDebug(), std::memory_order_relaxed);
    mpmcLogBuffer.SetIdleWait(std::chrono::milliseconds(10));
}

DebugManager::~DebugManager()
{
    ::OutputDebugStringW(L"[DebugManager::~DebugManager] begin\n");
    StopSink_();
    ::OutputDebugStringW(L"[DebugManager::~DebugManager] end\n");
}

void DebugManager::ConfigureDebugMode()
{
    debugWriteToFile = config.GetDebugWriteToFile();
    g_EnableSDTDebug.store(config.GetEnableSDTDebug(), std::memory_order_relaxed);
    g_EnableNITDebug.store(config.GetEnableNITDebug(), std::memory_order_relaxed);

    mpmcLogBuffer.SetIdleWait(std::chrono::milliseconds(10));

    if (!IsFileLogEnabled_()) {
        StopSink_();
    }
}

bool DebugManager::IsDebugEnabled() const
{
    return debugWriteToFile;
}

DebugManager& DebugManager::GetInstance()
{
    static DebugManager instance(ConfigManager::GetInstance());
    return instance;
}

void DebugManager::PushLog(const std::wstring& message)
{
    if (!IsFileLogEnabled_()) {
        return;
    }

    if (!EnsureSinkStarted_()) {
        return;
    }

    const std::wstring normalized = NormalizeLogLine_(message);

    if (!mpmcLogBuffer.PushLog(normalized)) {
        ::OutputDebugStringW(L"[DebugManager] log queue full: dropped\n");
    }
}

void DebugManager::BufferedDebugLog(const std::wstring& message)
{
    if (!IsFileLogEnabled_()) {
        return;
    }

    PushLog(message);
}

void DebugManager::FlushBufferedLogs(bool /*force*/)
{
    if (!IsFileLogEnabled_()) {
        return;
    }

    if (!sinkStarted_.load(std::memory_order_acquire)) {
        return;
    }

    mpmcLogBuffer.Flush();
}

void DebugManager::Flush(bool /*force*/)
{
    if (!IsFileLogEnabled_()) {
        return;
    }

    if (!sinkStarted_.load(std::memory_order_acquire)) {
        return;
    }

    mpmcLogBuffer.Flush();
}

std::string WStringToUTF8(const std::wstring& input)
{
    const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 1) {
        std::wcerr << L"[WStringToUTF8 Error] Failed to compute sizeNeeded.\n";
        return "";
    }

    std::string output(static_cast<std::size_t>(sizeNeeded - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, output.data(), sizeNeeded, nullptr, nullptr);
    return output;
}

static std::wstring SanitizeForConsole(const std::wstring& src)
{
    std::wstring out;
    out.reserve(src.size());

    for (wchar_t wc : src) {
        if (wc >= 0xD800 && wc <= 0xDFFF) {
            out.push_back(L'?');
        }
        else {
            out.push_back(wc);
        }
    }
    return out;
}

void DebugLog(const wchar_t* fmt, ...)
{
    ReentryGuard guard;
    if (!guard.entered) {
        ::OutputDebugStringW(L"[DebugLog] recursive logging suppressed\n");
        return;
    }

    constexpr size_t BUFFER_SIZE = 2048;
    wchar_t buffer[BUFFER_SIZE]{};

    va_list args;
    va_start(args, fmt);
    const int written = _vsnwprintf_s(buffer, BUFFER_SIZE, _TRUNCATE, fmt, args);
    va_end(args);

    if (written < 0) {
        OutputDebugStringW(L"[DebugLog] formatting error\n");
        return;
    }

    wchar_t timestamp[64]{};
    std::time_t now = std::time(nullptr);
    std::tm local_tm{};
    localtime_s(&local_tm, &now);
    std::wcsftime(timestamp, _countof(timestamp), L"%Y-%m-%d %H:%M:%S", &local_tm);

    std::wstring base = L"[DEBUG] ";
    base += timestamp;
    base += L" - ";
    base += buffer;

    if (!ApplyHotLogSampling_(base)) {
        return;
    }

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != nullptr && hOut != INVALID_HANDLE_VALUE) {
        std::wstring line = SanitizeForConsole(base);
        line.push_back(L'\n');

        DWORD writtenChars = 0;
        const BOOL ok = WriteConsoleW(hOut, line.c_str(), static_cast<DWORD>(line.size()), &writtenChars, nullptr);
        if (!ok) {
            OutputDebugStringW(L"[DebugLog] WriteConsoleW failed\n");
        }
    }
    else {
        OutputDebugStringW(base.c_str());
        OutputDebugStringW(L"\n");
    }

    if (IsLogSinkEnabled()) {
        DebugManager::GetInstance().PushLog(base);
    }
}

void DebugLogFormat(const wchar_t* format, ...)
{
    constexpr size_t BUFFER_SIZE = 1024;
    wchar_t buffer[BUFFER_SIZE]{};

    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, BUFFER_SIZE, _TRUNCATE, format, args);
    va_end(args);

    DebugLog(L"%ls", buffer);
}

void DebugLogFormatModern(std::wstring_view formatStr)
{
    OutputDebugStringW(formatStr.data());
    OutputDebugStringW(L"\n");
}

void DebugLogToUTF16File(const std::wstring& message)
{
    std::wofstream ofs(L"debug_utf16.log", std::ios::app);
    if (!ofs) {
        return;
    }
    ofs << message << std::endl;
}

void FlushDebugLog()
{
    DebugManager::GetInstance().Flush(true);
}

void DebugLogUTF32(const std::u32string& u32str, const wchar_t* prefix)
{
    std::wstringstream ss;
    ss << prefix << L" UTF-32 codepoints: ";

    for (char32_t cp : u32str) {
        ss << L"U+"
            << std::hex << std::uppercase << std::setw(4) << std::setfill(L'0')
            << static_cast<unsigned int>(cp) << L' ';
    }

    DebugLog(L"%ls", ss.str().c_str());
}

void DebugLogSafeString(const wchar_t* context, const std::wstring& s)
{
    DebugLog(L"%ls %ls", context, s.c_str());
}

void CheckUTF8Convertible(const std::wstring& wstr, const wchar_t* context)
{
    (void)context;
    (void)wstr;
}

bool WriteSafe(std::wostream& os, const std::wstring& wstr, const wchar_t* context)
{
    (void)context;
    os << wstr;
    return true;
}

void DebugLogbeforedecode(std::wstring_view label,
    const uint8_t* data,
    std::size_t length,
    std::size_t previewBytes) noexcept
{
    if (previewBytes == 0) {
        previewBytes = 16;
    }

    std::wostringstream oss;
    oss << L"[PRE-DECODE] " << label << L" len=" << static_cast<unsigned long long>(length);

    if (!data || length == 0) {
        oss << L" head=(empty)";
        DebugLog(L"%ls", oss.str().c_str());
        return;
    }

    const std::size_t n = (std::min)(length, previewBytes);

    oss << L" head=";
    oss << std::uppercase << std::hex << std::setfill(L'0');
    for (std::size_t i = 0; i < n; ++i) {
        oss << std::setw(2) << static_cast<unsigned>(data[i]);
        if (i + 1 < n) {
            oss << L' ';
        }
    }
    if (n < length) {
        oss << L" …";
    }

    auto to_printable = [](uint8_t b) -> wchar_t {
        return (b >= 0x20 && b <= 0x7E) ? static_cast<wchar_t>(b) : L'.';
        };

    oss << L"  ascii=\"";
    for (std::size_t i = 0; i < n; ++i) {
        oss << to_printable(data[i]);
    }
    if (n < length) {
        oss << L"…";
    }
    oss << L"\"";

    DebugLog(L"%ls", oss.str().c_str());
}

void DebugLogFormatSafe(const wchar_t* format, ...)
{
    ReentryGuard guard;
    if (!guard.entered) {
        ::OutputDebugStringW(L"[DebugLogFormatSafe] recursive logging suppressed\n");
        return;
    }

    try {
        constexpr size_t BUFFER_SIZE = 1024;
        wchar_t buffer[BUFFER_SIZE]{};

        va_list args;
        va_start(args, format);
        const int len = vswprintf_s(buffer, BUFFER_SIZE, format, args);
        va_end(args);

        if (len > 0) {
            std::wstring finalMessage = buffer;

            if (!ApplyHotLogSampling_(finalMessage)) {
                return;
            }

            finalMessage = SanitizeForConsole(finalMessage);

            OutputDebugStringW(finalMessage.c_str());
            OutputDebugStringW(L"\n");

            if (IsLogSinkEnabled()) {
                DebugManager::GetInstance().PushLog(finalMessage);
            }
        }
        else {
            OutputDebugStringW(L"[DebugLogFormatSafe] vswprintf_s 失敗\n");
        }
    }
    catch (...) {
        OutputDebugStringW(L"[DebugLogFormatSafe] 例外が発生しました\n");
    }
}