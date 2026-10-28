
#pragma once
#ifndef RESERVATION_TARGET_VERIFIER_HPP
#define RESERVATION_TARGET_VERIFIER_HPP

#include <cstdint>
#include <string>

#include "TaskListManager.hpp"

namespace Reservation_TargetVerifier
{
    struct ReservationTarget
    {
        int space = -1;

        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;
        std::uint16_t serviceId = 0;
        std::uint16_t eventId = 0;

        std::wstring title;
        std::wstring titleLabel;
        std::wstring serviceName;

        std::wstring startDate;
        std::wstring startTime;
        std::wstring endDate;
        std::wstring endTime;
    };

    struct ObservedTarget
    {
        int space = -1;
        int channel = -1;

        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;
        std::uint16_t serviceId = 0;
        std::uint16_t eventId = 0;

        int signal = 0;
        int bitrate = 0;

        std::wstring title;
        std::wstring serviceName;

        std::wstring startDate;
        std::wstring startTime;
        std::wstring endDate;
        std::wstring endTime;
    };

    struct VerifyOptions
    {
        bool requireSpace = true;
        bool requireNetworkId = true;
        bool requireTsId = true;
        bool requireServiceId = true;

        // eventId / title は EIT 取得タイミングに左右されるため、
        // 初期値では必須にしない。
        bool requireEventId = false;
        bool requireTitle = false;

        bool allowEmptyObservedProgram = true;
    };

    struct VerifyResult
    {
        bool serviceMatched = false;
        bool programMatched = false;
        bool ok = false;

        std::wstring reason;
    };

    ReservationTarget BuildTargetFromTaskEntry(
        const TaskListManager::TaskEntry& entry);

    VerifyResult Verify(
        const ReservationTarget& target,
        const ObservedTarget& observed,
        const VerifyOptions& options = {});

    VerifyResult VerifyServiceOnly(
        const ReservationTarget& target,
        const ObservedTarget& observed);
}

#endif // RESERVATION_TARGET_VERIFIER_HPP