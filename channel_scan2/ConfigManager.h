#pragma once
#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <string>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>
#include <string_view>

class ConfigManager {
public:
    static ConfigManager& GetInstance();

    // ---- Tuner limits ----
// Count は config.ini の [Tuners] Count をそのまま管理チューナー数として扱う。
// 同時使用数の抑制はここでは持たず、EnableX / UseRoleX / AllowParallelX / 予約側の確保処理で制御する。
    static constexpr int kMinTuners = 1;

    void LoadConfig();

    // ---- BonDriver ----
// 本体用 [BonDriver] Path。
// チャンネルスキャンなど、チューナー番号を使わない処理で使う。
    std::wstring GetBonDriverPath() const;
    void SetBonDriverPath(const std::wstring& path);

    // 地上波用 BonDriver。
    // config.ini:
    //   [BonDriver] Terrestrial
    std::wstring GetBonDriverTerrestrialPath() const;
    void SetBonDriverTerrestrialPath(const std::wstring& path);

    // 衛星用 BonDriver。
    // config.ini:
    //   [BonDriver] Satellite
    // space=1/2 で使用する。
    std::wstring GetBonDriverSatellitePath() const;
    void SetBonDriverSatellitePath(const std::wstring& path);

    // 予備 fallback。
    // config.ini:
    //   [BonDriver] Default
    std::wstring GetBonDriverDefaultPath() const;
    void SetBonDriverDefaultPath(const std::wstring& path);

    // space から BonDriver を解決する。
    //
    // space=0:
    //   Terrestrial -> Path -> Default
    //
    // space=1/2:
    //   Satellite
    //
    // unknown:
    //   Default -> Path
    //
    // 衛星用が空の場合、地上波BonDriverへ勝手に落とさない。
    std::wstring GetBonDriverPathForSpace(int space) const;

    // 明示名版。本体用 [BonDriver] Path を返す。
    std::wstring GetMainBonDriverPath() const;

    // チャンネルスキャン専用。
    // 旧互換の引数なし版。DefaultSpace を使う。
    std::wstring GetChannelScanBonDriverPath() const;

    // チャンネルスキャン専用。
    // space指定版。今後はこちらを使う。
    std::wstring GetChannelScanBonDriverPathForSpace(int space) const;

    // チューナー番号付き設定。
    // [Tuner0] BonDriverPath0 / [Tuner1] BonDriverPath1 を返す。
    std::wstring GetBonDriverPathForTuner(int tunerIndex) const;

    // 旧互換・手動 fallback 用。
// 内部録画 / EPG更新 / 予約実行では GetMultiTunerBonDriverPath() を使うこと。
// この関数は [TunerX] BonDriverPathX -> CommonBonDriverPath -> [BonDriver] Path の順で返す。
    std::wstring GetBonDriverPathForTunerOrDefault(int tunerIndex) const;

    // EPG更新・録画・予約実行などの多重責務用。
    // 呼び出し側は基本的にこれを使う。
    std::wstring GetMultiTunerBonDriverPath(int tunerIndex) const;

    void SetBonDriverPathForTuner(int tunerIndex, const std::wstring& path);

    // 内部機能用 BonDriver。
// 内部録画 / EPG更新 / 予約実行 / アプリ本体が LoadLibrary する用途。
    std::wstring GetTunerInternalBonDriverPath(int tunerIndex) const;
    void SetTunerInternalBonDriverPath(int tunerIndex, const std::wstring& path);

    // TVTest 起動用 BonDriver。
    // TVTest.exe 起動時の /d に渡す用途。
    std::wstring GetTunerTvTestBonDriverPath(int tunerIndex) const;
    void SetTunerTvTestBonDriverPath(int tunerIndex, const std::wstring& path);

    // ---- Debug ----
    bool GetDebugWriteToFile() const;
    void SetDebugWriteToFile(bool enable);

    std::wstring GetLogEncoding() const;
    void SetLogEncoding(const std::wstring& encoding);

    bool GetEnableSDTDebug() const noexcept { return enableSDTDebug; }
    void SetEnableSDTDebug(bool enable) noexcept { enableSDTDebug = enable; }

    bool GetEnableNITDebug() const noexcept { return enableNITDebug; }
    void SetEnableNITDebug(bool enable) noexcept { enableNITDebug = enable; }

    bool GetGuiLogEnabled() const noexcept { return guiLogEnabled; }
    void SetGuiLogEnabled(bool enable) noexcept { guiLogEnabled = enable; }

    std::wstring GetGuiLogFile() const;
    void SetGuiLogFile(const std::wstring& path);

    // ---- Idle monitor ----
    int GetIdleMonitorSampleMs() const noexcept;
    void SetIdleMonitorSampleMs(int v) noexcept;

    bool GetIdleMonitorConsoleLog() const noexcept;
    void SetIdleMonitorConsoleLog(bool enable) noexcept;

    int GetIdleMonitorSystemCpuPct() const noexcept;
    void SetIdleMonitorSystemCpuPct(int v) noexcept;

    int GetIdleMonitorProcessCpuPct() const noexcept;
    void SetIdleMonitorProcessCpuPct(int v) noexcept;

    int GetIdleMonitorReadKBps() const noexcept;
    void SetIdleMonitorReadKBps(int v) noexcept;

    int GetIdleMonitorWriteKBps() const noexcept;
    void SetIdleMonitorWriteKBps(int v) noexcept;

    int GetIdleMonitorMemoryLoadPct() const noexcept;
    void SetIdleMonitorMemoryLoadPct(int v) noexcept;

    // ---- CPU ----
    std::wstring GetSelectedCPUFeature() const;
    void SetSelectedCPUFeature(const std::wstring& feature);

    std::wstring GetCpuDetectedVendor() const;
    std::uint32_t GetCpuDetectedLogicalCores() const noexcept;

