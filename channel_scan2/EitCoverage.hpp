#pragma once
#ifndef EIT_COVERAGE_HPP
#define EIT_COVERAGE_HPP

#include <array>
#include <cstdint>

struct EitCoverageSummary {
    // 0〜2日分（本命）
    double coverageFirst2Days = 0.0; // 0.0〜1.0
    bool   first2DaysFull = false;

    // 2〜8日分（おまけ／バックアップの埋まり具合確認用）
    double coverage2to8Days = 0.0; // 0.0〜1.0
};

class EitCoverage {
public:
    static constexpr int kDays = 8;
    static constexpr int kSlotMinutes = 30;
    static constexpr int kSlotsPerHour = 60 / kSlotMinutes;
    static constexpr int kTotalSlots = kDays * 24 * kSlotsPerHour; // 8 * 24 * 2 = 384
    static constexpr int kTotalMinutes = kTotalSlots * kSlotMinutes; // 384 * 30 = 11520 (= 8日)

    EitCoverage() = default;

    // カバレッジ情報をクリア
    void Reset();

    // base からの経過分数と、イベントの分数長さを渡す
    //   startMinutesFromBase: base からのスタート (分)
    //   durationMinutes     : イベント長 (分)
    void OnEventRange(int startMinutesFromBase, int durationMinutes);

    // 集計結果取得
    [[nodiscard]]
    EitCoverageSummary GetSummary() const;

private:
    std::array<bool, kTotalSlots> m_slots{};
    bool m_initialized = false;

    void MarkSlotRange(int beginSlot, int endSlot);
};

#endif // EIT_COVERAGE_HPP
