// channel_scan_GUI.cpp  (観測強化 + ログ安全化版 / ConfigManager既存APIのみ使用)

#include "channel_scan_GUI.hpp"

#include <Windows.h>
#undef max
#undef min

#include <thread>
#include <chrono>
#include <memory>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <limits>
#include <type_traits>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <vector>
#include <sstream>
#include <algorithm>
#include <string_view>

#include "TSParser.h"
#include "FilterManager.hpp"
#include "MPMCRingBuffer.hpp"
#include "DebugManager.h"
#include "TSStreamUtils.hpp"
#include "PacketDumpManager.hpp"
#include "channellist.hpp"
#include "AribDecodecallUnit.hpp"
#include "PhysicalChannelListBuilder.hpp"
#include "TsPacketCapture.hpp"
#include "FilterManagerRunner.hpp"
#include "ConfigManager.h"

// ★GUI向けスナップショット
#include "GuiServiceSnapshot.hpp"

using namespace TSStreamUtils;

namespace {

    // ini の bool 取得（0/1）
    bool ReadIniBool(const std::wstring& iniPath,
        const wchar_t* section, const wchar_t* key, bool defVal) noexcept
    {
        if (iniPath.empty() || !section || !key) return defVal;
        const UINT v = GetPrivateProfileIntW(section, key, defVal ? 1u : 0u, iniPath.c_str());
        return v != 0u;
    }


    // ------------------------------------------------------------
    // UTF-8(BOM) ファイル出力ヘルパ（GUI で locale が未設定でも文字化けしない）
    // - std::wofstream は locale 依存で失敗しやすいので使わない
    // - BOM 付き UTF-8 で出力して Windows メモ帳等でも確実に読めるようにする
    // ------------------------------------------------------------
    std::string WideToUtf8(std::wstring_view w)
    {
        if (w.empty()) return {};

        // まずは厳密変換を試す（不正サロゲート等があれば失敗）
        int needed = ::WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            w.data(),
            static_cast<int>(w.size()),
            nullptr,
            0,
            nullptr,
            nullptr);

        if (needed <= 0) {
            // フォールバック（置換を許可）
            needed = ::WideCharToMultiByte(
                CP_UTF8,
                0,
                w.data(),
                static_cast<int>(w.size()),
                nullptr,
                0,
                nullptr,
                nullptr);
            if (needed <= 0) return {};
        }

