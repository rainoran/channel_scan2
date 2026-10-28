#pragma once
#ifndef TASK_RESERVATION_CACHE_HPP
#define TASK_RESERVATION_CACHE_HPP
#pragma once

#include "TaskListManager.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace TaskReservationCache
{
    struct Options
    {
        std::wstring tasksIniPath;
        std::wstring cacheRoot;

        bool writeGlobalViewIndex = true;
        bool writeServiceViewKeys = true;
        bool writeRunIndex = true;

        // 追加: 実行判定用の軽量キャッシュ
        bool writeExecIndex = true;

        // 追加: 古い service view cache を掃除してから再生成する
        bool cleanStaleServiceViewCache = true;
    };

    struct WriteResult
    {
        bool ok = false;

        std::size_t globalViewRows = 0;
        std::size_t serviceViewFiles = 0;
        std::size_t serviceViewRows = 0;
        std::size_t runRows = 0;

        // 追加
        std::size_t execRows = 0;

        std::wstring message;
    };

    std::wstring MakeDefaultCacheRoot(const std::wstring& tasksIniPath);

    WriteResult WriteAllFromTasks(
        const Options& options,
        const std::vector<TaskListManager::TaskEntry>& tasks);

    WriteResult WriteAllFromTasks(
        const std::wstring& tasksIniPath,
        const std::vector<TaskListManager::TaskEntry>& tasks);
}

#endif // TASK_RESERVATION_CACHE_HPP