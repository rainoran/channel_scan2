#pragma once
#ifndef APP_WORKER_POOL_HPP
#define APP_WORKER_POOL_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace AppWorker
{
    enum class TaskPriority : int
    {
        Low = 0,
        Normal = 1,
        High = 2
    };

    using TaskId = std::uint64_t;

    struct WorkerPoolOptions
    {
        // 0 の場合は自動。基本は logicalCore - 1。
        unsigned workerCount = 0;

        // UI用に1コア残す。
        bool reserveCoreForUi = true;

        // Workerを低めの優先度で動かす。
        bool backgroundMode = true;

        // スレッド名用。デバッガ上で見やすくする。
        std::wstring threadNamePrefix = L"AppWorker";
    };

    struct WorkerPoolStats
    {
        unsigned workerCount = 0;

        std::size_t queuedHigh = 0;
        std::size_t queuedNormal = 0;
        std::size_t queuedLow = 0;
        std::size_t queuedTotal = 0;

        std::uint64_t posted = 0;
        std::uint64_t rejected = 0;
        std::uint64_t completed = 0;
        std::uint64_t failed = 0;

        unsigned active = 0;

        bool running = false;
        bool accepting = false;
        bool stopRequested = false;
    };

    class TaskContext final
    {
    public:
        TaskContext() = default;

        bool IsStopRequested() const noexcept
        {
            return stopRequested_ != nullptr &&
                stopRequested_->load(std::memory_order_acquire);
        }

        TaskId GetTaskId() const noexcept
        {
            return taskId_;
        }

        TaskPriority GetPriority() const noexcept
        {
            return priority_;
        }

    private:
        friend class AppWorkerPool;

        explicit TaskContext(
            std::atomic_bool* stopRequested,
            TaskId taskId,
            TaskPriority priority) noexcept
            : stopRequested_(stopRequested)
            , taskId_(taskId)
            , priority_(priority)
        {
        }

    private:
        std::atomic_bool* stopRequested_ = nullptr;
        TaskId taskId_ = 0;
        TaskPriority priority_ = TaskPriority::Normal;
    };

    using TaskFunction =
        std::function<void(TaskContext&)>;

    class AppWorkerPool final
    {
    public:
        AppWorkerPool() = default;

        ~AppWorkerPool();

        AppWorkerPool(const AppWorkerPool&) = delete;
        AppWorkerPool& operator=(const AppWorkerPool&) = delete;

        bool Start(const WorkerPoolOptions& options);
        bool StartDefault();

        void StopAccepting() noexcept;

        // clearPending=true の場合、未実行タスクを破棄して終了する。
        void RequestStop(bool clearPending = true) noexcept;

        // StopAccepting / RequestStop 後に呼ぶ。
        void Wait() noexcept;

        void StopAndWait(bool clearPending = true) noexcept;

        // StopAndWait 後、再Startできる状態へ戻す。
        void Reset() noexcept;

        bool IsRunning() const noexcept;
        bool IsAccepting() const noexcept;
        bool IsStopRequested() const noexcept;

        TaskId Post(
            TaskPriority priority,
            TaskFunction&& task);

        TaskId PostNormal(TaskFunction&& task);
        TaskId PostHigh(TaskFunction&& task);
        TaskId PostLow(TaskFunction&& task);

        WorkerPoolStats GetStats() const;

        static unsigned GetLogicalProcessorCount() noexcept;
        static unsigned GetRecommendedWorkerCount(bool reserveCoreForUi) noexcept;

    private:
        struct TaskItem
        {
            TaskId id = 0;
            TaskPriority priority = TaskPriority::Normal;
            TaskFunction fn;
        };

    private:
        void WorkerMain_(unsigned workerIndex) noexcept;
        bool PopTaskLocked_(TaskItem& out);
        void ClearQueuesLocked_() noexcept;
        static void ApplyWorkerThreadPolicy_(const WorkerPoolOptions& options) noexcept;
        static void SetCurrentThreadName_(const std::wstring& name) noexcept;

    private:
        mutable std::mutex mutex_;
        std::condition_variable cv_;

        std::deque<TaskItem> highQueue_;
        std::deque<TaskItem> normalQueue_;
        std::deque<TaskItem> lowQueue_;

        std::vector<std::thread> workers_;

        WorkerPoolOptions options_{};

        std::atomic_bool running_{ false };
        std::atomic_bool accepting_{ false };
        std::atomic_bool stopRequested_{ false };

        std::atomic_uint activeCount_{ 0 };

        std::atomic_uint64_t nextTaskId_{ 1 };
        std::atomic_uint64_t postedCount_{ 0 };
        std::atomic_uint64_t rejectedCount_{ 0 };
        std::atomic_uint64_t completedCount_{ 0 };
        std::atomic_uint64_t failedCount_{ 0 };
    };
}

#endif // APP_WORKER_POOL_HPP