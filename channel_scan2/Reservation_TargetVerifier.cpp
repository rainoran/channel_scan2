#include "Reservation_TargetVerifier.hpp"

#include <sstream>

namespace Reservation_TargetVerifier
{
    ReservationTarget BuildTargetFromTaskEntry(
        const TaskListManager::TaskEntry& entry)
    {
        ReservationTarget t{};

        const auto& r = entry.reserve;

        t.space = r.space;
        t.networkId = r.networkId;
        t.tsId = r.tsId;
        t.serviceId = r.serviceId;
        t.eventId = r.eventId;

        t.title = r.title;
        t.titleLabel = r.titleLabel;
        t.serviceName = r.serviceName;

        t.startDate = r.startDate;
        t.startTime = r.startTime;
        t.endDate = r.endDate;
        t.endTime = r.endTime;

        return t;
    }

    VerifyResult Verify(
        const ReservationTarget& target,
        const ObservedTarget& observed,
        const VerifyOptions& options)
    {
        VerifyResult result{};

        auto fail = [&](const std::wstring& reason) {
            result.ok = false;
            result.reason = reason;
            return result;
            };

        if (options.requireSpace &&
            target.space >= 0 &&
            observed.space != target.space) {
            std::wostringstream oss;
            oss << L"space mismatch expected=" << target.space
                << L" actual=" << observed.space;
            return fail(oss.str());
        }

        if (options.requireNetworkId &&
            target.networkId != 0 &&
            observed.networkId != target.networkId) {
            std::wostringstream oss;
            oss << L"networkId mismatch expected="
                << static_cast<unsigned>(target.networkId)
                << L" actual="
                << static_cast<unsigned>(observed.networkId);
            return fail(oss.str());
        }

        if (options.requireTsId &&
            target.tsId != 0 &&
            observed.tsId != target.tsId) {
            std::wostringstream oss;
            oss << L"tsId mismatch expected="
                << static_cast<unsigned>(target.tsId)
                << L" actual="
                << static_cast<unsigned>(observed.tsId);
            return fail(oss.str());
        }

        if (options.requireServiceId &&
            target.serviceId != 0 &&
            observed.serviceId != target.serviceId) {
            std::wostringstream oss;
            oss << L"serviceId mismatch expected="
                << static_cast<unsigned>(target.serviceId)
                << L" actual="
                << static_cast<unsigned>(observed.serviceId);
            return fail(oss.str());
        }

        result.serviceMatched = true;

        if (options.requireEventId &&
            target.eventId != 0 &&
            observed.eventId != target.eventId) {
            std::wostringstream oss;
            oss << L"eventId mismatch expected="
                << static_cast<unsigned>(target.eventId)
                << L" actual="
                << static_cast<unsigned>(observed.eventId);
            return fail(oss.str());
        }

        if (options.requireTitle &&
            !target.title.empty()) {
            if (observed.title.empty() && options.allowEmptyObservedProgram) {
                result.programMatched = false;
                result.ok = true;
                result.reason = L"service matched, observed program title is empty";
                return result;
            }

            if (observed.title.find(target.title) == std::wstring::npos &&
                target.title.find(observed.title) == std::wstring::npos) {
                std::wostringstream oss;
                oss << L"title mismatch expected=" << target.title
                    << L" actual=" << observed.title;
                return fail(oss.str());
            }
        }

        result.programMatched =
            !target.eventId || observed.eventId == target.eventId;

        result.ok = true;
        result.reason = L"matched";
        return result;
    }

    VerifyResult VerifyServiceOnly(
        const ReservationTarget& target,
        const ObservedTarget& observed)
    {
        VerifyOptions opt{};
        opt.requireSpace = true;
        opt.requireNetworkId = true;
        opt.requireTsId = true;
        opt.requireServiceId = true;
        opt.requireEventId = false;
        opt.requireTitle = false;

        return Verify(target, observed, opt);
    }
}