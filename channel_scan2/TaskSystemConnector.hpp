// TaskSystemConnector.hpp
#pragma once
#ifndef TASK_SYSTEM_CONNECTOR_HPP
#define TASK_SYSTEM_CONNECTOR_HPP

#include <Windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <chrono>
#include <cstddef>

#include "TimeKeeper.hpp"
#include "TaskListManager.hpp"
#include "MultiTunerTaskRunner.hpp"
#include "TaskKinds.hpp"
#include "EpgHtmlPortal.hpp"
#include "EpgPortalReservationApiServer.hpp"
#include "AutoReserveRuleStore.hpp"

class IBonDriver2;

// 「予約管理（tasks.ini） + 時刻監視（TimeKeeper） + 実行（Runner）」を束ねる
class TaskSystemConnector final
{
public:
    struct Options {
        // --- Runner ---
        int  tunerCount = 1;
        bool enableUtilityWorker = true;
        int  maxQueueSize = 256;

        // --- TimeKeeper base ---
        // enableAuto=false でも OneShot 監視のために Start() はタイマーを張る（TimeKeeper設計に合わせる）
        bool enableAuto = false;   // periodic（Daily/Hourly/Interval）の可否
        int  bootGraceSeconds = 60;
        bool catchUpOnStart = false;
        int  tickSeconds = 1;
        int  idleRequiredSeconds = 0;

        int enqueueRetryBackoffSeconds = 30;

        std::uint8_t epgRequiredTuners = 1;

        // tasks.ini の既定パス（空なら呼び出し側が LoadTasksFromIni を使う）
        std::wstring tasksIniPath;
    };

    struct Callbacks {
        std::function<void(const std::wstring&)> Log;
        std::function<bool()> CanRun;

        std::function<void(int tunerIndex, IBonDriver2* bon)> OnEpgFull;
        std::function<void(int tunerIndex, IBonDriver2* bon)> OnEpgLight;

        std::function<void(int tunerIndex, IBonDriver2* bon, const TimeKeeper::TaskRequest& req)> OnRecPrepare;
        std::function<void(int tunerIndex, IBonDriver2* bon, const TimeKeeper::TaskRequest& req)> OnRecStart;
        std::function<void(int tunerIndex, IBonDriver2* bon, const TimeKeeper::TaskRequest& req)> OnRecStop;

        // tasks.ini / 予約キャッシュが更新されたとき、UI側へ表示更新を通知する。
        // 通常予約・削除・有効切替・EPG再構築後の再同期で使う。
        std::function<void()> OnReservationDataChanged;
    };

    struct TaskViewRow {
        std::uint32_t kind = 0;
        std::uint32_t instance = 0;
        bool enabled = false;

        bool disabledByUser = false;
        bool excludeFromAuto = false;
        bool autoReserved = false;

        TaskListManager::ScheduleType scheduleType = TaskListManager::ScheduleType::None;
        int dueYmd = 0;
        int dueHHMM = 0;
        int lateToleranceMinutes = -1;

        int dailyHour = 0;
        int dailyMinute = 0;
        int hourlyAtMinute = 0;
        int intervalMinutes = 0;

        bool useTimeWindow = false;
        int windowStartYmd = 0;
        int windowStartHHMM = 0;
        int windowEndYmd = 0;
        int windowEndHHMM = 0;

        std::wstring label;
        std::wstring category;
        std::wstring target;
        std::wstring note;
        std::wstring name;

        std::wstring reserveMode;
        std::wstring reserveSource;
        std::wstring dedupeKey;
        std::wstring excludeKey;

        int endYmd = 0;
        int endHHMM = 0;

        std::uint8_t requiredTuners = 1;
        int preferredTunerIndex = -1;
    };

    struct EpgReservationRequest {
        std::uint32_t kind = TaskKinds::EpgFull;

        int hhmm = 330;                 // config.ini の DailyFullUpdateHHMM を渡す想定
        int windowMinutes = 30;         // EPG用特殊ルール

        // Daily は「毎日の時刻テンプレート」として保存する。
        // 実際に TimeKeeper が起動するのは OneShot 側だけ。
        bool enabledDaily = false;
        bool enabledOneShot = true;

        // EPG取得完了後の再登録では、必ず翌日分の OneShot にする。
        // false の場合は「今日の時刻がまだなら今日、過ぎていれば明日」。
        bool forceNextDayOneShot = false;

        std::uint8_t requiredTuners = 1;
        int preferredTunerIndex = -1;

        std::uint32_t dailyInstance = 0;
        std::uint32_t oneShotInstance = 1;

        std::wstring dailyName = L"EPG Auto Update Template";
        std::wstring oneShotName = L"EPG Full OneShot (Auto)";

