#pragma once
#ifndef TDT_FILTER_HPP
#define TDT_FILTER_HPP

#include <cstdint>
#include <cstddef>

class TDTFilter
{
public:
    struct TimeInfo {
        int mjd = 0;
        int year = 0, month = 0, day = 0;
        int hour = 0, minute = 0, second = 0;
    };

    struct Stats {
        std::uint64_t inputPackets = 0;
        std::uint64_t pid14Packets = 0;
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
        std::uint64_t tdtSectionsParsed = 0;
        std::uint64_t recoveredCount = 0;
    };

public:
    TDTFilter();
    void Reset();

    void InputPacket(const uint8_t* tsPacket, std::size_t size);

    bool IsRecovered() const noexcept { return m_recovered; }
    const TimeInfo& GetLastTime() const noexcept { return m_lastTime; }

    const Stats& GetStats() const noexcept { return m_stats; }
    void ResetStats() noexcept { m_stats = {}; }

    void DebugDumpStats(const wchar_t* prefix = L"[TDT]") const;

private:
    void ParseTDTSection(const uint8_t* section, std::size_t length);

    static void DecodeMJD(int mjd, int& year, int& month, int& day);
    static int  BcdToInt(uint8_t b) noexcept;

    void ResetAssemble_() noexcept;

private:
    bool     m_recovered = false;
    TimeInfo m_lastTime{};
    Stats    m_stats{};

    uint8_t     m_sectionBuffer[1024]{};
    std::size_t m_sectionSize = 0;
    bool        m_waitingPayloadStart = true;
    uint8_t     m_lastContinuityCounter = 0xFF;
};

#endif // TDT_FILTER_HPP
