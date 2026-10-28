#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "AppWorkerPool.hpp"

#include <algorithm>
#include <exception>

namespace
{
    constexpr unsigned kMaxWorkerCount_ = 32;

    unsigned ClampWorkerCount_(unsigned v) noexcept
    {
        if (v < 1u) {
            return 1u;
        }

        if (v > kMaxWorkerCount_) {
            return kMaxWorkerCount_;
        }

        return v;
    }
}

namespace AppWorker
{
    AppWorkerPool::~AppWorkerPool()
    {
        // 安全網。通常は MainUI 側で StopAndWait() を明示的に呼ぶ。
        StopAndWait(true);
    }

    bool AppWorkerPool::Start(const WorkerPoolOptions& options)
    {
        StopAndWait(true);
        Reset();

        WorkerPoolOptions opt = options;

        if (opt.workerCount == 0) {
            opt.workerCount =
                GetRecommendedWorkerCount(opt.reserveCoreForUi);
        }

        opt.workerCount = ClampWorkerCount_(opt.workerCount);

        {
            std::lock_guard<std::mutex> lock(mutex_);

            options_ = opt;
            stopRequested_.store(false, std::memory_order_release);
            accepting_.store(true, std::memory_order_release);
            running_.store(true, std::memory_order_release);
        }

        try {
            workers_.reserve(opt.workerCount);

            for (unsigned i = 0; i < opt.workerCount; ++i) {
                workers_.emplace_back(
                    [this, i]()
                    {
                        WorkerMain_(i);
                    });
            }
        }
        catch (...) {
            RequestStop(true);
            Wait();
            return false;
        }

        return true;
    }

    bool AppWorkerPool::StartDefault()
    {
        WorkerPoolOptions opt{};
        return Start(opt);
    }

    void AppWorkerPool::StopAccepting() noexcept
    {
        accepting_.store(false, std::memory_order_release);
        cv_.notify_all();
    }

    void AppWorkerPool::RequestStop(bool clearPending) noexcept
    {
        accepting_.store(false, std::memory_order_release);
        stopRequested_.store(true, std::memory_order_release);

        if (clearPending) {
            std::lock_guard<std::mutex> lock(mutex_);
            ClearQueuesLocked_();
        }

        cv_.notify_all();
    }

    void AppWorkerPool::Wait() noexcept
    {
        for (auto& th : workers_) {
            if (th.joinable()) {
                try {
                    th.join();
                }
                catch (...) {
                }
            }
        }

        workers_.clear();

        running_.store(false, std::memory_order_release);
        accepting_.store(false, std::memory_order_release);
    }

    void AppWorkerPool::StopAndWait(bool clearPending) noexcept
    {
        RequestStop(clearPending);
        Wait();
    }

    void AppWorkerPool::Reset() noexcept
    {
        if (running_.load(std::memory_order_acquire)) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        ClearQueuesLocked_();

        accepting_.store(false, std::memory_order_release);
        stopRequested_.store(false, std::memory_order_release);
        activeCount_.store(0, std::memory_order_release);

        postedCount_.store(0, std::memory_order_release);
        rejectedCount_.store(0, std::memory_order_release);
        completedCount_.store(0, std::memory_order_release);
        failedCount_.store(0, std::memory_order_release);

        nextTaskId_.store(1, std::memory_order_release);
    }

    bool AppWorkerPool::IsRunning() const noexcept
    {
        return running_.load(std::memory_order_acquire);
    }

    bool AppWorkerPool::IsAccepting() const noexcept
    {
        return accepting_.load(std::memory_order_acquire);
    }

    bool AppWorkerPool::IsStopRequested() const noexcept
    {
        return stopRequested_.load(std::memory_order_acquire);
    }

    TaskId AppWorkerPool::Post(
        TaskPriority priority,
        TaskFunction&& task)
    {
        if (!task) {
            rejectedCount_.fetch_add(1, std::memory_order_acq_rel);
            return 0;
        }

        if (!running_.load(std::memory_order_acquire) ||
            !accepting_.load(std::memory_order_acquire) ||
            stopRequested_.load(std::memory_order_acquire)) {
            rejectedCount_.fetch_add(1, std::memory_order_acq_rel);
            return 0;
        }

        TaskItem item{};
        item.id = nextTaskId_.fetch_add(1, std::memory_order_acq_rel);
        item.priority = priority;
        item.fn = std::move(task);

        const TaskId postedTaskId = item.id;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (!accepting_.load(std::memory_order_acquire) ||
                stopRequested_.load(std::memory_order_acquire)) {
                rejectedCount_.fetch_add(1, std::memory_order_acq_rel);
                return 0;
            }

            switch (priority) {
            case TaskPriority::High:
                highQueue_.push_back(std::move(item));
                break;

            case TaskPriority::Low:
                lowQueue_.push_back(std::move(item));
                break;

            case TaskPriority::Normal:
            default:
                normalQueue_.push_back(std::move(item));
                break;
            }
        }

        postedCount_.fetch_add(1, std::memory_order_acq_rel);
        cv_.notify_one();

