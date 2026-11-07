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
#include <system_error>
#include <limits>

namespace
{
    // 1ファイルの退避判定値。書き込み中には検査しない。
    constexpr std::uintmax_t kSystemLogRotateBytes = 10ULL * 1024ULL * 1024ULL;

    // Windows標準のファイル名比較（大文字・小文字を区別しない）。
    static bool EqualFileNamePart_(std::wstring_view a, std::wstring_view b) noexcept
    {
        return ::CompareStringOrdinal(
            a.data(), static_cast<int>(a.size()),
            b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    }

    // system_old_XXXX.logから連番を読む。欠番は使い回さない。
    static bool ParseArchiveIndex_(
        std::wstring_view fileName,
        std::wstring_view prefix,
        std::wstring_view suffix,
        std::uint64_t& outIndex) noexcept
    {
        outIndex = 0;
        if (fileName.size() <= prefix.size() + suffix.size()) {
            return false;
        }
        if (!EqualFileNamePart_(fileName.substr(0, prefix.size()), prefix)) {
            return false;
        }
        if (!EqualFileNamePart_(fileName.substr(fileName.size() - suffix.size()), suffix)) {
            return false;
        }

        const auto digits = fileName.substr(
            prefix.size(), fileName.size() - prefix.size() - suffix.size());
        if (digits.empty()) {
            return false;
        }

        std::uint64_t value = 0;
        for (const wchar_t c : digits) {
            if (c < L'0' || c > L'9') {
                return false;
            }
            const std::uint64_t digit = static_cast<std::uint64_t>(c - L'0');
            if (value > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10) {
                return false;
            }
            value = value * 10 + digit;
        }
        outIndex = value;
        return true;
    }

    static std::wstring FormatArchiveIndex_(std::uint64_t value)
    {
        std::wstring digits = std::to_wstring(value);
        if (digits.size() < 4) {
            digits.insert(0, 4 - digits.size(), L'0');
        }
        return digits;
    }

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

bool SystemInfoManager::PrepareLogForStartup() noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            // Shutdown後の再開は行わない。既存APIの再設定とは分離。
            return false;
        }
        // 呼び出し元はConfigManager初期化後、初回SystemInfoLog*より前。
        LoadSettingsUnlocked();
        configured_ = true;
        const bool ok = RotateIfNeededUnlocked();
        if (!ok) {
            ::OutputDebugStringW(L"[SystemInfoManager] startup log rotation failed; original log preserved\n");
        }
        return ok;
    }
    catch (...) {
        ::OutputDebugStringW(L"[SystemInfoManager] PrepareLogForStartup exception\n");
        return false;
    }
}

bool SystemInfoManager::ShutdownAndRotate() noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!configured_) {
            LoadSettingsUnlocked();
            configured_ = true;
        }
        // ファイル書き込みと同じmutexを取得した後に遮断。
        shutdown_ = true;
        // 現行のWriteUtf8LineUnlockedは都度closeするため開放済み。
        const bool ok = RotateIfNeededUnlocked();
        if (!ok) {
            ::OutputDebugStringW(L"[SystemInfoManager] shutdown log rotation failed; original log preserved\n");
        }
        return ok;
    }
    catch (...) {
        ::OutputDebugStringW(L"[SystemInfoManager] ShutdownAndRotate exception\n");
        return false;
    }
}

bool SystemInfoManager::RotateIfNeededUnlocked() const noexcept
{
    namespace fs = std::filesystem;

    try {
        const fs::path source(logFilePath_);
        if (source.empty()) {
            return false;
        }

        std::error_code ec;
        const bool exists = fs::exists(source, ec);
        if (ec) {
            return false;
        }
        if (!exists) {
            return true; // 未作成: 退避不要
        }
        if (!fs::is_regular_file(source, ec) || ec) {
            return false;
        }

        const std::uintmax_t bytes = fs::file_size(source, ec);
        if (ec) {
            return false;
        }
        if (bytes < kSystemLogRotateBytes) {
            return true; // 10MiB未満なら継続使用
        }

        fs::path directory = source.parent_path();
        if (directory.empty()) {
            directory = fs::current_path(ec);
            if (ec) {
                return false;
            }
        }

        const std::wstring stem = source.stem().wstring();
        const std::wstring extension = source.has_extension()
            ? source.extension().wstring() : L".log";
        const std::wstring prefix = stem + L"_old_";

        // 既存ファイルの最大番号+1から割り当てる。欠番を再利用しない。
        std::uint64_t maxIndex = 0;
        const fs::directory_iterator end;
        fs::directory_iterator it(directory, ec);
        if (ec) {
            return false;
        }
        for (; it != end; it.increment(ec)) {
            if (ec) {
                return false;
            }
            std::error_code kindEc;
            if (!it->is_regular_file(kindEc)) {
                if (kindEc) {
                    return false;
                }
                continue;
            }

            const std::wstring name = it->path().filename().wstring();
            std::uint64_t index = 0;
            if (ParseArchiveIndex_(name, prefix, extension, index)) {
                maxIndex = (std::max)(maxIndex, index);
            }
        }
        if (ec || maxIndex == (std::numeric_limits<std::uint64_t>::max)()) {
            return false;
        }

        // MoveFileWは既存宛先を上書きしない。
        // 同時に別プロセスが宛先を作成しても、衝突した場合は次の連番を試す。
        // C6292対策: 常に真になる比較式をforの継続条件に使わない。
        std::uint64_t index = maxIndex + 1ULL;
        for (;;) {
            const fs::path destination = directory /
                (prefix + FormatArchiveIndex_(index) + extension);

            ec.clear();
            const bool destinationExists = fs::exists(destination, ec);
            if (ec) {
                return false;
            }

            if (!destinationExists) {
                if (::MoveFileW(source.c_str(), destination.c_str()) != FALSE) {
                    return true;
                }

                const DWORD winErr = ::GetLastError();
                if (winErr != ERROR_ALREADY_EXISTS && winErr != ERROR_FILE_EXISTS) {
                    return false;
                }
            }

            // uint64_tのオーバーフローで0000番に戻らないようにする。
            if (index == (std::numeric_limits<std::uint64_t>::max)()) {
                return false;
            }
            ++index;
        }
    }
    catch (...) {
        return false;
    }
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

void SystemInfoManager::EnsureBomIfNeededUnlocked() const
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

void SystemInfoManager::WriteUtf8LineUnlocked(const std::wstring& line) const
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