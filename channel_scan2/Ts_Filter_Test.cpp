#include <Windows.h>
#undef max
#undef min
#include <iostream>
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

#include "Ts_Filter_Test.hpp"
#include "ChannelListLoader.hpp"
#include "TSParser.h"
#include "FilterManager.hpp"
#include "ts_receiver.hpp"
#include "PacketCounter.h"
#include "MPMCRingBuffer.hpp"
#include "DebugManager.h"
#include "BonDriverHelper.hpp" 
#include "TSStreamUtils.hpp"
#include "TSBufferConstants.hpp"
#include "AribDecoder.hpp"
#include "TSBufferManager.h"
#include "AnalyzePID.hpp"
#include "TsStreamTypes.hpp"
#include "FilterManagerRunner.hpp"
#include "ConfigManager.h"
#include "PacketDumpManager.hpp"
#include "channellist.hpp"
#include "AribDecodecallUnit.hpp"
#include "EITEventSortUnit.hpp"
#include "EITFilter.hpp"
#include "EpgBuilder.hpp"
#include "EpgModel.hpp"
#include "EpgHtmlWriter.hpp"
#include "EncodingConfig.hpp"
#include "PhysicalChannelListBuilder.hpp"
#include "EitTextWriter.hpp"
#include "EITTextDumpLoader.hpp"
#include "EITHtmlProgramTable.hpp"
#include "EpgIdSortUnit.hpp"  
#include "EitWithCoverage.hpp"
#include "EitCoverage.hpp"
#include "TsPacketCapture.hpp"
#include "EitEventUpdateUnit.hpp"
#include "TOTFilter.hpp" 
#include "CDTFilter.hpp"


// EPGバックグラウンド更新機能の有効化フラグ
#ifndef ENABLE_EPG_BACKGROUND_UPDATE_FEATURE
// 今は構築テストだけなので 0（完全封印）
#define ENABLE_EPG_BACKGROUND_UPDATE_FEATURE 0
#endif





extern EncodingConfig g_EncodingConfig;
using namespace TSStreamUtils;
std::vector<TsPacket> tsCapturedPackets;






// フィルター処理テストの実行
void RunFilterProcessingTest(IBonDriver2* bon, FilterManagerMode mode)
{

    // CUI でメニューで ON にしてたやつをまず固定で再現
    g_EnableSDTDebug.store(true);
    g_EnableNITDebug.store(true);
    //g_EnableEITDebug.store(true);
    //g_EnableAribCharTrace.store(true);

    // GUIでも DebugManager のモード設定を反映
    DebugManager::GetInstance().ConfigureDebugMode();

    if (!bon) {
        DebugLog(L"[ERROR] BonDriver が無効です。");
        return;
    }

    auto AskYesNo = [](std::wstring_view prompt) -> bool {
        while (true) {
            std::wcout << prompt;
            wchar_t resp = 0;
            if (!(std::wcin >> resp)) {
                std::wcin.clear();
                return false;
            }
            switch (resp) {
            case L'y': case L'Y': return true;
            case L'n': case L'N': return false;
            default:
                std::wcout << L"[WARN] y か n で入力してください。\n";
                break;
            }
        }
        };

    const int space = 0;

    // --- channellist 初期化 ---
    {
        const std::wstring channelListFile = GetChannelListFileName(space);
        InitializeChannelListFile(channelListFile);
    }

    const bool enableTsDump = AskYesNo(L"[INPUT] TSストリームの保存を行いますか？ (y/n): ");
    const bool enableRingDump = AskYesNo(L"[INPUT] リングバッファ内のTSストリームの保存を行いますか？ (y/n): ");

    DebugLog(L"[INFO] 地上波チャンネル全体を対象にTSストリーム解析を開始します。");

    // 受信時間（秒）を 1 箇所で管理
    const int captureSec = 18;

    for (int ch = 0;; ++ch)
    {
        LPCTSTR chName = bon->EnumChannelName(space, ch);
        if (!chName) break;

        TSStreamUtils::ResetTsStreamStats();

        DebugLogFormat(L"[BEGIN] FilterTest: space=%d, ch=%d (%ls)", space, ch, chName);

        if (!bon->SetChannel(space, ch)) {
            DebugLogFormat(L"[WARN] SetChannel(space=%d, ch=%d) に失敗", space, ch);
            continue;
        }

        if (!TSStreamUtils::WaitForSignalLock(bon)) {
            DebugLogFormat(L"[WARN] 信号ロックに失敗: space=%d, ch=%d", space, ch);
            continue;
        }

        const float level = bon->GetSignalLevel();
        DebugLogFormat(L"[INFO] 受信開始: space=%d, ch=%d, 信号レベル=%.1f dB", space, ch, level);

        TSParser parser;
        FilterManager filterManager(&parser, nullptr);
        //filterManager.Initialize();
        filterManager.ApplyMode(mode);

        // ★ 少し大きめ推奨（満杯で取りこぼすとテストがブレる）
        constexpr size_t bufferSize = 8192;
        MPMCRingBuffer<TsPacket> ringBuffer(bufferSize);

        // 先に consumer を起動してから producer
        FilterManagerRunner runner(&filterManager, &ringBuffer);
        runner.Start();

        // -------------------------
        // producer を TsPacketCapture に置換
        // -------------------------
        TsPacketCaptureOptions capOpt;
        capOpt.captureSec = captureSec;
        capOpt.packetReserve = 4096;
        capOpt.sleepOnFailMs = 1;
        capOpt.receiveFn = &ReceiveTsStreamBlock; // ★注入（このTUでは ts_receiver.hpp で見えている想定）

        std::atomic_bool cancelRequested{ false }; // GUI中断などで外から true にできる（今は未使用）
        TsPacketCaptureSession capture;

        if (!capture.Start(bon, &ringBuffer, capOpt, &cancelRequested)) {
            DebugLogFormat(L"[ERROR] TsPacketCaptureSession.Start() に失敗: space=%d, ch=%d", space, ch);
            runner.Stop();
            continue;
        }

        // captureSec 経過で停止（元コードと同じ流れ）
        std::this_thread::sleep_for(std::chrono::seconds(captureSec));
        capture.StopAndJoin();

        const auto capCounts = capture.GetCounts();
        const std::size_t writeCount = capCounts.pushOk;
        const std::size_t writeFailCount = capCounts.pushFail;

        // runner が処理し切るのを待つ（最大 2 秒 / 0.25 秒アイドルで収束扱い）
        {
            using namespace std::chrono;
            const auto overall0 = steady_clock::now();
            auto idle0 = steady_clock::now();
            std::size_t last = filterManager.GetProcessedPacketCount();

            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));

                const std::size_t cur = filterManager.GetProcessedPacketCount();
                if (cur != last) {
                    last = cur;
                    idle0 = steady_clock::now();
                }

                if (steady_clock::now() - idle0 >= milliseconds(250)) break;
                if (steady_clock::now() - overall0 >= seconds(2)) break;
            }
        }

        runner.Stop();

        // 結果出力
        DebugLogFormat(L"[DEBUG] [RESULT] 書き込み成功：%zu 件", writeCount);
        DebugLogFormat(L"[DEBUG] [RESULT] 書き込み失敗（バッファ満）：%zu 件", writeFailCount);
        DebugLogFormat(L"[DEBUG] [RESULT] 読み出し成功（FilterManager処理）：%zu 件", filterManager.GetProcessedPacketCount());
        DebugLogFormat(L"[DEBUG] [RESULT] GetTsStream失敗回数：%zu", TSStreamUtils::GetTsStreamFailureCount());

        DebugLogFormat(L"[RESULT] PAT受信数：%zu", filterManager.GetPATCount());
        DebugLogFormat(L"[RESULT] PMT受信数：%zu", filterManager.GetPMTCount());
        DebugLogFormat(L"[RESULT] SDT受信数：%zu", filterManager.GetSDTCount());
        DebugLogFormat(L"[RESULT] NIT受信数：%zu", filterManager.GetNITCount());

        std::wstring basePrefix = L"FilterTest_space" + std::to_wstring(space) + L"_ch" + std::to_wstring(ch);

        // SDT パケットダンプ（チャンネルごとにファイル分離）
        PacketDumpManager::DumpHexPackets(filterManager.GetSDTPacketsBefore(), basePrefix + L"_SDT_Before.txt");
        PacketDumpManager::FlushDumpBuffer();

        // TS / リングダンプ
        if (enableTsDump) {
            TSStreamUtils::DumpAllCapturedPackets();
        }
        if (enableRingDump) {
            PacketDumpManager::DumpHexPackets(runner.GetExtractedPackets(), basePrefix + L"_RingBuffer.txt");
            PacketDumpManager::FlushDumpBuffer();
        }
        TSStreamUtils::ClearCapturedPackets();

        // SDT 出力
        {
            const auto& list = filterManager.GetServiceList();
            std::wofstream ofs(basePrefix + L"_SDT.txt");
            for (const auto& s : list) {
                ofs
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
                    << L", Name: " << s.service_name
                    << std::endl;
            }
        }

        // NIT 出力（remote_control_key_id / service_list も出す）
        {
            const auto& nets = filterManager.GetNetworkList();
            std::wofstream ofs(basePrefix + L"_NIT.txt");

            for (const auto& net : nets) {
                std::wstring nameW;
                for (auto c : net.networkName) nameW += static_cast<wchar_t>(c);

                ofs << L"NetworkID: " << net.networkId << L", Name: " << nameW << std::endl;

                if (!net.transportStreams.empty()) {
                    ofs << L"  TransportStreams:" << std::endl;
                    for (const auto& ts : net.transportStreams) {
                        ofs << L"    TSID=" << ts.transportStreamId
                            << L", ONID=" << ts.originalNetworkId;

                        if (ts.remoteControlKeyId >= 0) {
                            ofs << L", RemoteKeyID=" << ts.remoteControlKeyId;
                        }

                        if (!ts.services.empty()) {
                            ofs << L", Services=" << ts.services.size() << L" [";
                            const std::size_t show = (std::min<std::size_t>)(ts.services.size(), 16);
                            for (std::size_t i = 0; i < show; ++i) {
                                ofs << (i ? L"," : L"")
                                    << ts.services[i].serviceId
                                    << L":" << static_cast<int>(ts.services[i].serviceType);
                            }
                            if (ts.services.size() > show) ofs << L",...";
                            ofs << L"]";
                        }

                        ofs << std::endl;
                    }
                }

                ofs << std::endl;
            }
        }

        // --- 統合チャンネルリスト追記 ---
        {
            const auto& services = filterManager.GetServiceList();
            const auto& networks = filterManager.GetNetworkList();

            auto entries = BuildChannelListEntries(services, networks, space, ch);

            const std::wstring channelListFile = GetChannelListFileName(space);
            AppendChannelListFile(channelListFile, entries);
        }

        DebugLogFormat(L"[RESULT] space=%d, ch=%d → PAT: %zu, PMT: %zu, SDT: %zu, NIT: %zu",
            space, ch,
            filterManager.GetPATCount(),
            filterManager.GetPMTCount(),
            filterManager.GetSDTCount(),
            filterManager.GetNITCount());

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
    }

    DebugLog(L"[INFO] 全チャンネルのTSストリーム解析が完了しました。");
    FlushDebugLog();
}




