#pragma once
#ifndef EPG_HTML_PORTAL_RESERVATION_DATA_HPP
#define EPG_HTML_PORTAL_RESERVATION_DATA_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "EpgHtmlPortal.hpp"

class TaskSystemConnector;

namespace EpgHtmlPortal_ReservationData
{
    struct ProgramReservationPayload
    {
        int space = 0;
        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;
        std::uint16_t serviceId = 0;
        std::uint16_t eventId = 0;

        std::wstring title;
        std::wstring titleLabel;
        std::wstring serviceName;
        std::wstring summary;
        std::wstring genre;

        std::wstring dateText;
        std::wstring weekdayText;
        std::wstring timeText;

        std::wstring startDate;   // YYYY-MM-DD
        std::wstring startTime;   // HH:MM:SS
        std::wstring endDate;     // YYYY-MM-DD
        std::wstring endTime;     // HH:MM:SS
        std::uint32_t durationSec = 0;

        std::wstring reserveMode = L"record";          // watch / record
        std::wstring reserveSource = L"epg_portal_switch";
        std::wstring dedupeKey;
        std::wstring excludeKey;

        bool enabled = true;
        bool disabledByUser = false;
        bool excludeFromAuto = false;
        bool autoReserved = false;

        std::uint8_t requiredTuners = 1;
        int preferredTunerIndex = -1;
    };

    struct ReservationMutationResult
    {
        bool ok = false;
        bool changed = false;
        std::uint32_t instance = 0;
        std::wstring dedupeKey;
        std::wstring reason;
    };

    enum class ProgramReservationMutationAction
    {
        Upsert,
        Remove,
        SetEnabled
    };

    struct ProgramReservationMutationCommand
    {
        ProgramReservationMutationAction action = ProgramReservationMutationAction::Upsert;
        ProgramReservationPayload payload{};

        bool enabled = true;
        bool disabledByUser = false;
        bool excludeFromAuto = false;

        bool regenerateReservationJs = true;
    };

    ReservationMutationResult ApplyProgramReservationMutation(
        TaskSystemConnector& connector,
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramReservationMutationCommand& cmd);

    std::filesystem::path ResolveTaskIniPath(const EpgHtmlPortal::WriteOptions& opt);

    bool WriteReservationDataJs(const EpgHtmlPortal::WriteOptions& opt);
    bool WriteReservationDataJs(
        const EpgHtmlPortal::WriteOptions& opt,
        const TaskSystemConnector& connector);

    std::wstring BuildProgramReservationDedupeKey(
        const ProgramReservationPayload& payload);

    std::optional<ProgramReservationPayload> NormalizeProgramReservationPayload(
        const ProgramReservationPayload& payload);

    ReservationMutationResult UpsertProgramReservation(
        TaskSystemConnector& connector,
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramReservationPayload& payload,
        bool regenerateReservationJs = true);

    ReservationMutationResult RemoveProgramReservation(
        TaskSystemConnector& connector,
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramReservationPayload& payload,
        bool regenerateReservationJs = true);

    ReservationMutationResult SetProgramReservationEnabled(
        TaskSystemConnector& connector,
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramReservationPayload& payload,
        bool enabled,
        bool disabledByUser,
        bool excludeFromAuto,
        bool regenerateReservationJs = true);
}

#endif // EPG_HTML_PORTAL_RESERVATION_DATA_HPP