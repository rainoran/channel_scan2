#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <cwchar>
#include <cstdlib>
#include <algorithm>
#include <cwctype>
#include <vector>
#include <climits>
#include <mutex>
#include <thread>



#include "ConfigManager.h"
#include "CPUFeature.h"

namespace fs = std::filesystem;

static std::wstring GetExeDir() noexcept
{
    wchar_t exePath[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return fs::current_path().wstring();
    }
    return fs::path(exePath).parent_path().wstring();
}

static std::wstring TrimPathText_(std::wstring s)
{
    auto is_ws = [](wchar_t c) noexcept {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
        };

    while (!s.empty() && is_ws(s.front())) {
        s.erase(s.begin());
    }

    while (!s.empty() && is_ws(s.back())) {
        s.pop_back();
    }

    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') {
        s = s.substr(1, s.size() - 2);
    }

    return s;
}

static std::wstring ResolvePathFromBase_(
    const std::wstring& baseDir,
    const std::wstring& pathText,
    const wchar_t* fallbackRelative)
{
    std::wstring text = TrimPathText_(pathText);

    if (text.empty()) {
        text = fallbackRelative ? fallbackRelative : L".";
    }

    fs::path p(text);

    if (p.is_absolute()) {
        return p.lexically_normal().wstring();
    }

    const fs::path base =
        baseDir.empty()
        ? fs::path(GetExeDir())
        : fs::path(baseDir);

    return (base / p).lexically_normal().wstring();
}