// EPGデータ取得テスト
void RunEpgDataTest(IBonDriver2* bon, FilterManagerMode mode)
{
    if (!bon) {
        DebugLog(L"[ERROR] BonDriver が無効です。");
        return;
    }

    auto SanitizeForLog = [](std::wstring_view src) -> std::wstring {
        std::wstring out;
        constexpr std::size_t kMaxLen = 512;
        out.reserve((std::min)(src.size(), kMaxLen));

        for (wchar_t ch : src) {
            if (out.size() >= kMaxLen) {
                out.push_back(L'…');
                break;
            }
            if (ch < 0x20) {
                if (ch == L'\n' || ch == L'\r' || ch == L'\t') out.push_back(ch);
                else out.push_back(L'?');
                continue;
            }
            if (ch >= 0xD800 && ch <= 0xDFFF) {
                out.push_back(ch);
                continue;
            }
            out.push_back(ch);
        }
        return out;
        };

    auto AskYesNo = [](std::wstring_view prompt) -> bool {
        while (true) {
            std::wcout << prompt;
            wchar_t resp = 0;
            if (!(std::wcin >> resp)) {
                std::wcin.clear();
                return false;
            }
            switch (resp) {
            case L'y': case L'Y': return true;
            case L'n': case L'N': return false;
            default:
                std::wcout << L"[WARN] y か n で入力してください。" << std::endl;
                break;
            }
        }
        };

    struct EpgTestOptions {
        bool enableTsDump = false;
        bool enableRingDump = false;
        bool enableEitDump = true;
        bool enableAribTrace = false;
    } opts;

    {
        bool sdtDbg = AskYesNo(L"[INPUT] SDTFilter の詳細デバッグログを有効にしますか？ (y/n): ");
        g_EnableSDTDebug.store(sdtDbg, std::memory_order_relaxed);

        bool nitDbg = AskYesNo(L"[INPUT] NITFilter の詳細デバッグログを有効にしますか？ (y/n): ");
        g_EnableNITDebug.store(nitDbg, std::memory_order_relaxed);
    }

    {
        opts.enableTsDump = AskYesNo(L"[INPUT] TSストリームの保存を行いますか？ (y/n): ");
        opts.enableRingDump = AskYesNo(L"[INPUT] リングバッファ内のTSストリームの保存を行いますか？ (y/n): ");
        opts.enableEitDump = AskYesNo(L"[INPUT] EIT(番組情報)をテキストファイルに出力しますか？ (y/n): ");
        opts.enableAribTrace = AskYesNo(L"[INPUT] EIT ARIB デコード詳細トレースを有効にしますか？ (y/n): ");
    }

    const int space = 0;
    DebugLog(L"[INFO] 地上波チャンネル全体を対象に EIT(番組情報) 取得テストを開始します。");

    const std::filesystem::path eitCountersTsv =
        std::filesystem::path(L"EpgTest_EITCounters_AllChannels.tsv");

    // ---- duration 取得（メンバ有無両対応）----
    auto GetDurationSec = [](const auto& e) -> uint32_t {
        auto BcdToUint = [](uint8_t b) -> uint32_t {
            return static_cast<uint32_t>(((b >> 4) & 0x0F) * 10 + (b & 0x0F));
            };

        uint32_t durSec = 0;

        if constexpr (requires { e.duration; }) {
            durSec = static_cast<uint32_t>(e.duration);
        }
        if (durSec == 0) {
            const uint32_t bcd = static_cast<uint32_t>(e.duration_hhmmss);
            const uint8_t bH = static_cast<uint8_t>((bcd >> 16) & 0xFF);
            const uint8_t bM = static_cast<uint8_t>((bcd >> 8) & 0xFF);
            const uint8_t bS = static_cast<uint8_t>((bcd >> 0) & 0xFF);

            const uint32_t dh = BcdToUint(bH);
            const uint32_t dm = BcdToUint(bM);
            const uint32_t ds = BcdToUint(bS);

            if (dh <= 23U && dm <= 59U && ds <= 59U) {
                durSec = dh * 3600U + dm * 60U + ds;
            }
        }
        return durSec;
        };

    auto ProcessOneChannel = [&](int ch, LPCTSTR chName) {

        std::wstring chNameSafe = chName ? SanitizeForLog(chName) : L"(null)";

        TSStreamUtils::ResetTsStreamStats();
        TSValidator::ResetStats();

        DebugLogFormat(L"[INFO] 番組取得対象チャンネル候補: space=%d, ch=%d (%ls)",
            space, ch, chNameSafe.c_str());

        if (!bon->SetChannel(space, ch)) {
            DebugLogFormat(L"[WARN] SetChannel(space=%d, ch=%d) に失敗しました。", space, ch);
            return;
        }

        if (!TSStreamUtils::WaitForSignalLock(bon)) {
            DebugLogFormat(L"[WARN] 信号ロックに失敗しました: space=%d, ch=%d", space, ch);
            return;
        }

        float level = bon->GetSignalLevel();
        DebugLogFormat(L"[INFO] 受信開始: space=%d, ch=%d, 信号レベル=%.1f dB",
            space, ch, level);

        TSParser parser;
      
        FilterManager filterManager(&parser, nullptr);
        //filterManager.Initialize();
        filterManager.ApplyMode(mode);

        // =========================================================
        // EIT 取得（EIT-RETRY：テスト側で回す / ::eit_capture は使わない）
        // =========================================================
        struct EitRetryConfig {
            int    captureSecondsPerCycle = 60 * 3;
            int    maxCycles = 1*2;

            double coverageThreshold = 0.85;

            bool   stopWhenFirst2DaysFull = true;
            double first2DaysThreshold = 0.95;

            double plateauEps = 0.003;
            int    plateauNeed = 2;
            std::size_t plateauMinAddedUnique = 50;

            std::size_t ringBufferSize = 8192 * 2;

            int pushRetryCount = 8;
            int pushRetrySleepUs = 100;

            bool   enableSchedule50Eval = true;

            bool   enableSectionGate = true;
            double sectionGateThreshold = 0.50;

            bool   stopWhenSchedule50Enough = false;
            double schedule50SectionThreshold = 0.95;
        } eitCfg;

        int     usedCycles = 0;
        double  finalCoverageAverage = 0.0;
        bool    finishedByCoverage = false;
        std::size_t writeCountTotal = 0;
        std::size_t writeFailCountTotal = 0;

        // =========================================================
        // coverage計算：base を JST epoch minutes に揃える
        // =========================================================
        constexpr int64_t kJstOffsetMinutes = 9LL * 60LL; // JST=UTC+9
        const int64_t baseEpochMinutesUtc = []() -> int64_t {
            using namespace std::chrono;
            return duration_cast<minutes>(system_clock::now().time_since_epoch()).count();
            }();
        int64_t baseEpochMinutesJst = baseEpochMinutesUtc + kJstOffsetMinutes;
        baseEpochMinutesJst -= (baseEpochMinutesJst % static_cast<int64_t>(EitCoverage::kSlotMinutes));

        auto IsInvalidStartTime = [](uint64_t st) noexcept -> bool {
            return (st == 0ULL) || (st == 0xFFFFFFFFFFULL);
            };

        double prevScore = -1.0;
        std::size_t prevUnique = 0;
        int plateauCount = 0;

        for (int cycle = 1; cycle <= eitCfg.maxCycles; ++cycle)
        {
            usedCycles = cycle;

            const std::size_t beforeEvents = filterManager.GetEITEvents().size();

            MPMCRingBuffer<TsPacket> ringBuffer(eitCfg.ringBufferSize);
            std::atomic<std::size_t> writeCount(0), writeFail(0);

            FilterManagerRunner runner(&filterManager, &ringBuffer);
            runner.Start();

            std::thread producer([&]() {
                std::vector<TsPacket> packets;
                packets.reserve(4096);

                const ULONGLONG start = GetTickCount64();

                while ((GetTickCount64() - start) < static_cast<ULONGLONG>(eitCfg.captureSecondsPerCycle) * 1000ULL)
                {
                    packets.clear();

                    if (ReceiveTsStreamBlock(bon, packets)) {
                        for (const auto& pkt : packets) {
                            bool pushed = false;
                            for (int t = 0; t < eitCfg.pushRetryCount; ++t) {
                                if (ringBuffer.Push(pkt)) { pushed = true; break; }
                                std::this_thread::sleep_for(std::chrono::microseconds(eitCfg.pushRetrySleepUs));
                            }
                            if (pushed) writeCount.fetch_add(1, std::memory_order_relaxed);
                            else        writeFail.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
                });

            producer.join();

            // runner が処理し切るのを待つ（最大 2 秒 / 0.25 秒アイドルで収束扱い）
            {
                using namespace std::chrono;
                const auto overall0 = steady_clock::now();
                auto idle0 = steady_clock::now();

                std::size_t last = filterManager.GetProcessedPacketCount();

                for (;;) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));

                    const std::size_t cur = filterManager.GetProcessedPacketCount();
                    if (cur != last) {
                        last = cur;
                        idle0 = steady_clock::now();
                    }

                    if (steady_clock::now() - idle0 >= milliseconds(250)) break;
                    if (steady_clock::now() - overall0 >= seconds(2)) break;
                }
            }

            runner.Stop();

            writeCountTotal += writeCount.load(std::memory_order_relaxed);
            writeFailCountTotal += writeFail.load(std::memory_order_relaxed);

            // --- coverage 算出（ユニーク化したイベントを使う） ---
            EitCoverage cov;
            cov.Reset();

            const auto& rawEvents = filterManager.GetEITEvents();

            std::size_t rawHasName = 0;
            for (const auto& e : rawEvents) if (!e.short_event_name_bytes.empty()) ++rawHasName;
            DebugLogFormat(L"[CHECK] rawEvents=%zu rawHasName=%zu (%.1f%%)",
                rawEvents.size(), rawHasName,
                rawEvents.empty() ? 0.0 : (rawHasName * 100.0 / rawEvents.size()));

            auto uniqueEvents = EITEventSortUnit::BuildUniqueSortedEvents(rawEvents);

            // =========================================================
            // 0x50/0x51 評価（あなたの既存ロジックは維持）
            // =========================================================
            struct EventKeyStrict {
                uint16_t onid = 0, tsid = 0, sid = 0, eid = 0;
                uint64_t start_time_raw = 0;
                bool operator==(const EventKeyStrict& o) const noexcept {
                    return onid == o.onid && tsid == o.tsid && sid == o.sid && eid == o.eid && start_time_raw == o.start_time_raw;
                }
            };
            struct EventKeyStrictHash {
                std::size_t operator()(const EventKeyStrict& k) const noexcept {
                    uint64_t x = 0;
                    x ^= (uint64_t)k.onid << 48;
                    x ^= (uint64_t)k.tsid << 32;
                    x ^= (uint64_t)k.sid << 16;
                    x ^= (uint64_t)k.eid;
                    x ^= (k.start_time_raw * 0x9e3779b97f4a7c15ULL);
                    x ^= (x >> 33);
                    x *= 0xff51afd7ed558ccdULL;
                    x ^= (x >> 33);
                    return (std::size_t)x;
                }
            };

            struct EventKeyLoose {
                uint16_t onid = 0, tsid = 0, sid = 0, eid = 0;
                bool operator==(const EventKeyLoose& o) const noexcept {
                    return onid == o.onid && tsid == o.tsid && sid == o.sid && eid == o.eid;
                }
            };
            struct EventKeyLooseHash {
                std::size_t operator()(const EventKeyLoose& k) const noexcept {
                    uint64_t x = 0;
                    x ^= (uint64_t)k.onid << 32;
                    x ^= (uint64_t)k.tsid << 16;
                    x ^= (uint64_t)k.sid;
                    x ^= (uint64_t)k.eid << 1;
                    x ^= (x >> 33);
                    x *= 0xc4ceb9fe1a85ec53ULL;
                    x ^= (x >> 33);
                    return (std::size_t)x;
                }
            };

            struct RawInfo {
                uint8_t seenMask = 0;
                uint8_t nameMask = 0;
            };
            constexpr uint8_t kSeen50 = 0x01;
            constexpr uint8_t kSeen51 = 0x02;

            std::unordered_map<EventKeyStrict, RawInfo, EventKeyStrictHash> rawInfoStrict;
            std::unordered_map<EventKeyLoose, RawInfo, EventKeyLooseHash>  rawInfoLoose;
            rawInfoStrict.reserve((std::max<std::size_t>)(rawEvents.size() / 3, 128));
            rawInfoLoose.reserve((std::max<std::size_t>)(rawEvents.size() / 3, 128));

            struct SvcKey {
                uint16_t onid = 0, tsid = 0, sid = 0;
                bool operator==(const SvcKey& o) const noexcept { return onid == o.onid && tsid == o.tsid && sid == o.sid; }
            };
            struct SvcKeyHash {
                std::size_t operator()(const SvcKey& k) const noexcept {
                    uint64_t x = 0;
                    x ^= (uint64_t)k.onid << 32;
                    x ^= (uint64_t)k.tsid << 16;
                    x ^= (uint64_t)k.sid;
                    x ^= (x >> 33);
                    x *= 0xc4ceb9fe1a85ec53ULL;
                    x ^= (x >> 33);
                    return (std::size_t)x;
                }
            };

            std::unordered_set<SvcKey, SvcKeyHash> svcSeen50;
            std::unordered_set<SvcKey, SvcKeyHash> svcSeen51;
            svcSeen50.reserve(64);
            svcSeen51.reserve(64);

            std::size_t raw50 = 0, raw51 = 0, raw50Name = 0, raw51Name = 0;

            for (const auto& e : rawEvents) {
                const uint16_t onid = e.original_network_id;
                const uint16_t tsid = e.transport_stream_id;
                const uint16_t sid = e.service_id;
                const uint16_t eid = e.event_id;
                if (onid == 0 || tsid == 0 || sid == 0) continue;

                const bool hasName = !e.short_event_name_bytes.empty();

                const EventKeyStrict ks{ onid, tsid, sid, eid, e.start_time };
                const EventKeyLoose  kl{ onid, tsid, sid, eid };

                if (e.table_id == 0x50) {
                    ++raw50;
                    if (hasName) ++raw50Name;
                    svcSeen50.emplace(SvcKey{ onid, tsid, sid });

                    auto& s = rawInfoStrict[ks];
                    s.seenMask |= kSeen50;
                    if (hasName) s.nameMask |= kSeen50;

                    auto& l = rawInfoLoose[kl];
                    l.seenMask |= kSeen50;
                    if (hasName) l.nameMask |= kSeen50;
                }
                else if (e.table_id == 0x51) {
                    ++raw51;
                    if (hasName) ++raw51Name;
                    svcSeen51.emplace(SvcKey{ onid, tsid, sid });

                    auto& s = rawInfoStrict[ks];
                    s.seenMask |= kSeen51;
                    if (hasName) s.nameMask |= kSeen51;

                    auto& l = rawInfoLoose[kl];
                    l.seenMask |= kSeen51;
                    if (hasName) l.nameMask |= kSeen51;
                }
            }

            std::size_t uniqKey50 = 0, uniqKey51 = 0, uniqKey50Name = 0, uniqKey51Name = 0;
            std::size_t lostName50 = 0, lostName51 = 0;
            std::size_t unmatchedKeys = 0;

            auto LookupRawInfo = [&](const ParsedEITPacket& u) -> const RawInfo* {
                const EventKeyStrict ks{ u.original_network_id, u.transport_stream_id, u.service_id, u.event_id, u.start_time };
                auto itS = rawInfoStrict.find(ks);
                if (itS != rawInfoStrict.end()) return &itS->second;

                const EventKeyLoose kl{ u.original_network_id, u.transport_stream_id, u.service_id, u.event_id };
                auto itL = rawInfoLoose.find(kl);
                if (itL != rawInfoLoose.end()) return &itL->second;

                return nullptr;
                };

            for (const auto& u : uniqueEvents) {
                const RawInfo* info = LookupRawInfo(u);
                if (!info) { ++unmatchedKeys; continue; }

                const bool mergedHasName = !u.short_event_name_bytes.empty();

                if (info->seenMask & kSeen50) {
                    ++uniqKey50;
                    if (mergedHasName) ++uniqKey50Name;
                    if ((info->nameMask & kSeen50) && !mergedHasName) ++lostName50;
                }
                if (info->seenMask & kSeen51) {
                    ++uniqKey51;
                    if (mergedHasName) ++uniqKey51Name;
                    if ((info->nameMask & kSeen51) && !mergedHasName) ++lostName51;
                }
            }

            auto Pct = [](std::size_t a, std::size_t b) -> double {
                return (b == 0) ? 0.0 : (static_cast<double>(a) * 100.0 / static_cast<double>(b));
                };

            // =========================================================
            // 0x50/0x51 セクションカバレッジ（EITFilter側の指標）
            // =========================================================
            double cov50Sum = 0.0, cov51Sum = 0.0;
            double cov50Min = 1.0, cov51Min = 1.0;
            std::size_t seg50 = 0, seg51 = 0;
            std::size_t seg50Complete = 0, seg51Complete = 0;

            if (eitCfg.enableSchedule50Eval) {
                if (const EITFilter* eit = filterManager.GetEITFilterForDebug()) {
                    for (const auto& s : svcSeen50) {
                        const double r = eit->GetSectionCoverageRatio(s.onid, s.tsid, s.sid, 0x50);
                        cov50Sum += r;
                        cov50Min = (std::min)(cov50Min, r);
                        ++seg50;
                        if (eit->IsEpgSegmentComplete(s.onid, s.tsid, s.sid, 0x50, eitCfg.schedule50SectionThreshold)) {
                            ++seg50Complete;
                        }
                    }
                    for (const auto& s : svcSeen51) {
                        const double r = eit->GetSectionCoverageRatio(s.onid, s.tsid, s.sid, 0x51);
                        cov51Sum += r;
                        cov51Min = (std::min)(cov51Min, r);
                        ++seg51;
                        if (eit->IsEpgSegmentComplete(s.onid, s.tsid, s.sid, 0x51, eitCfg.schedule50SectionThreshold)) {
                            ++seg51Complete;
                        }
                    }
                }
            }

            const double raw50NamePct = Pct(raw50Name, raw50);
            const double raw51NamePct = Pct(raw51Name, raw51);
            const double uniq50NamePct = Pct(uniqKey50Name, uniqKey50);
            const double uniq51NamePct = Pct(uniqKey51Name, uniqKey51);

            const double cov50Avg = (seg50 > 0) ? (cov50Sum / static_cast<double>(seg50)) : 0.0;
            const double cov51Avg = (seg51 > 0) ? (cov51Sum / static_cast<double>(seg51)) : 0.0;
            if (seg50 == 0) cov50Min = 0.0;
            if (seg51 == 0) cov51Min = 0.0;

            const double secAvgAll = ((seg50 + seg51) > 0)
                ? ((cov50Sum + cov51Sum) / static_cast<double>(seg50 + seg51))
                : 0.0;

            DebugLogFormat(
                L"[EIT-50EVAL] raw50=%zu(name=%zu %.1f%%) raw51=%zu(name=%zu %.1f%%) | "
                L"uniqByKey50=%zu(name=%zu %.1f%%) uniqByKey51=%zu(name=%zu %.1f%%) | "
                L"lostName50=%zu lostName51=%zu unmatched=%zu | "
                L"sec50(avg/min)=%.3f/%.3f complete=%zu/%zu | sec51(avg/min)=%.3f/%.3f complete=%zu/%zu | "
                L"svc50=%zu svc51=%zu secAvgAll=%.3f",
                raw50, raw50Name, raw50NamePct,
                raw51, raw51Name, raw51NamePct,
                uniqKey50, uniqKey50Name, uniq50NamePct,
                uniqKey51, uniqKey51Name, uniq51NamePct,
                lostName50, lostName51, unmatchedKeys,
                cov50Avg, cov50Min, seg50Complete, seg50,
                cov51Avg, cov51Min, seg51Complete, seg51,
                svcSeen50.size(), svcSeen51.size(),
                secAvgAll
            );

            if (eitCfg.stopWhenSchedule50Enough && (seg50 + seg51) > 0) {
                if (secAvgAll >= eitCfg.schedule50SectionThreshold) {
                    DebugLogFormat(L"[EIT-50EVAL] stopBy50: secAvgAll=%.3f >= %.3f", secAvgAll, eitCfg.schedule50SectionThreshold);
                    finishedByCoverage = true;
                    break;
                }
            }

            // =========================================================
            // coverage（イベント時間帯ベース）
            // =========================================================
            std::size_t converted = 0;
            std::size_t skippedInvalidTime = 0;

            for (const auto& e : uniqueEvents) {
                if (IsInvalidStartTime(e.start_time)) { ++skippedInvalidTime; continue; }

                const uint8_t b0 = static_cast<uint8_t>((e.start_time >> 32) & 0xFF);
                const uint8_t b1 = static_cast<uint8_t>((e.start_time >> 24) & 0xFF);
                const uint8_t b2 = static_cast<uint8_t>((e.start_time >> 16) & 0xFF);
                const uint8_t b3 = static_cast<uint8_t>((e.start_time >> 8) & 0xFF);
                const uint8_t b4 = static_cast<uint8_t>((e.start_time >> 0) & 0xFF);

                const uint16_t mjd = static_cast<uint16_t>((static_cast<uint16_t>(b0) << 8) | b1);

                const auto bcd2 = [](uint8_t v) -> int {
                    return static_cast<int>(((v >> 4) & 0x0F) * 10 + (v & 0x0F));
                    };

                const int hh = bcd2(b2);
                const int mm = bcd2(b3);
                const int ss = bcd2(b4);

                if (mjd == 0 || mjd == 0xFFFF ||
                    hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59) {
                    ++skippedInvalidTime;
                    continue;
                }

                const int64_t daysSinceEpoch = static_cast<int64_t>(mjd) - 40587LL;
                if (daysSinceEpoch < -40000LL || daysSinceEpoch > 60000LL) {
                    ++skippedInvalidTime;
                    continue;
                }

                // EIT start_time は UTC なので、JST に合わせるなら +9h が必要
                const int64_t startEpochMinutesUtc =
                    daysSinceEpoch * 1440LL
                    + static_cast<int64_t>(hh) * 60LL
                    + static_cast<int64_t>(mm);

                const int64_t startEpochMinutesJst = startEpochMinutesUtc + kJstOffsetMinutes;

                const int64_t startMinutesFromBase = startEpochMinutesJst - baseEpochMinutesJst;

                const uint32_t durSec = GetDurationSec(e);
                if (durSec == 0) { ++skippedInvalidTime; continue; }

                const int durationMinutes = static_cast<int>((durSec + 59U) / 60U);
                if (durationMinutes <= 0) { ++skippedInvalidTime; continue; }

                const int64_t endMinutes = startMinutesFromBase + static_cast<int64_t>(durationMinutes);
                if (endMinutes <= 0 || startMinutesFromBase >= static_cast<int64_t>(EitCoverage::kTotalMinutes)) {
                    continue;
                }

                cov.OnEventRange(static_cast<int>(startMinutesFromBase), durationMinutes);
                ++converted;
            }

            const auto sum = cov.GetSummary();

            finalCoverageAverage = (sum.coverageFirst2Days + sum.coverage2to8Days) * 0.5;
            const double score = sum.coverageFirst2Days * 0.7 + sum.coverage2to8Days * 0.3;

            const double gatedScore = (eitCfg.enableSectionGate && (seg50 + seg51) > 0)
                ? (score * 0.7 + secAvgAll * 0.3)
                : score;

            const std::size_t afterEvents = rawEvents.size();
            const long long deltaEvents =
                static_cast<long long>(afterEvents) - static_cast<long long>(beforeEvents);

            const std::size_t uniq = uniqueEvents.size();
            const long long deltaUniq =
                static_cast<long long>(uniq) - static_cast<long long>(prevUnique);

            if (converted == 0) {
                DebugLog(L"[EIT-RETRY][WARN] coverage算出に必要な start_time/duration が有効なイベントが見つからず、coverage=0相当になります。");
            }

            DebugLogFormat(
                L"[EIT-RETRY] space=%d, ch=%d, cycle=%d/%d: raw(before=%zu, after=%zu, %+lld), unique=%zu (%+lld), "
                L"cov(avg)=%.3f, score=%.3f, gatedScore=%.3f, secAvgAll=%.3f, first2Days=%.3f, 2to8Days=%.3f, first2DaysFull=%d, converted=%zu, skipped=%zu",
                space, ch, cycle, eitCfg.maxCycles,
                beforeEvents, afterEvents, deltaEvents,
                uniq, deltaUniq,
                finalCoverageAverage,
                score,
                gatedScore,
                secAvgAll,
                sum.coverageFirst2Days, sum.coverage2to8Days,
                sum.first2DaysFull ? 1 : 0,
                converted,
                skippedInvalidTime
            );

            const bool nearEnough = sum.first2DaysFull || (sum.coverageFirst2Days >= eitCfg.first2DaysThreshold);

            if (eitCfg.stopWhenFirst2DaysFull && nearEnough) {
                if (!eitCfg.enableSectionGate || (seg50 + seg51) == 0 || secAvgAll >= eitCfg.sectionGateThreshold) {
                    finishedByCoverage = true;
                    break;
                }
                else {
                    DebugLogFormat(L"[EIT-RETRY] gate(first2Days): first2DaysFull but secAvgAll=%.3f < %.3f (continue)",
                        secAvgAll, eitCfg.sectionGateThreshold);
                }
            }

            if (finalCoverageAverage >= eitCfg.coverageThreshold) {
                if (!eitCfg.enableSectionGate || (seg50 + seg51) == 0 || secAvgAll >= eitCfg.sectionGateThreshold) {
                    finishedByCoverage = true;
                    break;
                }
                else {
                    DebugLogFormat(L"[EIT-RETRY] gate: cov(avg)=%.3f >= %.3f but secAvgAll=%.3f < %.3f (continue)",
                        finalCoverageAverage, eitCfg.coverageThreshold, secAvgAll, eitCfg.sectionGateThreshold);
                }
            }

            const std::size_t addedUniq =
                (uniq >= prevUnique) ? (uniq - prevUnique) : 0;

            if (prevScore >= 0.0 &&
                std::abs(score - prevScore) < eitCfg.plateauEps &&
                addedUniq < eitCfg.plateauMinAddedUnique)
            {
                ++plateauCount;
            }
            else {
                plateauCount = 0;
            }

            if (plateauCount >= eitCfg.plateauNeed) {
                DebugLogFormat(
                    L"[EIT-RETRY] plateau stop: score=%.3f (prev=%.3f), addedUnique=%zu (<%zu), count=%d/%d",
                    score, prevScore, addedUniq, eitCfg.plateauMinAddedUnique,
                    plateauCount, eitCfg.plateauNeed
                );
                break;
            }

            prevScore = score;
            prevUnique = uniq;
        }

        DebugLogFormat(
            L"[EIT] capture summary: space=%d, ch=%d, cycles=%d, avgCoverage=%.3f, events=%zu, finishedByCoverage=%d",
            space, ch,
            usedCycles,
            finalCoverageAverage,
            filterManager.GetEITEvents().size(),
            finishedByCoverage ? 1 : 0
        );

        DebugLogFormat(L"[DEBUG] [RESULT] 書き込み成功：%zu 件", writeCountTotal);
        DebugLogFormat(L"[DEBUG] [RESULT] 書き込み失敗（バッファ満）：%zu 件", writeFailCountTotal);
        DebugLogFormat(L"[DEBUG] [RESULT] 読み出し成功（FilterManager処理）：%zu 件",
            filterManager.GetProcessedPacketCount());
        DebugLogFormat(L"[DEBUG] [RESULT] GetTsStream失敗回数：%zu",
            TSStreamUtils::GetTsStreamFailureCount());

        DebugLogFormat(L"[RESULT] PAT受信数：%zu", filterManager.GetPATCount());
        DebugLogFormat(L"[RESULT] PMT受信数：%zu", filterManager.GetPMTCount());
        DebugLogFormat(L"[RESULT] SDT受信数：%zu", filterManager.GetSDTCount());
        DebugLogFormat(L"[RESULT] NIT受信数：%zu", filterManager.GetNITCount());
        DebugLogFormat(L"[RESULT] EIT受信数：%zu", filterManager.GetEITCount());

        std::wstring basePrefix = L"EpgTest_space" + std::to_wstring(space) + L"_ch" + std::to_wstring(ch);

        if (opts.enableTsDump) {
            TSStreamUtils::DumpAllCapturedPackets();
        }

        if (opts.enableRingDump) {
            DebugLog(L"[WARN] RingBuffer ダンプは本テストでは未対応です（必要なら consumer 側でミラー保存を追加してください）。");
        }

        TSStreamUtils::ClearCapturedPackets();

        if (opts.enableEitDump) {
            const auto& rawEvents2 = filterManager.GetEITEvents();
            std::wstring outName = basePrefix + L"_EIT.epg";

            if (rawEvents2.empty()) {
                DebugLogFormat(L"[WARN] EIT イベントが取得できませんでした: space=%d, ch=%d", space, ch);
            }
            else {
                auto events = EITEventSortUnit::BuildUniqueSortedEvents(rawEvents2);

                if (events.empty()) {
                    DebugLogFormat(L"[WARN] EIT ユニークイベントが 0 件でした: space=%d, ch=%d", space, ch);
                }
                else {
                    if (!WriteEitDumpTextFileFromParsed(outName, events, space, ch, opts.enableAribTrace)) {
                        DebugLogFormat(L"[ERROR] EIT 出力ファイルを書き込めませんでした: %ls", outName.c_str());
                    }
                    else {
                        DebugLogFormat(L"[INFO] EIT 番組情報を出力しました: %ls (ユニーク=%zu)", outName.c_str(), events.size());
                    }
                }
            }
        }

        DebugLogFormat(
            L"[RESULT] EPGテスト結果: space=%d, ch=%d → PAT=%zu, PMT=%zu, SDT=%zu, NIT=%zu, EIT=%zu",
            space, ch,
            filterManager.GetPATCount(),
            filterManager.GetPMTCount(),
            filterManager.GetSDTCount(),
            filterManager.GetNITCount(),
            filterManager.GetEITCount()
        );

        DebugLogFormat(L"[TSValidator] summary for space=%d, ch=%d:", space, ch);
        TSValidator::DumpStatsToDebugLog();

        if (const EITFilter* eit = filterManager.GetEITFilterForDebug()) {
            DebugLogFormat(L"[EIT] Debug counters for space=%d, ch=%d:", space, ch);
            eit->DumpDebugCounters();
        }

        if (const EITFilter* eit = filterManager.GetEITFilterForDebug()) {
            if (!eit->AppendCountersTsv_AllChannels(eitCountersTsv, space, ch)) {
                DebugLogFormat(L"[ERROR] EITカウンターTSVの追記に失敗: %ls", eitCountersTsv.c_str());
            }
            else {
                DebugLogFormat(L"[INFO] EITカウンターTSVへ追記: %ls (space=%d ch=%d)", eitCountersTsv.c_str(), space, ch);
            }
        }

        auto stats = TSStreamUtils::GetTsStreamStats();
        DebugLogFormat(
            L"[TSStats] space=%d, ch=%d: call=%zu, succ=%zu, fail=%zu, packets=%zu, failRate=%.3f, avgPktPerSucc=%.1f",
            space, ch,
            stats.callCount,
            stats.successCount,
            stats.failureCount,
            stats.packetCount,
            stats.GetFailureRate(),
            stats.GetAvgPacketsPerSuccess()
        );
        };

    for (int ch = 0;; ++ch) {
        LPCTSTR chName = bon->EnumChannelName(space, ch);
        if (!chName) break;
        ProcessOneChannel(ch, chName);
    }

    DebugLog(L"[INFO] 全チャンネルの EIT(番組情報) 取得テストが完了しました。");

    decoder::AribDecoder::DumpDecodeCounters();
    FlushDebugLog();
}




