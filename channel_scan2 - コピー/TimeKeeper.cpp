// TimeKeeper.cpp
#include "TimeKeeper.hpp"
#include "TaskKinds.hpp"
#include "DebugManager.h"

#include <algorithm>

void TimeKeeper::Configure(const Settings& s, const Hooks& h)
{
    settings_ = s;
    hooks_ = h;
}

bool TimeKeeper::Start(HWND hwnd, UINT_PTR timerId)
{
    hwnd_ = hwnd;
    timerId_ = timerId;

    // 既に走っていたら一旦止める（設定変更の反映を安全にする）
    if (started_ && hwnd_) {
        ::KillTimer(hwnd_, timerId_);
        started_ = false;
    }
    if (!hwnd_) return false;

    startTick_ = ::GetTickCount64();

    // periodic抑止の初期化（enableAuto=true の時だけ意味がある）
    if (settings_.enableAuto && !settings_.catchUpOnStart) {
        int ymd = 0, hh = 0, mm = 0, hhmm = 0;
        GetNow_(ymd, hh, mm, hhmm);

        if (settings_.enableDailyFull) {
            const int fullHHMM = settings_.dailyFullAt.hour * 100 + settings_.dailyFullAt.minute;
            if (hhmm >= fullHHMM) {
                lastFullYmd_ = ymd;
            }
        }

        if (settings_.enableHourly) {
            if (mm >= settings_.hourlyAtMinute) {
                lastLightHourKey_ = ymd * 100 + hh;
            }
        }
        else {
            lastLightTick_ = startTick_;
        }
    }

    const UINT elapseMs = (settings_.tickSeconds <= 0 ? 5 : settings_.tickSeconds) * 1000u;
    if (::SetTimer(hwnd_, timerId_, elapseMs, nullptr) == 0) {
        started_ = false;
        return false;
    }

    started_ = true;

    DebugLogFormatSafe(
        L"[TimeKeeper] started tick=%ds auto=%d oneShots=%zu",
        (settings_.tickSeconds <= 0 ? 5 : settings_.tickSeconds),
        settings_.enableAuto ? 1 : 0,
        (size_t)oneShots_.size()
    );
    return true;
}

void TimeKeeper::Stop(HWND /*hwnd*/)
{
    if (!started_) return;
    if (hwnd_) ::KillTimer(hwnd_, timerId_);
    started_ = false;
    DebugLog(L"[TimeKeeper] stopped");
}

void TimeKeeper::ClearOneShotTasks()
{
    oneShots_.clear();
    DebugLog(L"[TimeKeeper] OneShot cleared");
}

int TimeKeeper::CompareLocalYmdHHMM_(int ymd1, int hhmm1, int ymd2, int hhmm2)
{
    if (ymd1 != ymd2) return (ymd1 < ymd2) ? -1 : 1;
    if (hhmm1 != hhmm2) return (hhmm1 < hhmm2) ? -1 : 1;
    return 0;
}

void TimeKeeper::AddOneShotTask(
    int dueYmd,
    int dueHHMM,
    const TaskRequest& req,
    int lateToleranceMinutes,
    bool useTimeWindow,
    int windowStartYmd,
    int windowStartHHMM,
    int windowEndYmd,
    int windowEndHHMM)
{
    OneShotTask t{};
    t.dueYmd = dueYmd;
    t.dueHHMM = dueHHMM;
    t.req = req;
    t.lateToleranceMinutes = lateToleranceMinutes;

    t.useTimeWindow = useTimeWindow;
    t.windowStartYmd = windowStartYmd;
    t.windowStartHHMM = windowStartHHMM;
    t.windowEndYmd = windowEndYmd;
    t.windowEndHHMM = windowEndHHMM;

    t.lastAttemptTick = 0;
    t.firedOrMissed = false;
    oneShots_.push_back(t);

    DebugLogFormatSafe(
        L"[TimeKeeper] OneShot added kind=%u inst=%u due=%08d %02d:%02d window=%d [%08d %02d:%02d - %08d %02d:%02d] tol=%d",
        req.kind,
        req.instance,
        dueYmd,
        dueHHMM / 100, dueHHMM % 100,
        useTimeWindow ? 1 : 0,
        windowStartYmd,
        windowStartHHMM / 100, windowStartHHMM % 100,
        windowEndYmd,
        windowEndHHMM / 100, windowEndHHMM % 100,
        lateToleranceMinutes);
}

