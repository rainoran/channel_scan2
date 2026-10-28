#pragma once
#ifndef TOTFILTER_HPP
#define TOTFILTER_HPP

#include <cstdint>
#include <cstddef>

class TOTFilter
{
public:
    struct TimeInfo {
        int mjd = 0;
        int year = 0, month = 0, day = 0;
        int hour = 0, minute = 0, second = 0;
        int offsetMinutes = 0; // Local Time Offset (minutes), e.g. JST=+540
    };

    struct Stats {
        std::uint64_t inputPackets = 0;          // InputPacket呼び出し回数
        std::uint64_t pid14Packets = 0;          // PID=0x0014 を実際に処理した回数
        std::uint64_t ignoredNonPid14 = 0;

        std::uint64_t teiDrops = 0;
        std::uint64_t scrambledDrops = 0;

        std::uint64_t payloadlessDrops = 0;
        std::uint64_t pusiPackets = 0;

        std::uint64_t ccDuplicate = 0;
        std::uint64_t ccDiscontinuity = 0;

        std::uint64_t pointerFieldErrors = 0;
        std::uint64_t adaptationErrors = 0;

        std::uint64_t sectionLenErrors = 0;
        std::uint64_t sectionOverflowErrors = 0;

        std::uint64_t sectionsParsed = 0;
        std::uint64_t totSectionsParsed = 0;
        std::uint64_t recoveredCount = 0;

        std::uint64_t localTimeOffsetParsed = 0;
        std::uint64_t descriptorLoopErrors = 0;
    };

public:
    TOTFilter();
    void Reset();

    // 188バイトTSパケット投入（204/192等は呼び出し側で188に揃える前提）
    void InputPacket(const uint8_t* tsPacket, std::size_t size);

    bool IsRecovered() const noexcept { return m_recovered; }
    const TimeInfo& GetLastTime() const noexcept { return m_lastTime; }

    const Stats& GetStats() const noexcept { return m_stats; }
    void ResetStats() noexcept { m_stats = {}; }

    // 異常時調査用（必要なときだけ呼ぶ想定）
    void DebugDumpStats(const wchar_t* prefix = L"[TOT]") const;

private:
    void ParseTOTSection(const uint8_t* section, std::size_t length);

    static void DecodeMJD(int mjd, int& year, int& month, int& day);
    static int  BcdToInt(uint8_t b) noexcept;

private:
    void ResetAssemble_() noexcept;

private:
    bool     m_recovered = false;
    TimeInfo m_lastTime{};
    Stats    m_stats{};

    // section assemble
    uint8_t     m_sectionBuffer[1024]{};
    std::size_t m_sectionSize = 0;
    bool        m_waitingPayloadStart = true;
    uint8_t     m_lastContinuityCounter = 0xFF;
};

#endif // TOTFILTER_HPP
