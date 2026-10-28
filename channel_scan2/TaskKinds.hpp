#pragma once
#ifndef TASK_KINDS_HPP
#define TASK_KINDS_HPP

#include <cstdint>

namespace TaskKinds
{
    // --- EPG ---
    // TimeKeeper / TaskListManager / Runner が共通で参照する kind
    constexpr std::uint32_t EpgFull = 1;
    constexpr std::uint32_t EpgLight = 2;

    // --- Recording (予約系) ---
    constexpr std::uint32_t RecPrepare = 1001;
    constexpr std::uint32_t RecStart = 1002;
    constexpr std::uint32_t RecStop = 1003;

    // --- Utility (チューナー不要タスク) ---
    constexpr std::uint32_t CleanupLogs = 2001;
}

#endif // TASK_KINDS_HPP