#pragma once
#ifndef EPG_PORTAL_RESERVATION_API_SERVER_HPP
#define EPG_PORTAL_RESERVATION_API_SERVER_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "EpgHtmlPortal.hpp"
#include "EpgHtmlPortal_ReservationData.hpp"
#include "AutoReserveRuleStore.hpp"

class TaskSystemConnector;

class EpgPortalReservationApiServer
{
public:
    EpgPortalReservationApiServer() = default;
    ~EpgPortalReservationApiServer();

    EpgPortalReservationApiServer(const EpgPortalReservationApiServer&) = delete;
    EpgPortalReservationApiServer& operator=(const EpgPortalReservationApiServer&) = delete;

    bool Start(
        std::uint16_t port,
        TaskSystemConnector* connector,
        const EpgHtmlPortal::WriteOptions& writeOptions);

    void Stop();

    bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }
    std::uint16_t GetPort() const noexcept { return port_; }

    void SetConnector(TaskSystemConnector* connector);
    void SetWriteOptions(const EpgHtmlPortal::WriteOptions& writeOptions);

private:
    struct RequestContext
    {
        std::string method;
        std::string path;
        std::string body;
        std::map<std::string, std::string> headers;
    };

    struct ApiStateSnapshot_
    {
        TaskSystemConnector* connector = nullptr;
        EpgHtmlPortal::WriteOptions writeOptions{};
    };

    struct AutoReserveRuleSaveFlowResult_
    {
        AutoReserve::SaveResult save{};
        AutoReserve::CurrentSearchRegisterResult registration{};
    };

    void WorkerMain_();
    bool InitWinsock_();
    void CleanupWinsock_() noexcept;
    bool PopClientSocket_(std::uintptr_t& outClientSock) noexcept;

    bool OpenListenSocket_();
    void CloseListenSocket_() noexcept;

    bool QueueClient_(std::uintptr_t clientSock);
    void ClientWorkerMain_();
    void CloseQueuedClients_() noexcept;

    bool ReadRequest_(std::uintptr_t clientSock, RequestContext& outReq);
    void HandleClient_(std::uintptr_t clientSock);

    bool LoadAuthenticationConfig_();
    void ResetAuthenticationState_() noexcept;
    bool IsRequestAuthenticated_(const RequestContext& req);
    bool ValidateLoginForm_(const std::string& formBody) const;
    std::string CreateSessionToken_();
    void RemoveRequestSession_(const RequestContext& req);
    static std::string GetCookieValue_(const RequestContext& req, const char* name);
    static std::string BuildLoginPage_(bool loginFailed);

    std::string DispatchRequest_(
        const RequestContext& req,
        int& statusCode,
        std::string& contentType);

    std::string HandleHealth_();
    std::string HandleSystemLog_(const std::string& requestPath, int& statusCode);
    std::string HandleUpsert_(const std::string& formBody, int& statusCode);
    std::string HandleRemove_(const std::string& formBody, int& statusCode);
    std::string HandleSetEnabled_(const std::string& formBody, int& statusCode);
    std::string HandleRemoveById_(const std::string& formBody, int& statusCode);
    std::string HandleSetEnabledById_(const std::string& formBody, int& statusCode);
    std::string HandleList_(int& statusCode);
    std::string HandleAutoReserveRulesSave_(const std::string& formBody, int& statusCode);

    ApiStateSnapshot_ GetApiStateSnapshot_() const;

    bool SaveAutoReserveRulesFromForm_(
        const std::string& formBody,
        AutoReserveRuleSaveFlowResult_& flow,
        int& statusCode,
        std::string& earlyResponseJson);

    void ApplyAutoReserveAfterRuleSave_(
        AutoReserveRuleSaveFlowResult_& flow);



    std::string BuildAutoReserveRuleSaveResponseJson_(
        const AutoReserveRuleSaveFlowResult_& flow) const;

    std::string HandleStaticFile_(
        const std::string& urlPath,
        int& statusCode,
        std::string& contentType);

    bool ResolveStaticFilePath_(
        const std::string& urlPath,
        std::filesystem::path& outPath) const;

    static std::string GetMimeType_(const std::filesystem::path& path);
    static std::string StripQueryString_(const std::string& path);

    static std::wstring UrlDecodeToWide_(const std::string& s);
    static std::string JsonEscapeUtf8_(const std::wstring& s);

    static std::wstring GetFormValue_(
        const std::map<std::wstring, std::wstring>& form,
        const wchar_t* key);

    static bool ParseBoolLoose_(const std::wstring& s, bool defValue);
    static int ParseIntLoose_(const std::wstring& s, int defValue);

    static std::map<std::wstring, std::wstring> ParseFormUrlEncoded_(
        const std::string& body);

    EpgHtmlPortal_ReservationData::ProgramReservationPayload BuildPayload_(
        const std::map<std::wstring, std::wstring>& form) const;

    std::string BuildReservationListJson_(const TaskSystemConnector& connector);
    std::string BuildJsonResult_(
        const EpgHtmlPortal_ReservationData::ReservationMutationResult& r,
        const TaskSystemConnector* connector);

private:
    std::atomic_bool running_{ false };
    std::thread worker_;

    std::uint16_t port_ = 37781;
    std::uintptr_t listenSock_ = static_cast<std::uintptr_t>(~std::uintptr_t{ 0 });

    std::mutex clientQueueMutex_{};
    std::condition_variable clientQueueCv_{};
    std::deque<std::uintptr_t> clientQueue_{};
    std::vector<std::thread> clientWorkers_{};
    std::uint32_t workerCount_ = 4;
    std::uint32_t maxQueuedClients_ = 128;

    std::atomic_uint64_t servedRequests_{ 0 };
    std::atomic_uint64_t rejectedRequests_{ 0 };

    mutable std::mutex stateMutex_;

    // TaskSystemConnector / TaskListManager はUI・TimeKeeper・APIから触られるため、
    // API worker pool からの同時操作をここで直列化する。
    // これを入れないと remove_by_id / set_enabled_by_id の同時実行や list 取得と
    // 更新の競合で std::out_of_range などが出る可能性がある。
    mutable std::mutex reservationApiMutex_;

    TaskSystemConnector* connector_ = nullptr;
    EpgHtmlPortal::WriteOptions writeOptions_{};

    bool winsockReady_ = false;

    bool passwordAuthEnabled_ = false;
    // 認証ONなのに資格情報が欠けている場合は、起動失敗ではなく
    // ループバック限定で復旧できる状態にする。
    bool forceLoopbackForInvalidAuth_ = false;
    std::wstring loginUser_{};
    std::wstring loginPassword_{};
    std::int64_t sessionLifetimeSeconds_ = std::int64_t{ 12 } *60 * 60;
    std::mutex authSessionMutex_{};
    std::map<std::string, std::int64_t> authSessions_{};


};

#endif // EPG_PORTAL_RESERVATION_API_SERVER_HPP
