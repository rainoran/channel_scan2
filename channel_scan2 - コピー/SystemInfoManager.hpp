#pragma once
#pragma once
#ifndef SYSTEM_INFO_MANAGER_HPP
#define SYSTEM_INFO_MANAGER_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "configmanager.h"

#include <string>
#include <string_view>
#include <mutex>
#include <cstdarg>
#include <cstddef>
#include <cstdint>

class SystemInfoManager
{
public:
    explicit SystemInfoManager(ConfigManager& config);
    ~SystemInfoManager();

    SystemInfoManager(const SystemInfoManager&) = delete;
    SystemInfoManager& operator=(const SystemInfoManager&) = delete;

    static SystemInfoManager& GetInstance();

    void ConfigureSystemInfoMode();

    bool IsEnabled() const noexcept;
    bool IsConsoleOutputEnabled() const noexcept;

    void WriteLine(const std::wstring& line);
    void Flush();
    void Shutdown() noexcept;

    const std::wstring& GetLogFilePath() const noexcept;

private:
    void LoadSettingsUnlocked();
    void WriteUtf8LineUnlocked(const std::wstring& line);
    void EnsureBomIfNeededUnlocked();

private:
    ConfigManager& config_;
    mutable std::mutex mutex_;

    bool configured_ = false;
    bool shutdown_ = false;
    bool enabled_ = true;
    bool consoleOutputEnabled_ = false;

    std::wstring logFilePath_ = L"system.log";
};

void SystemInfoLog(const wchar_t* fmt, ...);
void SystemInfoLogInfo(const wchar_t* fmt, ...);
void SystemInfoLogWarn(const wchar_t* fmt, ...);
void SystemInfoLogError(const wchar_t* fmt, ...);
void FlushSystemInfoLog();

#endif // SYSTEM_INFO_MANAGER_HPP