bool TimeKeeper::CancelOneShotTask(std::uint32_t kind, std::uint32_t instance)
{
    const auto it = std::remove_if(oneShots_.begin(), oneShots_.end(),
        [kind, instance](const OneShotTask& t) {
            return (t.req.kind == kind && t.req.instance == instance);
        });
    const bool removed = (it != oneShots_.end());
    oneShots_.erase(it, oneShots_.end());

    DebugLogFormatSafe(L"[TimeKeeper] OneShot cancel kind=%u inst=%u removed=%d", kind, instance, removed ? 1 : 0);
    return removed;
}

bool TimeKeeper::IsIdleEnough_() const
{
    if (settings_.idleRequiredSeconds <= 0) return true;

    LASTINPUTINFO li{};
    li.cbSize = sizeof(li);
    if (!::GetLastInputInfo(&li)) return true;

    const ULONGLONG now = ::GetTickCount64();
    const ULONGLONG last = li.dwTime;
    const ULONGLONG idleMs = (now >= last) ? (now - last) : 0;
    return idleMs >= static_cast<ULONGLONG>(settings_.idleRequiredSeconds) * 1000ULL;
}

void TimeKeeper::GetNow_(int& outYmd, int& outHour, int& outMin, int& outHHMM)
{
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    outYmd = (int)st.wYear * 10000 + (int)st.wMonth * 100 + (int)st.wDay;
    outHour = (int)st.wHour;
    outMin = (int)st.wMinute;
    outHHMM = outHour * 100 + outMin;
}

bool TimeKeeper::IsWithinBackoff_(ULONGLONG nowTick, ULONGLONG lastAttemptTick, int backoffSeconds)
{
    if (backoffSeconds <= 0) return false;
    if (lastAttemptTick == 0) return false;
    const ULONGLONG backoffMs = (ULONGLONG)backoffSeconds * 1000ULL;
    return (nowTick - lastAttemptTick) < backoffMs;
}

bool TimeKeeper::Dispatch_(const TaskRequest& req, ULONGLONG& inoutLastAttemptTick, const wchar_t* logTag)
{
    const ULONGLONG nowTick = ::GetTickCount64();

    if (IsWithinBackoff_(nowTick, inoutLastAttemptTick, settings_.enqueueRetryBackoffSeconds)) {
        DebugLogFormatSafe(L"[TimeKeeper] backoff skip %s kind=%u inst=%u", logTag, req.kind, req.instance);
        return false;
    }
    inoutLastAttemptTick = nowTick;

    if (hooks_.EnqueueTask) {
        const bool accepted = hooks_.EnqueueTask(req);
        DebugLogFormatSafe(
            L"[TimeKeeper] enqueue %s %s kind=%u inst=%u",
            accepted ? L"accepted" : L"rejected",
            logTag,
            req.kind, req.instance
        );
        return accepted;
    }

    // legacy
    if (req.kind == TaskKinds::EpgFull) {
        DebugLog(L"[TimeKeeper] legacy FULL");
        if (hooks_.StartFullUpdate) { hooks_.StartFullUpdate(); return true; }
        return false;
    }
    if (req.kind == TaskKinds::EpgLight) {
        DebugLog(L"[TimeKeeper] legacy LIGHT");
        if (hooks_.StartLightUpdate) { hooks_.StartLightUpdate(); return true; }
        return false;
    }

    DebugLogFormatSafe(L"[TimeKeeper] no dispatcher kind=%u inst=%u", req.kind, req.instance);
    return false;
}