static std::wstring ToLowerTrim(std::wstring s)
{
    auto is_ws = [](wchar_t c) { return (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'); };
    while (!s.empty() && is_ws(s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws(s.back())) s.pop_back();

    std::transform(s.begin(), s.end(), s.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return s;
}

static std::wstring NormalizeSelectedCPUFeature_(const std::wstring& feature)
{
    const std::wstring v = ToLowerTrim(feature);

    if (v.empty() || v == L"auto") {
        return L"AUTO";
    }

    if (v == L"scalar") {
        return L"SCALAR";
    }

    if (v == L"sse2") {
        return L"SSE2";
    }

    if (v == L"sse4.2" ||
        v == L"sse42" ||
        v == L"sse_4_2") {
        return L"SSE4.2";
    }

    if (v == L"avx2") {
        return L"AVX2";
    }

    // 未知値は安全側。
    // GUI表示も空欄にせず、通常運用のAUTOへ戻す。
    return L"AUTO";
}

struct IniKeyValue_
{
    std::wstring key;
    std::wstring value;
};

static std::wstring IniBoolText_(bool value)
{
    return value ? L"1" : L"0";
}

static std::wstring IniIntText_(int value)
{
    return std::to_wstring(value);
}

static std::wstring IniSizeText_(std::size_t value)
{
    return std::to_wstring(
        static_cast<unsigned long long>(value));
}

static std::wstring IniFloatText_(float value)
{
    wchar_t buf[64]{};
    _snwprintf_s(
        buf,
        _countof(buf),
        _TRUNCATE,
        L"%.6f",
        static_cast<double>(value));

    return std::wstring(buf);
}

static void AddIniKey_(
    std::vector<IniKeyValue_>& values,
    const wchar_t* key,
    const std::wstring& value)
{
    if (key == nullptr || key[0] == L'\0') {
        return;
    }

    values.push_back(
        IniKeyValue_{
            std::wstring(key),
            value
        });
}

static bool WriteIniSectionFast_(
    std::mutex& mutex,
    const std::wstring& iniPath,
    const wchar_t* sectionName,
    const std::vector<IniKeyValue_>& values) noexcept
{
    if (sectionName == nullptr || sectionName[0] == L'\0') {
        return false;
    }

    try {
        std::wstring block;

        std::size_t reserveChars = 1u;

        for (const auto& kv : values) {
            reserveChars += kv.key.size();
            reserveChars += 1u;
            reserveChars += kv.value.size();
            reserveChars += 1u;
        }

        block.reserve(reserveChars);

        for (const auto& kv : values) {
            if (kv.key.empty()) {
                continue;
            }

            block += kv.key;
            block += L"=";
            block += kv.value;
            block.push_back(L'\0');
        }

        block.push_back(L'\0');

        std::lock_guard<std::mutex> lock(mutex);

        return WritePrivateProfileSectionW(
            sectionName,
            block.c_str(),
            iniPath.c_str()) != FALSE;
    }
    catch (...) {
        return false;
    }
}

static bool DeleteIniSectionFast_(
    std::mutex& mutex,
    const std::wstring& iniPath,
    const wchar_t* sectionName) noexcept
{
    if (sectionName == nullptr || sectionName[0] == L'\0') {
        return false;
    }

    try {
        std::lock_guard<std::mutex> lock(mutex);

        return WritePrivateProfileStringW(
            sectionName,
            nullptr,
            nullptr,
            iniPath.c_str()) != FALSE;
    }
    catch (...) {
        return false;
    }
}

static int ClampInt(int v, int lo, int hi) noexcept
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static std::uint32_t ClampUInt32_(
    int value,
    std::uint32_t defaultValue,
    std::uint32_t minValue,
    std::uint32_t maxValue) noexcept
{
    if (value < static_cast<int>(minValue)) {
        return defaultValue;
    }

    const std::uint32_t u =
        static_cast<std::uint32_t>(value);

    if (u > maxValue) {
        return maxValue;
    }

    return u;
}

static constexpr int kTimeShiftCapacityGbMin = 1;
static constexpr int kTimeShiftCapacityGbDefault = 100;

// アプリ側の人工的な容量上限は設けない。
// ConfigManager / UI が int で扱うため、実装上の最大は INT_MAX GB。
static constexpr int kTimeShiftCapacityGbMax = INT_MAX;

static int NormalizeTimeShiftCapacityGb_(int gb) noexcept
{
    if (gb < kTimeShiftCapacityGbMin) {
        return kTimeShiftCapacityGbMin;
    }

    return gb;
}

static int ParseHHMMLoose(const std::wstring& s, int defHHMM) noexcept
{
    if (s.empty()) return defHHMM;

    int h = -1, m = -1;

    if (s.find(L':') != std::wstring::npos) {
        if (swscanf_s(s.c_str(), L"%d:%d", &h, &m) == 2) {
            if (0 <= h && h <= 23 && 0 <= m && m <= 59) return h * 100 + m;
        }
        return defHHMM;
    }

    int v = _wtoi(s.c_str());
    h = v / 100;
    m = v % 100;
    if (0 <= h && h <= 23 && 0 <= m && m <= 59) return h * 100 + m;
    return defHHMM;
}

static std::wstring FormatHHMM4(int hhmm)
{
    hhmm = ClampInt(hhmm, 0, 2359);
    wchar_t buf[16]{};
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%04d", hhmm);
    return std::wstring(buf);
}

static std::wstring MakeDefaultTunerName(int idx)
{
    return L"Tuner" + std::to_wstring(idx);
}

static std::wstring MakeTunerSectionNameLocal(int tunerIndex)
{
    if (tunerIndex < 0) {
        tunerIndex = 0;
    }

    return L"Tuner" + std::to_wstring(tunerIndex);
}

static std::wstring MakeDefaultTvTestControlPipeNameLocal(int tunerIndex)
{
    if (tunerIndex < 0) tunerIndex = 0;

    wchar_t buf[64]{};
    _snwprintf_s(
        buf,
        _countof(buf),
        _TRUNCATE,
        L"\\\\.\\pipe\\TVTestBridge_%d",
        tunerIndex);
    return std::wstring(buf);
}

static std::wstring MakeDefaultTvTestTsPipeNameLocal(int tunerIndex)
{
    if (tunerIndex < 0) tunerIndex = 0;

    wchar_t buf[64]{};
    _snwprintf_s(
        buf,
        _countof(buf),
        _TRUNCATE,
        L"\\\\.\\pipe\\TVTestBridgeTS_%d",
        tunerIndex);
    return std::wstring(buf);
}

static std::wstring MakeDefaultTvTestDidLocal(int tunerIndex)
{
    if (tunerIndex < 0) {
        tunerIndex = 0;
    }

    return std::to_wstring(tunerIndex);
}

static ConfigManager::EpgReserveButtonMode ParseEpgReserveButtonModeValue(const std::wstring& s) noexcept
{
    const std::wstring v = ToLowerTrim(s);

    if (v == L"record") {
        return ConfigManager::EpgReserveButtonMode::Record;
    }
    if (v == L"watch") {
        return ConfigManager::EpgReserveButtonMode::Watch;
    }

    return ConfigManager::EpgReserveButtonMode::Select;
}

static ConfigManager::EpgHtmlRenderRoute ParseEpgHtmlRenderRouteValue(const std::wstring& s) noexcept
{
    const std::wstring v = ToLowerTrim(s);



    if (v == L"program_page_writer" || v == L"programpagewriter" || v == L"writer") {
        return ConfigManager::EpgHtmlRenderRoute::ProgramPageWriter;
    }
    if (v == L"plugin") {
        return ConfigManager::EpgHtmlRenderRoute::Plugin;
    }
    if (v == L"auto") {
        return ConfigManager::EpgHtmlRenderRoute::Auto;
    }
    if (v == L"lite" || v == L"epghtmllite" || v == L"epg_html_lite") {
        return ConfigManager::EpgHtmlRenderRoute::Lite;
    }

    return ConfigManager::EpgHtmlRenderRoute::Legacy;
}

static const wchar_t* ToIniEpgHtmlRenderRouteValue(ConfigManager::EpgHtmlRenderRoute route) noexcept
{
    switch (route) {
    case ConfigManager::EpgHtmlRenderRoute::ProgramPageWriter:
        return L"program_page_writer";
    case ConfigManager::EpgHtmlRenderRoute::Plugin:
        return L"plugin";
    case ConfigManager::EpgHtmlRenderRoute::Auto:
        return L"auto";
    case ConfigManager::EpgHtmlRenderRoute::Lite:
        return L"lite";
    case ConfigManager::EpgHtmlRenderRoute::Legacy:
    default:
        return L"legacy";
    }
}

static const wchar_t* ToIniEpgReserveButtonModeValue(ConfigManager::EpgReserveButtonMode mode) noexcept
{
    switch (mode) {
    case ConfigManager::EpgReserveButtonMode::Record:
        return L"record";
    case ConfigManager::EpgReserveButtonMode::Watch:
        return L"watch";
    case ConfigManager::EpgReserveButtonMode::Select:
    default:
        return L"select";
    }
}

static ConfigManager::EpgAutoMode ParseEpgAutoModeValue(const std::wstring& s, ConfigManager::EpgAutoMode fallback) noexcept
{
    const std::wstring v = ToLowerTrim(s);
    if (v == L"task" || v == L"1" || v == L"on" || v == L"true") {
        return ConfigManager::EpgAutoMode::Task;
    }
    if (v == L"off" || v == L"0" || v == L"false") {
        return ConfigManager::EpgAutoMode::Off;
    }
    return fallback;
}

static const wchar_t* ToIniEpgAutoModeValue(ConfigManager::EpgAutoMode mode) noexcept
{
    return (mode == ConfigManager::EpgAutoMode::Task) ? L"Task" : L"Off";
}

ConfigManager::ConfigManager()
{
    iniPath =
        (fs::path(GetExeDir()) / L"config.ini").wstring();

    std::error_code ec;

    const bool hadConfigFile =
        fs::exists(
            fs::path(iniPath),
            ec) && !ec;

    InitializeConfig();
    LoadConfig();

    SyncCpuDetectedAtStartup_(hadConfigFile);
}

ConfigManager& ConfigManager::GetInstance()
{
    static ConfigManager instance;
    return instance;
}

void ConfigManager::LoadConfig()
{
    systemVendor = ReadIniValue(L"System", L"VENDOR", L"Unknown");

    {
        const std::wstring legacyBonDriverPath =
            TrimPathText_(
                ReadIniValue(
                    L"BonDriver",
                    L"Path",
                    L""));

        bonDriverTerrestrialPath =
            TrimPathText_(
                ReadIniValue(
                    L"BonDriver",
                    L"Terrestrial",
                    legacyBonDriverPath));

        bonDriverSatellitePath =
            TrimPathText_(
                ReadIniValue(
                    L"BonDriver",
                    L"Satellite",
                    L""));

        bonDriverDefaultPath =
            TrimPathText_(
                ReadIniValue(
                    L"BonDriver",
                    L"Default",
                    legacyBonDriverPath));

        if (bonDriverTerrestrialPath.empty()) {
            bonDriverTerrestrialPath = legacyBonDriverPath;
        }

        // 旧互換。
        // 既存コードが GetBonDriverPath() を読む間は地上波用を返す。
        bonDriverPath =
            !bonDriverTerrestrialPath.empty()
            ? bonDriverTerrestrialPath
            : legacyBonDriverPath;
    }

    pathAppWorkDir =
        ReadIniValue(L"Path", L"AppWorkDir", L".");

    pathEpgOutDir =
        ReadIniValue(L"Path", L"EpgOutDir", L"epg_out");

    pathEpgDataWorkDir =
        ReadIniValue(L"Path", L"EpgDataWorkDir", L"epg_work");

    if (TrimPathText_(pathAppWorkDir).empty()) {
        pathAppWorkDir = L".";
    }

    if (TrimPathText_(pathEpgOutDir).empty()) {
        pathEpgOutDir = L"epg_out";
    }

    if (TrimPathText_(pathEpgDataWorkDir).empty()) {
        pathEpgDataWorkDir = L"epg_work";
    }

    timeShiftRootFolder =
        ReadIniValue(L"TimeShift", L"RootFolder", L"timeshift");

    timeShiftRootFolder = TrimPathText_(timeShiftRootFolder);
    if (timeShiftRootFolder.empty()) {
        timeShiftRootFolder = L"timeshift";
    }

    if (!IsIniKeyMissing(L"TimeShift", L"MaxBytesGB")) {
        timeShiftMaxBytesGB =
            NormalizeTimeShiftCapacityGb_(
                ReadIniInt(
                    L"TimeShift",
                    L"MaxBytesGB",
                    kTimeShiftCapacityGbDefault));
    }
    else {
        // 旧互換: MaxBytesMB があれば GB に丸めて読む。
        const int legacyMb =
            ReadIniInt(
                L"TimeShift",
                L"MaxBytesMB",
                kTimeShiftCapacityGbDefault * 1024);

        const int convertedGb =
            (legacyMb <= 0)
            ? kTimeShiftCapacityGbDefault
            : ((legacyMb + 1023) / 1024);

        timeShiftMaxBytesGB =
            NormalizeTimeShiftCapacityGb_(convertedGb);
    }

    debugWriteToFile = ParseBool(ReadIniValue(L"Debug", L"WriteToFile", L"0"));
    logEncoding = ReadIniValue(L"Debug", L"LogEncoding", L"utf8");
    enableSDTDebug = ParseBool(ReadIniValue(L"Debug", L"EnableSDTDebug", L"0"));
    enableNITDebug = ParseBool(ReadIniValue(L"Debug", L"EnableNITDebug", L"0"));
    guiLogEnabled = ParseBool(ReadIniValue(L"Debug", L"GuiLogEnabled", L"1"));
    guiLogFile = ReadIniValue(L"Debug", L"GuiLogFile", L"debug.log");

    idleMonitorSampleMs =
        ClampIdleMonitorSampleMs_(
            ReadIniInt(
                L"Debug",
                L"IdleMonitorSampleMs",
                5000));

    idleMonitorConsoleLog =
        ParseBool(
            ReadIniValue(
                L"Debug",
                L"IdleMonitorConsoleLog",
                L"1"));

    idleMonitorSystemCpuPct =
        ClampIdleMonitorPercent_(
            ReadIniInt(
                L"Debug",
                L"IdleMonitorSystemCpuPct",
                20));

    idleMonitorProcessCpuPct =
        ClampIdleMonitorPercent_(
            ReadIniInt(
                L"Debug",
                L"IdleMonitorProcessCpuPct",
                5));

    idleMonitorReadKBps =
        ClampIdleMonitorIoKBps_(
            ReadIniInt(
                L"Debug",
                L"IdleMonitorReadKBps",
                2048));

    idleMonitorWriteKBps =
        ClampIdleMonitorIoKBps_(
            ReadIniInt(
                L"Debug",
                L"IdleMonitorWriteKBps",
                4096));

    idleMonitorMemoryLoadPct =
        ClampIdleMonitorPercent_(
            ReadIniInt(
                L"Debug",
                L"IdleMonitorMemoryLoadPct",
                90));

    selectedCPUFeature =
        NormalizeSelectedCPUFeature_(
            ReadIniValue(L"CPU", L"SelectedFeature", L"AUTO"));

    cpuUseSSE2 = ParseBool(ReadIniValue(L"CPU", L"USE_SSE2", L"1"));
    cpuUseSSE3 = ParseBool(ReadIniValue(L"CPU", L"USE_SSE3", L"1"));
    cpuUseSSSE3 = ParseBool(ReadIniValue(L"CPU", L"USE_SSSE3", L"1"));
    cpuUseSSE4_1 = ParseBool(ReadIniValue(L"CPU", L"USE_SSE4_1", L"1"));
    cpuUseSSE4_2 = ParseBool(ReadIniValue(L"CPU", L"USE_SSE4_2", L"1"));

    cpuUseAVX = ParseBool(ReadIniValue(L"CPU", L"USE_AVX", L"1"));
    cpuUseAVX2 = ParseBool(ReadIniValue(L"CPU", L"USE_AVX2", L"1"));
    cpuUseAVX512 = ParseBool(ReadIniValue(L"CPU", L"USE_AVX512", L"0"));

    cpuUseFMA = ParseBool(ReadIniValue(L"CPU", L"USE_FMA", L"1"));
    cpuUsePOPCNT = ParseBool(ReadIniValue(L"CPU", L"USE_POPCNT", L"1"));
    cpuUseAESNI = ParseBool(ReadIniValue(L"CPU", L"USE_AES_NI", L"1"));
    cpuUseSHA = ParseBool(ReadIniValue(L"CPU", L"USE_SHA", L"1"));
    cpuUseBMI1 = ParseBool(ReadIniValue(L"CPU", L"USE_BMI1", L"1"));
    cpuUseBMI2 = ParseBool(ReadIniValue(L"CPU", L"USE_BMI2", L"1"));

    cpuEnableParallelWorkers =
        ParseBool(
            ReadIniValue(
                L"CPU",
                L"EnableParallelWorkers",
                L"1"));

    cpuRuntimeWorkerReserveCores =
        ClampUInt32_(
            ReadIniInt(
                L"CPU",
                L"RuntimeWorkerReserveCores",
                1),
            1u,
            0u,
            64u);

    cpuRuntimeMaxWorkerThreads =
        ClampUInt32_(
            ReadIniInt(
                L"CPU",
                L"RuntimeMaxWorkerThreads",
                0),
            0u,
            0u,
            256u);

    LoadCpuDetectedFromIni_();

    aribEnableTrace = ParseBool(ReadIniValue(L"Arib", L"EnableTrace", L"0"));
    aribTraceG = ParseBool(ReadIniValue(L"Arib", L"TraceG", aribEnableTrace ? L"1" : L"0"));
    aribTraceChars = ParseBool(ReadIniValue(L"Arib", L"TraceChars", L"0"));

    channelScanEnableTsDump = ParseBool(ReadIniValue(L"ChannelScan", L"EnableTsDump", L"0"));
    channelScanEnableRingDump = ParseBool(ReadIniValue(L"ChannelScan", L"EnableRingDump", L"0"));

    channelScanFixedSignalLevelThreshold = ReadIniFloat(L"ChannelScanFixed", L"SignalLevelThreshold", 0.0f);
    channelScanFixedLockWaitMs = ReadIniInt(L"ChannelScanFixed", L"LockWaitMs", 600);
    channelScanFixedTsCheckWaitMs = ReadIniInt(L"ChannelScanFixed", L"TsCheckWaitMs", 800);
    channelScanFixedTsCheckPollMs = ReadIniInt(L"ChannelScanFixed", L"TsCheckPollMs", 50);
    channelScanFixedMinTsBytes = ReadIniSizeT(L"ChannelScanFixed", L"MinTsBytes", 940);
    channelScanFixedDropVideoAudioSubtitle = ParseBool(ReadIniValue(L"ChannelScanFixed", L"DropVideoAudioSubtitle", L"1"));
    channelScanFixedSavePerSpace = ParseBool(ReadIniValue(L"ChannelScanFixed", L"SavePerSpace", L"1"));
    channelScanFixedLogConfigSnapshot = ParseBool(ReadIniValue(L"ChannelScanFixed", L"LogConfigSnapshot", L"1"));
    channelScanFixedCaptureSec = ReadIniInt(L"ChannelScanFixed", L"CaptureSec", 3);
    channelScanFixedRingBufferSizeBytes = ReadIniSizeT(L"ChannelScanFixed", L"RingBufferSizeBytes", (32ull * 1024ull * 1024ull));
    channelScanFixedPacketReserve = ReadIniSizeT(L"ChannelScanFixed", L"PacketReserve", 4096);
    channelScanFixedSleepOnFailMs = ReadIniInt(L"ChannelScanFixed", L"SleepOnFailMs", 1);
    channelScanFixedScanAllSpaces = ParseBool(ReadIniValue(L"ChannelScanFixed", L"ScanAllSpaces", L"0"));
    channelScanFixedMaxSpaceToTry = ReadIniInt(L"ChannelScanFixed", L"MaxSpaceToTry", 7);
    channelScanFixedDefaultSpace = ReadIniInt(L"ChannelScanFixed", L"DefaultSpace", 0);

    uiMinimizeToTray = ParseBool(ReadIniValue(L"UI", L"MinimizeToTray", L"1"));
    uiStartupEnabled = ParseBool(ReadIniValue(L"UI", L"StartupEnabled", L"0"));
    uiWindowX = ReadIniInt(L"UI", L"WindowX", -1);
    uiWindowY = ReadIniInt(L"UI", L"WindowY", -1);
    uiWindowW = ReadIniInt(L"UI", L"WindowW", 1100);
    uiWindowH = ReadIniInt(L"UI", L"WindowH", 720);
    uiLastTabIndex = ReadIniInt(L"UI", L"LastTabIndex", 0);

    tunersCount = ReadIniInt(L"Tuners", L"Count", 1);
    if (tunersCount < 1) tunersCount = 1;

    tunersActiveIndex = ReadIniInt(L"Tuners", L"ActiveIndex", 0);
    if (tunersActiveIndex < 0) tunersActiveIndex = 0;
    if (tunersActiveIndex >= tunersCount) tunersActiveIndex = tunersCount - 1;

    tunerGlobalUseCommonBonDriver =
        ParseBool(ReadIniValue(L"TunerGlobal", L"UseCommonBonDriver", L"0"));
    tunerGlobalCommonBonDriverPath =
        ReadIniValue(L"TunerGlobal", L"CommonBonDriverPath", L"");
    tunerGlobalConflictPolicy =
        static_cast<TunerGlobalConflictPolicy>(ClampInt(ReadIniInt(L"TunerGlobal", L"ConflictPolicy", 0), 0, 2));
    tunerGlobalPauseEpgWhileRecording =
        ParseBool(ReadIniValue(L"TunerGlobal", L"PauseEpgWhileRecording", L"1"));
    tunerGlobalPreferRecordingOverEpg =
        ParseBool(ReadIniValue(L"TunerGlobal", L"PreferRecordingOverEpg", L"1"));
    tunerGlobalCommonExternalAppPath =
        ReadIniValue(L"TunerGlobal", L"CommonExternalAppPath", L"");

    EnsureTunerVectors_();

    for (int i = 0; i < tunersCount; ++i) {
        const std::size_t idx = static_cast<std::size_t>(i);
        const std::wstring tunerSection = MakeTunerSectionNameLocal(i);

        const auto readTunerBlockString =
            [this, &tunerSection, i](
                const wchar_t* baseKey,
                const wchar_t* oldSection,
                const wchar_t* oldBaseKey,
                const std::wstring& fallback) -> std::wstring
            {
                if (baseKey == nullptr || baseKey[0] == L'\0') {
                    return fallback;
                }

                const std::wstring indexedNewKey = MakeIndexedKey_(baseKey, i);

                // 新形式・番号付き:
                //   [Tuner0] BonDriverPath0=...
                //   [Tuner1] BonDriverPath1=...
                //
                // キーが存在するなら、空文字でもその値を採用する。
                // これにより ExternalAppPath1= のような「意図した空欄」が旧設定で復活しない。
                if (!IsIniKeyMissing(tunerSection, indexedNewKey)) {
                    return ReadIniValue(tunerSection, indexedNewKey, L"");
                }

                // 新形式・番号なし互換:
                //   [Tuner0] BonDriverPath=...
                //
                // こちらもキーが存在するなら空文字を採用する。
                if (!IsIniKeyMissing(tunerSection, baseKey)) {
                    return ReadIniValue(tunerSection, baseKey, L"");
                }

                // 旧形式互換:
                //   [TunerSettings] BonDriverPath0=...
                //   [TunerTVTest] ControlPipeName0=...
                //
                // 新形式キーが存在しない場合だけ旧形式を見る。
                if (oldSection != nullptr && oldBaseKey != nullptr) {
                    const std::wstring oldKey = MakeIndexedKey_(oldBaseKey, i);

                    if (!IsIniKeyMissing(oldSection, oldKey)) {
                        return ReadIniValue(oldSection, oldKey, fallback);
                    }
                }

                return fallback;
            };

        const auto readTunerBlockInt =
            [&readTunerBlockString](
                const wchar_t* baseKey,
                const wchar_t* oldSection,
                const wchar_t* oldBaseKey,
                int fallback) -> int
            {
                const std::wstring raw =
                    ToLowerTrim(readTunerBlockString(
                        baseKey,
                        oldSection,
                        oldBaseKey,
                        std::to_wstring(fallback)));

                if (raw.empty()) {
                    return fallback;
                }

                wchar_t* end = nullptr;
                const long v = std::wcstol(raw.c_str(), &end, 10);
                if (end == raw.c_str()) {
                    return fallback;
                }

                return static_cast<int>(v);
            };

        const auto readTunerBlockBool =
            [this, &readTunerBlockString](
                const wchar_t* baseKey,
                const wchar_t* oldSection,
                const wchar_t* oldBaseKey,
                bool fallback) -> bool
            {
                const std::wstring raw =
                    readTunerBlockString(
                        baseKey,
                        oldSection,
                        oldBaseKey,
                        fallback ? L"1" : L"0");

                if (raw.empty()) {
                    return fallback;
                }

                return ParseBool(raw);
            };

        const std::wstring oldEnableKey = MakeIndexedKey_(L"Enable", i);
        const std::wstring oldNameKey = MakeIndexedKey_(L"Name", i);
        const std::wstring oldBonDriverPathKey = MakeIndexedKey_(L"Path", i);

        const std::wstring defName = MakeDefaultTunerName(i);

        bonDriverPathPerTuner[idx] =
            readTunerBlockString(
                L"BonDriverPath",
                L"TunerSettings",
                L"BonDriverPath",
                ReadIniValue(L"BonDriver", oldBonDriverPathKey, L""));

        tunerInternalBonDriverPathPerTuner[idx] =
            readTunerBlockString(
                L"InternalBonDriverPath",
                L"TunerSettings",
                L"BonDriverPath",
                bonDriverPathPerTuner[idx]);

        tunerTvTestBonDriverPathPerTuner[idx] =
            readTunerBlockString(
                L"TvTestBonDriverPath",
                L"TunerTVTest",
                L"BonDriverPath",
                bonDriverPathPerTuner[idx]);

        tunerEnablePerTuner[idx] =
            readTunerBlockBool(
                L"Enable",
                L"TunerSettings",
                L"Enable",
                ParseBool(ReadIniValue(L"Tuners", oldEnableKey, L"1"))) ? 1 : 0;

        std::wstring name =
            readTunerBlockString(
                L"Name",
                L"TunerSettings",
                L"Name",
                ReadIniValue(L"Tuners", oldNameKey, defName));

        if (name.empty()) {
            name = defName;
        }

        tunerNamePerTuner[idx] = std::move(name);

        tunerExternalAppPathPerTuner[idx] =
            readTunerBlockString(
                L"ExternalAppPath",
                L"TunerSettings",
                L"ExternalAppPath",
                L"");

        tunerReceiveModePerTuner[idx] =
            ClampInt(
                readTunerBlockInt(
                    L"ReceiveMode",
                    L"TunerSettings",
                    L"ReceiveMode",
                    0),
                0,
                3);

        tunerUseRolePerTuner[idx] =
            ClampInt(
                readTunerBlockInt(
                    L"UseRole",
                    L"TunerSettings",
                    L"UseRole",
                    0),
                0,
                5);

        tunerAllowParallelPerTuner[idx] =
            readTunerBlockBool(
                L"AllowParallel",
                L"TunerSettings",
                L"AllowParallel",
                true) ? 1 : 0;

        tunerPriorityPerTuner[idx] =
            ClampInt(
                readTunerBlockInt(
                    L"Priority",
                    L"TunerSettings",
                    L"Priority",
                    i),
                -100,
                100);

        tunerTimeShiftLockSpacePerTuner[idx] =
            ClampInt(
                readTunerBlockInt(
                    L"TimeShiftLockSpace",
                    L"TunerSettings",
                    L"TimeShiftLockSpace",
                    0),
                0,
                15);

        tunerTimeShiftLockChannelPerTuner[idx] =
            ClampInt(
                readTunerBlockInt(
                    L"TimeShiftLockChannel",
                    L"TunerSettings",
                    L"TimeShiftLockChannel",
                    -1),
                -1,
                4096);

        tunerTimeShiftLockServiceIdPerTuner[idx] =
            ClampInt(
                readTunerBlockInt(
                    L"TimeShiftLockServiceId",
                    L"TunerSettings",
                    L"TimeShiftLockServiceId",
                    0),
                0,
                65535);

        tunerTimeShiftLockNetworkIdPerTuner[idx] =
            ClampInt(
                readTunerBlockInt(
                    L"TimeShiftLockNetworkId",
                    L"TunerSettings",
                    L"TimeShiftLockNetworkId",
                    0),
                0,
                65535);

        tunerTimeShiftLockTsIdPerTuner[idx] =
            ClampInt(
                readTunerBlockInt(
                    L"TimeShiftLockTsId",
                    L"TunerSettings",
                    L"TimeShiftLockTsId",
                    0),
                0,
                65535);

        tunerTimeShiftLockNamePerTuner[idx] =
            readTunerBlockString(
                L"TimeShiftLockName",
                L"TunerSettings",
                L"TimeShiftLockName",
                L"");

        tunerUseExternalAppPerTuner[idx] =
            readTunerBlockBool(
                L"UseExternalApp",
                L"TunerSettings",
                L"UseExternalApp",
                false) ? 1 : 0;

        tunerTvTestUseBridgePerTuner[idx] =
            readTunerBlockBool(
                L"TvTestUseBridge",
                L"TunerTVTest",
                L"UseBridge",
                false) ? 1 : 0;

        tunerTvTestDidPerTuner[idx] =
            readTunerBlockString(
                L"TvTestDid",
                L"TunerTVTest",
                L"DID",
                MakeDefaultTvTestDidLocal(i));

        if (tunerTvTestDidPerTuner[idx].empty()) {
            tunerTvTestDidPerTuner[idx] = MakeDefaultTvTestDidLocal(i);
        }

        tunerTvTestExePathPerTuner[idx] =
            readTunerBlockString(
                L"TvTestExePath",
                L"TunerTVTest",
                L"ExePath",
                tunerExternalAppPathPerTuner[idx]);

        tunerTvTestControlPipeNamePerTuner[idx] =
            readTunerBlockString(
                L"TvTestControlPipeName",
                L"TunerTVTest",
                L"ControlPipeName",
                MakeDefaultTvTestControlPipeName_(i));

        tunerTvTestTsPipeNamePerTuner[idx] =
            readTunerBlockString(
                L"TvTestTsPipeName",
                L"TunerTVTest",
                L"TsPipeName",
                MakeDefaultTvTestTsPipeName_(i));

        // 多重起動用の安全補正。
        // 旧設定から \\.\pipe\TVTestBridge を引き継いだ場合は Tuner別名へ補正する。
        if (tunersCount >= 2) {
            if (tunerTvTestControlPipeNamePerTuner[idx].empty() ||
                tunerTvTestControlPipeNamePerTuner[idx] == L"\\\\.\\pipe\\TVTestBridge") {
                tunerTvTestControlPipeNamePerTuner[idx] =
                    MakeDefaultTvTestControlPipeName_(i);
            }

            if (tunerTvTestTsPipeNamePerTuner[idx].empty() ||
                tunerTvTestTsPipeNamePerTuner[idx] == L"\\\\.\\pipe\\TVTestBridgeTS") {
                tunerTvTestTsPipeNamePerTuner[idx] =
                    MakeDefaultTvTestTsPipeName_(i);
            }
        }
        else {
            if (tunerTvTestControlPipeNamePerTuner[idx].empty()) {
                tunerTvTestControlPipeNamePerTuner[idx] =
                    MakeDefaultTvTestControlPipeName_(i);
            }

            if (tunerTvTestTsPipeNamePerTuner[idx].empty()) {
                tunerTvTestTsPipeNamePerTuner[idx] =
                    MakeDefaultTvTestTsPipeName_(i);
            }
        }

        tunerTvTestCommandPerTuner[idx] =
            readTunerBlockString(
                L"TvTestCommand",
                L"TunerTVTest",
                L"Command",
                L"PING");

        if (tunerTvTestCommandPerTuner[idx].empty()) {
            tunerTvTestCommandPerTuner[idx] = L"PING";
        }
    }

    // EPG update settings
    {
        if (!IsIniKeyMissing(L"EPGUpdate", L"EpgAutoMode")) {
            epgAutoMode = ParseEpgAutoModeValue(
                ReadIniValue(L"EPGUpdate", L"EpgAutoMode", L"Off"),
                EpgAutoMode::Off);
        }
        else {
            const int mode = ReadIniInt(L"EPGUpdate", L"AutoMode", 0);
            epgAutoMode = (mode == 1) ? EpgAutoMode::Task : EpgAutoMode::Off;
        }

        epgBootGraceSec = ReadIniInt(
            L"EPGUpdate",
            L"EpgBootGraceSec",
            ReadIniInt(L"EPGUpdate", L"BootGraceSec", 60));
        epgCatchUpOnStart = ParseBool(ReadIniValue(
            L"EPGUpdate",
            L"EpgCatchUpOnStart",
            ReadIniValue(L"EPGUpdate", L"CatchUpOnStart", L"1")));
        epgTickSeconds = ReadIniInt(
            L"EPGUpdate",
            L"EpgTickSeconds",
            ReadIniInt(L"EPGUpdate", L"TickSeconds", 1));
        epgIdleRequiredSec = ReadIniInt(
            L"EPGUpdate",
            L"EpgIdleRequiredSec",
            ReadIniInt(L"EPGUpdate", L"IdleRequiredSec", 30));

        epgEnableDailyFull = ParseBool(ReadIniValue(
            L"EPGUpdate",
            L"EpgEnableDailyFull",
            ReadIniValue(L"EPGUpdate", L"EnableDailyFull", L"1")));
        const std::wstring hhmmStr = ReadIniValue(
            L"EPGUpdate",
            L"EpgDailyFullTimeHHMM",
            ReadIniValue(L"EPGUpdate", L"DailyFullUpdateHHMM", L"0330"));
        epgDailyFullHHMM = ParseHHMMLoose(hhmmStr, 330);

        epgEnableHourly = ParseBool(ReadIniValue(
            L"EPGUpdate",
            L"EpgEnableHourly",
            ReadIniValue(L"EPGUpdate", L"EnableHourly", L"0")));
        epgHourlyAtMinute = ReadIniInt(
            L"EPGUpdate",
            L"EpgHourlyAtMinute",
            ReadIniInt(L"EPGUpdate", L"HourlyAtMinute", 5));
        epgLightUpdateIntervalMin = ReadIniInt(
            L"EPGUpdate",
            L"EpgLightUpdateIntervalMin",
            ReadIniInt(L"EPGUpdate", L"LightUpdateIntervalMin", 60));

        manualFullEstimatedMinutes = ReadIniInt(L"EPGUpdate", L"ManualFullEstimatedMinutes", 90);
        manualFullGuardMinutes = ReadIniInt(L"EPGUpdate", L"ManualFullGuardMinutes", 30);
        manualLightBaseMinutes = ReadIniInt(L"EPGUpdate", L"ManualLightBaseMinutes", 15);
        manualLightPerChannelMinutes = ReadIniInt(L"EPGUpdate", L"ManualLightPerChannelMinutes", 3);
        manualLightGuardMinutes = ReadIniInt(L"EPGUpdate", L"ManualLightGuardMinutes", 30);

        // 新構成:
//   [EPGUpdate] EpgHtmlRenderRoute
//
// 旧構成:
//   [EPGUpdate] HtmlRenderRoute
//
// 旧config.iniとの互換のため、旧キーも fallback として読む。
        epgHtmlRenderRoute =
            ParseEpgHtmlRenderRouteValue(
                ReadIniValue(
                    L"EPGUpdate",
                    L"EpgHtmlRenderRoute",
                    ReadIniValue(L"EPGUpdate", L"HtmlRenderRoute", L"legacy")));

        epgBootGraceSec = ClampInt(epgBootGraceSec, 0, 3600);
        epgTickSeconds = ClampInt(epgTickSeconds, 1, 60);
        epgIdleRequiredSec = ClampInt(epgIdleRequiredSec, 0, 24 * 3600);
        epgDailyFullHHMM = ClampInt(epgDailyFullHHMM, 0, 2359);
        epgHourlyAtMinute = ClampInt(epgHourlyAtMinute, 0, 59);
        epgLightUpdateIntervalMin = ClampInt(epgLightUpdateIntervalMin, 0, 24 * 60);
        manualFullEstimatedMinutes = ClampMinutes_(manualFullEstimatedMinutes, 1, 24 * 60);
        manualFullGuardMinutes = ClampMinutes_(manualFullGuardMinutes, 0, 24 * 60);
        manualLightBaseMinutes = ClampMinutes_(manualLightBaseMinutes, 1, 24 * 60);
        manualLightPerChannelMinutes = ClampMinutes_(manualLightPerChannelMinutes, 0, 24 * 60);
        manualLightGuardMinutes = ClampMinutes_(manualLightGuardMinutes, 0, 24 * 60);
    }

    // Reservation settings
    {
        reservationStartAdvanceSec = ReadIniInt(L"Reservation", L"StartAdvanceSec", 30);
        reservationStartAdjustSec = ReadIniInt(L"Reservation", L"StartAdjustSec", 0);
        reservationEndAdjustMin = ReadIniInt(L"Reservation", L"EndAdjustMin", 0);
        reservationPreferredTuner = ReadIniInt(L"Reservation", L"PreferredTuner", -1);

        const int cp = ReadIniInt(L"Reservation", L"ConflictPolicy", 0);
        reservationConflictPolicy = static_cast<ReservationConflictPolicy>(ClampInt(cp, 0, 2));

        reservationPauseEpgWhileRecording =
            ParseBool(ReadIniValue(L"Reservation", L"PauseEpgWhileRecording", L"1"));

        reservationCleanupCompletedAfterEpgBuild =
            ParseBool(ReadIniValue(L"Reservation", L"CleanupCompletedAfterEpgBuild", L"1"));

        reservationEpgReserveButtonMode =
            ParseEpgReserveButtonModeValue(
                ReadIniValue(L"Reservation", L"EpgReserveButtonMode", L"select"));

        reservationStartAdvanceSec = ClampInt(reservationStartAdvanceSec, 0, 600);
        reservationStartAdjustSec = ClampInt(reservationStartAdjustSec, -1, 1);
        reservationEndAdjustMin = ClampInt(reservationEndAdjustMin, -30, 180);

        if (reservationPreferredTuner < -1) reservationPreferredTuner = -1;
        if (reservationPreferredTuner >= tunersCount) reservationPreferredTuner = -1;
    }

    // Reservation execution settings
    {
        reservationExecutionEnableExecution =
            ParseBool(ReadIniValue(L"ReservationExecution", L"EnableExecution", L"1"));

        reservationExecutionEnableWatchReservation =
            ParseBool(ReadIniValue(L"ReservationExecution", L"EnableWatchReservation", L"1"));

        reservationExecutionEnableTvTestWatch =
            ParseBool(ReadIniValue(L"ReservationExecution", L"EnableTvTestWatch", L"1"));

        reservationExecutionEnableRecordExecution =
            ParseBool(ReadIniValue(L"ReservationExecution", L"EnableRecordExecution", L"1"));

        reservationExecutionEnableTsPipeDuringWatch =
            ParseBool(ReadIniValue(L"ReservationExecution", L"EnableTsPipeDuringWatch", L"0"));

        reservationExecutionTvTestRecordShowVideo =
            ParseBool(ReadIniValue(L"ReservationExecution", L"TvTestRecordShowVideo", L"1"));

        reservationExecutionRecordFolder =
            ReadIniValue(L"ReservationExecution", L"RecordFolder", L"");

        reservationExecutionPrepareSeconds =
            ClampPrepareSeconds_(ReadIniInt(L"ReservationExecution", L"PrepareSeconds", 30));
    }

    // チャンネルスキャンの固定設定のうち、論理的に不正な値を自動修正する
    if (channelScanFixedLockWaitMs < 0) channelScanFixedLockWaitMs = 0;
    if (channelScanFixedTsCheckWaitMs < 0) channelScanFixedTsCheckWaitMs = 0;
    if (channelScanFixedTsCheckPollMs < 1) channelScanFixedTsCheckPollMs = 1;
    if (channelScanFixedCaptureSec < 1) channelScanFixedCaptureSec = 1;
    if (channelScanFixedMaxSpaceToTry < 0) channelScanFixedMaxSpaceToTry = 0;

    NormalizeTimeShiftRolesFromLocks_();

    // Load直後はINI内容を正とするので、未保存変更扱いにしない。
    timeShiftTunerSettingsDirty_ = false;
}


std::wstring ConfigManager::GetBonDriverPath() const
{
    return GetBonDriverTerrestrialPath();
}

void ConfigManager::SetBonDriverPath(const std::wstring& path)
{
    const std::wstring normalized =
        TrimPathText_(path);

    bonDriverPath = normalized;
    bonDriverTerrestrialPath = normalized;

    if (TrimPathText_(bonDriverDefaultPath).empty()) {
        bonDriverDefaultPath = normalized;
    }
}

std::wstring ConfigManager::GetBonDriverTerrestrialPath() const
{
    const std::wstring terrestrial =
        TrimPathText_(bonDriverTerrestrialPath);

    if (!terrestrial.empty()) {
        return terrestrial;
    }

    const std::wstring legacy =
        TrimPathText_(bonDriverPath);

    if (!legacy.empty()) {
        return legacy;
    }

    return TrimPathText_(bonDriverDefaultPath);
}

void ConfigManager::SetBonDriverTerrestrialPath(
    const std::wstring& path)
{
    const std::wstring normalized =
        TrimPathText_(path);

    bonDriverTerrestrialPath = normalized;

    // 旧互換。
    // 既存コードが GetBonDriverPath() / [BonDriver] Path を使う間は、
    // 地上波BonDriverを本体BonDriverとして扱う。
    bonDriverPath = normalized;

    if (TrimPathText_(bonDriverDefaultPath).empty()) {
        bonDriverDefaultPath = normalized;
    }
}

std::wstring ConfigManager::GetBonDriverSatellitePath() const
{
    return TrimPathText_(bonDriverSatellitePath);
}

void ConfigManager::SetBonDriverSatellitePath(
    const std::wstring& path)
{
    bonDriverSatellitePath =
        TrimPathText_(path);
}

std::wstring ConfigManager::GetBonDriverDefaultPath() const
{
    const std::wstring def =
        TrimPathText_(bonDriverDefaultPath);

    if (!def.empty()) {
        return def;
    }

    return TrimPathText_(bonDriverPath);
}

void ConfigManager::SetBonDriverDefaultPath(
    const std::wstring& path)
{
    bonDriverDefaultPath =
        TrimPathText_(path);
}

std::wstring ConfigManager::GetBonDriverPathForSpace(
    int space) const
{
    switch (space) {
    case 0:
    {
        const std::wstring terrestrial =
            GetBonDriverTerrestrialPath();

        if (!terrestrial.empty()) {
            return terrestrial;
        }

        return GetBonDriverDefaultPath();
    }

    case 1:
    case 2:
    {
        // 衛星用が未設定なら空で返す。
        // 地上波BonDriverへ勝手に落とすと、別DLL構成の機種で誤ロードする。
        return GetBonDriverSatellitePath();
    }

    default:
    {
        const std::wstring def =
            GetBonDriverDefaultPath();

        if (!def.empty()) {
            return def;
        }

        return GetBonDriverTerrestrialPath();
    }
    }
}

std::wstring ConfigManager::GetMainBonDriverPath() const
{
    return GetBonDriverTerrestrialPath();
}

std::wstring ConfigManager::GetChannelScanBonDriverPath() const
{
    return GetBonDriverPathForSpace(channelScanFixedDefaultSpace);
}

std::wstring ConfigManager::GetChannelScanBonDriverPathForSpace(
    int space) const
{
    return GetBonDriverPathForSpace(space);
}

std::wstring ConfigManager::GetMultiTunerBonDriverPath(int tunerIndex) const
{
    // EPG更新・内部録画・予約実行など、
    // アプリ本体が直接 LoadLibrary する用途。
    // TVTest用 BonDriver は絶対にここでは使わない。

    std::wstring internalPath = GetTunerInternalBonDriverPath(tunerIndex);
    internalPath = TrimPathText_(internalPath);

    if (!internalPath.empty()) {
        return internalPath;
    }

    // 既存互換。
    // 旧設定では BonDriverPathX が内部用として使われていたため fallback する。
    std::wstring legacyPath = GetBonDriverPathForTuner(tunerIndex);
    legacyPath = TrimPathText_(legacyPath);

    if (!legacyPath.empty()) {
        return legacyPath;
    }

    // 多重責務では [BonDriver] Path へ落とさない。
    // ここで本体用パスへ落とすと、チューナー番号指定を無視して
    // 内部録画/EPG更新が別BonDriverを掴む事故になる。
    return L"";
}

std::wstring ConfigManager::ResolvePathFromAppBase(const std::wstring& pathText) const
{
    return ResolvePathFromBase_(
        GetExeDir(),
        pathText,
        L".");
}

std::wstring ConfigManager::ResolvePathFromWorkDir(const std::wstring& pathText) const
{
    return ResolvePathFromBase_(
        GetPathAppWorkDir(),
        pathText,
        L".");
}

std::wstring ConfigManager::GetPathAppWorkDirRaw() const
{
    return pathAppWorkDir;
}

void ConfigManager::SetPathAppWorkDirRaw(const std::wstring& path)
{
    pathAppWorkDir = TrimPathText_(path);

    if (pathAppWorkDir.empty()) {
        pathAppWorkDir = L".";
    }
}

std::wstring ConfigManager::GetPathAppWorkDir() const
{
    return ResolvePathFromBase_(
        GetExeDir(),
        pathAppWorkDir,
        L".");
}

std::wstring ConfigManager::GetPathEpgOutDirRaw() const
{
    return pathEpgOutDir;
}

void ConfigManager::SetPathEpgOutDirRaw(const std::wstring& path)
{
    pathEpgOutDir = TrimPathText_(path);

    if (pathEpgOutDir.empty()) {
        pathEpgOutDir = L"epg_out";
    }
}

std::wstring ConfigManager::GetPathEpgOutDir() const
{
    return ResolvePathFromBase_(
        GetPathAppWorkDir(),
        pathEpgOutDir,
        L"epg_out");
}

std::wstring ConfigManager::GetPathEpgDataWorkDirRaw() const
{
    return pathEpgDataWorkDir;
}

void ConfigManager::SetPathEpgDataWorkDirRaw(const std::wstring& path)
{
    pathEpgDataWorkDir = TrimPathText_(path);

    if (pathEpgDataWorkDir.empty()) {
        pathEpgDataWorkDir = L"epg_work";
    }
}

std::wstring ConfigManager::GetPathEpgDataWorkDir() const
{
    return ResolvePathFromBase_(
        GetPathAppWorkDir(),
        pathEpgDataWorkDir,
        L"epg_work");
}

std::wstring ConfigManager::GetTimeShiftRootFolderRaw() const
{
    const std::wstring path = TrimPathText_(timeShiftRootFolder);

    return path.empty() ? L"timeshift" : path;
}

void ConfigManager::SetTimeShiftRootFolderRaw(const std::wstring& path)
{
    timeShiftRootFolder = TrimPathText_(path);

    if (timeShiftRootFolder.empty()) {
        timeShiftRootFolder = L"timeshift";
    }
}

std::wstring ConfigManager::GetTimeShiftRootFolder() const
{
    return ResolvePathFromAppBase(GetTimeShiftRootFolderRaw());
}

int ConfigManager::GetTimeShiftMaxBytesGB() const noexcept
{
    return NormalizeTimeShiftCapacityGb_(timeShiftMaxBytesGB);
}

void ConfigManager::SetTimeShiftMaxBytesGB(int gb) noexcept
{
    timeShiftMaxBytesGB = NormalizeTimeShiftCapacityGb_(gb);
}

int ConfigManager::GetTimeShiftMaxBytesMB() const noexcept
{
    const int gb = GetTimeShiftMaxBytesGB();

    // 旧互換用。MBはintなので、大容量では飽和させる。
    if (gb > INT_MAX / 1024) {
        return INT_MAX;
    }

    return gb * 1024;
}

void ConfigManager::SetTimeShiftMaxBytesMB(int mb) noexcept
{
    const int gb =
        (mb <= 0)
        ? kTimeShiftCapacityGbDefault
        : ((mb + 1023) / 1024);

    SetTimeShiftMaxBytesGB(gb);
}

bool ConfigManager::IsTimeShiftLockConfiguredValues_(
    int channel,
    int serviceId,
    int networkId,
    int tsId) noexcept
{
    if (channel < 0) {
        return false;
    }

    // 地上波チャンネル番号だけだと誤検出しやすいので、
    // SID/NID/TSID のどれかが入っている場合を TimeShift target と見る。
    return serviceId > 0 ||
        networkId > 0 ||
        tsId > 0;
}

bool ConfigManager::IsTimeShiftLockConfiguredAt_(
    int tunerIndex) const noexcept
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) {
        return false;
    }

    const auto idx =
        static_cast<std::size_t>(tunerIndex);

    if (idx >= tunerTimeShiftLockChannelPerTuner.size() ||
        idx >= tunerTimeShiftLockServiceIdPerTuner.size() ||
        idx >= tunerTimeShiftLockNetworkIdPerTuner.size() ||
        idx >= tunerTimeShiftLockTsIdPerTuner.size()) {
        return false;
    }

    return IsTimeShiftLockConfiguredValues_(
        tunerTimeShiftLockChannelPerTuner[idx],
        tunerTimeShiftLockServiceIdPerTuner[idx],
        tunerTimeShiftLockNetworkIdPerTuner[idx],
        tunerTimeShiftLockTsIdPerTuner[idx]);
}

int ConfigManager::ReadExistingTunerIntForSave_(
    int tunerIndex,
    const wchar_t* baseKey,
    const wchar_t* oldSection,
    const wchar_t* oldBaseKey,
    int fallback) const
{
    if (baseKey == nullptr || baseKey[0] == L'\0') {
        return fallback;
    }

    const std::wstring tunerSection =
        MakeTunerSectionNameLocal(tunerIndex);

    const std::wstring indexedKey =
        MakeIndexedKey_(baseKey, tunerIndex);

    if (!IsIniKeyMissing(tunerSection, indexedKey)) {
        return ReadIniInt(tunerSection, indexedKey, fallback);
    }

    if (!IsIniKeyMissing(tunerSection, baseKey)) {
        return ReadIniInt(tunerSection, baseKey, fallback);
    }

    if (oldSection != nullptr &&
        oldSection[0] != L'\0' &&
        oldBaseKey != nullptr &&
        oldBaseKey[0] != L'\0') {
        const std::wstring oldKey =
            MakeIndexedKey_(oldBaseKey, tunerIndex);

        if (!IsIniKeyMissing(oldSection, oldKey)) {
            return ReadIniInt(oldSection, oldKey, fallback);
        }
    }

    return fallback;
}

std::wstring ConfigManager::ReadExistingTunerStringForSave_(
    int tunerIndex,
    const wchar_t* baseKey,
    const wchar_t* oldSection,
    const wchar_t* oldBaseKey,
    const std::wstring& fallback) const
{
    if (baseKey == nullptr || baseKey[0] == L'\0') {
        return fallback;
    }

    const std::wstring tunerSection =
        MakeTunerSectionNameLocal(tunerIndex);

    const std::wstring indexedKey =
        MakeIndexedKey_(baseKey, tunerIndex);

    if (!IsIniKeyMissing(tunerSection, indexedKey)) {
        return ReadIniValue(tunerSection, indexedKey, fallback);
    }

    if (!IsIniKeyMissing(tunerSection, baseKey)) {
        return ReadIniValue(tunerSection, baseKey, fallback);
    }

    if (oldSection != nullptr &&
        oldSection[0] != L'\0' &&
        oldBaseKey != nullptr &&
        oldBaseKey[0] != L'\0') {
        const std::wstring oldKey =
            MakeIndexedKey_(oldBaseKey, tunerIndex);

        if (!IsIniKeyMissing(oldSection, oldKey)) {
            return ReadIniValue(oldSection, oldKey, fallback);
        }
    }

    return fallback;
}

void ConfigManager::NormalizeTimeShiftRolesFromLocks_() noexcept
{
    try {
        EnsureTunerVectors_();

        const int timeShiftRole =
            static_cast<int>(TunerUseRole::TimeShiftOnly);

        for (int i = 0; i < tunersCount; ++i) {
            const auto idx =
                static_cast<std::size_t>(i);

            if (idx >= tunerUseRolePerTuner.size()) {
                continue;
            }

            if (IsTimeShiftLockConfiguredAt_(i)) {
                tunerUseRolePerTuner[idx] = timeShiftRole;
            }
        }
    }
    catch (...) {
    }
}

void ConfigManager::PreserveTimeShiftTunerSettingsBeforeFullSave_() noexcept
{
    try {
        EnsureTunerVectors_();

        // TimeShift設定画面で明示変更した保存なら、現在値を優先する。
        // デザイン設定など、TimeShift以外の保存時だけ既存値を保護する。
        const bool allowRestoreFromIni =
            !timeShiftTunerSettingsDirty_;

        const int timeShiftRole =
            static_cast<int>(TunerUseRole::TimeShiftOnly);

        for (int i = 0; i < tunersCount; ++i) {
            const auto idx =
                static_cast<std::size_t>(i);

            if (idx >= tunerUseRolePerTuner.size() ||
                idx >= tunerTimeShiftLockSpacePerTuner.size() ||
                idx >= tunerTimeShiftLockChannelPerTuner.size() ||
                idx >= tunerTimeShiftLockServiceIdPerTuner.size() ||
                idx >= tunerTimeShiftLockNetworkIdPerTuner.size() ||
                idx >= tunerTimeShiftLockTsIdPerTuner.size() ||
                idx >= tunerTimeShiftLockNamePerTuner.size()) {
                continue;
            }

            const bool currentLockConfigured =
                IsTimeShiftLockConfiguredAt_(i);

            const bool currentRoleIsTimeShift =
                tunerUseRolePerTuner[idx] == timeShiftRole;

            const int oldRole =
                ReadExistingTunerIntForSave_(
                    i,
                    L"UseRole",
                    L"TunerSettings",
                    L"UseRole",
                    static_cast<int>(TunerUseRole::General));

            const int oldSpace =
                ReadExistingTunerIntForSave_(
                    i,
                    L"TimeShiftLockSpace",
                    L"TunerSettings",
                    L"TimeShiftLockSpace",
                    0);

            const int oldChannel =
                ReadExistingTunerIntForSave_(
                    i,
                    L"TimeShiftLockChannel",
                    L"TunerSettings",
                    L"TimeShiftLockChannel",
                    -1);

            const int oldServiceId =
                ReadExistingTunerIntForSave_(
                    i,
                    L"TimeShiftLockServiceId",
                    L"TunerSettings",
                    L"TimeShiftLockServiceId",
                    0);

            const int oldNetworkId =
                ReadExistingTunerIntForSave_(
                    i,
                    L"TimeShiftLockNetworkId",
                    L"TunerSettings",
                    L"TimeShiftLockNetworkId",
                    0);

            const int oldTsId =
                ReadExistingTunerIntForSave_(
                    i,
                    L"TimeShiftLockTsId",
                    L"TunerSettings",
                    L"TimeShiftLockTsId",
                    0);

            const std::wstring oldName =
                ReadExistingTunerStringForSave_(
                    i,
                    L"TimeShiftLockName",
                    L"TunerSettings",
                    L"TimeShiftLockName",
                    L"");

            const bool oldLockConfigured =
                IsTimeShiftLockConfiguredValues_(
                    oldChannel,
                    oldServiceId,
                    oldNetworkId,
                    oldTsId);

            const bool oldIndicatesTimeShift =
                oldRole == timeShiftRole ||
                oldLockConfigured;

            const bool currentIndicatesTimeShift =
                currentRoleIsTimeShift ||
                currentLockConfigured;

            if (allowRestoreFromIni &&
                oldIndicatesTimeShift &&
                !currentIndicatesTimeShift) {
                tunerUseRolePerTuner[idx] = timeShiftRole;
                tunerTimeShiftLockSpacePerTuner[idx] =
                    ClampInt(oldSpace, 0, 15);
                tunerTimeShiftLockChannelPerTuner[idx] =
                    ClampInt(oldChannel, -1, 4096);
                tunerTimeShiftLockServiceIdPerTuner[idx] =
                    ClampInt(oldServiceId, 0, 65535);
                tunerTimeShiftLockNetworkIdPerTuner[idx] =
                    ClampInt(oldNetworkId, 0, 65535);
                tunerTimeShiftLockTsIdPerTuner[idx] =
                    ClampInt(oldTsId, 0, 65535);
                tunerTimeShiftLockNamePerTuner[idx] = oldName;
                continue;
            }

            if (currentLockConfigured &&
                tunerUseRolePerTuner[idx] != timeShiftRole) {
                tunerUseRolePerTuner[idx] = timeShiftRole;
                continue;
            }

            if (currentRoleIsTimeShift &&
                !currentLockConfigured &&
                oldLockConfigured) {
                tunerTimeShiftLockSpacePerTuner[idx] =
                    ClampInt(oldSpace, 0, 15);
                tunerTimeShiftLockChannelPerTuner[idx] =
                    ClampInt(oldChannel, -1, 4096);
                tunerTimeShiftLockServiceIdPerTuner[idx] =
                    ClampInt(oldServiceId, 0, 65535);
                tunerTimeShiftLockNetworkIdPerTuner[idx] =
                    ClampInt(oldNetworkId, 0, 65535);
                tunerTimeShiftLockTsIdPerTuner[idx] =
                    ClampInt(oldTsId, 0, 65535);
                tunerTimeShiftLockNamePerTuner[idx] = oldName;
            }
        }
    }
    catch (...) {
    }
}

bool ConfigManager::GetDebugWriteToFile() const { return debugWriteToFile; }
void ConfigManager::SetDebugWriteToFile(bool enable) { debugWriteToFile = enable; }

int ConfigManager::GetIdleMonitorSampleMs() const noexcept
{
    return idleMonitorSampleMs;
}

void ConfigManager::SetIdleMonitorSampleMs(int v) noexcept
{
    idleMonitorSampleMs = ClampIdleMonitorSampleMs_(v);
}

bool ConfigManager::GetIdleMonitorConsoleLog() const noexcept
{
    return idleMonitorConsoleLog;
}

void ConfigManager::SetIdleMonitorConsoleLog(bool enable) noexcept
{
    idleMonitorConsoleLog = enable;
}

int ConfigManager::GetIdleMonitorSystemCpuPct() const noexcept
{
    return idleMonitorSystemCpuPct;
}

void ConfigManager::SetIdleMonitorSystemCpuPct(int v) noexcept
{
    idleMonitorSystemCpuPct = ClampIdleMonitorPercent_(v);
}

int ConfigManager::GetIdleMonitorProcessCpuPct() const noexcept
{
    return idleMonitorProcessCpuPct;
}

void ConfigManager::SetIdleMonitorProcessCpuPct(int v) noexcept
{
    idleMonitorProcessCpuPct = ClampIdleMonitorPercent_(v);
}

int ConfigManager::GetIdleMonitorReadKBps() const noexcept
{
    return idleMonitorReadKBps;
}

void ConfigManager::SetIdleMonitorReadKBps(int v) noexcept
{
    idleMonitorReadKBps = ClampIdleMonitorIoKBps_(v);
}

int ConfigManager::GetIdleMonitorWriteKBps() const noexcept
{
    return idleMonitorWriteKBps;
}

void ConfigManager::SetIdleMonitorWriteKBps(int v) noexcept
{
    idleMonitorWriteKBps = ClampIdleMonitorIoKBps_(v);
}

int ConfigManager::GetIdleMonitorMemoryLoadPct() const noexcept
{
    return idleMonitorMemoryLoadPct;
}

void ConfigManager::SetIdleMonitorMemoryLoadPct(int v) noexcept
{
    idleMonitorMemoryLoadPct = ClampIdleMonitorPercent_(v);
}

std::wstring ConfigManager::GetSelectedCPUFeature() const
{
    return NormalizeSelectedCPUFeature_(selectedCPUFeature);
}

void ConfigManager::SetSelectedCPUFeature(const std::wstring& feature)
{
    selectedCPUFeature = NormalizeSelectedCPUFeature_(feature);
}

std::wstring ConfigManager::GetLogEncoding() const { return logEncoding; }
void ConfigManager::SetLogEncoding(const std::wstring& encoding) { logEncoding = encoding; }

std::wstring ConfigManager::GetSystemVendor() const { return systemVendor; }
void ConfigManager::SetSystemVendor(const std::wstring& vendor) { systemVendor = vendor; }

std::wstring ConfigManager::GetGuiLogFile() const { return guiLogFile; }
void ConfigManager::SetGuiLogFile(const std::wstring& path) { guiLogFile = path; }

void ConfigManager::SetUiWindowRect(int x, int y, int w, int h) noexcept
{
    uiWindowX = x;
    uiWindowY = y;
    uiWindowW = w;
    uiWindowH = h;
}

ConfigManager::NetworkSettings ConfigManager::LoadNetworkSettings() const
{
    NetworkSettings settings{};

    settings.enableServer =
        ParseBool(ReadIniValue(L"Network", L"EnableServer", L"0"));
    settings.allowRemoteAccess =
        ParseBool(ReadIniValue(L"Network", L"AllowRemoteAccess", L"0"));
    settings.bindAddress = settings.allowRemoteAccess
        ? L"0.0.0.0"
        : L"127.0.0.1";
    settings.httpPort =
        ClampInt(ReadIniInt(L"Network", L"HttpPort", 8080), 1, 65535);
    settings.autoStartServer =
        ParseBool(ReadIniValue(L"Network", L"AutoStartServer", L"0"));

    // Schannel は未実装。設定値が残っていても現在は有効化しない。
    settings.enableHttps = false;
    settings.httpsPort =
        ClampInt(ReadIniInt(L"Network", L"HttpsPort", 8443), 1, 65535);
    settings.pfxPath =
        ReadIniValue(L"Network", L"PfxPath", L"");
    settings.pfxPasswordProtected =
        ReadIniValue(L"Network", L"PfxPasswordProtected", L"");

    settings.enablePasswordAuth =
        ParseBool(ReadIniValue(L"Network", L"EnablePasswordAuth", L"0"));
    settings.loginUser =
        ReadIniValue(L"Network", L"LoginUser", L"epg");
    settings.loginPasswordProtected =
        ReadIniValue(L"Network", L"LoginPasswordProtected", L"");
    settings.sessionTimeoutMinutes =
        ClampInt(ReadIniInt(L"Network", L"SessionTimeoutMinutes", 720), 5, 1440);

    settings.requestTimeoutMs =
        ClampInt(ReadIniInt(L"Network", L"RequestTimeoutMs", 15000), 100, 600000);
    settings.enableCors =
        ParseBool(ReadIniValue(L"Network", L"EnableCors", L"0"));
    settings.openBrowserOnStart =
        ParseBool(ReadIniValue(L"Network", L"OpenBrowserOnStart", L"1"));
    settings.tlsMode = L"http";
    settings.listenScope = settings.allowRemoteAccess ? L"lan" : L"local";

    return settings;
}

bool ConfigManager::SaveNetworkSettings(
    const NetworkSettings& source) noexcept
{
    try {
        NetworkSettings settings = source;

        settings.bindAddress = settings.allowRemoteAccess
            ? L"0.0.0.0"
            : L"127.0.0.1";
        settings.httpPort = ClampInt(settings.httpPort, 1, 65535);
        settings.enableHttps = false;
        settings.httpsPort = ClampInt(settings.httpsPort, 1, 65535);
        settings.sessionTimeoutMinutes =
            ClampInt(settings.sessionTimeoutMinutes, 5, 1440);
        settings.requestTimeoutMs =
            ClampInt(settings.requestTimeoutMs, 100, 600000);
        settings.tlsMode = L"http";
        settings.listenScope =
            settings.allowRemoteAccess ? L"lan" : L"local";

        std::vector<IniKeyValue_> section;
        section.reserve(18);

        AddIniKey_(section, L"EnableServer", IniBoolText_(settings.enableServer));
        AddIniKey_(section, L"BindAddress", settings.bindAddress);
        AddIniKey_(section, L"HttpPort", IniIntText_(settings.httpPort));
        AddIniKey_(section, L"AllowRemoteAccess", IniBoolText_(settings.allowRemoteAccess));
        AddIniKey_(section, L"AutoStartServer", IniBoolText_(settings.autoStartServer));

        AddIniKey_(section, L"EnableHttps", L"0");
        AddIniKey_(section, L"HttpsPort", IniIntText_(settings.httpsPort));
        AddIniKey_(section, L"PfxPath", settings.pfxPath);
        AddIniKey_(section, L"PfxPasswordProtected", settings.pfxPasswordProtected);

        AddIniKey_(section, L"EnablePasswordAuth", IniBoolText_(settings.enablePasswordAuth));
        AddIniKey_(section, L"LoginUser", settings.loginUser);
        AddIniKey_(section, L"LoginPasswordProtected", settings.loginPasswordProtected);
        AddIniKey_(section, L"SessionTimeoutMinutes", IniIntText_(settings.sessionTimeoutMinutes));

        AddIniKey_(section, L"RequestTimeoutMs", IniIntText_(settings.requestTimeoutMs));
        AddIniKey_(section, L"EnableCors", IniBoolText_(settings.enableCors));
        AddIniKey_(section, L"OpenBrowserOnStart", IniBoolText_(settings.openBrowserOnStart));
        AddIniKey_(section, L"TlsMode", settings.tlsMode);
        AddIniKey_(section, L"ListenScope", settings.listenScope);

        return WriteIniSectionFast_(
            iniMutex,
            iniPath,
            L"Network",
            section);
    }
    catch (...) {
        return false;
    }
}

bool ConfigManager::ParseBool(const std::wstring& value) const
{
    const std::wstring v = ToLowerTrim(value);
    if (v == L"1" || v == L"true" || v == L"yes" || v == L"on") return true;
    if (v == L"0" || v == L"false" || v == L"no" || v == L"off") return false;
    return false;
}

std::wstring ConfigManager::ReadIniValue(
    const std::wstring& section,
    const std::wstring& key,
    const std::wstring& defaultValue) const
{
    std::lock_guard<std::mutex> lock(iniMutex);

    wchar_t buffer[1024]{};
    GetPrivateProfileStringW(
        section.c_str(),
        key.c_str(),
        defaultValue.c_str(),
        buffer,
        (DWORD)_countof(buffer),
        iniPath.c_str());
    return buffer;
}

int ConfigManager::ReadIniInt(
    const std::wstring& section,
    const std::wstring& key,
    int defaultValue) const
{
    const std::wstring s = ToLowerTrim(ReadIniValue(section, key, L""));
    if (s.empty()) return defaultValue;

    wchar_t* end = nullptr;
    long v = std::wcstol(s.c_str(), &end, 10);
    if (end == s.c_str()) return defaultValue;
    return static_cast<int>(v);
}

float ConfigManager::ReadIniFloat(
    const std::wstring& section,
    const std::wstring& key,
    float defaultValue) const
{
    const std::wstring s = ToLowerTrim(ReadIniValue(section, key, L""));
    if (s.empty()) return defaultValue;

    wchar_t* end = nullptr;
    double v = std::wcstod(s.c_str(), &end);
    if (end == s.c_str()) return defaultValue;
    return static_cast<float>(v);
}

std::size_t ConfigManager::ReadIniSizeT(
    const std::wstring& section,
    const std::wstring& key,
    std::size_t defaultValue) const
{
    const std::wstring s = ToLowerTrim(ReadIniValue(section, key, L""));
    if (s.empty()) return defaultValue;

    wchar_t* end = nullptr;
    unsigned long long v = std::wcstoull(s.c_str(), &end, 10);
    if (end == s.c_str()) return defaultValue;
    return static_cast<std::size_t>(v);
}

void ConfigManager::WriteIniValue(
    const std::wstring& section,
    const std::wstring& key,
    const std::wstring& value)
{
    std::lock_guard<std::mutex> lock(iniMutex);

    WritePrivateProfileStringW(
        section.c_str(),
        key.c_str(),
        value.c_str(),
        iniPath.c_str());
}

void ConfigManager::WriteIniInt(
    const std::wstring& section,
    const std::wstring& key,
    int value)
{
    wchar_t buf[64]{};
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d", value);
    WriteIniValue(section, key, buf);
}

void ConfigManager::WriteIniFloat(
    const std::wstring& section,
    const std::wstring& key,
    float value)
{
    wchar_t buf[64]{};
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%.6f", static_cast<double>(value));
    WriteIniValue(section, key, buf);
}

void ConfigManager::WriteIniSizeT(
    const std::wstring& section,
    const std::wstring& key,
    std::size_t value)
{
    wchar_t buf[64]{};
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%llu",
        static_cast<unsigned long long>(value));
    WriteIniValue(section, key, buf);
}

void ConfigManager::WriteIniBool(
    const std::wstring& section,
    const std::wstring& key,
    bool value)
{
    WriteIniValue(section, key, value ? L"1" : L"0");
}

void ConfigManager::DeleteIniKey(
    const std::wstring& section,
    const std::wstring& key)
{
    std::lock_guard<std::mutex> lock(iniMutex);

    WritePrivateProfileStringW(
        section.c_str(),
        key.c_str(),
        nullptr,
        iniPath.c_str());
}

void ConfigManager::DeleteIniSection(const std::wstring& section)
{
    std::lock_guard<std::mutex> lock(iniMutex);

    WritePrivateProfileStringW(
        section.c_str(),
        nullptr,
        nullptr,
        iniPath.c_str());
}

bool ConfigManager::IsIniKeyMissing(const std::wstring& section, const std::wstring& key) const
{
    std::lock_guard<std::mutex> lock(iniMutex);

    constexpr const wchar_t* kMissing = L"__MISSING__";
    wchar_t buffer[64]{};
    GetPrivateProfileStringW(
        section.c_str(),
        key.c_str(),
        kMissing,
        buffer,
        (DWORD)_countof(buffer),
        iniPath.c_str());
    return (wcscmp(buffer, kMissing) == 0);
}

void ConfigManager::EnsureDefault(
    const std::wstring& section,
    const std::wstring& key,
    const std::wstring& defaultValue)
{
    if (IsIniKeyMissing(section, key)) {
        WriteIniValue(section, key, defaultValue);
    }
}

void ConfigManager::InitializeConfig()
{
    EnsureDefault(L"System", L"VENDOR", L"Unknown");

    EnsureDefault(L"BonDriver", L"Path", L"");
    EnsureDefault(L"BonDriver", L"Terrestrial", L"");
    EnsureDefault(L"BonDriver", L"Satellite", L"");
    EnsureDefault(L"BonDriver", L"Default", L"");

    EnsureDefault(L"Path", L"AppWorkDir", L".");
    EnsureDefault(L"Path", L"EpgOutDir", L"epg_out");
    EnsureDefault(L"Path", L"EpgDataWorkDir", L"epg_work");

    EnsureDefault(L"Network", L"EnableServer", L"0");
    EnsureDefault(L"Network", L"BindAddress", L"127.0.0.1");
    EnsureDefault(L"Network", L"HttpPort", L"8080");
    EnsureDefault(L"Network", L"AllowRemoteAccess", L"0");
    EnsureDefault(L"Network", L"AutoStartServer", L"0");
    EnsureDefault(L"Network", L"EnableHttps", L"0");
    EnsureDefault(L"Network", L"HttpsPort", L"8443");
    EnsureDefault(L"Network", L"PfxPath", L"");
    EnsureDefault(L"Network", L"PfxPasswordProtected", L"");
    EnsureDefault(L"Network", L"EnablePasswordAuth", L"0");
    EnsureDefault(L"Network", L"LoginUser", L"epg");
    EnsureDefault(L"Network", L"LoginPasswordProtected", L"");
    EnsureDefault(L"Network", L"SessionTimeoutMinutes", L"720");
    EnsureDefault(L"Network", L"RequestTimeoutMs", L"15000");
    EnsureDefault(L"Network", L"EnableCors", L"0");
    EnsureDefault(L"Network", L"OpenBrowserOnStart", L"1");
    EnsureDefault(L"Network", L"TlsMode", L"http");
    EnsureDefault(L"Network", L"ListenScope", L"local");

    EnsureDefault(L"TimeShift", L"RootFolder", L"timeshift");
    EnsureDefault(L"TimeShift", L"MaxBytesGB", L"100");

    EnsureDefault(L"Debug", L"WriteToFile", L"0");
    EnsureDefault(L"Debug", L"LogEncoding", L"utf8");
    EnsureDefault(L"Debug", L"EnableSDTDebug", L"0");
    EnsureDefault(L"Debug", L"EnableNITDebug", L"0");
    EnsureDefault(L"Debug", L"GuiLogEnabled", L"1");
    EnsureDefault(L"Debug", L"GuiLogFile", L"debug.log");

    EnsureDefault(L"Debug", L"IdleMonitorSampleMs", L"5000");
    EnsureDefault(L"Debug", L"IdleMonitorConsoleLog", L"1");
    EnsureDefault(L"Debug", L"IdleMonitorSystemCpuPct", L"20");
    EnsureDefault(L"Debug", L"IdleMonitorProcessCpuPct", L"5");
    EnsureDefault(L"Debug", L"IdleMonitorReadKBps", L"2048");
    EnsureDefault(L"Debug", L"IdleMonitorWriteKBps", L"4096");
    EnsureDefault(L"Debug", L"IdleMonitorMemoryLoadPct", L"90");

    EnsureDefault(L"CPU", L"SelectedFeature", L"AUTO");
    EnsureDefault(L"CPU", L"USE_SSE2", L"1");
    EnsureDefault(L"CPU", L"USE_SSE3", L"1");
    EnsureDefault(L"CPU", L"USE_SSSE3", L"1");
    EnsureDefault(L"CPU", L"USE_SSE4_1", L"1");
    EnsureDefault(L"CPU", L"USE_SSE4_2", L"1");
    EnsureDefault(L"CPU", L"USE_AVX", L"1");
    EnsureDefault(L"CPU", L"USE_AVX2", L"1");
    EnsureDefault(L"CPU", L"USE_AVX512", L"0");
    EnsureDefault(L"CPU", L"USE_FMA", L"1");
    EnsureDefault(L"CPU", L"USE_POPCNT", L"1");
    EnsureDefault(L"CPU", L"USE_AES_NI", L"1");
    EnsureDefault(L"CPU", L"USE_SHA", L"1");
    EnsureDefault(L"CPU", L"DetectedVendor", L"Unknown");
    EnsureDefault(L"CPU", L"DetectedLogicalCores", L"0");

    EnsureDefault(L"CPU", L"DetectedSSE2", L"0");
    EnsureDefault(L"CPU", L"DetectedSSE3", L"0");
    EnsureDefault(L"CPU", L"DetectedSSSE3", L"0");
    EnsureDefault(L"CPU", L"DetectedSSE4_1", L"0");
    EnsureDefault(L"CPU", L"DetectedSSE4_2", L"0");
    EnsureDefault(L"CPU", L"DetectedAVX", L"0");
    EnsureDefault(L"CPU", L"DetectedAVX2", L"0");
    EnsureDefault(L"CPU", L"DetectedAVX512", L"0");
    EnsureDefault(L"CPU", L"DetectedFMA", L"0");
    EnsureDefault(L"CPU", L"DetectedPOPCNT", L"0");
    EnsureDefault(L"CPU", L"DetectedAES_NI", L"0");
    EnsureDefault(L"CPU", L"DetectedSHA", L"0");
    EnsureDefault(L"CPU", L"DetectedBMI1", L"0");
    EnsureDefault(L"CPU", L"DetectedBMI2", L"0");

    EnsureDefault(L"CPU", L"USE_BMI1", L"1");
    EnsureDefault(L"CPU", L"USE_BMI2", L"1");

    EnsureDefault(L"CPU", L"EnableParallelWorkers", L"1");
    EnsureDefault(L"CPU", L"RuntimeWorkerReserveCores", L"1");
    EnsureDefault(L"CPU", L"RuntimeMaxWorkerThreads", L"0");

    EnsureDefault(L"Arib", L"EnableTrace", L"0");
    EnsureDefault(L"Arib", L"TraceG", L"0");
    EnsureDefault(L"Arib", L"TraceChars", L"0");

    EnsureDefault(L"ChannelScan", L"EnableTsDump", L"0");
    EnsureDefault(L"ChannelScan", L"EnableRingDump", L"0");

    EnsureDefault(L"ChannelScanFixed", L"SignalLevelThreshold", L"0.0");
    EnsureDefault(L"ChannelScanFixed", L"LockWaitMs", L"600");
    EnsureDefault(L"ChannelScanFixed", L"TsCheckWaitMs", L"800");
    EnsureDefault(L"ChannelScanFixed", L"TsCheckPollMs", L"50");
    EnsureDefault(L"ChannelScanFixed", L"MinTsBytes", L"940");
    EnsureDefault(L"ChannelScanFixed", L"DropVideoAudioSubtitle", L"1");
    EnsureDefault(L"ChannelScanFixed", L"SavePerSpace", L"1");
    EnsureDefault(L"ChannelScanFixed", L"LogConfigSnapshot", L"1");
    EnsureDefault(L"ChannelScanFixed", L"CaptureSec", L"3");
    EnsureDefault(L"ChannelScanFixed", L"RingBufferSizeBytes", L"33554432");
    EnsureDefault(L"ChannelScanFixed", L"PacketReserve", L"4096");
    EnsureDefault(L"ChannelScanFixed", L"SleepOnFailMs", L"1");
    EnsureDefault(L"ChannelScanFixed", L"ScanAllSpaces", L"0");
    EnsureDefault(L"ChannelScanFixed", L"MaxSpaceToTry", L"7");
    EnsureDefault(L"ChannelScanFixed", L"DefaultSpace", L"0");

    EnsureDefault(L"UI", L"MinimizeToTray", L"1");
    EnsureDefault(L"UI", L"StartupEnabled", L"0");
    EnsureDefault(L"UI", L"WindowX", L"-1");
    EnsureDefault(L"UI", L"WindowY", L"-1");
    EnsureDefault(L"UI", L"WindowW", L"1100");
    EnsureDefault(L"UI", L"WindowH", L"720");
    EnsureDefault(L"UI", L"LastTabIndex", L"0");

    EnsureDefault(L"Tuners", L"Count", L"1");
    EnsureDefault(L"Tuners", L"ActiveIndex", L"0");

    {
        int count = ReadIniInt(L"Tuners", L"Count", 1);
        if (count < 1) {
            count = 1;
        }

        for (int i = 0; i < count; ++i) {
            const std::wstring tunerSection = MakeTunerSectionNameLocal(i);

            const auto ensureTunerDefault =
                [this, &tunerSection, i](const wchar_t* baseKey, const std::wstring& value)
                {
                    const std::wstring indexedKey = MakeIndexedKey_(baseKey, i);

                    // 番号付きキーが無く、番号なしキーも無い場合だけ作る。
                    // 既存の番号なし [TunerN] Key を壊さない。
                    if (IsIniKeyMissing(tunerSection, indexedKey) &&
                        IsIniKeyMissing(tunerSection, baseKey)) {
                        EnsureDefault(tunerSection, indexedKey, value);
                    }
                };

            const std::wstring enableKey = MakeIndexedKey_(L"Enable", i);
            const std::wstring nameKey = MakeIndexedKey_(L"Name", i);
            const std::wstring bonDriverPathKey = MakeIndexedKey_(L"BonDriverPath", i);
            const std::wstring oldBonDriverPathKey = MakeIndexedKey_(L"Path", i);

            if (IsIniKeyMissing(tunerSection, MakeIndexedKey_(L"Enable", i)) &&
                IsIniKeyMissing(tunerSection, L"Enable") &&
                IsIniKeyMissing(L"TunerSettings", enableKey) &&
                IsIniKeyMissing(L"Tuners", enableKey)) {
                EnsureDefault(tunerSection, MakeIndexedKey_(L"Enable", i), L"1");
            }

            if (IsIniKeyMissing(tunerSection, MakeIndexedKey_(L"Name", i)) &&
                IsIniKeyMissing(tunerSection, L"Name") &&
                IsIniKeyMissing(L"TunerSettings", nameKey) &&
                IsIniKeyMissing(L"Tuners", nameKey)) {
                EnsureDefault(tunerSection, MakeIndexedKey_(L"Name", i), MakeDefaultTunerName(i));
            }

            if (IsIniKeyMissing(tunerSection, MakeIndexedKey_(L"BonDriverPath", i)) &&
                IsIniKeyMissing(tunerSection, L"BonDriverPath") &&
                IsIniKeyMissing(L"TunerSettings", bonDriverPathKey) &&
                IsIniKeyMissing(L"BonDriver", oldBonDriverPathKey)) {
                EnsureDefault(tunerSection, MakeIndexedKey_(L"BonDriverPath", i), L"");
            }

            ensureTunerDefault(L"UseExternalApp", L"0");
            ensureTunerDefault(L"ExternalAppPath", L"");
            ensureTunerDefault(L"ReceiveMode", L"0");
            ensureTunerDefault(L"UseRole", L"0");
            ensureTunerDefault(L"AllowParallel", L"1");
            ensureTunerDefault(L"Priority", std::to_wstring(i));

            ensureTunerDefault(L"TimeShiftLockSpace", L"0");
            ensureTunerDefault(L"TimeShiftLockChannel", L"-1");
            ensureTunerDefault(L"TimeShiftLockServiceId", L"0");
            ensureTunerDefault(L"TimeShiftLockNetworkId", L"0");
            ensureTunerDefault(L"TimeShiftLockTsId", L"0");
            ensureTunerDefault(L"TimeShiftLockName", L"");

            ensureTunerDefault(L"TvTestUseBridge", L"0");
            ensureTunerDefault(L"TvTestDid", MakeDefaultTvTestDidLocal(i));
            ensureTunerDefault(L"TvTestExePath", L"");
            ensureTunerDefault(L"TvTestControlPipeName", MakeDefaultTvTestControlPipeNameLocal(i));
            ensureTunerDefault(L"TvTestTsPipeName", MakeDefaultTvTestTsPipeNameLocal(i));
            ensureTunerDefault(L"TvTestCommand", L"PING");
        }
    }

    EnsureDefault(L"TunerGlobal", L"UseCommonBonDriver", L"0");
    EnsureDefault(L"TunerGlobal", L"CommonBonDriverPath", L"");
    EnsureDefault(L"TunerGlobal", L"ConflictPolicy", L"0");
    EnsureDefault(L"TunerGlobal", L"PauseEpgWhileRecording", L"1");
    EnsureDefault(L"TunerGlobal", L"PreferRecordingOverEpg", L"1");
    EnsureDefault(L"TunerGlobal", L"CommonExternalAppPath", L"");

    EnsureDefault(L"EpgServiceEnable", L"DefaultEnable", L"1");

    EnsureDefault(L"EPGUpdate", L"EpgAutoMode", L"Task");
    EnsureDefault(L"EPGUpdate", L"EpgBootGraceSec", L"60");
    EnsureDefault(L"EPGUpdate", L"EpgCatchUpOnStart", L"1");
    EnsureDefault(L"EPGUpdate", L"EpgTickSeconds", L"1");
    EnsureDefault(L"EPGUpdate", L"EpgIdleRequiredSec", L"30");
    EnsureDefault(L"EPGUpdate", L"EpgEnableDailyFull", L"1");
    EnsureDefault(L"EPGUpdate", L"EpgDailyFullTimeHHMM", L"0330");
    EnsureDefault(L"EPGUpdate", L"EpgEnableHourly", L"0");
    EnsureDefault(L"EPGUpdate", L"EpgHourlyAtMinute", L"5");
    EnsureDefault(L"EPGUpdate", L"EpgLightUpdateIntervalMin", L"60");
    EnsureDefault(L"EPGUpdate", L"ManualFullEstimatedMinutes", L"90");
    EnsureDefault(L"EPGUpdate", L"ManualFullGuardMinutes", L"30");
    EnsureDefault(L"EPGUpdate", L"ManualLightBaseMinutes", L"15");
    EnsureDefault(L"EPGUpdate", L"ManualLightPerChannelMinutes", L"3");
    EnsureDefault(L"EPGUpdate", L"ManualLightGuardMinutes", L"30");
    EnsureDefault(L"EPGUpdate", L"EpgHtmlRenderRoute", L"legacy");

    EnsureDefault(L"ReservationExecution", L"EnableExecution", L"1");
    EnsureDefault(L"ReservationExecution", L"EnableWatchReservation", L"1");
    EnsureDefault(L"ReservationExecution", L"EnableTvTestWatch", L"1");
    EnsureDefault(L"ReservationExecution", L"EnableRecordExecution", L"1");
    EnsureDefault(L"ReservationExecution", L"EnableTsPipeDuringWatch", L"0");
    EnsureDefault(L"ReservationExecution", L"TvTestRecordShowVideo", L"1");
    EnsureDefault(L"ReservationExecution", L"RecordFolder", L"");
    EnsureDefault(L"ReservationExecution", L"PrepareSeconds", L"30");
}

void ConfigManager::SaveConfig()
{
    PreserveTimeShiftTunerSettingsBeforeFullSave_();
    NormalizeTimeShiftRolesFromLocks_();

    // 設定をINIファイルに保存する
    {
        std::vector<IniKeyValue_> sec;
        AddIniKey_(sec, L"VENDOR", systemVendor);

        WriteIniSectionFast_(
            iniMutex,
            iniPath,
            L"System",
            sec);
    }

    // BonDriver 設定。
//
// Path:
//   旧互換。基本は地上波BonDriverを保存する。
//
// Terrestrial:
//   space=0 用。
//
// Satellite:
//   space=1/2 用。
//
// Default:
//   旧コード・未知space用の予備。
    {
        std::vector<IniKeyValue_> sec;

        const std::wstring terrestrial =
            TrimPathText_(bonDriverTerrestrialPath).empty()
            ? TrimPathText_(bonDriverPath)
            : TrimPathText_(bonDriverTerrestrialPath);

        const std::wstring satellite =
            TrimPathText_(bonDriverSatellitePath);

        const std::wstring legacyPath =
            !terrestrial.empty()
            ? terrestrial
            : TrimPathText_(bonDriverPath);

        const std::wstring defPath =
            TrimPathText_(bonDriverDefaultPath).empty()
            ? legacyPath
            : TrimPathText_(bonDriverDefaultPath);

        AddIniKey_(sec, L"Path", legacyPath);
        AddIniKey_(sec, L"Terrestrial", terrestrial);
        AddIniKey_(sec, L"Satellite", satellite);
        AddIniKey_(sec, L"Default", defPath);

        WriteIniSectionFast_(
            iniMutex,
            iniPath,
            L"BonDriver",
            sec);
    }

    // Path 設定
    {
        std::vector<IniKeyValue_> sec;

        AddIniKey_(
            sec,
            L"AppWorkDir",
            TrimPathText_(pathAppWorkDir).empty()
            ? L"."
            : pathAppWorkDir);

        AddIniKey_(
            sec,
            L"EpgOutDir",
            TrimPathText_(pathEpgOutDir).empty()
            ? L"epg_out"
            : pathEpgOutDir);

        AddIniKey_(
            sec,
            L"EpgDataWorkDir",
            TrimPathText_(pathEpgDataWorkDir).empty()
            ? L"epg_work"
            : pathEpgDataWorkDir);

        WriteIniSectionFast_(
            iniMutex,
            iniPath,
            L"Path",
            sec);
    }

    // TimeShift 設定
    {
        std::vector<IniKeyValue_> sec;

        AddIniKey_(sec, L"RootFolder", GetTimeShiftRootFolderRaw());
        AddIniKey_(sec, L"MaxBytesGB", IniIntText_(GetTimeShiftMaxBytesGB()));

        // 旧 MaxBytesMB はこのセクション一括保存で消える。

        WriteIniSectionFast_(
            iniMutex,
            iniPath,
            L"TimeShift",
            sec);
    }

    // Debug 設定
    {
        std::vector<IniKeyValue_> sec;

        AddIniKey_(sec, L"WriteToFile", IniBoolText_(debugWriteToFile));
        AddIniKey_(sec, L"LogEncoding", logEncoding);
        AddIniKey_(sec, L"EnableSDTDebug", IniBoolText_(enableSDTDebug));
        AddIniKey_(sec, L"EnableNITDebug", IniBoolText_(enableNITDebug));
        AddIniKey_(sec, L"GuiLogEnabled", IniBoolText_(guiLogEnabled));
        AddIniKey_(sec, L"GuiLogFile", guiLogFile);

        AddIniKey_(sec, L"IdleMonitorSampleMs", IniIntText_(idleMonitorSampleMs));
        AddIniKey_(sec, L"IdleMonitorConsoleLog", IniBoolText_(idleMonitorConsoleLog));
        AddIniKey_(sec, L"IdleMonitorSystemCpuPct", IniIntText_(idleMonitorSystemCpuPct));
        AddIniKey_(sec, L"IdleMonitorProcessCpuPct", IniIntText_(idleMonitorProcessCpuPct));
        AddIniKey_(sec, L"IdleMonitorReadKBps", IniIntText_(idleMonitorReadKBps));
        AddIniKey_(sec, L"IdleMonitorWriteKBps", IniIntText_(idleMonitorWriteKBps));
        AddIniKey_(sec, L"IdleMonitorMemoryLoadPct", IniIntText_(idleMonitorMemoryLoadPct));

        WriteIniSectionFast_(
            iniMutex,
            iniPath,
            L"Debug",
            sec);
    }

    // CPU 設定
    {
        RefreshCpuDetectedFromFeature_();

        std::vector<IniKeyValue_> sec;

        AddIniKey_(sec, L"SelectedFeature", GetSelectedCPUFeature());

        AddIniKey_(sec, L"DetectedVendor", cpuDetectedVendor);
        AddIniKey_(
            sec,
            L"DetectedLogicalCores",
            std::to_wstring(cpuDetectedLogicalCores));

        AddIniKey_(sec, L"DetectedSSE2", IniBoolText_(cpuDetectedSSE2));
        AddIniKey_(sec, L"DetectedSSE3", IniBoolText_(cpuDetectedSSE3));
        AddIniKey_(sec, L"DetectedSSSE3", IniBoolText_(cpuDetectedSSSE3));
        AddIniKey_(sec, L"DetectedSSE4_1", IniBoolText_(cpuDetectedSSE4_1));
        AddIniKey_(sec, L"DetectedSSE4_2", IniBoolText_(cpuDetectedSSE4_2));

        AddIniKey_(sec, L"DetectedAVX", IniBoolText_(cpuDetectedAVX));
        AddIniKey_(sec, L"DetectedAVX2", IniBoolText_(cpuDetectedAVX2));
        AddIniKey_(sec, L"DetectedAVX512", IniBoolText_(cpuDetectedAVX512));

        AddIniKey_(sec, L"DetectedFMA", IniBoolText_(cpuDetectedFMA));
        AddIniKey_(sec, L"DetectedPOPCNT", IniBoolText_(cpuDetectedPOPCNT));
        AddIniKey_(sec, L"DetectedAES_NI", IniBoolText_(cpuDetectedAESNI));
        AddIniKey_(sec, L"DetectedSHA", IniBoolText_(cpuDetectedSHA));
        AddIniKey_(sec, L"DetectedBMI1", IniBoolText_(cpuDetectedBMI1));
        AddIniKey_(sec, L"DetectedBMI2", IniBoolText_(cpuDetectedBMI2));

        AddIniKey_(sec, L"USE_SSE2", IniBoolText_(cpuUseSSE2));
        AddIniKey_(sec, L"USE_SSE3", IniBoolText_(cpuUseSSE3));
        AddIniKey_(sec, L"USE_SSSE3", IniBoolText_(cpuUseSSSE3));
        AddIniKey_(sec, L"USE_SSE4_1", IniBoolText_(cpuUseSSE4_1));
        AddIniKey_(sec, L"USE_SSE4_2", IniBoolText_(cpuUseSSE4_2));

        AddIniKey_(sec, L"USE_AVX", IniBoolText_(cpuUseAVX));
        AddIniKey_(sec, L"USE_AVX2", IniBoolText_(cpuUseAVX2));
        AddIniKey_(sec, L"USE_AVX512", IniBoolText_(cpuUseAVX512));

        AddIniKey_(sec, L"USE_FMA", IniBoolText_(cpuUseFMA));
        AddIniKey_(sec, L"USE_POPCNT", IniBoolText_(cpuUsePOPCNT));
        AddIniKey_(sec, L"USE_AES_NI", IniBoolText_(cpuUseAESNI));
        AddIniKey_(sec, L"USE_SHA", IniBoolText_(cpuUseSHA));
        AddIniKey_(sec, L"USE_BMI1", IniBoolText_(cpuUseBMI1));
        AddIniKey_(sec, L"USE_BMI2", IniBoolText_(cpuUseBMI2));

        AddIniKey_(
            sec,
            L"EnableParallelWorkers",
            IniBoolText_(cpuEnableParallelWorkers));

        AddIniKey_(
            sec,
            L"RuntimeWorkerReserveCores",
            std::to_wstring(cpuRuntimeWorkerReserveCores));

        AddIniKey_(
            sec,
            L"RuntimeMaxWorkerThreads",
            std::to_wstring(cpuRuntimeMaxWorkerThreads));

        WriteIniSectionFast_(
            iniMutex,
            iniPath,
            L"CPU",
            sec);
    }

    // ARIB 設定
    {
        std::vector<IniKeyValue_> sec;

        AddIniKey_(sec, L"EnableTrace", IniBoolText_(aribEnableTrace));
        AddIniKey_(sec, L"TraceG", IniBoolText_(aribTraceG));
        AddIniKey_(sec, L"TraceChars", IniBoolText_(aribTraceChars));

        WriteIniSectionFast_(
            iniMutex,
            iniPath,
            L"Arib",
            sec);
    }

    // ChannelScan 設定
    {
        std::vector<IniKeyValue_> sec;

        AddIniKey_(sec, L"EnableTsDump", IniBoolText_(channelScanEnableTsDump));
        AddIniKey_(sec, L"EnableRingDump", IniBoolText_(channelScanEnableRingDump));

        WriteIniSectionFast_(
            iniMutex,
            iniPath,
            L"ChannelScan",
            sec);
    }

    // ChannelScanFixed 設定
    {
        std::vector<IniKeyValue_> sec;

        AddIniKey_(sec, L"SignalLevelThreshold", IniFloatText_(channelScanFixedSignalLevelThreshold));
        AddIniKey_(sec, L"LockWaitMs", IniIntText_(channelScanFixedLockWaitMs));
        AddIniKey_(sec, L"TsCheckWaitMs", IniIntText_(channelScanFixedTsCheckWaitMs));
        AddIniKey_(sec, L"TsCheckPollMs", IniIntText_(channelScanFixedTsCheckPollMs));
        AddIniKey_(sec, L"MinTsBytes", IniSizeText_(channelScanFixedMinTsBytes));
        AddIniKey_(sec, L"DropVideoAudioSubtitle", IniBoolText_(channelScanFixedDropVideoAudioSubtitle));
        AddIniKey_(sec, L"SavePerSpace", IniBoolText_(channelScanFixedSavePerSpace));
        AddIniKey_(sec, L"LogConfigSnapshot", IniBoolText_(channelScanFixedLogConfigSnapshot));
        AddIniKey_(sec, L"CaptureSec", IniIntText_(channelScanFixedCaptureSec));
        AddIniKey_(sec, L"RingBufferSizeBytes", IniSizeText_(channelScanFixedRingBufferSizeBytes));
        AddIniKey_(sec, L"PacketReserve", IniSizeText_(channelScanFixedPacketReserve));
        AddIniKey_(sec, L"SleepOnFailMs", IniIntText_(channelScanFixedSleepOnFailMs));
        AddIniKey_(sec, L"ScanAllSpaces", IniBoolText_(channelScanFixedScanAllSpaces));
        AddIniKey_(sec, L"MaxSpaceToTry", IniIntText_(channelScanFixedMaxSpaceToTry));
        AddIniKey_(sec, L"DefaultSpace", IniIntText_(channelScanFixedDefaultSpace));

        WriteIniSectionFast_(
            iniMutex,
            iniPath,
            L"ChannelScanFixed",
            sec);
    }

    // UI 設定
    {
        std::vector<IniKeyValue_> sec;

        AddIniKey_(sec, L"MinimizeToTray", IniBoolText_(uiMinimizeToTray));
        AddIniKey_(sec, L"StartupEnabled", IniBoolText_(uiStartupEnabled));
        AddIniKey_(sec, L"WindowX", IniIntText_(uiWindowX));
        AddIniKey_(sec, L"WindowY", IniIntText_(uiWindowY));
        AddIniKey_(sec, L"WindowW", IniIntText_(uiWindowW));
        AddIniKey_(sec, L"WindowH", IniIntText_(uiWindowH));
        AddIniKey_(sec, L"LastTabIndex", IniIntText_(uiLastTabIndex));

        WriteIniSectionFast_(
            iniMutex,
            iniPath,
            L"UI",
            sec);
    }

    // Tuners 設定
    {
        std::vector<IniKeyValue_> sec;

        AddIniKey_(sec, L"Count", IniIntText_(tunersCount));
        AddIniKey_(sec, L"ActiveIndex", IniIntText_(tunersActiveIndex));

        // 旧 Enable0 / Name0 などはこの一括保存で消える。

        WriteIniSectionFast_(
            iniMutex,
            iniPath,
            L"Tuners",
            sec);
    }

    // TunerGlobal 設定
    {
        std::vector<IniKeyValue_> sec;

        AddIniKey_(sec, L"UseCommonBonDriver", IniBoolText_(tunerGlobalUseCommonBonDriver));
        AddIniKey_(sec, L"CommonBonDriverPath", tunerGlobalCommonBonDriverPath);
        AddIniKey_(sec, L"ConflictPolicy", IniIntText_(static_cast<int>(tunerGlobalConflictPolicy)));
        AddIniKey_(sec, L"PauseEpgWhileRecording", IniBoolText_(tunerGlobalPauseEpgWhileRecording));
        AddIniKey_(sec, L"PreferRecordingOverEpg", IniBoolText_(tunerGlobalPreferRecordingOverEpg));
        AddIniKey_(sec, L"CommonExternalAppPath", tunerGlobalCommonExternalAppPath);

        WriteIniSectionFast_(
            iniMutex,
            iniPath,
            L"TunerGlobal",
            sec);
    }

    EnsureTunerVectors_();

    for (int i = 0; i < tunersCount; ++i) {
        const std::size_t idx = static_cast<std::size_t>(i);
        const std::wstring tunerSection = MakeTunerSectionNameLocal(i);

        std::vector<IniKeyValue_> sec;

        AddIniKey_(sec, MakeIndexedKey_(L"Enable", i).c_str(), IniBoolText_(tunerEnablePerTuner[idx] != 0));

        AddIniKey_(
            sec,
            MakeIndexedKey_(L"Name", i).c_str(),
            tunerNamePerTuner[idx].empty()
            ? MakeDefaultTunerName(i)
            : tunerNamePerTuner[idx]);

        AddIniKey_(sec, MakeIndexedKey_(L"BonDriverPath", i).c_str(), bonDriverPathPerTuner[idx]);
        AddIniKey_(sec, MakeIndexedKey_(L"InternalBonDriverPath", i).c_str(), tunerInternalBonDriverPathPerTuner[idx]);
        AddIniKey_(sec, MakeIndexedKey_(L"TvTestBonDriverPath", i).c_str(), tunerTvTestBonDriverPathPerTuner[idx]);

        AddIniKey_(sec, MakeIndexedKey_(L"UseExternalApp", i).c_str(), IniBoolText_(tunerUseExternalAppPerTuner[idx] != 0));
        AddIniKey_(sec, MakeIndexedKey_(L"ExternalAppPath", i).c_str(), tunerExternalAppPathPerTuner[idx]);

        AddIniKey_(sec, MakeIndexedKey_(L"ReceiveMode", i).c_str(), IniIntText_(tunerReceiveModePerTuner[idx]));
        AddIniKey_(sec, MakeIndexedKey_(L"UseRole", i).c_str(), IniIntText_(tunerUseRolePerTuner[idx]));
        AddIniKey_(sec, MakeIndexedKey_(L"AllowParallel", i).c_str(), IniBoolText_(tunerAllowParallelPerTuner[idx] != 0));
        AddIniKey_(sec, MakeIndexedKey_(L"Priority", i).c_str(), IniIntText_(tunerPriorityPerTuner[idx]));

        AddIniKey_(sec, MakeIndexedKey_(L"TimeShiftLockSpace", i).c_str(), IniIntText_(tunerTimeShiftLockSpacePerTuner[idx]));
        AddIniKey_(sec, MakeIndexedKey_(L"TimeShiftLockChannel", i).c_str(), IniIntText_(tunerTimeShiftLockChannelPerTuner[idx]));
        AddIniKey_(sec, MakeIndexedKey_(L"TimeShiftLockServiceId", i).c_str(), IniIntText_(tunerTimeShiftLockServiceIdPerTuner[idx]));
        AddIniKey_(sec, MakeIndexedKey_(L"TimeShiftLockNetworkId", i).c_str(), IniIntText_(tunerTimeShiftLockNetworkIdPerTuner[idx]));
        AddIniKey_(sec, MakeIndexedKey_(L"TimeShiftLockTsId", i).c_str(), IniIntText_(tunerTimeShiftLockTsIdPerTuner[idx]));
        AddIniKey_(sec, MakeIndexedKey_(L"TimeShiftLockName", i).c_str(), tunerTimeShiftLockNamePerTuner[idx]);

        AddIniKey_(sec, MakeIndexedKey_(L"TvTestUseBridge", i).c_str(), IniBoolText_(tunerTvTestUseBridgePerTuner[idx] != 0));

        AddIniKey_(
            sec,
            MakeIndexedKey_(L"TvTestDid", i).c_str(),
            tunerTvTestDidPerTuner[idx].empty()
            ? MakeDefaultTvTestDidLocal(i)
            : tunerTvTestDidPerTuner[idx]);

        AddIniKey_(sec, MakeIndexedKey_(L"TvTestExePath", i).c_str(), tunerTvTestExePathPerTuner[idx]);

        AddIniKey_(
            sec,
            MakeIndexedKey_(L"TvTestControlPipeName", i).c_str(),
            tunerTvTestControlPipeNamePerTuner[idx].empty()
            ? MakeDefaultTvTestControlPipeName_(i)
            : tunerTvTestControlPipeNamePerTuner[idx]);

        AddIniKey_(
            sec,
            MakeIndexedKey_(L"TvTestTsPipeName", i).c_str(),
            tunerTvTestTsPipeNamePerTuner[idx].empty()
            ? MakeDefaultTvTestTsPipeName_(i)
            : tunerTvTestTsPipeNamePerTuner[idx]);

        AddIniKey_(
            sec,
            MakeIndexedKey_(L"TvTestCommand", i).c_str(),
            tunerTvTestCommandPerTuner[idx].empty()
            ? L"PING"
            : tunerTvTestCommandPerTuner[idx]);

        // 番号なしキーはセクション一括保存により自然に消える。

        WriteIniSectionFast_(
            iniMutex,
            iniPath,
            tunerSection.c_str(),
            sec);
    }

    {
        std::vector<IniKeyValue_> sec;

        AddIniKey_(sec, L"EpgAutoMode", ToIniEpgAutoModeValue(epgAutoMode));
        AddIniKey_(sec, L"EpgBootGraceSec", IniIntText_(epgBootGraceSec));
        AddIniKey_(sec, L"EpgCatchUpOnStart", IniBoolText_(epgCatchUpOnStart));
        AddIniKey_(sec, L"EpgTickSeconds", IniIntText_(epgTickSeconds));
        AddIniKey_(sec, L"EpgIdleRequiredSec", IniIntText_(epgIdleRequiredSec));

        AddIniKey_(sec, L"EpgEnableDailyFull", IniBoolText_(epgEnableDailyFull));
        AddIniKey_(sec, L"EpgDailyFullTimeHHMM", FormatHHMM4(epgDailyFullHHMM));

        AddIniKey_(sec, L"EpgEnableHourly", IniBoolText_(epgEnableHourly));
        AddIniKey_(sec, L"EpgHourlyAtMinute", IniIntText_(epgHourlyAtMinute));
        AddIniKey_(sec, L"EpgLightUpdateIntervalMin", IniIntText_(epgLightUpdateIntervalMin));

        AddIniKey_(sec, L"ManualFullEstimatedMinutes", IniIntText_(manualFullEstimatedMinutes));
        AddIniKey_(sec, L"ManualFullGuardMinutes", IniIntText_(manualFullGuardMinutes));
        AddIniKey_(sec, L"ManualLightBaseMinutes", IniIntText_(manualLightBaseMinutes));
        AddIniKey_(sec, L"ManualLightPerChannelMinutes", IniIntText_(manualLightPerChannelMinutes));
        AddIniKey_(sec, L"ManualLightGuardMinutes", IniIntText_(manualLightGuardMinutes));

        AddIniKey_(sec, L"EpgHtmlRenderRoute", ToIniEpgHtmlRenderRouteValue(epgHtmlRenderRoute));

        // 旧 EPGUpdate キーはこの一括保存で消える。

        WriteIniSectionFast_(
            iniMutex,
            iniPath,
            L"EPGUpdate",
            sec);
    }

    {
        std::vector<IniKeyValue_> sec;

        AddIniKey_(sec, L"EnableExecution", IniBoolText_(reservationExecutionEnableExecution));
        AddIniKey_(sec, L"EnableWatchReservation", IniBoolText_(reservationExecutionEnableWatchReservation));
        AddIniKey_(sec, L"EnableTvTestWatch", IniBoolText_(reservationExecutionEnableTvTestWatch));
        AddIniKey_(sec, L"EnableRecordExecution", IniBoolText_(reservationExecutionEnableRecordExecution));
        AddIniKey_(sec, L"EnableTsPipeDuringWatch", IniBoolText_(reservationExecutionEnableTsPipeDuringWatch));
        AddIniKey_(sec, L"TvTestRecordShowVideo", IniBoolText_(reservationExecutionTvTestRecordShowVideo));
        AddIniKey_(sec, L"RecordFolder", reservationExecutionRecordFolder);
        AddIniKey_(sec, L"PrepareSeconds", IniIntText_(reservationExecutionPrepareSeconds));

        // 旧 ReservationExecution 詳細キーはこの一括保存で消える。

        WriteIniSectionFast_(
            iniMutex,
            iniPath,
            L"ReservationExecution",
            sec);
    }

    PurgeObsoleteManagedConfigKeys();

    timeShiftTunerSettingsDirty_ = false;
}

void ConfigManager::PurgeObsoleteManagedConfigKeys()
{
    // SaveConfig() が各管理セクションを WritePrivateProfileSectionW() で
    // 一括保存するため、同一セクション内の旧キーは保存時に自然に消える。
    //
    // ここでは旧形式の専用セクションだけ削除する。
    //
    // 残す:
    //   [Reservation]
    //     LoadConfig() でまだ読むため削除禁止。
    //
    // 触らない:
    //   [EpgServiceEnable]
    //   [EpgServiceEnable_spaceX]
    //     サービス有効/無効リストなので別管理。
    DeleteIniSectionFast_(
        iniMutex,
        iniPath,
        L"TunerSettings");

    DeleteIniSectionFast_(
        iniMutex,
        iniPath,
        L"TunerTVTest");
}

std::wstring ConfigManager::MakeEpgServiceSection(int space)
{
    return L"EpgServiceEnable_space" + std::to_wstring(space);
}

std::wstring ConfigManager::MakeEpgServiceKey(std::uint16_t onid, std::uint16_t tsid, std::uint16_t sid)
{
    return std::to_wstring((int)onid) + L"-" + std::to_wstring((int)tsid) + L"-" + std::to_wstring((int)sid);
}

std::wstring ConfigManager::TrimCopy(std::wstring s)
{
    auto is_ws = [](wchar_t c) { return (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'); };
    while (!s.empty() && is_ws(s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws(s.back())) s.pop_back();
    return s;
}

std::wstring ConfigManager::CsvEscape(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size() + 8);
    out.push_back(L'"');
    for (wchar_t ch : s) {
        if (ch == L'\r' || ch == L'\n') { out.push_back(L' '); continue; }
        if (ch == L'"') { out.push_back(L'"'); out.push_back(L'"'); continue; }
        out.push_back(ch);
    }
    out.push_back(L'"');
    return out;
}

bool ConfigManager::GetEpgServiceDefaultEnable(int space, bool fallback) const
{
    const std::wstring secSpace = MakeEpgServiceSection(space);
    const std::wstring defStr = fallback ? L"1" : L"0";

    if (!IsIniKeyMissing(secSpace, L"DefaultEnable")) {
        return ParseBool(ReadIniValue(secSpace, L"DefaultEnable", defStr));
    }
    if (!IsIniKeyMissing(L"EpgServiceEnable", L"DefaultEnable")) {
        return ParseBool(ReadIniValue(L"EpgServiceEnable", L"DefaultEnable", defStr));
    }
    return fallback;
}

bool ConfigManager::GetEpgServiceEnabled(int space, std::uint16_t onid, std::uint16_t tsid, std::uint16_t sid, bool fallbackDefault) const
{
    const std::wstring sec = MakeEpgServiceSection(space);
    const std::wstring key = MakeEpgServiceKey(onid, tsid, sid);

    if (IsIniKeyMissing(sec, key)) {
        return fallbackDefault;
    }

    const std::wstring val = ReadIniValue(sec, key, L"");
    if (val.empty()) {
        return fallbackDefault;
    }

    const std::size_t comma = val.find(L',');
    std::wstring head = (comma == std::wstring::npos) ? val : val.substr(0, comma);
    head = TrimCopy(std::move(head));
    if (head.empty()) return fallbackDefault;

    return ParseBool(head);
}

void ConfigManager::SetEpgServiceEnabled(
    int space,
    std::uint16_t onid,
    std::uint16_t tsid,
    std::uint16_t sid,
    bool enabled,
    int ch,
    int serviceType,
    const std::wstring& serviceName,
    const std::wstring& networkName)
{
    const std::wstring sec = MakeEpgServiceSection(space);
    const std::wstring key = MakeEpgServiceKey(onid, tsid, sid);

    std::wstring newVal;
    newVal.reserve(64 + serviceName.size() + networkName.size());
    newVal += (enabled ? L"1" : L"0");
    newVal += L",";
    newVal += CsvEscape(serviceName);
    newVal += L"," + std::to_wstring(space);
    newVal += L"," + std::to_wstring(ch);
    newVal += L"," + std::to_wstring(serviceType);
    newVal += L"," + std::to_wstring((int)sid);
    newVal += L"," + std::to_wstring((int)onid);
    newVal += L"," + std::to_wstring((int)tsid);
    newVal += L",";
    newVal += CsvEscape(networkName);

    if (!IsIniKeyMissing(sec, key)) {
        const std::wstring oldVal = ReadIniValue(sec, key, L"");
        if (oldVal == newVal) return;
    }

    WriteIniValue(sec, key, newVal);
}

std::wstring ConfigManager::MakeIndexedKey_(std::wstring_view base, int index)
{
    std::wstring k(base);
    k += std::to_wstring(index);
    return k;
}

int ConfigManager::ClampMinutes_(int v, int lo, int hi) noexcept
{
    return ClampInt(v, lo, hi);
}

int ConfigManager::ClampPrepareSeconds_(int v) noexcept
{
    return ClampInt(v, 0, 600);
}

int ConfigManager::ClampIdleMonitorSampleMs_(int v) noexcept
{
    return ClampInt(v, 1000, 60000);
}

int ConfigManager::ClampIdleMonitorPercent_(int v) noexcept
{
    return ClampInt(v, 1, 100);
}

int ConfigManager::ClampIdleMonitorIoKBps_(int v) noexcept
{
    return ClampInt(v, 0, 1024 * 1024);
}

std::wstring ConfigManager::MakeDefaultTvTestControlPipeName_(int tunerIndex)
{
    return MakeDefaultTvTestControlPipeNameLocal(tunerIndex);
}

std::wstring ConfigManager::MakeDefaultTvTestTsPipeName_(int tunerIndex)
{
    return MakeDefaultTvTestTsPipeNameLocal(tunerIndex);
}

std::wstring ConfigManager::MakeDefaultTvTestDid_(int tunerIndex)
{
    return MakeDefaultTvTestDidLocal(tunerIndex);
}

void ConfigManager::EnsureTunerVectors_() noexcept
{
    if (tunersCount < 1) tunersCount = 1;

    const std::size_t newSize = static_cast<std::size_t>(tunersCount);
    const std::size_t oldSize = bonDriverPathPerTuner.size();

    bonDriverPathPerTuner.resize(newSize);
    tunerInternalBonDriverPathPerTuner.resize(newSize);
    tunerTvTestBonDriverPathPerTuner.resize(newSize);
    tunerNamePerTuner.resize(newSize);
    tunerEnablePerTuner.resize(newSize, 1);

    tunerExternalAppPathPerTuner.resize(newSize);
    tunerReceiveModePerTuner.resize(newSize, static_cast<int>(TunerReceiveMode::Generic));
    tunerUseRolePerTuner.resize(newSize, static_cast<int>(TunerUseRole::General));
    tunerAllowParallelPerTuner.resize(newSize, 1);
    tunerPriorityPerTuner.resize(newSize, 0);
    tunerUseExternalAppPerTuner.resize(newSize, 0);
    tunerTimeShiftLockSpacePerTuner.resize(newSize, 0);
    tunerTimeShiftLockChannelPerTuner.resize(newSize, -1);
    tunerTimeShiftLockServiceIdPerTuner.resize(newSize, 0);
    tunerTimeShiftLockNetworkIdPerTuner.resize(newSize, 0);
    tunerTimeShiftLockTsIdPerTuner.resize(newSize, 0);
    tunerTimeShiftLockNamePerTuner.resize(newSize);
    tunerTvTestUseBridgePerTuner.resize(newSize, 0);
    tunerTvTestDidPerTuner.resize(newSize);
    tunerTvTestExePathPerTuner.resize(newSize);
    tunerTvTestControlPipeNamePerTuner.resize(newSize);
    tunerTvTestTsPipeNamePerTuner.resize(newSize);
    tunerTvTestCommandPerTuner.resize(newSize);

    if (newSize > oldSize) {
        for (std::size_t i = oldSize; i < newSize; ++i) {
            if (tunerNamePerTuner[i].empty()) {
                tunerNamePerTuner[i] = MakeDefaultTunerName((int)i);
            }

            if (tunerTvTestDidPerTuner[i].empty()) {
                tunerTvTestDidPerTuner[i] =
                    MakeDefaultTvTestDid_(static_cast<int>(i));
            }

            if (tunerTvTestControlPipeNamePerTuner[i].empty()) {
                tunerTvTestControlPipeNamePerTuner[i] =
                    MakeDefaultTvTestControlPipeName_(static_cast<int>(i));
            }

            if (tunerTvTestTsPipeNamePerTuner[i].empty()) {
                tunerTvTestTsPipeNamePerTuner[i] =
                    MakeDefaultTvTestTsPipeName_(static_cast<int>(i));
            }

            if (tunerTvTestCommandPerTuner[i].empty()) {
                tunerTvTestCommandPerTuner[i] = L"PING";
            }
        }
    }

    if (tunersActiveIndex < 0) tunersActiveIndex = 0;
    if (tunersActiveIndex >= tunersCount) tunersActiveIndex = tunersCount - 1;
}

std::wstring ConfigManager::GetBonDriverPathForTuner(int tunerIndex) const
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) return L"";
    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= bonDriverPathPerTuner.size()) return L"";
    return bonDriverPathPerTuner[i];
}

