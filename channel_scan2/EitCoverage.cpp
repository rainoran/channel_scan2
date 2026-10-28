#include "EitCoverage.hpp"

#include <algorithm>
#include <cstdint>

void EitCoverage::Reset()
{
    m_slots.fill(false);
    m_initialized = true;
}

void EitCoverage::MarkSlotRange(int beginSlot, int endSlot)
{
    if (!m_initialized) {
        // 安全側対応：呼び忘れ防止
        Reset();
    }

    beginSlot = (std::max)(beginSlot, 0);
    endSlot = (std::min)(endSlot, kTotalSlots);
    if (beginSlot >= endSlot) return;

    for (int i = beginSlot; i < endSlot; ++i) {
        m_slots[static_cast<std::size_t>(i)] = true;
    }
}

void EitCoverage::OnEventRange(int startMinutesFromBase, int durationMinutes)
{
    if (durationMinutes <= 0) return;

    // 安全側：範囲外（負値/8日超え）をクランプ
    constexpr int kTotalMinutes = kDays * 24 * 60;
    std::int64_t start = static_cast<std::int64_t>(startMinutesFromBase);
    std::int64_t dur = static_cast<std::int64_t>(durationMinutes);

    if (start < 0) {
        // base より前に開始している場合：開始を0に寄せ、長さも詰める
        dur += start; // start は負
        start = 0;
        if (dur <= 0) return;
    }

    if (start >= kTotalMinutes) return;

    std::int64_t end = start + dur;
    if (end <= 0) return;
    if (end > kTotalMinutes) end = kTotalMinutes;

    // 30分単位スロットに変換
    const int beginSlot = static_cast<int>(start / kSlotMinutes);
    const int endSlot = static_cast<int>((end + kSlotMinutes - 1) / kSlotMinutes);

    MarkSlotRange(beginSlot, endSlot);
}

EitCoverageSummary EitCoverage::GetSummary() const
{
    EitCoverageSummary summary{};

    if (!m_initialized) {
        return summary;
    }

    // 0〜2日 = 0〜(2*24*2) = 0〜96スロット
    constexpr int first2DaysSlots = 2 * 24 * kSlotsPerHour; // 96
    constexpr int totalSlots2to8 = kTotalSlots - first2DaysSlots; // 384 - 96 = 288

    int first2DaysUsed = 0;
    int restUsed = 0;

    for (int i = 0; i < kTotalSlots; ++i) {
        if (!m_slots[static_cast<std::size_t>(i)]) continue;
        if (i < first2DaysSlots) {
            ++first2DaysUsed;
        }
        else {
            ++restUsed;
        }
    }

    if (first2DaysSlots > 0) {
        summary.coverageFirst2Days = static_cast<double>(first2DaysUsed) /
            static_cast<double>(first2DaysSlots);
        summary.first2DaysFull = (first2DaysUsed == first2DaysSlots);
    }

    if (totalSlots2to8 > 0) {
        summary.coverage2to8Days = static_cast<double>(restUsed) /
            static_cast<double>(totalSlots2to8);
    }

    return summary;
}
