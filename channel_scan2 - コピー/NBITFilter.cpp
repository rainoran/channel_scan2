#include "NBITFilter.hpp"

#include <algorithm>

namespace {
    static uint32_t ComputePsiCrc32(const uint8_t* data, std::size_t length) noexcept
    {
        uint32_t crc = 0xFFFFFFFFu;
        for (std::size_t i = 0; i < length; ++i) {
            crc ^= (uint32_t)data[i] << 24;
            for (int b = 0; b < 8; ++b) {
                crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u) : (crc << 1);
            }
        }
        return crc;
    }

    static uint32_t ReadU32BE(const uint8_t* p) noexcept
    {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }

    static bool ReadTsHeaderBasic(const uint8_t* data, std::size_t size,
        uint16_t& pid, bool& pusi, uint8_t& afc, uint8_t& cc,
        std::size_t& payloadPos) noexcept
    {
        if (!data || size < 4) return false;
        if (data[0] != 0x47) return false;

        pusi = (data[1] & 0x40) != 0;
        pid = (uint16_t)(((data[1] & 0x1F) << 8) | data[2]);

        const uint8_t scrambling = (data[3] & 0xC0) >> 6;
        if (scrambling != 0) return false;

        afc = (data[3] >> 4) & 0x03;
        cc = (data[3] & 0x0F);

        payloadPos = 4;
        if (afc & 0x02) {
            if (payloadPos >= size) return false;
            const uint8_t adaptLen = data[payloadPos];
            payloadPos += 1u + adaptLen;
            if (payloadPos > size) return false;
        }
        if ((afc & 0x01) == 0) return false; // payload 無し
        if (payloadPos >= size) return false;
        return true;
    }

    static bool IsNbitTableId(uint8_t tid) noexcept
    {
        return (tid == 0xC5) || (tid == 0xC6);
    }
}

void NBITFilter::Reset() noexcept
{
    secBuf.clear();
    assembling = false;
    lastCC = 0xFF;
    expectedTotal = 0;

    recovered.store(false, std::memory_order_release);
    packetCount.store(0, std::memory_order_relaxed);
    pusiCount.store(0, std::memory_order_relaxed);
    sectionCount.store(0, std::memory_order_relaxed);
    invalidCount.store(0, std::memory_order_relaxed);
    crcErrorCount.store(0, std::memory_order_relaxed);
    ccErrorCount.store(0, std::memory_order_relaxed);
    ignoredTableCount.store(0, std::memory_order_relaxed);

    lastSnapshot = Snapshot{};
}

void NBITFilter::InputPacket(const uint8_t* data, std::size_t size)
{
    packetCount.fetch_add(1, std::memory_order_relaxed);

    uint16_t pid = 0;
    bool pusi = false;
    uint8_t afc = 0, cc = 0;
    std::size_t p = 0;
    if (!ReadTsHeaderBasic(data, size, pid, pusi, afc, cc, p)) return;

    if (pid != 0x0025) return; // NBIT PID（LDT と共用）

    if (pusi) pusiCount.fetch_add(1, std::memory_order_relaxed);

    if (pusi) {
        if (p >= size) return;

        const uint8_t pointer = data[p];
        p += 1u;
        if (p + pointer > size) {
            assembling = false;
            secBuf.clear();
            expectedTotal = 0;
            lastCC = 0xFF;
            return;
        }

        // pointer で前セクション継続を埋める
        if (assembling && pointer > 0 && expectedTotal > 0) {
            const std::size_t can = (std::min<std::size_t>)(pointer, size - p);
            secBuf.insert(secBuf.end(), data + p, data + p + can);
            if (secBuf.size() >= expectedTotal) {
                OnSection(secBuf.data(), expectedTotal);
                assembling = false;
                secBuf.clear();
                expectedTotal = 0;
            }
        }

        p += pointer;
        if (p + 3 > size) return;

        const uint8_t tid = data[p];
        if (!IsNbitTableId(tid)) {
            // LDT(0xC7) 等は無視（invalid にしない）
            ignoredTableCount.fetch_add(1, std::memory_order_relaxed);
            assembling = false;
            secBuf.clear();
            expectedTotal = 0;
            lastCC = 0xFF;
            return;
        }

        assembling = true;
        secBuf.clear();
        lastCC = cc;

        const uint16_t section_length = (uint16_t)(((data[p + 1] & 0x0F) << 8) | data[p + 2]);
        expectedTotal = 3u + section_length;

        secBuf.insert(secBuf.end(), data + p, data + size);

        if (secBuf.size() >= expectedTotal) {
            OnSection(secBuf.data(), expectedTotal);
            assembling = false;
            secBuf.clear();
            expectedTotal = 0;
        }
    }
    else {
        if (!assembling || expectedTotal == 0) return;

        const uint8_t expect = uint8_t((lastCC + 1) & 0x0F);
        if (cc != expect) {
            ccErrorCount.fetch_add(1, std::memory_order_relaxed);
            assembling = false;
            secBuf.clear();
            expectedTotal = 0;
            lastCC = 0xFF;
            return;
        }
        lastCC = cc;

        secBuf.insert(secBuf.end(), data + p, data + size);

        if (secBuf.size() >= expectedTotal) {
            OnSection(secBuf.data(), expectedTotal);
            assembling = false;
            secBuf.clear();
            expectedTotal = 0;
        }
    }
}

void NBITFilter::OnSection(const uint8_t* sec, std::size_t len)
{
    if (!sec || len < 12) {
        invalidCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    sectionCount.fetch_add(1, std::memory_order_relaxed);

    const uint8_t table_id = sec[0];
    if (!IsNbitTableId(table_id)) {
        ignoredTableCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const bool ssi = (sec[1] & 0x80) != 0;
    const uint16_t section_length = (uint16_t)(((sec[1] & 0x0F) << 8) | sec[2]);
    const std::size_t total = 3u + section_length;
    if (total != len) {
        invalidCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // CRC（ssi==1 の long section を想定）
    if (ssi && len >= 8) {
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
