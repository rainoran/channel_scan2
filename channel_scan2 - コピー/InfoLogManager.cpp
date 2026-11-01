#include "InfoLogManager.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>

InfoLogManager& InfoLogManager::GetInstance() {
    static InfoLogManager inst;
    return inst;
}

void InfoLogManager::Configure(bool enable, const std::wstring& filePath) {
    std::lock_guard lk(mtx_);
    enabled_ = enable;
    path_ = filePath;
}

std::wstring InfoLogManager::MakeTimestamp_() const {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[64]{};
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    return std::wstring(buf);
}

void InfoLogManager::WriteLineLocked_(const std::wstring& line) {
    if (!enabled_ || path_.empty()) return;

    // UTF-8で追記（WindowsのCCS指定）
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path_.c_str(), L"a, ccs=UTF-8") != 0 || !fp) return;

    // 1行追記
    fwprintf(fp, L"%s\n", line.c_str());
    fclose(fp);
}

void InfoLogManager::Log(const std::wstring& message) {
    std::lock_guard lk(mtx_);
    const std::wstring line = L"[INFO] " + MakeTimestamp_() + L" - " + message;
    WriteLineLocked_(line);
}

void InfoLogManager::Logf(const wchar_t* fmt, ...) {
    if (!fmt) return;

    wchar_t msg[1024]{};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    Log(std::wstring(msg));
}

void InfoLogManager::Flush() {
    // FILE追記方式なのでFlushは不要（必要なら将来ofstream保持方式にする）
}