// EPGデータ取得テスト (channellist.ch ベース)
void RunEpgDataTest_FromChannelList(IBonDriver2* bon, FilterManagerMode mode, int space)
{
    if (!bon) {
        DebugLog(L"[ERROR] BonDriver が無効です。");
        return;
    }

    // ログ用サニタイズ関数（RunEpgDataTest と同じ）
    auto SanitizeForLog = [](std::wstring_view src) -> std::wstring {
        std::wstring out;
        constexpr std::size_t kMaxLen = 512;
        out.reserve((std::min)(src.size(), kMaxLen));

        for (wchar_t ch : src) {
            if (out.size() >= kMaxLen) {
                out.push_back(L'…');
                break;
            }

            if (ch < 0x20) {
                if (ch == L'\n' || ch == L'\r' || ch == L'\t')
                    out.push_back(ch);
                else
                    out.push_back(L'?');
                continue;
            }

            if (ch >= 0xD800 && ch <= 0xDFFF) {
                // サロゲートペアはそのまま通す
                out.push_back(ch);
                continue;
            }

            out.push_back(ch);
        }
        return out;
        };

    // フィルター別デバッグスイッチ
    {
        std::wcout << L"[INPUT] SDTFilter の詳細デバッグログを有効にしますか？ (y/n): ";
        wchar_t sdtResp;
        std::wcin >> sdtResp;
        g_EnableSDTDebug.store(sdtResp == L'y' || sdtResp == L'Y', std::memory_order_relaxed);

        std::wcout << L"[INPUT] NITFilter の詳細デバッグログを有効にしますか？ (y/n): ";
        wchar_t nitResp;
        std::wcin >> nitResp;
        g_EnableNITDebug.store(nitResp == L'y' || nitResp == L'Y', std::memory_order_relaxed);
    }

    // 保存オプション
    bool enableTsDump = false;
    bool enableRingDump = false;
    bool enableEitDump = true;
    bool enableAribTrace = false;

    {
        std::wcout << L"[INPUT] TSストリームの保存を行いますか？ (y/n): ";
        wchar_t resp;
        std::wcin >> resp;
        enableTsDump = (resp == L'y' || resp == L'Y');

        std::wcout << L"[INPUT] リングバッファ内のTSストリームの保存を行いますか？ (y/n): ";
        wchar_t resp2;
        std::wcin >> resp2;
        enableRingDump = (resp2 == L'y' || resp2 == L'Y');

        std::wcout << L"[INPUT] EIT(番組情報)をテキストファイルに出力しますか？ (y/n): ";
        wchar_t resp3;
        std::wcin >> resp3;
        enableEitDump = (resp3 == L'y' || resp3 == L'Y');

        std::wcout << L"[INPUT] EIT ARIB デコード詳細トレースを有効にしますか？ (y/n): ";
        wchar_t resp4;
        std::wcin >> resp4;
        enableAribTrace = (resp4 == L'y' || resp4 == L'Y');
    }

    constexpr int CAPTURE_SECONDS = 40;

    // --- channellist_spaceX.ch 読み込み ---
    const std::wstring channelListFile = GetChannelListFileName(space);

    ChannelListLoader loader;
    if (!loader.LoadFromChannelListCh(channelListFile)) {
        DebugLogFormat(L"[ERROR] channellist 読み込みに失敗しました: %ls",
            channelListFile.c_str());
        std::wcout << L"[ERROR] channellist 読み込み失敗: "
            << channelListFile << std::endl;
        return;
    }

    const auto& list = loader.GetChannelList();
    if (list.empty()) {
        DebugLogFormat(L"[WARN] channellist が空です: %ls", channelListFile.c_str());
        std::wcout << L"[WARN] channellist が空のため EIT テストを実行できません。\n";
        return;
    }

    // ★サービス単位リストから「物理チャンネル一覧」を一時的に構築
    auto physicalList = PhysicalChannelListBuilder::BuildForSpace(list, space);  // :contentReference[oaicite:2]{index=2}

    if (physicalList.empty()) {
        DebugLogFormat(
            L"[WARN] 指定 space=%d に対応する物理チャンネルが channellist に存在しません: %ls",
            space, channelListFile.c_str());
        return;
    }

    DebugLogFormat(
        L"[INFO] channellist(%ls) を元に EIT(番組情報) 取得テストを開始します。 space=%d, entries=%zu, physical=%zu",
        channelListFile.c_str(),
        space,
        list.size(),
        physicalList.size()
    );

    // --- 物理チャンネルごとに EIT 取得 ---
    for (const auto& pc : physicalList)
    {
        const int ch = pc.channel;

        // ログ用の代表サービス名（最初のサービスを採用）
        std::wstring chName = L"(unknown)";
        uint16_t reprSid = 0;
        uint16_t reprTsid = 0;
        uint16_t reprOnid = 0;

        if (!pc.services.empty()) {
            const auto* svc = pc.services.front();  // 代表
            if (!svc->serviceName.empty())
                chName = svc->serviceName;
            reprSid = svc->serviceId;
            reprTsid = svc->tsid;
            reprOnid = svc->onid;

            if (pc.services.size() > 1) {
                chName += L" 他" + std::to_wstring(pc.services.size() - 1) + L"サービス";
            }
        }

        // チャンネルごとに統計リセット
        TSStreamUtils::ResetTsStreamStats();
        TSValidator::ResetStats();

        DebugLogFormat(
            L"[INFO] EIT 取得対象(物理ch): space=%d, ch=%d, Service=\"%ls\" (services=%zu, repr SID=%u, TSID=%u, ONID=%u)",
            pc.space,
            pc.channel,
            chName.c_str(),
            pc.services.size(),
            reprSid,
            reprTsid,
            reprOnid
        );

        if (!bon->SetChannel(pc.space, pc.channel)) {
            DebugLogFormat(L"[WARN] SetChannel(space=%d, ch=%d) に失敗しました。", pc.space, pc.channel);
            continue;
        }

        if (!TSStreamUtils::WaitForSignalLock(bon)) {
            DebugLogFormat(L"[WARN] 信号ロックに失敗しました: space=%d, ch=%d", pc.space, pc.channel);
            continue;
        }

        float level = bon->GetSignalLevel();
        DebugLogFormat(
            L"[INFO] 受信開始: space=%d, ch=%d, 信号レベル=%.1f dB",
            pc.space, pc.channel, level
        );

        // === フィルター処理 ===
        TSParser parser;
        FilterManager filterManager(&parser, nullptr);
        
        filterManager.ApplyMode(mode);

        constexpr size_t bufferSize = 1024;
        MPMCRingBuffer<TsPacket> ringBuffer(bufferSize);

        std::atomic<bool>   running(true);
        std::atomic<size_t> writeCount(0), writeFailCount(0);

        std::thread producer([&]() {
            std::vector<TsPacket> packets;

            while (running.load(std::memory_order_relaxed)) {
                if (ReceiveTsStreamBlock(bon, packets)) {
                    for (const auto& pkt : packets) {
                        if (ringBuffer.Push(pkt)) {
                            writeCount++;
                        }
                        else {
                            writeFailCount++;
                        }
                    }
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            });

        FilterManagerRunner runner(&filterManager, &ringBuffer);
        runner.Start();

        std::this_thread::sleep_for(std::chrono::seconds(CAPTURE_SECONDS));
        running.store(false, std::memory_order_relaxed);

        producer.join();
        runner.Stop();

        // 結果出力（カウンタ類）
        DebugLogFormat(L"[DEBUG] [RESULT] 書き込み成功：%zu 件", writeCount.load());
        DebugLogFormat(L"[DEBUG] [RESULT] 書き込み失敗（バッファ満）：%zu 件", writeFailCount.load());
        DebugLogFormat(L"[DEBUG] [RESULT] 読み出し成功（FilterManager処理）：%zu 件",
            filterManager.GetProcessedPacketCount());
        DebugLogFormat(L"[DEBUG] [RESULT] GetTsStream失敗回数：%zu",
            TSStreamUtils::GetTsStreamFailureCount());

        DebugLogFormat(L"[RESULT] PAT受信数：%zu", filterManager.GetPATCount());
        DebugLogFormat(L"[RESULT] PMT受信数：%zu", filterManager.GetPMTCount());
        DebugLogFormat(L"[RESULT] SDT受信数：%zu", filterManager.GetSDTCount());
        DebugLogFormat(L"[RESULT] NIT受信数：%zu", filterManager.GetNITCount());
        DebugLogFormat(L"[RESULT] EIT受信数：%zu", filterManager.GetEITCount());

        // TS / リングバッファのダンプ
        std::wstring basePrefix =
            L"EpgTest_space" + std::to_wstring(pc.space) + L"_ch" + std::to_wstring(pc.channel);

        if (enableTsDump) {
            TSStreamUtils::DumpAllCapturedPackets();
        }
        if (enableRingDump) {
            PacketDumpManager::DumpHexPackets(
                runner.GetExtractedPackets(),
                basePrefix + L"_RingBuffer.txt"
            );
        }
        TSStreamUtils::ClearCapturedPackets();

        // === EIT（番組情報）のテキスト出力 ===
                // === EIT（番組情報）のテキスト出力 ===
        if (enableEitDump) {
            const auto& rawEvents = filterManager.GetEITEvents();
            std::wstring outName = basePrefix + L"_EIT.epg";

            if (rawEvents.empty()) {
                DebugLogFormat(
                    L"[WARN] EIT イベントが取得できませんでした: space=%d, ch=%d",
                    pc.space, pc.channel
                );
            }
            else {
                // 重複除去＋ソート済み ParsedEITPacket の一覧
                auto events = EITEventSortUnit::BuildUniqueSortedEvents(rawEvents);

                if (events.empty()) {
                    DebugLogFormat(
                        L"[WARN] EIT ユニークイベントが 0 件でした: space=%d, ch=%d",
                        pc.space, pc.channel
                    );
                }
                else {
                    // ★ 5 引数版のヘルパを呼ぶ（MakeDumpEventFromParsed を必ず通る）
                    if (!WriteEitDumpTextFileFromParsed(
                        outName,
                        events,
                        pc.space,
                        pc.channel,
                        enableAribTrace))
                    {
                        DebugLogFormat(
                            L"[ERROR] EIT 出力ファイルを書き込めませんでした: %ls",
                            outName.c_str()
                        );
                    }
                    else {
                        DebugLogFormat(
                            L"[INFO] EIT 番組情報を出力しました: %ls (ユニーク=%zu)",
                            outName.c_str(), events.size()
                        );
                    }
                }
            }
        }




        DebugLogFormat(
            L"[RESULT] EPG(channellist)テスト結果: space=%d, ch=%d → PAT=%zu, PMT=%zu, SDT=%zu, NIT=%zu, EIT=%zu",
            pc.space, pc.channel,
            filterManager.GetPATCount(),
            filterManager.GetPMTCount(),
            filterManager.GetSDTCount(),
            filterManager.GetNITCount(),
            filterManager.GetEITCount()
        );

        DebugLogFormat(L"[TSValidator] summary for space=%d, ch=%d:", pc.space, pc.channel);
        TSValidator::DumpStatsToDebugLog();

        if (const EITFilter* eit = filterManager.GetEITFilterForDebug()) {
            DebugLogFormat(L"[EIT] Debug counters for space=%d, ch=%d:", pc.space, pc.channel);
            eit->DumpDebugCounters();
        }

        auto stats = TSStreamUtils::GetTsStreamStats();

        DebugLogFormat(L"[TSStats] space=%d, ch=%d: call=%zu, succ=%zu, fail=%zu, packets=%zu, failRate=%.3f, avgPktPerSucc=%.1f",
            pc.space, pc.channel,
            stats.callCount,
            stats.successCount,
            stats.failureCount,
            stats.packetCount,
            stats.GetFailureRate(),
            stats.GetAvgPacketsPerSuccess()
        );
    }

    DebugLog(L"[INFO] channellist に基づく全物理チャンネルの EIT(番組情報) 取得テストが完了しました。");

    decoder::AribDecoder::DumpDecodeCounters();
    FlushDebugLog();
}

// EPG ダンプパースチェック
void RunEpgDumpParseCheck()
{
    RunEITTextDumpFieldTest(L"EpgTest_space0_ch15_EIT.epg");
}


void RunEpgSortDebugTest()
{
    DumpEpgSortedByServiceAndTime(
        L"EpgTest_space0_ch15_EIT.epg",
        L"channellist_space0.ch");
}

// EPG HTML 生成テスト
void RunEpgHtmlTest()
{
    // 例: EpgTest_space0_ch15_EIT.epg から HTML を生成
    RunEITHtmlProgramTableTest(L"EpgTest_space0_ch15_EIT.epg");
    // → カレントディレクトリに EIT_ProgramTable.html ができる想定
}


// EPG 構築テスト
void RunEpgBuildTest(IBonDriver2* bon, bool enableUpdate)
{
    // 現状このテストでは BonDriver は使用しないが、
    // インターフェース互換のために引数として受け取る。
    if (!bon) {
        DebugLog(L"[ERROR] BonDriver が無効です。");
        std::wcout << L"[ERROR] BonDriver が無効です。\n";
        return;
    }

    // いまは地上波 space=0 のテストに限定
    const int space = 0;

    if (enableUpdate) {
        DebugLog(L"[EPG] enableUpdate=true ですが、現段階ではオフライン構築のみを行います。");
        std::wcout << L"[INFO] 現在、バックグラウンド更新機能は未実装です（オフライン構築のみ）。\n";
    }

    // 1) channellist_space0.ch を読み込む（物理 ch 一覧取得に使う）
    const std::wstring chFilePath = L"channellist_space0.ch";

    DebugLogFormat(L"[EPG] channellist 読み込み開始: %ls", chFilePath.c_str());
    std::wcout << L"[INFO] channellist 読み込み: " << chFilePath << L"\n";

    ChannelListLoader loader;
    if (!loader.LoadFromChannelListCh(chFilePath)) {
        DebugLogFormat(L"[ERROR][EPG] channellist 読み込み失敗: %ls", chFilePath.c_str());
        std::wcout << L"[ERROR] channellist 読み込み失敗: " << chFilePath << L"\n";
        return;
    }

    const auto& iniChannels = loader.GetChannelList();
    if (iniChannels.empty()) {
        DebugLog(L"[EPG] channellist_space0.ch にチャンネルがありません。");
        std::wcout << L"[WARN] channellist_space0.ch にチャンネルがありません。\n";
        return;
    }

    // 2) space=0 の物理チャンネルごとに、EpgTest_space0_chXX_EIT.epg を集めて “マージ .epg” を作る
    namespace fs = std::filesystem;

    const std::wstring mergedEpgPath = L"EpgMerged_space0_All_EIT.epg";

    std::ofstream ofsMerged(fs::path(mergedEpgPath), std::ios::binary);
    if (!ofsMerged) {
        DebugLogFormat(L"[ERROR][EPG] マージ出力ファイル作成失敗: %ls", mergedEpgPath.c_str());
        std::wcout << L"[ERROR] マージ出力ファイル作成失敗: " << mergedEpgPath << L"\n";
        return;
    }

    auto AppendFileSkipUtf8Bom = [&](const fs::path& srcPath) -> bool
        {
            std::ifstream ifs(srcPath, std::ios::binary);
            if (!ifs) return false;

            std::string bytes;
            ifs.seekg(0, std::ios::end);
            const std::streamoff len = ifs.tellg();
            if (len < 0) return false;
            bytes.resize(static_cast<std::size_t>(len));
            ifs.seekg(0, std::ios::beg);
            if (len > 0) ifs.read(bytes.data(), len);

            if (bytes.size() >= 3 &&
                static_cast<unsigned char>(bytes[0]) == 0xEF &&
                static_cast<unsigned char>(bytes[1]) == 0xBB &&
                static_cast<unsigned char>(bytes[2]) == 0xBF)
            {
                bytes.erase(bytes.begin(), bytes.begin() + 3);
            }

            if (!bytes.empty()) {
                ofsMerged.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            }

            // ファイル境界に改行を入れてパーサが壊れないようにする
            ofsMerged.write("\n\n", 2);
            return ofsMerged.good();
        };

    std::set<int> scannedChannels;
    std::size_t appendedCount = 0;

    for (const auto& ch : iniChannels) {
        if (ch.space != space) continue;

        // 物理chごとに1回
        if (!scannedChannels.insert(ch.channel).second) continue;

        std::wostringstream oss;
        oss << L"EpgTest_space" << space
            << L"_ch" << ch.channel
            << L"_EIT.epg";
        const std::wstring eitPathW = oss.str();
        const fs::path eitPath = fs::path(eitPathW);

        if (!fs::exists(eitPath)) {
            DebugLogFormat(L"[EPG] EIT テキストが見つかりません: %ls", eitPathW.c_str());
            continue;
        }

        DebugLogFormat(L"[EPG] EIT テキストをマージに追加: %ls", eitPathW.c_str());
        if (AppendFileSkipUtf8Bom(eitPath)) {
            ++appendedCount;
        }
        else {
            DebugLogFormat(L"[EPG][WARN] マージ失敗: %ls", eitPathW.c_str());
        }
    }

    ofsMerged.flush();
    ofsMerged.close();

    if (appendedCount == 0) {
        DebugLog(L"[ERROR][EPG] マージ対象の .epg が 0 件でした。EpgTest_space0_chXX_EIT.epg が存在するか確認してください。");
        std::wcout << L"[ERROR] マージ対象の .epg が 0 件でした。EpgTest_space0_chXX_EIT.epg を確認してください。\n";
        return;
    }

    DebugLogFormat(L"[EPG] マージ完了: %ls (inputs=%zu)",
        mergedEpgPath.c_str(), appendedCount);
    std::wcout << L"[INFO] マージ完了: " << mergedEpgPath
        << L" (inputs=" << appendedCount << L")\n";

    // 3) 追加した “EIT→HTML(時間×サービス)” 生成関数で出力
    const std::wstring htmlPath = L"epg_test_grid.html";

    if (!GenerateEITHtmlProgramTable(mergedEpgPath, chFilePath, htmlPath)) {
        DebugLogFormat(L"[ERROR][EPG] GenerateEITHtmlProgramTable 失敗: merged=%ls, ch=%ls -> %ls",
            mergedEpgPath.c_str(), chFilePath.c_str(), htmlPath.c_str());
        std::wcout << L"[ERROR] EPG HTML の書き出しに失敗: " << htmlPath << L"\n";
        return;
    }

    DebugLogFormat(L"[EPG] EPG HTML を出力しました: %ls", htmlPath.c_str());
    std::wcout << L"[OK] EPG HTML を出力しました: " << htmlPath << L"\n";

    // 4) （任意）サービス＋時間順のデバッグログを出したい場合
    //    ※ログが膨大になるので、必要なときだけ有効にしてください。
    // DumpEpgSortedByServiceAndTime(mergedEpgPath, chFilePath);
}


// =============================================================
// EIT取得 → (ch別 .epg 出力) → マージ(.epg) → HTML生成 まで一括実行
// =============================================================
void RunEpgBuildTest2(IBonDriver2* bon, FilterManagerMode mode, bool enableUpdate)
{
    if (!bon) {
        DebugLog(L"[ERROR] BonDriver が無効です。");
        std::wcout << L"[ERROR] BonDriver が無効です。\n";
        return;
    }

    // 現状は地上波 space=0 の統合テストに限定
    const int space = 0;

    if (enableUpdate) {
        DebugLog(L"[EPG] enableUpdate=true: EitEventUpdateUnit による更新マージを有効化します（同一実行内の差分/改善を統合）。");
        std::wcout << L"[INFO] enableUpdate=true: 更新マージ（EitEventUpdateUnit）を有効化します。\n";
    }

    // HTML生成側が channellist_space0.ch を前提にしているので、先に存在チェック
    const std::wstring chFilePath = L"channellist_space0.ch";
    if (!std::filesystem::exists(std::filesystem::path(chFilePath))) {
        DebugLogFormat(L"[ERROR][EPG] 必要な channellist がありません: %ls", chFilePath.c_str());
        std::wcout << L"[ERROR] 必要な channellist がありません: " << chFilePath << L"\n";
        std::wcout << L"        先にチャンネルスキャンで channellist_space0.ch を生成してください。\n";
        return;
    }

    auto SanitizeForLog = [](std::wstring_view src) -> std::wstring {
        std::wstring out;
        constexpr std::size_t kMaxLen = 512;
        out.reserve((std::min)(src.size(), kMaxLen));

        for (wchar_t ch : src) {
            if (out.size() >= kMaxLen) {
                out.push_back(L'…');
                break;
            }
            if (ch < 0x20) {
                if (ch == L'\n' || ch == L'\r' || ch == L'\t') out.push_back(ch);
                else out.push_back(L'?');
                continue;
            }
            if (ch >= 0xD800 && ch <= 0xDFFF) {
                out.push_back(ch);
                continue;
            }
            out.push_back(ch);
        }
        return out;
        };

    auto AskYesNo = [](std::wstring_view prompt) -> bool {
        while (true) {
            std::wcout << prompt;
            wchar_t resp = 0;
            if (!(std::wcin >> resp)) {
                std::wcin.clear();
                return false;
            }
            switch (resp) {
            case L'y': case L'Y': return true;
            case L'n': case L'N': return false;
            default:
                std::wcout << L"[WARN] y か n で入力してください。\n";
                break;
            }
        }
        };

    struct EpgBuild2Options {
        bool enableTsDump = false;
        bool enableRingDump = false;
        bool enableEitDump = true;     // ★ RunEpgBuildTest2 では強制ON（後段のHTML生成に必要）
        bool enableAribTrace = false;
    } opts;

    // --- フィルター詳細ログ（SDT/NIT） ---
    {
        bool sdtDbg = AskYesNo(L"[INPUT] SDTFilter の詳細デバッグログを有効にしますか？ (y/n): ");
        g_EnableSDTDebug.store(sdtDbg, std::memory_order_relaxed);

        bool nitDbg = AskYesNo(L"[INPUT] NITFilter の詳細デバッグログを有効にしますか？ (y/n): ");
        g_EnableNITDebug.store(nitDbg, std::memory_order_relaxed);
    }

    // --- ダンプ系 ---
    {
        opts.enableTsDump = AskYesNo(L"[INPUT] TSストリームの保存を行いますか？ (y/n): ");
        opts.enableRingDump = AskYesNo(L"[INPUT] リングバッファ内のTSストリームの保存を行いますか？ (y/n): ");
        std::wcout << L"[INFO] RunEpgBuildTest2 では EIT(.epg) 出力は強制的に有効です（HTML生成に必要）。\n";
        opts.enableAribTrace = AskYesNo(L"[INPUT] EIT ARIB デコード詳細トレースを有効にしますか？ (y/n): ");
        opts.enableEitDump = true;
    }

    DebugLog(L"[INFO] RunEpgBuildTest2: EIT取得→HTML生成の統合テストを開始します。");
    DebugLogFormat(L"[INFO] target: space=%d (terrestrial)", space);

    const std::filesystem::path eitCountersTsv =
        std::filesystem::path(L"EpgTest_EITCounters_AllChannels.tsv");

    // ---- duration 取得（メンバ有無両対応）----
    auto GetDurationSec = [](const auto& e) -> uint32_t {
        auto BcdToUint = [](uint8_t b) -> uint32_t {
            return static_cast<uint32_t>(((b >> 4) & 0x0F) * 10 + (b & 0x0F));
            };

        uint32_t durSec = 0;

        if constexpr (requires { e.duration; }) {
            durSec = static_cast<uint32_t>(e.duration);
        }
        if (durSec == 0) {
            const uint32_t bcd = static_cast<uint32_t>(e.duration_hhmmss);
            const uint8_t bH = static_cast<uint8_t>((bcd >> 16) & 0xFF);
            const uint8_t bM = static_cast<uint8_t>((bcd >> 8) & 0xFF);
            const uint8_t bS = static_cast<uint8_t>((bcd >> 0) & 0xFF);

            const uint32_t dh = BcdToUint(bH);
            const uint32_t dm = BcdToUint(bM);
            const uint32_t ds = BcdToUint(bS);

            if (dh <= 23U && dm <= 59U && ds <= 59U) {
                durSec = dh * 3600U + dm * 60U + ds;
            }
        }
        return durSec;
        };

    // =========================================================
    // 1) EIT取得（全ch）→ ch別 EpgTest_space0_chXX_EIT.epg を生成
    //    ★更新対応：EitEventUpdateUnit を channel内に組み込み
    // =========================================================
    auto ProcessOneChannel = [&](int ch, LPCTSTR chName) {

        std::wstring chNameSafe = chName ? SanitizeForLog(chName) : L"(null)";

        TSStreamUtils::ResetTsStreamStats();
        TSValidator::ResetStats();

        DebugLogFormat(L"[INFO] 番組取得対象チャンネル候補: space=%d, ch=%d (%ls)",
            space, ch, chNameSafe.c_str());

        if (!bon->SetChannel(space, ch)) {
            DebugLogFormat(L"[WARN] SetChannel(space=%d, ch=%d) に失敗しました。", space, ch);
            return;
        }

        if (!TSStreamUtils::WaitForSignalLock(bon)) {
            DebugLogFormat(L"[WARN] 信号ロックに失敗しました: space=%d, ch=%d", space, ch);
            return;
        }

        float level = bon->GetSignalLevel();
        DebugLogFormat(L"[INFO] 受信開始: space=%d, ch=%d, 信号レベル=%.1f dB",
            space, ch, level);

        TSParser parser;
        FilterManager filterManager(&parser, nullptr);
        filterManager.ApplyMode(mode);

        // =========================================================
        // 更新ストア（差分/改善/差し替え 統合）
        // =========================================================
        EitEventUpdateUnit updateStore;
        EitEventUpdateUnit::UpdateOptions upOpt{};

        // raw差分を Apply するので、内部 unique 化を有効にしておく（安全）
        upOpt.enablePreSort = true;
        upOpt.relaxKeyWhenInvalidStartTime = true;

        // =========================================================
        // EIT-RETRY（RunEpgDataTest のロジックをベースに維持）
        // =========================================================
        struct EitRetryConfig {
            int    captureSecondsPerCycle = 60 * 2;
            int    maxCycles = 1 * 2;

            double coverageThreshold = 0.85;

            bool   stopWhenFirst2DaysFull = true;
            double first2DaysThreshold = 0.95;

            double plateauEps = 0.003;
            int    plateauNeed = 2;
            std::size_t plateauMinAddedUnique = 50;

            std::size_t ringBufferSize = 8192 * 2;

            int pushRetryCount = 8;
            int pushRetrySleepUs = 100;

            bool   enableSchedule50Eval = true;

            bool   enableSectionGate = true;
            double sectionGateThreshold = 0.50;

            bool   stopWhenSchedule50Enough = false;
            double schedule50SectionThreshold = 0.95;
        } eitCfg;

        int     usedCycles = 0;
        double  finalCoverageAverage = 0.0;
        bool    finishedByCoverage = false;
        std::size_t writeCountTotal = 0;
        std::size_t writeFailCountTotal = 0;

        // =========================================================
        // coverage計算：base を JST epoch minutes に揃える
        // =========================================================
        constexpr int64_t kJstOffsetMinutes = 9LL * 60LL; // JST=UTC+9
        const int64_t baseEpochMinutesUtc = []() -> int64_t {
            using namespace std::chrono;
            return duration_cast<minutes>(system_clock::now().time_since_epoch()).count();
            }();
        int64_t baseEpochMinutesJst = baseEpochMinutesUtc + kJstOffsetMinutes;
        baseEpochMinutesJst -= (baseEpochMinutesJst % static_cast<int64_t>(EitCoverage::kSlotMinutes));

        auto IsInvalidStartTime = [](uint64_t st) noexcept -> bool {
            return (st == 0ULL) || (st == 0xFFFFFFFFFFULL);
            };

        double prevScore = -1.0;
        std::size_t prevUnique = 0;
        int plateauCount = 0;

        for (int cycle = 1; cycle <= eitCfg.maxCycles; ++cycle)
        {
            usedCycles = cycle;

            const std::size_t beforeEvents = filterManager.GetEITEvents().size();

            MPMCRingBuffer<TsPacket> ringBuffer(eitCfg.ringBufferSize);
            std::atomic<std::size_t> writeCount(0), writeFail(0);

            FilterManagerRunner runner(&filterManager, &ringBuffer);
            runner.Start();

            std::thread producer([&]() {
                std::vector<TsPacket> packets;
                packets.reserve(4096);

                const ULONGLONG start = GetTickCount64();

                while ((GetTickCount64() - start) < static_cast<ULONGLONG>(eitCfg.captureSecondsPerCycle) * 1000ULL)
                {
                    packets.clear();

                    if (ReceiveTsStreamBlock(bon, packets)) {
                        for (const auto& pkt : packets) {
                            bool pushed = false;
                            for (int t = 0; t < eitCfg.pushRetryCount; ++t) {
                                if (ringBuffer.Push(pkt)) { pushed = true; break; }
                                std::this_thread::sleep_for(std::chrono::microseconds(eitCfg.pushRetrySleepUs));
                            }
                            if (pushed) writeCount.fetch_add(1, std::memory_order_relaxed);
                            else        writeFail.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
                });

            producer.join();

            // runner が処理し切るのを待つ（最大 2 秒 / 0.25 秒アイドルで収束扱い）
            {
                using namespace std::chrono;
                const auto overall0 = steady_clock::now();
                auto idle0 = steady_clock::now();

                std::size_t last = filterManager.GetProcessedPacketCount();

                for (;;) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));

                    const std::size_t cur = filterManager.GetProcessedPacketCount();
                    if (cur != last) {
                        last = cur;
                        idle0 = steady_clock::now();
                    }

                    if (steady_clock::now() - idle0 >= milliseconds(250)) break;
                    if (steady_clock::now() - overall0 >= seconds(2)) break;
                }
            }

            runner.Stop();

            writeCountTotal += writeCount.load(std::memory_order_relaxed);
            writeFailCountTotal += writeFail.load(std::memory_order_relaxed);

            // =========================================================
            // ★ 更新マージ：この cycle で増えた差分だけ Apply
            // =========================================================
            {
                const auto& rawAll = filterManager.GetEITEvents();

                std::vector<ParsedEITPacket> delta;
                if (rawAll.size() > beforeEvents) {
                    delta.assign(rawAll.begin() + static_cast<std::ptrdiff_t>(beforeEvents), rawAll.end());
                }

                auto ust = updateStore.Apply(delta, upOpt);

                DebugLogFormat(
                    L"[EIT-UPD] space=%d ch=%d cycle=%d: delta=%zu | added=%llu updated=%llu replaced=%llu unchanged=%llu filled=%llu mergedItems=%llu store=%llu",
                    space, ch, cycle,
                    delta.size(),
                    (unsigned long long)ust.added,
                    (unsigned long long)ust.updated,
                    (unsigned long long)ust.replaced,
                    (unsigned long long)ust.unchanged,
                    (unsigned long long)ust.filledFields,
                    (unsigned long long)ust.mergedExtItems,
                    (unsigned long long)ust.storeSize
                );
            }

            // --- coverage 算出 ---
            EitCoverage cov;
            cov.Reset();

            const auto& rawEvents = filterManager.GetEITEvents();

            std::size_t rawHasName = 0;
            for (const auto& e : rawEvents) if (!e.short_event_name_bytes.empty()) ++rawHasName;
            DebugLogFormat(L"[CHECK] rawEvents=%zu rawHasName=%zu (%.1f%%)",
                rawEvents.size(), rawHasName,
                rawEvents.empty() ? 0.0 : (rawHasName * 100.0 / rawEvents.size()));

            // 評価用（従来どおり rawベース）
            auto uniqueEventsEval = EITEventSortUnit::BuildUniqueSortedEvents(rawEvents);

            // ★停止判定/カバレッジは UpdateUnit スナップショット基準
            auto snapshot = updateStore.ExportSnapshotSorted();
            auto uniqueEventsCov = EITEventSortUnit::BuildUniqueSortedEvents(snapshot);

            // =========================================================
            // 0x50/0x51 評価（既存ロジック維持：raw + uniqueEventsEval）
            // =========================================================
            struct EventKeyStrict {
                uint16_t onid = 0, tsid = 0, sid = 0, eid = 0;
                uint64_t start_time_raw = 0;
                bool operator==(const EventKeyStrict& o) const noexcept {
                    return onid == o.onid && tsid == o.tsid && sid == o.sid && eid == o.eid && start_time_raw == o.start_time_raw;
                }
            };
            struct EventKeyStrictHash {
                std::size_t operator()(const EventKeyStrict& k) const noexcept {
                    uint64_t x = 0;
                    x ^= (uint64_t)k.onid << 48;
                    x ^= (uint64_t)k.tsid << 32;
                    x ^= (uint64_t)k.sid << 16;
                    x ^= (uint64_t)k.eid;
                    x ^= (k.start_time_raw * 0x9e3779b97f4a7c15ULL);
                    x ^= (x >> 33);
                    x *= 0xff51afd7ed558ccdULL;
                    x ^= (x >> 33);
                    return (std::size_t)x;
                }
            };

            struct EventKeyLoose {
                uint16_t onid = 0, tsid = 0, sid = 0, eid = 0;
                bool operator==(const EventKeyLoose& o) const noexcept {
                    return onid == o.onid && tsid == o.tsid && sid == o.sid && eid == o.eid;
                }
            };
            struct EventKeyLooseHash {
                std::size_t operator()(const EventKeyLoose& k) const noexcept {
                    uint64_t x = 0;
                    x ^= (uint64_t)k.onid << 32;
                    x ^= (uint64_t)k.tsid << 16;
                    x ^= (uint64_t)k.sid;
                    x ^= (uint64_t)k.eid << 1;
                    x ^= (x >> 33);
                    x *= 0xc4ceb9fe1a85ec53ULL;
                    x ^= (x >> 33);
                    return (std::size_t)x;
                }
            };

            struct RawInfo {
                uint8_t seenMask = 0;
                uint8_t nameMask = 0;
            };
            constexpr uint8_t kSeen50 = 0x01;
            constexpr uint8_t kSeen51 = 0x02;

            std::unordered_map<EventKeyStrict, RawInfo, EventKeyStrictHash> rawInfoStrict;
            std::unordered_map<EventKeyLoose, RawInfo, EventKeyLooseHash>  rawInfoLoose;
            rawInfoStrict.reserve((std::max<std::size_t>)(rawEvents.size() / 3, 128));
            rawInfoLoose.reserve((std::max<std::size_t>)(rawEvents.size() / 3, 128));

            struct SvcKey {
                uint16_t onid = 0, tsid = 0, sid = 0;
                bool operator==(const SvcKey& o) const noexcept { return onid == o.onid && tsid == o.tsid && sid == o.sid; }
            };
            struct SvcKeyHash {
                std::size_t operator()(const SvcKey& k) const noexcept {
                    uint64_t x = 0;
                    x ^= (uint64_t)k.onid << 32;
                    x ^= (uint64_t)k.tsid << 16;
                    x ^= (uint64_t)k.sid;
                    x ^= (x >> 33);
                    x *= 0xc4ceb9fe1a85ec53ULL;
                    x ^= (x >> 33);
                    return (std::size_t)x;
                }
            };

            std::unordered_set<SvcKey, SvcKeyHash> svcSeen50;
            std::unordered_set<SvcKey, SvcKeyHash> svcSeen51;
            svcSeen50.reserve(64);
            svcSeen51.reserve(64);

            std::size_t raw50 = 0, raw51 = 0, raw50Name = 0, raw51Name = 0;

            for (const auto& e : rawEvents) {
                const uint16_t onid = e.original_network_id;
                const uint16_t tsid = e.transport_stream_id;
                const uint16_t sid = e.service_id;
                const uint16_t eid = e.event_id;
                if (onid == 0 || tsid == 0 || sid == 0) continue;

                const bool hasName = !e.short_event_name_bytes.empty();

                const EventKeyStrict ks{ onid, tsid, sid, eid, e.start_time };
                const EventKeyLoose  kl{ onid, tsid, sid, eid };

                if (e.table_id == 0x50) {
                    ++raw50;
                    if (hasName) ++raw50Name;
                    svcSeen50.emplace(SvcKey{ onid, tsid, sid });

                    auto& s = rawInfoStrict[ks];
                    s.seenMask |= kSeen50;
                    if (hasName) s.nameMask |= kSeen50;

                    auto& l = rawInfoLoose[kl];
                    l.seenMask |= kSeen50;
                    if (hasName) l.nameMask |= kSeen50;
                }
                else if (e.table_id == 0x51) {
                    ++raw51;
                    if (hasName) ++raw51Name;
                    svcSeen51.emplace(SvcKey{ onid, tsid, sid });

                    auto& s = rawInfoStrict[ks];
                    s.seenMask |= kSeen51;
                    if (hasName) s.nameMask |= kSeen51;

                    auto& l = rawInfoLoose[kl];
                    l.seenMask |= kSeen51;
                    if (hasName) l.nameMask |= kSeen51;
                }
            }

            std::size_t uniqKey50 = 0, uniqKey51 = 0, uniqKey50Name = 0, uniqKey51Name = 0;
            std::size_t lostName50 = 0, lostName51 = 0;
            std::size_t unmatchedKeys = 0;

            auto LookupRawInfo = [&](const ParsedEITPacket& u) -> const RawInfo* {
                const EventKeyStrict ks{ u.original_network_id, u.transport_stream_id, u.service_id, u.event_id, u.start_time };
                auto itS = rawInfoStrict.find(ks);
                if (itS != rawInfoStrict.end()) return &itS->second;

                const EventKeyLoose kl{ u.original_network_id, u.transport_stream_id, u.service_id, u.event_id };
                auto itL = rawInfoLoose.find(kl);
                if (itL != rawInfoLoose.end()) return &itL->second;

                return nullptr;
                };

            for (const auto& u : uniqueEventsEval) {
                const RawInfo* info = LookupRawInfo(u);
                if (!info) { ++unmatchedKeys; continue; }

                const bool mergedHasName = !u.short_event_name_bytes.empty();

                if (info->seenMask & kSeen50) {
                    ++uniqKey50;
                    if (mergedHasName) ++uniqKey50Name;
                    if ((info->nameMask & kSeen50) && !mergedHasName) ++lostName50;
                }
                if (info->seenMask & kSeen51) {
                    ++uniqKey51;
                    if (mergedHasName) ++uniqKey51Name;
                    if ((info->nameMask & kSeen51) && !mergedHasName) ++lostName51;
                }
            }

            auto Pct = [](std::size_t a, std::size_t b) -> double {
                return (b == 0) ? 0.0 : (static_cast<double>(a) * 100.0 / static_cast<double>(b));
                };

            // =========================================================
            // 0x50/0x51 セクションカバレッジ（EITFilter側の指標）
            // =========================================================
            double cov50Sum = 0.0, cov51Sum = 0.0;
            double cov50Min = 1.0, cov51Min = 1.0;
            std::size_t seg50 = 0, seg51 = 0;
            std::size_t seg50Complete = 0, seg51Complete = 0;

            if (eitCfg.enableSchedule50Eval) {
                if (const EITFilter* eit = filterManager.GetEITFilterForDebug()) {
                    for (const auto& s : svcSeen50) {
                        const double r = eit->GetSectionCoverageRatio(s.onid, s.tsid, s.sid, 0x50);
                        cov50Sum += r;
                        cov50Min = (std::min)(cov50Min, r);
                        ++seg50;
                        if (eit->IsEpgSegmentComplete(s.onid, s.tsid, s.sid, 0x50, eitCfg.schedule50SectionThreshold)) {
                            ++seg50Complete;
                        }
                    }
                    for (const auto& s : svcSeen51) {
                        const double r = eit->GetSectionCoverageRatio(s.onid, s.tsid, s.sid, 0x51);
                        cov51Sum += r;
                        cov51Min = (std::min)(cov51Min, r);
                        ++seg51;
                        if (eit->IsEpgSegmentComplete(s.onid, s.tsid, s.sid, 0x51, eitCfg.schedule50SectionThreshold)) {
                            ++seg51Complete;
                        }
                    }
                }
            }

            const double raw50NamePct = Pct(raw50Name, raw50);
            const double raw51NamePct = Pct(raw51Name, raw51);
            const double uniq50NamePct = Pct(uniqKey50Name, uniqKey50);
            const double uniq51NamePct = Pct(uniqKey51Name, uniqKey51);

            const double cov50Avg = (seg50 > 0) ? (cov50Sum / static_cast<double>(seg50)) : 0.0;
            const double cov51Avg = (seg51 > 0) ? (cov51Sum / static_cast<double>(seg51)) : 0.0;
            if (seg50 == 0) cov50Min = 0.0;
            if (seg51 == 0) cov51Min = 0.0;

            const double secAvgAll = ((seg50 + seg51) > 0)
                ? ((cov50Sum + cov51Sum) / static_cast<double>(seg50 + seg51))
                : 0.0;

            DebugLogFormat(
                L"[EIT-50EVAL] raw50=%zu(name=%zu %.1f%%) raw51=%zu(name=%zu %.1f%%) | "
                L"uniqByKey50=%zu(name=%zu %.1f%%) uniqByKey51=%zu(name=%zu %.1f%%) | "
                L"lostName50=%zu lostName51=%zu unmatched=%zu | "
                L"sec50(avg/min)=%.3f/%.3f complete=%zu/%zu | sec51(avg/min)=%.3f/%.3f complete=%zu/%zu | "
                L"svc50=%zu svc51=%zu secAvgAll=%.3f",
                raw50, raw50Name, raw50NamePct,
                raw51, raw51Name, raw51NamePct,
                uniqKey50, uniqKey50Name, uniq50NamePct,
                uniqKey51, uniqKey51Name, uniq51NamePct,
                lostName50, lostName51, unmatchedKeys,
                cov50Avg, cov50Min, seg50Complete, seg50,
                cov51Avg, cov51Min, seg51Complete, seg51,
                svcSeen50.size(), svcSeen51.size(),
                secAvgAll
            );

            if (eitCfg.stopWhenSchedule50Enough && (seg50 + seg51) > 0) {
                if (secAvgAll >= eitCfg.schedule50SectionThreshold) {
                    DebugLogFormat(L"[EIT-50EVAL] stopBy50: secAvgAll=%.3f >= %.3f", secAvgAll, eitCfg.schedule50SectionThreshold);
                    finishedByCoverage = true;
                    break;
                }
            }

            // =========================================================
            // coverage（イベント時間帯ベース） ※ updateStore スナップショット基準
            // =========================================================
            std::size_t converted = 0;
            std::size_t skippedInvalidTime = 0;

            for (const auto& e : uniqueEventsCov) {
                if (IsInvalidStartTime(e.start_time)) { ++skippedInvalidTime; continue; }

                const uint8_t b0 = static_cast<uint8_t>((e.start_time >> 32) & 0xFF);
                const uint8_t b1 = static_cast<uint8_t>((e.start_time >> 24) & 0xFF);
                const uint8_t b2 = static_cast<uint8_t>((e.start_time >> 16) & 0xFF);
                const uint8_t b3 = static_cast<uint8_t>((e.start_time >> 8) & 0xFF);
                const uint8_t b4 = static_cast<uint8_t>((e.start_time >> 0) & 0xFF);

                const uint16_t mjd = static_cast<uint16_t>((static_cast<uint16_t>(b0) << 8) | b1);

                const auto bcd2 = [](uint8_t v) -> int {
                    return static_cast<int>(((v >> 4) & 0x0F) * 10 + (v & 0x0F));
                    };

                const int hh = bcd2(b2);
                const int mm = bcd2(b3);
                const int ss = bcd2(b4);

                if (mjd == 0 || mjd == 0xFFFF ||
                    hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59) {
                    ++skippedInvalidTime;
                    continue;
                }

                const int64_t daysSinceEpoch = static_cast<int64_t>(mjd) - 40587LL;
                if (daysSinceEpoch < -40000LL || daysSinceEpoch > 60000LL) {
                    ++skippedInvalidTime;
                    continue;
                }

                // EIT start_time は UTC なので、JSTに合わせるなら +9h
                const int64_t startEpochMinutesUtc =
                    daysSinceEpoch * 1440LL
                    + static_cast<int64_t>(hh) * 60LL
                    + static_cast<int64_t>(mm);

                const int64_t startEpochMinutesJst = startEpochMinutesUtc + kJstOffsetMinutes;

                const int64_t startMinutesFromBase = startEpochMinutesJst - baseEpochMinutesJst;

                const uint32_t durSec = GetDurationSec(e);
                if (durSec == 0) { ++skippedInvalidTime; continue; }

                const int durationMinutes = static_cast<int>((durSec + 59U) / 60U);
                if (durationMinutes <= 0) { ++skippedInvalidTime; continue; }

                const int64_t endMinutes = startMinutesFromBase + static_cast<int64_t>(durationMinutes);
                if (endMinutes <= 0 || startMinutesFromBase >= static_cast<int64_t>(EitCoverage::kTotalMinutes)) {
                    continue;
                }

                cov.OnEventRange(static_cast<int>(startMinutesFromBase), durationMinutes);
                ++converted;
            }

            const auto sum = cov.GetSummary();

            finalCoverageAverage = (sum.coverageFirst2Days + sum.coverage2to8Days) * 0.5;
            const double score = sum.coverageFirst2Days * 0.7 + sum.coverage2to8Days * 0.3;

            const double gatedScore = (eitCfg.enableSectionGate && (seg50 + seg51) > 0)
                ? (score * 0.7 + secAvgAll * 0.3)
                : score;

            const std::size_t afterEvents = rawEvents.size();
            const long long deltaEvents =
                static_cast<long long>(afterEvents) - static_cast<long long>(beforeEvents);

            const std::size_t uniq = uniqueEventsCov.size();
            const long long deltaUniq =
                static_cast<long long>(uniq) - static_cast<long long>(prevUnique);

            if (converted == 0) {
                DebugLog(L"[EIT-RETRY][WARN] coverage算出に必要な start_time/duration が有効なイベントが見つからず、coverage=0相当になります。");
            }

            DebugLogFormat(
                L"[EIT-RETRY] space=%d, ch=%d, cycle=%d/%d: raw(before=%zu, after=%zu, %+lld), unique(store)=%zu (%+lld), "
                L"cov(avg)=%.3f, score=%.3f, gatedScore=%.3f, secAvgAll=%.3f, first2Days=%.3f, 2to8Days=%.3f, first2DaysFull=%d, converted=%zu, skipped=%zu",
                space, ch, cycle, eitCfg.maxCycles,
                beforeEvents, afterEvents, deltaEvents,
                uniq, deltaUniq,
                finalCoverageAverage,
                score,
                gatedScore,
                secAvgAll,
                sum.coverageFirst2Days, sum.coverage2to8Days,
                sum.first2DaysFull ? 1 : 0,
                converted,
                skippedInvalidTime
            );

            const bool nearEnough = sum.first2DaysFull || (sum.coverageFirst2Days >= eitCfg.first2DaysThreshold);

            if (eitCfg.stopWhenFirst2DaysFull && nearEnough) {
                if (!eitCfg.enableSectionGate || (seg50 + seg51) == 0 || secAvgAll >= eitCfg.sectionGateThreshold) {
                    finishedByCoverage = true;
                    break;
                }
                else {
                    DebugLogFormat(L"[EIT-RETRY] gate(first2Days): first2DaysFull but secAvgAll=%.3f < %.3f (continue)",
                        secAvgAll, eitCfg.sectionGateThreshold);
                }
            }

            if (finalCoverageAverage >= eitCfg.coverageThreshold) {
                if (!eitCfg.enableSectionGate || (seg50 + seg51) == 0 || secAvgAll >= eitCfg.sectionGateThreshold) {
                    finishedByCoverage = true;
                    break;
                }
                else {
                    DebugLogFormat(L"[EIT-RETRY] gate: cov(avg)=%.3f >= %.3f but secAvgAll=%.3f < %.3f (continue)",
                        finalCoverageAverage, eitCfg.coverageThreshold, secAvgAll, eitCfg.sectionGateThreshold);
                }
            }

            const std::size_t addedUniq =
                (uniq >= prevUnique) ? (uniq - prevUnique) : 0;

            if (prevScore >= 0.0 &&
                std::abs(score - prevScore) < eitCfg.plateauEps &&
                addedUniq < eitCfg.plateauMinAddedUnique)
            {
                ++plateauCount;
            }
            else {
                plateauCount = 0;
            }

            if (plateauCount >= eitCfg.plateauNeed) {
                DebugLogFormat(
                    L"[EIT-RETRY] plateau stop: score=%.3f (prev=%.3f), addedUnique=%zu (<%zu), count=%d/%d",
                    score, prevScore, addedUniq, eitCfg.plateauMinAddedUnique,
                    plateauCount, eitCfg.plateauNeed
                );
                break;
            }

            prevScore = score;
            prevUnique = uniq;
        }

        DebugLogFormat(
            L"[EIT] capture summary: space=%d, ch=%d, cycles=%d, avgCoverage=%.3f, storeSize=%zu, finishedByCoverage=%d",
            space, ch,
            usedCycles,
            finalCoverageAverage,
            updateStore.Size(),
            finishedByCoverage ? 1 : 0
        );

        DebugLogFormat(L"[DEBUG] [RESULT] 書き込み成功：%zu 件", writeCountTotal);
        DebugLogFormat(L"[DEBUG] [RESULT] 書き込み失敗（バッファ満）：%zu 件", writeFailCountTotal);
        DebugLogFormat(L"[DEBUG] [RESULT] 読み出し成功（FilterManager処理）：%zu 件",
            filterManager.GetProcessedPacketCount());
        DebugLogFormat(L"[DEBUG] [RESULT] GetTsStream失敗回数：%zu",
            TSStreamUtils::GetTsStreamFailureCount());

        DebugLogFormat(L"[RESULT] PAT受信数：%zu", filterManager.GetPATCount());
        DebugLogFormat(L"[RESULT] PMT受信数：%zu", filterManager.GetPMTCount());
        DebugLogFormat(L"[RESULT] SDT受信数：%zu", filterManager.GetSDTCount());
        DebugLogFormat(L"[RESULT] NIT受信数：%zu", filterManager.GetNITCount());
        DebugLogFormat(L"[RESULT] EIT受信数：%zu", filterManager.GetEITCount());

        std::wstring basePrefix = L"EpgTest_space" + std::to_wstring(space) + L"_ch" + std::to_wstring(ch);

        if (opts.enableTsDump) {
            TSStreamUtils::DumpAllCapturedPackets();
        }

        if (opts.enableRingDump) {
            DebugLog(L"[WARN] RingBuffer ダンプは本テストでは未対応です（必要なら consumer 側でミラー保存を追加してください）。");
        }

        TSStreamUtils::ClearCapturedPackets();

        // ★ HTML生成に必要なので必ず .epg を出力する（更新ストア基準）
        {
            std::wstring outName = basePrefix + L"_EIT.epg";

            auto snapshot2 = updateStore.ExportSnapshotSorted();
            if (snapshot2.empty()) {
                DebugLogFormat(L"[WARN] EIT イベントが取得できませんでした: space=%d, ch=%d", space, ch);
            }
            else {
                auto events = EITEventSortUnit::BuildUniqueSortedEvents(snapshot2);
                if (events.empty()) {
                    DebugLogFormat(L"[WARN] EIT ユニークイベントが 0 件でした: space=%d, ch=%d", space, ch);
                }
                else {
                    if (!WriteEitDumpTextFileFromParsed(outName, events, space, ch, opts.enableAribTrace)) {
                        DebugLogFormat(L"[ERROR] EIT 出力ファイルを書き込めませんでした: %ls", outName.c_str());
                    }
                    else {
                        DebugLogFormat(L"[INFO] EIT 番組情報を出力しました: %ls (ユニーク=%zu)", outName.c_str(), events.size());
                    }
                }
            }
        }

        DebugLogFormat(
            L"[RESULT] EPGテスト結果: space=%d, ch=%d → PAT=%zu, PMT=%zu, SDT=%zu, NIT=%zu, EIT=%zu",
            space, ch,
            filterManager.GetPATCount(),
            filterManager.GetPMTCount(),
            filterManager.GetSDTCount(),
            filterManager.GetNITCount(),
            filterManager.GetEITCount()
        );

        DebugLogFormat(L"[TSValidator] summary for space=%d, ch=%d:", space, ch);
        TSValidator::DumpStatsToDebugLog();

        if (const EITFilter* eit = filterManager.GetEITFilterForDebug()) {
            DebugLogFormat(L"[EIT] Debug counters for space=%d, ch=%d:", space, ch);
            eit->DumpDebugCounters();

            if (!eit->AppendCountersTsv_AllChannels(eitCountersTsv, space, ch)) {
                DebugLogFormat(L"[ERROR] EITカウンターTSVの追記に失敗: %ls", eitCountersTsv.c_str());
            }
            else {
                DebugLogFormat(L"[INFO] EITカウンターTSVへ追記: %ls (space=%d ch=%d)", eitCountersTsv.c_str(), space, ch);
            }
        }

        auto stats = TSStreamUtils::GetTsStreamStats();
        DebugLogFormat(
            L"[TSStats] space=%d, ch=%d: call=%zu, succ=%zu, fail=%zu, packets=%zu, failRate=%.3f, avgPktPerSucc=%.1f",
            space, ch,
            stats.callCount,
            stats.successCount,
            stats.failureCount,
            stats.packetCount,
            stats.GetFailureRate(),
            stats.GetAvgPacketsPerSuccess()
        );
        };

    // --- 全ch ---
    for (int ch = 0;; ++ch) {
        LPCTSTR chName = bon->EnumChannelName(space, ch);
        if (!chName) break;
        ProcessOneChannel(ch, chName);
    }

    DebugLog(L"[INFO] RunEpgBuildTest2: 全チャンネルの EIT 取得が完了しました。");

    // =========================================================
    // 2) channellist_space0.ch を読み込み、EpgTest_space0_chXX_EIT.epg をマージ
    // =========================================================
    ChannelListLoader loader;
    DebugLogFormat(L"[EPG] channellist 読み込み開始: %ls", chFilePath.c_str());
    std::wcout << L"[INFO] channellist 読み込み: " << chFilePath << L"\n";

    if (!loader.LoadFromChannelListCh(chFilePath)) {
        DebugLogFormat(L"[ERROR][EPG] channellist 読み込み失敗: %ls", chFilePath.c_str());
        std::wcout << L"[ERROR] channellist 読み込み失敗: " << chFilePath << L"\n";
        return;
    }

    const auto& iniChannels = loader.GetChannelList();
    if (iniChannels.empty()) {
        DebugLog(L"[EPG] channellist_space0.ch にチャンネルがありません。");
        std::wcout << L"[WARN] channellist_space0.ch にチャンネルがありません。\n";
        return;
    }

    namespace fs = std::filesystem;

    const std::wstring mergedEpgPath = L"EpgMerged_space0_All_EIT.epg";
    std::ofstream ofsMerged(fs::path(mergedEpgPath), std::ios::binary);
    if (!ofsMerged) {
        DebugLogFormat(L"[ERROR][EPG] マージ出力ファイル作成失敗: %ls", mergedEpgPath.c_str());
        std::wcout << L"[ERROR] マージ出力ファイル作成失敗: " << mergedEpgPath << L"\n";
        return;
    }

    auto AppendFileSkipUtf8Bom = [&](const fs::path& srcPath) -> bool
        {
            std::ifstream ifs(srcPath, std::ios::binary);
            if (!ifs) return false;

            std::string bytes;
            ifs.seekg(0, std::ios::end);
            const std::streamoff len = ifs.tellg();
            if (len < 0) return false;
            bytes.resize(static_cast<std::size_t>(len));
            ifs.seekg(0, std::ios::beg);
            if (len > 0) ifs.read(bytes.data(), len);

            if (bytes.size() >= 3 &&
                static_cast<unsigned char>(bytes[0]) == 0xEF &&
                static_cast<unsigned char>(bytes[1]) == 0xBB &&
                static_cast<unsigned char>(bytes[2]) == 0xBF)
            {
                bytes.erase(bytes.begin(), bytes.begin() + 3);
            }

            if (!bytes.empty()) {
                ofsMerged.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            }

            // ファイル境界に改行を入れてパーサが壊れないようにする
            ofsMerged.write("\n\n", 2);
            return ofsMerged.good();
        };

    std::set<int> scannedChannels;
    std::size_t appendedCount = 0;

    for (const auto& ch : iniChannels) {
        if (ch.space != space) continue;

        // 物理chごとに1回
        if (!scannedChannels.insert(ch.channel).second) continue;

        std::wostringstream oss;
        oss << L"EpgTest_space" << space
            << L"_ch" << ch.channel
            << L"_EIT.epg";
        const std::wstring eitPathW = oss.str();
        const fs::path eitPath = fs::path(eitPathW);

        if (!fs::exists(eitPath)) {
            DebugLogFormat(L"[EPG] EIT テキストが見つかりません: %ls", eitPathW.c_str());
            continue;
        }

        DebugLogFormat(L"[EPG] EIT テキストをマージに追加: %ls", eitPathW.c_str());
        if (AppendFileSkipUtf8Bom(eitPath)) {
            ++appendedCount;
        }
        else {
            DebugLogFormat(L"[EPG][WARN] マージ失敗: %ls", eitPathW.c_str());
        }
    }

    ofsMerged.flush();
    ofsMerged.close();

    if (appendedCount == 0) {
        DebugLog(L"[ERROR][EPG] マージ対象の .epg が 0 件でした。EpgTest_space0_chXX_EIT.epg が存在するか確認してください。");
        std::wcout << L"[ERROR] マージ対象の .epg が 0 件でした。EpgTest_space0_chXX_EIT.epg を確認してください。\n";
        return;
    }

    DebugLogFormat(L"[EPG] マージ完了: %ls (inputs=%zu)",
        mergedEpgPath.c_str(), appendedCount);
    std::wcout << L"[INFO] マージ完了: " << mergedEpgPath
        << L" (inputs=" << appendedCount << L")\n";

    // =========================================================
    // 3) HTML生成
    // =========================================================
    const std::wstring htmlPath = L"epg_test_grid.html";

    if (!GenerateEITHtmlProgramTable(mergedEpgPath, chFilePath, htmlPath)) {
        DebugLogFormat(L"[ERROR][EPG] GenerateEITHtmlProgramTable 失敗: merged=%ls, ch=%ls -> %ls",
            mergedEpgPath.c_str(), chFilePath.c_str(), htmlPath.c_str());
        std::wcout << L"[ERROR] EPG HTML の書き出しに失敗: " << htmlPath << L"\n";
        return;
    }

    DebugLogFormat(L"[EPG] EPG HTML を出力しました: %ls", htmlPath.c_str());
    std::wcout << L"[OK] EPG HTML を出力しました: " << htmlPath << L"\n";

    // 4) 末尾の統計/カウンタ出力
    decoder::AribDecoder::DumpDecodeCounters();
    FlushDebugLog();
}

// ===== TDT/TOT テスト（PID 0x0014）=====
// - TOT は TOTFilter を使用
// - TDT はこの関数内の簡易(Lite)実装で復元（TDTFilterクラス未実装でもOK）
//
// 注意：BonDriver が 204/192 バイトパケットを返す場合があるため、
//       1パケット分の先頭 188 バイトのみをフィルターへ渡す。
void RunTdtTotTest(IBonDriver2* bon, int space, int captureSec)
{
    if (!bon) {
        DebugLog(L"[ERROR] BonDriver が無効です。");
        return;
    }

    if (captureSec <= 0) captureSec = 10;
    captureSec = (std::clamp)(captureSec, 5, 120);

    struct Layout {
        int pktSize = 188;
        std::size_t syncOff = 0;
    };

    auto DetectPacketLayout = [](const uint8_t* buf, std::size_t len) -> Layout {
        // 4連続同期で (pktSize, syncOff) を決める（誤同期対策）
        for (int step : {188, 204, 192}) {
            const std::size_t s = static_cast<std::size_t>(step);
            if (len < s * 4) continue;
            for (std::size_t off = 0; off < s && off + s * 3 < len; ++off) {
                if (buf[off] == 0x47 &&
                    buf[off + s] == 0x47 &&
                    buf[off + s * 2] == 0x47 &&
                    buf[off + s * 3] == 0x47) {
                    return Layout{ step, off };
                }
            }
        }
        // fallback
        for (std::size_t off = 0; off < len; ++off) {
            if (buf[off] == 0x47) return Layout{ 188, off };
        }
        return Layout{ 188, 0 };
        };

    auto FormatTdt = [](const TDTFilter::TimeInfo& t) -> std::wstring {
        std::wstringstream ss;
        ss << std::setfill(L'0')
            << t.year << L"-"
            << std::setw(2) << t.month << L"-"
            << std::setw(2) << t.day << L" "
            << std::setw(2) << t.hour << L":"
            << std::setw(2) << t.minute << L":"
            << std::setw(2) << t.second;
        return ss.str();
        };

    auto FormatTot = [](const TOTFilter::TimeInfo& t) -> std::wstring {
        std::wstringstream ss;
        ss << std::setfill(L'0')
            << t.year << L"-"
            << std::setw(2) << t.month << L"-"
            << std::setw(2) << t.day << L" "
            << std::setw(2) << t.hour << L":"
            << std::setw(2) << t.minute << L":"
            << std::setw(2) << t.second
            << L" (offsetMin=" << t.offsetMinutes << L")";
        return ss.str();
        };

    DebugLogFormat(L"[TDT/TOT] start: space=%d, captureSec=%d", space, captureSec);

    for (int ch = 0;; ++ch)
    {
        LPCTSTR chName = bon->EnumChannelName(space, ch);
        if (!chName) break;

        DebugLogFormat(L"[TDT/TOT] begin: space=%d, ch=%d (%ls)", space, ch, chName);

        if (!bon->SetChannel(space, ch)) {
            DebugLogFormat(L"[TDT/TOT] WARN: SetChannel failed: space=%d, ch=%d", space, ch);
            continue;
        }

        if (!TSStreamUtils::WaitForSignalLock(bon)) {
            DebugLogFormat(L"[TDT/TOT] WARN: signal lock failed: space=%d, ch=%d", space, ch);
            continue;
        }

        const float level = bon->GetSignalLevel();
        DebugLogFormat(L"[TDT/TOT] signal: space=%d, ch=%d, level=%.1f dB", space, ch, level);

        TDTFilter tdt;
        TOTFilter tot;
        tdt.Reset();
        tot.Reset();

        std::size_t gotPackets = 0;
        std::size_t pid14Packets = 0;
        std::size_t tdtSeen = 0;
        std::size_t totSeen = 0;
        std::size_t emptyReads = 0;

        const auto tEnd = std::chrono::steady_clock::now() + std::chrono::seconds(captureSec);

        // ★両方揃ったら早期終了。TOTだけ取れても captureSec までは粘る（TDT探索）
        while (std::chrono::steady_clock::now() < tEnd && !(tdt.IsRecovered() && tot.IsRecovered()))
        {
            (void)bon->WaitTsStream(50);

            DWORD size = 0, remain = 0;
            BYTE* p = nullptr;
            (void)bon->GetTsStream(&p, &size, &remain);

            if (!p || size == 0) {
                ++emptyReads;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            const auto* buf = reinterpret_cast<const uint8_t*>(p);
            const Layout lay = DetectPacketLayout(buf, size);

            for (std::size_t pos = lay.syncOff;
                pos + static_cast<std::size_t>(lay.pktSize) <= size;
                pos += static_cast<std::size_t>(lay.pktSize))
            {
                const uint8_t* pkt = buf + pos;
                if (pkt[0] != 0x47) continue;

                ++gotPackets;

                if (pkt[1] & 0x80) continue; // TEI

                const uint16_t pid = static_cast<uint16_t>(((pkt[1] & 0x1F) << 8) | pkt[2]);
                if (pid != 0x0014) continue;

                ++pid14Packets;

                // table_id 覗き（PUSI=1 の payload 先頭から、1パケット内に完結する複数セクションも数える）
                const bool pusi = (pkt[1] & 0x40) != 0;
                const uint8_t afc = (pkt[3] >> 4) & 0x03;
                if (pusi && ((afc & 0x01) != 0)) {
                    std::size_t ppos = 4;

                    if (afc & 0x02) {
                        if (ppos >= 188) goto FEED_FILTERS;
                        const uint8_t al = pkt[ppos];
                        if (ppos + 1u + al > 188) goto FEED_FILTERS;
                        ppos += 1u + al;
                        if (ppos >= 188) goto FEED_FILTERS;
                    }

                    if (ppos >= 188) goto FEED_FILTERS;
                    const uint8_t pointer = pkt[ppos];
                    if (ppos + 1u + pointer > 188) goto FEED_FILTERS;
                    ppos += 1u + pointer;
                    if (ppos >= 188) goto FEED_FILTERS;

                    std::size_t remainBytes = 188 - ppos;
                    while (remainBytes >= 3) {
                        const uint8_t tableId = pkt[ppos];
                        const uint16_t secLen =
                            static_cast<uint16_t>(((pkt[ppos + 1] & 0x0F) << 8) | pkt[ppos + 2]);
                        const std::size_t total = 3u + static_cast<std::size_t>(secLen);
                        if (total > remainBytes) break; // 次パケットに跨る

                        if (tableId == 0x70) ++tdtSeen;
                        else if (tableId == 0x73) ++totSeen;

                        ppos += total;
                        remainBytes -= total;
                    }
                }

            FEED_FILTERS:
                // フィルター投入（先頭188バイトだけでOK：204/192でもヘッダ+payloadは188）
                tdt.InputPacket(pkt, 188);
                tot.InputPacket(pkt, 188);

                // 早期終了判定は while 条件でやってるのでここでは何もしない
            }
        }

        // 結果出力
        if (tdt.IsRecovered()) {
            DebugLogFormat(L"[TDT] space=%d, ch=%d: %ls", space, ch, FormatTdt(tdt.GetLastTime()).c_str());
        }
        else {
            DebugLogFormat(L"[TDT] space=%d, ch=%d: (not recovered)", space, ch);
        }

        if (tot.IsRecovered()) {
            DebugLogFormat(L"[TOT] space=%d, ch=%d: %ls", space, ch, FormatTot(tot.GetLastTime()).c_str());
        }
        else {
            DebugLogFormat(L"[TOT] space=%d, ch=%d: (not recovered)", space, ch);
        }

        DebugLogFormat(L"[TDT/TOT] stats: gotPackets=%zu, pid14=%zu, tdtSeen=%zu, totSeen=%zu, emptyReads=%zu",
            gotPackets, pid14Packets, tdtSeen, totSeen, emptyReads);

        if (totSeen > 0 && tdtSeen == 0) {
            DebugLog(L"[TDT/TOT] note: TOT seen but TDT not seen (TDT may be absent or very low frequency).");
        }

        DebugLogFormat(L"[TDT/TOT] end: space=%d, ch=%d", space, ch);
    }

    DebugLog(L"[TDT/TOT] finished.");
    FlushDebugLog();
}

// ---------------------- CDT テスト ----------------------
void RunCdtTest(IBonDriver2* bon, int space, int captureSec)
{
    if (!bon) {
        DebugLog(L"[ERROR] BonDriver が無効です。");
        return;
    }
    if (captureSec <= 0) captureSec = 8;

    DebugLogFormat(L"[CDT] start: space=%d, captureSec=%d", space, captureSec);

    for (int ch = 0;; ++ch)
    {
        LPCTSTR chName = bon->EnumChannelName(space, ch);
        if (!chName) break;

        TSStreamUtils::ResetTsStreamStats();

        DebugLogFormat(L"[CDT] begin: space=%d, ch=%d (%ls)", space, ch, chName);

        if (!bon->SetChannel(space, ch)) {
            DebugLogFormat(L"[CDT] WARN: SetChannel failed: space=%d, ch=%d", space, ch);
            continue;
        }
        if (!TSStreamUtils::WaitForSignalLock(bon)) {
            DebugLogFormat(L"[CDT] WARN: signal lock failed: space=%d, ch=%d", space, ch);
            continue;
        }

        const float level = bon->GetSignalLevel();
        DebugLogFormat(L"[CDT] signal: space=%d, ch=%d, level=%.1f dB", space, ch, level);

        TSParser parser;
        FilterManager filterManager(&parser, nullptr);

        // ★ CDT のみ有効化（他は切る）
        {
            FilterManagerFlags f = filterManager.GetFlags();
            f.enablePIDAnalyze = true;

            f.enablePAT = false;
            f.enablePMT = false;
            f.enableSDT = false;
            f.enableNIT = false;
            f.enableEIT = false;
            f.enableTDT = false;
            f.enableTOT = false;

            f.enableCDT = true; // ★ここが目的
            filterManager.SetFlags(f);
        }

        constexpr size_t bufferSize = 8192;
        MPMCRingBuffer<TsPacket> ringBuffer(bufferSize);

        FilterManagerRunner runner(&filterManager, &ringBuffer);
        runner.Start();

        TsPacketCaptureOptions capOpt;
        capOpt.captureSec = captureSec;
        capOpt.packetReserve = 4096;
        capOpt.sleepOnFailMs = 1;
        capOpt.receiveFn = &ReceiveTsStreamBlock;

        std::atomic_bool cancelRequested{ false };
        TsPacketCaptureSession capture;

        if (!capture.Start(bon, &ringBuffer, capOpt, &cancelRequested)) {
            DebugLogFormat(L"[CDT] ERROR: capture.Start failed: space=%d, ch=%d", space, ch);
            runner.Stop();
            continue;
        }

        std::this_thread::sleep_for(std::chrono::seconds(captureSec));
        capture.StopAndJoin();

        // runner が処理し切るのを少し待つ
        {
            using namespace std::chrono;
            const auto overall0 = steady_clock::now();
            auto idle0 = steady_clock::now();
            std::size_t last = filterManager.GetProcessedPacketCount();

            for (;;) {
                std::this_thread::sleep_for(milliseconds(10));
                const std::size_t cur = filterManager.GetProcessedPacketCount();
                if (cur != last) { last = cur; idle0 = steady_clock::now(); }
                if (steady_clock::now() - idle0 >= milliseconds(250)) break;
                if (steady_clock::now() - overall0 >= seconds(2)) break;
            }
        }

        runner.Stop();

        // ★結果
        DebugLogFormat(L"[CDT] stats: processed=%zu, pid29=%zu, pusi=%zu, c8Seen=%zu",
            filterManager.GetProcessedPacketCount(),
            filterManager.GetCDTPidCount(),
            filterManager.GetCDTPusiCount(),
            filterManager.GetCDTCount());

        DebugLogFormat(L"[CDT] recovered=%d", filterManager.IsCDTRecovered() ? 1 : 0);

        DebugLogFormat(L"[CDT] end: space=%d, ch=%d", space, ch);
    }

    DebugLog(L"[CDT] finished.");
    FlushDebugLog();
}


// ---------------------- CAT/BIT テスト ----------------------
void RunCatBitTest(IBonDriver2* bon, int space, int captureSec)
{
    if (!bon) {
        DebugLog(L"[ERROR] BonDriver が無効です。");
        return;
    }

    DebugLogFormat(L"[CAT/BIT] Test begin: space=%d, captureSec=%d", space, captureSec);

    for (int ch = 0;; ++ch)
    {
        LPCTSTR chName = bon->EnumChannelName(space, ch);
        if (!chName) break;

        TSStreamUtils::ResetTsStreamStats();

        DebugLogFormat(L"[CAT/BIT] BEGIN: space=%d, ch=%d (%ls)", space, ch, chName);

        if (!bon->SetChannel(space, ch)) {
            DebugLogFormat(L"[CAT/BIT] [WARN] SetChannel(space=%d, ch=%d) に失敗", space, ch);
            continue;
        }

        if (!TSStreamUtils::WaitForSignalLock(bon)) {
            DebugLogFormat(L"[CAT/BIT] [WARN] 信号ロックに失敗: space=%d, ch=%d", space, ch);
            continue;
        }

        const float level = bon->GetSignalLevel();
        DebugLogFormat(L"[CAT/BIT] 受信開始: space=%d, ch=%d, 信号レベル=%.1f dB", space, ch, level);

        TSParser parser;
        FilterManager filterManager(&parser, nullptr);

        // CAT/BIT だけ有効化（他はOFF）
        FilterManagerFlags flags{};
        flags.enablePIDAnalyze = false;
        flags.enablePAT = false;
        flags.enablePMT = false;
        flags.enableSDT = false;
        flags.enableNIT = false;
        flags.enableEIT = false;
        flags.enableTDT = false;
        flags.enableTOT = false;
        flags.enableCDT = false;
        flags.enableCAT = true;
        flags.enableBIT = true;
        filterManager.SetFlags(flags);

        constexpr size_t bufferSize = 8192;
        MPMCRingBuffer<TsPacket> ringBuffer(bufferSize);

        FilterManagerRunner runner(&filterManager, &ringBuffer);
        runner.Start();

        // GetTsStream は ReceiveTsStreamBlock に任せる（＝このTUで直呼びしない）
        TsPacketCaptureOptions capOpt;
        capOpt.captureSec = captureSec;
        capOpt.packetReserve = 4096;
        capOpt.sleepOnFailMs = 1;
        capOpt.receiveFn = &ReceiveTsStreamBlock; // ts_receiver.hpp 側の実績ルート

        std::atomic_bool cancelRequested{ false };
        TsPacketCaptureSession capture;

        if (!capture.Start(bon, &ringBuffer, capOpt, &cancelRequested)) {
            DebugLogFormat(L"[CAT/BIT] [ERROR] TsPacketCaptureSession.Start() に失敗: space=%d, ch=%d", space, ch);
            runner.Stop();
            continue;
        }

        std::this_thread::sleep_for(std::chrono::seconds(captureSec));
        capture.StopAndJoin();

        const auto capCounts = capture.GetCounts();

        // runner の取りこぼし待ち（最大2秒 / 250msアイドルで収束扱い）
        {
            using namespace std::chrono;
            const auto overall0 = steady_clock::now();
            auto idle0 = steady_clock::now();
            std::size_t last = filterManager.GetProcessedPacketCount();

            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));

                const std::size_t cur = filterManager.GetProcessedPacketCount();
                if (cur != last) {
                    last = cur;
                    idle0 = steady_clock::now();
                }

                if (steady_clock::now() - idle0 >= milliseconds(250)) break;
                if (steady_clock::now() - overall0 >= seconds(2)) break;
            }
        }

        runner.Stop();

        const auto stats = TSStreamUtils::GetTsStreamStats();

        DebugLogFormat(L"[CAT/BIT] [RESULT] pushOk=%zu, pushFail=%zu, processed=%zu",
            capCounts.pushOk,
            capCounts.pushFail,
            filterManager.GetProcessedPacketCount());

        DebugLogFormat(L"[CAT/BIT] [TSStats] call=%zu, succ=%zu, fail=%zu, packets=%zu, failRate=%.3f, avgPktPerSucc=%.1f",
            stats.callCount,
            stats.successCount,
            stats.failureCount,
            stats.packetCount,
            stats.GetFailureRate(),
            stats.GetAvgPacketsPerSuccess());

        DebugLogFormat(L"[CAT/BIT] [Counts] CAT(pidPackets)=%zu, recovered=%d",
            filterManager.GetCATCount(),
            filterManager.IsCATRecovered() ? 1 : 0);

        DebugLogFormat(L"[CAT/BIT] [Counts] BIT(pidPackets)=%zu, pusi=%zu, tableSeen=%zu, recovered=%d",
            filterManager.GetBITPidCount(),
            filterManager.GetBITPusiCount(),
            filterManager.GetBITCount(),
            filterManager.IsBITRecovered() ? 1 : 0);

        DebugLogFormat(L"[CAT/BIT] END: space=%d, ch=%d (%ls)", space, ch, chName);
    }

    DebugLog(L"[CAT/BIT] Test end.");
    FlushDebugLog();
}

