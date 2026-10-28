
#pragma once
#ifndef RESERVATION_TVTEST_WATCH_TEST_HPP
#define RESERVATION_TVTEST_WATCH_TEST_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstdint>
#include <string>

namespace ReservationTvTestWatchTest
{
    enum class Status
    {
        None,
        InvalidArgument,
        ConfigDisabled,
        LaunchFailed,
        PipeUnavailable,
        SetChannelFailed,
        VerifyFailed,
        ProgramMismatch,
        Finished,
        CloseFailed
    };

    struct Request
    {
        int tunerIndex = 0;

        int space = 0;
        int channel = -1;
        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;
        std::uint16_t serviceId = 0;
        std::uint16_t eventId = 0;

        std::wstring title;
        std::wstring serviceName;

        std::wstring startDate; // yyyy-MM-dd
        std::wstring startTime; // HH:mm:ss
        std::wstring endDate;   // yyyy-MM-dd
        std::wstring endTime;   // HH:mm:ss

        bool launchIfNeeded = true;

        // 番組タイトル照合を必須にする。
        // false の場合は、サービス一致だけでも成功扱いにする。
        bool requireProgramTitleMatch = true;

        // 終了時にTVTestを閉じる。
        bool closeTvTestOnFinish = true;

        // true の場合、このユニットが起動したTVTestだけ閉じる。
        // 既に起動済みだったTVTestを閉じたい試験では false にする。
        bool closeOnlyIfLaunchedByThisTest = true;

        // 番組照合が外れた場合に終了する。
        bool closeOnMismatch = true;

        DWORD pingWaitMs = 10000;
        DWORD pingIntervalMs = 250;
        DWORD afterSettleMs = 1000;
        DWORD verifyTimeoutMs = 15000;
        DWORD monitorPollMs = 5000;

        // 試験用。0なら endDate/endTime まで監視する。
        // 30000 などを入れると30秒で打ち切れる。
        DWORD maxMonitorMs = 0;

        // 連続で何回ミスマッチしたら失敗扱いにするか。
        int mismatchLimit = 2;
    };

    struct Result
    {
        bool ok = false;
        Status status = Status::None;

        bool launchedByThisTest = false;
        bool channelSet = false;
        bool verified = false;
        bool closeSent = false;

        std::wstring stage;
        std::wstring message;

        std::wstring pingResponse;
        std::wstring setChannelResponse;
        std::wstring stateResponse;
        std::wstring statusResponse;
        std::wstring programResponse;
        std::wstring closeResponse;
    };

    Result RunWatchTestFromConfig(
        const std::wstring& configIniPath,
        const Request& request);
}

#endif // RESERVATION_TVTEST_WATCH_TEST_HPP