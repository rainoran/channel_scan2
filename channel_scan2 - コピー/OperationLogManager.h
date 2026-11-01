// OperationLogManager.h
#pragma once
#ifndef OPERATION_LOG_MANAGER_H
#define OPERATION_LOG_MANAGER_H

#include <string>
#include <string_view>
#include <cstdarg>

namespace op_log
{
    // 初期化: ファイル名とバッファ容量を指定
    bool Initialize(const std::wstring& filePath,
        std::size_t bufferCapacity = 1024);

    // 終了: スレッド停止 & バッファ flush
    void Finalize();

    // 単純な 1 行ログ
    void Log(std::wstring_view msg);

    // printf 風フォーマットログ
    void LogFormat(const wchar_t* fmt, ...);

} // namespace op_log

#endif // OPERATION_LOG_MANAGER_H