    bool GetCpuDetectedSSE2() const noexcept { return cpuDetectedSSE2; }
    bool GetCpuDetectedSSE3() const noexcept { return cpuDetectedSSE3; }
    bool GetCpuDetectedSSSE3() const noexcept { return cpuDetectedSSSE3; }
    bool GetCpuDetectedSSE4_1() const noexcept { return cpuDetectedSSE4_1; }
    bool GetCpuDetectedSSE4_2() const noexcept { return cpuDetectedSSE4_2; }
    bool GetCpuDetectedAVX() const noexcept { return cpuDetectedAVX; }
    bool GetCpuDetectedAVX2() const noexcept { return cpuDetectedAVX2; }
    bool GetCpuDetectedAVX512() const noexcept { return cpuDetectedAVX512; }
    bool GetCpuDetectedFMA() const noexcept { return cpuDetectedFMA; }
    bool GetCpuDetectedPOPCNT() const noexcept { return cpuDetectedPOPCNT; }
    bool GetCpuDetectedAESNI() const noexcept { return cpuDetectedAESNI; }
    bool GetCpuDetectedSHA() const noexcept { return cpuDetectedSHA; }
    bool GetCpuDetectedBMI1() const noexcept { return cpuDetectedBMI1; }
    bool GetCpuDetectedBMI2() const noexcept { return cpuDetectedBMI2; }

    bool GetCpuEnableParallelWorkers() const noexcept { return cpuEnableParallelWorkers; }
    void SetCpuEnableParallelWorkers(bool v) noexcept { cpuEnableParallelWorkers = v; }

    std::uint32_t GetCpuRuntimeWorkerReserveCores() const noexcept;
    void SetCpuRuntimeWorkerReserveCores(std::uint32_t v) noexcept;

    std::uint32_t GetCpuRuntimeMaxWorkerThreads() const noexcept;
    void SetCpuRuntimeMaxWorkerThreads(std::uint32_t v) noexcept;

    std::uint32_t DecideRuntimeWorkerCount(
        std::uint32_t requestedMax,
        std::uint32_t hardMax,
        std::uint32_t minWorkers) const noexcept;

    // ---- System ----
    std::wstring GetSystemVendor() const;
    void SetSystemVendor(const std::wstring& vendor);

    // ---- ARIB trace ----
    bool GetAribEnableTrace() const noexcept { return aribEnableTrace; }
    void SetAribEnableTrace(bool enable) noexcept { aribEnableTrace = enable; }

    bool GetAribTraceG() const noexcept { return aribTraceG; }
    void SetAribTraceG(bool enable) noexcept { aribTraceG = enable; }

    bool GetAribTraceChars() const noexcept { return aribTraceChars; }
    void SetAribTraceChars(bool enable) noexcept { aribTraceChars = enable; }

    // ---- ChannelScan switches ----
    bool GetChannelScanEnableTsDump() const noexcept { return channelScanEnableTsDump; }
    void SetChannelScanEnableTsDump(bool enable) noexcept { channelScanEnableTsDump = enable; }

    bool GetChannelScanEnableRingDump() const noexcept { return channelScanEnableRingDump; }
    void SetChannelScanEnableRingDump(bool enable) noexcept { channelScanEnableRingDump = enable; }

    // ---- ChannelScan Fixed ----
    float GetChannelScanFixedSignalLevelThreshold() const noexcept { return channelScanFixedSignalLevelThreshold; }
    void SetChannelScanFixedSignalLevelThreshold(float v) noexcept { channelScanFixedSignalLevelThreshold = v; }

    int GetChannelScanFixedLockWaitMs() const noexcept { return channelScanFixedLockWaitMs; }
    void SetChannelScanFixedLockWaitMs(int v) noexcept { channelScanFixedLockWaitMs = v; }

    int GetChannelScanFixedTsCheckWaitMs() const noexcept { return channelScanFixedTsCheckWaitMs; }
    void SetChannelScanFixedTsCheckWaitMs(int v) noexcept { channelScanFixedTsCheckWaitMs = v; }

    int GetChannelScanFixedTsCheckPollMs() const noexcept { return channelScanFixedTsCheckPollMs; }
    void SetChannelScanFixedTsCheckPollMs(int v) noexcept { channelScanFixedTsCheckPollMs = v; }

    std::size_t GetChannelScanFixedMinTsBytes() const noexcept { return channelScanFixedMinTsBytes; }
    void SetChannelScanFixedMinTsBytes(std::size_t v) noexcept { channelScanFixedMinTsBytes = v; }

    bool GetChannelScanFixedDropVideoAudioSubtitle() const noexcept { return channelScanFixedDropVideoAudioSubtitle; }
    void SetChannelScanFixedDropVideoAudioSubtitle(bool v) noexcept { channelScanFixedDropVideoAudioSubtitle = v; }

    bool GetChannelScanFixedSavePerSpace() const noexcept { return channelScanFixedSavePerSpace; }
    void SetChannelScanFixedSavePerSpace(bool v) noexcept { channelScanFixedSavePerSpace = v; }

    bool GetChannelScanFixedLogConfigSnapshot() const noexcept { return channelScanFixedLogConfigSnapshot; }
    void SetChannelScanFixedLogConfigSnapshot(bool v) noexcept { channelScanFixedLogConfigSnapshot = v; }

    int GetChannelScanFixedCaptureSec() const noexcept { return channelScanFixedCaptureSec; }
    void SetChannelScanFixedCaptureSec(int v) noexcept { channelScanFixedCaptureSec = v; }

    std::size_t GetChannelScanFixedRingBufferSizeBytes() const noexcept { return channelScanFixedRingBufferSizeBytes; }
    void SetChannelScanFixedRingBufferSizeBytes(std::size_t v) noexcept { channelScanFixedRingBufferSizeBytes = v; }

    std::size_t GetChannelScanFixedPacketReserve() const noexcept { return channelScanFixedPacketReserve; }
    void SetChannelScanFixedPacketReserve(std::size_t v) noexcept { channelScanFixedPacketReserve = v; }