        std::wstring dailyLabel = L"EPG自動更新設定";
        std::wstring oneShotLabel = L"EPG取得予約(次回)";

        std::wstring target = L"EPG対象サービス";
        std::wstring dailyNote = L"毎日時刻テンプレート。実行は次回OneShotのみ";
    };

    bool ScheduleEpgReservation(const EpgReservationRequest& req);

    struct ProgramReservationRequest {
        std::uint32_t instance = 0;
        std::wstring title;
        std::wstring serviceName;

        int startYmd = 0;
        int startHHMM = 0;
        int endYmd = 0;
        int endHHMM = 0;

        int prepareMinutes = 1;
        int stopMarginMinutes = 0;

        bool enabled = true;
        std::uint8_t requiredTuners = 1;
        int preferredTunerIndex = -1;
    };

    struct EpgReserveCommandEx
    {
        int space = 0;
        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;
        std::uint16_t serviceId = 0;
        std::uint16_t eventId = 0;

        std::wstring title;
        std::wstring titleLabel;
        std::wstring serviceName;
        std::wstring summary;
        std::wstring genre;

        std::wstring dateText;
        std::wstring weekdayText;
        std::wstring timeText;

        std::wstring startDate;   // YYYY-MM-DD
        std::wstring startTime;   // HH:MM:SS
        std::wstring endDate;
        std::wstring endTime;
        std::uint32_t durationSec = 0;

        std::wstring reserveMode;   // "watch" / "record"
        std::wstring reserveSource; // "epg_portal"
        std::wstring dedupeKey;     // 重複防止キー

        bool enabled = true;
        bool disabledByUser = false;
        bool excludeFromAuto = false;
        bool autoReserved = false;
        std::wstring excludeKey;

        std::uint8_t requiredTuners = 1;
        int preferredTunerIndex = -1;
    };



    struct AutoReserveRecordingApplyOptions
    {
        bool dryRun = false;
        bool allowRegister = true;

        // candidate / auto の区別は登録可否に使わない。
        // 検索条件に一致したものは全部録画予約対象にする。
        bool includeCandidateRules = true;
        bool includeAutoRules = true;

        // 同一検索結果・既存予約との重複を除外する。
        bool skipDuplicate = true;

        // ユーザーが無効化 / 自動予約除外した既存予約は再登録しない。
        bool protectUserBlockedReservation = true;

        // 0なら無制限。
        std::size_t maxRegisterCount = 0;

        std::wstring reserveMode = L"record";
        std::wstring reserveSource = L"auto_reserve";

        // 条件一致したものは録画予約として有効登録する。
        bool forceCreatedReservationEnabled = true;

        std::uint8_t requiredTuners = 1;
        int preferredTunerIndex = -1;
    };

    struct AutoReserveRecordingApplyResult
    {
        bool ok = false;

        std::size_t inputMatches = 0;
        std::size_t candidateInputs = 0;
        std::size_t autoInputs = 0;

        std::size_t targetPrograms = 0;
        std::size_t dryRunSkipped = 0;
        std::size_t registered = 0;
        std::size_t failed = 0;

        std::size_t duplicateSkipped = 0;
        std::size_t userBlockedSkipped = 0;
        std::size_t invalidSkipped = 0;
        std::size_t limitSkipped = 0;
        std::size_t actionSkipped = 0;

        std::wstring message;
    };

    AutoReserveRecordingApplyResult RegisterAutoReserveMatchesAsRecording(
        const AutoReserve::RuleMatchResult& matchResult,
        const AutoReserveRecordingApplyOptions& options);

    AutoReserveRecordingApplyResult RegisterAutoReserveMatchesAsRecordingFixed(
        const AutoReserve::RuleMatchResult& matchResult);



    struct ProgramReservationAdjustRequest
    {
        std::uint32_t instance = 0;

        int space = 0;
        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;
        std::uint16_t serviceId = 0;
        std::uint16_t eventId = 0;

        std::wstring title;
        std::wstring titleLabel;
        std::wstring summary;
        std::wstring genre;

        std::wstring startDate;
        std::wstring startTime;
        std::wstring endDate;
        std::wstring endTime;

        std::uint32_t durationSec = 0;

        bool allowTitleUpdate = true;
        bool allowStartTimeUpdate = true;
        bool allowStopTimeUpdate = true;
    };

    struct EpgUpdateTaskRefreshRequest
    {
        std::wstring source;

        int space = -1;
        int physicalChannel = -1;

        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;
        std::uint16_t serviceId = 0;

        std::wstring epgPath;

