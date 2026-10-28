#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Reservation_EitMonitor.hpp"

#include "DebugManager.h"
#include "SystemInfoManager.hpp"
#include "TaskSystemConnector.hpp"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    std::vector<std::wstring> SplitWs_(const std::wstring& s, wchar_t delim)
    {
        std::vector<std::wstring> out;
        std::wstring cur;
        bool inQuote = false;

        for (wchar_t ch : s) {
            if (ch == L'\"') {
                inQuote = !inQuote;
                cur.push_back(ch);
            }
            else if (ch == delim && !inQuote) {
                out.push_back(cur);
                cur.clear();
            }
            else {
                cur.push_back(ch);
            }
        }

        out.push_back(cur);
        return out;
    }

    std::wstring Trim_(std::wstring s)
    {
        auto isWs = [](wchar_t c) noexcept {
            return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\"';
            };

        while (!s.empty() && isWs(s.front())) {
            s.erase(s.begin());
        }

        while (!s.empty() && isWs(s.back())) {
            s.pop_back();
        }

        return s;
    }

    bool ParseUInt16_(const std::wstring& s, std::uint16_t& out) noexcept
    {
        try {
            const unsigned long v = std::stoul(s, nullptr, 0);
            if (v > 0xFFFFul) {
                return false;
            }

            out = static_cast<std::uint16_t>(v);
            return true;
        }
        catch (...) {
            return false;
        }
    }

    bool ParseUInt32_(const std::wstring& s, std::uint32_t& out) noexcept
    {
        try {
            const unsigned long v = std::stoul(s, nullptr, 0);
            out = static_cast<std::uint32_t>(v);
            return true;
        }
        catch (...) {
            return false;
        }
    }

    bool ExtractKeyValue_(const std::wstring& token, std::wstring& key, std::wstring& value)
    {
        const std::size_t pos = token.find(L'=');
        if (pos == std::wstring::npos) {
            return false;
        }

        key = Trim_(token.substr(0, pos));
        value = Trim_(token.substr(pos + 1));
        return !key.empty();
    }

    std::wstring NormalizeDate_(std::wstring s)
    {
        for (wchar_t& ch : s) {
            if (ch == L'/') {
                ch = L'-';
            }
        }

        return s;
    }

    std::wstring NormalizeTime_(std::wstring s)
    {
        if (s.size() >= 8) {
            return s.substr(0, 8);
        }

        if (s.size() >= 5) {
            return s.substr(0, 5) + L":00";
        }

        return s;
    }

    bool SplitDateTime_(const std::wstring& text, std::wstring& outDate, std::wstring& outTime)
    {
        outDate.clear();
        outTime.clear();

        const std::wstring s = Trim_(text);
        if (s.empty()) {
            return false;
        }

        std::size_t pos = s.find(L'T');
        if (pos == std::wstring::npos) {
            pos = s.find(L'_');
        }
        if (pos == std::wstring::npos) {
            pos = s.find(L' ');
        }

        if (pos == std::wstring::npos) {
            return false;
        }

        outDate = NormalizeDate_(s.substr(0, pos));
        outTime = NormalizeTime_(s.substr(pos + 1));

        return !outDate.empty() && !outTime.empty();
    }

    std::wstring SafeTitle_(const TaskListManager::TaskEntry& entry)
    {
        if (!entry.reserve.titleLabel.empty()) {
            return entry.reserve.titleLabel;
        }
        if (!entry.reserve.title.empty()) {
            return entry.reserve.title;
        }
        if (!entry.display.label.empty()) {
            return entry.display.label;
        }

        return entry.name.empty() ? L"(no title)" : entry.name;
    }

    bool SameTextLoose_(std::wstring a, std::wstring b)
    {
        auto normalize = [](std::wstring s) {
            s.erase(
                std::remove_if(
                    s.begin(),
                    s.end(),
                    [](wchar_t ch) {
                        return ch == L' ' ||
                            ch == L'　' ||
                            ch == L'\t' ||
                            ch == L'\r' ||
                            ch == L'\n';
                    }),
                s.end());
            return s;
            };

        return normalize(std::move(a)) == normalize(std::move(b));
    }

    constexpr long long kTvTestRecordDelayStopMinutes_ = 10;

    bool ParseIsoDate_(const std::wstring& text, int& outYmd) noexcept
    {
        outYmd = 0;

        unsigned y = 0;
        unsigned m = 0;
        unsigned d = 0;

        if (swscanf_s(text.c_str(), L"%u-%u-%u", &y, &m, &d) != 3) {
            return false;
        }

        if (y < 1900 || m < 1 || m > 12 || d < 1 || d > 31) {
            return false;
        }

        outYmd = static_cast<int>(y * 10000 + m * 100 + d);
        return true;
    }

    bool ParseIsoTime_(const std::wstring& text, int& outHHMM, int& outSec) noexcept
    {
        outHHMM = -1;
        outSec = 0;

        unsigned hh = 0;
        unsigned mm = 0;
        unsigned ss = 0;

        const int n = swscanf_s(text.c_str(), L"%u:%u:%u", &hh, &mm, &ss);
        if (n < 2) {
            return false;
        }

        if (hh > 47 || mm > 59 || ss > 59) {
            return false;
        }

        outHHMM = static_cast<int>(hh * 100 + mm);
        outSec = static_cast<int>(ss);
        return true;
    }

    bool LocalYmdHHMMSSToUtcFileTime_(
        int ymd,
        int hhmm,
        int sec,
        FILETIME& outUtcFileTime) noexcept
    {
        outUtcFileTime = FILETIME{};

        if (ymd <= 0 || hhmm < 0) {
            return false;
        }

        SYSTEMTIME local{};
        local.wYear = static_cast<WORD>(ymd / 10000);
        local.wMonth = static_cast<WORD>((ymd / 100) % 100);
        local.wDay = static_cast<WORD>(ymd % 100);
        local.wHour = static_cast<WORD>(hhmm / 100);
        local.wMinute = static_cast<WORD>(hhmm % 100);
        local.wSecond = static_cast<WORD>((std::clamp)(sec, 0, 59));
        local.wMilliseconds = 0;

        SYSTEMTIME utc{};
        if (!::TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) {
            return false;
        }

        return ::SystemTimeToFileTime(&utc, &outUtcFileTime) != 0;
    }

    ULONGLONG FileTimeToU64_(const FILETIME& ft) noexcept
    {
        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return u.QuadPart;
    }

    bool TryIsoDateTimeToFileTime_(
        const std::wstring& date,
        const std::wstring& time,
        FILETIME& outFileTime) noexcept
    {
        outFileTime = FILETIME{};

        int ymd = 0;
        int hhmm = -1;
        int sec = 0;

        if (!ParseIsoDate_(date, ymd)) {
            return false;
        }

        if (!ParseIsoTime_(time, hhmm, sec)) {
            return false;
        }

        return LocalYmdHHMMSSToUtcFileTime_(
            ymd,
            hhmm,
            sec,
            outFileTime);
    }

    bool TryCalcStartDelayMinutes_(
        const TaskListManager::TaskEntry& entry,
        const std::wstring& observedStartDate,
        const std::wstring& observedStartTime,
        long long& outDelayMinutes) noexcept
    {
        outDelayMinutes = 0;

        if (entry.reserve.startDate.empty() ||
            entry.reserve.startTime.empty() ||
            observedStartDate.empty() ||
            observedStartTime.empty()) {
            return false;
        }

        FILETIME targetFt{};
        FILETIME observedFt{};

        if (!TryIsoDateTimeToFileTime_(
            entry.reserve.startDate,
            entry.reserve.startTime,
            targetFt)) {
            return false;
        }

        if (!TryIsoDateTimeToFileTime_(
            observedStartDate,
            observedStartTime,
            observedFt)) {
            return false;
        }

        const ULONGLONG targetValue = FileTimeToU64_(targetFt);
        const ULONGLONG observedValue = FileTimeToU64_(observedFt);

        constexpr ULONGLONG kMinute100ns =
            60ULL * 1000ULL * 1000ULL * 10ULL;

        if (observedValue >= targetValue) {
            outDelayMinutes =
                static_cast<long long>((observedValue - targetValue) / kMinute100ns);
        }
        else {
            outDelayMinutes =
                -static_cast<long long>((targetValue - observedValue) / kMinute100ns);
        }

        return true;
    }

    bool IsPastReserveStartTime_(
        const TaskListManager::TaskEntry& entry,
        DWORD graceSeconds = 30) noexcept
    {
        if (entry.reserve.startDate.empty() ||
            entry.reserve.startTime.empty()) {
            // 開始時刻が取れない場合は従来通り判定する。
            return true;
        }

        FILETIME startFt{};
        if (!TryIsoDateTimeToFileTime_(
            entry.reserve.startDate,
            entry.reserve.startTime,
            startFt)) {
            return true;
        }

        FILETIME nowFt{};
        ::GetSystemTimeAsFileTime(&nowFt);

        const ULONGLONG startValue = FileTimeToU64_(startFt);
        const ULONGLONG nowValue = FileTimeToU64_(nowFt);

        constexpr ULONGLONG kSecond100ns =
            1000ULL * 1000ULL * 10ULL;

        const ULONGLONG grace =
            static_cast<ULONGLONG>(graceSeconds) * kSecond100ns;

        // 予約開始時刻 + graceSeconds を過ぎてから不一致判定を有効にする。
        return nowValue >= (startValue + grace);
    }

    bool HasTvTestProgramPayload_(
        const Reservation_EitMonitor::Snapshot& snap) noexcept
    {
        return snap.eventIdNow != 0 ||
            snap.eventIdNext != 0 ||
            !snap.eventNameNow.empty() ||
            !snap.eventNameNext.empty() ||
            !snap.startDateNow.empty() ||
            !snap.startTimeNow.empty() ||
            !snap.startDateNext.empty() ||
            !snap.startTimeNext.empty();
    }
}// namespace