    int GetChannelScanFixedSleepOnFailMs() const noexcept { return channelScanFixedSleepOnFailMs; }
    void SetChannelScanFixedSleepOnFailMs(int v) noexcept { channelScanFixedSleepOnFailMs = v; }

    bool GetChannelScanFixedScanAllSpaces() const noexcept { return channelScanFixedScanAllSpaces; }
    void SetChannelScanFixedScanAllSpaces(bool v) noexcept { channelScanFixedScanAllSpaces = v; }

    int GetChannelScanFixedMaxSpaceToTry() const noexcept { return channelScanFixedMaxSpaceToTry; }
    void SetChannelScanFixedMaxSpaceToTry(int v) noexcept { channelScanFixedMaxSpaceToTry = v; }

    int GetChannelScanFixedDefaultSpace() const noexcept { return channelScanFixedDefaultSpace; }
    void SetChannelScanFixedDefaultSpace(int v) noexcept { channelScanFixedDefaultSpace = v; }

    // ---- UI settings ----
    bool GetUiMinimizeToTray() const noexcept { return uiMinimizeToTray; }
    void SetUiMinimizeToTray(bool enable) noexcept { uiMinimizeToTray = enable; }

    bool GetUiStartupEnabled() const noexcept { return uiStartupEnabled; }
    void SetUiStartupEnabled(bool enable) noexcept { uiStartupEnabled = enable; }

    int GetUiWindowX() const noexcept { return uiWindowX; }
    int GetUiWindowY() const noexcept { return uiWindowY; }
    int GetUiWindowW() const noexcept { return uiWindowW; }
    int GetUiWindowH() const noexcept { return uiWindowH; }
    int GetUiLastTabIndex() const noexcept { return uiLastTabIndex; }

    void SetUiWindowRect(int x, int y, int w, int h) noexcept;
    void SetUiLastTabIndex(int idx) noexcept { uiLastTabIndex = idx; }

    // ---- Tuners ----
    int GetTunersCount() const noexcept { return tunersCount; }

    void SetTunersCount(int count) noexcept
    {
        if (count < kMinTuners) {
            count = kMinTuners;
        }

        tunersCount = count;

        if (tunersActiveIndex < 0) {
            tunersActiveIndex = 0;
        }

        if (tunersActiveIndex >= tunersCount) {
            tunersActiveIndex = tunersCount - 1;
        }

        EnsureTunerVectors_();
    }

    int GetTunersActiveIndex() const noexcept { return tunersActiveIndex; }

    void SetTunersActiveIndex(int idx) noexcept
    {
        if (idx < 0) {
            idx = 0;
        }

        if (idx >= tunersCount) {
            idx = tunersCount - 1;
        }

        tunersActiveIndex = idx;
    }

    bool GetTunerEnabled(int tunerIndex) const noexcept;
    void SetTunerEnabled(int tunerIndex, bool enabled) noexcept;

    // EPG更新・録画・予約実行で使用可能なチューナーか判定する。
    // 条件:
    //   EnableX=1
    //   GetMultiTunerBonDriverPath(X) が空ではない
    bool IsTunerUsableForMultiTuner(int tunerIndex) const;

    // 使用可能チューナー一覧を返す。
    // [Tuners] Count の範囲内で、EnableX=1 かつ BonDriverPath があるものだけ返す。
    // 同時使用制限はここでは行わない。
    std::vector<int> BuildUsableMultiTunerIndexList() const;

    std::wstring GetTunerName(int tunerIndex) const;
    void SetTunerName(int tunerIndex, const std::wstring& name);

    // ---- Tuner global ----
    enum class TunerGlobalConflictPolicy : int {
        AutoTryNextTuner = 0,
        PreferPreferred = 1,
        FailIfBusy = 2,
    };

    bool GetTunerGlobalUseCommonBonDriver() const noexcept { return tunerGlobalUseCommonBonDriver; }
    void SetTunerGlobalUseCommonBonDriver(bool v) noexcept { tunerGlobalUseCommonBonDriver = v; }

    std::wstring GetTunerGlobalCommonBonDriverPath() const { return tunerGlobalCommonBonDriverPath; }
    void SetTunerGlobalCommonBonDriverPath(const std::wstring& path) { tunerGlobalCommonBonDriverPath = path; }

    TunerGlobalConflictPolicy GetTunerGlobalConflictPolicy() const noexcept { return tunerGlobalConflictPolicy; }
    void SetTunerGlobalConflictPolicy(TunerGlobalConflictPolicy p) noexcept { tunerGlobalConflictPolicy = p; }

    bool GetTunerGlobalPauseEpgWhileRecording() const noexcept { return tunerGlobalPauseEpgWhileRecording; }
    void SetTunerGlobalPauseEpgWhileRecording(bool v) noexcept { tunerGlobalPauseEpgWhileRecording = v; }

    bool GetTunerGlobalPreferRecordingOverEpg() const noexcept { return tunerGlobalPreferRecordingOverEpg; }
    void SetTunerGlobalPreferRecordingOverEpg(bool v) noexcept { tunerGlobalPreferRecordingOverEpg = v; }

    std::wstring GetTunerGlobalCommonExternalAppPath() const { return tunerGlobalCommonExternalAppPath; }
    void SetTunerGlobalCommonExternalAppPath(const std::wstring& path) { tunerGlobalCommonExternalAppPath = path; }

    // ---- Tuner individual ----
    enum class TunerReceiveMode : int {
        Generic = 0,
        Terrestrial = 1,
        BS = 2,
        CS = 3,
    };

    enum class TunerUseRole : int {
        General = 0,
        RecordOnly = 1,
        EpgOnly = 2,
        ScanOnly = 3,
        ViewOnly = 4,
        TimeShiftOnly = 5,
    };

    std::wstring GetTunerExternalAppPath(int tunerIndex) const;
    void SetTunerExternalAppPath(int tunerIndex, const std::wstring& path);

