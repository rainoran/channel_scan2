#pragma once
// OperationMPMCLogBuffer.hpp
#pragma once

#include <deque>
#include <mutex>
#include <condition_variable>
#include <string>
#include <chrono>

namespace op_log
{
    class MPMCLogBuffer
    {
    public:
        explicit MPMCLogBuffer(std::size_t capacity = 1024)
            : m_capacity(capacity)
        {
        }

        // 生の wstring を push（コピー）
        void push(const std::wstring& line)
        {
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                // バッファがいっぱいなら「最古の1件だけ捨てる」
                // ※低頻度なので基本ここには来ないはず
                if (m_queue.size() >= m_capacity) {
                    m_queue.pop_front();
                }
                m_queue.push_back(line);
            }
            m_cv.notify_one();
        }

        // タイムアウト付き pop
        //  - true  : 取り出し成功（out にセット）
        //  - false : タイムアウト（何もなかった）
        bool pop(std::wstring& out,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(500))
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (!m_cv.wait_for(lock, timeout, [this]() { return !m_queue.empty(); })) {
                return false; // timeout
            }
            out = std::move(m_queue.front());
            m_queue.pop_front();
            return true;
        }

        // 残っているログをすべて取り出す（終了処理用）
        void drain_all(std::wstring& concat, const std::wstring& lineSep = L"\r\n")
        {
            std::deque<std::wstring> temp;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                temp.swap(m_queue);
            }

            // まとめて結合
            bool first = true;
            for (auto& s : temp) {
                if (!first) concat += lineSep;
                concat += s;
                first = false;
            }
        }

    private:
        std::size_t              m_capacity;
        std::deque<std::wstring> m_queue;
        std::mutex               m_mutex;
        std::condition_variable  m_cv;
    };

} // namespace op_log
