#include "SystemInfoManager.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <fstream>
#include <filesystem>
#include <ctime>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>

namespace
{
    static std::wstring ResolveConfigIniPath() noexcept
    {
        try {
            wchar_t modulePath[MAX_PATH]{};
            const DWORD n = ::GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            if (n > 0 && n < MAX_PATH) {
                const std::filesystem::path exeDir = std::filesystem::path(modulePath).parent_path();
                const std::filesystem::path ini1 = exeDir / L"config.ini";
                if (std::filesystem::exists(ini1)) {
                    return ini1.wstring();
                }
            }

            const std::filesystem::path ini2 = std::filesystem::current_path() / L"config.ini";
            if (std::filesystem::exists(ini2)) {
                return ini2.wstring();
            }
        }
        catch (...) {
        }

        return L"config.ini";
    }

    static std::filesystem::path ResolveExeDir() noexcept
    {
        try {
            wchar_t modulePath[MAX_PATH]{};
            const DWORD n = ::GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            if (n > 0 && n < MAX_PATH) {
                return std::filesystem::path(modulePath).parent_path();
            }
        }
        catch (...) {
        }

        return std::filesystem::current_path();
    }

    static bool ReadIniBool(const wchar_t* section, const wchar_t* key, int defaultValue = 0) noexcept
    {
        const std::wstring iniPath = ResolveConfigIniPath();
        const UINT value = ::GetPrivateProfileIntW(
            section,
            key,
            static_cast<UINT>(defaultValue),
            iniPath.c_str());
        return value != 0;
    }

    static std::wstring ReadIniString(
        const wchar_t* section,
        const wchar_t* key,
        const wchar_t* defaultValue) noexcept
    {
        wchar_t buffer[1024]{};
        const std::wstring iniPath = ResolveConfigIniPath();

        ::GetPrivateProfileStringW(
            section,
            key,
            defaultValue,
            buffer,
            static_cast<DWORD>(std::size(buffer)),
            iniPath.c_str());

        return std::wstring(buffer);
    }

    static std::wstring NormalizeLogPath(const std::wstring& fileName) noexcept
    {
        try {
            std::filesystem::path p(fileName.empty() ? L"system.log" : fileName);
            if (p.is_absolute()) {
                return p.wstring();
            }
            return (ResolveExeDir() / p).wstring();
        }
        catch (...) {
            return L"system.log";
        }
    }

    static std::wstring BuildTimestamp() noexcept
    {
        wchar_t timestamp[64]{};

        std::time_t now = std::time(nullptr);
        std::tm localTm{};
        localtime_s(&localTm, &now);

        std::wcsftime(
            timestamp,
            static_cast<std::size_t>(std::size(timestamp)),
            L"%Y-%m-%d %H:%M:%S",
            &localTm);

        return std::wstring(timestamp);
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

    static std::string WStringToUTF8(const std::wstring& input)
    {
        if (input.empty()) {
            return {};
        }

        const int sizeNeeded = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            input.c_str(),
            static_cast<int>(input.size()),
            nullptr,
            0,
            nullptr,
            nullptr);

        if (sizeNeeded <= 0) {
            return {};
        }

        std::string output(static_cast<std::size_t>(sizeNeeded), '\0');

        const int written = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            input.c_str(),
            static_cast<int>(input.size()),
            output.data(),
            sizeNeeded,
            nullptr,
            nullptr);

        if (written <= 0) {
            return {};
        }

        return output;
    }

    static void OutputConsoleLine(const std::wstring& line) noexcept
    {
        HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut == nullptr || hOut == INVALID_HANDLE_VALUE) {
            return;
        }

        const std::wstring safe = SanitizeForConsole(line);

        DWORD writtenChars = 0;
        ::WriteConsoleW(
            hOut,
            safe.c_str(),
            static_cast<DWORD>(safe.size()),
            &writtenChars,
            nullptr);
    }

    static void WriteSystemInfoLogV(const wchar_t* level, const wchar_t* fmt, va_list args)
    {
        constexpr std::size_t kBufferSize = 2048;
        wchar_t messageBuffer[kBufferSize]{};

        const int written = _vsnwprintf_s(
            messageBuffer,
            kBufferSize,
            _TRUNCATE,
            fmt,
            args);

        if (written < 0) {
            ::OutputDebugStringW(L"[SystemInfoLog] formatting error\n");
            return;
        }

        std::wstring line;
        line.reserve(256);
        line += L"[SYSTEM][";
        line += level;
        line += L"] ";
        line += BuildTimestamp();
        line += L" - ";
        line += messageBuffer;

        ::OutputDebugStringW(line.c_str());
        ::OutputDebugStringW(L"\n");

        SystemInfoManager& mgr = SystemInfoManager::GetInstance();

        if (mgr.IsConsoleOutputEnabled()) {
            std::wstring consoleLine = line;
            consoleLine.push_back(L'\n');
            OutputConsoleLine(consoleLine);
        }

        if (mgr.IsEnabled()) {
            mgr.WriteLine(line);
        }
    }
}

