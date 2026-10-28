#include "FilterManager.hpp"
#include "DebugManager.h"
#include <optional>
#include <cstring>

#define TSCEPG_TRACE_RST 1  // 必要な時だけON

#if TSCEPG_TRACE_RST
static std::atomic<uint64_t> g_pid13_packets{ 0 };
static std::atomic<uint64_t> g_pid13_pusi{ 0 };
static std::atomic<uint64_t> g_pid13_table71{ 0 };
static std::atomic<uint8_t>  g_pid13_last_tid{ 0xFF };
#endif

static constexpr size_t kMaxSdtDumpPackets = 4096;

FilterManager::~FilterManager() = default;

namespace {

    static std::optional<uint8_t> PeekPsiTableId_OnPusi(const uint8_t* data, size_t size) noexcept
    {
        if (!data || size < 4) return std::nullopt;
        if (data[0] != 0x47) return std::nullopt;

        const bool pusi = (data[1] & 0x40) != 0;
        if (!pusi) return std::nullopt;

        const uint8_t afc = (data[3] >> 4) & 0x03;
        if ((afc & 0x01) == 0) return std::nullopt; // payload無し

        size_t p = 4;

        if (afc & 0x02) { // adaptation_field
            if (p >= size) return std::nullopt;
            const uint8_t adapt_len = data[p];
            p += 1u + adapt_len;
            if (p >= size) return std::nullopt;
        }

        if (p >= size) return std::nullopt;
        const uint8_t pointer = data[p];
        p += 1u;

        if (p + pointer >= size) return std::nullopt;
        p += pointer;

        if (p >= size) return std::nullopt;
        return data[p];
    }

#if TSCEPG_TRACE_RST
    static void TraceRST_OnPid13Packet(const uint8_t* data, size_t size) noexcept
    {
        const uint64_t pktN = g_pid13_packets.fetch_add(1, std::memory_order_relaxed) + 1;

        const bool pusi = (data && size >= 2) ? ((data[1] & 0x40) != 0) : false;
        if (pusi) g_pid13_pusi.fetch_add(1, std::memory_order_relaxed);

        if (auto tid = PeekPsiTableId_OnPusi(data, size)) {
            g_pid13_last_tid.store(*tid, std::memory_order_relaxed);
            if (*tid == 0x71) g_pid13_table71.fetch_add(1, std::memory_order_relaxed);
        }

        if (pktN == 1 || pktN == 10 || (pktN % 1000) == 0) {
            const auto pusiN = g_pid13_pusi.load(std::memory_order_relaxed);
            const auto t71N = g_pid13_table71.load(std::memory_order_relaxed);
            const auto last = g_pid13_last_tid.load(std::memory_order_relaxed);

            DebugLogFormat(
                L"[RST-TRACE] PID=0x0013 packets=%llu, pusi=%llu, table71=%llu, last_tid=0x%02X",
                static_cast<unsigned long long>(pktN),
                static_cast<unsigned long long>(pusiN),
                static_cast<unsigned long long>(t71N),
                static_cast<unsigned int>(last)
            );
        }
    }
#endif

} // namespace

const RSTFilter* FilterManager::GetRSTFilterForDebug() const noexcept
{
    return rstFilter ? rstFilter.get() : nullptr;
}