    TunerReceiveMode GetTunerReceiveMode(int tunerIndex) const noexcept;
    void SetTunerReceiveMode(int tunerIndex, TunerReceiveMode mode) noexcept;

    TunerUseRole GetTunerUseRole(int tunerIndex) const noexcept;
    void SetTunerUseRole(int tunerIndex, TunerUseRole role) noexcept;

    bool GetTunerAllowParallel(int tunerIndex) const noexcept;
    void SetTunerAllowParallel(int tunerIndex, bool allow) noexcept;

    int GetTunerPriority(int tunerIndex) const noexcept;
    void SetTunerPriority(int tunerIndex, int priority) noexcept;

    int GetTunerTimeShiftLockSpace(int tunerIndex) const noexcept;
    void SetTunerTimeShiftLockSpace(int tunerIndex, int space) noexcept;

    int GetTunerTimeShiftLockChannel(int tunerIndex) const noexcept;
    void SetTunerTimeShiftLockChannel(int tunerIndex, int channel) noexcept;

    std::uint16_t GetTunerTimeShiftLockServiceId(int tunerIndex) const noexcept;
    void SetTunerTimeShiftLockServiceId(int tunerIndex, std::uint16_t serviceId) noexcept;

    std::uint16_t GetTunerTimeShiftLockNetworkId(int tunerIndex) const noexcept;
    void SetTunerTimeShiftLockNetworkId(int tunerIndex, std::uint16_t networkId) noexcept;

    std::uint16_t GetTunerTimeShiftLockTsId(int tunerIndex) const noexcept;
    void SetTunerTimeShiftLockTsId(int tunerIndex, std::uint16_t tsId) noexcept;

    std::wstring GetTunerTimeShiftLockName(int tunerIndex) const;
    void SetTunerTimeShiftLockName(int tunerIndex, const std::wstring& name);

    bool GetTunerUseExternalApp(int tunerIndex) const noexcept;
    void SetTunerUseExternalApp(int tunerIndex, bool enable) noexcept;

    // ---- Tuner TVTest bridge ----
    bool GetTunerTvTestUseBridge(int tunerIndex) const noexcept;
    void SetTunerTvTestUseBridge(int tunerIndex, bool enable) noexcept;

    std::wstring GetTunerTvTestDid(int tunerIndex) const;
    void SetTunerTvTestDid(int tunerIndex, const std::wstring& did);

    std::wstring GetTunerTvTestExePath(int tunerIndex) const;
    void SetTunerTvTestExePath(int tunerIndex, const std::wstring& path);

    std::wstring GetTunerTvTestControlPipeName(int tunerIndex) const;
    void SetTunerTvTestControlPipeName(int tunerIndex, const std::wstring& path);

    std::wstring GetTunerTvTestTsPipeName(int tunerIndex) const;
    void SetTunerTvTestTsPipeName(int tunerIndex, const std::wstring& path);

    std::wstring GetTunerTvTestCommand(int tunerIndex) const;
    void SetTunerTvTestCommand(int tunerIndex, const std::wstring& command);

    // ---- Reservation execution ----
    bool GetReservationExecutionEnableExecution() const noexcept { return reservationExecutionEnableExecution; }
    void SetReservationExecutionEnableExecution(bool v) noexcept { reservationExecutionEnableExecution = v; }

    bool GetReservationExecutionEnableWatchReservation() const noexcept { return reservationExecutionEnableWatchReservation; }
    void SetReservationExecutionEnableWatchReservation(bool v) noexcept { reservationExecutionEnableWatchReservation = v; }

    bool GetReservationExecutionEnableTvTestWatch() const noexcept { return reservationExecutionEnableTvTestWatch; }
    void SetReservationExecutionEnableTvTestWatch(bool v) noexcept { reservationExecutionEnableTvTestWatch = v; }

    bool GetReservationExecutionEnableRecordExecution() const noexcept
    {
        return reservationExecutionEnableRecordExecution;
    }

    void SetReservationExecutionEnableRecordExecution(bool v) noexcept
    {
        reservationExecutionEnableRecordExecution = v;
    }

    bool GetReservationExecutionEnableTsPipeDuringWatch() const noexcept { return reservationExecutionEnableTsPipeDuringWatch; }
    void SetReservationExecutionEnableTsPipeDuringWatch(bool v) noexcept { reservationExecutionEnableTsPipeDuringWatch = v; }

    bool GetReservationExecutionTvTestRecordShowVideo() const noexcept
    {
        return reservationExecutionTvTestRecordShowVideo;
    }

    void SetReservationExecutionTvTestRecordShowVideo(bool v) noexcept
    {
        reservationExecutionTvTestRecordShowVideo = v;
    }

    std::wstring GetReservationExecutionRecordFolder() const { return reservationExecutionRecordFolder; }
    void SetReservationExecutionRecordFolder(const std::wstring& path) { reservationExecutionRecordFolder = path; }

    int GetReservationExecutionPrepareSeconds() const noexcept { return reservationExecutionPrepareSeconds; }
    void SetReservationExecutionPrepareSeconds(int v) noexcept { reservationExecutionPrepareSeconds = ClampPrepareSeconds_(v); }

    // ---- CPU capability flags ----
    bool GetCpuUseSSE2() const noexcept { return cpuUseSSE2; }
    bool GetCpuUseSSE3() const noexcept { return cpuUseSSE3; }
    bool GetCpuUseSSSE3() const noexcept { return cpuUseSSSE3; }
    bool GetCpuUseSSE4_1() const noexcept { return cpuUseSSE4_1; }
    bool GetCpuUseSSE4_2() const noexcept { return cpuUseSSE4_2; }
    bool GetCpuUseAVX() const noexcept { return cpuUseAVX; }
    bool GetCpuUseAVX2() const noexcept { return cpuUseAVX2; }
    bool GetCpuUseAVX512() const noexcept { return cpuUseAVX512; }
    bool GetCpuUseFMA() const noexcept { return cpuUseFMA; }
    bool GetCpuUsePOPCNT() const noexcept { return cpuUsePOPCNT; }
    bool GetCpuUseAESNI() const noexcept { return cpuUseAESNI; }
    bool GetCpuUseSHA() const noexcept { return cpuUseSHA; }
    bool GetCpuUseBMI1() const noexcept { return cpuUseBMI1; }
    bool GetCpuUseBMI2() const noexcept { return cpuUseBMI2; }

