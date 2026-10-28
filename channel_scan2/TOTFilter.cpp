#include "TOTFilter.hpp"
#include "DebugManager.h"

#include <algorithm>
#include <cstring>

namespace {
    constexpr uint16_t kPidTdtTot = 0x0014;
    constexpr uint8_t  kTableIdTDT = 0x70;
    constexpr uint8_t  kTableIdTOT = 0x73;

    // Local Time Offset Descriptor
    constexpr uint8_t  kDescLocalTimeOffset = 0x58;
}

TOTFilter::TOTFilter()
{
    Reset();
}

void TOTFilter::Reset()
{
    m_recovered = false;
    m_lastTime = TimeInfo{};
    ResetAssemble_();
    // Stats は Reset で消さない（チャンネル単位で集計したい場合がある）
    // 必要なら呼び出し側で ResetStats() を呼ぶ
}

void TOTFilter::ResetAssemble_() noexcept
{
    m_sectionSize = 0;
    m_waitingPayloadStart = true;
    m_lastContinuityCounter = 0xFF;
}

int TOTFilter::BcdToInt(uint8_t b) noexcept
{
    return static_cast<int>(((b >> 4) & 0x0F) * 10 + (b & 0x0F));
}

void TOTFilter::DecodeMJD(int mjd, int& year, int& month, int& day)
{
    // 既存ロジック踏襲（安定）
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

void TOTFilter::ParseTOTSection(const uint8_t* sec, std::size_t len)
{
    if (!sec || len < 8) return;

    // table_id
    const uint8_t table_id = sec[0];
    m_stats.sectionsParsed++;

    if (table_id != kTableIdTOT) {
        // TDT等は無視（同PID共有）
        return;
    }
    m_stats.totSectionsParsed++;

    // section_length (12bit)
    const uint16_t sectionLength = static_cast<uint16_t>(((sec[1] & 0x0F) << 8) | sec[2]);
    if (sectionLength < 7) { // UTC(5) + desc_loop_len(2) が最低
        m_stats.sectionLenErrors++;
        return;
    }
    const std::size_t total = 3u + static_cast<std::size_t>(sectionLength);
    if (total > len) {
        m_stats.sectionLenErrors++;
        return;
    }

    // UTC time (MJD(2) + BCD(3))
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
    t.offsetMinutes = 0;

    // descriptor loop length (12bit)
    const uint8_t* p = sec + 3 + 5;
    if (p + 2 > sec + total) {
        m_stats.sectionLenErrors++;
        return;
    }

    const uint16_t descLoopLen = static_cast<uint16_t>(((p[0] & 0x0F) << 8) | p[1]);
    p += 2;

    if (p + descLoopLen > sec + total) {
        m_stats.descriptorLoopErrors++;
        return;
    }

    // parse descriptors
    const uint8_t* dcur = p;
    const uint8_t* dend = p + descLoopLen;

    while (dcur + 2 <= dend) {
        const uint8_t tag = dcur[0];
        const uint8_t dlen = dcur[1];
        dcur += 2;

        if (dcur + dlen > dend) {
            m_stats.descriptorLoopErrors++;
            break;
        }

        if (tag == kDescLocalTimeOffset && dlen >= 13) {
            // DVB/ARIB: country_code(3), region/polarity(1), local_time_offset(2), time_of_change(5), next_time_offset(2)
            // offsetは local_time_offset を読む（b[4], b[5]）
            const uint8_t* b = dcur;

            const uint8_t polarity = b[3] & 0x01; // 1=negative
            const int off_h = BcdToInt(b[4]);
            const int off_m = BcdToInt(b[5]);

            int minutes = off_h * 60 + off_m;
            if (polarity) minutes = -minutes;

            t.offsetMinutes = minutes;
            m_stats.localTimeOffsetParsed++;
            // 複数ある場合もあり得るが、日本運用なら1件で十分
        }

        dcur += dlen;
    }

    m_lastTime = t;
    m_recovered = true;
    m_stats.recoveredCount++;
}

void TOTFilter::InputPacket(const uint8_t* tsPacket188, std::size_t size188)
{
    m_stats.inputPackets++;

    if (!tsPacket188 || size188 < 188) return;
    if (tsPacket188[0] != 0x47) return;

    // PID check（呼び出し側がPID14だけ投げても、誤用耐性を付ける）
    const uint16_t pid = static_cast<uint16_t>(((tsPacket188[1] & 0x1F) << 8) | tsPacket188[2]);
    if (pid != kPidTdtTot) {
        m_stats.ignoredNonPid14++;
        return;
    }
    m_stats.pid14Packets++;

    // TEI
    if (tsPacket188[1] & 0x80) {
        m_stats.teiDrops++;
        ResetAssemble_();
        return;
    }

    // Scrambled
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

    // continuity（簡易、十分安全寄り）
    if (m_lastContinuityCounter != 0xFF) {
        const uint8_t expected = (m_lastContinuityCounter + 1) & 0x0F;
        if (cc == m_lastContinuityCounter) {
            m_stats.ccDuplicate++;
            return; // duplicate
        }
        else if (cc != expected) {
            m_stats.ccDiscontinuity++;
            ResetAssemble_();
        }
    }
    m_lastContinuityCounter = cc;

    const uint8_t* p = tsPacket188 + 4;
    std::size_t payloadSize = 188 - 4;

    // adaptation skip
    if (afc & 0x02) {
        if (payloadSize < 1) { m_stats.adaptationErrors++; return; }
        const uint8_t afl = *p++;
        if (afl > payloadSize - 1) { m_stats.adaptationErrors++; return; }
        p += afl;
        payloadSize -= (1u + afl);
        if (payloadSize == 0) { m_stats.payloadlessDrops++; return; }
    }

    // PUSI processing
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

        // 新しいセクション開始
        m_sectionSize = 0;
        p += pointer;
        payloadSize -= (1u + pointer);
        if (payloadSize == 0) return;
    }
    else {
        if (m_waitingPayloadStart) return;
    }

    // section assemble（複数セクションが同一payloadに入る可能性あり）
    while (payloadSize > 0) {
        // header(3)まで
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

        ParseTOTSection(m_sectionBuffer, total);
        m_sectionSize = 0;
    }
}

void TOTFilter::DebugDumpStats(const wchar_t* prefix) const
{
    DebugLogFormat(L"%ls stats: in=%llu pid14=%llu pusi=%llu tei=%llu scr=%llu ccDup=%llu ccDisc=%llu ptrErr=%llu secLenErr=%llu secOv=%llu parsed=%llu totParsed=%llu recovered=%llu lto=%llu descErr=%llu",
        prefix ? prefix : L"[TOT]",
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
        (unsigned long long)m_stats.totSectionsParsed,
        (unsigned long long)m_stats.recoveredCount,
        (unsigned long long)m_stats.localTimeOffsetParsed,
        (unsigned long long)m_stats.descriptorLoopErrors
    );
}