        std::string out(static_cast<std::size_t>(needed), '\0');
        const int wrote = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            w.data(),
            static_cast<int>(w.size()),
            out.data(),
            needed,
            nullptr,
            nullptr);

        if (wrote <= 0) return {};
        if (wrote != needed) out.resize(static_cast<std::size_t>(wrote));
        return out;
    }

    class Utf8BomWriter final
    {
    public:
        Utf8BomWriter() = default;

        explicit Utf8BomWriter(const std::filesystem::path& path, bool append = false)
        {
            Open(path, append);
        }

        bool Open(const std::filesystem::path& path, bool append = false)
        {
            Close();

            m_path = path;
            std::ios::openmode mode = std::ios::binary | std::ios::out;
            if (append) mode |= std::ios::app;

            m_ofs.open(path, mode);
            if (!m_ofs.is_open()) return false;

            // append でも新規作成/空の場合は BOM を入れる
            if (!append) {
                WriteBom();
            }
            else {
                // app は末尾に移動済みなので、存在しない or 空のときのみ BOM を追加
                // （追記中に BOM を混入させない）
                try {
                    if (!std::filesystem::exists(path) || std::filesystem::file_size(path) == 0) {
                        WriteBom();
                    }
                }
                catch (...) {
                    // file_size 例外は無視（BOMなしでも致命ではない）
                }
            }
            return true;
        }

        bool IsOpen() const noexcept { return m_ofs.is_open(); }

        void Close()
        {
            if (m_ofs.is_open()) {
                m_ofs.flush();
                m_ofs.close();
            }
        }

        void Write(std::wstring_view s)
        {
            if (!m_ofs.is_open()) return;
            const std::string u8 = WideToUtf8(s);
            if (!u8.empty()) m_ofs.write(u8.data(), static_cast<std::streamsize>(u8.size()));
        }

        void WriteLine(std::wstring_view s)
        {
            Write(s);
            // Windows 向けに CRLF
            m_ofs.write("\r\n", 2);
        }

        ~Utf8BomWriter()
        {
            Close();
        }

    private:
        void WriteBom()
        {
            static constexpr unsigned char kBom[3] = { 0xEF, 0xBB, 0xBF };
            m_ofs.write(reinterpret_cast<const char*>(kBom), 3);
        }

        std::filesystem::path m_path;
        std::ofstream m_ofs;
    };

    std::wstring FormatHMS(std::chrono::seconds sec)
    {
        using ll = long long;
        ll t = sec.count();
        if (t < 0) t = 0;

        const ll h = t / 3600;
        const ll m = (t % 3600) / 60;
        const ll s = (t % 60);

        std::wstringstream ss;
        ss << std::setfill(L'0');
        if (h > 0) ss << std::setw(2) << h << L":" << std::setw(2) << m << L":" << std::setw(2) << s;
        else       ss << std::setw(2) << m << L":" << std::setw(2) << s;
        return ss.str();
    }

    void EmitStatus(const ChannelScanGUIOptions& opt, const std::wstring& s)
    {
        if (opt.onStatusText) opt.onStatusText(s); // worker thread から呼ばれる
    }

    void EmitRunningState(const ChannelScanGUIOptions& opt, bool running)
    {
        if (opt.onRunningState) opt.onRunningState(running); // worker thread から呼ばれる
    }

    bool IsCancelRequested(const ChannelScanGUIOptions& opt) noexcept
    {
        return opt.cancelRequested != nullptr
            && opt.cancelRequested->load(std::memory_order_acquire);
    }

    void SafeStopCapture(
        TsPacketCaptureSession& capture,
        bool& captureStarted,
        std::atomic_bool* captureCancel,
        bool requestCancel) noexcept
    {
        if (requestCancel && captureCancel != nullptr) {
            captureCancel->store(true, std::memory_order_release);
        }

        if (!captureStarted) {
            return;
        }

        try {
            capture.StopAndJoin();
        }
        catch (...) {
            DebugLog(L"[WARN] capture.StopAndJoin() で例外が発生しました。");
        }

        captureStarted = false;
    }

    void WaitForRunnerDrain(
        FilterManager& filterManager,
        const ChannelScanGUIOptions& opt,
        int overallWaitMs = 2000,
        int idleWaitMs = 250) noexcept
    {
        using namespace std::chrono;

        try {
            const auto overall0 = steady_clock::now();
            auto idle0 = steady_clock::now();
            std::size_t last = filterManager.GetProcessedPacketCount();

            for (;;) {
                std::this_thread::sleep_for(milliseconds(10));

                const std::size_t cur = filterManager.GetProcessedPacketCount();
                if (cur != last) {
                    last = cur;
                    idle0 = steady_clock::now();
                }

                if (steady_clock::now() - idle0 >= milliseconds(idleWaitMs)) {
                    break;
                }
                if (steady_clock::now() - overall0 >= milliseconds(overallWaitMs)) {
                    break;
                }
                if (IsCancelRequested(opt) &&
                    steady_clock::now() - overall0 >= milliseconds(100)) {
                    break;
                }
            }
        }
        catch (...) {
            DebugLog(L"[WARN] WaitForRunnerDrain() 中に例外が発生しました。");
        }
    }

    void SafeStopRunner(
        FilterManagerRunner& runner,
        bool& runnerStarted) noexcept
    {
        if (!runnerStarted) {
            return;
        }

        try {
            runner.Stop();
        }
        catch (...) {
            DebugLog(L"[WARN] runner.Stop() で例外が発生しました。");
        }

        runnerStarted = false;
    }

    // ---- 観測用 ----
    struct WStringObs {
        std::size_t len = 0;
        bool hasNul = false;
        bool hasCtrl = false;
        std::size_t ctrlCount = 0;
        bool hasCrLfTab = false;
        std::size_t crlfTabCount = 0;
    };

    WStringObs ObserveWString(const std::wstring& s)
    {
        WStringObs o{};
        o.len = s.size();
        for (wchar_t ch : s) {
            if (ch == L'\0') o.hasNul = true;

            // CR/LF/TAB はログ破壊原因になり得るので別枠で観測
            if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
                o.hasCrLfTab = true;
                ++o.crlfTabCount;
                continue;
            }

            if (ch < 0x20) {
                o.hasCtrl = true;
                ++o.ctrlCount;
            }
        }
        return o;
    }

    // ログが壊れないように、wstringを「ASCIIのみ」に逃がす（\uXXXX）
    std::wstring EscapeForLog(const std::wstring& s, std::size_t maxLen = 256)
    {
        std::wstringstream ss;
        const std::size_t n = (std::min)(s.size(), maxLen);

        for (std::size_t i = 0; i < n; ++i) {
            wchar_t ch = s[i];

            // 明示的に潰す（ログ結合/崩れ防止）
            if (ch == L'\r') { ss << L"\\r"; continue; }
            if (ch == L'\n') { ss << L"\\n"; continue; }
            if (ch == L'\t') { ss << L"\\t"; continue; }
            if (ch == L'\0') { ss << L"\\0"; continue; }

            // printable ASCII
            if (ch >= 0x20 && ch <= 0x7E) {
                // バックスラッシュはエスケープ
                if (ch == L'\\') ss << L"\\\\";
                else ss << ch;
                continue;
            }

            // その他は \uXXXX
            ss << L"\\u" << std::hex << std::setw(4) << std::setfill(L'0') << (unsigned)ch
                << std::dec << std::setfill(L' ');
        }

        if (s.size() > n) ss << L"...";
        return ss.str();
    }

    // 文字列の先頭N個を hex 化して観測（UTF-16コード単位）
    std::wstring ToHexList(const std::wstring& s, std::size_t maxItems = 24)
    {
        std::wstringstream ss;
        ss << L"[";
        const std::size_t n = (std::min)(s.size(), maxItems);
        for (std::size_t i = 0; i < n; ++i) {
            if (i) ss << L" ";
            ss << L"0x" << std::hex << std::setw(4) << std::setfill(L'0') << (unsigned)s[i]
                << std::dec << std::setfill(L' ');
        }
        if (s.size() > n) ss << L" ...";
        ss << L"]";
        return ss.str();
    }

    // 観測用：サービス一覧を「先頭N件」ログに出す（ログ安全化版）
    template <class ServiceT>
    void LogServiceListPreview(int space, int ch,
        const std::vector<ServiceT>& servicesSafe,
        std::size_t previewN = 12)
    {
        DebugLogFormat(L"[OBS][SRV] space=%d ch=%d services=%zu", space, ch, servicesSafe.size());

        const std::size_t show = (std::min<std::size_t>)(servicesSafe.size(), previewN);
        for (std::size_t i = 0; i < show; ++i) {
            const auto& s = servicesSafe[i];

            const auto oP = ObserveWString(s.provider_name);
            const auto oN = ObserveWString(s.service_name);

            const std::wstring provEsc = EscapeForLog(s.provider_name);
            const std::wstring nameEsc = EscapeForLog(s.service_name);

            const std::wstring provHex = ToHexList(s.provider_name);
            const std::wstring nameHex = ToHexList(s.service_name);

            DebugLogFormat(
                L"[OBS][SRV] #%zu sid=%u tsid=%u onid=%u type=%d sec=%d freeCA=%d "
                L"prov(len=%zu nul=%d ctrl=%d crlfTab=%d) name(len=%zu nul=%d ctrl=%d crlfTab=%d) "
                L"provEsc='%ls' nameEsc='%ls' provHex=%ls nameHex=%ls",
                i,
                (unsigned)s.service_id,
                (unsigned)s.transport_stream_id,
                (unsigned)s.original_network_id,
                (int)s.service_type,
                (int)s.section_number,
                (int)(s.free_CA_mode ? 1 : 0),
                oP.len, (int)oP.hasNul, (int)oP.hasCtrl, (int)oP.hasCrLfTab,
                oN.len, (int)oN.hasNul, (int)oN.hasCtrl, (int)oN.hasCrLfTab,
                provEsc.c_str(),
                nameEsc.c_str(),
                provHex.c_str(),
                nameHex.c_str()
            );
        }
        if (servicesSafe.size() > show) {
            DebugLogFormat(L"[OBS][SRV] ... (%zu more)", servicesSafe.size() - show);
        }
    }

} // namespace