SystemInfoManager::SystemInfoManager(ConfigManager& config)
    : config_(config)
{
}

SystemInfoManager::~SystemInfoManager()
{
    Shutdown();
}

SystemInfoManager& SystemInfoManager::GetInstance()
{
    static SystemInfoManager instance(ConfigManager::GetInstance());
    return instance;
}

void SystemInfoManager::ConfigureSystemInfoMode()
{
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = false;
    LoadSettingsUnlocked();
    configured_ = true;
}

void SystemInfoManager::LoadSettingsUnlocked()
{
    enabled_ = ReadIniBool(L"SystemLog", L"Enable", 1);
    consoleOutputEnabled_ = ReadIniBool(L"SystemLog", L"ConsoleOutput", 0);
    logFilePath_ = NormalizeLogPath(ReadIniString(L"SystemLog", L"FileName", L"system.log"));
}

bool SystemInfoManager::IsEnabled() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_) {
        const_cast<SystemInfoManager*>(this)->LoadSettingsUnlocked();
        const_cast<SystemInfoManager*>(this)->configured_ = true;
    }
    return !shutdown_ && enabled_;
}

bool SystemInfoManager::IsConsoleOutputEnabled() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_) {
        const_cast<SystemInfoManager*>(this)->LoadSettingsUnlocked();
        const_cast<SystemInfoManager*>(this)->configured_ = true;
    }
    return !shutdown_ && consoleOutputEnabled_;
}

const std::wstring& SystemInfoManager::GetLogFilePath() const noexcept
{
    return logFilePath_;
}

void SystemInfoManager::EnsureBomIfNeededUnlocked()
{
    try {
        const std::filesystem::path path(logFilePath_);

        bool needBom = true;
        if (std::filesystem::exists(path)) {
            const auto size = std::filesystem::file_size(path);
            needBom = (size == 0);
        }

        if (!needBom) {
            return;
        }

        std::ofstream ofs(path, std::ios::binary | std::ios::app);
        if (!ofs) {
            return;
        }

        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        ofs.write(reinterpret_cast<const char*>(bom), 3);
    }
    catch (...) {
    }
}

void SystemInfoManager::WriteUtf8LineUnlocked(const std::wstring& line)
{
    try {
        EnsureBomIfNeededUnlocked();

        std::ofstream ofs(logFilePath_, std::ios::binary | std::ios::app);
        if (!ofs) {
            return;
        }

        std::wstring normalized = line;
        if (normalized.empty() || (normalized.back() != L'\n' && normalized.back() != L'\r')) {
            normalized.push_back(L'\n');
        }

        const std::string utf8 = WStringToUTF8(normalized);
        if (!utf8.empty()) {
            ofs.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
            ofs.flush();
        }
    }
    catch (...) {
    }
}

void SystemInfoManager::WriteLine(const std::wstring& line)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!configured_) {
        LoadSettingsUnlocked();
        configured_ = true;
    }

    if (shutdown_ || !enabled_) {
        return;
    }

    WriteUtf8LineUnlocked(line);
}

void SystemInfoManager::Flush()
{
    // 直接書き込み方式なので、各出力時点で flush 済み。
    // インターフェース統一用に残す。
}

void SystemInfoManager::Shutdown() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = true;
}

void SystemInfoLog(const wchar_t* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    WriteSystemInfoLogV(L"INFO", fmt, args);
    va_end(args);
}

void SystemInfoLogInfo(const wchar_t* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    WriteSystemInfoLogV(L"INFO", fmt, args);
    va_end(args);
}

void SystemInfoLogWarn(const wchar_t* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    WriteSystemInfoLogV(L"WARN", fmt, args);
    va_end(args);
}

void SystemInfoLogError(const wchar_t* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    WriteSystemInfoLogV(L"ERROR", fmt, args);
    va_end(args);
}

void FlushSystemInfoLog()
{
    SystemInfoManager::GetInstance().Flush();
}