#include "SITFilter.hpp"
#include <algorithm>

namespace {

    constexpr uint16_t kPidSIT = 0x001F;  // ★ ARIB資料側の値
    constexpr std::size_t kTsSize = 188;
    constexpr std::size_t kMaxSection = 1024 + 3;

    static uint32_t ComputePsiCrc32(const uint8_t* data, std::size_t length) noexcept
    {
        uint32_t crc = 0xFFFFFFFFu;
        for (std::size_t i = 0; i < length; ++i) {
            crc ^= (uint32_t)data[i] << 24;
            for (int b = 0; b < 8; ++b)
                crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u) : (crc << 1);
        }
        return crc;
    }

    static uint32_t ReadU32BE(const uint8_t* p) noexcept
    {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }

    static bool ReadTsHeaderBasic188(const uint8_t* data, std::size_t size,
        uint16_t& pid, bool& pusi, uint8_t& afc, uint8_t& cc,
        bool& discontinuity, std::size_t& payloadPos, std::size_t& packetSize) noexcept
    {
        discontinuity = false;
        if (!data) return false;

        packetSize = (size >= kTsSize) ? kTsSize : size;
        if (packetSize < 4) return false;
        if (data[0] != 0x47) return false;

        pusi = (data[1] & 0x40) != 0;
        pid = (uint16_t)(((data[1] & 0x1F) << 8) | data[2]);

        const uint8_t scrambling = (data[3] & 0xC0) >> 6;
        if (scrambling != 0) return false;

        afc = (data[3] >> 4) & 0x03;
        cc = data[3] & 0x0F;

        payloadPos = 4;

        if (afc & 0x02) {
            if (payloadPos >= packetSize) return false;
            const uint8_t adaptLen = data[payloadPos];
            if (payloadPos + 1u + adaptLen > packetSize) return false;

            if (adaptLen >= 1) {
                const uint8_t flags = data[payloadPos + 1];
                discontinuity = (flags & 0x80) != 0;
            }

            payloadPos += 1u + adaptLen;
        }

        if ((afc & 0x01) == 0) return false;
        if (payloadPos >= packetSize) return false;

        return true;
    }

} // namespace

void SITFilter::Reset() noexcept
{
    secBuf.clear();
    assembling = false;
    lastCC = 0xFF;
    expectedTotal = 0;
    recovered.store(false, std::memory_order_release);
    lastSnapshot = Snapshot{};
}

void SITFilter::InputPacket(const uint8_t* data, std::size_t size)
{
    packetCount.fetch_add(1, std::memory_order_relaxed);

    uint16_t pid = 0;
    bool pusi = false;
    uint8_t afc = 0, cc = 0;
    bool discontinuity = false;
    std::size_t p = 0, pktSize = 0;

    if (!ReadTsHeaderBasic188(data, size, pid, pusi, afc, cc, discontinuity, p, pktSize))
        return;

    if (pid != kPidSIT) return;

    if (discontinuity) {
        assembling = false;
        secBuf.clear();
        expectedTotal = 0;
        lastCC = 0xFF;
    }

    if (pusi) pusiCount.fetch_add(1, std::memory_order_relaxed);

    if (pusi) {
        if (p >= pktSize) return;
        const uint8_t pointer = data[p++];
        if (p + pointer > pktSize) {
            assembling = false; secBuf.clear(); expectedTotal = 0; lastCC = 0xFF;
            return;
        }

        if (assembling && pointer > 0 && expectedTotal > 0) {
            const std::size_t can = (std::min<std::size_t>)(pointer, pktSize - p);
            secBuf.insert(secBuf.end(), data + p, data + p + can);

            if (secBuf.size() >= expectedTotal) {
                OnSection(secBuf.data(), expectedTotal);
                assembling = false;
                secBuf.clear();
                expectedTotal = 0;
            }
        }

        p += pointer;

        while (p + 3 <= pktSize) {
            if (data[p] == 0xFF) break;

            const uint16_t section_length =
                (uint16_t)(((data[p + 1] & 0x0F) << 8) | data[p + 2]);
            const std::size_t total = 3u + section_length;

            if (total < 12 || total > kMaxSection) {
                invalidCount.fetch_add(1, std::memory_order_relaxed);
                break;
            }

            const std::size_t remain = pktSize - p;
            if (remain >= total) {
                OnSection(data + p, total);
                p += total;
                continue;
            }

            assembling = true;
            secBuf.assign(data + p, data + pktSize);
            expectedTotal = total;
            lastCC = cc;
            break;
        }

        return;
    }

    if (!assembling || expectedTotal == 0) return;

    const uint8_t expect = uint8_t((lastCC + 1) & 0x0F);
    if (cc != expect) {
        ccErrorCount.fetch_add(1, std::memory_order_relaxed);
        assembling = false; secBuf.clear(); expectedTotal = 0; lastCC = 0xFF;
        return;
    }
    lastCC = cc;

    secBuf.insert(secBuf.end(), data + p, data + pktSize);

    if (secBuf.size() >= expectedTotal) {
        OnSection(secBuf.data(), expectedTotal);
        assembling = false;
        secBuf.clear();
        expectedTotal = 0;
    }
}

void SITFilter::OnSection(const uint8_t* sec, std::size_t len)
{
    if (!sec || len < 12) { invalidCount.fetch_add(1, std::memory_order_relaxed); return; }

    sectionCount.fetch_add(1, std::memory_order_relaxed);

    const uint8_t table_id = sec[0];
    if (table_id != 0x7F) { invalidCount.fetch_add(1, std::memory_order_relaxed); return; }

    const bool ssi = (sec[1] & 0x80) != 0;
    const uint16_t section_length = (uint16_t)(((sec[1] & 0x0F) << 8) | sec[2]);
    const std::size_t total = 3u + section_length;
    if (total != len) { invalidCount.fetch_add(1, std::memory_order_relaxed); return; }

    // CRC（ssi==1 の場合）
    if (ssi) {
        if (len < 8) { invalidCount.fetch_add(1, std::memory_order_relaxed); return; }
        const uint32_t expected = ReadU32BE(sec + (len - 4));
        const uint32_t computed = ComputePsiCrc32(sec, len - 4);
        if (computed != expected) {
            crcErrorCount.fetch_add(1, std::memory_order_relaxed);
            invalidCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    Snapshot snap{};
    snap.valid = true;
    snap.table_id = table_id;
    snap.section_length = section_length;

    snap.table_id_ext = (uint16_t)((sec[3] << 8) | sec[4]);
    snap.version = (uint8_t)((sec[5] >> 1) & 0x1F);
    snap.current_next = (sec[5] & 0x01) != 0;
    snap.section_number = sec[6];
    snap.last_section_number = sec[7];

    lastSnapshot = snap;
    recovered.store(true, std::memory_order_release);
}