        // EPG更新で得られた番組更新候補。
        // 通常EPG更新 / 録画中EIT更新 / TVTest TS EIT更新のどれでもここへ入れる。
        std::vector<ProgramReservationAdjustRequest> programs;

        // true の場合、programs が空でも TimeKeeper / 予約キャッシュ再同期だけ行う。
        bool refreshEvenIfNoProgram = true;

        bool initialBuild = false;
    };

    struct ProgramEventUpdateRequest
    {
        std::wstring source;

        int space = -1;
        int physicalChannel = -1;

        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;
        std::uint16_t serviceId = 0;
        std::uint16_t eventId = 0;

        std::wstring epgPath;

        std::wstring title;
        std::wstring titleLabel;
        std::wstring summary;
        std::wstring genre;

        std::wstring startDate;
        std::wstring startTime;
        std::wstring endDate;
        std::wstring endTime;
        std::uint32_t durationSec = 0;

        bool allowTitleUpdate = true;
        bool allowStartTimeUpdate = true;
        bool allowStopTimeUpdate = true;

        bool initialBuild = false;
    };



    struct EpgUpdateTaskRefreshResult
    {
        bool ok = false;

        std::size_t candidates = 0;
        std::size_t adjusted = 0;
        std::size_t skippedByChannel = 0;
        std::size_t failed = 0;

        bool maintenanceOk = false;
        bool applyOk = false;
        bool saveOk = false;

        std::wstring message;
    };

    // 1番組ぶんのEPG更新を、予約補正系へ流す簡易入口。
    // 最初の照合は channel identifiers + eventId で行う。
    EpgUpdateTaskRefreshResult RefreshProgramReservationByEventId(
        const ProgramEventUpdateRequest& req);

    // EPG更新後のタスク再確認入口。
    // TaskSystemConnector自体はTS受信を知らず、更新候補を受け取ってtasks側だけ更新する。
    EpgUpdateTaskRefreshResult RefreshProgramReservationsAfterEpgUpdate(
        const EpgUpdateTaskRefreshRequest& req);

    bool AdjustProgramReservationFromEit(
        const ProgramReservationAdjustRequest& req);

    bool ScheduleOneShotWindow(std::uint32_t kind, std::uint32_t instance,
        int windowStartYmd, int windowStartHHMM,
        int windowEndYmd, int windowEndHHMM,
        bool enabled = true,
        const std::wstring& name = L"",
        int lateToleranceMinutes = -1);

    bool ScheduleProgramReservation(const ProgramReservationRequest& req);
    bool ScheduleProgramReservation(const EpgReserveCommandEx& cmd);
public:
    TaskSystemConnector() = default;

    bool Initialize(HWND hwnd, UINT_PTR timerId, const Options& opt, const Callbacks& cb);

    // --- lifecycle ---
    bool Start();                  // 監視開始（enableAuto=falseでも OneShot監視のためタイマーは動かす）
    void Stop(bool clearQueue = false);
    void OnTick();                 // WM_TIMER から呼ぶ
    bool IsRunning() const { return running_; }
    bool IsInitialized() const { return initialized_; }

    void SetReservationApiWriteOptions(const EpgHtmlPortal::WriteOptions& opt);
    bool StartReservationApiServer();
    void StopReservationApiServer();

    // --- tuner binding ---
    void SetTuner(int tunerIndex, IBonDriver2* bon);
    void SetTunerAvailable(int tunerIndex, bool available);

    // --- task list access ---
    TaskListManager& GetTaskListManager() { return taskList_; }
    const TaskListManager& GetTaskListManager() const { return taskList_; }

    // tasks.ini を “予約管理の入口” として扱う
    bool LoadTasksFromIni(const std::wstring& path);
    bool SaveTasksToIni(const std::wstring& path) const;

    // 現在の tasks.ini パス（Options で指定した場合）
    const std::wstring& GetTasksIniPath() const { return opt_.tasksIniPath; }

    // タスク表を TimeKeeper に反映（OneShotも含む）
    bool ApplyTaskList();

    // UIから使う：Upsert/Remove -> 反映
    bool UpsertTaskAndApply(const TaskListManager::TaskEntry& entry);
    bool RemoveTaskAndApply(std::uint32_t kind, std::uint32_t instance);
    bool RemoveProgramReservationGroupAndApply(std::uint32_t instance);
    // EPG構築完了後に、終了済み番組予約を整理する。
// 初回構築時は isInitialBuild=true を渡して何もしない。
    bool RunReservationMaintenanceAfterEpgBuild(bool isInitialBuild);

    struct ExternalEpgUpdateSyncResult
    {
        bool ok = false;

        bool loadOk = false;
        bool maintenanceOk = false;
        bool applyOk = false;

