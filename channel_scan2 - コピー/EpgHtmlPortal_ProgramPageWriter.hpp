#pragma once
#ifndef EPG_HTML_PORTAL_PROGRAM_PAGE_WRITER_HPP
#define EPG_HTML_PORTAL_PROGRAM_PAGE_WRITER_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <string>
#include <vector>

#include "EpgHtmlPortal.hpp"

namespace EpgHtmlPortal_ProgramPageWriter
{
    enum class ProgramPageKind
    {
        Current,
        Before
    };

    // 番組表ページの表示種別。
    //
    // Legacy:
    //   既存互換。tableHtml をそのまま1ページに出力する。
    //
    // Timetable:
    //   時間割表示用。timetableHtml または tableHtml を出力する。
    //
    // List:
    //   表一覧表示用。listHtml または tableHtml を出力する。
    enum class ProgramPageViewKind
    {
        Legacy,
        Timetable,
        List
    };

    struct ChannelServiceInfo
    {
        int space = 0;
        int physicalChannel = -1;

        int networkId = 0;
        int tsId = 0;
        int serviceId = 0;

        std::wstring serviceKey;
        std::wstring serviceName;
        std::wstring channelName;
        std::wstring remoteControlKeyId;
    };

    struct ProgramCellMeta
    {
        int space = 0;

        std::wstring title;
        std::wstring dateText;
        std::wstring weekdayText;
        std::wstring timeText;

        std::wstring startDate;
        std::wstring startTime;
        std::wstring endDate;
        std::wstring endTime;
        int durationSec = 0;

        std::wstring serviceKey;
        std::wstring serviceName;
        std::wstring summary;
        std::wstring genre;

        int networkId = 0;
        int tsId = 0;
        int serviceId = 0;
        int eventId = 0;
    };

    struct ProgramPageRequest
    {
        int space = 0;
        ProgramPageKind kind = ProgramPageKind::Current;

        // Legacy / Timetable / List を切り替える。
        // 既存の WriteProgramPage() 呼び出しでは Legacy のままでよい。
        ProgramPageViewKind viewKind = ProgramPageViewKind::Legacy;

        std::wstring outputFileName;
        std::wstring htmlTitle;
        std::wstring spaceLabel;

        // 既存互換用。
        // 旧来の1ページ出力では、この tableHtml をそのまま流し込む。
        std::wstring tableHtml;

        // 分離出力用。
        // 時間割ページでは timetableHtml を優先する。
        // 空の場合は tableHtml を fallback として使う。
        std::wstring timetableHtml;

        // 分離出力用。
        // 表一覧ページでは listHtml を優先する。
        // 空の場合は tableHtml を fallback として使う。
        std::wstring listHtml;

        // 番組表ページから参照できるチャンネル固定情報。
        std::vector<ChannelServiceInfo> services;

        // true の場合、outputDir / serviceMapFileName に
        // window.EPG_SERVICE_MAP を出力する。
        bool writeServiceMap = false;
        std::wstring serviceMapFileName = L"epg_service_map.js";

        // 過去アーカイブ用。
// 例: archiveDateKey = L"20260527"
//     archiveDateLabel = L"2026/05/27"
        std::wstring archiveDateKey;
        std::wstring archiveDateLabel;

        // Current では true、Before/Archive では false が基本。
        bool enableReservationSwitch = true;

        // Before/Archive では true にして、未実装タイムシフトボタンを出す。
        bool enableTimeShiftSwitch = false;

        // 現在・今後の番組表に「この番組名で自動予約」リンクを差し込む。
        // Portal 版の出力先構成を変更する場合は、リンク先を呼び出し側で
        // 相対パスまたは絶対パスとして差し替えられる。
        bool enableAutoReserveAssist = true;
        std::wstring autoReserveEntryUrl = L"epg_auto_reserve.html";

    };

    std::wstring MakeServiceKey(
        int space,
        int networkId,
        int tsId,
        int serviceId);

    bool HasValidServiceIdentity(
        int networkId,
        int tsId,
        int serviceId) noexcept;

    bool HasValidProgramCellIdentity(
        const ProgramCellMeta& meta) noexcept;

    std::wstring BuildServiceHeaderDataAttributes(
        const ChannelServiceInfo& service);

    std::wstring BuildProgramCellDataAttributes(
        const ProgramCellMeta& meta);

    bool WriteServiceMapScript(
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramPageRequest& req);

    // 既存互換。
    // req.tableHtml をそのまま番組表ページとして出力する。
    bool WriteProgramPage(
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramPageRequest& req);

    // 時間割専用ページ出力。
    // req.timetableHtml を優先し、空なら req.tableHtml を使う。
    bool WriteProgramTimetablePage(
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramPageRequest& req);

    // 表一覧専用ページ出力。
    // req.listHtml を優先し、空なら req.tableHtml を使う。
    bool WriteProgramListPage(
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramPageRequest& req);

    // 過去アーカイブ用: 1日分の時間割HTMLを書き出す。
// req.outputFileName には archive/space0/20260527/... のような
// outputDir からの相対パスを指定できる。
    bool WritePastProgramTimetablePage(
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramPageRequest& req);

    // 過去アーカイブ用: 1日分の表一覧HTMLを書き出す。
    bool WritePastProgramListPage(
        const EpgHtmlPortal::WriteOptions& opt,
        const ProgramPageRequest& req);
}

#endif // EPG_HTML_PORTAL_PROGRAM_PAGE_WRITER_HPP
