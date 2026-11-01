#include "TDTFilter.hpp"
#include "DebugManager.h"

#include <algorithm>
#include <cstring>

namespace {
    constexpr uint16_t kPidTdtTot = 0x0014;
    constexpr uint8_t  kTableIdTDT = 0x70;
}

TDTFilter::TDTFilter()
{
    Reset();
}

void TDTFilter::Reset()
{
    m_recovered = false;
    m_lastTime = TimeInfo{};
    ResetAssemble_();
}

void TDTFilter::ResetAssemble_() noexcept
{
    m_sectionSize = 0;
    m_waitingPayloadStart = true;
    m_lastContinuityCounter = 0xFF;
}

int TDTFilter::BcdToInt(uint8_t b) noexcept
{
    return static_cast<int>(((b >> 4) & 0x0F) * 10 + (b & 0x0F));
}

void TDTFilter::DecodeMJD(int mjd, int& year, int& month, int& day)
{
    int jd = mjd + 2400001 + 68569;
    int c = 4 * jd / 146097;
    jd = jd - (146097 * c + 3) / 4;
    int e = 4000 * (jd + 1) / 1461001;
    jd = jd - 1461 * e / 4 + 31;
    int f = 80 * jd / 2447;
    day = jd - 2447 * f / 80;
    jd = f / 11;
    month = f + 2 - 12 * jd;
    year = 100 * (c - 49) + e + jd;
}

void TDTFilter::ParseTDTSection(const uint8_t* sec, std::size_t len)
{
    if (!sec || len < 8) return;

    const uint8_t table_id = sec[0];
    m_stats.sectionsParsed++;

    if (table_id != kTableIdTDT) return;
    m_stats.tdtSectionsParsed++;

    const uint16_t sectionLength = static_cast<uint16_t>(((sec[1] & 0x0F) << 8) | sec[2]);
    if (sectionLength < 5) { // UTC(5) が最低
        m_stats.sectionLenErrors++;
        return;
    }
    const std::size_t total = 3u + static_cast<std::size_t>(sectionLength);
    if (total > len) {
        m_stats.sectionLenErrors++;
        return;
    }

    const uint8_t* utc = sec + 3;
    const int mjd = (static_cast<int>(utc[0]) << 8) | static_cast<int>(utc[1]);

    int y = 0, m = 0, d = 0;
    DecodeMJD(mjd, y, m, d);

    TimeInfo t{};
    t.mjd = mjd;
    t.year = y; t.month = m; t.day = d;
    t.hour = BcdToInt(utc[2]);
    t.minute = BcdToInt(utc[3]);
    t.second = BcdToInt(utc[4]);

    m_lastTime = t;
    m_recovered = true;
    m_stats.recoveredCount++;
}

