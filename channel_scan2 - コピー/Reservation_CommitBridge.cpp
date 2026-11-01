#include "Reservation_CommitBridge.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <sstream>
#include <utility>

#include "DebugManager.h"
#include "TaskSystemConnector.hpp"

namespace ReservationCommitBridge
{
    namespace
    {
        std::wstring EscapeJsonString_(std::wstring_view s)
        {
            std::wstring out;
            out.reserve(s.size() + 16);

            for (wchar_t ch : s) {
                switch (ch) {
                case L'\\':
                    out += L"\\\\";
                    break;
                case L'"':
                    out += L"\\\"";
                    break;
                case L'\r':
                    out += L"\\r";
                    break;
                case L'\n':
                    out += L"\\n";
                    break;
                case L'\t':
                    out += L"\\t";
                    break;
                default:
                    out.push_back(ch);
                    break;
                }
            }

            return out;
        }

        CommitResult ConvertResult_(
            const EpgHtmlPortal_ReservationData::ReservationMutationResult& src,
            std::uint64_t revision)
        {
            CommitResult out{};
            out.ok = src.ok;
            out.changed = src.changed;
            out.revision = revision;
            out.instance = src.instance;
            out.dedupeKey = src.dedupeKey;
            out.reason = src.reason;
            return out;
        }
    }

    void Store::Configure(
        TaskSystemConnector* connector,
        const EpgHtmlPortal::WriteOptions& opt)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connector_ = connector;
        opt_ = opt;

        DebugLog(L"[ReservationCommitBridge] configured");
    }

    CommitResult Store::Apply(
        const EpgHtmlPortal_ReservationData::ProgramReservationMutationCommand& cmd)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        CommitResult failed{};
        failed.ok = false;
        failed.changed = false;
        failed.revision = revision_;

        if (connector_ == nullptr) {
            failed.reason = L"connector-not-configured";
            return failed;
        }

        auto fixedCmd = cmd;

        // 予約操作は保存成功をもって成功扱いにする。
        // mutation 側で tasks.ini 更新および reservation_data.js 再生成まで行う。
        fixedCmd.regenerateReservationJs = true;

        const auto rawResult =
            EpgHtmlPortal_ReservationData::ApplyProgramReservationMutation(
                *connector_,
                opt_,
                fixedCmd);

        if (rawResult.ok) {
            ++revision_;
        }

        auto result = ConvertResult_(rawResult, revision_);

        DebugLogFormat(
            L"[ReservationCommitBridge] apply result ok=%d changed=%d rev=%llu reason=%ls",
            result.ok ? 1 : 0,
            result.changed ? 1 : 0,
            static_cast<unsigned long long>(result.revision),
            result.reason.c_str());

        return result;
    }

    CommitResult Store::UpsertProgram(
        const EpgHtmlPortal_ReservationData::ProgramReservationPayload& payload)
    {
        EpgHtmlPortal_ReservationData::ProgramReservationMutationCommand cmd{};
        cmd.action =
            EpgHtmlPortal_ReservationData::ProgramReservationMutationAction::Upsert;
        cmd.payload = payload;
        cmd.regenerateReservationJs = true;

        return Apply(cmd);
    }

    CommitResult Store::RemoveProgram(
        const EpgHtmlPortal_ReservationData::ProgramReservationPayload& payload)
    {
        EpgHtmlPortal_ReservationData::ProgramReservationMutationCommand cmd{};
        cmd.action =
            EpgHtmlPortal_ReservationData::ProgramReservationMutationAction::Remove;
        cmd.payload = payload;
        cmd.regenerateReservationJs = true;

        return Apply(cmd);
    }

    CommitResult Store::SetProgramEnabled(
        const EpgHtmlPortal_ReservationData::ProgramReservationPayload& payload,
        bool enabled)
    {
        EpgHtmlPortal_ReservationData::ProgramReservationMutationCommand cmd{};
        cmd.action =
            EpgHtmlPortal_ReservationData::ProgramReservationMutationAction::SetEnabled;
        cmd.payload = payload;
        cmd.enabled = enabled;
        cmd.disabledByUser = !enabled;
        cmd.excludeFromAuto = !enabled;
        cmd.regenerateReservationJs = true;

        return Apply(cmd);
    }

    std::uint64_t Store::GetRevision() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return revision_;
    }

    std::wstring BuildCommitResultJson(const CommitResult& result)
    {
        std::wostringstream oss;

        oss << L"{";
        oss << L"\"ok\":" << (result.ok ? L"true" : L"false") << L",";
        oss << L"\"changed\":" << (result.changed ? L"true" : L"false") << L",";
        oss << L"\"revision\":" << result.revision << L",";
        oss << L"\"instance\":" << result.instance << L",";
        oss << L"\"dedupeKey\":\"" << EscapeJsonString_(result.dedupeKey) << L"\",";
        oss << L"\"reason\":\"" << EscapeJsonString_(result.reason) << L"\"";
        oss << L"}";

        return oss.str();
    }
}