Reservation_EitMonitor::~Reservation_EitMonitor() noexcept
{
    Reset();
}

void Reservation_EitMonitor::Configure(const Reservation_ExecutionSettings& settings)
{
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = settings;
}

void Reservation_EitMonitor::Reset() noexcept
{
    std::unordered_map<std::uint32_t, std::shared_ptr<RecordSession>> sessions;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions.swap(recordSessions_);
    }

    for (auto& kv : sessions) {
        if (kv.second) {
            kv.second->stopRequested.store(true, std::memory_order_release);
        }
    }

    for (auto& kv : sessions) {
        auto& session = kv.second;
        if (!session || !session->worker.joinable()) {
            continue;
        }

        try {
            if (session->worker.get_id() == std::this_thread::get_id()) {
                session->worker.detach();
            }
            else {
                session->worker.join();
            }
        }
        catch (...) {
        }
    }
}

void Reservation_EitMonitor::StartWatchMonitor(
    std::uint32_t instance,
    const TaskListManager::TaskEntry& entry,
    Reservation_TvTestController& controller)
{
    StartWatchMonitor(instance, entry, controller, nullptr);
}

void Reservation_EitMonitor::StartWatchMonitor(
    std::uint32_t instance,
    const TaskListManager::TaskEntry& entry,
    Reservation_TvTestController& controller,
    TaskSystemConnector* taskSystem)
{
    Reservation_ExecutionSettings settings{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings = settings_;
    }

    if (!settings.enableEitMonitorDuringWatch) {
        return;
    }

    SystemInfoLogInfo(
        L"視聴中EIT照合を開始します。instance=%u title=%ls service=%ls",
        static_cast<unsigned>(instance),
        SafeTitle_(entry).c_str(),
        entry.reserve.serviceName.c_str());

    const Snapshot snap = QueryOnce(controller);

    CompareAndLog_(
        instance,
        entry,
        snap,
        false,
        taskSystem,
        {},
        nullptr,
        nullptr);
}

