// FilterManager.hpp（TSParser/PIDDataBuffer 対応）
#pragma once
#ifndef FILTER_MANAGER_HPP
#define FILTER_MANAGER_HPP

#include <Windows.h>
#include <vector>
#include <memory>
#include <cstdint>
#include <array>
#include <atomic>
#include <chrono>
#include <algorithm>

#include "TSBufferConstants.hpp"

#include "AnalyzePID.hpp"
#include "TSParser.h"
#include "TSMPMCRingBuffer.hpp"

#include "PATFilter.hpp"
#include "PMTFilter.hpp"
#include "SDTFilter.hpp"
#include "NITFilter.hpp"
#include "EITFilter.hpp"
#include "TOTFilter.hpp"
#include "TDTFilter.hpp"
#include "CDTFilter.hpp"
#include "CATFilter.hpp"
#include "BITFilter.hpp"
#include "RSTFilter.hpp"
#include "BATFilter.hpp"
#include "TSDTFilter.hpp"
#include "DITFilter.hpp"
#include "SITFilter.hpp"
#include "STFilter.hpp"
#include "NBITFilter.hpp"

#include "NITNetworkInfo.hpp"
#include "ServiceInfo.hpp"

class TSParser;
class PIDDataBuffer;

// FilterManager が扱うモード
enum class FilterManagerMode {
    ChannelScan,
    EpgFullScan,
    RealtimeEpgUpdate,
    LiveView,
    TsRecordLite,
    SignalMonitor
};

// FilterManagerFlags（デフォルトは全部 false：ApplyMode で必ず明示する）
struct FilterManagerFlags
{
    bool enablePIDAnalyze = false;

    bool enablePAT = false;
    bool enablePMT = false;
    bool enableSDT = false;
    bool enableNIT = false;
    bool enableEIT = false;

    bool enableTDT = false;
    bool enableTOT = false;
    bool enableST = false;

    bool enableCDT = false;
    bool enableCAT = false;
    bool enableBIT = false;
    bool enableRST = false;
    bool enableBAT = false;

    bool enableTSDT = false;
    bool enableDIT = false;
    bool enableSIT = false;

    bool enableNBIT = false;
};

class FilterManager
{
public:
    FilterManager() = delete;
    explicit FilterManager(TSParser* parser, PIDDataBuffer* pidBuffer = nullptr)
        : parser(parser), pidBuffer(pidBuffer)
    {
        Initialize();
        ApplyMode(FilterManagerMode::ChannelScan);
    }

    ~FilterManager();

    void Rebind(TSParser* p, PIDDataBuffer* b = nullptr) {
        parser = p;
        pidBuffer = b;
        Initialize();
        ApplyMode(FilterManagerMode::ChannelScan);
    }

    void ClearDebugBuffers() {
        sdtRawPacketsBefore.clear();
        sdtRawPacketsAfter.clear();
    }

    void SetBufferManager(TSParser* p, PIDDataBuffer* b) noexcept {
        parser = p;
        pidBuffer = b;
    }

    // ★ST/NBIT は IsAllRecovered に含めない。GUI等で補助ステータス表示用。
    bool IsAuxRecovered() const
    {
        const bool stOk = (!m_flags.enableST) || (stFilter && stFilter->IsRecovered());
        const bool nbitOk = (!m_flags.enableNBIT) || (nbitFilter && nbitFilter->IsRecovered());
        return stOk && nbitOk;
    }

    void InputPacket(const std::array<uint8_t, TS_PACKET_SIZE>& pkt);

    void SetFlags(const FilterManagerFlags& flags) { m_flags = flags; }
    const FilterManagerFlags& GetFlags() const { return m_flags; }

    void ApplyMode(FilterManagerMode mode);
    void SetChannelScanMode() { ApplyMode(FilterManagerMode::ChannelScan); }
    void SetEpgFullScanMode() { ApplyMode(FilterManagerMode::EpgFullScan); }
    void SetRealtimeEpgUpdateMode() { ApplyMode(FilterManagerMode::RealtimeEpgUpdate); }
    void SetLiveViewMode() { ApplyMode(FilterManagerMode::LiveView); }

    bool IsAllRecovered() const; // ★ST/NBIT は含めない（現状維持）

