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

    // GUIの最初のsystem.log出力より前に一度だけ呼ぶ。
    // 既存ログが10MiB以上なら、連番oldログへ移動する。
    // true: 処理成功（退避不要を含む）、false: 退避失敗。
    bool PrepareLogForStartup() noexcept;

    // ログ出力を停止し、10MiB以上なら連番oldログへ移動する。
    // アプリのログ出力元／workerが全停止した後に一度だけ呼ぶ。
    // この後のSystemInfoLog*はファイルへ書き込まれない。
    bool ShutdownAndRotate() noexcept;

    const std::wstring& GetLogFilePath() const noexcept;

private:
    void LoadSettingsUnlocked();
    void WriteUtf8LineUnlocked(const std::wstring& line) const;
    void EnsureBomIfNeededUnlocked() const;
    bool RotateIfNeededUnlocked() const noexcept;

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