#pragma once

#include <deque>
#include <mutex>
#include <string>

class DebugLogBuffer {
public:
    explicit DebugLogBuffer(size_t capacity = 1000);
    void AddLog(const std::wstring& message);
    void FlushToFile(const std::wstring& path);
    void ForceFlush(); // 全部出力したい時用

private:
    size_t capacity_;
    std::deque<std::wstring> buffer_;
    std::mutex mutex_;
};
