// DITFilter.cpp
#include "DITFilter.hpp"

namespace {
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
        cc = data[3] & 0x0F;

        payloadPos = 4;
        if (afc & 0x02) {
            if (payloadPos >= size) return false;
            const uint8_t adaptLen = data[payloadPos];
            payloadPos += 1u + adaptLen;
            if (payloadPos > size) return false;
        }
        if ((afc & 0x01) == 0) return false; // payload無し
        if (payloadPos >= size) return false;
        return true;
    }

    static constexpr uint16_t kDitPid = 0x001E; // ★修正：DIT PID
}

void DITFilter::Reset() noexcept
{
    secBuf.clear();
    assembling = false;
    lastCC = 0xFF;
    expectedTotal = 0;
    recovered.store(false, std::memory_order_release);
    lastSnapshot = Snapshot{};
}

void DITFilter::InputPacket(const uint8_t* data, std::size_t size)
{
    packetCount.fetch_add(1, std::memory_order_relaxed);

    uint16_t pid = 0;
    bool pusi = false;
    uint8_t afc = 0, cc = 0;
    std::size_t p = 0;
    if (!ReadTsHeaderBasic(data, size, pid, pusi, afc, cc, p)) return;

    if (pid != kDitPid) return; // ★修正

    if (pusi) pusiCount.fetch_add(1, std::memory_order_relaxed);

    if (pusi) {
        if (p >= size) { assembling = false; secBuf.clear(); expectedTotal = 0; return; }

        const uint8_t pointer = data[p];
        p += 1u;
        if (p + pointer > size) { assembling = false; secBuf.clear(); expectedTotal = 0; return; }

        // pointerで前セクション継続を埋める（簡易）
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

        if (p + 3 <= size) {
            assembling = true;
            secBuf.clear();
            expectedTotal = 0;
            lastCC = cc;

            const uint16_t section_length = (uint16_t)(((data[p + 1] & 0x0F) << 8) | data[p + 2]);
            expectedTotal = 3u + section_length;

            secBuf.reserve(expectedTotal);
            secBuf.insert(secBuf.end(), data + p, data + size);

            if (expectedTotal > 0 && secBuf.size() >= expectedTotal) {
                OnSection(secBuf.data(), expectedTotal);
                assembling = false;
                secBuf.clear();
                expectedTotal = 0;
            }
        }
        else {
            // 新規セクション開始に必要な3バイトが無い
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

void DITFilter::OnSection(const uint8_t* sec, std::size_t len)
{
    if (!sec || len < 4) { invalidCount.fetch_add(1, std::memory_order_relaxed); return; }
    sectionCount.fetch_add(1, std::memory_order_relaxed);

    const uint8_t table_id = sec[0];
    if (table_id != 0x7E) { invalidCount.fetch_add(1, std::memory_order_relaxed); return; }

    const bool ssi = (sec[1] & 0x80) != 0;
    if (ssi) { // ★DITは通常 ssi=0 を想定
        invalidCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const uint16_t section_length = (uint16_t)(((sec[1] & 0x0F) << 8) | sec[2]);
    const std::size_t total = 3u + section_length;
    if (total != len) { invalidCount.fetch_add(1, std::memory_order_relaxed); return; }

    Snapshot snap{};
    snap.valid = true;
    snap.table_id = table_id;
    snap.section_length = section_length;
    snap.transition_flag = (sec[3] & 0x01) != 0;

    lastSnapshot = snap;
    recovered.store(true, std::memory_order_release);
}
