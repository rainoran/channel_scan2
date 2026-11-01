#pragma once
#ifndef EPG_HTML_LITE_HPP
#define EPG_HTML_LITE_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>
#include <functional>

namespace EpgHtmlLite
{
    enum class MenuId : std::uint32_t
    {
        ProgramGuide = 0,
        ReservationList,
        KeywordSearch,
        AutoKeywordReservationList,
        ReservationEdit,
        Controller,
        NewPrograms,
        Recommended,
        Movie,
        Log,
        PastPrograms
    };

    struct MenuLink
    {
        MenuId id{};
        std::wstring label;
        std::wstring fileName;
        bool implemented = false;
    };

    struct ProgramEntry
    {
        std::wstring reserveKey;
        std::wstring serviceName;
        std::wstring title;
        std::wstring description;

        // 表示用。旧ページ互換と最小描画用。
        std::wstring startText;
        std::wstring endText;

        // Lite分割用。EPGファイル読込時に EpgBuild_GUI.cpp 側で確定させる。
        std::wstring startDate;
        std::wstring startTime;
        std::wstring endDate;
        std::wstring endTime;
        std::wstring broadcastDate;
        std::wstring broadcastDateKey;

        std::wstring genreKey = L"all";
        std::wstring broadcastType = L"地上波";
        std::wstring reserveState = L"none";

        std::uint16_t serviceId = 0;
        std::uint16_t eventId = 0;

        int space = 0;
        bool isPast = false;
        int sortOrder = 0;

        // Lite表示の分割キー。
        int dayIndex = -99999;
        int timeBlock = -1;
        int startMinutesOfDay = -1;
        int endMinutesOfDay = -1;
    };

    struct SharedData
    {
        std::vector<ProgramEntry> entries;
    };

    struct Context
    {
        std::wstring mergedEpgPath;
        std::wstring channelListPath;
        std::filesystem::path outputDir;
        std::wstring title = L"EPG";
        int space = 0;
        std::wstring indexFileName = L"index.html";

        // Lite分割生成用。
        std::wstring baseBroadcastDate;
        int futureDays = 8;
        int pastKeepDays = 30;

        const SharedData* sharedData = nullptr;

        // sharedData の独立コピーが完了した直後に1回だけ呼ばれる。
        // BonDriver / EPG取得器 / リングバッファ等の手動解放境界として使う。
        // 未指定なら従来動作。false を返した場合はHTML生成を開始しない。
        // コールバック内では sharedData を変更してよい。
        std::function<bool()> releaseInputResources;
    };

    struct Result
    {
        bool success = false;
        std::wstring indexPath;
        std::vector<std::wstring> generatedFiles;
        std::vector<MenuLink> menuLinks;
    };

    bool Generate(const Context& ctx, Result* outResult = nullptr);
}

#endif // EPG_HTML_LITE_HPP