        return postedTaskId;
    }

    TaskId AppWorkerPool::PostNormal(TaskFunction&& task)
    {
        return Post(TaskPriority::Normal, std::move(task));
    }

    TaskId AppWorkerPool::PostHigh(TaskFunction&& task)
    {
        return Post(TaskPriority::High, std::move(task));
    }

    TaskId AppWorkerPool::PostLow(TaskFunction&& task)
    {
        return Post(TaskPriority::Low, std::move(task));
    }

    WorkerPoolStats AppWorkerPool::GetStats() const
    {
        WorkerPoolStats s{};

        {
            std::lock_guard<std::mutex> lock(mutex_);

            s.queuedHigh = highQueue_.size();
            s.queuedNormal = normalQueue_.size();
            s.queuedLow = lowQueue_.size();
        }

        s.queuedTotal =
            s.queuedHigh +
            s.queuedNormal +
            s.queuedLow;

        s.workerCount =
            static_cast<unsigned>(workers_.size());

        s.posted =
            postedCount_.load(std::memory_order_acquire);

        s.rejected =
            rejectedCount_.load(std::memory_order_acquire);

        s.completed =
            completedCount_.load(std::memory_order_acquire);

        s.failed =
            failedCount_.load(std::memory_order_acquire);

        s.active =
            activeCount_.load(std::memory_order_acquire);

        s.running =
            running_.load(std::memory_order_acquire);

        s.accepting =
            accepting_.load(std::memory_order_acquire);

        s.stopRequested =
            stopRequested_.load(std::memory_order_acquire);

        return s;
    }

    unsigned AppWorkerPool::GetLogicalProcessorCount() noexcept
    {
        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);

        if (si.dwNumberOfProcessors == 0) {
            return 1;
        }

        return static_cast<unsigned>(si.dwNumberOfProcessors);
    }

    unsigned AppWorkerPool::GetRecommendedWorkerCount(
        bool reserveCoreForUi) noexcept
    {
        unsigned count = GetLogicalProcessorCount();

        if (reserveCoreForUi && count > 1u) {
            --count;
        }

        return ClampWorkerCount_(count);
    }

    void AppWorkerPool::WorkerMain_(unsigned workerIndex) noexcept
    {
        ApplyWorkerThreadPolicy_(options_);

        if (!options_.threadNamePrefix.empty()) {
            std::wstring name = options_.threadNamePrefix;
            name += L"#";
            name += std::to_wstring(workerIndex);
            SetCurrentThreadName_(name);
        }

        for (;;) {
            TaskItem item{};

            std::unique_lock<std::mutex> lock(mutex_);

            cv_.wait(
                lock,
                [this]()
                {
                    return
                        stopRequested_.load(std::memory_order_acquire) ||
                        !highQueue_.empty() ||
                        !normalQueue_.empty() ||
                        !lowQueue_.empty() ||
                        !accepting_.load(std::memory_order_acquire);
                });

            const bool hasTask =
                PopTaskLocked_(item);

            // Visual Studio の静的解析が unique_lock のスコープ解放を
            // 追いきれないことがあるため、break 判定前に明示 unlock する。
            lock.unlock();

            if (!hasTask) {
                break;
            }

            activeCount_.fetch_add(1, std::memory_order_acq_rel);

            try {
                TaskContext ctx(
                    &stopRequested_,
                    item.id,
                    item.priority);

                item.fn(ctx);

                completedCount_.fetch_add(1, std::memory_order_acq_rel);
            }
            catch (...) {
                failedCount_.fetch_add(1, std::memory_order_acq_rel);
            }

            activeCount_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    bool AppWorkerPool::PopTaskLocked_(TaskItem& out)
    {
        if (stopRequested_.load(std::memory_order_acquire)) {
            return false;
        }

        if (!highQueue_.empty()) {
            out = std::move(highQueue_.front());
            highQueue_.pop_front();
            return true;
        }

        if (!normalQueue_.empty()) {
            out = std::move(normalQueue_.front());
            normalQueue_.pop_front();
            return true;
        }

        if (!lowQueue_.empty()) {
            out = std::move(lowQueue_.front());
            lowQueue_.pop_front();
            return true;
        }

        if (!accepting_.load(std::memory_order_acquire)) {
            return false;
        }

        return false;
    }

    void AppWorkerPool::ClearQueuesLocked_() noexcept
    {
        highQueue_.clear();
        normalQueue_.clear();
        lowQueue_.clear();
    }

    void AppWorkerPool::ApplyWorkerThreadPolicy_(
        const WorkerPoolOptions& options) noexcept
    {
        if (!options.backgroundMode) {
            return;
        }

        HANDLE hThread = GetCurrentThread();

        if (SetThreadPriority(
            hThread,
            THREAD_MODE_BACKGROUND_BEGIN) == FALSE) {
            SetThreadPriority(
                hThread,
                THREAD_PRIORITY_BELOW_NORMAL);
        }
    }

    void AppWorkerPool::SetCurrentThreadName_(
        const std::wstring& name) noexcept
    {
        if (name.empty()) {
            return;
        }

        using SetThreadDescriptionFunc =
            HRESULT(WINAPI*)(HANDLE, PCWSTR);

        HMODULE hKernel = GetModuleHandleW(L"Kernel32.dll");

        if (hKernel == nullptr) {
            return;
        }

        auto* fn =
            reinterpret_cast<SetThreadDescriptionFunc>(
                GetProcAddress(
                    hKernel,
                    "SetThreadDescription"));

        if (fn == nullptr) {
            return;
        }

        fn(GetCurrentThread(), name.c_str());
    }
}