    void SetCpuUseSSE2(bool v) noexcept { cpuUseSSE2 = v; }
    void SetCpuUseSSE3(bool v) noexcept { cpuUseSSE3 = v; }
    void SetCpuUseSSSE3(bool v) noexcept { cpuUseSSSE3 = v; }
    void SetCpuUseSSE4_1(bool v) noexcept { cpuUseSSE4_1 = v; }
    void SetCpuUseSSE4_2(bool v) noexcept { cpuUseSSE4_2 = v; }
    void SetCpuUseAVX(bool v) noexcept { cpuUseAVX = v; }
    void SetCpuUseAVX2(bool v) noexcept { cpuUseAVX2 = v; }
    void SetCpuUseAVX512(bool v) noexcept { cpuUseAVX512 = v; }
    void SetCpuUseFMA(bool v) noexcept { cpuUseFMA = v; }
    void SetCpuUsePOPCNT(bool v) noexcept { cpuUsePOPCNT = v; }
    void SetCpuUseAESNI(bool v) noexcept { cpuUseAESNI = v; }
    void SetCpuUseSHA(bool v) noexcept { cpuUseSHA = v; }
    void SetCpuUseBMI1(bool v) noexcept { cpuUseBMI1 = v; }
    void SetCpuUseBMI2(bool v) noexcept { cpuUseBMI2 = v; }

    // ---- EPG 自動更新 ----
    enum class EpgAutoMode : int {
        Off = 0,
        Task = 1,
    };

    // ---- EPG HTML rendering ----
    enum class EpgHtmlRenderRoute : int
    {
        Legacy = 0,
        ProgramPageWriter = 1,
        Plugin = 2,
        Auto = 3,
        Lite = 4
    };

    // ---- Reservation ----
        // ---- Reservation ----
    enum class ReservationConflictPolicy : int {
        AutoTryNextTuner = 0,
        PreferPreferred = 1,
        FailIfBusy = 2,
    };

    enum class EpgReserveButtonMode : int {
        Select = 0,
        Record = 1,
        Watch = 2,
    };

    int  GetReservationStartAdvanceSec() const noexcept { return reservationStartAdvanceSec; }
    void SetReservationStartAdvanceSec(int v) noexcept { reservationStartAdvanceSec = (v < 0) ? 0 : (v > 600 ? 600 : v); }

    int  GetReservationStartAdjustSec() const noexcept { return reservationStartAdjustSec; }
    void SetReservationStartAdjustSec(int v) noexcept { reservationStartAdjustSec = (v < -1) ? -1 : (v > 1 ? 1 : v); }

    int  GetReservationEndAdjustMin() const noexcept { return reservationEndAdjustMin; }
    void SetReservationEndAdjustMin(int v) noexcept { reservationEndAdjustMin = (v < -30) ? -30 : (v > 180 ? 180 : v); }

    int  GetReservationPreferredTuner() const noexcept { return reservationPreferredTuner; }
    void SetReservationPreferredTuner(int v) noexcept {
        if (v < -1) v = -1;
        if (v >= tunersCount) v = -1;
        reservationPreferredTuner = v;
    }

    ReservationConflictPolicy GetReservationConflictPolicy() const noexcept { return reservationConflictPolicy; }
    void SetReservationConflictPolicy(ReservationConflictPolicy p) noexcept { reservationConflictPolicy = p; }

    bool GetReservationPauseEpgWhileRecording() const noexcept { return reservationPauseEpgWhileRecording; }
    void SetReservationPauseEpgWhileRecording(bool v) noexcept { reservationPauseEpgWhileRecording = v; }

    bool GetReservationCleanupCompletedAfterEpgBuild() const noexcept
    {
        return reservationCleanupCompletedAfterEpgBuild;
    }

    void SetReservationCleanupCompletedAfterEpgBuild(bool v) noexcept
    {
        reservationCleanupCompletedAfterEpgBuild = v;
    }

    EpgReserveButtonMode GetReservationEpgReserveButtonMode() const noexcept { return reservationEpgReserveButtonMode; }
    void SetReservationEpgReserveButtonMode(EpgReserveButtonMode mode) noexcept { reservationEpgReserveButtonMode = mode; }

    EpgAutoMode GetEpgAutoMode() const noexcept { return epgAutoMode; }
    void SetEpgAutoMode(EpgAutoMode m) noexcept { epgAutoMode = m; }

    int GetEpgBootGraceSec() const noexcept { return epgBootGraceSec; }
    void SetEpgBootGraceSec(int v) noexcept { epgBootGraceSec = v; }

    bool GetEpgCatchUpOnStart() const noexcept { return epgCatchUpOnStart; }
    void SetEpgCatchUpOnStart(bool v) noexcept { epgCatchUpOnStart = v; }

    int GetEpgTickSeconds() const noexcept { return epgTickSeconds; }
    void SetEpgTickSeconds(int v) noexcept { epgTickSeconds = v; }

    int GetEpgIdleRequiredSec() const noexcept { return epgIdleRequiredSec; }
    void SetEpgIdleRequiredSec(int v) noexcept { epgIdleRequiredSec = v; }

    bool GetEpgEnableDailyFull() const noexcept { return epgEnableDailyFull; }
    void SetEpgEnableDailyFull(bool v) noexcept { epgEnableDailyFull = v; }

    int GetEpgDailyFullHHMM() const noexcept { return epgDailyFullHHMM; }
    void SetEpgDailyFullHHMM(int hhmm) noexcept { epgDailyFullHHMM = hhmm; }

