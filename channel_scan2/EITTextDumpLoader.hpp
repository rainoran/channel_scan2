#pragma once
#ifndef EIT_TEXT_DUMP_LOADER_HPP
#define EIT_TEXT_DUMP_LOADER_HPP

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

// ★ EIT テキストダンプ用テストログスイッチ（デフォルト ON）
#if !defined(ENABLE_EIT_TEXT_DUMP_DEBUG)
#define ENABLE_EIT_TEXT_DUMP_DEBUG 1
#endif

// 1イベント分のテキスト情報
struct EITTextEvent
{
    // 各項目ごとのパース成功フラグ
    struct ParseState {
        bool headerTableId = false;
        bool headerServiceId = false;
        bool headerEventId = false;
        bool headerTsid = false;
        bool headerOnid = false;

        bool versionSection = false;
        bool startTime = false;
        bool duration = false;
        bool runningStatus = false;
        bool freeCA = false;

        bool shortTitle = false;
        bool shortSummary = false;
        bool extendedItems = false;
        bool genre = false;

        void Reset() noexcept {
            headerTableId = false;
            headerServiceId = false;
            headerEventId = false;
            headerTsid = false;
            headerOnid = false;

            versionSection = false;
            startTime = false;
            duration = false;
            runningStatus = false;
            freeCA = false;

            shortTitle = false;
            shortSummary = false;
            extendedItems = false;
            genre = false;
        }
    };

    ParseState parsed;

    // ヘッダ系
    std::uint8_t  tableId = 0;  // 0x4E / 0x50 など
    std::uint16_t serviceId = 0;
    std::uint16_t eventId = 0;
    std::uint16_t tsid = 0;
    std::uint16_t onid = 0;

    std::uint8_t  version = 0;
    std::uint8_t  sectionNumber = 0;
    std::uint8_t  lastSectionNumber = 0;

    // 時刻・時間
    std::wstring  startTimeRawHex;   // "0xEE51..." など
    std::wstring  durationHHMMSS;    // "00:29:00" など
    std::uint8_t  runningStatus = 0;
    bool          freeCA = false;

    // タイトル・概要
    std::wstring  shortTitle;        // [短形式タイトル]
    std::wstring  shortSummary;      // [短形式概要]

    // 拡張items: (label, text)
    std::vector<std::pair<std::wstring, std::wstring>> extendedItems;

    // ジャンル
    std::uint8_t  contentNibbleLevel1 = 0; // primary
    std::uint8_t  contentNibbleLevel2 = 0; // secondary
    std::uint8_t  userNibble1 = 0;
    std::uint8_t  userNibble2 = 0;

    // ===== 追加: パース状態をまとめて判定するヘルパ =====

    // ヘッダ 5項目がそろっているか？
    bool HasHeader() const noexcept {
        return parsed.headerTableId &&
            parsed.headerServiceId &&
            parsed.headerEventId &&
            parsed.headerTsid &&
            parsed.headerOnid;
    }

    // 時刻まわり（startTime + duration）が取れているか？
    bool HasTime() const noexcept {
        return parsed.startTime && parsed.duration;
    }

    // 短形式タイトル & 概要がそろっているか？
    bool HasShortText() const noexcept {
        return parsed.shortTitle && parsed.shortSummary;
    }

    // ジャンル情報が取れているか？
    bool HasGenre() const noexcept {
        return parsed.genre;
    }

    // 一通りそろっているかの簡易チェック（必要なら）
    bool IsBasicallyValid() const noexcept {
        return HasHeader() && HasTime() && parsed.shortTitle;
    }
};

// コールバック型（「関数格納」の受け口）
using EITEventHandler = std::function<void(const EITTextEvent&)>;

// filePath の .epg テキストを読み込み、イベントごとに handler に渡すユニット
bool ForEachEITTextEventInDump(
    const std::wstring& filePath,
    const EITEventHandler& handler);

// テスト用: 項目ごとのパース成否を DebugLog 経由で出力（スイッチ付き）
void RunEITTextDumpFieldTest(const std::wstring& filePath);

#endif // EIT_TEXT_DUMP_LOADER_HPP
