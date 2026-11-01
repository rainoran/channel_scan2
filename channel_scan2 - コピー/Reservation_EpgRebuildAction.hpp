#pragma once
#ifndef RESERVATION_EPG_REBUILD_ACTION_HPP
#define RESERVATION_EPG_REBUILD_ACTION_HPP

#include <cstdint>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace ReservationEpgRebuildAction
{
    struct LatestProgramInfo
    {
        int space = 0;

        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;
        std::uint16_t serviceId = 0;
        std::uint16_t eventId = 0;

        std::wstring serviceKey;
        std::wstring serviceName;

        std::wstring title;
        std::wstring titleLabel;
        std::wstring summary;
        std::wstring genre;

        std::wstring dateText;
        std::wstring weekdayText;
        std::wstring timeText;

        std::wstring startDate;
        std::wstring startTime;
        std::wstring endDate;
        std::wstring endTime;

        std::uint32_t durationSec = 0;
    };

    struct Options
    {
        std::wstring tasksIniPath;

        // EIT再取得後に構築した最新EPG番組リスト。
        // まだ接続前なら空でもよい。その場合は予約キャッシュ更新だけ行う。
        std::vector<LatestProgramInfo> latestPrograms;

        // EventID は同一サービス内でも再利用されるため、EventID だけでは
        // 同一番組と判定しない。開始日が同じで、この範囲内の時刻変更だけを
        // EPG による放送時刻変更として許可する。
        std::uint32_t maxEventTimeShiftMinutes = 6u * 60u;

        // 完全なサービス識別子がある場合でも EventID の同日再利用に備え、
        // 時刻変更を追従するときはタイトル一致も要求する。
        bool requireTitleMatchForStrictEvent = true;

        // NID / TSID が欠けた loose-event 照合では、時刻に加えてタイトルの
        // 整合も必須にする。仮タイトルは開始日時一致側で更新される。
        bool requireTitleMatchForLooseEvent = true;

        // 同日内で見つからなかった予約を、同一サービス・同一タイトルの
        // 後日番組へ追従させる。災害特番など現在枠の別番組は対象にしない。
        bool enableRescheduleFollow = true;

        // 予約元の開始時刻から、この日数以内にある後日番組だけを候補にする。
        std::uint32_t maxRescheduleDays = 14;

        // 実行前なら既存instanceを移動し、開始時刻到達後なら元予約を残したまま
        // 後日分を新instanceで登録する。
        bool createReservationAfterStart = true;

        // テスト時刻差し替え用。空なら system_clock::now() を使用する。
        std::function<std::chrono::system_clock::time_point()> nowProvider;

        // EIT再構築時は、予約内容に変更がなくてもキャッシュを更新する。
        bool forceCacheRebuild = true;

        // 変更があった場合だけ tasks.ini を保存する。
        bool saveTasksWhenChanged = true;

        // EIT再構築後はブラウザ側の予約リスト更新も必要になりやすいので既定 true。
        bool refreshReservationDataJs = true;

        // reservation_data.js 再生成用。
        // 既存の生成関数名が場所により違うため、ここでは callback にしておく。
        std::function<bool(const std::wstring& tasksIniPath)> rebuildReservationDataJs;
    };

    enum class UpdateAction : std::uint8_t
    {
        None = 0,
        UpdateExisting,
        CreateRescheduled,
        DuplicateSkipped,
        AmbiguousSkipped
    };

    struct UpdatedItem
    {
        std::uint32_t kind = 0;
        std::uint32_t instance = 0;
        std::uint32_t sourceInstance = 0;

        UpdateAction action = UpdateAction::None;

        std::wstring beforeTitle;
        std::wstring afterTitle;
        std::wstring reason;

        LatestProgramInfo program;
    };

    struct Result
    {
        bool ok = false;

        std::size_t taskCount = 0;
        std::size_t programCount = 0;
        std::size_t matchedCount = 0;
        std::size_t updatedCount = 0;
        std::size_t updatedReservationCount = 0;
        std::size_t createdReservationCount = 0;
        std::size_t duplicateSkippedCount = 0;
        std::size_t ambiguousSkippedCount = 0;

        bool tasksSaved = false;
        bool reservationCacheUpdated = false;
        bool reservationDataJsUpdated = false;

        std::wstring message;
        std::vector<UpdatedItem> updatedItems;
    };

    Result Run(const Options& options);
}

#endif
