#pragma once
#ifndef RESERVATION_RAW_TS_RECORDER_HPP
#define RESERVATION_RAW_TS_RECORDER_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "TsStreamTypes.hpp"

class IBonDriver2;

// 内部録画用の生TS保存ユニット。
//
// BonDriver から TSStreamUtils::ReceiveTsStreamBlock() で188byte TSパケットを取得し、
// 解析前の生TSをそのまま .ts ファイルへ保存する。
//
// 既存 Start() は予約録画用として維持する。
// TS機能用には StartTsCapture() を追加し、内部的には同じ保存処理を使う。
//
// 通常経路では Start() / Stop() または StartTsCapture() / Stop() を必ず対で呼ぶ。
// Reset / ResetAll は異常時・終了時の手動片付け用。
class Reservation_RawTsRecorder final
{
public:
    enum class StartPurpose
    {
        ReservationRecord,
        TsCapture
    };

    struct PacketBlock
    {
        StartPurpose purpose = StartPurpose::ReservationRecord;

        std::uint32_t instance = 0;
        std::uint64_t blockIndex = 0;
        std::uint64_t totalPackets = 0;
        std::uint64_t totalBytes = 0;

        // Worker_ 内の一時 vector を参照する。
        // callback 内だけで使用し、保持しないこと。
        const std::vector<TsPacket>* packets = nullptr;
    };

    using PacketCallback = std::function<void(const PacketBlock&)>;

    struct StartRequest
    {
        std::uint32_t instance = 0;
        IBonDriver2* bon = nullptr;
        std::wstring outputPath;

        // 録画開始前に BonDriver のチャンネルを合わせる。
        // 予約実行側で既に SetChannel 済みなら false。
        bool setChannelBeforeStart = false;
        int space = 0;
        int channel = -1;

        // channel が未指定の場合、config.ini の
        // [EpgServiceEnable_spaceX] から NetworkId-TsId-ServiceId で物理chを解決する。
        bool resolveChannelFromConfigIni = true;

        // 空なら exeDir/config.ini を使用する。
        std::wstring configIniPath;

        // config.ini 上で 0=無効 のサービスでも、チャンネル番号解決には使う。
        // 予約登録済みの番組なら、表示/EPG対象の有効無効で録画開始を止めない方が安全。
        bool allowDisabledServiceForChannelResolve = true;

        // config.ini で見つからない場合の fallback。
        // channellist_spaceX.ch から物理chを自動解決する。
        bool resolveChannelFromChannelList = true;

        // 空なら exeDir/channellist_spaceX.ch を使用する。
        std::wstring channelListPath;

        // 予約データ由来のサービス識別子。
        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;
        std::uint16_t serviceId = 0;
        std::wstring serviceName;

        // チャンネル切替後、信号ロックを待つ。
        bool waitForSignalLock = true;
        int signalLockTimeoutMs = 5000;

        // SetChannel直後に古いTSが残るBonDriver向け。
        // 0なら捨てない。
        DWORD settleSleepMs = 300;
        int discardInitialBlocks = 3;

        // 受信できない時の待機。
        DWORD idleSleepMs = 5;

        // 録画開始後、TSが一定時間まったく来なければ異常扱い。
        // 0なら無効。
        DWORD noDataTimeoutMs = 15000;

        // 一定量ごとにFlushFileBuffersする。
        std::uint64_t flushIntervalBytes = 64ull * 1024ull * 1024ull;

        // 保存先フォルダが無ければ作る。
        bool createParentDirectories = true;

        // 既存 Start() は予約録画用として維持する。
        // TS機能用は StartTsCapture() から Start() へ流す。
        StartPurpose purpose = StartPurpose::ReservationRecord;

        // 受信したTSブロックを照合・解析側へ渡すための入口。
        //
        // 予約録画:
        //   Reservation_TargetVerifier の照合試験へ接続する。
        //
        // TS機能:
        //   必要なら解析・保存済みTS検査側へ接続する。
        //
        // 注意:
        //   Worker_ の受信ループを止めないため、ここで重い解析を直接行わない。
        //   重い処理は解析キューへ渡す形にする。
        PacketCallback onPackets;
    };

    struct TsCaptureRequest
    {
        std::uint32_t instance = 0;
        IBonDriver2* bon = nullptr;
        std::wstring outputPath;

        // TS機能側でチャンネル切替も行う場合だけ true。
        // すでに選局済みの BonDriver から保存するなら false。
        bool setChannelBeforeStart = false;
        int space = 0;
        int channel = -1;

