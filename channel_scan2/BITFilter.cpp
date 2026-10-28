#include "BITFilter.hpp"

static constexpr uint16_t kBIT_PID = 0x0024;
static constexpr uint8_t  kBIT_TABLE_ID = 0xC4;

void BITFilter::Reset()
{
    sec_buf.clear();
    expected_len = 0;
    last_cc = -1;

    broadcaster_desc.clear();
    body_rest.clear();

    recovered.store(false);
    status.store(Status::NotSeen);
    broadcaster_id.store(0);
    last_version.store(0);
    last_current.store(false);

    pid_packets.store(0);
    pusi_count.store(0);
    section_ok.store(0);
    crc_fail.store(0);
    discontinuity.store(0);
}

uint32_t BITFilter::ComputePsiCrc32(const uint8_t* data, size_t len) noexcept
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

bool BITFilter::IsCrcOk(const uint8_t* sec, size_t len) noexcept
{
    if (!sec || len < 4) return false;
    return ComputePsiCrc32(sec, len) == 0;
}

void BITFilter::InputPacket(const uint8_t* data, size_t size)
{
    if (!data || size < 4) return;
    if (data[0] != 0x47) return;

    const uint16_t pid = (uint16_t)(((data[1] & 0x1F) << 8) | data[2]);
    if (pid != kBIT_PID) return;

    pid_packets++;

    const bool pusi = (data[1] & 0x40) != 0;
    const uint8_t cc = (uint8_t)(data[3] & 0x0F);

    const uint8_t afc = (uint8_t)((data[3] >> 4) & 0x03);
    if ((afc & 0x01) == 0) return;

    size_t p = 4;
    if (afc & 0x02) {
        if (p >= size) return;
        const uint8_t adapt_len = data[p];
        p += 1u + adapt_len;
        if (p >= size) return;
    }
    if (p >= size) return;

    if (!sec_buf.empty() && !pusi) {
        if (last_cc >= 0) {
            const int expect = (last_cc + 1) & 0x0F;
            if ((int)cc != expect) {
                discontinuity++;
                sec_buf.clear();
                expected_len = 0;
            }
        }
    }
    last_cc = cc;

    if (pusi) {
        pusi_count++;
        const uint8_t pointer = data[p];
        p += 1u;
        if (p + pointer > size) return;
        p += pointer;

        if (!sec_buf.empty()) {
            sec_buf.clear();
            expected_len = 0;
        }
    }

    ConsumePayloadBytes(data + p, size - p, pusi, cc);
}

void BITFilter::ConsumePayloadBytes(const uint8_t* payload, size_t len, bool /*pusi*/, uint8_t /*cc*/)
{
    if (!payload || len == 0) return;
    for (size_t i = 0; i < len; ++i) {
        TryConsumeByte(payload[i]);
    }
}

void BITFilter::TryConsumeByte(uint8_t b)
{
    if (expected_len == 0) {
        sec_buf.push_back(b);
        if (sec_buf.size() == 3) {
            const uint16_t sl = (uint16_t)(((sec_buf[1] & 0x0F) << 8) | sec_buf[2]);
            expected_len = 3u + (size_t)sl;

            if (expected_len < 12 || expected_len > 1024 + 3) {
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

void BITFilter::ProcessSection(const uint8_t* sec, size_t len)
{
    if (!sec || len < 16) return;

    const uint8_t table_id = sec[0];
    if (table_id != kBIT_TABLE_ID) return;

    const bool ssi = (sec[1] & 0x80) != 0;
    if (!ssi) return;

    if (!IsCrcOk(sec, len)) {
        crc_fail++;
        status.store(Status::CrcError);
        return;
    }

    // BIT: table_id_extension を broadcaster_id として扱う（一般PSI互換）
    const uint16_t bid = (uint16_t)((sec[3] << 8) | sec[4]);
    const uint8_t version = (uint8_t)((sec[5] >> 1) & 0x1F);
    const bool current = (sec[5] & 0x01) != 0;

    // ここから先は仕様差が出るので「最初の descriptors_loop だけ確実に抜く」作り
    // [8..] に 12bit descriptors_loop_length が来る前提で処理（ARIB BIT）
    if (len < 12) return;

    size_t p = 8;
    if (p + 2 > len - 4) return;

    const uint16_t loop_len = (uint16_t)(((sec[p] & 0x0F) << 8) | sec[p + 1]);
    p += 2;

    const size_t loop_end = p + loop_len;
    const size_t body_end = len - 4;
    if (loop_end > body_end) return;

    std::vector<Descriptor> descs;
    size_t q = p;
    while (q + 2 <= loop_end) {
        const uint8_t tag = sec[q];
        const uint8_t dlen = sec[q + 1];
        q += 2;
        if (q + dlen > loop_end) break;

        Descriptor d{};
        d.tag = tag;
        d.data.assign(sec + q, sec + q + dlen);
        descs.push_back(std::move(d));

        q += dlen;
    }

    broadcaster_desc = std::move(descs);

    // 残り（もし後続ループ等があるならここに残る）
    body_rest.clear();
    if (loop_end < body_end) {
        body_rest.assign(sec + loop_end, sec + body_end);
    }

    broadcaster_id.store(bid);
    last_version.store(version);
    last_current.store(current);
    recovered.store(true);
    status.store(Status::Recovered);
    section_ok++;
}