    bool IsPATRecovered() const { return patFilter && patFilter->IsRecovered(); }
    bool IsPMTRecovered() const { return pmtFilter && pmtFilter->IsRecovered(); }
    bool IsSDTRecovered() const { return sdtFilter && sdtFilter->IsRecovered(); }
    bool IsNITRecovered() const { return nitFilter && nitFilter->IsRecovered(); }
    bool IsEITRecovered() const { return eitFilter && eitFilter->HasValidData(); }
    bool IsTOTRecovered() const { return totFilter && totFilter->IsRecovered(); }
    bool IsTDTRecovered() const { return tdtFilter && tdtFilter->IsRecovered(); }
    bool IsCDTRecovered() const { return cdtFilter && cdtFilter->IsRecovered(); }
    bool IsCATRecovered() const { return catFilter && catFilter->IsRecovered(); }
    bool IsBITRecovered() const { return bitFilter && bitFilter->IsRecovered(); }
    bool IsRSTRecovered() const { return rstFilter && rstFilter->IsRecovered(); }
    bool IsBATRecovered() const { return batFilter && batFilter->IsRecovered(); }
    bool IsTSDTRecovered() const { return tsdtFilter && tsdtFilter->IsRecovered(); }
    bool IsDITRecovered()  const { return ditFilter && ditFilter->IsRecovered(); }
    bool IsSITRecovered()  const { return sitFilter && sitFilter->IsRecovered(); }
    bool IsSTRecovered()   const { return stFilter && stFilter->IsRecovered(); }
    bool IsNBITRecovered() const { return nbitFilter && nbitFilter->IsRecovered(); }

    const std::vector<ServiceInfo>& GetServiceList() const;
    const std::vector<NITNetworkInfo>& GetNetworkList() const;

    const std::vector<ServiceInfo> GetServiceListSorted() const;
    std::vector<ServiceInfo> GetSortedServiceListBySection() const;

    const EITFilter* GetEITFilterForDebug() const noexcept { return eitFilter.get(); }

    double GetEITCoverageRatio(uint16_t onid, uint16_t tsid, uint16_t sid, uint8_t table_id) const
    {
        if (!eitFilter) return 0.0;
        return eitFilter->GetSectionCoverageRatio(onid, tsid, sid, table_id);
    }

    TDTFilter::TimeInfo GetLastTDTTime() const {
        return tdtFilter ? tdtFilter->GetLastTime() : TDTFilter::TimeInfo{};
    }
    TOTFilter::TimeInfo GetLastTOTTime() const {
        return totFilter ? totFilter->GetLastTime() : TOTFilter::TimeInfo{};
    }

    bool IsEITSegmentComplete(uint16_t onid, uint16_t tsid, uint16_t sid, uint8_t table_id, double threshold = 0.8) const
    {
        if (!eitFilter) return false;
        return eitFilter->IsEpgSegmentComplete(onid, tsid, sid, table_id, threshold);
    }

    // ---------------- カウント ----------------
    size_t GetProcessedPacketCount() const;

    size_t GetPATCount() const { return patCount.load(); }
    size_t GetPMTCount() const { return pmtCount.load(); }
    size_t GetSDTCount() const { return sdtCount.load(); }
    size_t GetNITCount() const { return nitCount.load(); }

    size_t GetCATCount() const { return catCount.load(); }
    size_t GetTSDTCount() const { return tsdtCount.load(); }
    size_t GetBATCount() const { return batCount.load(); }
    size_t GetEITCount() const { return eitCount.load(); }

    size_t GetTDTCount() const { return tdtCount.load(); }           // table_id=0x70 見えた回数
    size_t GetTOTCount() const { return totCount.load(); }           // table_id=0x73 見えた回数
    size_t GetSTCount()  const { return stCount.load(); }            // table_id=0x72 見えた回数（ST）

    size_t GetRSTCount() const { return rstCount.load(); }
    size_t GetDITCount() const { return ditCount.load(); }
    size_t GetSITCount() const { return sitCount.load(); }

    size_t GetNullCount() const { return nullCount.load(); }
    size_t GetUnmatchedCount() const { return unmatchedCount.load(); }
    size_t GetScrambledCount() const { return scrambledCount.load(); }

    size_t GetTdtTotPidCount()  const { return tdtTotPidCount.load(); }   // PID=0x0014 パケット数
    size_t GetTdtTotPusiCount() const { return tdtTotPusiCount.load(); }  // PID=0x0014 PUSI 数

    size_t GetCDTCount() const { return cdtCount.load(); }           // table_id=0xC8
    size_t GetCDTPidCount() const { return cdtPidCount.load(); }     // PID=0x0029
    size_t GetCDTPusiCount() const { return cdtPusiCount.load(); }   // PID=0x0029 PUSI

    // BIT
    size_t GetBITCount() const { return bitCount.load(); }           // table_id=0xC4
    size_t GetBITPidCount() const { return bitPidCount.load(); }     // PID=0x0024
    size_t GetBITPusiCount() const { return bitPusiCount.load(); }   // PID=0x0024 PUSI

