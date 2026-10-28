#pragma once
#pragma once
#ifndef CDT_FILTER_HPP
#define CDT_FILTER_HPP

#include <Windows.h>
#undef max
#undef min

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include <optional>
#include <algorithm> 

// CDT (Common Data Table)
// ARIB STD-B21 (Data Broadcasting), ARIB STD-B10 (Service Information)
// - PID:     0x0029
// - table_id:0xC8
// 主用途: ロゴ等の共通データ伝送（data_type=0x01:ロゴ）

class CDTFilter
{
public:
    static constexpr uint16_t kPidCDT = 0x0029;
    static constexpr uint8_t  kTableIdCDT = 0xC8;

    struct Stats
    {
        uint64_t packetCount = 0;
        uint64_t packetWithPayloadCount = 0;
        uint64_t pusiPacketCount = 0;

        uint64_t continuityErrorCount = 0;

        uint64_t sectionsSeen = 0;
        uint64_t sectionsParsedOk = 0;
        uint64_t sectionsCrcError = 0;
        uint64_t sectionsBadLength = 0;
        uint64_t sectionsBadSyntax = 0;
        uint64_t sectionsNonCdtTableId = 0;

        uint64_t dataSetsCreated = 0;
        uint64_t dataSetsCompleted = 0;
    };

    struct CDTSectionInfo
    {
        uint8_t  table_id = 0;                 // 0xC8
        uint16_t section_length = 0;

        uint16_t download_data_id = 0;         // table_id_extension
        uint8_t  version_number = 0;
        bool     current_next = false;

        uint8_t  section_number = 0;
        uint8_t  last_section_number = 0;

        // ARIB STD-B21: CDT body
        uint16_t original_network_id = 0;
        uint8_t  data_type = 0;

        std::vector<uint8_t> descriptors;
        std::vector<uint8_t> data_module;

        uint32_t crc32 = 0;
    };

    CDTFilter() = default;

    void Reset();

    // TS packet input（188/204 どちらでも可。payload解析は188前提ではない）
    void InputPacket(const uint8_t* packet, std::size_t size);

    bool IsRecovered() const noexcept { return recovered_; }

    const Stats& GetStats() const noexcept { return stats_; }

    std::optional<CDTSectionInfo> GetLastSection() const { return lastSection_; }

    // 完成済み DataSet を取得（必要なら out_desc で descriptors も取れる）
    // data_type は代表用途だと 0x01 がロゴ（ARIB記述）
    bool GetCompletedDataSet(uint16_t original_network_id,
        uint16_t download_data_id,
        uint8_t data_type,
        std::vector<uint8_t>& out_data,
        std::vector<uint8_t>* out_desc = nullptr) const;

private:
    // DataSet識別キー（同じPID上で混線しない単位）
    struct DataSetKey
    {
        uint16_t onid = 0;
        uint16_t download_id = 0;
        uint8_t  version = 0;
        uint8_t  data_type = 0;

        friend bool operator==(const DataSetKey& a, const DataSetKey& b) noexcept
        {
            return a.onid == b.onid &&
                a.download_id == b.download_id &&
                a.version == b.version &&
                a.data_type == b.data_type;
        }
    };

    struct DataSetKeyHash
    {
        std::size_t operator()(const DataSetKey& k) const noexcept
        {
            // 16+16+8+8 = 48bit を詰めて hash
            const uint64_t v =
                (static_cast<uint64_t>(k.onid) << 32) |
                (static_cast<uint64_t>(k.download_id) << 16) |
                (static_cast<uint64_t>(k.version) << 8) |
                (static_cast<uint64_t>(k.data_type));
            return static_cast<std::size_t>(v ^ (v >> 23) ^ (v >> 41));
        }
    };

    struct DataSet
    {
        uint16_t onid = 0;
        uint16_t download_id = 0;
        uint8_t  version = 0;
        uint8_t  data_type = 0;

        uint8_t  last_section = 0;

        std::vector<uint8_t> descriptors;

        std::vector<bool> got;
        std::vector<std::vector<uint8_t>> modules;

        bool complete = false;

        void Init(uint16_t onid_, uint16_t did_, uint8_t ver_, uint8_t dt_, uint8_t last)
        {
            onid = onid_;
            download_id = did_;
            version = ver_;
            data_type = dt_;
            last_section = last;

            got.assign(static_cast<std::size_t>(last_section) + 1, false);
            modules.assign(static_cast<std::size_t>(last_section) + 1, {});
            complete = false;
        }

        bool MarkSection(uint8_t secno, const std::vector<uint8_t>& data)
        {
            if (secno > last_section) return false;
            const std::size_t idx = static_cast<std::size_t>(secno);
            got[idx] = true;
            modules[idx] = data;

            // complete判定
            if (!complete) {
                complete = std::all_of(got.begin(), got.end(), [](bool v) { return v; });
            }
            return complete;
        }

        void Build(std::vector<uint8_t>& out) const
        {
            out.clear();
            if (!complete) return;
            std::size_t total = 0;
            for (const auto& m : modules) total += m.size();
            out.reserve(total);
            for (const auto& m : modules) out.insert(out.end(), m.begin(), m.end());
        }
    };

    struct Assembler
    {
        std::vector<uint8_t> buf;
        std::size_t expectTotal = 0;
        bool collecting = false;

        uint8_t lastCC = 0xFF;
        bool haveCC = false;

        void Reset()
        {
            buf.clear();
            expectTotal = 0;
            collecting = false;
            haveCC = false;
            lastCC = 0xFF;
        }
    };

private:
    // payload処理（PUSI/ pointer_field 対応）
    void ProcessPayload(const uint8_t* payload, std::size_t payload_len, bool pusi);
    void StartOrContinueSection(const uint8_t* bytes, std::size_t len);

    void ProcessCompletedSection(const std::vector<uint8_t>& section);

    static uint32_t ComputePsiCrc32(const uint8_t* data, std::size_t length) noexcept;

private:
    static constexpr std::size_t kMaxDataSets = 64;

    bool recovered_ = false;

    Stats stats_{};
    Assembler asm_{};

    std::optional<CDTSectionInfo> lastSection_{};

    std::unordered_map<DataSetKey, DataSet, DataSetKeyHash> dataSets_{};
    std::vector<DataSetKey> insertionOrder_; // eviction簡易用
};

#endif // CDT_FILTER_HPP