    bool GetEpgEnableHourly() const noexcept { return epgEnableHourly; }
    void SetEpgEnableHourly(bool v) noexcept { epgEnableHourly = v; }

    int GetEpgHourlyAtMinute() const noexcept { return epgHourlyAtMinute; }
    void SetEpgHourlyAtMinute(int v) noexcept { epgHourlyAtMinute = v; }

    int GetEpgLightUpdateIntervalMin() const noexcept { return epgLightUpdateIntervalMin; }
    void SetEpgLightUpdateIntervalMin(int v) noexcept { epgLightUpdateIntervalMin = v; }

    int GetManualFullEstimatedMinutes() const noexcept { return manualFullEstimatedMinutes; }
    void SetManualFullEstimatedMinutes(int v) noexcept { manualFullEstimatedMinutes = ClampMinutes_(v, 1, 24 * 60); }

    int GetManualFullGuardMinutes() const noexcept { return manualFullGuardMinutes; }
    void SetManualFullGuardMinutes(int v) noexcept { manualFullGuardMinutes = ClampMinutes_(v, 0, 24 * 60); }

    int GetManualLightBaseMinutes() const noexcept { return manualLightBaseMinutes; }
    void SetManualLightBaseMinutes(int v) noexcept { manualLightBaseMinutes = ClampMinutes_(v, 1, 24 * 60); }

    int GetManualLightPerChannelMinutes() const noexcept { return manualLightPerChannelMinutes; }
    void SetManualLightPerChannelMinutes(int v) noexcept { manualLightPerChannelMinutes = ClampMinutes_(v, 0, 24 * 60); }

    int GetManualLightGuardMinutes() const noexcept { return manualLightGuardMinutes; }
    void SetManualLightGuardMinutes(int v) noexcept { manualLightGuardMinutes = ClampMinutes_(v, 0, 24 * 60); }

    EpgHtmlRenderRoute GetEpgHtmlRenderRoute() const noexcept { return epgHtmlRenderRoute; }
    void SetEpgHtmlRenderRoute(EpgHtmlRenderRoute v) noexcept { epgHtmlRenderRoute = v; }

    // ---- EPG services ----
    bool GetEpgServiceDefaultEnable(int space, bool fallback) const;
    bool GetEpgServiceEnabled(int space, std::uint16_t onid, std::uint16_t tsid, std::uint16_t sid, bool fallbackDefault) const;

    void SetEpgServiceEnabled(
        int space,
        std::uint16_t onid,
        std::uint16_t tsid,
        std::uint16_t sid,
        bool enabled,
        int ch,
        int serviceType,
        const std::wstring& serviceName,
        const std::wstring& networkName);

    void SaveConfig();
    void InitializeConfig();

    // ---- Network / EPG portal ----
    // NetworkSettingsTab と HTTP サーバーが共有する [Network] 設定。
    // パスワード本体は保持せず、Windows DPAPI で保護した文字列だけを扱う。
    struct NetworkSettings
    {
        bool enableServer = false;
        std::wstring bindAddress = L"127.0.0.1";
        int httpPort = 8080;
        bool allowRemoteAccess = false;
        bool autoStartServer = false;

        bool enableHttps = false;
        int httpsPort = 8443;
        std::wstring pfxPath{};
        std::wstring pfxPasswordProtected{};

        bool enablePasswordAuth = false;
        std::wstring loginUser = L"epg";
        std::wstring loginPasswordProtected{};
        int sessionTimeoutMinutes = 720;

        int requestTimeoutMs = 15000;
        bool enableCors = false;
        bool openBrowserOnStart = true;
        std::wstring tlsMode = L"http";
        std::wstring listenScope = L"local";
    };

    NetworkSettings LoadNetworkSettings() const;
    bool SaveNetworkSettings(const NetworkSettings& settings) noexcept;

    // 旧構成から新構成へ寄せた後、重複・廃止キーを削除する。
    void PurgeObsoleteManagedConfigKeys();

    bool ParseBool(const std::wstring& value) const;

    const std::wstring& GetIniPath() const noexcept { return iniPath; }

    // ---- Path settings ----
// config.ini には相対パスで保存可能。
// 実行時は AppWorkDir / EpgOutDir / EpgDataWorkDir を明確に解決して使う。
    std::wstring ResolvePathFromAppBase(const std::wstring& pathText) const;
    std::wstring ResolvePathFromWorkDir(const std::wstring& pathText) const;

    std::wstring GetPathAppWorkDirRaw() const;
    void SetPathAppWorkDirRaw(const std::wstring& path);

    std::wstring GetPathAppWorkDir() const;

    std::wstring GetPathEpgOutDirRaw() const;
    void SetPathEpgOutDirRaw(const std::wstring& path);

    std::wstring GetPathEpgOutDir() const;

    std::wstring GetPathEpgDataWorkDirRaw() const;
    void SetPathEpgDataWorkDirRaw(const std::wstring& path);

    std::wstring GetPathEpgDataWorkDir() const;

    // ---- TimeShift settings ----
    std::wstring GetTimeShiftRootFolderRaw() const;
    void SetTimeShiftRootFolderRaw(const std::wstring& path);

    std::wstring GetTimeShiftRootFolder() const;

    // UI / config.ini は GB 単位。
    int GetTimeShiftMaxBytesGB() const noexcept;
    void SetTimeShiftMaxBytesGB(int gb) noexcept;

    // 旧互換用。内部接続済みコードがMBを使っていても壊さない。
    int GetTimeShiftMaxBytesMB() const noexcept;
    void SetTimeShiftMaxBytesMB(int mb) noexcept;

    bool GetCpuDetectedChangedAtStartup() const noexcept
    {
        return cpuDetectedChangedAtStartup_;
    }


private:
    ConfigManager();

    std::wstring iniPath;
    mutable std::mutex iniMutex;

    void EnsureTunerVectors_() noexcept;
    static bool IsTimeShiftLockConfiguredValues_(
        int channel,
        int serviceId,
        int networkId,
        int tsId) noexcept;

