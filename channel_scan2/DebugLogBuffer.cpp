#include "DebugLogBuffer.hpp"
#include <fstream>

DebugLogBuffer::DebugLogBuffer(size_t capacity) : capacity_(capacity) {}

void DebugLogBuffer::AddLog(const std::wstring& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.push_back(message);
    if (buffer_.size() > capacity_ * 0.6) {
        FlushToFile(L"debug_ring.log");
    }
}

void DebugLogBuffer::FlushToFile(const std::wstring& path) {
    std::wofstream ofs(path, std::ios::app);
    if (!ofs) return;

    for (const auto& msg : buffer_) {
        ofs << msg << L"\n";
    }
    buffer_.clear();
}

void DebugLogBuffer::ForceFlush() {
    FlushToFile(L"debug_ring.log");
}