        // TS機能用では予約サービスIDからの自動解決はしない。
        // チャンネル切替が必要なら caller 側で space/channel を指定する。
        bool waitForSignalLock = false;
        int signalLockTimeoutMs = 3000;

        DWORD settleSleepMs = 100;
        int discardInitialBlocks = 0;

        DWORD idleSleepMs = 5;
        DWORD noDataTimeoutMs = 15000;
        std::uint64_t flushIntervalBytes = 64ull * 1024ull * 1024ull;

        bool createParentDirectories = true;

        // TS機能でも必要なら解析側へ渡せる。
        // 予約対象がない単純TS保存なら未設定でよい。
        PacketCallback onPackets;
    };

    struct Result
    {
        bool ok = false;
        std::wstring reason;
        std::wstring outputPath;

        bool channelResolved = false;
        int resolvedChannel = -1;

        bool resolvedFromConfigIni = false;
        bool resolvedServiceEnabled = false;

        bool channelSetTried = false;
        bool channelSetOk = false;

        bool signalLockTried = false;
        bool signalLockOk = false;

        bool noDataTimeout = false;

        std::uint64_t bytesWritten = 0;
        std::uint64_t packetsWritten = 0;
        std::uint64_t blocksWritten = 0;

        std::uint64_t receiveFailures = 0;
        std::uint64_t writeFailures = 0;

        // onPackets callback 側で例外が出た回数。
        // TS保存自体は継続し、照合・解析側の失敗だけ集計する。
        std::uint64_t callbackFailures = 0;

        std::uint64_t discardedBlocks = 0;
        std::uint64_t discardedPackets = 0;
    };

public:
    Reservation_RawTsRecorder() = default;
    ~Reservation_RawTsRecorder() noexcept;

    Reservation_RawTsRecorder(const Reservation_RawTsRecorder&) = delete;
    Reservation_RawTsRecorder& operator=(const Reservation_RawTsRecorder&) = delete;

    // 既存の予約録画用関数。
    // この関数名と基本動作は維持する。
    bool Start(const StartRequest& req, Result* out = nullptr);

    // TS機能用の呼び出し関数。
    // 予約サービスID解決を使わず、space/channel 指定または選局済みBonDriverから保存する。
    bool StartTsCapture(const TsCaptureRequest& req, Result* out = nullptr);

    Result Stop(std::uint32_t instance);

    void Reset(std::uint32_t instance) noexcept;
    void ResetAll() noexcept;

    bool IsRunning(std::uint32_t instance) const;

private:
    struct Session
    {
        std::atomic_bool stopRequested{ false };

        std::uint32_t instance = 0;
        IBonDriver2* bon = nullptr;
        std::wstring outputPath;

        DWORD idleSleepMs = 5;
        DWORD noDataTimeoutMs = 15000;

        std::uint64_t flushIntervalBytes = 64ull * 1024ull * 1024ull;
        std::uint64_t bytesSinceFlush = 0;

        ULONGLONG startTickMs = 0;
        ULONGLONG lastDataTickMs = 0;

        HANDLE file = INVALID_HANDLE_VALUE;
        std::thread worker;

        StartPurpose purpose = StartPurpose::ReservationRecord;
        PacketCallback onPackets;

        mutable std::mutex statMutex;
        Result stat{};
    };

private:
    static void Worker_(const std::shared_ptr<Session>& session) noexcept;

    static Result Snapshot_(const std::shared_ptr<Session>& session) noexcept;

    static void AddReceiveFailure_(
        const std::shared_ptr<Session>& session) noexcept;

    static void AddWriteStats_(
        const std::shared_ptr<Session>& session,
        std::uint64_t bytes,
        std::uint64_t packets) noexcept;

    static void AddCallbackFailure_(
        const std::shared_ptr<Session>& session) noexcept;

    static void AddDiscardStats_(
        const std::shared_ptr<Session>& session,
        std::uint64_t blocks,
        std::uint64_t packets) noexcept;

    static void MarkFailure_(
        const std::shared_ptr<Session>& session,
        const std::wstring& reason) noexcept;

    static void MarkNoDataTimeout_(
        const std::shared_ptr<Session>& session,
        const std::wstring& reason) noexcept;

private:
    mutable std::mutex lifecycleMutex_{};
    mutable std::mutex mutex_{};
    std::unordered_map<std::uint32_t, std::shared_ptr<Session>> sessions_{};
};

#endif // RESERVATION_RAW_TS_RECORDER_HPP