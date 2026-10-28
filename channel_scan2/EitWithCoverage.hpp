#pragma once
#ifndef EIT_WITH_COVERAGE_HPP
#define EIT_WITH_COVERAGE_HPP

#include <vector>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <limits>
#include <algorithm>
#include "EitCoverage.hpp"

// ------------------------------------------------------------
// 既存互換を保ちつつ、table_id を追加（A/O判定に使う）
// ------------------------------------------------------------
struct EitEvent {
    uint16_t serviceId = 0;
    uint16_t eventId = 0;

    // EIT table_id（0x50-0x6F 等）
    uint8_t  tableId = 0;

    // 1970-01-01 00:00 からの分数（同一軸で統一されている前提）
    int64_t  startEpochMinutes = 0;
    int      durationMinutes = 0;
};

// リトライ1回ぶんの計測結果
struct EitRetryMetrics
{
    std::size_t beforeEvents = 0;
    std::size_t afterEvents = 0;

    // 追加 / 減少の両方を見たいので signed を追加
    int64_t     deltaEventsSigned = 0;
    std::size_t addedEvents = 0;
    std::size_t removedEvents = 0;

    double coverageFirst2Days = 0.0;
    double coverage2to8Days = 0.0;
    bool   first2DaysFull = false;
};

// TSV の A_cov_* / O_cov_* を作るための集計
struct EitCoverageAggregate
{
    // overall = (first2Days + 2to8Days) / 2 の平均/最小/最大
    double overall_avg = 0.0;
    double overall_min = 0.0;
    double overall_max = 0.0;

    // 内訳も欲しい場合用（必要なければ使わなくてOK）
    double first2_avg = 0.0;
    double first2_min = 0.0;
    double first2_max = 0.0;

    double days2to8_avg = 0.0;
    double days2to8_min = 0.0;
    double days2to8_max = 0.0;

    std::size_t tables = 0; // 集計対象になったサービス数
};

enum class EitTableGroup : uint8_t
{
    Unknown = 0,
    ScheduleActual = 1, // 0x50-0x5F
    ScheduleOther = 2  // 0x60-0x6F
};

class EitWithCoverage {
public:
    void Clear()
    {
        m_events.clear();
        ResetCoverage_();
        m_baseEpochMinutes.reset();
    }

    // baseMinutes は「カバレッジ基準時刻」を分単位で渡す
    // rebuildFromExisting=true なら、保持している m_events から再構築
    void SetCoverageBase(int64_t baseEpochMinutes, bool rebuildFromExisting = false)
    {
        m_baseEpochMinutes = baseEpochMinutes;
        ResetCoverage_();

        if (rebuildFromExisting) {
            RebuildCoverageFromEvents_();
        }
    }

    void AddEvent(const EitEvent& ev)
    {
        m_events.push_back(ev);
        UpdateCoverageForEvent_(ev);
    }

    const std::vector<EitEvent>& GetEvents() const noexcept
    {
        return m_events;
    }

    // 既存互換：全イベントの「ユニオン」カバレッジ
    [[nodiscard]]
    EitCoverageSummary GetCoverageSummary() const
    {
        return m_unionCoverage.GetSummary();
    }

