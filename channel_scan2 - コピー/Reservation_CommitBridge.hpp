#pragma once
#pragma once
#ifndef RESERVATION_COMMIT_BRIDGE_HPP
#define RESERVATION_COMMIT_BRIDGE_HPP

#include <cstdint>
#include <mutex>
#include <string>

#include "EpgHtmlPortal.hpp"
#include "EpgHtmlPortal_ReservationData.hpp"

class TaskSystemConnector;

namespace ReservationCommitBridge
{
    struct CommitResult
    {
        bool ok = false;
        bool changed = false;

        std::uint64_t revision = 0;
        std::uint32_t instance = 0;

        std::wstring dedupeKey;
        std::wstring reason;
    };

    class Store
    {
    public:
        Store() = default;

        Store(const Store&) = delete;
        Store& operator=(const Store&) = delete;

        void Configure(
            TaskSystemConnector* connector,
            const EpgHtmlPortal::WriteOptions& opt);

        CommitResult Apply(
            const EpgHtmlPortal_ReservationData::ProgramReservationMutationCommand& cmd);

        CommitResult UpsertProgram(
            const EpgHtmlPortal_ReservationData::ProgramReservationPayload& payload);

        CommitResult RemoveProgram(
            const EpgHtmlPortal_ReservationData::ProgramReservationPayload& payload);

        CommitResult SetProgramEnabled(
            const EpgHtmlPortal_ReservationData::ProgramReservationPayload& payload,
            bool enabled);

        std::uint64_t GetRevision() const noexcept;

    private:
        mutable std::mutex mutex_;
        TaskSystemConnector* connector_ = nullptr;
        EpgHtmlPortal::WriteOptions opt_{};
        std::uint64_t revision_ = 0;
    };

    std::wstring BuildCommitResultJson(const CommitResult& result);
}

#endif // RESERVATION_COMMIT_BRIDGE_HPP