    bool IsTimeShiftLockConfiguredAt_(int tunerIndex) const noexcept;

    int ReadExistingTunerIntForSave_(
        int tunerIndex,
        const wchar_t* baseKey,
        const wchar_t* oldSection,
        const wchar_t* oldBaseKey,
        int fallback) const;

    std::wstring ReadExistingTunerStringForSave_(
        int tunerIndex,
        const wchar_t* baseKey,
        const wchar_t* oldSection,
        const wchar_t* oldBaseKey,
        const std::wstring& fallback) const;

    void NormalizeTimeShiftRolesFromLocks_() noexcept;
    void PreserveTimeShiftTunerSettingsBeforeFullSave_() noexcept;

    static std::wstring MakeIndexedKey_(std::wstring_view base, int index);
    static int ClampMinutes_(int v, int lo, int hi) noexcept;
    static int ClampPrepareSeconds_(int v) noexcept;
    static int ClampIdleMonitorSampleMs_(int v) noexcept;
    static int ClampIdleMonitorPercent_(int v) noexcept;
    static int ClampIdleMonitorIoKBps_(int v) noexcept;

    static std::wstring MakeDefaultTvTestControlPipeName_(int tunerIndex);
    static std::wstring MakeDefaultTvTestTsPipeName_(int tunerIndex);
    static std::wstring MakeDefaultTvTestDid_(int tunerIndex);

    int tunersActiveIndex = 0;

    std::vector<std::wstring> bonDriverPathPerTuner;
    std::vector<std::wstring> tunerNamePerTuner;
    std::vector<std::uint8_t> tunerEnablePerTuner;

    std::vector<std::wstring> tunerExternalAppPathPerTuner;
    std::vector<int> tunerReceiveModePerTuner;
    std::vector<int> tunerUseRolePerTuner;
    std::vector<std::uint8_t> tunerAllowParallelPerTuner;
    std::vector<int> tunerPriorityPerTuner;
    std::vector<int> tunerTimeShiftLockSpacePerTuner;
    std::vector<int> tunerTimeShiftLockChannelPerTuner;
    std::vector<int> tunerTimeShiftLockServiceIdPerTuner;
    std::vector<int> tunerTimeShiftLockNetworkIdPerTuner;
    std::vector<int> tunerTimeShiftLockTsIdPerTuner;
    std::vector<std::wstring> tunerTimeShiftLockNamePerTuner;
    std::vector<std::uint8_t> tunerUseExternalAppPerTuner;
    std::vector<std::uint8_t> tunerTvTestUseBridgePerTuner;
    std::vector<std::wstring> tunerTvTestDidPerTuner;
    std::vector<std::wstring> tunerTvTestExePathPerTuner;
    std::vector<std::wstring> tunerTvTestControlPipeNamePerTuner;
    std::vector<std::wstring> tunerTvTestTsPipeNamePerTuner;
    std::vector<std::wstring> tunerTvTestCommandPerTuner;
    std::vector<std::wstring> tunerInternalBonDriverPathPerTuner;
    std::vector<std::wstring> tunerTvTestBonDriverPathPerTuner;

    std::wstring systemVendor = L"Unknown";

    // 旧互換 / 本体既定。
// 基本的には地上波BonDriverを入れる。
    std::wstring bonDriverPath;

    // space=0 用。
    std::wstring bonDriverTerrestrialPath;

    // space=1/2 用。
    std::wstring bonDriverSatellitePath;

    // 予備 fallback。
    std::wstring bonDriverDefaultPath;
    bool debugWriteToFile = false;
    std::wstring selectedCPUFeature = L"AUTO";
    std::wstring logEncoding = L"utf8";

    bool enableSDTDebug = false;
    bool enableNITDebug = false;

    bool guiLogEnabled = true;
    std::wstring guiLogFile = L"debug.log";


    int idleMonitorSampleMs = 5000;
    bool idleMonitorConsoleLog = true;
    int idleMonitorSystemCpuPct = 20;
    int idleMonitorProcessCpuPct = 5;
    int idleMonitorReadKBps = 2048;
    int idleMonitorWriteKBps = 4096;
    int idleMonitorMemoryLoadPct = 90;
    bool aribEnableTrace = false;
    bool aribTraceG = false;
    bool aribTraceChars = false;

    bool channelScanEnableTsDump = false;
    bool channelScanEnableRingDump = false;

    float channelScanFixedSignalLevelThreshold = 0.0f;
    int channelScanFixedLockWaitMs = 600;
    int channelScanFixedTsCheckWaitMs = 800;
    int channelScanFixedTsCheckPollMs = 50;
    std::size_t channelScanFixedMinTsBytes = 940;
    bool channelScanFixedDropVideoAudioSubtitle = true;
    bool channelScanFixedSavePerSpace = true;
    bool channelScanFixedLogConfigSnapshot = true;

    int channelScanFixedCaptureSec = 3;
    std::size_t channelScanFixedRingBufferSizeBytes = 32ull * 1024ull * 1024ull;
    std::size_t channelScanFixedPacketReserve = 4096;
    int channelScanFixedSleepOnFailMs = 1;
    bool channelScanFixedScanAllSpaces = false;
    int channelScanFixedMaxSpaceToTry = 7;
    int channelScanFixedDefaultSpace = 0;

    bool uiMinimizeToTray = true;
    bool uiStartupEnabled = false;
    int uiWindowX = -1;
    int uiWindowY = -1;
    int uiWindowW = 1100;
    int uiWindowH = 720;
    int uiLastTabIndex = 0;

    int tunersCount = 1;

    bool tunerGlobalUseCommonBonDriver = true;
    std::wstring tunerGlobalCommonBonDriverPath;
    TunerGlobalConflictPolicy tunerGlobalConflictPolicy = TunerGlobalConflictPolicy::AutoTryNextTuner;
    bool tunerGlobalPauseEpgWhileRecording = true;
    bool tunerGlobalPreferRecordingOverEpg = true;
    std::wstring tunerGlobalCommonExternalAppPath;