void FilterManager::ApplyMode(FilterManagerMode mode)
{
    FilterManagerFlags f{}; // ★全部 false から、各モードで全フラグ明示

    switch (mode) {
    case FilterManagerMode::ChannelScan:
        f.enablePIDAnalyze = true;

        f.enablePAT = true;
        f.enablePMT = true;
        f.enableSDT = true;
        f.enableNIT = true;
        f.enableEIT = false;

        f.enableTDT = false;
        f.enableTOT = false;
        f.enableST = false;

        f.enableCDT = false;
        f.enableCAT = false;
        f.enableBIT = false;
        f.enableRST = false;
        f.enableBAT = false;

        f.enableTSDT = true;
        f.enableDIT = true;
        f.enableSIT = true;

        f.enableNBIT = false;
        break;

    case FilterManagerMode::EpgFullScan:
        f.enablePIDAnalyze = true;

        f.enablePAT = true;
        f.enablePMT = true;
        f.enableSDT = true;
        f.enableNIT = true;
        f.enableEIT = true;

        f.enableTDT = true;
        f.enableTOT = true;
        f.enableST = true;

        f.enableCDT = false;
        f.enableCAT = false;
        f.enableBIT = false;
        f.enableRST = false;
        f.enableBAT = false;

        f.enableTSDT = true;
        f.enableDIT = true;
        f.enableSIT = true;

        f.enableNBIT = true;
        break;

    case FilterManagerMode::RealtimeEpgUpdate:
        f.enablePIDAnalyze = true;

        f.enablePAT = true;
        f.enablePMT = true;
        f.enableSDT = true;
        f.enableNIT = false;
        f.enableEIT = true;

        f.enableTDT = true;
        f.enableTOT = true;
        f.enableST = true;

        f.enableCDT = false;
        f.enableCAT = false;
        f.enableBIT = false;
        f.enableRST = true;
        f.enableBAT = false;

        f.enableTSDT = true;
        f.enableDIT = true;
        f.enableSIT = true;

        f.enableNBIT = true;
        break;

    case FilterManagerMode::LiveView:
        f.enablePIDAnalyze = true;

        f.enablePAT = true;
        f.enablePMT = true;
        f.enableSDT = true;
        f.enableNIT = false;
        f.enableEIT = true;

        f.enableTDT = true;
        f.enableTOT = true;
        f.enableST = true;

        f.enableCDT = false;
        f.enableCAT = false;
        f.enableBIT = false;
        f.enableRST = true;
        f.enableBAT = false;

        f.enableTSDT = true;
        f.enableDIT = true;
        f.enableSIT = true;

        f.enableNBIT = false; // 必要なら true
        break;

    case FilterManagerMode::TsRecordLite:
        f.enablePIDAnalyze = true;

        f.enablePAT = true;
        f.enablePMT = true;
        f.enableSDT = false;
        f.enableNIT = false;
        f.enableEIT = false;

        f.enableTDT = false;
        f.enableTOT = false;
        f.enableST = false;

        f.enableCDT = false;
        f.enableCAT = false;
        f.enableBIT = false;
        f.enableRST = false;
        f.enableBAT = false;

        f.enableTSDT = false;
        f.enableDIT = false;
        f.enableSIT = false;

        f.enableNBIT = false;
        break;

    case FilterManagerMode::SignalMonitor:
        f.enablePIDAnalyze = true;

        f.enablePAT = true;
        f.enablePMT = false;
        f.enableSDT = false;
        f.enableNIT = false;
        f.enableEIT = false;

        f.enableTDT = false;
        f.enableTOT = false;
        f.enableST = false;

        f.enableCDT = false;
        f.enableCAT = false;
        f.enableBIT = false;
        f.enableRST = false;
        f.enableBAT = false;

        f.enableTSDT = false;
        f.enableDIT = false;
        f.enableSIT = false;

        f.enableNBIT = false;
        break;
    }

    SetFlags(f);
    pmtPidsInjected = false;
}

void FilterManager::Initialize()
{
    pidAnalyzer = std::make_unique<AnalyzePID>();

    patFilter = std::make_unique<PATFilter>(parser, pidBuffer);
    pmtFilter = std::make_unique<PMTFilter>(parser, pidBuffer);
    sdtFilter = std::make_unique<SDTFilter>(parser, pidBuffer);
    nitFilter = std::make_unique<NITFilter>(parser, pidBuffer);
    eitFilter = std::make_unique<EITFilter>();

    tdtFilter = std::make_unique<TDTFilter>();
    totFilter = std::make_unique<TOTFilter>();
    cdtFilter = std::make_unique<CDTFilter>();
    catFilter = std::make_unique<CATFilter>();
    bitFilter = std::make_unique<BITFilter>();
    rstFilter = std::make_unique<RSTFilter>();
    batFilter = std::make_unique<BATFilter>();
    tsdtFilter = std::make_unique<TSDTFilter>();
    ditFilter = std::make_unique<DITFilter>();
    sitFilter = std::make_unique<SITFilter>();
    stFilter = std::make_unique<STFilter>();
    nbitFilter = std::make_unique<NBITFilter>();

    patComplete = pmtComplete = sdtComplete = nitComplete = eitComplete = false;
    cdtComplete = catComplete = bitComplete = rstComplete = batComplete = false;

    pmtPidsInjected = false;
    lastPid11TableId = 0xFF;
}

void FilterManager::TryInjectPmtPids_Once()
{
    if (pmtPidsInjected) return;
    if (!m_flags.enablePMT) { pmtPidsInjected = true; return; }
    if (!patFilter || !pmtFilter) return;
    if (!patFilter->IsRecovered()) return;

    const auto pids = patFilter->GetPMTPIDs();
    for (auto pid : pids) pmtFilter->AddTargetPID(pid);

    if (!pids.empty()) {
        DebugLogFormat(L"[FilterManager] Inject PMT PIDs from PAT: count=%zu", pids.size());
        pmtPidsInjected = true;
    }
}