std::wstring ConfigManager::GetBonDriverPathForTunerOrDefault(int tunerIndex) const
{
    const std::wstring tunerPath = GetBonDriverPathForTuner(tunerIndex);

    if (!tunerPath.empty()) {
        return tunerPath;
    }

    if (tunerGlobalUseCommonBonDriver && !tunerGlobalCommonBonDriverPath.empty()) {
        return tunerGlobalCommonBonDriverPath;
    }

    return bonDriverPath;
}

void ConfigManager::SetBonDriverPathForTuner(int tunerIndex, const std::wstring& path)
{
    if (!IsValidTunerIndex_(tunerIndex)) {
        return;
    }

    EnsureTunerVectors_();
    bonDriverPathPerTuner[static_cast<std::size_t>(tunerIndex)] = path;
}

std::wstring ConfigManager::GetTunerInternalBonDriverPath(int tunerIndex) const
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) {
        return L"";
    }

    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerInternalBonDriverPathPerTuner.size()) {
        return L"";
    }

    return tunerInternalBonDriverPathPerTuner[i];
}

void ConfigManager::SetTunerInternalBonDriverPath(int tunerIndex, const std::wstring& path)
{
    if (!IsValidTunerIndex_(tunerIndex)) {
        return;
    }

    EnsureTunerVectors_();
    tunerInternalBonDriverPathPerTuner[static_cast<std::size_t>(tunerIndex)] = path;
}

