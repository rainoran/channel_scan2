#pragma once
#pragma once
#ifndef SYSTEM_LOG_MANAGER_HPP
#define SYSTEM_LOG_MANAGER_HPP

#include <string>
#include <string_view>
#include <mutex>
#include <fstream>
#include <chrono>

class SystemLogManager
{
public:
    static SystemLogManager& GetInstance() noexcept;

    // デストラクタに頼らない（明示的に閉じる）
    bool Initialize(std::wstring_view filePath) noexcept;
    void Shutdown() noexcept;

    void SetEnabled(bool enabled) noexcept { enabled_.store(enabled); }

    void Info(std::wstring_view msg) noexcept;
    void EventStart(std::wstring_view feature) noexcept;
    void EventEnd(std::wstring_view feature, std::wstring_view result) noexcept;

    void Flush() noexcept;

private:
    SystemLogManager() = default;
    SystemLogManager(const SystemLogManager&) = delete;
    SystemLogManager& operator=(const SystemLogManager&) = delete;

    void WriteLine(std::wstring_view level, std::wstring_view msg) noexcept;
    static std::wstring NowString() noexcept;

private:
    std::mutex mtx_;
    std::wofstream ofs_;
    std::wstring path_;
    std::atomic<bool> enabled_{ true };
    std::atomic<bool> initialized_{ false };
};

#endif // SYSTEM_LOG_MANAGER_HPP
