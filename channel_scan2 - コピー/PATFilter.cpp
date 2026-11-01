#include "PATFilter.hpp"
#include "DebugManager.h"
#include <algorithm>
#include <cstring>

PATFilter::PATFilter(TSParser* parser, PIDDataBuffer* buffer)
    : tsParser(parser), tsBuffer(buffer)
{
}

void PATFilter::Reset()
{
    std::unique_lock<std::shared_mutex> lock(patMutex);
    programMap.clear();
    sectionBuffer.clear();
    expectedSectionSize = 0;
    haveLastCC = false;
    lastCC = 0xFF;
}

uint32_t PATFilter::ComputePsiCrc32(const uint8_t* data, size_t length) noexcept
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

void PATFilter::ConsumePayloadBytes(const uint8_t* p, size_t n)
{
    if (!p || n == 0) return;

    sectionBuffer.insert(sectionBuffer.end(), p, p + n);

    while (sectionBuffer.size() >= 3) {
        if (expectedSectionSize == 0) {
            const size_t section_length = ((sectionBuffer[1] & 0x0F) << 8) | sectionBuffer[2];
            if (section_length < 9 || section_length > 1021) {
                // 再同期：1バイト捨て
                sectionBuffer.erase(sectionBuffer.begin());
                continue;
            }
            expectedSectionSize = 3 + section_length;
        }

        if (sectionBuffer.size() < expectedSectionSize) break;

        // 1セクション完成
        ParsePATSection(sectionBuffer.data(), expectedSectionSize);

        sectionBuffer.erase(sectionBuffer.begin(), sectionBuffer.begin() + expectedSectionSize);
        expectedSectionSize = 0;
    }
}

void PATFilter::InputPacket(const uint8_t* data, size_t size)
{
    if (!data || size < 188) return;
    if (data[0] != 0x47) return;

    const uint16_t pid = static_cast<uint16_t>(((data[1] & 0x1F) << 8) | data[2]);
    if (pid != 0x0000) return;

    const uint8_t afc = (data[3] >> 4) & 0x03;
    if ((afc & 0x01) == 0) return; // payload無し

    const uint8_t cc = data[3] & 0x0F;
    if (haveLastCC) {
        const uint8_t expect = static_cast<uint8_t>((lastCC + 1) & 0x0F);
        if (cc != expect) {
            // 不連続 → 途中セクション破棄
            sectionBuffer.clear();
            expectedSectionSize = 0;
        }
    }
    haveLastCC = true;
    lastCC = cc;

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
        const uint8_t pointer_field = data[p++];
        if (p + pointer_field > size) return;

        // pointer_field 分は「前セクションの残り」
        if (pointer_field > 0 && !sectionBuffer.empty()) {
            ConsumePayloadBytes(data + p, pointer_field);
        }
        // 新セクション開始
        p += pointer_field;
        // 新セクションの先頭以降を投入（同一TS内に複数セクションがあってもOK）
        if (p < size) {
            ConsumePayloadBytes(data + p, size - p);
        }
    }
    else {
        // 継続
        if (p < size) {
            ConsumePayloadBytes(data + p, size - p);
        }
    }
}

void PATFilter::InputPacket(const std::array<uint8_t, TS_PACKET_SIZE>& pkt)
{
    InputPacket(pkt.data(), pkt.size());
}

void PATFilter::ParsePATSection(const uint8_t* section, size_t length)
{
    if (!section || length < 12) return;

    if (section[0] != 0x00) return; // table_id PAT

    const uint16_t section_length = static_cast<uint16_t>(((section[1] & 0x0F) << 8) | section[2]);
    if (3 + section_length != length) return;

    // CRC32（PSIは「計算結果が0」になる）
    if (ComputePsiCrc32(section, length) != 0) {
        DebugLog(L"[PAT] CRC32 mismatch -> drop section");
        return;
    }

    // program loop
    const size_t sectionEnd = 3 + section_length - 4; // CRC除外
    size_t pos = 8; // loop start
    if (sectionEnd < pos) return;

    std::map<uint16_t, uint16_t> newMap;

    while (pos + 4 <= sectionEnd) {
        const uint16_t program_number = static_cast<uint16_t>((section[pos] << 8) | section[pos + 1]);
        const uint16_t pmtPid = static_cast<uint16_t>(((section[pos + 2] & 0x1F) << 8) | section[pos + 3]);
        if (program_number != 0) {
            newMap[program_number] = pmtPid;
        }
        pos += 4;
    }

    if (!newMap.empty()) {
        std::unique_lock<std::shared_mutex> lock(patMutex);
        programMap = std::move(newMap);
    }
}

bool PATFilter::IsRecovered() const
{
    std::shared_lock<std::shared_mutex> lock(patMutex);
    return !programMap.empty();
}

std::vector<uint16_t> PATFilter::GetPMTPIDs() const
{
    std::shared_lock<std::shared_mutex> lock(patMutex);
    std::vector<uint16_t> pids;
    pids.reserve(programMap.size());
    for (const auto& kv : programMap) {
        pids.push_back(kv.second);
    }
    return pids;
}