std::wstring ConfigManager::GetTunerTvTestBonDriverPath(int tunerIndex) const
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) {
        return L"";
    }

    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerTvTestBonDriverPathPerTuner.size()) {
        return L"";
    }

    return tunerTvTestBonDriverPathPerTuner[i];
}

void ConfigManager::SetTunerTvTestBonDriverPath(int tunerIndex, const std::wstring& path)
{
    if (!IsValidTunerIndex_(tunerIndex)) {
        return;
    }

    EnsureTunerVectors_();
    tunerTvTestBonDriverPathPerTuner[static_cast<std::size_t>(tunerIndex)] = path;
}

bool ConfigManager::GetTunerEnabled(int tunerIndex) const noexcept
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) return false;
    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerEnablePerTuner.size()) return false;
    return tunerEnablePerTuner[i] != 0;
}

void ConfigManager::SetTunerEnabled(int tunerIndex, bool enabled) noexcept
{
    if (!IsValidTunerIndex_(tunerIndex)) {
        return;
    }

    EnsureTunerVectors_();
    tunerEnablePerTuner[static_cast<std::size_t>(tunerIndex)] = enabled ? 1 : 0;
}

bool ConfigManager::IsTunerUsableForMultiTuner(int tunerIndex) const
{
    if (!GetTunerEnabled(tunerIndex)) {
        return false;
    }

    const std::wstring path = TrimPathText_(GetMultiTunerBonDriverPath(tunerIndex));

    if (path.empty()) {
        return false;
    }

    return true;
}

