#pragma once
#pragma once
#ifndef INFO_LOG_MANAGER_H
#define INFO_LOG_MANAGER_H

#include <Windows.h>
#include <string>
#include <mutex>

class InfoLogManager {
public:
    static InfoLogManager& GetInstance();

    // 設定：有効/無効、出力パスなど
    void Configure(bool enable, const std::wstring& filePath);

    // 書き込み（printf風）
    void Logf(const wchar_t* fmt, ...);

    // 文字列直
    void Log(const std::wstring& message);

    // 明示フラッシュ（任意）
    void Flush();

private:
    InfoLogManager() = default;
    ~InfoLogManager() = default;
    InfoLogManager(const InfoLogManager&) = delete;
    InfoLogManager& operator=(const InfoLogManager&) = delete;

    void WriteLineLocked_(const std::wstring& line);
    std::wstring MakeTimestamp_() const;

private:
    bool enabled_ = false;
    std::wstring path_;
    std::mutex mtx_;
};

//
// グローバル関数（呼び出し側を簡単にする）
//
inline void InfoLog(const std::wstring& s) {
    InfoLogManager::GetInstance().Log(s);
}
inline void InfoLogf(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    // va_list を渡せないので Logf を直接呼んでOK（実装側が va_list を受けない版の場合）
    va_end(ap);
    InfoLogManager::GetInstance().Logf(fmt);
}

#endif // INFO_LOG_MANAGER_H