void TDTFilter::InputPacket(const uint8_t* tsPacket188, std::size_t size188)
{
    m_stats.inputPackets++;

    if (!tsPacket188 || size188 < 188) return;
    if (tsPacket188[0] != 0x47) return;

    const uint16_t pid = static_cast<uint16_t>(((tsPacket188[1] & 0x1F) << 8) | tsPacket188[2]);
    if (pid != kPidTdtTot) {
        m_stats.ignoredNonPid14++;
        return;
    }
    m_stats.pid14Packets++;

    if (tsPacket188[1] & 0x80) {
        m_stats.teiDrops++;
        ResetAssemble_();
        return;
    }

    const uint8_t scrambling = (tsPacket188[3] & 0xC0) >> 6;
    if (scrambling != 0) {
        m_stats.scrambledDrops++;
        return;
    }

    const bool pusi = (tsPacket188[1] & 0x40) != 0;
    const uint8_t afc = (tsPacket188[3] >> 4) & 0x03;
    const uint8_t cc = tsPacket188[3] & 0x0F;

    if ((afc & 0x01) == 0) {
        m_stats.payloadlessDrops++;
        return;
    }

    if (m_lastContinuityCounter != 0xFF) {
        const uint8_t expected = (m_lastContinuityCounter + 1) & 0x0F;
        if (cc == m_lastContinuityCounter) {
            m_stats.ccDuplicate++;
            return;
        }
        else if (cc != expected) {
            m_stats.ccDiscontinuity++;
            ResetAssemble_();
        }
    }
    m_lastContinuityCounter = cc;

    const uint8_t* p = tsPacket188 + 4;
    std::size_t payloadSize = 188 - 4;

    if (afc & 0x02) {
        if (payloadSize < 1) { m_stats.adaptationErrors++; return; }
        const uint8_t afl = *p++;
        if (afl > payloadSize - 1) { m_stats.adaptationErrors++; return; }
        p += afl;
        payloadSize -= (1u + afl);
        if (payloadSize == 0) { m_stats.payloadlessDrops++; return; }
    }

    if (pusi) {
        m_stats.pusiPackets++;
        m_waitingPayloadStart = false;

        if (payloadSize < 1) { m_stats.pointerFieldErrors++; ResetAssemble_(); return; }
        const uint8_t pointer = *p++;
        if (pointer > payloadSize - 1) {
            m_stats.pointerFieldErrors++;
            ResetAssemble_();
            return;
        }

        m_sectionSize = 0;
        p += pointer;
        payloadSize -= (1u + pointer);
        if (payloadSize == 0) return;
    }
    else {
        if (m_waitingPayloadStart) return;
    }

    while (payloadSize > 0) {
        if (m_sectionSize < 3) {
            const std::size_t need = 3 - m_sectionSize;
            const std::size_t copy = (std::min)(need, payloadSize);
            std::memcpy(m_sectionBuffer + m_sectionSize, p, copy);
            m_sectionSize += copy;
            p += copy;
            payloadSize -= copy;

            if (m_sectionSize < 3) return;

            const uint16_t secLen = static_cast<uint16_t>(((m_sectionBuffer[1] & 0x0F) << 8) | m_sectionBuffer[2]);
            if (secLen > 1021) {
                m_stats.sectionLenErrors++;
                ResetAssemble_();
                return;
            }
        }

        const uint16_t secLen = static_cast<uint16_t>(((m_sectionBuffer[1] & 0x0F) << 8) | m_sectionBuffer[2]);
        const std::size_t total = 3u + static_cast<std::size_t>(secLen);

        if (total > sizeof(m_sectionBuffer)) {
            m_stats.sectionOverflowErrors++;
            ResetAssemble_();
            return;
        }

        const std::size_t need = total - m_sectionSize;
        if (need > 0) {
            const std::size_t copy = (std::min)(need, payloadSize);
            std::memcpy(m_sectionBuffer + m_sectionSize, p, copy);
            m_sectionSize += copy;
            p += copy;
            payloadSize -= copy;
        }

        if (m_sectionSize < total) return;

        ParseTDTSection(m_sectionBuffer, total);
        m_sectionSize = 0;
    }
}

void TDTFilter::DebugDumpStats(const wchar_t* prefix) const
{
    DebugLogFormat(L"%ls stats: in=%llu pid14=%llu pusi=%llu tei=%llu scr=%llu ccDup=%llu ccDisc=%llu ptrErr=%llu secLenErr=%llu secOv=%llu parsed=%llu tdtParsed=%llu recovered=%llu",
        prefix ? prefix : L"[TDT]",
        (unsigned long long)m_stats.inputPackets,
        (unsigned long long)m_stats.pid14Packets,
        (unsigned long long)m_stats.pusiPackets,
        (unsigned long long)m_stats.teiDrops,
        (unsigned long long)m_stats.scrambledDrops,
        (unsigned long long)m_stats.ccDuplicate,
        (unsigned long long)m_stats.ccDiscontinuity,
        (unsigned long long)m_stats.pointerFieldErrors,
        (unsigned long long)m_stats.sectionLenErrors,
        (unsigned long long)m_stats.sectionOverflowErrors,
        (unsigned long long)m_stats.sectionsParsed,
        (unsigned long long)m_stats.tdtSectionsParsed,
        (unsigned long long)m_stats.recoveredCount
    );
}
