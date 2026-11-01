#include "CATFilter.hpp"

#include <algorithm>

static constexpr uint16_t kCAT_PID = 0x0001;
static constexpr uint8_t  kCAT_TABLE_ID = 0x01;

void CATFilter::Reset()
{
    sec_buf.clear();
    expected_len = 0;
    last_cc = -1;

    ca_desc.clear();

    recovered.store(false);
    status.store(Status::NotSeen);
    last_version.store(0);
    last_current.store(false);

    pid_packets.store(0);
    pusi_count.store(0);
    section_ok.store(0);
    crc_fail.store(0);
    discontinuity.store(0);
}

uint32_t CATFilter::ComputePsiCrc32(const uint8_t* data, size_t len) noexcept
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint32_t)data[i] << 24;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x80000000u) crc = (crc << 1) ^ 0x04C11DB7u;
            else crc <<= 1;
        }
    }
    return crc;
}

bool CATFilter::IsCrcOk(const uint8_t* sec, size_t len) noexcept
{
    // PSI/SI のCRCは「全体（CRC含む）で計算して 0 になる」が一般的
    if (!sec || len < 4) return false;
    return ComputePsiCrc32(sec, len) == 0;
}

void CATFilter::InputPacket(const uint8_t* data, size_t size)
{
    if (!data || size < 4) return;
    if (data[0] != 0x47) return;

    const uint16_t pid = (uint16_t)(((data[1] & 0x1F) << 8) | data[2]);
    if (pid != kCAT_PID) return;

    pid_packets++;

    const bool pusi = (data[1] & 0x40) != 0;
    const uint8_t cc = (uint8_t)(data[3] & 0x0F);

    const uint8_t afc = (uint8_t)((data[3] >> 4) & 0x03);
    if ((afc & 0x01) == 0) return; // payload無し

    size_t p = 4;
    if (afc & 0x02) {
        if (p >= size) return;
        const uint8_t adapt_len = data[p];
        p += 1u + adapt_len;
        if (p >= size) return;
    }

    if (p >= size) return;

    // continuity チェック（組み立て中のみ厳密）
    if (!sec_buf.empty() && !pusi) {
        if (last_cc >= 0) {
            const int expect = (last_cc + 1) & 0x0F;
            if ((int)cc != expect) {
                // 途切れたので破棄
                discontinuity++;
                sec_buf.clear();
                expected_len = 0;
            }
        }
    }
    last_cc = cc;

    // PUSI の場合 pointer_field
    if (pusi) {
        pusi_count++;
        const uint8_t pointer = data[p];
        p += 1u;
        if (p + pointer > size) return;
        p += pointer;
        // PUSI で新しいセクションが始まるので、未完成が残ってたら捨てる
        if (!sec_buf.empty()) {
            sec_buf.clear();
            expected_len = 0;
        }
    }

    ConsumePayloadBytes(data + p, size - p, pusi, cc);
}

void CATFilter::ConsumePayloadBytes(const uint8_t* payload, size_t len, bool /*pusi*/, uint8_t /*cc*/)
{
    if (!payload || len == 0) return;

    for (size_t i = 0; i < len; ++i) {
        TryConsumeByte(payload[i]);
        // 1パケット内に複数セクションが入る場合に備え、ここは継続
    }
}

void CATFilter::TryConsumeByte(uint8_t b)
{
    if (expected_len == 0) {
        // まず 3byte（table_id + section_length）
        sec_buf.push_back(b);
        if (sec_buf.size() == 3) {
            const uint16_t sl = (uint16_t)(((sec_buf[1] & 0x0F) << 8) | sec_buf[2]);
            expected_len = 3u + (size_t)sl;

            // 異常長は破棄
            if (expected_len < 8 || expected_len > 1024 + 3) {
                sec_buf.clear();
                expected_len = 0;
            }
        }
        return;
    }

    sec_buf.push_back(b);
    if (sec_buf.size() == expected_len) {
        ProcessSection(sec_buf.data(), sec_buf.size());
        sec_buf.clear();
        expected_len = 0;
    }
}

void CATFilter::ProcessSection(const uint8_t* sec, size_t len)
{
    if (!sec || len < 12) return; // header+CRC最低

    const uint8_t table_id = sec[0];
    if (table_id != kCAT_TABLE_ID) return;

    // section_syntax_indicator は 1
    const bool ssi = (sec[1] & 0x80) != 0;
    if (!ssi) return;

    if (!IsCrcOk(sec, len)) {
        crc_fail++;
        status.store(Status::CrcError);
        return;
    }

    // CAT の基本ヘッダ（MPEG-2 PSI 準拠）
    const uint8_t version = (uint8_t)((sec[5] >> 1) & 0x1F);
    const bool current = (sec[5] & 0x01) != 0;

    // descriptors は [8 .. len-4)
    if (len < 12) return;
    const size_t desc_start = 8;
    const size_t desc_end = len - 4;
    if (desc_end < desc_start) return;

    std::vector<CADescriptor> parsed;
    size_t p = desc_start;
    while (p + 2 <= desc_end) {
        const uint8_t tag = sec[p];
        const uint8_t dlen = sec[p + 1];
        p += 2;

        if (p + dlen > desc_end) break;

        if (tag == 0x09 && dlen >= 4) {
            CADescriptor d{};
            d.ca_system_id = (uint16_t)((sec[p] << 8) | sec[p + 1]);
            const uint16_t pid13 = (uint16_t)(((sec[p + 2] & 0x1F) << 8) | sec[p + 3]);
            d.ca_pid = pid13;

            if (dlen > 4) {
                d.private_data.assign(sec + p + 4, sec + p + dlen);
            }
            parsed.push_back(std::move(d));
        }

        p += dlen;
    }

    ca_desc = std::move(parsed);

    last_version.store(version);
    last_current.store(current);
    recovered.store(true);
    status.store(Status::Recovered);
    section_ok++;
}