std::vector<int> ConfigManager::BuildUsableMultiTunerIndexList() const
{
    std::vector<int> result;

    if (tunersCount < 1) {
        return result;
    }

    result.reserve(static_cast<std::size_t>(tunersCount));

    for (int i = 0; i < tunersCount; ++i) {
        if (!IsTunerUsableForMultiTuner(i)) {
            continue;
        }

        result.push_back(i);
    }

    return result;
}

std::wstring ConfigManager::GetTunerName(int tunerIndex) const
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) return L"";
    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerNamePerTuner.size()) return L"";
    return tunerNamePerTuner[i];
}

void ConfigManager::SetTunerName(int tunerIndex, const std::wstring& name)
{
    if (tunerIndex < 0) return;
    if (tunerIndex >= tunersCount) SetTunersCount(tunerIndex + 1);
    EnsureTunerVectors_();
    tunerNamePerTuner[static_cast<std::size_t>(tunerIndex)] = name;
}

std::wstring ConfigManager::GetTunerExternalAppPath(int tunerIndex) const
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) return L"";
    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerExternalAppPathPerTuner.size()) return L"";
    return tunerExternalAppPathPerTuner[i];
}

void ConfigManager::SetTunerExternalAppPath(int tunerIndex, const std::wstring& path)
{
    if (tunerIndex < 0) return;
    if (tunerIndex >= tunersCount) SetTunersCount(tunerIndex + 1);
    EnsureTunerVectors_();
    tunerExternalAppPathPerTuner[static_cast<std::size_t>(tunerIndex)] = path;
}

