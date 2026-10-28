#include "SystemLogManager.hpp"
#include <iomanip>
#include <sstream>
#include <filesystem>

SystemLogManager& SystemLogManager::GetInstance() noexcept
{
    static SystemLogManager inst;
    return inst;
}

bool SystemLogManager::Initialize(std::wstring_view filePath) noexcept
{
    std::lock_guard<std::mutex> lock(mtx_);

    path_ = std::wstring(filePath);
    try {
        // 追記専用
        ofs_.open(path_, std::ios::out | std::ios::app);
        if (!ofs_.is_open()) {
            initialized_.store(false);
            return false;
        }
        // Windows の wofstream はロケール依存になりがちなので、必要なら
        // ConfigManager の設定に合わせて imbue を調整してください（utf8/sjis）
        initialized_.store(true);
        return true;
    }
    catch (...) {
        initialized_.store(false);
        return false;
    }
}

void SystemLogManager::Shutdown() noexcept
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (ofs_.is_open()) {
        ofs_.flush();
        ofs_.close();
    }
    initialized_.store(false);
}

void SystemLogManager::Flush() noexcept
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (ofs_.is_open()) ofs_.flush();
}

void SystemLogManager::Info(std::wstring_view msg) noexcept
{
    if (!enabled_.load() || !initialized_.load()) return;
    WriteLine(L"INFO", msg);
}

void SystemLogManager::EventStart(std::wstring_view feature) noexcept
{
    if (!enabled_.load() || !initialized_.load()) return;
    std::wstring line = L"START: ";
    line += feature;
    WriteLine(L"SYS", line);
}

void SystemLogManager::EventEnd(std::wstring_view feature, std::wstring_view result) noexcept
{
    if (!enabled_.load() || !initialized_.load()) return;
    std::wstring line = L"END: ";
    line += feature;
    line += L" result=";
    line += result;
    WriteLine(L"SYS", line);
}

void SystemLogManager::WriteLine(std::wstring_view level, std::wstring_view msg) noexcept
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (!ofs_.is_open()) return;

    ofs_ << NowString() << L" [" << level << L"] " << msg << L"\n";
}

std::wstring SystemLogManager::NowString() noexcept
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto tt = system_clock::to_time_t(now);

    tm tmv{};
    localtime_s(&tmv, &tt);

    std::wstringstream ss;
    ss << std::setfill(L'0')
        << (tmv.tm_year + 1900) << L"-"
        << std::setw(2) << (tmv.tm_mon + 1) << L"-"
        << std::setw(2) << tmv.tm_mday << L" "
        << std::setw(2) << tmv.tm_hour << L":"
        << std::setw(2) << tmv.tm_min << L":"
        << std::setw(2) << tmv.tm_sec;
    return ss.str();
}