void FilterManager::InputPacket(const std::array<uint8_t, TS_PACKET_SIZE>& pkt)
{
    processedPacketCount++;

    const uint8_t* data = pkt.data();
    const size_t   size = pkt.size();

    if (size < 4 || data[0] != 0x47) return;

    const uint16_t pid = static_cast<uint16_t>(((data[1] & 0x1F) << 8) | data[2]);

#if TSCEPG_TRACE_RST
    if (pid == 0x0013) TraceRST_OnPid13Packet(data, size);
#endif

    const uint8_t scrambling = (data[3] & 0xC0) >> 6;
    if (scrambling != 0) scrambledCount++;

    if (pid == 0x1FFF) {
        nullCount++;
        return;
    }

    if (m_flags.enablePIDAnalyze && pidAnalyzer) {
        pidAnalyzer->ProcessPacket(data, size);
    }

    TryInjectPmtPids_Once();

    switch (pid)
    {
    case 0x0000: // PAT
        patCount++;
        if (m_flags.enablePAT && patFilter) patFilter->InputPacket(pkt);
        break;

    case 0x0001: // CAT
        catCount++;
        if (m_flags.enableCAT && catFilter) catFilter->InputPacket(data, size);
        break;

    case 0x0002: // TSDT
        tsdtCount++;
        if (m_flags.enableTSDT && tsdtFilter) tsdtFilter->InputPacket(data, size);
        break;

    case 0x0010: // NIT
        nitCount++;
        if (m_flags.enableNIT && nitFilter) nitFilter->InputPacket(data, size);
        break;

    case 0x0011: // SDT/BAT
    {
        sdtCount++;

        const auto tidOpt = PeekPsiTableId_OnPusi(data, size);
        if (tidOpt) {
            lastPid11TableId = *tidOpt;
            if (*tidOpt == 0x4A) batCount++; // BAT先頭(PUSI)観測
        }

        const uint8_t tid = lastPid11TableId;
        const bool isBat = (tid == 0x4A);
        const bool routeToBat = isBat && m_flags.enableBAT && batFilter;

        if (!routeToBat) {
            if (sdtRawPacketsBefore.size() < kMaxSdtDumpPackets)
                sdtRawPacketsBefore.push_back(pkt);
        }

        if (routeToBat) {
            batFilter->InputPacket(pkt);
        }
        else {
            if (m_flags.enableSDT && sdtFilter) {
                sdtFilter->InputPacket(pkt);
                if (sdtRawPacketsAfter.size() < kMaxSdtDumpPackets)
                    sdtRawPacketsAfter.push_back(pkt);
            }
        }
        break;
    }

    case 0x0012: // EIT
        eitCount++;
        if (m_flags.enableEIT && eitFilter) eitFilter->InputPacket(pkt);
        break;

    case 0x0013: // RST
        rstCount++;
        if (m_flags.enableRST && rstFilter) {
            rstFilter->InputPacket(data, size);
        }
        break;

    case 0x0014: // TDT/TOT/ST
    {
        tdtTotPidCount++;

        if (m_flags.enableTDT && tdtFilter) tdtFilter->InputPacket(data, size);
        if (m_flags.enableTOT && totFilter) totFilter->InputPacket(data, size);
        if (m_flags.enableST && stFilter)  stFilter->InputPacket(data, size);

        const bool pusi = (data[1] & 0x40) != 0;
        if (!pusi) break;

        tdtTotPusiCount++;

        const uint8_t afc = (data[3] >> 4) & 0x03;
        if ((afc & 0x01) == 0) break;

        size_t p = 4;
        if (afc & 0x02) {
            if (p >= size) break;
            const uint8_t adapt_len = data[p];
            p += 1u + adapt_len;
            if (p >= size) break;
        }

        if (p >= size) break;
        const uint8_t pointer = data[p];
        p += 1u + pointer;
        if (p >= size) break;

        const uint8_t table_id = data[p];
        if (table_id == 0x70) tdtCount++;
        else if (table_id == 0x73) totCount++;
        else if (table_id == 0x72) stCount++;
        break;
    }

    case 0x001E: // DIT
        ditCount++;
        if (m_flags.enableDIT && ditFilter) ditFilter->InputPacket(data, size);
        break;

    case 0x001F: // SIT
        sitCount++;
        if (m_flags.enableSIT && sitFilter) sitFilter->InputPacket(data, size);
        break;

    case 0x0024: // BIT
    {
        bitPidCount++;
        if (m_flags.enableBIT && bitFilter) bitFilter->InputPacket(data, size);

        const auto tidOpt = PeekPsiTableId_OnPusi(data, size);
        if (!tidOpt) break;

        bitPusiCount++;
        if (*tidOpt == 0xC4) bitCount++;
        break;
    }

    case 0x0025: // NBIT (LDT と共用)
    {
        nbitPidCount++;

        if (m_flags.enableNBIT && nbitFilter) {
            nbitFilter->InputPacket(data, size);
        }

        const auto tidOpt = PeekPsiTableId_OnPusi(data, size);
        if (!tidOpt) break;

        nbitPusiCount++;

        const uint8_t tid = *tidOpt;
        if (tid == 0xC5) {
            nbitTableCount++;
            nbitC5Count++;
        }
        else if (tid == 0xC6) {
            nbitTableCount++;
            nbitC6Count++;
        }
        // 0xC7(LDT) 等は “table seen” にしない（GUIが嘘にならない）
        break;
    }

    case 0x0029: // CDT
    {
        cdtPidCount++;
        if (m_flags.enableCDT && cdtFilter) cdtFilter->InputPacket(data, size);

        const auto tidOpt = PeekPsiTableId_OnPusi(data, size);
        if (!tidOpt) break;

        cdtPusiCount++;
        if (*tidOpt == 0xC8) cdtCount++;
        break;
    }

    default:
        if (m_flags.enablePMT && pmtFilter && pmtFilter->IsTargetPID(pid)) {
            pmtCount++;
            pmtFilter->InputPacket(data, size);
        }
        else {
            unmatchedCount++;
            if ((unmatchedCount.load() % 10000) == 0) {
                DebugLogFormat(L"[OTHER] Unmatched PID=0x%04X (total=%zu)", pid, unmatchedCount.load());
            }
        }
        break;
    }

    // recovered 判定（無効は true）
    patComplete = !m_flags.enablePAT || (patFilter && patFilter->IsRecovered());
    pmtComplete = !m_flags.enablePMT || (pmtFilter && pmtFilter->IsRecovered());
    sdtComplete = !m_flags.enableSDT || (sdtFilter && sdtFilter->IsRecovered());
    nitComplete = !m_flags.enableNIT || (nitFilter && nitFilter->IsRecovered());
    eitComplete = !m_flags.enableEIT || (eitFilter && eitFilter->HasValidData());

    cdtComplete = !m_flags.enableCDT || (cdtFilter && cdtFilter->IsRecovered());
    catComplete = !m_flags.enableCAT || (catFilter && catFilter->IsRecovered());
    bitComplete = !m_flags.enableBIT || (bitFilter && bitFilter->IsRecovered());
    rstComplete = !m_flags.enableRST || (rstFilter && rstFilter->IsRecovered());
    batComplete = !m_flags.enableBAT || (batFilter && batFilter->IsRecovered());
}