ConfigManager::TunerReceiveMode ConfigManager::GetTunerReceiveMode(int tunerIndex) const noexcept
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) return TunerReceiveMode::Generic;
    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerReceiveModePerTuner.size()) return TunerReceiveMode::Generic;
    return static_cast<TunerReceiveMode>(tunerReceiveModePerTuner[i]);
}

void ConfigManager::SetTunerReceiveMode(int tunerIndex, TunerReceiveMode mode) noexcept
{
    if (tunerIndex < 0) return;
    if (tunerIndex >= tunersCount) SetTunersCount(tunerIndex + 1);
    EnsureTunerVectors_();
    tunerReceiveModePerTuner[static_cast<std::size_t>(tunerIndex)] = static_cast<int>(mode);
}

ConfigManager::TunerUseRole ConfigManager::GetTunerUseRole(int tunerIndex) const noexcept
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) return TunerUseRole::General;
    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerUseRolePerTuner.size()) return TunerUseRole::General;
    return static_cast<TunerUseRole>(tunerUseRolePerTuner[i]);
}

void ConfigManager::SetTunerUseRole(int tunerIndex, TunerUseRole role) noexcept
{
    if (tunerIndex < 0) return;
    if (tunerIndex >= tunersCount) SetTunersCount(tunerIndex + 1);
    EnsureTunerVectors_();

    timeShiftTunerSettingsDirty_ = true;

    tunerUseRolePerTuner[static_cast<std::size_t>(tunerIndex)] =
        static_cast<int>(role);
}

