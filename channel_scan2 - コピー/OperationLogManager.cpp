// OperationLogManager.cpp
#include "OperationLogManager.h"
#include "OperationMPMCLogBuffer.hpp"

#include <thread>
#include <atomic>
#include <cstdio>      // _wfopen_s, fwprintf
#include <memory>
#include <chrono>
#include <vector>
#include <cstdarg>
#include <string>
#include <mutex>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace op_log
{
    namespace
    {
        std::unique_ptr<MPMCLogBuffer> g_buffer;
        std::thread                    g_worker;
        std::atomic<bool>              g_running{ false };
        std::wstring                   g_filePath;

        // 実際にファイルへ書く関数
        void WorkerThread()
        {
            FILE* fp = nullptr;
            if (_wfopen_s(&fp, g_filePath.c_str(), L"a, ccs=UTF-8") != 0 || !fp) {
                g_running.store(false, std::memory_order_relaxed);
                return;
            }

            while (g_running.load(std::memory_order_relaxed)) {
                std::wstring line;
                bool wroteAny = false;

                // 500ms 窓の間にあるだけ全部書く
                auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                do {
                    if (g_buffer->pop(line, std::chrono::milliseconds(50))) {
                        fwprintf(fp, L"%ls\n", line.c_str());
                        wroteAny = true;
                    }
                    else {
                        // 何もなければ少し待って次へ
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                } while (std::chrono::steady_clock::now() < deadline);

                if (wroteAny) {
                    fflush(fp); // ここでまとめて flush
                }
            }

            // 終了時の残り flush は今のままで OK
            std::wstring rest;
            g_buffer->drain_all(rest);
            if (!rest.empty()) {
                fwprintf(fp, L"%ls\n", rest.c_str());
                fflush(fp);
            }

            fclose(fp);
        }


        // フォーマット用ヘルパ
        std::wstring VFormat(const wchar_t* fmt, va_list args)
        {
            if (!fmt) return L"";

            // 一旦適当サイズで試す
            wchar_t buf[1024];
            va_list argsCopy;
            va_copy(argsCopy, args);
            int len = _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, argsCopy);
            va_end(argsCopy);

            if (len >= 0 && len < static_cast<int>(_countof(buf))) {
                return std::wstring(buf, len);
            }

            // 長すぎる場合は動的確保
            int required = _vscwprintf(fmt, args);
            if (required <= 0) return L"";

            std::wstring out;
            out.resize(required);
            _vsnwprintf_s(out.data(), out.size() + 1, _TRUNCATE, fmt, args);
            return out;
        }

        // ★ タイムスタンプ＋レベルを付けた 1 行を組み立てる
        std::wstring MakeTimestampedLine(std::wstring_view msg, std::wstring_view level = L"INFO")
        {
            using namespace std::chrono;

            SYSTEMTIME st{};
            GetLocalTime(&st);

            wchar_t timeBuf[64];
            // [YYYY-MM-DD HH:MM:SS.mmm][LEVEL] メッセージ
            _snwprintf_s(
                timeBuf, _countof(timeBuf), _TRUNCATE,
                L"%04d-%02d-%02d %02d:%02d:%02d.%03d",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds
            );

            std::wstring line;
            line.reserve(64 + msg.size());
            line.append(L"[");
            line.append(timeBuf);
            line.append(L"] [");
            line.append(level);
            line.append(L"] ");
            line.append(msg.begin(), msg.end());
            return line;
        }

    } // anonymous namespace


    bool Initialize(const std::wstring& filePath, std::size_t bufferCapacity)
    {
        if (g_running.load(std::memory_order_relaxed)) {
            // すでに初期化済み
            return true;
        }

        g_filePath = filePath;
        g_buffer = std::make_unique<MPMCLogBuffer>(bufferCapacity);

        g_running.store(true, std::memory_order_relaxed);
        g_worker = std::thread(WorkerThread);
        return true;
    }

    void Finalize()
    {
        if (!g_running.exchange(false, std::memory_order_relaxed)) {
            return;
        }

        if (g_worker.joinable()) {
            g_worker.join();
        }

        g_buffer.reset();
        g_filePath.clear();
    }

    void Log(std::wstring_view msg)
    {
        if (!g_running.load(std::memory_order_relaxed) || !g_buffer) {
            return;
        }
        std::wstring line = MakeTimestampedLine(msg, L"INFO");
        g_buffer->push(std::move(line));
    }

    void LogFormat(const wchar_t* fmt, ...)
    {
        if (!g_running.load(std::memory_order_relaxed) || !g_buffer || !fmt) {
            return;
        }

        va_list args;
        va_start(args, fmt);
        std::wstring msg = VFormat(fmt, args);
        va_end(args);

        if (!msg.empty()) {
            std::wstring line = MakeTimestampedLine(msg, L"INFO");
            g_buffer->push(std::move(line));
        }
    }


} // namespace op_log
