#include "CDTFilter.hpp"

#include <algorithm>

namespace {

    inline uint16_t ReadBE16(const uint8_t* p) noexcept {
        return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
    }

    inline uint32_t ReadBE32(const uint8_t* p) noexcept {
        return (static_cast<uint32_t>(p[0]) << 24) |
            (static_cast<uint32_t>(p[1]) << 16) |
            (static_cast<uint32_t>(p[2]) << 8) |
            (static_cast<uint32_t>(p[3]) << 0);
    }

    // PSI/SI section: total = 3 + section_length
    inline uint16_t ReadSectionLength(const uint8_t* p) noexcept {
        return static_cast<uint16_t>(((p[1] & 0x0F) << 8) | p[2]);
    }

} // namespace

void CDTFilter::Reset()
{
    recovered_ = false;
    stats_ = Stats{};
    asm_.Reset();
    lastSection_.reset();
    dataSets_.clear();
    insertionOrder_.clear();
}

void CDTFilter::InputPacket(const uint8_t* packet, std::size_t size)
{
    stats_.packetCount++;

    if (!packet || size < 4) return;
    if (packet[0] != 0x47) return;

    const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
    if (pid != kPidCDT) return;

    const bool pusi = (packet[1] & 0x40) != 0;
    const uint8_t afc = static_cast<uint8_t>((packet[3] >> 4) & 0x03);
    const uint8_t cc = static_cast<uint8_t>(packet[3] & 0x0F);

    // continuity check（収集中だけ厳密に）
    if (asm_.haveCC && asm_.collecting) {
        const uint8_t expected = static_cast<uint8_t>((asm_.lastCC + 1) & 0x0F);
        if (cc != expected) {
            stats_.continuityErrorCount++;
            asm_.buf.clear();
            asm_.expectTotal = 0;
            asm_.collecting = false;
        }
    }
    asm_.haveCC = true;
    asm_.lastCC = cc;

    // payload位置
    std::size_t pos = 4;
    if (afc == 0 || afc == 2) {
        // no payload
        return;
    }
    if (afc == 3) {
        if (pos >= size) return;
        const uint8_t afl = packet[pos];
        pos += 1u + static_cast<std::size_t>(afl);
        if (pos >= size) return;
    }
    // afc==1: payload only

    stats_.packetWithPayloadCount++;
    if (pusi) stats_.pusiPacketCount++;

    const uint8_t* payload = packet + pos;
    const std::size_t payload_len = size - pos;
    ProcessPayload(payload, payload_len, pusi);
}

void CDTFilter::ProcessPayload(const uint8_t* payload, std::size_t payload_len, bool pusi)
{
    if (!payload || payload_len == 0) return;

    if (!pusi) {
        // PUSI無しで新規セクション開始はしない（収集中だけ継続）
        if (asm_.collecting) {
            StartOrContinueSection(payload, payload_len);
        }
        return;
    }

    // PUSI: pointer_field
    if (payload_len < 1) return;
    const uint8_t pointer = payload[0];

    std::size_t idx = 1;
    if (idx + pointer > payload_len) {
        // 不正 pointer
        stats_.sectionsBadLength++;
        return;
    }

    // 先頭pointer分は「前セクションの残り」になり得る
    if (asm_.collecting && pointer > 0) {
        const std::size_t need = (asm_.expectTotal > asm_.buf.size()) ? (asm_.expectTotal - asm_.buf.size()) : 0;
        const std::size_t take = std::min<std::size_t>(need, pointer);
        if (take > 0) {
            asm_.buf.insert(asm_.buf.end(), payload + idx, payload + idx + take);
        }
        idx += pointer;

        if (asm_.collecting && asm_.expectTotal > 0 && asm_.buf.size() == asm_.expectTotal) {
            ProcessCompletedSection(asm_.buf);
            asm_.buf.clear();
            asm_.expectTotal = 0;
            asm_.collecting = false;
        }
    }
    else {
        idx += pointer; // 破棄
    }

    // idx以降にセクション列
    if (idx >= payload_len) return;
    StartOrContinueSection(payload + idx, payload_len - idx);
}

void CDTFilter::StartOrContinueSection(const uint8_t* bytes, std::size_t len)
{
    if (!bytes || len == 0) return;

    std::size_t idx = 0;

    while (idx < len) {
        if (!asm_.collecting) {
            if (len - idx < 3) {
                // 次パケット待ち（PUSIパケット内に3byte未満は稀だが安全側）
                return;
            }

            const uint16_t section_length = ReadSectionLength(bytes + idx);
            const std::size_t total = static_cast<std::size_t>(3) + section_length;

            // DVB/ARIB PSIの一般的上限は 4093+3 = 4096 程度
            if (total < 7 || total > 4096) { // 7は最低限のガード
                stats_.sectionsBadLength++;
                return;
            }

            asm_.buf.clear();
            asm_.buf.reserve(total);
            asm_.expectTotal = total;
            asm_.collecting = true;
        }

        const std::size_t need = (asm_.expectTotal > asm_.buf.size()) ? (asm_.expectTotal - asm_.buf.size()) : 0;
        if (need == 0) {
            asm_.collecting = false;
            asm_.expectTotal = 0;
            asm_.buf.clear();
            continue;
        }

        const std::size_t take = std::min<std::size_t>(need, len - idx);
        asm_.buf.insert(asm_.buf.end(), bytes + idx, bytes + idx + take);
        idx += take;

        if (asm_.buf.size() == asm_.expectTotal) {
            ProcessCompletedSection(asm_.buf);
            asm_.buf.clear();
            asm_.expectTotal = 0;
            asm_.collecting = false;
            continue;
        }

        // まだ足りない → 次パケットへ
        return;
    }
}