bool ConfigManager::GetTunerAllowParallel(int tunerIndex) const noexcept
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) return false;
    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerAllowParallelPerTuner.size()) return false;
    return tunerAllowParallelPerTuner[i] != 0;
}

void ConfigManager::SetTunerAllowParallel(int tunerIndex, bool allow) noexcept
{
    if (tunerIndex < 0) return;
    if (tunerIndex >= tunersCount) SetTunersCount(tunerIndex + 1);
    EnsureTunerVectors_();
    tunerAllowParallelPerTuner[static_cast<std::size_t>(tunerIndex)] = allow ? 1 : 0;
}

int ConfigManager::GetTunerPriority(int tunerIndex) const noexcept
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) return 0;
    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerPriorityPerTuner.size()) return 0;
    return tunerPriorityPerTuner[i];
}

void ConfigManager::SetTunerPriority(int tunerIndex, int priority) noexcept
{
    if (tunerIndex < 0) return;
    if (tunerIndex >= tunersCount) SetTunersCount(tunerIndex + 1);
    EnsureTunerVectors_();
    tunerPriorityPerTuner[static_cast<std::size_t>(tunerIndex)] = ClampInt(priority, -100, 100);
}

int ConfigManager::GetTunerTimeShiftLockSpace(int tunerIndex) const noexcept
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) return 0;
    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerTimeShiftLockSpacePerTuner.size()) return 0;
    return tunerTimeShiftLockSpacePerTuner[i];
}

void ConfigManager::SetTunerTimeShiftLockSpace(int tunerIndex, int space) noexcept
{
    if (tunerIndex < 0) return;
    if (tunerIndex >= tunersCount) SetTunersCount(tunerIndex + 1);
    EnsureTunerVectors_();

    timeShiftTunerSettingsDirty_ = true;

    tunerTimeShiftLockSpacePerTuner[static_cast<std::size_t>(tunerIndex)] =
        ClampInt(space, 0, 15);
}

int ConfigManager::GetTunerTimeShiftLockChannel(int tunerIndex) const noexcept
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) return -1;
    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerTimeShiftLockChannelPerTuner.size()) return -1;
    return tunerTimeShiftLockChannelPerTuner[i];
}

void ConfigManager::SetTunerTimeShiftLockChannel(int tunerIndex, int channel) noexcept
{
    if (tunerIndex < 0) return;
    if (tunerIndex >= tunersCount) SetTunersCount(tunerIndex + 1);
    EnsureTunerVectors_();

    timeShiftTunerSettingsDirty_ = true;

    tunerTimeShiftLockChannelPerTuner[static_cast<std::size_t>(tunerIndex)] =
        ClampInt(channel, -1, 4096);
}

std::uint16_t ConfigManager::GetTunerTimeShiftLockServiceId(int tunerIndex) const noexcept
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) return 0;
    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerTimeShiftLockServiceIdPerTuner.size()) return 0;
    return static_cast<std::uint16_t>(ClampInt(tunerTimeShiftLockServiceIdPerTuner[i], 0, 65535));
}

void ConfigManager::SetTunerTimeShiftLockServiceId(int tunerIndex, std::uint16_t serviceId) noexcept
{
    if (tunerIndex < 0) return;
    if (tunerIndex >= tunersCount) SetTunersCount(tunerIndex + 1);
    EnsureTunerVectors_();

    timeShiftTunerSettingsDirty_ = true;

    tunerTimeShiftLockServiceIdPerTuner[static_cast<std::size_t>(tunerIndex)] =
        static_cast<int>(serviceId);
}

std::uint16_t ConfigManager::GetTunerTimeShiftLockNetworkId(int tunerIndex) const noexcept
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) return 0;
    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerTimeShiftLockNetworkIdPerTuner.size()) return 0;
    return static_cast<std::uint16_t>(ClampInt(tunerTimeShiftLockNetworkIdPerTuner[i], 0, 65535));
}

void ConfigManager::SetTunerTimeShiftLockNetworkId(int tunerIndex, std::uint16_t networkId) noexcept
{
    if (tunerIndex < 0) return;
    if (tunerIndex >= tunersCount) SetTunersCount(tunerIndex + 1);
    EnsureTunerVectors_();

    timeShiftTunerSettingsDirty_ = true;

    tunerTimeShiftLockNetworkIdPerTuner[static_cast<std::size_t>(tunerIndex)] =
        static_cast<int>(networkId);
}

std::uint16_t ConfigManager::GetTunerTimeShiftLockTsId(int tunerIndex) const noexcept
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) return 0;
    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerTimeShiftLockTsIdPerTuner.size()) return 0;
    return static_cast<std::uint16_t>(ClampInt(tunerTimeShiftLockTsIdPerTuner[i], 0, 65535));
}

void ConfigManager::SetTunerTimeShiftLockTsId(int tunerIndex, std::uint16_t tsId) noexcept
{
    if (tunerIndex < 0) return;
    if (tunerIndex >= tunersCount) SetTunersCount(tunerIndex + 1);
    EnsureTunerVectors_();

    timeShiftTunerSettingsDirty_ = true;

    tunerTimeShiftLockTsIdPerTuner[static_cast<std::size_t>(tunerIndex)] =
        static_cast<int>(tsId);
}

std::wstring ConfigManager::GetTunerTimeShiftLockName(int tunerIndex) const
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) return L"";
    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerTimeShiftLockNamePerTuner.size()) return L"";
    return tunerTimeShiftLockNamePerTuner[i];
}

void ConfigManager::SetTunerTimeShiftLockName(int tunerIndex, const std::wstring& name)
{
    if (tunerIndex < 0) return;
    if (tunerIndex >= tunersCount) SetTunersCount(tunerIndex + 1);
    EnsureTunerVectors_();

    timeShiftTunerSettingsDirty_ = true;

    tunerTimeShiftLockNamePerTuner[static_cast<std::size_t>(tunerIndex)] = name;
}

bool ConfigManager::GetTunerUseExternalApp(int tunerIndex) const noexcept
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) return false;
    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerUseExternalAppPerTuner.size()) return false;
    return tunerUseExternalAppPerTuner[i] != 0;
}

void ConfigManager::SetTunerUseExternalApp(int tunerIndex, bool enable) noexcept
{
    if (tunerIndex < 0) return;
    if (tunerIndex >= tunersCount) SetTunersCount(tunerIndex + 1);
    EnsureTunerVectors_();
    tunerUseExternalAppPerTuner[static_cast<std::size_t>(tunerIndex)] = enable ? 1 : 0;
}

bool ConfigManager::GetTunerTvTestUseBridge(int tunerIndex) const noexcept
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) return false;
    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerTvTestUseBridgePerTuner.size()) return false;
    return tunerTvTestUseBridgePerTuner[i] != 0;
}