        std::wstring tasksIniPath;
        std::wstring message;
    };

    // EpgBuild_GUI / 外部EPG更新処理が tasks.ini を更新した後、
    // TaskSystemConnector 側のメモリ上 taskList_ と TimeKeeper を再同期する。
    ExternalEpgUpdateSyncResult ReloadTasksAfterExternalEpgUpdate(
        bool isInitialBuild);

    bool SetProgramReservationGroupEnabledAndApply(
        std::uint32_t instance,
        bool enabled,
        bool disabledByUser,
        bool excludeFromAuto);

    // 予約API（“専用機能”としてここに寄せる）
    bool ScheduleOneShot(std::uint32_t kind, std::uint32_t instance,
        int dueYmd, int dueHHMM,
        int lateToleranceMinutes,
        bool enabled = true,
        const std::wstring& name = L"");

    bool ScheduleDaily(std::uint32_t kind, std::uint32_t instance,
        int hour, int minute,
        bool enabled = true,
        const std::wstring& name = L"");

    bool ScheduleHourly(std::uint32_t kind, std::uint32_t instance,
        int atMinute,
        bool enabled = true,
        const std::wstring& name = L"");

    bool ScheduleInterval(std::uint32_t kind, std::uint32_t instance,
        int intervalMinutes,
        bool enabled = true,
        const std::wstring& name = L"");

    // 設定系（UIスイッチで叩ける）
    void SetEnableAuto(bool enable);        // periodic のON/OFF（ApplyTaskListで有効化）
    void SetTickSeconds(int sec);           // 次の Start から有効（再Start推奨）
    void SetIdleRequiredSeconds(int sec);   // ApplyTaskListで反映
    void SetBootGraceSeconds(int sec);      // ApplyTaskListで反映

    // 表示用：現在のタスク一覧を取り出す
    std::vector<TaskViewRow> GetTaskViewSnapshot() const;

    struct ManualEpgStartCheckResult
    {
        bool canStart = false;
        std::wstring reason;
        std::wstring conflictName;
        std::uint32_t conflictKind = 0;
        std::uint32_t conflictInstance = 0;
        int conflictYmd = 0;
        int conflictHHMM = -1;
    };

    ManualEpgStartCheckResult CanStartManualEpgAcquisition(
        int estimatedMinutes,
        int guardMinutes) const;

private:
    void Log_(const wchar_t* fmt, ...) const;
    static std::uint64_t MakeMonitorKey_(std::uint32_t kind, std::uint32_t instance) noexcept;
    bool TryMarkTaskDispatching_(std::uint32_t kind, std::uint32_t instance);
    void MarkTaskExecuted_(std::uint32_t kind, std::uint32_t instance);
    void ResetExecutionMonitorForTask_(std::uint32_t kind, std::uint32_t instance);
    void SyncExecutionMonitorWithTaskList_();
    void EmitOverdueTaskWarnings_();

    bool HandleMissedTaskAfterLogged_(const TaskListManager::TaskEntry& t);
    bool HandleMissedEpgOneShotAfterLogged_(const TaskListManager::TaskEntry& missed);
    bool RefreshNextEpgOneShotAfterAcquired_(const TimeKeeper::TaskRequest& r, const wchar_t* reason);

    std::chrono::steady_clock::time_point lastOverdueCheck_{};
    int overdueCheckIntervalSec_ = 15;


private:
    HWND hwnd_ = nullptr;
    UINT_PTR timerId_ = 0;

    Options opt_{};
    Callbacks cb_{};

    TimeKeeper timeKeeper_{};
    TaskListManager taskList_{};
    MultiTunerTaskRunner runner_{};

    bool initialized_ = false;
    bool running_ = false;

    mutable std::mutex executionMonitorMutex_{};
    std::unordered_set<std::uint64_t> executedTaskKeys_{};
    std::unordered_set<std::uint64_t> warnedTaskKeys_{};

    EpgPortalReservationApiServer reservationApiServer_{};
    EpgHtmlPortal::WriteOptions reservationApiWriteOptions_{};
    bool reservationApiWriteOptionsReady_ = false;

    std::wstring ResolveTasksIniPathForSave_() const;

    bool RefreshReservationCaches_(const std::wstring& tasksIniPath) const;
    bool SaveTasksToConfiguredIni_() const;
    void NotifyReservationDataChanged_() const;



    static constexpr DWORD kTasksIniWatchIntervalMs_ = 1000;

    mutable bool tasksIniStampValid_ = false;
    mutable FILETIME tasksIniLastWriteTime_{};
    mutable ULONGLONG tasksIniWatchLastTick_ = 0;

};

#endif // TASK_SYSTEM_CONNECTOR_HPP