void Reservation_EitMonitor::StopWatchMonitor(std::uint32_t instance)
{
    Reservation_ExecutionSettings settings{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings = settings_;
    }

    if (settings.enableEitMonitorDuringWatch) {
        SystemInfoLogInfo(
            L"視聴中EIT照合を終了しました。instance=%u",
            static_cast<unsigned>(instance));
    }
}

void Reservation_EitMonitor::StartRecordMonitor(
    std::uint32_t instance,
    const TaskListManager::TaskEntry& entry,
    QueryCallback query,
    TaskSystemConnector* taskSystem,
    NeedStopCallback onNeedStop)
{
    if (instance == 0 || !query) {
        return;
    }

    /*
    Reservation_ExecutionSettings settings{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings = settings_;
    }

    // 現段階では録画専用フラグを増やさず、
    // 視聴中EIT照合フラグをTVTest EIT照合の共通スイッチとして使う。
    if (!settings.enableEitMonitorDuringWatch) {
        return;
    }
*/
    StopRecordMonitor(instance);

    auto session = std::make_shared<RecordSession>();
    session->instance = instance;
    session->entry = entry;
    session->query = std::move(query);
    session->taskSystem = taskSystem;
    session->onNeedStop = std::move(onNeedStop);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        recordSessions_[instance] = session;
    }

    try {
        session->worker = std::thread(
            [this, session]() noexcept {
                RecordLoop_(session);
            });
    }
    catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        recordSessions_.erase(instance);
        SystemInfoLogWarn(
            L"録画中EIT監視スレッドの開始に失敗しました。instance=%u",
            static_cast<unsigned>(instance));
        return;
    }

    SystemInfoLogInfo(
        L"録画中EIT監視を開始しました。instance=%u title=%ls service=%ls",
        static_cast<unsigned>(instance),
        SafeTitle_(entry).c_str(),
        entry.reserve.serviceName.c_str());
}