    // ★NBIT（分離）
    size_t GetNBITPidCount()  const { return nbitPidCount.load(); }   // PID=0x0025 パケット数
    size_t GetNBITPusiCount() const { return nbitPusiCount.load(); }  // PID=0x0025 PUSI 数
    size_t GetNBITCount()     const { return nbitTableCount.load(); } // table_id=0xC5/0xC6 を見た回数（合計）
    size_t GetNBITC5Count()   const { return nbitC5Count.load(); }    // table_id=0xC5
    size_t GetNBITC6Count()   const { return nbitC6Count.load(); }    // table_id=0xC6

    const std::vector<std::array<uint8_t, 188>>& GetSDTPacketsBefore() const;
    const std::vector<std::array<uint8_t, 188>>& GetSDTPacketsAfter() const;

    const std::vector<ParsedEITPacket>& GetEITEvents() const {
        static const std::vector<ParsedEITPacket> empty;
        return eitFilter ? eitFilter->GetEvents() : empty;
    }

    const RSTFilter* GetRSTFilterForDebug() const noexcept;



private:
    void TryInjectPmtPids_Once();
    void Initialize();

private:
    TSParser* parser = nullptr;
    PIDDataBuffer* pidBuffer = nullptr;

    uint8_t lastPid11TableId = 0xFF;

    std::unique_ptr<AnalyzePID> pidAnalyzer;
    std::unique_ptr<PATFilter> patFilter;
    std::unique_ptr<PMTFilter> pmtFilter;
    std::unique_ptr<SDTFilter> sdtFilter;
    std::unique_ptr<NITFilter> nitFilter;
    std::unique_ptr<EITFilter> eitFilter;
    std::unique_ptr<TDTFilter> tdtFilter;
    std::unique_ptr<TOTFilter> totFilter;
    std::unique_ptr<CDTFilter> cdtFilter;
    std::unique_ptr<CATFilter> catFilter;
    std::unique_ptr<BITFilter> bitFilter;
    std::unique_ptr<RSTFilter> rstFilter;
    std::unique_ptr<BATFilter> batFilter;
    std::unique_ptr<TSDTFilter> tsdtFilter;
    std::unique_ptr<DITFilter> ditFilter;
    std::unique_ptr<SITFilter> sitFilter;
    std::unique_ptr<STFilter> stFilter;
    std::unique_ptr<NBITFilter> nbitFilter;

    FilterManagerFlags m_flags{};

    bool patComplete = false;
    bool pmtComplete = false;
    bool sdtComplete = false;
    bool nitComplete = false;
    bool eitComplete = false;
    bool cdtComplete = false;
    bool catComplete = false;
    bool bitComplete = false;
    bool rstComplete = false;
    bool batComplete = false;

    bool pmtPidsInjected = false;

    std::atomic<size_t> processedPacketCount{ 0 };

    std::atomic<size_t> patCount{ 0 };
    std::atomic<size_t> pmtCount{ 0 };
    std::atomic<size_t> sdtCount{ 0 };
    std::atomic<size_t> nitCount{ 0 };

    std::atomic<size_t> catCount{ 0 };
    std::atomic<size_t> tsdtCount{ 0 };
    std::atomic<size_t> batCount{ 0 };
    std::atomic<size_t> eitCount{ 0 };

    std::atomic<size_t> tdtCount{ 0 };
    std::atomic<size_t> totCount{ 0 };
    std::atomic<size_t> stCount{ 0 };

    std::atomic<size_t> rstCount{ 0 };
    std::atomic<size_t> ditCount{ 0 };
    std::atomic<size_t> sitCount{ 0 };

    std::atomic<size_t> nullCount{ 0 };
    std::atomic<size_t> tdtTotPidCount{ 0 };
    std::atomic<size_t> tdtTotPusiCount{ 0 };
    std::atomic<size_t> unmatchedCount{ 0 };
    std::atomic<size_t> scrambledCount{ 0 };

    // CDT
    std::atomic<size_t> cdtCount{ 0 };
    std::atomic<size_t> cdtPidCount{ 0 };
    std::atomic<size_t> cdtPusiCount{ 0 };

    // BIT
    std::atomic<size_t> bitCount{ 0 };
    std::atomic<size_t> bitPidCount{ 0 };
    std::atomic<size_t> bitPusiCount{ 0 };

    // ★NBIT（分離）
    std::atomic<size_t> nbitPidCount{ 0 };
    std::atomic<size_t> nbitPusiCount{ 0 };
    std::atomic<size_t> nbitTableCount{ 0 }; // C5/C6 合計（table見えた回数）
    std::atomic<size_t> nbitC5Count{ 0 };
    std::atomic<size_t> nbitC6Count{ 0 };

    std::vector<std::array<uint8_t, 188>> sdtRawPacketsBefore;
    std::vector<std::array<uint8_t, 188>> sdtRawPacketsAfter;
};

#endif // FILTER_MANAGER_HPP