void CDTFilter::ProcessCompletedSection(const std::vector<uint8_t>& section)
{
    stats_.sectionsSeen++;

    if (section.size() < 7) {
        stats_.sectionsBadLength++;
        return;
    }

    const uint8_t table_id = section[0];
    if (table_id != kTableIdCDT) {
        stats_.sectionsNonCdtTableId++;
        return;
    }

    // section_syntax_indicator must be 1 for PSI tables with CRC
    const bool section_syntax_indicator = (section[1] & 0x80) != 0;
    if (!section_syntax_indicator) {
        stats_.sectionsBadSyntax++;
        return;
    }

    const uint16_t section_length = ReadSectionLength(section.data());
    const std::size_t total = static_cast<std::size_t>(3) + section_length;
    if (total != section.size()) {
        stats_.sectionsBadLength++;
        return;
    }
    if (total > 4096) {
        stats_.sectionsBadLength++;
        return;
    }

    // CRC32 check: PSI/SI CRC over whole section should become 0
    if (ComputePsiCrc32(section.data(), section.size()) != 0) {
        stats_.sectionsCrcError++;
        return;
    }

    // ---- parse ----
    CDTSectionInfo info{};
    info.table_id = table_id;
    info.section_length = section_length;

    info.download_data_id = ReadBE16(&section[3]);
    info.version_number = static_cast<uint8_t>((section[5] >> 1) & 0x1F);
    info.current_next = (section[5] & 0x01) != 0;

    info.section_number = section[6];
    info.last_section_number = section[7];

    // CDT body (ARIB B21): original_network_id, reserved, data_type, descriptors_loop_length, ...
    // ここは「reserved 1byte が入る」配置で実装（byte境界で descriptor loop を開始する前提）
    if (section.size() < 8 + 6 + 4) { // header(8) + (onid2 + rsv1 + dt1 + len2)=6 + CRC(4)
        stats_.sectionsBadLength++;
        return;
    }

    info.original_network_id = ReadBE16(&section[8]);
    info.data_type = section[11];

    const uint16_t desc_len = static_cast<uint16_t>(((section[12] & 0x0F) << 8) | section[13]);
    const std::size_t desc_start = 14;
    const std::size_t crc_start = section.size() - 4;

    if (desc_start + desc_len > crc_start) {
        stats_.sectionsBadLength++;
        return;
    }

    info.descriptors.assign(section.begin() + static_cast<std::ptrdiff_t>(desc_start),
        section.begin() + static_cast<std::ptrdiff_t>(desc_start + desc_len));

    info.data_module.assign(section.begin() + static_cast<std::ptrdiff_t>(desc_start + desc_len),
        section.begin() + static_cast<std::ptrdiff_t>(crc_start));

    info.crc32 = ReadBE32(&section[crc_start]);

    // デバッグ用保持
    lastSection_ = info;
    recovered_ = true;
    stats_.sectionsParsedOk++;

    // ---------- DataSet（download_data_id + version + data_type + onid）でセクションを束ねる ----------
    DataSetKey key;
    key.onid = info.original_network_id;
    key.download_id = info.download_data_id;
    key.version = info.version_number;
    key.data_type = info.data_type;

    auto it = dataSets_.find(key);
    if (it == dataSets_.end()) {
        DataSet ds;
        ds.Init(key.onid, key.download_id, key.version, key.data_type, info.last_section_number);
        ds.descriptors = info.descriptors;

        it = dataSets_.emplace(key, std::move(ds)).first;
        insertionOrder_.push_back(key);
        stats_.dataSetsCreated++;

        // eviction
        if (dataSets_.size() > kMaxDataSets && !insertionOrder_.empty()) {
            const DataSetKey old = insertionOrder_.front();
            insertionOrder_.erase(insertionOrder_.begin());
            dataSets_.erase(old);
        }
    }
    else {
        // last_section の変更は再初期化（混線防止）
        if (it->second.last_section != info.last_section_number) {
            it->second.Init(key.onid, key.download_id, key.version, key.data_type, info.last_section_number);
            it->second.descriptors = info.descriptors;
        }
    }

    // セクション登録
    if (!it->second.complete) {
        const bool completed = it->second.MarkSection(info.section_number, info.data_module);
        if (completed) {
            stats_.dataSetsCompleted++;
        }
    }
}

bool CDTFilter::GetCompletedDataSet(uint16_t original_network_id,
    uint16_t download_data_id,
    uint8_t data_type,
    std::vector<uint8_t>& out_data,
    std::vector<uint8_t>* out_desc) const
{
    for (const auto& [key, ds] : dataSets_) {
        if (!ds.complete) continue;
        if (key.onid != original_network_id) continue;
        if (key.download_id != download_data_id) continue;
        if (key.data_type != data_type) continue;

        ds.Build(out_data);
        if (out_desc) *out_desc = ds.descriptors;
        return true;
    }
    return false;
}

uint32_t CDTFilter::ComputePsiCrc32(const uint8_t* data, std::size_t length) noexcept
{
    // polynomial 0x04C11DB7, init 0xFFFFFFFF, XOROUT なし（PSI/SI）
    uint32_t crc = 0xFFFFFFFFu;

    for (std::size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x80000000u) crc = (crc << 1) ^ 0x04C11DB7u;
            else crc <<= 1;
        }
    }

    return crc;
}