void ConfigManager::SetTunerTvTestUseBridge(int tunerIndex, bool enable) noexcept
{
    if (tunerIndex < 0) return;
    if (tunerIndex >= tunersCount) SetTunersCount(tunerIndex + 1);
    EnsureTunerVectors_();
    tunerTvTestUseBridgePerTuner[static_cast<std::size_t>(tunerIndex)] = enable ? 1 : 0;
}

std::wstring ConfigManager::GetTunerTvTestDid(int tunerIndex) const
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) {
        return MakeDefaultTvTestDid_(0);
    }

    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerTvTestDidPerTuner.size()) {
        return MakeDefaultTvTestDid_(tunerIndex);
    }

    if (tunerTvTestDidPerTuner[i].empty()) {
        return MakeDefaultTvTestDid_(tunerIndex);
    }

    return tunerTvTestDidPerTuner[i];
}

void ConfigManager::SetTunerTvTestDid(int tunerIndex, const std::wstring& did)
{
    if (tunerIndex < 0) {
        return;
    }

    if (tunerIndex >= tunersCount) {
        SetTunersCount(tunerIndex + 1);
    }

    EnsureTunerVectors_();

    tunerTvTestDidPerTuner[static_cast<std::size_t>(tunerIndex)] =
        did.empty()
        ? MakeDefaultTvTestDid_(tunerIndex)
        : did;
}

std::wstring ConfigManager::GetTunerTvTestExePath(int tunerIndex) const
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) return L"";
    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerTvTestExePathPerTuner.size()) return L"";
    return tunerTvTestExePathPerTuner[i];
}

void ConfigManager::SetTunerTvTestExePath(int tunerIndex, const std::wstring& path)
{
    if (tunerIndex < 0) return;
    if (tunerIndex >= tunersCount) SetTunersCount(tunerIndex + 1);
    EnsureTunerVectors_();
    tunerTvTestExePathPerTuner[static_cast<std::size_t>(tunerIndex)] = path;
}

std::wstring ConfigManager::GetTunerTvTestControlPipeName(int tunerIndex) const
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) {
        return MakeDefaultTvTestControlPipeName_(0);
    }

    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerTvTestControlPipeNamePerTuner.size()) {
        return MakeDefaultTvTestControlPipeName_(tunerIndex);
    }

    if (tunerTvTestControlPipeNamePerTuner[i].empty()) {
        return MakeDefaultTvTestControlPipeName_(tunerIndex);
    }

    return tunerTvTestControlPipeNamePerTuner[i];
}

void ConfigManager::SetTunerTvTestControlPipeName(int tunerIndex, const std::wstring& path)
{
    if (tunerIndex < 0) return;
    if (tunerIndex >= tunersCount) SetTunersCount(tunerIndex + 1);
    EnsureTunerVectors_();

    const std::wstring value = path.empty()
        ? MakeDefaultTvTestControlPipeName_(tunerIndex)
        : path;

    tunerTvTestControlPipeNamePerTuner[static_cast<std::size_t>(tunerIndex)] = value;
}

std::wstring ConfigManager::GetTunerTvTestTsPipeName(int tunerIndex) const
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) {
        return MakeDefaultTvTestTsPipeName_(0);
    }

    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerTvTestTsPipeNamePerTuner.size()) {
        return MakeDefaultTvTestTsPipeName_(tunerIndex);
    }

    if (tunerTvTestTsPipeNamePerTuner[i].empty()) {
        return MakeDefaultTvTestTsPipeName_(tunerIndex);
    }

    return tunerTvTestTsPipeNamePerTuner[i];
}

void ConfigManager::SetTunerTvTestTsPipeName(int tunerIndex, const std::wstring& path)
{
    if (tunerIndex < 0) return;
    if (tunerIndex >= tunersCount) SetTunersCount(tunerIndex + 1);
    EnsureTunerVectors_();

    const std::wstring value = path.empty()
        ? MakeDefaultTvTestTsPipeName_(tunerIndex)
        : path;

    tunerTvTestTsPipeNamePerTuner[static_cast<std::size_t>(tunerIndex)] = value;
}

std::wstring ConfigManager::GetTunerTvTestCommand(int tunerIndex) const
{
    if (tunerIndex < 0 || tunerIndex >= tunersCount) return L"PING";
    const auto i = static_cast<std::size_t>(tunerIndex);
    if (i >= tunerTvTestCommandPerTuner.size()) return L"PING";
    if (tunerTvTestCommandPerTuner[i].empty()) return L"PING";
    return tunerTvTestCommandPerTuner[i];
}

void ConfigManager::SetTunerTvTestCommand(int tunerIndex, const std::wstring& command)
{
    if (tunerIndex < 0) return;
    if (tunerIndex >= tunersCount) SetTunersCount(tunerIndex + 1);
    EnsureTunerVectors_();

    tunerTvTestCommandPerTuner[static_cast<std::size_t>(tunerIndex)] =
        command.empty() ? L"PING" : command;
}

bool ConfigManager::IsValidTunerIndex_(int tunerIndex) const noexcept
{
    return tunerIndex >= 0 && tunerIndex < tunersCount;
}

void ConfigManager::LoadCpuDetectedFromIni_() noexcept
{
    try {
        cpuDetectedVendor =
            ReadIniValue(
                L"CPU",
                L"DetectedVendor",
                L"Unknown");

        if (cpuDetectedVendor.empty()) {
            cpuDetectedVendor = L"Unknown";
        }

        cpuDetectedLogicalCores =
            ClampUInt32_(
                ReadIniInt(
                    L"CPU",
                    L"DetectedLogicalCores",
                    0),
                0u,
                0u,
                256u);

        cpuDetectedSSE2 =
            ParseBool(ReadIniValue(L"CPU", L"DetectedSSE2", L"0"));

        cpuDetectedSSE3 =
            ParseBool(ReadIniValue(L"CPU", L"DetectedSSE3", L"0"));

        cpuDetectedSSSE3 =
            ParseBool(ReadIniValue(L"CPU", L"DetectedSSSE3", L"0"));

        cpuDetectedSSE4_1 =
            ParseBool(ReadIniValue(L"CPU", L"DetectedSSE4_1", L"0"));

        cpuDetectedSSE4_2 =
            ParseBool(ReadIniValue(L"CPU", L"DetectedSSE4_2", L"0"));

        cpuDetectedAVX =
            ParseBool(ReadIniValue(L"CPU", L"DetectedAVX", L"0"));

        cpuDetectedAVX2 =
            ParseBool(ReadIniValue(L"CPU", L"DetectedAVX2", L"0"));

        cpuDetectedAVX512 =
            ParseBool(ReadIniValue(L"CPU", L"DetectedAVX512", L"0"));

        cpuDetectedFMA =
            ParseBool(ReadIniValue(L"CPU", L"DetectedFMA", L"0"));

        cpuDetectedPOPCNT =
            ParseBool(ReadIniValue(L"CPU", L"DetectedPOPCNT", L"0"));

        cpuDetectedAESNI =
            ParseBool(ReadIniValue(L"CPU", L"DetectedAES_NI", L"0"));

        cpuDetectedSHA =
            ParseBool(ReadIniValue(L"CPU", L"DetectedSHA", L"0"));

        cpuDetectedBMI1 =
            ParseBool(ReadIniValue(L"CPU", L"DetectedBMI1", L"0"));

        cpuDetectedBMI2 =
            ParseBool(ReadIniValue(L"CPU", L"DetectedBMI2", L"0"));
    }
    catch (...) {
        cpuDetectedVendor = L"Unknown";
        cpuDetectedLogicalCores = 0;

        cpuDetectedSSE2 = false;
        cpuDetectedSSE3 = false;
        cpuDetectedSSSE3 = false;
        cpuDetectedSSE4_1 = false;
        cpuDetectedSSE4_2 = false;

        cpuDetectedAVX = false;
        cpuDetectedAVX2 = false;
        cpuDetectedAVX512 = false;

        cpuDetectedFMA = false;
        cpuDetectedPOPCNT = false;
        cpuDetectedAESNI = false;
        cpuDetectedSHA = false;
        cpuDetectedBMI1 = false;
        cpuDetectedBMI2 = false;
    }
}

void ConfigManager::RefreshCpuDetectedFromFeature_() noexcept
{
    try {
        CPUFeature& cpu =
            CPUFeature::GetInstance();



        cpuDetectedVendor = cpu.GetVendorString();

        if (cpuDetectedVendor.empty()) {
            cpuDetectedVendor = L"Unknown";
        }

        cpuDetectedLogicalCores =
            cpu.GetLogicalCoreCount();

        if (cpuDetectedLogicalCores == 0u) {
            const unsigned int hc =
                std::thread::hardware_concurrency();

            cpuDetectedLogicalCores =
                hc == 0u
                ? 1u
                : static_cast<std::uint32_t>(hc);
        }

        cpuDetectedSSE2 = cpu.hasSSE2;
        cpuDetectedSSE3 = cpu.hasSSE3;
        cpuDetectedSSSE3 = cpu.hasSSSE3;
        cpuDetectedSSE4_1 = cpu.hasSSE4_1;
        cpuDetectedSSE4_2 = cpu.hasSSE4_2;

        cpuDetectedAVX = cpu.hasAVX;
        cpuDetectedAVX2 = cpu.hasAVX2;
        cpuDetectedAVX512 = cpu.hasAVX512;

        cpuDetectedFMA = cpu.hasFMA;
        cpuDetectedPOPCNT = cpu.hasPOPCNT;
        cpuDetectedAESNI = cpu.hasAESNI;
        cpuDetectedSHA = cpu.hasSHA;
        cpuDetectedBMI1 = cpu.hasBMI1;
        cpuDetectedBMI2 = cpu.hasBMI2;
    }
    catch (...) {
        cpuDetectedVendor = L"Unknown";
        cpuDetectedLogicalCores = 1;

        cpuDetectedSSE2 = false;
        cpuDetectedSSE3 = false;
        cpuDetectedSSSE3 = false;
        cpuDetectedSSE4_1 = false;
        cpuDetectedSSE4_2 = false;

        cpuDetectedAVX = false;
        cpuDetectedAVX2 = false;
        cpuDetectedAVX512 = false;

        cpuDetectedFMA = false;
        cpuDetectedPOPCNT = false;
        cpuDetectedAESNI = false;
        cpuDetectedSHA = false;
        cpuDetectedBMI1 = false;
        cpuDetectedBMI2 = false;
    }
}

void ConfigManager::SyncCpuDetectedAtStartup_(
    bool hadConfigFile) noexcept
{
    const std::wstring oldVendor =
        cpuDetectedVendor;

    const std::uint32_t oldLogicalCores =
        cpuDetectedLogicalCores;

    const bool oldSSE2 = cpuDetectedSSE2;
    const bool oldSSE3 = cpuDetectedSSE3;
    const bool oldSSSE3 = cpuDetectedSSSE3;
    const bool oldSSE4_1 = cpuDetectedSSE4_1;
    const bool oldSSE4_2 = cpuDetectedSSE4_2;

    const bool oldAVX = cpuDetectedAVX;
    const bool oldAVX2 = cpuDetectedAVX2;
    const bool oldAVX512 = cpuDetectedAVX512;

    const bool oldFMA = cpuDetectedFMA;
    const bool oldPOPCNT = cpuDetectedPOPCNT;
    const bool oldAESNI = cpuDetectedAESNI;
    const bool oldSHA = cpuDetectedSHA;
    const bool oldBMI1 = cpuDetectedBMI1;
    const bool oldBMI2 = cpuDetectedBMI2;

    RefreshCpuDetectedFromFeature_();

    const bool changed =
        !hadConfigFile ||
        oldVendor != cpuDetectedVendor ||
        oldLogicalCores != cpuDetectedLogicalCores ||
        oldSSE2 != cpuDetectedSSE2 ||
        oldSSE3 != cpuDetectedSSE3 ||
        oldSSSE3 != cpuDetectedSSSE3 ||
        oldSSE4_1 != cpuDetectedSSE4_1 ||
        oldSSE4_2 != cpuDetectedSSE4_2 ||
        oldAVX != cpuDetectedAVX ||
        oldAVX2 != cpuDetectedAVX2 ||
        oldAVX512 != cpuDetectedAVX512 ||
        oldFMA != cpuDetectedFMA ||
        oldPOPCNT != cpuDetectedPOPCNT ||
        oldAESNI != cpuDetectedAESNI ||
        oldSHA != cpuDetectedSHA ||
        oldBMI1 != cpuDetectedBMI1 ||
        oldBMI2 != cpuDetectedBMI2;

    cpuDetectedChangedAtStartup_ = changed;

    if (changed) {
        SaveCpuSectionOnly_();
    }
}

void ConfigManager::SaveCpuSectionOnly_() noexcept
{
    try {


        std::vector<IniKeyValue_> sec;

        AddIniKey_(sec, L"SelectedFeature", GetSelectedCPUFeature());

        AddIniKey_(sec, L"DetectedVendor", cpuDetectedVendor);

        AddIniKey_(
            sec,
            L"DetectedLogicalCores",
            std::to_wstring(cpuDetectedLogicalCores));

        AddIniKey_(sec, L"DetectedSSE2", IniBoolText_(cpuDetectedSSE2));
        AddIniKey_(sec, L"DetectedSSE3", IniBoolText_(cpuDetectedSSE3));
        AddIniKey_(sec, L"DetectedSSSE3", IniBoolText_(cpuDetectedSSSE3));
        AddIniKey_(sec, L"DetectedSSE4_1", IniBoolText_(cpuDetectedSSE4_1));
        AddIniKey_(sec, L"DetectedSSE4_2", IniBoolText_(cpuDetectedSSE4_2));

        AddIniKey_(sec, L"DetectedAVX", IniBoolText_(cpuDetectedAVX));
        AddIniKey_(sec, L"DetectedAVX2", IniBoolText_(cpuDetectedAVX2));
        AddIniKey_(sec, L"DetectedAVX512", IniBoolText_(cpuDetectedAVX512));

        AddIniKey_(sec, L"DetectedFMA", IniBoolText_(cpuDetectedFMA));
        AddIniKey_(sec, L"DetectedPOPCNT", IniBoolText_(cpuDetectedPOPCNT));
        AddIniKey_(sec, L"DetectedAES_NI", IniBoolText_(cpuDetectedAESNI));
        AddIniKey_(sec, L"DetectedSHA", IniBoolText_(cpuDetectedSHA));
        AddIniKey_(sec, L"DetectedBMI1", IniBoolText_(cpuDetectedBMI1));
        AddIniKey_(sec, L"DetectedBMI2", IniBoolText_(cpuDetectedBMI2));

        AddIniKey_(sec, L"USE_SSE2", IniBoolText_(cpuUseSSE2));
        AddIniKey_(sec, L"USE_SSE3", IniBoolText_(cpuUseSSE3));
        AddIniKey_(sec, L"USE_SSSE3", IniBoolText_(cpuUseSSSE3));
        AddIniKey_(sec, L"USE_SSE4_1", IniBoolText_(cpuUseSSE4_1));
        AddIniKey_(sec, L"USE_SSE4_2", IniBoolText_(cpuUseSSE4_2));

        AddIniKey_(sec, L"USE_AVX", IniBoolText_(cpuUseAVX));
        AddIniKey_(sec, L"USE_AVX2", IniBoolText_(cpuUseAVX2));
        AddIniKey_(sec, L"USE_AVX512", IniBoolText_(cpuUseAVX512));

        AddIniKey_(sec, L"USE_FMA", IniBoolText_(cpuUseFMA));
        AddIniKey_(sec, L"USE_POPCNT", IniBoolText_(cpuUsePOPCNT));
        AddIniKey_(sec, L"USE_AES_NI", IniBoolText_(cpuUseAESNI));
        AddIniKey_(sec, L"USE_SHA", IniBoolText_(cpuUseSHA));
        AddIniKey_(sec, L"USE_BMI1", IniBoolText_(cpuUseBMI1));
        AddIniKey_(sec, L"USE_BMI2", IniBoolText_(cpuUseBMI2));

        AddIniKey_(
            sec,
            L"EnableParallelWorkers",
            IniBoolText_(cpuEnableParallelWorkers));

        AddIniKey_(
            sec,
            L"RuntimeWorkerReserveCores",
            std::to_wstring(cpuRuntimeWorkerReserveCores));

        AddIniKey_(
            sec,
            L"RuntimeMaxWorkerThreads",
            std::to_wstring(cpuRuntimeMaxWorkerThreads));

        WriteIniSectionFast_(
            iniMutex,
            iniPath,
            L"CPU",
            sec);
    }
    catch (...) {
    }
}

std::wstring ConfigManager::GetCpuDetectedVendor() const
{
    return cpuDetectedVendor;
}

std::uint32_t ConfigManager::GetCpuDetectedLogicalCores() const noexcept
{
    return cpuDetectedLogicalCores == 0u ? 1u : cpuDetectedLogicalCores;
}

std::uint32_t ConfigManager::GetCpuRuntimeWorkerReserveCores() const noexcept
{
    return cpuRuntimeWorkerReserveCores;
}

void ConfigManager::SetCpuRuntimeWorkerReserveCores(
    std::uint32_t v) noexcept
{
    if (v > 64u) {
        v = 64u;
    }

    cpuRuntimeWorkerReserveCores = v;
}

std::uint32_t ConfigManager::GetCpuRuntimeMaxWorkerThreads() const noexcept
{
    return cpuRuntimeMaxWorkerThreads;
}

void ConfigManager::SetCpuRuntimeMaxWorkerThreads(
    std::uint32_t v) noexcept
{
    if (v > 256u) {
        v = 256u;
    }

    cpuRuntimeMaxWorkerThreads = v;
}

std::uint32_t ConfigManager::DecideRuntimeWorkerCount(
    std::uint32_t requestedMax,
    std::uint32_t hardMax,
    std::uint32_t minWorkers) const noexcept
{
    if (minWorkers == 0u) {
        minWorkers = 1u;
    }

    if (!cpuEnableParallelWorkers) {
        return minWorkers;
    }

    std::uint32_t cores =
        GetCpuDetectedLogicalCores();

    if (cores == 0u) {
        cores = 1u;
    }

    std::uint32_t workers = cores;

    const std::uint32_t reserve =
        GetCpuRuntimeWorkerReserveCores();

    if (workers > reserve + minWorkers) {
        workers -= reserve;
    }
    else {
        workers = minWorkers;
    }

    const std::uint32_t runtimeMax =
        GetCpuRuntimeMaxWorkerThreads();

    if (runtimeMax > 0u && workers > runtimeMax) {
        workers = runtimeMax;
    }

    if (requestedMax > 0u && workers > requestedMax) {
        workers = requestedMax;
    }

    if (hardMax > 0u && workers > hardMax) {
        workers = hardMax;
    }

    if (workers < minWorkers) {
        workers = minWorkers;
    }

    return workers;
}
