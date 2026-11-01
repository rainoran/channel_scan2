#include "MultiTunerTaskRunner.hpp"
#include "IBonDriver2.h"

#include <algorithm>
#include <utility>

MultiTunerTaskRunner::~MultiTunerTaskRunner()
{
    Stop(false);
}

bool MultiTunerTaskRunner::Initialize(const Settings& s, const Hooks& h)
{
    Stop(true);

    settings_ = s;
    hooks_ = h;

    tunerCount_ = (settings_.tunerCount <= 0) ? 1 : settings_.tunerCount;
    tuners_.assign(static_cast<std::size_t>(tunerCount_), nullptr);
    tunerAvailable_.assign(static_cast<std::size_t>(tunerCount_), true);

    qTunerPer_.clear();
    qTunerPer_.resize(static_cast<std::size_t>(tunerCount_));
    qTunerAny_.clear();
    qUtility_.clear();
    pendingOrRunning_.clear();

    return true;
}

void MultiTunerTaskRunner::SetTuner(int tunerIndex, IBonDriver2* bon)
{
    if (tunerIndex < 0 || tunerIndex >= tunerCount_) return;
    std::lock_guard lk(mtx_);
    tuners_[static_cast<std::size_t>(tunerIndex)] = bon;
}

void MultiTunerTaskRunner::SetTunerAvailable(int tunerIndex, bool available)
{
    if (tunerIndex < 0 || tunerIndex >= tunerCount_) return;
    {
        std::lock_guard lk(mtx_);
        tunerAvailable_[static_cast<std::size_t>(tunerIndex)] = available;
    }
    cv_.notify_all();
}

bool MultiTunerTaskRunner::RegisterHandler(std::uint32_t kind, TaskHandler handler)
{
    if (!handler) return false;
    std::lock_guard lk(mtx_);
    handlers_[kind] = std::move(handler);
    return true;
}

bool MultiTunerTaskRunner::Start()
{
    if (running_.load()) return true;

    running_.store(true);

    tunerWorkers_.clear();
    tunerWorkers_.reserve(static_cast<std::size_t>(tunerCount_));
    for (int i = 0; i < tunerCount_; ++i) {
        tunerWorkers_.emplace_back([this, i]() { WorkerLoopTuner_(i); });
    }

    if (settings_.enableUtilityWorker) {
        utilityWorker_ = std::thread([this]() { WorkerLoopUtility_(); });
    }

    Log_(L"[TaskRunner] started");
    return true;
}

void MultiTunerTaskRunner::Stop(bool clearQueue)
{
    const bool wasRunning = running_.exchange(false);
    if (wasRunning) {
        cv_.notify_all();
        for (auto& th : tunerWorkers_) {
            if (th.joinable()) th.join();
        }
        tunerWorkers_.clear();

        if (utilityWorker_.joinable()) {
            utilityWorker_.join();
        }

        Log_(L"[TaskRunner] stopped");
    }

    if (clearQueue) {
        std::lock_guard lk(mtx_);
        for (auto& q : qTunerPer_) q.clear();
        qTunerAny_.clear();
        qUtility_.clear();
        pendingOrRunning_.clear();
    }
}

std::uint64_t MultiTunerTaskRunner::MakeKey_(std::uint32_t kind, std::uint32_t instance)
{
    return (static_cast<std::uint64_t>(kind) << 32) | static_cast<std::uint64_t>(instance);
}

int MultiTunerTaskRunner::TotalQueueSizeUnsafe_() const
{
    int n = (int)qTunerAny_.size() + (int)qUtility_.size();
    for (const auto& q : qTunerPer_) n += (int)q.size();
    return n;
}

void MultiTunerTaskRunner::CompleteKey_(std::uint64_t key) noexcept
{
    std::lock_guard<std::mutex> lock(mtx_);
    pendingOrRunning_.erase(key);
}

MultiTunerTaskRunner::TaskHandler
MultiTunerTaskRunner::FindHandler_(std::uint32_t kind) const
{
    std::lock_guard<std::mutex> lock(mtx_);
    const auto it = handlers_.find(kind);
    return it != handlers_.end() ? it->second : TaskHandler{};
}