// ---------------------- RST テスト ----------------------
void RunRstTest(IBonDriver2* bon, int space, int captureSec)
{
    if (!bon) {
        DebugLog(L"[ERROR] BonDriver が無効です。");
        return;
    }

    DebugLogFormat(L"[RST] Test begin: space=%d, captureSec=%d", space, captureSec);

    for (int ch = 0;; ++ch)
    {
        LPCTSTR chName = bon->EnumChannelName(space, ch);
        if (!chName) break;

        TSStreamUtils::ResetTsStreamStats();

        DebugLogFormat(L"[RST] BEGIN: space=%d, ch=%d (%ls)", space, ch, chName);

        if (!bon->SetChannel(space, ch)) {
            DebugLogFormat(L"[RST] [WARN] SetChannel(space=%d, ch=%d) に失敗", space, ch);
            continue;
        }
        if (!TSStreamUtils::WaitForSignalLock(bon)) {
            DebugLogFormat(L"[RST] [WARN] 信号ロックに失敗: space=%d, ch=%d", space, ch);
            continue;
        }

        const float level = bon->GetSignalLevel();
        DebugLogFormat(L"[RST] 受信開始: space=%d, ch=%d, 信号レベル=%.1f dB", space, ch, level);

        TSParser parser;
        FilterManager filterManager(&parser, nullptr);

        FilterManagerFlags flags{};
        flags.enablePIDAnalyze = false;
        flags.enablePAT = false;
        flags.enablePMT = false;
        flags.enableSDT = false;
        flags.enableNIT = false;
        flags.enableEIT = false;
        flags.enableTDT = false;
        flags.enableTOT = false;
        flags.enableCDT = false;
        flags.enableCAT = false;
        flags.enableBIT = false;
        flags.enableRST = true;
        filterManager.SetFlags(flags);

        constexpr size_t bufferSize = 8192;
        MPMCRingBuffer<TsPacket> ringBuffer(bufferSize);

        FilterManagerRunner runner(&filterManager, &ringBuffer);
        runner.Start();

        TsPacketCaptureOptions capOpt;
        capOpt.captureSec = captureSec;
        capOpt.packetReserve = 4096;
        capOpt.sleepOnFailMs = 1;
        capOpt.receiveFn = &ReceiveTsStreamBlock;

        std::atomic_bool cancelRequested{ false };
        TsPacketCaptureSession capture;

        if (!capture.Start(bon, &ringBuffer, capOpt, &cancelRequested)) {
            DebugLogFormat(L"[RST] [ERROR] TsPacketCaptureSession.Start() に失敗: space=%d, ch=%d", space, ch);
            runner.Stop();
            continue;
        }

        std::this_thread::sleep_for(std::chrono::seconds(captureSec));
        capture.StopAndJoin();

        const auto capCounts = capture.GetCounts();

        // runner の取りこぼし待ち
        {
            using namespace std::chrono;
            const auto overall0 = steady_clock::now();
            auto idle0 = steady_clock::now();
            std::size_t last = filterManager.GetProcessedPacketCount();

            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));

                const std::size_t cur = filterManager.GetProcessedPacketCount();
                if (cur != last) {
                    last = cur;
                    idle0 = steady_clock::now();
                }

                if (steady_clock::now() - idle0 >= milliseconds(250)) break;
                if (steady_clock::now() - overall0 >= seconds(2)) break;
            }
        }

        // ★ Stop 前に RSTFilter 値を“コピー”して退避（Stop後にポインタを触らない）
        std::size_t rf_pid = 0, rf_pusi = 0, rf_sec = 0, rf_ent = 0, rf_inv = 0, rf_cc = 0;
        std::size_t rf_snap = 0;
        bool rf_ok = false;

        if (const RSTFilter* rf = filterManager.GetRSTFilterForDebug()) {
            rf_ok = true;
            rf_pid = rf->GetPidPacketCount();
            rf_pusi = rf->GetPusiCount();
            rf_sec = rf->GetSectionCount();
            rf_ent = rf->GetEntryCount();
            rf_inv = rf->GetInvalidSectionCount();
            rf_cc = rf->GetContinuityErrorCount();
            rf_snap = rf->GetSnapshot().size(); // 大量出力防止：件数だけ
        }

        runner.Stop();

        const auto stats = TSStreamUtils::GetTsStreamStats();

        DebugLogFormat(L"[RST] [RESULT] pushOk=%zu, pushFail=%zu, processed=%zu",
            capCounts.pushOk, capCounts.pushFail, filterManager.GetProcessedPacketCount());

        DebugLogFormat(L"[RST] [TSStats] call=%zu, succ=%zu, fail=%zu, packets=%zu, failRate=%.3f, avgPktPerSucc=%.1f",
            stats.callCount, stats.successCount, stats.failureCount, stats.packetCount,
            stats.GetFailureRate(), stats.GetAvgPacketsPerSuccess());

        DebugLogFormat(L"[RST] [Counts] RST(pidPackets)=%zu, recovered=%d",
            filterManager.GetRSTCount(), filterManager.IsRSTRecovered() ? 1 : 0);

        if (rf_ok) {
            DebugLogFormat(L"[RST] [RSTFilter] pid=%zu, pusi=%zu, section=%zu, entry=%zu, invalid=%zu, ccErr=%zu, snapshot=%zu",
                rf_pid, rf_pusi, rf_sec, rf_ent, rf_inv, rf_cc, rf_snap);
        }

        DebugLogFormat(L"[RST] END: space=%d, ch=%d (%ls)", space, ch, chName);
    }

    DebugLog(L"[RST] Test end.");
    FlushDebugLog();
}