bool FilterManager::IsAllRecovered() const
{
    // ★時刻系：TDT または TOT のどちらかでOK（両方OFFならOK）
    const bool timeOk =
        (!m_flags.enableTDT && !m_flags.enableTOT) ||
        (m_flags.enableTDT && tdtFilter && tdtFilter->IsRecovered()) ||
        (m_flags.enableTOT && totFilter && totFilter->IsRecovered());

    // ★ST/NBIT は含めない（現状維持）
    return patComplete && pmtComplete && sdtComplete && nitComplete && eitComplete &&
        cdtComplete && timeOk && rstComplete && batComplete;
}

const std::vector<ServiceInfo>& FilterManager::GetServiceList() const
{
    static const std::vector<ServiceInfo> empty;
    return sdtFilter ? sdtFilter->GetServiceList() : empty;
}

const std::vector<NITNetworkInfo>& FilterManager::GetNetworkList() const
{
    static const std::vector<NITNetworkInfo> empty;
    return nitFilter ? nitFilter->GetNetworkList() : empty;
}

const std::vector<ServiceInfo> FilterManager::GetServiceListSorted() const
{
    if (!sdtFilter) return {};
    auto list = sdtFilter->GetServiceList();
    std::sort(list.begin(), list.end(), [](const ServiceInfo& a, const ServiceInfo& b) {
        return a.section_number < b.section_number;
        });
    return list;
}

std::vector<ServiceInfo> FilterManager::GetSortedServiceListBySection() const
{
    if (!sdtFilter) return {};
    return sdtFilter->GetSortedServiceList();
}

size_t FilterManager::GetProcessedPacketCount() const
{
    return processedPacketCount.load();
}

const std::vector<std::array<uint8_t, 188>>& FilterManager::GetSDTPacketsBefore() const
{
    return sdtRawPacketsBefore;
}

const std::vector<std::array<uint8_t, 188>>& FilterManager::GetSDTPacketsAfter() const
{
    return sdtRawPacketsAfter;
}
