#include "BATFilter.hpp"

// FilterManager 側の PeekPsiTableId_OnPusi と同等の簡易版（BATFilter 単体で完結）
static bool PeekTableIdOnPusi(const uint8_t* data, size_t size, uint8_t& outTid) noexcept
{
    if (!data || size < 4) return false;
    if (data[0] != 0x47) return false;

    const bool pusi = (data[1] & 0x40) != 0;
    if (!pusi) return false;

    const uint8_t afc = (data[3] >> 4) & 0x03;
    if ((afc & 0x01) == 0) return false; // payload無し

    size_t p = 4;
    if (afc & 0x02) {
        if (p >= size) return false;
        const uint8_t adapt_len = data[p];
        p += 1u + adapt_len;
        if (p >= size) return false;
    }

    if (p >= size) return false;
    const uint8_t pointer = data[p];
    p += 1u;
    if (p + pointer >= size) return false;
    p += pointer;

    if (p >= size) return false;
    outTid = data[p];
    return true;
}

void BATFilter::InputPacket(const std::array<uint8_t, TS_PACKET_SIZE>& pkt) noexcept
{
    pidPackets.fetch_add(1, std::memory_order_relaxed);

    const uint8_t* data = pkt.data();
    const size_t size = pkt.size();

    // PID チェック（安全のため）
    if (size < 4 || data[0] != 0x47) return;
    const uint16_t pid = static_cast<uint16_t>(((data[1] & 0x1F) << 8) | data[2]);
    if (pid != 0x0011) return;

    const bool pusi = (data[1] & 0x40) != 0;
    if (pusi) pusiPackets.fetch_add(1, std::memory_order_relaxed);

    uint8_t tid = 0xFF;
    if (PeekTableIdOnPusi(data, size, tid)) {
        if (tid == 0x4A) {
            table4aSeen.fetch_add(1, std::memory_order_relaxed);
            recovered.store(true, std::memory_order_relaxed);
        }
    }
}