void Reservation_EitMonitor::StopRecordMonitor(std::uint32_t instance)
{
    std::shared_ptr<RecordSession> session;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = recordSessions_.find(instance);
        if (it == recordSessions_.end()) {
            return;
        }

        session = it->second;
        recordSessions_.erase(it);
    }

    if (session) {
        session->stopRequested.store(true, std::memory_order_release);

        if (session->worker.joinable()) {
            try {
                if (session->worker.get_id() == std::this_thread::get_id()) {
                    session->worker.detach();
                }
                else {
                    session->worker.join();
                }
            }
            catch (...) {
            }
        }
    }

    SystemInfoLogInfo(
        L"録画中EIT監視を終了しました。instance=%u",
        static_cast<unsigned>(instance));
}

Reservation_EitMonitor::Snapshot Reservation_EitMonitor::QueryOnce(
    Reservation_TvTestController& controller) const
{
    Snapshot snap{};
    const auto r = controller.QueryProgram();

    snap.ok = r.ok;
    snap.rawLine = r.ok ? r.response : r.reason;

    if (!r.ok) {
        return snap;
    }

    const auto tokens = SplitWs_(r.response, L' ');

    for (const auto& token : tokens) {
        std::wstring k;
        std::wstring v;

        if (!ExtractKeyValue_(token, k, v)) {
            continue;
        }

        std::transform(k.begin(), k.end(), k.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
            });

        if (k == L"nid" || k == L"networkid" || k == L"network_id") {
            ParseUInt16_(v, snap.networkId);
        }
        else if (k == L"tsid" || k == L"transportstreamid" || k == L"ts_id") {
            ParseUInt16_(v, snap.tsId);
        }
        else if (k == L"sid" || k == L"serviceid" || k == L"service_id") {
            ParseUInt16_(v, snap.serviceId);
        }
        else if (k == L"eid" ||
            k == L"eid_now" ||
            k == L"eventid" ||
            k == L"event_id_now") {
            ParseUInt16_(v, snap.eventIdNow);
        }
        else if (k == L"eid_next" ||
            k == L"eventidnext" ||
            k == L"event_id_next") {
            ParseUInt16_(v, snap.eventIdNext);
        }
        else if (k == L"name_now" ||
            k == L"eventname_now" ||
            k == L"event_now") {
            snap.eventNameNow = v;
        }
        else if (k == L"name_next" ||
            k == L"eventname_next" ||
            k == L"event_next") {
            snap.eventNameNext = v;
        }
        else if (k == L"start_now" ||
            k == L"startdatetime_now" ||
            k == L"start_time_now") {
            (void)SplitDateTime_(v, snap.startDateNow, snap.startTimeNow);
        }
        else if (k == L"end_now" ||
            k == L"enddatetime_now" ||
            k == L"end_time_now") {
            (void)SplitDateTime_(v, snap.endDateNow, snap.endTimeNow);
        }
        else if (k == L"duration_now" ||
            k == L"durationsec_now" ||
            k == L"duration_sec_now") {
            ParseUInt32_(v, snap.durationSecNow);
        }
        else if (k == L"start_next" ||
            k == L"startdatetime_next" ||
            k == L"start_time_next") {
            (void)SplitDateTime_(v, snap.startDateNext, snap.startTimeNext);
        }
        else if (k == L"end_next" ||
            k == L"enddatetime_next" ||
            k == L"end_time_next") {
            (void)SplitDateTime_(v, snap.endDateNext, snap.endTimeNext);
        }
        else if (k == L"duration_next" ||
            k == L"durationsec_next" ||
            k == L"duration_sec_next") {
            ParseUInt32_(v, snap.durationSecNext);
        }
    }

    return snap;
}