bool MultiTunerTaskRunner::Enqueue(const TimeKeeper::TaskRequest& req)
{
    if (!running_.load()) return false;

    if (req.requiredTuners > 1) {
        Log_(L"[TaskRunner] reject: requiredTuners>1 not supported yet");
        return false;
    }

    const auto key = MakeKey_(req.kind, req.instance);

    {
        std::lock_guard lk(mtx_);

        const int totalQ = TotalQueueSizeUnsafe_();
        if (settings_.maxQueueSize > 0 && totalQ >= settings_.maxQueueSize) {
            Log_(L"[TaskRunner] reject: queue overflow");
            return false;
        }

        if (pendingOrRunning_.find(key) != pendingOrRunning_.end()) {
            Log_(L"[TaskRunner] coalesced duplicate request");
            return true;
        }
        pendingOrRunning_.insert(key);

        auto pushByPriority = [](std::deque<TimeKeeper::TaskRequest>& q, const TimeKeeper::TaskRequest& r) {
            if (r.priority == TimeKeeper::TaskPriority::Hard) q.push_front(r);
            else q.push_back(r);
            };

        if (req.requiredTuners == 0) {
            pushByPriority(qUtility_, req);
        }
        else {
            // ★preferred tuner が有効ならそのtuner専用キューへ
            const int pref = req.preferredTunerIndex;
            if (0 <= pref && pref < tunerCount_ &&
                tunerAvailable_[static_cast<std::size_t>(pref)]) {
                pushByPriority(qTunerPer_[static_cast<std::size_t>(pref)], req);
            }
            else {
                pushByPriority(qTunerAny_, req);
            }
        }
    }

    cv_.notify_all();
    return true;
}

bool MultiTunerTaskRunner::PopNextForTuner_(int tunerIndex, TimeKeeper::TaskRequest& out)
{
    std::unique_lock lk(mtx_);
    cv_.wait(lk, [this, tunerIndex]() {
        if (!running_.load()) return true;
        if (tunerIndex < 0 || tunerIndex >= tunerCount_) return true;
        if (!tunerAvailable_[static_cast<std::size_t>(tunerIndex)]) return false;
        if (!qTunerPer_[static_cast<std::size_t>(tunerIndex)].empty()) return true;
        if (!qTunerAny_.empty()) return true;
        return false;
        });

    if (!running_.load()) return false;
    if (tunerIndex < 0 || tunerIndex >= tunerCount_) return false;
    if (!tunerAvailable_[static_cast<std::size_t>(tunerIndex)]) return false;

    auto& qMine = qTunerPer_[static_cast<std::size_t>(tunerIndex)];
    if (!qMine.empty()) {
        out = qMine.front();
        qMine.pop_front();
        return true;
    }
    if (!qTunerAny_.empty()) {
        out = qTunerAny_.front();
        qTunerAny_.pop_front();
        return true;
    }
    return false;
}

bool MultiTunerTaskRunner::PopNextForUtility_(TimeKeeper::TaskRequest& out)
{
    std::unique_lock lk(mtx_);
    cv_.wait(lk, [this]() {
        return !running_.load() || !qUtility_.empty();
        });

    if (!running_.load()) return false;
    if (qUtility_.empty()) return false;

    out = qUtility_.front();
    qUtility_.pop_front();
    return true;
}

void MultiTunerTaskRunner::WorkerLoopTuner_(int tunerIndex)
{
    for (;;) {
        TimeKeeper::TaskRequest req{};
        if (!PopNextForTuner_(tunerIndex, req)) break;

        const auto key = MakeKey_(req.kind, req.instance);

        if (hooks_.CanRun && !hooks_.CanRun()) {
            Log_(L"[TaskRunner] blocked by CanRun()");
            CompleteKey_(key);
            continue;
        }

        TaskHandler handler = FindHandler_(req.kind);
        IBonDriver2* bon = nullptr;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (0 <= tunerIndex && tunerIndex < tunerCount_) {
                bon = tuners_[static_cast<std::size_t>(tunerIndex)];
            }
        }

        if (!handler) {
            Log_(L"[TaskRunner] drop: no handler for task kind");
            CompleteKey_(key);
            continue;
        }

        try {
            handler(tunerIndex, bon, req);
        }
        catch (...) {
            Log_(L"[TaskRunner] handler threw exception");
        }

        CompleteKey_(key);
    }
}

void MultiTunerTaskRunner::WorkerLoopUtility_()
{
    for (;;) {
        TimeKeeper::TaskRequest req{};
        if (!PopNextForUtility_(req)) break;

        const auto key = MakeKey_(req.kind, req.instance);

        if (hooks_.CanRun && !hooks_.CanRun()) {
            Log_(L"[TaskRunner] utility blocked by CanRun()");
            CompleteKey_(key);
            continue;
        }

        // Worker本体ではmtx_を保持しない。MSVC C26115の解析対象から
        // ロック寿命を切り離し、FindHandler_内で必ず解放する。
        TaskHandler handler = FindHandler_(req.kind);

        if (!handler) {
            Log_(L"[TaskRunner] utility drop: no handler");
            CompleteKey_(key);
            continue;
        }

        try {
            handler(-1, nullptr, req);
        }
        catch (...) {
            Log_(L"[TaskRunner] utility handler threw exception");
        }

        CompleteKey_(key);
    }
}

void MultiTunerTaskRunner::Log_(const std::wstring& msg) const
{
    if (hooks_.Log) hooks_.Log(msg);
}