    // ★ 追加：A/O を TSV 用に集計
    [[nodiscard]]
    EitCoverageAggregate GetAggregate(EitTableGroup group) const
    {
        EitCoverageAggregate out;

        double overall_sum = 0.0;
        double first2_sum = 0.0;
        double days2to8_sum = 0.0;

        out.overall_min = std::numeric_limits<double>::infinity();
        out.first2_min = std::numeric_limits<double>::infinity();
        out.days2to8_min = std::numeric_limits<double>::infinity();

        out.overall_max = -std::numeric_limits<double>::infinity();
        out.first2_max = -std::numeric_limits<double>::infinity();
        out.days2to8_max = -std::numeric_limits<double>::infinity();

        std::size_t n = 0;

        for (const auto& kv : m_covByServiceGroup) {
            const uint32_t key = kv.first;
            const auto g = static_cast<EitTableGroup>(key & 0xFFu);
            if (g != group) continue;

            const auto summary = kv.second.GetSummary();
            const double overall = (summary.coverageFirst2Days + summary.coverage2to8Days) * 0.5;

            overall_sum += overall;
            first2_sum += summary.coverageFirst2Days;
            days2to8_sum += summary.coverage2to8Days;

            out.overall_min = (std::min)(out.overall_min, overall);
            out.overall_max = (std::max)(out.overall_max, overall);

            out.first2_min = (std::min)(out.first2_min, summary.coverageFirst2Days);
            out.first2_max = (std::max)(out.first2_max, summary.coverageFirst2Days);

            out.days2to8_min = (std::min)(out.days2to8_min, summary.coverage2to8Days);
            out.days2to8_max = (std::max)(out.days2to8_max, summary.coverage2to8Days);

            ++n;
        }

        out.tables = n;

        if (n == 0) {
            // 値は全部 0 にして返す
            out.overall_min = out.overall_max = 0.0;
            out.first2_min = out.first2_max = 0.0;
            out.days2to8_min = out.days2to8_max = 0.0;
            return out;
        }

        out.overall_avg = overall_sum / static_cast<double>(n);
        out.first2_avg = first2_sum / static_cast<double>(n);
        out.days2to8_avg = days2to8_sum / static_cast<double>(n);

        // infinity を戻す安全策
        if (!std::isfinite(out.overall_min)) out.overall_min = 0.0;
        if (!std::isfinite(out.first2_min))  out.first2_min = 0.0;
        if (!std::isfinite(out.days2to8_min)) out.days2to8_min = 0.0;

        if (!std::isfinite(out.overall_max)) out.overall_max = 0.0;
        if (!std::isfinite(out.first2_max))  out.first2_max = 0.0;
        if (!std::isfinite(out.days2to8_max)) out.days2to8_max = 0.0;

        return out;
    }

    // ★ 既存：リトライ用の計測結果を生成（マイナスも出す）
    [[nodiscard]]
    EitRetryMetrics MakeRetryMetrics(std::size_t beforeEvents) const noexcept
    {
        EitRetryMetrics m{};
        m.beforeEvents = beforeEvents;
        m.afterEvents = m_events.size();

        const int64_t delta = static_cast<int64_t>(m.afterEvents) - static_cast<int64_t>(m.beforeEvents);
        m.deltaEventsSigned = delta;

        if (delta >= 0) {
            m.addedEvents = static_cast<std::size_t>(delta);
            m.removedEvents = 0;
        }
        else {
            m.addedEvents = 0;
            m.removedEvents = static_cast<std::size_t>(-delta);
        }

        const auto summary = m_unionCoverage.GetSummary();
        m.coverageFirst2Days = summary.coverageFirst2Days;
        m.coverage2to8Days = summary.coverage2to8Days;
        m.first2DaysFull = summary.first2DaysFull;

        return m;
    }

private:
    static constexpr EitTableGroup ClassifyTableId_(uint8_t tid) noexcept
    {
        if (tid >= 0x50 && tid <= 0x5F) return EitTableGroup::ScheduleActual;
        if (tid >= 0x60 && tid <= 0x6F) return EitTableGroup::ScheduleOther;
        return EitTableGroup::Unknown;
    }

    static constexpr uint32_t MakeKey_(uint16_t serviceId, EitTableGroup group) noexcept
    {
        return (static_cast<uint32_t>(serviceId) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(group));
    }

    void ResetCoverage_()
    {
        m_unionCoverage.Reset();
        m_covByServiceGroup.clear();
    }

    void RebuildCoverageFromEvents_()
    {
        // m_events から coverage を再構築（m_events 自体は維持）
        for (const auto& ev : m_events) {
            UpdateCoverageForEvent_(ev);
        }
    }

    void UpdateCoverageForEvent_(const EitEvent& ev)
    {
        if (!m_baseEpochMinutes.has_value()) return;
        if (ev.durationMinutes <= 0) return;

        const auto group = ClassifyTableId_(ev.tableId);
        if (group == EitTableGroup::Unknown) {
            // present/following 等は coverage 対象外（必要ならここを拡張）
            return;
        }

        const int startDelta =
            static_cast<int>(ev.startEpochMinutes - *m_baseEpochMinutes);

        // ユニオン coverage
        m_unionCoverage.OnEventRange(startDelta, ev.durationMinutes);

        // サービス別 coverage
        const uint32_t key = MakeKey_(ev.serviceId, group);
        auto& cov = m_covByServiceGroup[key]; // 無ければ生成される
        cov.OnEventRange(startDelta, ev.durationMinutes);
    }

private:
    std::vector<EitEvent> m_events;

    // 全イベント（Schedule A+O を union）した coverage
    EitCoverage m_unionCoverage;

    // (serviceId, group) -> coverage
    std::unordered_map<uint32_t, EitCoverage> m_covByServiceGroup;

    std::optional<int64_t> m_baseEpochMinutes;
};

#endif // EIT_WITH_COVERAGE_HPP