    bool cpuUseSSE2 = true;
    bool cpuUseSSE3 = true;
    bool cpuUseSSSE3 = true;
    bool cpuUseSSE4_1 = true;
    bool cpuUseSSE4_2 = true;
    bool cpuUseAVX = true;
    bool cpuUseAVX2 = true;
    bool cpuUseAVX512 = false;
    bool cpuUseFMA = true;
    bool cpuUsePOPCNT = true;
    bool cpuUseAESNI = true;
    bool cpuUseSHA = true;

    std::wstring cpuDetectedVendor = L"Unknown";
    std::uint32_t cpuDetectedLogicalCores = 1;

    bool cpuDetectedSSE2 = false;
    bool cpuDetectedSSE3 = false;
    bool cpuDetectedSSSE3 = false;
    bool cpuDetectedSSE4_1 = false;
    bool cpuDetectedSSE4_2 = false;
    bool cpuDetectedAVX = false;
    bool cpuDetectedAVX2 = false;
    bool cpuDetectedAVX512 = false;
    bool cpuDetectedFMA = false;
    bool cpuDetectedPOPCNT = false;
    bool cpuDetectedAESNI = false;
    bool cpuDetectedSHA = false;
    bool cpuDetectedBMI1 = false;
    bool cpuDetectedBMI2 = false;

    bool cpuUseBMI1 = true;
    bool cpuUseBMI2 = true;

    bool cpuEnableParallelWorkers = true;
    std::uint32_t cpuRuntimeWorkerReserveCores = 1;
    std::uint32_t cpuRuntimeMaxWorkerThreads = 0;

    int reservationStartAdvanceSec = 30;
    int reservationStartAdjustSec = 0;
    int reservationEndAdjustMin = 0;
    int reservationPreferredTuner = -1;

    ReservationConflictPolicy reservationConflictPolicy = ReservationConflictPolicy::AutoTryNextTuner;
    bool reservationPauseEpgWhileRecording = true;
    bool reservationCleanupCompletedAfterEpgBuild = true;
    EpgReserveButtonMode reservationEpgReserveButtonMode = EpgReserveButtonMode::Select;



    EpgAutoMode epgAutoMode = EpgAutoMode::Off;
    int  epgBootGraceSec = 60;
    bool epgCatchUpOnStart = false;
    int  epgTickSeconds = 5;
    int  epgIdleRequiredSec = 300;
    bool epgEnableDailyFull = true;
    int  epgDailyFullHHMM = 330;
    bool epgEnableHourly = true;
    int  epgHourlyAtMinute = 5;
    int  epgLightUpdateIntervalMin = 0;
    int  manualFullEstimatedMinutes = 90;
    int  manualFullGuardMinutes = 30;
    int  manualLightBaseMinutes = 15;
    int  manualLightPerChannelMinutes = 3;
    int  manualLightGuardMinutes = 30;
    EpgHtmlRenderRoute epgHtmlRenderRoute = EpgHtmlRenderRoute::Legacy;

    std::wstring ReadIniValue(const std::wstring& section,
        const std::wstring& key,
        const std::wstring& defaultValue) const;

    int ReadIniInt(const std::wstring& section,
        const std::wstring& key,
        int defaultValue) const;

    float ReadIniFloat(const std::wstring& section,
        const std::wstring& key,
        float defaultValue) const;

    std::size_t ReadIniSizeT(const std::wstring& section,
        const std::wstring& key,
        std::size_t defaultValue) const;

    bool IsIniKeyMissing(const std::wstring& section,
        const std::wstring& key) const;

    void WriteIniValue(const std::wstring& section,
        const std::wstring& key,
        const std::wstring& value);

    void WriteIniInt(const std::wstring& section,
        const std::wstring& key,
        int value);

    void WriteIniFloat(const std::wstring& section,
        const std::wstring& key,
        float value);

    void WriteIniSizeT(const std::wstring& section,
        const std::wstring& key,
        std::size_t value);

    void WriteIniBool(const std::wstring& section,
        const std::wstring& key,
        bool value);

    void DeleteIniKey(const std::wstring& section,
        const std::wstring& key);

    void DeleteIniSection(const std::wstring& section);

    void EnsureDefault(const std::wstring& section,
        const std::wstring& key,
        const std::wstring& defaultValue);

    void RefreshCpuDetectedFromFeature_() noexcept;
    void SaveCpuSectionOnly_() noexcept;

    void LoadCpuDetectedFromIni_() noexcept;
    void SyncCpuDetectedAtStartup_(bool hadConfigFile) noexcept;

    bool cpuDetectedChangedAtStartup_ = false;
    bool timeShiftTunerSettingsDirty_ = false;

    static std::wstring MakeEpgServiceSection(int space);
    static std::wstring MakeEpgServiceKey(std::uint16_t onid, std::uint16_t tsid, std::uint16_t sid);
    static std::wstring CsvEscape(const std::wstring& s);
    static std::wstring TrimCopy(std::wstring s);
    bool IsValidTunerIndex_(int tunerIndex) const noexcept;

    bool reservationExecutionEnableExecution = true;
    bool reservationExecutionEnableWatchReservation = true;
    bool reservationExecutionEnableTvTestWatch = true;
    bool reservationExecutionEnableRecordExecution = true;
    bool reservationExecutionEnableTsPipeDuringWatch = false;
    bool reservationExecutionTvTestRecordShowVideo = true;
    std::wstring reservationExecutionRecordFolder;
    int reservationExecutionPrepareSeconds = 30;

    std::wstring pathAppWorkDir = L".";
    std::wstring pathEpgOutDir = L"epg_out";
    std::wstring pathEpgDataWorkDir = L"epg_work";

    std::wstring timeShiftRootFolder = L"timeshift";
    int timeShiftMaxBytesGB = 100;
};

#endif // CONFIG_MANAGER_H