void Reservation_EitMonitor::CompareAndLog_(
    std::uint32_t instance,
    const TaskListManager::TaskEntry& entry,
    const Snapshot& snap,
    bool recordMode,
    TaskSystemConnector* taskSystem,
    const NeedStopCallback& onNeedStop,
    std::atomic_bool* stopIssued,
    RecordSession* recordSession) const
{
    const wchar_t* modeText = recordMode ? L"録画" : L"視聴";

    if (!snap.ok) {
        if (recordSession == nullptr ||
            !recordSession->queryFailureLogged.exchange(true, std::memory_order_acq_rel)) {
            SystemInfoLogWarn(
                L"%ls中EIT照合に失敗しました。instance=%u response=%ls",
                modeText,
                static_cast<unsigned>(instance),
                snap.rawLine.c_str());
        }
        return;
    }

    const bool serviceMatch =
        (entry.reserve.networkId == 0 ||
            snap.networkId == 0 ||
            entry.reserve.networkId == snap.networkId) &&
        (entry.reserve.tsId == 0 ||
            snap.tsId == 0 ||
            entry.reserve.tsId == snap.tsId) &&
        (entry.reserve.serviceId == 0 ||
            snap.serviceId == 0 ||
            entry.reserve.serviceId == snap.serviceId);

    if (!serviceMatch) {
        if (recordSession == nullptr ||
            !recordSession->serviceMismatchLogged.exchange(true, std::memory_order_acq_rel)) {
            SystemInfoLogWarn(
                L"%ls中EIT照合でサービスが一致しません。instance=%u reserve(nid=%u tsid=%u sid=%u) tvtest(nid=%u tsid=%u sid=%u)",
                modeText,
                static_cast<unsigned>(instance),
                static_cast<unsigned>(entry.reserve.networkId),
                static_cast<unsigned>(entry.reserve.tsId),
                static_cast<unsigned>(entry.reserve.serviceId),
                static_cast<unsigned>(snap.networkId),
                static_cast<unsigned>(snap.tsId),
                static_cast<unsigned>(snap.serviceId));
        }
        return;
    }

    const bool nowMatch =
        entry.reserve.eventId != 0 &&
        entry.reserve.eventId == snap.eventIdNow;

    const bool nextMatch =
        entry.reserve.eventId != 0 &&
        entry.reserve.eventId == snap.eventIdNext;

    const bool pastReserveStart =
        IsPastReserveStartTime_(entry, 30);

    // 録画中は、予約開始時刻を過ぎるまで不一致判定を行わない。
    // TVTestのPROGRAMは切替直後に eid_now=0/eid_next=0 を返すことがあるため、
    // 開始前や開始直後の空応答で誤警告・誤停止しないようにする。
    if (recordMode && !pastReserveStart) {
        if (nextMatch) {
            DebugLogFormat(
                L"[ReservationEitMonitor][PENDING-BEFORE-START] inst=%u reserveEid=%u now=%u next=%u nextTitle=%ls raw=%ls",
                static_cast<unsigned>(instance),
                static_cast<unsigned>(entry.reserve.eventId),
                static_cast<unsigned>(snap.eventIdNow),
                static_cast<unsigned>(snap.eventIdNext),
                snap.eventNameNext.empty() ? L"(unknown)" : snap.eventNameNext.c_str(),
                snap.rawLine.empty() ? L"(empty)" : snap.rawLine.c_str());
        }
        else {
            DebugLogFormat(
                L"[ReservationEitMonitor][WAIT-GRACE-BEFORE-JUDGE] inst=%u reserveEid=%u now=%u next=%u payload=%d raw=%ls",
                static_cast<unsigned>(instance),
                static_cast<unsigned>(entry.reserve.eventId),
                static_cast<unsigned>(snap.eventIdNow),
                static_cast<unsigned>(snap.eventIdNext),
                HasTvTestProgramPayload_(snap) ? 1 : 0,
                snap.rawLine.empty() ? L"(empty)" : snap.rawLine.c_str());
        }

        return;
    }

    // 開始時刻を過ぎてもTVTest側の番組情報が空なら、まだ不一致扱いにしない。
    // ただし、これは一時待ちなので警告ではなくdebugのみ。
    if (recordMode && !HasTvTestProgramPayload_(snap)) {
        DebugLogFormat(
            L"[ReservationEitMonitor][PROGRAM-PENDING-AFTER-START] inst=%u sid=%u reserveEid=%u raw=%ls",
            static_cast<unsigned>(instance),
            static_cast<unsigned>(snap.serviceId),
            static_cast<unsigned>(entry.reserve.eventId),
            snap.rawLine.empty() ? L"(empty)" : snap.rawLine.c_str());

        return;
    }

    if (nowMatch) {
        DebugLogFormat(
            L"[ReservationEitMonitor][MATCH-NOW] mode=%ls inst=%u eventId=%u title=%ls",
            modeText,
            static_cast<unsigned>(instance),
            static_cast<unsigned>(entry.reserve.eventId),
            SafeTitle_(entry).c_str());

        if (taskSystem != nullptr) {
            const bool titleChanged =
                !snap.eventNameNow.empty() &&
                !SameTextLoose_(snap.eventNameNow, SafeTitle_(entry));

            const bool endChanged =
                !snap.endDateNow.empty() &&
                !snap.endTimeNow.empty() &&
                (entry.reserve.endDate != snap.endDateNow ||
                    entry.reserve.endTime != snap.endTimeNow);

            if (titleChanged || endChanged) {
                bool allowAdjust = true;

                if (recordMode && recordSession != nullptr) {
                    bool expected = false;
                    allowAdjust =
                        recordSession->nowAdjustLogged.compare_exchange_strong(
                            expected,
                            true,
                            std::memory_order_acq_rel);
                }

                if (!allowAdjust) {
                    std::uint32_t skipCount = 0;

                    if (recordSession != nullptr) {
                        skipCount =
                            recordSession->nowAdjustSkipCount.fetch_add(
                                1,
                                std::memory_order_relaxed);
                    }

                    // 補正済み後のskipは初回だけ出す。
                    // MATCH-NOW自体は継続して出るので、動作確認性は維持できる。
                    if (skipCount == 0) {
                        DebugLogFormat(
                            L"[ReservationEitMonitor][NOW-ADJUST-SKIP-ONCE] inst=%u titleChanged=%d endChanged=%d nowTitle=%ls end=%ls %ls",
                            static_cast<unsigned>(instance),
                            titleChanged ? 1 : 0,
                            endChanged ? 1 : 0,
                            snap.eventNameNow.empty() ? L"(empty)" : snap.eventNameNow.c_str(),
                            snap.endDateNow.empty() ? L"(empty)" : snap.endDateNow.c_str(),
                            snap.endTimeNow.empty() ? L"(empty)" : snap.endTimeNow.c_str());
                    }

                    return;
                }

                TaskSystemConnector::ProgramReservationAdjustRequest ar{};
                ar.instance = instance;
                ar.space = entry.reserve.space;
                ar.networkId = snap.networkId;
                ar.tsId = snap.tsId;
                ar.serviceId = snap.serviceId;
                ar.eventId = entry.reserve.eventId;

                ar.title = titleChanged ? snap.eventNameNow : L"";
                ar.titleLabel = ar.title;

                ar.endDate = snap.endDateNow;
                ar.endTime = snap.endTimeNow;
                ar.durationSec = snap.durationSecNow;

                ar.allowTitleUpdate = titleChanged;
                ar.allowStartTimeUpdate = false;
                ar.allowStopTimeUpdate = endChanged;

                const bool adjusted =
                    taskSystem->AdjustProgramReservationFromEit(ar);

                SystemInfoLogWarn(
                    L"%ls中EIT情報により予約情報を補正しました。instance=%u adjusted=%d titleChanged=%d endChanged=%d",
                    modeText,
                    static_cast<unsigned>(instance),
                    adjusted ? 1 : 0,
                    titleChanged ? 1 : 0,
                    endChanged ? 1 : 0);
            }
        }
        

        return;
    }

    if (recordMode && nextMatch) {
        long long startDelayMinutes = 0;
        const bool hasStartDelay =
            TryCalcStartDelayMinutes_(
                entry,
                snap.startDateNext,
                snap.startTimeNext,
                startDelayMinutes);

        const std::wstring delayText =
            hasStartDelay
            ? std::to_wstring(startDelayMinutes)
            : std::wstring(L"(unknown)");

        const bool shouldStopAndReschedule =
            hasStartDelay &&
            startDelayMinutes >= kTvTestRecordDelayStopMinutes_;

        if (shouldStopAndReschedule) {
            if (recordSession == nullptr ||
                !recordSession->nextLongDelayLogged.exchange(true, std::memory_order_acq_rel)) {
                SystemInfoLogWarn(
                    L"録画中EIT照合で予約番組が現在番組ではなく次番組にあり、開始時刻が10分以上後ろへズレているため録画停止と予約時刻補正を行います。instance=%u reserveEid=%u nowEid=%u nextEid=%u delayMin=%ls start=%ls %ls end=%ls %ls",
                    static_cast<unsigned>(instance),
                    static_cast<unsigned>(entry.reserve.eventId),
                    static_cast<unsigned>(snap.eventIdNow),
                    static_cast<unsigned>(snap.eventIdNext),
                    delayText.c_str(),
                    snap.startDateNext.empty() ? L"(unknown)" : snap.startDateNext.c_str(),
                    snap.startTimeNext.empty() ? L"(unknown)" : snap.startTimeNext.c_str(),
                    snap.endDateNext.empty() ? L"(unknown)" : snap.endDateNext.c_str(),
                    snap.endTimeNext.empty() ? L"(unknown)" : snap.endTimeNext.c_str());
            }

            bool allowLongDelayAdjust = true;

            if (recordSession != nullptr) {
                bool expected = false;
                allowLongDelayAdjust =
                    recordSession->nextLongDelayAdjusted.compare_exchange_strong(
                        expected,
                        true,
                        std::memory_order_acq_rel);
            }

            bool adjusted = false;

            if (allowLongDelayAdjust &&
                taskSystem != nullptr &&
                !snap.startDateNext.empty() &&
                !snap.startTimeNext.empty() &&
                !snap.endDateNext.empty() &&
                !snap.endTimeNext.empty()) {
                TaskSystemConnector::ProgramReservationAdjustRequest ar{};
                ar.instance = instance;
                ar.space = entry.reserve.space;
                ar.networkId = snap.networkId;
                ar.tsId = snap.tsId;
                ar.serviceId = snap.serviceId;
                ar.eventId = entry.reserve.eventId;

                ar.title = snap.eventNameNext;
                ar.titleLabel = snap.eventNameNext;

                ar.startDate = snap.startDateNext;
                ar.startTime = snap.startTimeNext;
                ar.endDate = snap.endDateNext;
                ar.endTime = snap.endTimeNext;
                ar.durationSec = snap.durationSecNext;

                ar.allowTitleUpdate = !snap.eventNameNext.empty();
                ar.allowStartTimeUpdate = true;
                ar.allowStopTimeUpdate = true;

                adjusted =
                    taskSystem->AdjustProgramReservationFromEit(ar);

                SystemInfoLogWarn(
                    L"録画中EIT情報により録画開始・停止時刻の補正を試みました。instance=%u adjusted=%d start=%ls %ls end=%ls %ls",
                    static_cast<unsigned>(instance),
                    adjusted ? 1 : 0,
                    ar.startDate.c_str(),
                    ar.startTime.c_str(),
                    ar.endDate.c_str(),
                    ar.endTime.c_str());
            }
            else if (!allowLongDelayAdjust) {
                std::uint32_t skipCount = 0;

                if (recordSession != nullptr) {
                    skipCount =
                        recordSession->nextLongDelayAdjustSkipCount.fetch_add(
                            1,
                            std::memory_order_relaxed);
                }

                if (skipCount == 0) {
                    DebugLogFormat(
                        L"[ReservationEitMonitor][LONG-DELAY-ADJUST-SKIP-ONCE] inst=%u reserveEid=%u nowEid=%u nextEid=%u delayMin=%ls",
                        static_cast<unsigned>(instance),
                        static_cast<unsigned>(entry.reserve.eventId),
                        static_cast<unsigned>(snap.eventIdNow),
                        static_cast<unsigned>(snap.eventIdNext),
                        delayText.c_str());
                }
            }

            DebugLogFormat(
                L"[ReservationEitMonitor][DELAY-STOP-RESCHEDULE] inst=%u delayMin=%ls adjusted=%d reserveEid=%u nowEid=%u nextEid=%u",
                static_cast<unsigned>(instance),
                delayText.c_str(),
                adjusted ? 1 : 0,
                static_cast<unsigned>(entry.reserve.eventId),
                static_cast<unsigned>(snap.eventIdNow),
                static_cast<unsigned>(snap.eventIdNext));

            if (onNeedStop && stopIssued != nullptr) {
                bool expected = false;
                if (stopIssued->compare_exchange_strong(expected, true)) {
                    onNeedStop(instance);
                }
            }

            return;
        }

        if (recordSession == nullptr ||
            !recordSession->nextShortDelayLogged.exchange(true, std::memory_order_acq_rel)) {
            SystemInfoLogWarn(
                L"録画中EIT照合で予約番組が現在番組ではなく次番組にあります。10分未満のズレとして録画は継続し、停止時刻のみ補正します。instance=%u reserveEid=%u nowEid=%u nextEid=%u delayMin=%ls start=%ls %ls end=%ls %ls",
                static_cast<unsigned>(instance),
                static_cast<unsigned>(entry.reserve.eventId),
                static_cast<unsigned>(snap.eventIdNow),
                static_cast<unsigned>(snap.eventIdNext),
                delayText.c_str(),
                snap.startDateNext.empty() ? L"(unknown)" : snap.startDateNext.c_str(),
                snap.startTimeNext.empty() ? L"(unknown)" : snap.startTimeNext.c_str(),
                snap.endDateNext.empty() ? L"(unknown)" : snap.endDateNext.c_str(),
                snap.endTimeNext.empty() ? L"(unknown)" : snap.endTimeNext.c_str());
        }

        bool allowShortDelayAdjust = true;

        if (recordSession != nullptr) {
            bool expected = false;
            allowShortDelayAdjust =
                recordSession->nextShortDelayAdjusted.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel);
        }

        if (!allowShortDelayAdjust) {
            std::uint32_t skipCount = 0;

            if (recordSession != nullptr) {
                skipCount =
                    recordSession->nextShortDelayAdjustSkipCount.fetch_add(
                        1,
                        std::memory_order_relaxed);
            }

            if (skipCount == 0) {
                DebugLogFormat(
                    L"[ReservationEitMonitor][SHORT-DELAY-ADJUST-SKIP-ONCE] inst=%u reserveEid=%u nowEid=%u nextEid=%u delayMin=%ls",
                    static_cast<unsigned>(instance),
                    static_cast<unsigned>(entry.reserve.eventId),
                    static_cast<unsigned>(snap.eventIdNow),
                    static_cast<unsigned>(snap.eventIdNext),
                    delayText.c_str());
            }

            return;
        }

        if (taskSystem != nullptr &&
            !snap.endDateNext.empty() &&
            !snap.endTimeNext.empty()) {
            TaskSystemConnector::ProgramReservationAdjustRequest ar{};
            ar.instance = instance;
            ar.space = entry.reserve.space;
            ar.networkId = snap.networkId;
            ar.tsId = snap.tsId;
            ar.serviceId = snap.serviceId;
            ar.eventId = entry.reserve.eventId;

            ar.title = snap.eventNameNext;
            ar.titleLabel = snap.eventNameNext;

            ar.startDate.clear();
            ar.startTime.clear();

            ar.endDate = snap.endDateNext;
            ar.endTime = snap.endTimeNext;
            ar.durationSec = snap.durationSecNext;

            ar.allowTitleUpdate = !snap.eventNameNext.empty();
            ar.allowStartTimeUpdate = false;
            ar.allowStopTimeUpdate = true;

            const bool adjusted =
                taskSystem->AdjustProgramReservationFromEit(ar);

            SystemInfoLogWarn(
                L"録画中EIT情報により停止時刻のみ補正を試みました。instance=%u adjusted=%d end=%ls %ls",
                static_cast<unsigned>(instance),
                adjusted ? 1 : 0,
                ar.endDate.c_str(),
                ar.endTime.c_str());
        }

        return;
    }

    if (recordSession == nullptr ||
        !recordSession->mismatchLogged.exchange(true, std::memory_order_acq_rel)) {
        SystemInfoLogWarn(
            L"%ls中EIT照合で予約番組と現在/次番組が一致しません。instance=%u reserve(nid=%u tsid=%u sid=%u eid=%u title=%ls) tvtest(nid=%u tsid=%u sid=%u now=%u next=%u raw=%ls)",
            modeText,
            static_cast<unsigned>(instance),
            static_cast<unsigned>(entry.reserve.networkId),
            static_cast<unsigned>(entry.reserve.tsId),
            static_cast<unsigned>(entry.reserve.serviceId),
            static_cast<unsigned>(entry.reserve.eventId),
            SafeTitle_(entry).c_str(),
            static_cast<unsigned>(snap.networkId),
            static_cast<unsigned>(snap.tsId),
            static_cast<unsigned>(snap.serviceId),
            static_cast<unsigned>(snap.eventIdNow),
            static_cast<unsigned>(snap.eventIdNext),
            snap.rawLine.c_str());
    }

    // now/nextどちらにも予約番組がない場合は、TVTest側でも停止は慎重にする。
    // 誤停止を避けるため、現在番組EIDが取れている場合のみ停止要求。
    // ここに到達する時点で、録画中は予約開始時刻+猶予秒を過ぎている。
    if (recordMode && onNeedStop && stopIssued != nullptr && snap.eventIdNow != 0) {
        bool expected = false;
        if (stopIssued->compare_exchange_strong(expected, true)) {
            onNeedStop(instance);
        }
    }
}

void Reservation_EitMonitor::RecordLoop_(
    const std::shared_ptr<RecordSession>& session) noexcept
{
    if (!session) {
        return;
    }

    // 現段階では固定10秒。
    // 予約発火監視とは別なので、ここは高頻度にしない。
    constexpr int kIntervalSec = 10;

    while (!session->stopRequested.load(std::memory_order_acquire)) {
        try {
            const Snapshot snap = session->query();

            CompareAndLog_(
                session->instance,
                session->entry,
                snap,
                true,
                session->taskSystem,
                session->onNeedStop,
                &session->stopIssuedByMonitor,
                session.get());
        }
        catch (...) {
            DebugLogFormat(
                L"[ReservationEitMonitor][RECORD][EXCEPTION] inst=%u",
                static_cast<unsigned>(session->instance));
        }

        if (session->stopIssuedByMonitor.load(std::memory_order_acquire)) {
            session->stopRequested.store(true, std::memory_order_release);
            break;
        }

        for (int i = 0; i < kIntervalSec * 10; ++i) {
            if (session->stopRequested.load(std::memory_order_acquire)) {
                return;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}