// ------------------------------------------------------------
// channel_scan_start（GUI用の入口）
// ------------------------------------------------------------
void channel_scan_start(IBonDriver2* bon, FilterManagerMode mode)
{
    ChannelScanGUIOptions opt;
    opt.enablePromptInConsole = false;
    opt.enableTsDump = false;
    opt.enableRingDump = false;

    channel_scan_GUI(bon, mode, opt);
}

void channel_scan_start(IBonDriver2* bon, FilterManagerMode mode, const ChannelScanGUIOptions& opt)
{
    channel_scan_GUI(bon, mode, opt);
}

// ------------------------------------------------------------
// オプション省略版（プロンプト無し / ダンプ無し）
// ------------------------------------------------------------
void channel_scan_GUI(IBonDriver2* bon, FilterManagerMode mode)
{
    ChannelScanGUIOptions opt;
    opt.enablePromptInConsole = false;
    opt.enableTsDump = false;
    opt.enableRingDump = false;

    channel_scan_GUI(bon, mode, opt);
}

// ------------------------------------------------------------
// 本体
// ------------------------------------------------------------
void channel_scan_GUI(IBonDriver2* bon, FilterManagerMode mode, const ChannelScanGUIOptions& opt)
{
    if (!bon) {
        DebugLog(L"[ERROR] BonDriver が無効です。");
        return;
    }

    EmitRunningState(opt, true);

    const auto scanStart = std::chrono::steady_clock::now();

    bool canceled = false;
    bool abortedByError = false;
    bool runningStateClosed = false;

    auto finalizeScan = [&](const wchar_t* statusLabel, const wchar_t* logMessage) noexcept {
        if (runningStateClosed) {
            return;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - scanStart);

        if (logMessage != nullptr) {
            DebugLog(logMessage);
        }

        std::wstring status = statusLabel;
        status += L"  ";
        status += FormatHMS(elapsed);
        EmitStatus(opt, status);

        FlushDebugLog();
        EmitRunningState(opt, false);
        runningStateClosed = true;
        };

    try {
        auto& cfg = ConfigManager::GetInstance();
        if (opt.reloadConfigAtStart) {
            cfg.LoadConfig();
        }

        const std::wstring iniPath = cfg.GetIniPath();

        struct FixedSnapshot {
            std::wstring ini;
            bool enableTsDump = false;
            bool enableRingDump = false;
            bool enableSdtDebug = false;
            bool enableNitDebug = false;
            bool aribTraceEnable = false;
            int captureSec = 0;
            std::size_t ringBufferSize = 0;
            std::size_t packetReserve = 0;
            int sleepOnFailMs = 0;
        } snap;

        snap.ini = iniPath;
        snap.enableTsDump = ReadIniBool(iniPath, L"ChannelScan", L"EnableTsDump", false);
        snap.enableRingDump = ReadIniBool(iniPath, L"ChannelScan", L"EnableRingDump", false);
        snap.enableSdtDebug = cfg.GetEnableSDTDebug();
        snap.enableNitDebug = cfg.GetEnableNITDebug();

        const bool aribEnableTrace = cfg.GetAribEnableTrace();
        const bool aribTraceG = cfg.GetAribTraceG();
        const bool aribTraceChars = cfg.GetAribTraceChars();
        snap.aribTraceEnable = (aribEnableTrace || aribTraceG || aribTraceChars);

        snap.captureSec = (opt.captureSec > 0) ? opt.captureSec : 18;
        snap.ringBufferSize = (opt.ringBufferSize > 0) ? opt.ringBufferSize : (32u * 1024u * 1024u);
        snap.packetReserve = (opt.packetReserve > 0) ? opt.packetReserve : 4096u;
        snap.sleepOnFailMs = (opt.sleepOnFailMs >= 0) ? opt.sleepOnFailMs : 1;

        if (opt.dumpFixedConfigSnapshot) {
            DebugLogFormat(L"[GUI][CFG] ini=%ls", snap.ini.c_str());
            DebugLogFormat(L"[GUI][CFG] ChannelScan.EnableTsDump=%d, EnableRingDump=%d (opt TsDump=%d, RingDump=%d)",
                (int)snap.enableTsDump, (int)snap.enableRingDump, (int)opt.enableTsDump, (int)opt.enableRingDump);
            DebugLogFormat(L"[GUI][CFG] Debug.EnableSDTDebug=%d, EnableNITDebug=%d",
                (int)snap.enableSdtDebug, (int)snap.enableNitDebug);
            DebugLogFormat(L"[GUI][CFG] Arib.TraceEnable=%d (EnableTrace=%d TraceG=%d TraceChars=%d)",
                (int)snap.aribTraceEnable, (int)aribEnableTrace, (int)aribTraceG, (int)aribTraceChars);
            DebugLogFormat(L"[GUI][CFG] captureSec=%d, ringBufferSize=%zu, packetReserve=%zu, sleepOnFailMs=%d",
                snap.captureSec, snap.ringBufferSize, snap.packetReserve, snap.sleepOnFailMs);
            DebugLogFormat(L"[GUI][CFG] scanAllSpaces=%d, maxSpaceToTry=%d, spacesToScan.size=%zu, defaultSpace=%d",
                (int)opt.scanAllSpaces, opt.maxSpaceToTry, opt.spacesToScan.size(), opt.space);
        }

        std::vector<int> spaces;
        if (!opt.spacesToScan.empty()) {
            spaces = opt.spacesToScan;
        }
        else if (opt.scanAllSpaces) {
            const int maxS = (opt.maxSpaceToTry < 0) ? 0 : opt.maxSpaceToTry;
            spaces.reserve((std::size_t)maxS + 1);
            for (int s = 0; s <= maxS; ++s) {
                spaces.push_back(s);
            }
        }
        else {
            spaces.push_back(opt.space);
        }

        const bool enableTsDump = (opt.enableTsDump || snap.enableTsDump);
        const bool enableRingDump = (opt.enableRingDump || snap.enableRingDump);

        DebugLog(L"[INFO] チャンネルスキャン（GUI）: TSストリーム解析を開始します。");
        EmitStatus(opt, L"ChannelScan: START  00:00");

        aribdecode::AribDecodecallUnit aribUnit;
        aribUnit.EnableTraceForTest(snap.aribTraceEnable);

        for (int space : spaces)
        {
            if (IsCancelRequested(opt)) {
                canceled = true;
                break;
            }

            DebugLogFormat(L"[SPACE] begin space=%d", space);

            {
                const std::wstring channelListFile = GetChannelListFileName(space);
                InitializeChannelListFile(channelListFile);
                DebugLogFormat(L"[SPACE] channellist initialized: %ls", channelListFile.c_str());
            }

            for (int ch = 0;; ++ch)
            {
                if (IsCancelRequested(opt)) {
                    canceled = true;
                    break;
                }

                LPCTSTR chName = bon->EnumChannelName(space, ch);
                if (!chName) {
                    DebugLogFormat(L"[SPACE] EnumChannelName end: space=%d last_ch=%d", space, ch);
                    break;
                }

                TSStreamUtils::ResetTsStreamStats();
                const auto chStart = std::chrono::steady_clock::now();

                {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - scanStart);
                    std::wstring msg = L"Space " + std::to_wstring(space) + L" / CH " + std::to_wstring(ch);
                    msg += L" ("; msg += chName; msg += L")  ";
                    msg += FormatHMS(elapsed);
                    EmitStatus(opt, msg);
                }

                DebugLogFormat(L"[BEGIN] space=%d, ch=%d (%ls)", space, ch, chName);

                try {
                    if (IsCancelRequested(opt)) {
                        canceled = true;
                        break;
                    }

                    if (!bon->SetChannel(space, ch)) {
                        DebugLogFormat(L"[WARN] SetChannel failed: space=%d, ch=%d", space, ch);
                        continue;
                    }

                    if (IsCancelRequested(opt)) {
                        canceled = true;
                        break;
                    }

                    if (!TSStreamUtils::WaitForSignalLock(bon)) {
                        if (IsCancelRequested(opt)) {
                            canceled = true;
                            break;
                        }
                        DebugLogFormat(L"[WARN] SignalLock failed: space=%d, ch=%d", space, ch);
                        continue;
                    }

                    if (IsCancelRequested(opt)) {
                        canceled = true;
                        break;
                    }

                    const float level = bon->GetSignalLevel();
                    DebugLogFormat(L"[INFO] Locked: space=%d, ch=%d, signal=%.2f dB", space, ch, level);

                    TSParser parser;
                    FilterManager filterManager(&parser, nullptr);
                    filterManager.ApplyMode(mode);

                    MPMCRingBuffer<TsPacket> ringBuffer(snap.ringBufferSize);

                    FilterManagerRunner runner(&filterManager, &ringBuffer);
                    bool runnerStarted = false;

                    TsPacketCaptureOptions capOpt;
                    capOpt.captureSec = snap.captureSec;
                    capOpt.packetReserve = snap.packetReserve;
                    capOpt.sleepOnFailMs = snap.sleepOnFailMs;
                    capOpt.receiveFn = &ReceiveTsStreamBlock;

                    std::atomic_bool localCancel{ false };
                    std::atomic_bool* captureCancel =
                        (opt.cancelRequested != nullptr) ? opt.cancelRequested : &localCancel;

                    TsPacketCaptureSession capture;
                    bool captureStarted = false;

                    auto cleanupChannel = [&](bool requestCancel) noexcept {
                        SafeStopCapture(capture, captureStarted, captureCancel, requestCancel);
                        WaitForRunnerDrain(filterManager, opt);
                        SafeStopRunner(runner, runnerStarted);
                        };

                    runner.Start();
                    runnerStarted = true;

                    if (!capture.Start(bon, &ringBuffer, capOpt, captureCancel)) {
                        DebugLogFormat(L"[ERROR] TsPacketCapture.Start failed: space=%d ch=%d", space, ch);
                        cleanupChannel(false);
                        if (IsCancelRequested(opt)) {
                            canceled = true;
                            break;
                        }
                        continue;
                    }
                    captureStarted = true;

                    constexpr int kTickMs = 100;
                    const int totalTicks = (snap.captureSec > 0)
                        ? (snap.captureSec * 1000 / kTickMs)
                        : 0;

                    for (int tick = 0; tick < totalTicks; ++tick) {
                        if (IsCancelRequested(opt)) {
                            canceled = true;
                            EmitStatus(opt, L"停止要求を受信しました。現在のチャンネル処理を終了します…");
                            break;
                        }

                        std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));

                        if (((tick + 1) % 10) != 0 && (tick + 1) != totalTicks) {
                            continue;
                        }

                        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - scanStart);
                        const auto thisCh = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - chStart);

                        std::wstring msg = L"Space " + std::to_wstring(space) + L" / CH " + std::to_wstring(ch);
                        msg += L"  Capturing  ";
                        msg += FormatHMS(elapsed);
                        msg += L"  (ch ";
                        msg += FormatHMS(thisCh);
                        msg += L")";
                        EmitStatus(opt, msg);
                    }

                    const bool cancelNow = canceled || IsCancelRequested(opt);
                    cleanupChannel(cancelNow);

                    if (cancelNow) {
                        DebugLogFormat(L"[CANCEL] stop requested before post-process: space=%d ch=%d", space, ch);
                        break;
                    }

                    const auto capCounts = capture.GetCounts();
                    const std::size_t writeOk = capCounts.pushOk;
                    const std::size_t writeFail = capCounts.pushFail;

                    const auto& servicesRaw = filterManager.GetServiceList();
                    using ServiceT = std::decay_t<decltype(servicesRaw)>::value_type;

                    DebugLogFormat(L"[OBS][A] servicesRaw.size=%zu ptr=%p", servicesRaw.size(), (const void*)servicesRaw.data());
                    if (!servicesRaw.empty()) {
                        const auto& r0 = servicesRaw[0];
                        const auto oP = ObserveWString(r0.provider_name);
                        const auto oN = ObserveWString(r0.service_name);
                        DebugLogFormat(L"[OBS][A] raw[0] sid=%u prov(len=%zu nul=%d ctrl=%d crlfTab=%d) name(len=%zu nul=%d ctrl=%d crlfTab=%d)",
                            (unsigned)r0.service_id,
                            oP.len, (int)oP.hasNul, (int)oP.hasCtrl, (int)oP.hasCrLfTab,
                            oN.len, (int)oN.hasNul, (int)oN.hasCtrl, (int)oN.hasCrLfTab
                        );
                    }

                    std::vector<ServiceT> servicesSafe;
                    servicesSafe.reserve(servicesRaw.size());
                    servicesSafe.insert(servicesSafe.end(), servicesRaw.begin(), servicesRaw.end());

                    DebugLogFormat(L"[OBS][B] servicesSafe.size=%zu ptr=%p", servicesSafe.size(), (const void*)servicesSafe.data());

                    if (opt.normalizeStringsForWin32) {
                        for (std::size_t i = 0; i < servicesSafe.size(); ++i) {
                            auto& s = servicesSafe[i];

                            const auto beforeP = ObserveWString(s.provider_name);
                            const auto beforeN = ObserveWString(s.service_name);

                            GuiSnapshot::NormalizeForWin32(s.provider_name);
                            GuiSnapshot::NormalizeForWin32(s.service_name);

                            const auto afterP = ObserveWString(s.provider_name);
                            const auto afterN = ObserveWString(s.service_name);

                            DebugLogFormat(
                                L"[OBS][NORM] space=%d ch=%d #%zu provider changed=%d len %zu->%zu nul %d->%d ctrl %d(%zu)->%d(%zu) crlfTab %d(%zu)->%d(%zu)",
                                space, ch, i,
                                (int)(s.provider_name.size() != beforeP.len),
                                beforeP.len, afterP.len,
                                (int)beforeP.hasNul, (int)afterP.hasNul,
                                (int)beforeP.hasCtrl, beforeP.ctrlCount, (int)afterP.hasCtrl, afterP.ctrlCount,
                                (int)beforeP.hasCrLfTab, beforeP.crlfTabCount, (int)afterP.hasCrLfTab, afterP.crlfTabCount
                            );
                            DebugLogFormat(
                                L"[OBS][NORM] space=%d ch=%d #%zu service  changed=%d len %zu->%zu nul %d->%d ctrl %d(%zu)->%d(%zu) crlfTab %d(%zu)->%d(%zu)",
                                space, ch, i,
                                (int)(s.service_name.size() != beforeN.len),
                                beforeN.len, afterN.len,
                                (int)beforeN.hasNul, (int)afterN.hasNul,
                                (int)beforeN.hasCtrl, beforeN.ctrlCount, (int)afterN.hasCtrl, afterN.ctrlCount,
                                (int)beforeN.hasCrLfTab, beforeN.crlfTabCount, (int)afterN.hasCrLfTab, afterN.crlfTabCount
                            );
                        }
                    }

                    DebugLogFormat(L"[OBS][D] Snapshot build start: servicesSafe=%zu", servicesSafe.size());
                    std::vector<GuiSnapshot::ServiceRow> sdtSnapshot =
                        GuiSnapshot::MakeSnapshot(servicesSafe, false);
                    GuiSnapshot::SortBySectionThenSid(sdtSnapshot);
                    DebugLogFormat(L"[OBS][D] Snapshot build done: rows=%zu", sdtSnapshot.size());

                    if (!sdtSnapshot.empty()) {
                        const auto& x = sdtSnapshot[0];
                        const std::wstring provEsc = EscapeForLog(x.provider_name);
                        const std::wstring nameEsc = EscapeForLog(x.service_name);
                        DebugLogFormat(L"[OBS][D] snap[0] sid=%u provEsc='%ls' nameEsc='%ls' provHex=%ls nameHex=%ls",
                            (unsigned)x.service_id,
                            provEsc.c_str(),
                            nameEsc.c_str(),
                            ToHexList(x.provider_name).c_str(),
                            ToHexList(x.service_name).c_str()
                        );
                    }

                    if (opt.onSdtSnapshot) {
                        opt.onSdtSnapshot(space, ch, sdtSnapshot);
                    }
                    else {
                        DebugLog(L"[OBS][E] onSdtSnapshot is null (skip)");
                    }

                    DebugLogFormat(L"[RESULT][CAP] pushOk=%zu pushFail=%zu", writeOk, writeFail);
                    DebugLogFormat(L"[RESULT][FM] processed=%zu", filterManager.GetProcessedPacketCount());
                    DebugLogFormat(L"[RESULT][TS] GetTsStreamFail=%zu", TSStreamUtils::GetTsStreamFailureCount());
                    DebugLogFormat(L"[RESULT][CNT] PAT=%zu PMT=%zu SDT=%zu NIT=%zu",
                        filterManager.GetPATCount(),
                        filterManager.GetPMTCount(),
                        filterManager.GetSDTCount(),
                        filterManager.GetNITCount());

                    DebugLogFormat(L"[OBS] servicesSafe=%zu (raw=%zu)", servicesSafe.size(), servicesRaw.size());
                    LogServiceListPreview(space, ch, servicesSafe, 12);

                    std::wstring basePrefix = L"FilterTest_space" + std::to_wstring(space) + L"_ch" + std::to_wstring(ch);

                    if (snap.enableSdtDebug) {
                        PacketDumpManager::DumpHexPackets(filterManager.GetSDTPacketsBefore(), basePrefix + L"_SDT_Before.txt");
                        PacketDumpManager::FlushDumpBuffer();
                    }

                    if (enableTsDump) {
                        TSStreamUtils::DumpAllCapturedPackets();
                    }
                    if (enableRingDump) {
                        PacketDumpManager::DumpHexPackets(runner.GetExtractedPackets(), basePrefix + L"_RingBuffer.txt");
                        PacketDumpManager::FlushDumpBuffer();
                    }

                    TSStreamUtils::ClearCapturedPackets();

                    if (snap.enableSdtDebug) {
                        const std::filesystem::path outPath = std::filesystem::path(basePrefix + L"_SDT.txt");
                        Utf8BomWriter ofs(outPath, false);
                        if (!ofs.IsOpen()) {
                            DebugLogFormat(L"[ERROR] SDT output open failed: %ls", outPath.wstring().c_str());
                        }
                        else {
                            for (const auto& s : servicesSafe) {
                                std::wstringstream line;
                                line
                                    << L"ServiceID: " << s.service_id
                                    << L", TSID: " << s.transport_stream_id
                                    << L", ONID: " << s.original_network_id
                                    << L", Type: " << static_cast<int>(s.service_type)
                                    << L", FreeCA: " << (s.free_CA_mode ? 1 : 0)
                                    << L", EITsch: " << (s.eit_schedule_flag ? 1 : 0)
                                    << L", EITpf: " << (s.eit_present_following_flag ? 1 : 0)
                                    << L", Run: " << static_cast<int>(s.running_status)
                                    << L", Section: " << static_cast<int>(s.section_number)
                                    << L", Provider: " << s.provider_name
                                    << L", Name: " << s.service_name;

                                ofs.WriteLine(line.str());
                            }
                        }
                    }

                    if (snap.enableNitDebug) {
                        const auto& nets = filterManager.GetNetworkList();
                        const std::filesystem::path outPath = std::filesystem::path(basePrefix + L"_NIT.txt");
                        Utf8BomWriter ofs(outPath, false);
                        if (!ofs.IsOpen()) {
                            DebugLogFormat(L"[ERROR] NIT output open failed: %ls", outPath.wstring().c_str());
                        }
                        else {
                            DebugLogFormat(L"[OBS][NIT] networks=%zu", nets.size());

                            for (const auto& net : nets) {
                                aribUnit.ResetDecoder();

                                std::vector<uint8_t> bytes;
                                bytes.reserve(net.networkName.size());
                                for (auto c : net.networkName) {
                                    bytes.push_back((uint8_t)c);
                                }

                                const std::wstring nameW = bytes.empty()
                                    ? std::wstring()
                                    : aribUnit.DecodeToWStringForWin32(bytes, snap.aribTraceEnable);

                                std::wstringstream line;
                                line << L"NetworkID: " << net.networkId << L", Name: " << nameW;
                                ofs.WriteLine(line.str());
                            }
                        }
                    }

                    {
                        const auto& networks = filterManager.GetNetworkList();
                        auto entries = BuildChannelListEntries(servicesSafe, networks, space, ch);

                        const std::wstring channelListFile = GetChannelListFileName(space);
                        AppendChannelListFile(channelListFile, entries);
                    }

                    auto stats = TSStreamUtils::GetTsStreamStats();
                    DebugLogFormat(L"[TSStats] space=%d, ch=%d: call=%zu, succ=%zu, fail=%zu, packets=%zu, failRate=%.3f, avgPktPerSucc=%.1f",
                        space, ch,
                        stats.callCount,
                        stats.successCount,
                        stats.failureCount,
                        stats.packetCount,
                        stats.GetFailureRate(),
                        stats.GetAvgPacketsPerSuccess()
                    );

                    {
                        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - scanStart);
                        std::wstring msg = L"Space " + std::to_wstring(space) + L" / CH " + std::to_wstring(ch);
                        msg += L"  DONE  ";
                        msg += FormatHMS(elapsed);
                        EmitStatus(opt, msg);
                    }
                }
                catch (...) {
                    DebugLogFormat(L"[ERROR] channel_scan_GUI: channel scope exception space=%d ch=%d", space, ch);
                    EmitStatus(opt, L"ChannelScan: ERROR (channel exception)");
                    abortedByError = true;
                    break;
                }
            }

            if (canceled || abortedByError) {
                break;
            }

            DebugLogFormat(L"[SPACE] end space=%d", space);
        }
    }
    catch (...) {
        DebugLog(L"[ERROR] channel_scan_GUI: unhandled exception");
        abortedByError = true;
    }

    if (abortedByError) {
        finalizeScan(L"ChannelScan: ERROR", L"[ERROR] チャンネルスキャン（GUI）は例外で終了しました。");
        return;
    }

    if (canceled) {
        finalizeScan(L"ChannelScan: CANCELED", L"[INFO] チャンネルスキャン（GUI）は停止要求で終了しました。");
        return;
    }

    finalizeScan(L"ChannelScan: FINISHED", L"[INFO] 全チャンネルのTSストリーム解析が完了しました。（GUI）");
}