void RunTsdtDitSitScanTest(IBonDriver2* bon)
{
    if (!bon) {
        DebugLog(L"[ERROR] BonDriver が無効です。");
        return;
    }

    const int space = 0;

    // 受信時間（秒）: DIT/SIT が稀なら 30～120 に上げる
    const int captureSec = 18;

    DebugLog(L"[INFO] TSDT/DIT/SIT ScanTest: 地上波チャンネル全体を対象に解析を開始します。");
    DebugLogFormat(L"[INFO] captureSec=%d", captureSec);

    for (int ch = 0;; ++ch)
    {
        LPCTSTR chName = bon->EnumChannelName(space, ch);
        if (!chName) break;

        TSStreamUtils::ResetTsStreamStats();

        DebugLogFormat(L"[BEGIN] TsdtDitSitScan: space=%d, ch=%d (%ls)", space, ch, chName);

        if (!bon->SetChannel(space, ch)) {
            DebugLogFormat(L"[WARN] SetChannel(space=%d, ch=%d) に失敗", space, ch);
            continue;
        }

        if (!TSStreamUtils::WaitForSignalLock(bon)) {
            DebugLogFormat(L"[WARN] 信号ロックに失敗: space=%d, ch=%d", space, ch);
            continue;
        }

        const float level = bon->GetSignalLevel();
        DebugLogFormat(L"[INFO] 受信開始: space=%d, ch=%d, 信号レベル=%.1f dB", space, ch, level);

        TSParser parser;
        FilterManager filterManager(&parser, nullptr);

        // まず軽いモードで開始（必要最低限）
        filterManager.ApplyMode(FilterManagerMode::SignalMonitor);

        // flags に enableTSDT/enableDIT/enableSIT が存在するなら ON（存在しない場合は何もしない）
        {
            auto f = filterManager.GetFlags();
            if constexpr (requires { f.enableTSDT; }) f.enableTSDT = true;
            if constexpr (requires { f.enableDIT; })  f.enableDIT = true;
            if constexpr (requires { f.enableSIT; })  f.enableSIT = true;
            filterManager.SetFlags(f);
        }

        // ★ 少し大きめ推奨（満杯で取りこぼすとテストがブレる）
        constexpr size_t bufferSize = 8192;
        MPMCRingBuffer<TsPacket> ringBuffer(bufferSize);

        // consumer 起動
        FilterManagerRunner runner(&filterManager, &ringBuffer);
        runner.Start();

        // producer（TsPacketCaptureSession）
        TsPacketCaptureOptions capOpt;
        capOpt.captureSec = captureSec;
        capOpt.packetReserve = 4096;
        capOpt.sleepOnFailMs = 1;
        capOpt.receiveFn = &ReceiveTsStreamBlock;

        std::atomic_bool cancelRequested{ false };
        TsPacketCaptureSession capture;

        if (!capture.Start(bon, &ringBuffer, capOpt, &cancelRequested)) {
            DebugLogFormat(L"[ERROR] TsPacketCaptureSession.Start() に失敗: space=%d, ch=%d", space, ch);
            runner.Stop();
            continue;
        }

        std::this_thread::sleep_for(std::chrono::seconds(captureSec));
        capture.StopAndJoin();

        const auto capCounts = capture.GetCounts();

        // runner が処理し切るのを待つ（最大 2 秒 / 0.25 秒アイドルで収束扱い）
        {
            using namespace std::chrono;
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

                if (steady_clock::now() - idle0 >= milliseconds(250)) break;
                if (steady_clock::now() - overall0 >= seconds(2)) break;
            }
        }

        runner.Stop();

        // 結果出力（到来確認メイン）
        DebugLogFormat(L"[RESULT] pushOk=%zu, pushFail=%zu, processed=%zu",
            capCounts.pushOk, capCounts.pushFail, filterManager.GetProcessedPacketCount());

        DebugLogFormat(L"[RESULT] PID 0x0002(TSDT) packets=%zu", filterManager.GetTSDTCount());
        DebugLogFormat(L"[RESULT] PID 0x0015(DIT)  packets=%zu", filterManager.GetDITCount());
        DebugLogFormat(L"[RESULT] PID 0x0016(SIT)  packets=%zu", filterManager.GetSITCount());

        // 回収判定APIが存在する場合のみ出力（存在しないならコンパイルに影響なし）
        if constexpr (requires (const FilterManager & fm) { fm.IsTSDTRecovered(); }) {
            DebugLogFormat(L"[RESULT] TSDT recovered=%d", filterManager.IsTSDTRecovered() ? 1 : 0);
        }
        if constexpr (requires (const FilterManager & fm) { fm.IsDITRecovered(); }) {
            DebugLogFormat(L"[RESULT] DIT  recovered=%d", filterManager.IsDITRecovered() ? 1 : 0);
        }
        if constexpr (requires (const FilterManager & fm) { fm.IsSITRecovered(); }) {
            DebugLogFormat(L"[RESULT] SIT  recovered=%d", filterManager.IsSITRecovered() ? 1 : 0);
        }

        DebugLogFormat(L"[RESULT] GetTsStream失敗回数：%zu", TSStreamUtils::GetTsStreamFailureCount());

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

        DebugLogFormat(L"[END] TsdtDitSitScan: space=%d, ch=%d (%ls)", space, ch, chName);
    }

    DebugLog(L"[INFO] TSDT/DIT/SIT ScanTest: 全チャンネルの走査が完了しました。");
    FlushDebugLog();
}