ULONGLONG TimeKeeper::FileTimeToULL_(const FILETIME& ft)
{
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

bool TimeKeeper::LocalYmdHHMMToUtcFileTime_(int ymd, int hhmm, FILETIME& outUtcFt)
{
    SYSTEMTIME local{};
    local.wYear = (WORD)(ymd / 10000);
    local.wMonth = (WORD)((ymd / 100) % 100);
    local.wDay = (WORD)(ymd % 100);
    local.wHour = (WORD)(hhmm / 100);
    local.wMinute = (WORD)(hhmm % 100);
    local.wSecond = 0;
    local.wMilliseconds = 0;

    SYSTEMTIME utc{};
    if (!::TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) {
        return false;
    }
    return ::SystemTimeToFileTime(&utc, &outUtcFt) != 0;
}

int TimeKeeper::DiffMinutesUtcFileTime_(const FILETIME& a, const FILETIME& b)
{
    const ULONGLONG ua = FileTimeToULL_(a);
    const ULONGLONG ub = FileTimeToULL_(b);
    const ULONGLONG diff100ns = (ua >= ub) ? (ua - ub) : (ub - ua);
    const int mins = (int)(diff100ns / (10ULL * 1000ULL * 1000ULL * 60ULL));
    return (ua >= ub) ? mins : -mins;
}

void TimeKeeper::OnTick()
{
    const ULONGLONG nowTick = ::GetTickCount64();

    // 1) OneShot：enableAuto と無関係に常に監視
        // 1) OneShot：enableAuto と無関係に常に監視
    if (!oneShots_.empty()) {
        int nowYmd = 0, nowH = 0, nowM = 0, nowHHMM = 0;
        GetNow_(nowYmd, nowH, nowM, nowHHMM);

        FILETIME nowUtcFt{};
        ::GetSystemTimeAsFileTime(&nowUtcFt);

        for (auto& t : oneShots_) {
            if (t.firedOrMissed) continue;

            if (t.useTimeWindow) {
                const int cmpStart = CompareLocalYmdHHMM_(nowYmd, nowHHMM, t.windowStartYmd, t.windowStartHHMM);
                if (cmpStart < 0) {
                    continue;
                }

                const int cmpEnd = CompareLocalYmdHHMM_(nowYmd, nowHHMM, t.windowEndYmd, t.windowEndHHMM);
                if (cmpEnd > 0) {
                    t.firedOrMissed = true;
                    DebugLogFormatSafe(
                        L"[TimeKeeper] OneShot window missed kind=%u inst=%u now=%08d %04d end=%08d %04d",
                        t.req.kind,
                        t.req.instance,
                        nowYmd, nowHHMM,
                        t.windowEndYmd, t.windowEndHHMM);

                    if (hooks_.OnTaskMissed) {
                        hooks_.OnTaskMissed(t.req, L"time window expired");
                    }
                    continue;
                }

                if (Dispatch_(t.req, t.lastAttemptTick, L"OneShotWindow")) {
                    t.firedOrMissed = true;
                    return;
                }
                continue;
            }

            const bool due = (nowYmd > t.dueYmd) || (nowYmd == t.dueYmd && nowHHMM >= t.dueHHMM);
            if (!due) continue;

            DebugLogFormatSafe(
                L"[TimeKeeper] OneShot due now=%08d %02d:%02d due=%08d %02d:%02d",
                nowYmd, nowHHMM / 100, nowHHMM % 100,
                t.dueYmd, t.dueHHMM / 100, t.dueHHMM % 100
            );

            if (t.lateToleranceMinutes >= 0) {
                FILETIME dueUtcFt{};
                if (LocalYmdHHMMToUtcFileTime_(t.dueYmd, t.dueHHMM, dueUtcFt)) {
                    const int lateMin = DiffMinutesUtcFileTime_(nowUtcFt, dueUtcFt);
                    if (lateMin > t.lateToleranceMinutes) {
                        t.firedOrMissed = true;
                        DebugLogFormatSafe(
                            L"[TimeKeeper] OneShot missed late=%d tol=%d kind=%u inst=%u",
                            lateMin,
                            t.lateToleranceMinutes,
                            t.req.kind,
                            t.req.instance);

                        if (hooks_.OnTaskMissed) {
                            hooks_.OnTaskMissed(t.req, L"late tolerance expired");
                        }
                        continue;
                    }
                }
            }

            if (Dispatch_(t.req, t.lastAttemptTick, L"OneShot")) {
                t.firedOrMissed = true;
                return;
            }
        }

        oneShots_.erase(
            std::remove_if(oneShots_.begin(), oneShots_.end(),
                [](const OneShotTask& x) { return x.firedOrMissed; }),
            oneShots_.end());
    }

    // 2) periodic は enableAuto=false なら動かさない
    if (!settings_.enableAuto) return;

    // 3) boot grace（Softのみ）
    if (settings_.bootGraceSeconds > 0) {
        const ULONGLONG graceMs = (ULONGLONG)settings_.bootGraceSeconds * 1000ULL;
        if (startTick_ != 0 && (nowTick - startTick_) < graceMs) {
            return;
        }
    }

    if (!IsIdleEnough_()) return;
    if (hooks_.CanRun && !hooks_.CanRun()) return;

    int nowYmd = 0, nowH = 0, nowM = 0, nowHHMM = 0;
    GetNow_(nowYmd, nowH, nowM, nowHHMM);

    // FULL（1日1回）
    if (settings_.enableDailyFull) {
        const int fullHHMM =
            settings_.dailyFullAt.hour * 100 +
            settings_.dailyFullAt.minute;

        // Daily Full は指定分ちょうどだけ起動する。
        // 遅延起動・起動時catch-upは OneShot window 側で扱う。
        if (nowHHMM == fullHHMM && lastFullYmd_ != nowYmd) {
            TaskRequest req{};
            req.kind = TaskKinds::EpgFull;
            req.instance = 0;
            req.priority = TaskPriority::Soft;
            req.requiredTuners = settings_.epgRequiredTuners;

            if (Dispatch_(req, lastFullAttemptTick_, L"FULL")) {
                lastFullYmd_ = nowYmd;
                return;
            }
        }
        else if (nowHHMM > fullHHMM && lastFullYmd_ != nowYmd) {
            lastFullYmd_ = nowYmd;

            DebugLogFormatSafe(
                L"[TimeKeeper] FULL skipped outside exact minute now=%08d %04d full=%04d",
                nowYmd,
                nowHHMM,
                fullHHMM);
        }
    }

    // LIGHT
    if (settings_.enableHourly) {
        const int hourKey = nowYmd * 100 + nowH;
        if (nowM >= settings_.hourlyAtMinute && lastLightHourKey_ != hourKey) {
            TaskRequest req{};
            req.kind = TaskKinds::EpgLight;
            req.instance = 0;
            req.priority = TaskPriority::Soft;
            req.requiredTuners = settings_.epgRequiredTuners;

            if (Dispatch_(req, lastLightAttemptTick_, L"LIGHT(hourly)")) {
                lastLightHourKey_ = hourKey;
                return;
            }
        }
        return;
    }

    if (settings_.lightIntervalMinutes > 0) {
        const ULONGLONG intervalMs =
            static_cast<ULONGLONG>(settings_.lightIntervalMinutes) * 60ULL * 1000ULL;

        if (lastLightTick_ == 0 || (nowTick - lastLightTick_) >= intervalMs) {
            TaskRequest req{};
            req.kind = TaskKinds::EpgLight;
            req.instance = 0;
            req.priority = TaskPriority::Soft;
            req.requiredTuners = settings_.epgRequiredTuners;

            if (Dispatch_(req, lastLightAttemptTick_, L"LIGHT(interval)")) {
                lastLightTick_ = nowTick;
                return;
            }
        }
    }
}