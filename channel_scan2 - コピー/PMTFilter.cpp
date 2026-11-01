#include "PMTFilter.hpp"
#include "DebugManager.h"
#include <cstring>

PMTFilter::PMTFilter() = default;

PMTFilter::PMTFilter(TSParser* parser, PIDDataBuffer* buffer)
    : tsParser(parser), pidBuffer(buffer)
{
}

void PMTFilter::Reset()
{
    std::unique_lock<std::shared_mutex> lock(mtx);
    streamMap.clear();
    reasm.clear();
    recovered = false;
    lastValidationSuccess = false;
    validationFailCount = 0;
}

void PMTFilter::AddTargetPID(uint16_t pid)
{
    std::unique_lock<std::shared_mutex> lock(mtx);
    targetPIDs.insert(pid);
}

bool PMTFilter::IsTargetPID(uint16_t pid) const
{
    std::shared_lock<std::shared_mutex> lock(mtx);
    return targetPIDs.find(pid) != targetPIDs.end();
}

bool PMTFilter::IsRecovered() const
{
    std::shared_lock<std::shared_mutex> lock(mtx);
    return recovered;
}

bool PMTFilter::WasLastValidationSuccessful() const
{
    return lastValidationSuccess;
}

size_t PMTFilter::GetValidationFailCount() const
{
    return validationFailCount;
}

uint32_t PMTFilter::ComputePsiCrc32(const uint8_t* data, size_t length) noexcept
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x80000000u) crc = (crc << 1) ^ 0x04C11DB7u;
            else crc <<= 1;
        }
    }
    return crc;
}

void PMTFilter::ConsumePayloadBytes(uint16_t pid, const uint8_t* p, size_t n)
{
    if (!p || n == 0) return;

    auto& ctx = reasm[pid];
    ctx.buf.insert(ctx.buf.end(), p, p + n);

    while (ctx.buf.size() >= 3) {
        if (ctx.expected == 0) {
            const size_t sl = ((ctx.buf[1] & 0x0F) << 8) | ctx.buf[2];
            if (sl < 9 || sl > 1021) {
                ctx.buf.erase(ctx.buf.begin());
                continue;
            }
            ctx.expected = 3 + sl;
        }

        if (ctx.buf.size() < ctx.expected) break;

        ParsePMTSection(ctx.buf.data(), ctx.expected);

        ctx.buf.erase(ctx.buf.begin(), ctx.buf.begin() + ctx.expected);
        ctx.expected = 0;
    }
}

void PMTFilter::InputPacket(const uint8_t* data, size_t size)
{
    if (!data || size < 188) return;
    if (data[0] != 0x47) return;

    const uint16_t pid = static_cast<uint16_t>(((data[1] & 0x1F) << 8) | data[2]);

    {
        std::shared_lock<std::shared_mutex> lock(mtx);
        if (targetPIDs.find(pid) == targetPIDs.end()) {
            return;
        }
    }

    const uint8_t afc = (data[3] >> 4) & 0x03;
    if ((afc & 0x01) == 0) return; // payload無し

    const uint8_t cc = data[3] & 0x0F;
    auto& ctx = reasm[pid];
    if (ctx.haveLastCC) {
        const uint8_t expect = static_cast<uint8_t>((ctx.lastCC + 1) & 0x0F);
        if (cc != expect) {
            ctx.buf.clear();
            ctx.expected = 0;
        }
    }
    ctx.haveLastCC = true;
    ctx.lastCC = cc;

    size_t p = 4;
    if (afc & 0x02) {
        if (p >= size) return;
        const uint8_t adapt_len = data[p];
        p += 1 + adapt_len;
        if (p >= size) return;
    }

    const bool pusi = (data[1] & 0x40) != 0;
    if (pusi) {
        if (p >= size) return;
        const uint8_t pointer = data[p++];
        if (p + pointer > size) return;

        // pointer 分は前セクションの残り
        if (pointer > 0 && !ctx.buf.empty()) {
            ConsumePayloadBytes(pid, data + p, pointer);
        }
        p += pointer;
        if (p < size) ConsumePayloadBytes(pid, data + p, size - p);
    }
    else {
        if (p < size) ConsumePayloadBytes(pid, data + p, size - p);
    }
}

void PMTFilter::ParsePMTSection(const uint8_t* section, size_t length)
{
    if (!section || length < 16) return;

    // table_id=0x02
    if (section[0] != 0x02) return;

    const uint16_t sl = static_cast<uint16_t>(((section[1] & 0x0F) << 8) | section[2]);
    if (3 + sl != length) return;

    // CRC
    if (ComputePsiCrc32(section, length) != 0) {
        lastValidationSuccess = false;
        ++validationFailCount;
        return;
    }
    lastValidationSuccess = true;

    // program_info_length
    if (length < 12) return;
    const uint16_t programInfoLength = static_cast<uint16_t>(((section[10] & 0x0F) << 8) | section[11]);

    size_t pos = 12 + programInfoLength;
    const size_t end = 3 + sl - 4; // CRC除外

    if (pos > end) return;

    std::unique_lock<std::shared_mutex> lock(mtx);

    while (pos + 5 <= end) {
        const uint8_t  stream_type = section[pos];
        const uint16_t elem_pid = static_cast<uint16_t>(((section[pos + 1] & 0x1F) << 8) | section[pos + 2]);
        const uint16_t es_info_len = static_cast<uint16_t>(((section[pos + 3] & 0x0F) << 8) | section[pos + 4]);

        streamMap[elem_pid] = stream_type;

        pos += 5 + es_info_len;
        if (pos > end) break;
    }

    if (!streamMap.empty()) {
        recovered = true;
    }
}
