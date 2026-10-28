#include "EpgHtmlPortal_SearchData.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <utility>
#include <exception>
#include <array>
#include <windows.h>


#include "DebugManager.h"
#include "EpgHtmlPortal_Common.hpp"
#include "AppSimdPolicy.hpp"

namespace EpgHtmlPortal_SearchData
{
    namespace
    {
        struct SearchEvent
        {
            std::wstring dateText;
            std::wstring weekdayText;
            std::wstring timeText;
            std::wstring title;
            std::wstring summary;
            std::wstring serviceName;
            std::wstring genreName;
            std::uint16_t serviceId = 0;
            std::uint16_t eventId = 0;

            bool hasStart = false;
            int startHour = 0;
            int startMinute = 0;
            int startSecond = 0;

            bool hasDuration = false;
            int durationHour = 0;
            int durationMinute = 0;
            int durationSecond = 0;
        };


        struct SearchDataSettings
        {
            std::wstring searchDataMode = L"CurrentAndFuture";
            int searchStartHour = 4;
            int futureDays = 8;
            int includePastHours = 2;

            bool buildArchive = true;
            std::wstring archiveDir = L"search_archive";
            int archiveDays = 30;
            bool lazyLoadArchive = true;

            std::wstring currentFile = L"epg_search_data.js";
            std::wstring manifestFile = L"epg_search_manifest.js";
        };

        using LocalMarkerScanStats_ =
            AppSimdPolicy::FixedTextScanResult;

        struct MergedParseStats_
        {
            std::uint64_t lines = 0;
            std::uint64_t blankLines = 0;

            std::uint64_t tableIdLines = 0;
            std::uint64_t tableIdOffsetMatches = 0;

            std::uint64_t flushCalls = 0;
            std::uint64_t flushIgnoredNoEvent = 0;

            std::uint64_t accepted = 0;
            std::uint64_t skipNoTitle = 0;
            std::uint64_t titleFromSummary = 0;

            std::uint64_t startParsed = 0;
            std::uint64_t startParseFailed = 0;

            std::uint64_t durationParsed = 0;
            std::uint64_t durationParseFailed = 0;

            std::uint64_t serviceMapHit = 0;
            std::uint64_t serviceMapMiss = 0;
            std::uint64_t serviceSidFallback = 0;

            std::uint64_t genreFallback = 0;

            std::uint64_t extendedSummarySeed = 0;
            std::uint64_t extendedSummaryAppend = 0;
        };

        bool CollectProgramMarkerOffsets_(
            const std::wstring& html,
            std::vector<std::size_t>& outOffsets,
            LocalMarkerScanStats_* outStats) noexcept
        {
            return AppSimdPolicy::CollectFixedTextOffsetsFromCurrentConfig(
                AppSimdPolicy::BlockKind::EpgFixedTextSearch,
                html,
                L"data-epg-program=\"1\"",
                outOffsets,
                outStats);
        }

        bool CollectTableIdOffsets_(
            const std::wstring& epgText,
            std::vector<std::size_t>& outOffsets,
            LocalMarkerScanStats_* outStats) noexcept
        {
            return AppSimdPolicy::CollectFixedTextOffsetsFromCurrentConfig(
                AppSimdPolicy::BlockKind::EpgBlockOffsetScan,
                epgText,
                L"TableID",
                outOffsets,
                outStats);
        }

        void LogSearchMarkerScanStats_(
            const LocalMarkerScanStats_& stats,
            const wchar_t* tag) noexcept
        {
            try {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][SCAN] tag=%ls block=%ls vendor=%ls requested=%ls policyActual=%ls executed=%ls kernel=%ls fallback=%d forcedScalar=%d backendPrepared=%d backendUsed=%d backendDeferred=%d simdTried=%d matches=%zu chars=%zu ms=%llu reason=%ls fallbackReason=%ls",
                    tag ? tag : L"",
                    AppSimdPolicy::ToText(stats.policy.blockKind),
                    AppSimdPolicy::ToText(stats.policy.vendor),
                    AppSimdPolicy::ToText(stats.policy.requested),
                    AppSimdPolicy::ToText(stats.policy.actual),
                    AppSimdPolicy::ToText(stats.executedPath),
                    AppSimdPolicy::ToText(stats.policy.kernel),
                    stats.policy.fallback ? 1 : 0,
                    stats.policy.forcedScalarBlock ? 1 : 0,
                    stats.backendPrepared ? 1 : 0,
                    stats.backendUsed ? 1 : 0,
                    stats.backendDeferred ? 1 : 0,
                    stats.simdTried ? 1 : 0,
                    stats.matchCount,
                    stats.textChars,
                    static_cast<unsigned long long>(stats.elapsedMs),
                    stats.policy.reason ? stats.policy.reason : L"",
                    stats.fallbackReason.c_str());
            }
            catch (...) {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][SCAN][WARN] log failed: tag=%ls",
                    tag ? tag : L"");
            }
        }

        void LogMergedParseStats_(
            int space,
            const std::filesystem::path& filePath,
            const MergedParseStats_& stats) noexcept
        {
            try {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][MERGED][STATS] space=%d lines=%llu blank=%llu tableOffsetMatches=%llu tableLines=%llu flush=%llu flushNoEvent=%llu accepted=%llu skipNoTitle=%llu titleFromSummary=%llu startOk=%llu startNg=%llu durationOk=%llu durationNg=%llu serviceHit=%llu serviceMiss=%llu sidFallback=%llu genreFallback=%llu extSeed=%llu extAppend=%llu path=%ls",
                    space,
                    static_cast<unsigned long long>(stats.lines),
                    static_cast<unsigned long long>(stats.blankLines),
                    static_cast<unsigned long long>(stats.tableIdOffsetMatches),
                    static_cast<unsigned long long>(stats.tableIdLines),
                    static_cast<unsigned long long>(stats.flushCalls),
                    static_cast<unsigned long long>(stats.flushIgnoredNoEvent),
                    static_cast<unsigned long long>(stats.accepted),
                    static_cast<unsigned long long>(stats.skipNoTitle),
                    static_cast<unsigned long long>(stats.titleFromSummary),
                    static_cast<unsigned long long>(stats.startParsed),
                    static_cast<unsigned long long>(stats.startParseFailed),
                    static_cast<unsigned long long>(stats.durationParsed),
                    static_cast<unsigned long long>(stats.durationParseFailed),
                    static_cast<unsigned long long>(stats.serviceMapHit),
                    static_cast<unsigned long long>(stats.serviceMapMiss),
                    static_cast<unsigned long long>(stats.serviceSidFallback),
                    static_cast<unsigned long long>(stats.genreFallback),
                    static_cast<unsigned long long>(stats.extendedSummarySeed),
                    static_cast<unsigned long long>(stats.extendedSummaryAppend),
                    filePath.wstring().c_str());
            }
            catch (...) {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][MERGED][STATS][WARN] log failed: space=%d path=%ls",
                    space,
                    filePath.wstring().c_str());
            }
        }

        std::filesystem::path FindConfigIniPath_()
        {
            try {
                wchar_t modulePath[MAX_PATH]{};
                const DWORD n = ::GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
                if (n > 0 && n < MAX_PATH) {
                    return std::filesystem::path(modulePath).parent_path() / L"config.ini";
                }
            }
            catch (...) {
            }

            try {
                return std::filesystem::current_path() / L"config.ini";
            }
            catch (...) {
            }

            return {};
        }

        std::wstring ReadIniString_(
            const std::filesystem::path& iniPath,
            const wchar_t* section,
            const wchar_t* key,
            const wchar_t* defValue)
        {
            wchar_t buf[512]{};
            ::GetPrivateProfileStringW(
                section,
                key,
                defValue,
                buf,
                static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])),
                iniPath.c_str());

            return std::wstring(buf);
        }

        int ReadIniInt_(
            const std::filesystem::path& iniPath,
            const wchar_t* section,
            const wchar_t* key,
            int defValue)
        {
            return static_cast<int>(
                ::GetPrivateProfileIntW(
                    section,
                    key,
                    defValue,
                    iniPath.c_str()));
        }

        bool ReadIniBool_(
            const std::filesystem::path& iniPath,
            const wchar_t* section,
            const wchar_t* key,
            bool defValue)
        {
            return ReadIniInt_(iniPath, section, key, defValue ? 1 : 0) != 0;
        }

        bool IniKeyExists_(
            const std::filesystem::path& iniPath,
            const wchar_t* section,
            const wchar_t* key)
        {
            wchar_t buf[64]{};
            ::GetPrivateProfileStringW(
                section,
                key,
                L"__EPG_MISSING__",
                buf,
                static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])),
                iniPath.c_str());

            return std::wstring(buf) != L"__EPG_MISSING__";
        }

        void WriteIniDefaultString_(
            const std::filesystem::path& iniPath,
            const wchar_t* section,
            const wchar_t* key,
            const wchar_t* value)
        {
            if (iniPath.empty()) return;

            if (!IniKeyExists_(iniPath, section, key)) {
                ::WritePrivateProfileStringW(section, key, value, iniPath.c_str());
            }
        }

        void WriteIniDefaultInt_(
            const std::filesystem::path& iniPath,
            const wchar_t* section,
            const wchar_t* key,
            int value)
        {
            wchar_t buf[64]{};
            swprintf_s(buf, L"%d", value);
            WriteIniDefaultString_(iniPath, section, key, buf);
        }

        void EnsureSearchDataConfigDefaults_(const std::filesystem::path& iniPath)
        {
            if (iniPath.empty()) return;

            constexpr const wchar_t* sec = L"EPGSearch";

            WriteIniDefaultString_(iniPath, sec, L"SearchDataMode", L"CurrentAndFuture");
            WriteIniDefaultInt_(iniPath, sec, L"SearchStartHour", 4);
            WriteIniDefaultInt_(iniPath, sec, L"FutureDays", 8);
            WriteIniDefaultInt_(iniPath, sec, L"IncludePastHours", 2);

            WriteIniDefaultInt_(iniPath, sec, L"BuildArchive", 1);
            WriteIniDefaultString_(iniPath, sec, L"ArchiveDir", L"search_archive");
            WriteIniDefaultInt_(iniPath, sec, L"ArchiveDays", 30);
            WriteIniDefaultInt_(iniPath, sec, L"LazyLoadArchive", 1);

            WriteIniDefaultString_(iniPath, sec, L"CurrentFile", L"epg_search_data.js");
            WriteIniDefaultString_(iniPath, sec, L"ManifestFile", L"epg_search_manifest.js");
        }

        SearchDataSettings LoadSearchDataSettings_()
        {
            SearchDataSettings s{};

            const std::filesystem::path iniPath = FindConfigIniPath_();
            if (iniPath.empty()) {
                return s;
            }

            EnsureSearchDataConfigDefaults_(iniPath);

            constexpr const wchar_t* sec = L"EPGSearch";

            s.searchDataMode = ReadIniString_(iniPath, sec, L"SearchDataMode", L"CurrentAndFuture");
            s.searchStartHour = (std::clamp)(ReadIniInt_(iniPath, sec, L"SearchStartHour", 4), 0, 23);
            s.futureDays = (std::clamp)(ReadIniInt_(iniPath, sec, L"FutureDays", 8), 1, 31);
            s.includePastHours = (std::clamp)(ReadIniInt_(iniPath, sec, L"IncludePastHours", 2), 0, 24);

            s.buildArchive = ReadIniBool_(iniPath, sec, L"BuildArchive", true);
            s.archiveDir = ReadIniString_(iniPath, sec, L"ArchiveDir", L"search_archive");
            if (s.archiveDir.empty()) {
                s.archiveDir = L"search_archive";
            }

            s.archiveDays = (std::clamp)(ReadIniInt_(iniPath, sec, L"ArchiveDays", 30), 0, 3650);
            s.lazyLoadArchive = ReadIniBool_(iniPath, sec, L"LazyLoadArchive", true);

            s.currentFile = ReadIniString_(iniPath, sec, L"CurrentFile", L"epg_search_data.js");
            if (s.currentFile.empty()) {
                s.currentFile = L"epg_search_data.js";
            }

            s.manifestFile = ReadIniString_(iniPath, sec, L"ManifestFile", L"epg_search_manifest.js");
            if (s.manifestFile.empty()) {
                s.manifestFile = L"epg_search_manifest.js";
            }

            return s;
        }
        // ------------------------------------------------------------
        // 軽量検索DB設定
        // ------------------------------------------------------------
        // index.html の初期ロード確認用に、検索用JSを軽くする。
        // EpgMerged_spaceX_All_EIT など元データは変更しない。
        // 重い summary / 過去番組検索DB / HTML fallback を抑制して、
        // 「検索DBがindex停止の原因か」を切り分ける目的の設定。
        constexpr bool kSearchDataLightMode_ = true;

        // merged EPG が読めなかった場合に巨大HTMLを読み直す fallback は、
        // index停止原因の切り分け時には無効にする。
        constexpr bool kEnableHtmlFallbackInLightMode_ = true;

        // 0 は件数制限なし。
// 番組表に表示されている番組が検索DBから欠けるのを防ぐ。
        constexpr std::size_t kMaxNowSearchEventsPerSpace_ = 0;

        // 過去番組側は index 初期ロードへの負荷が大きいため、まずは0件にする。
        constexpr std::size_t kMaxPastSearchEventsPerSpace_ = 0;

        // JSへ吐く文字列の最大長。summary は最初は空で出す。
        constexpr std::size_t kMaxSearchTitleChars_ = 96;
        constexpr std::size_t kMaxSearchTimeChars_ = 48;
        constexpr std::size_t kMaxSearchServiceChars_ = 64;
        constexpr std::size_t kMaxSearchGenreChars_ = 32;
        constexpr std::size_t kMaxSearchSummaryChars_ = 0;

        std::wstring TruncateSearchText_(std::wstring text, std::size_t maxChars)
        {
            text = EpgHtmlPortal_Common::TrimW(std::move(text));

            if (maxChars == 0) {
                return {};
            }
            if (text.size() <= maxChars) {
                return text;
            }

            text.resize(maxChars);
            text += L"…";
            return text;
        }

        void CompactSearchEvent_(SearchEvent& ev)
        {
            if (!kSearchDataLightMode_) {
                return;
            }

            ev.dateText = TruncateSearchText_(std::move(ev.dateText), 16);
            ev.weekdayText = TruncateSearchText_(std::move(ev.weekdayText), 4);
            ev.timeText = TruncateSearchText_(std::move(ev.timeText), kMaxSearchTimeChars_);
            ev.title = TruncateSearchText_(std::move(ev.title), kMaxSearchTitleChars_);
            ev.summary = TruncateSearchText_(std::move(ev.summary), kMaxSearchSummaryChars_);
            ev.serviceName = TruncateSearchText_(std::move(ev.serviceName), kMaxSearchServiceChars_);
            ev.genreName = TruncateSearchText_(std::move(ev.genreName), kMaxSearchGenreChars_);
        }

        constexpr bool SearchShouldKeepSummaryForOutput_() noexcept
        {
            if constexpr (kSearchDataLightMode_) {
                if constexpr (kMaxSearchSummaryChars_ == 0u) {
                    return false;
                }
                else {
                    return true;
                }
            }
            else {
                return true;
            }
        }

        constexpr bool SearchShouldUseSummaryAsTitleFallback_() noexcept
        {
            // title 欠損時の保険だけは残す。
            // 現在のEPGはかなり綺麗だが、少数の欠損や変則データ対策として使う。
            return true;
        }

        void CompactSearchEvents_(std::vector<SearchEvent>& events, std::size_t maxCount)
        {
            if (!kSearchDataLightMode_) {
                return;
            }

            if (maxCount > 0 && events.size() > maxCount) {
                events.resize(maxCount);
            }

            for (auto& ev : events) {
                CompactSearchEvent_(ev);
            }
        }

        bool HasAnySearchEvents_(
            const std::vector<std::vector<SearchEvent>>& bySpace)
        {
            for (const auto& events : bySpace) {
                if (!events.empty()) {
                    return true;
                }
            }

            return false;
        }

        void CompactSearchDataset_(
            std::vector<std::vector<SearchEvent>>& bySpace,
            std::size_t maxCountPerSpace)
        {
            if (!kSearchDataLightMode_) {
                return;
            }

            for (auto& events : bySpace) {
                CompactSearchEvents_(events, maxCountPerSpace);
            }
        }

        void CompactArchiveSearchDataset_(
            std::map<std::wstring, std::vector<std::vector<SearchEvent>>>& archiveByDate)
        {
            if (!kSearchDataLightMode_) {
                return;
            }

            for (auto it = archiveByDate.begin(); it != archiveByDate.end(); ) {
                auto& bySpace = it->second;

                if (bySpace.empty()) {
                    bySpace.resize(3);
                }

                for (auto& events : bySpace) {
                    CompactSearchEvents_(events, kMaxPastSearchEventsPerSpace_);
                }

                if (!HasAnySearchEvents_(bySpace)) {
                    it = archiveByDate.erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        bool ShouldBuildArchiveFiles_(const SearchDataSettings& settings)
        {
            if (!settings.buildArchive) {
                return false;
            }

            // light mode で過去検索を0件にする設定なら、
            // archive ファイルも生成しない。
            // index 初期表示と通常検索の軽量化を優先する。
            if (kSearchDataLightMode_ && kMaxPastSearchEventsPerSpace_ == 0) {
                return false;
            }

            return true;
        }

        std::wstring ExtractValueAfterColon(std::wstring_view line)
        {
            const std::size_t pos = line.find(L':');
            if (pos == std::wstring_view::npos) {
                return {};
            }
            return EpgHtmlPortal_Common::TrimW(std::wstring(line.substr(pos + 1)));
        }

        std::wstring ExtractValueAfterPrefix(std::wstring_view line, std::wstring_view prefix)
        {
            if (line.rfind(prefix, 0) != 0) {
                return {};
            }
            return EpgHtmlPortal_Common::TrimW(std::wstring(line.substr(prefix.size())));
        }

        std::wstring GenreNameFromPrimary(int primary)
        {
            switch (primary) {
            case 0x0: return L"ニュース・報道";
            case 0x1: return L"スポーツ";
            case 0x2: return L"情報・ワイドショー";
            case 0x3: return L"ドラマ";
            case 0x4: return L"音楽";
            case 0x5: return L"バラエティ";
            case 0x6: return L"映画";
            case 0x7: return L"アニメ・特撮";
            case 0x8: return L"ドキュメンタリー";
            case 0x9: return L"劇場・公演";
            case 0xA: return L"趣味・教育";
            case 0xB: return L"福祉";
            default:  return L"--";
            }
        }

        bool DecodeMjdToDate(int mjd, int& year, int& month, int& day)
        {
            long j = static_cast<long>(mjd) + 2400001L + 68569L;
            long c = 4L * j / 146097L;
            j = j - (146097L * c + 3L) / 4L;
            long y = 4000L * (j + 1L) / 1461001L;
            j = j - 1461L * y / 4L + 31L;
            long m = 80L * j / 2447L;
            long d = j - 2447L * m / 80L;
            j = m / 11L;
            m = m + 2L - 12L * j;
            y = 100L * (c - 49L) + y + j;

            year = static_cast<int>(y);
            month = static_cast<int>(m);
            day = static_cast<int>(d);
            return true;
        }

        int DecodeBcdByte(std::uint8_t v)
        {
            return static_cast<int>(((v >> 4) & 0x0F) * 10 + (v & 0x0F));
        }

        bool ParseStartHex(
            std::wstring_view value,
            int& year,
            int& month,
            int& day,
            int& hour,
            int& minute,
            int& second)
        {
            const std::size_t hexPos = value.find(L"0x");
            if (hexPos == std::wstring_view::npos) {
                return false;
            }

            std::wstring hex;
            hex.reserve(16);

            for (std::size_t i = hexPos + 2; i < value.size(); ++i) {
                const wchar_t c = value[i];
                const bool isHex =
                    (c >= L'0' && c <= L'9') ||
                    (c >= L'A' && c <= L'F') ||
                    (c >= L'a' && c <= L'f');

                if (!isHex) {
                    break;
                }

                hex.push_back(c);

                // MJD(16bit)+BCD時刻(24bit)なので本来10桁。
                // 余計な文字列で stoull が重くならないよう抑える。
                if (hex.size() >= 16) {
                    break;
                }
            }

            if (hex.size() < 10) {
                return false;
            }

            std::uint64_t raw = 0;
            try {
                raw = static_cast<std::uint64_t>(std::stoull(hex, nullptr, 16));
            }
            catch (...) {
                return false;
            }

            const int mjd = static_cast<int>((raw >> 24) & 0xFFFFULL);

            DecodeMjdToDate(mjd, year, month, day);
            hour = DecodeBcdByte(static_cast<std::uint8_t>((raw >> 16) & 0xFFULL));
            minute = DecodeBcdByte(static_cast<std::uint8_t>((raw >> 8) & 0xFFULL));
            second = DecodeBcdByte(static_cast<std::uint8_t>(raw & 0xFFULL));

            return true;
        }

        void AddDuration(
            int hour,
            int minute,
            int second,
            int addHour,
            int addMinute,
            int addSecond,
            int& outHour,
            int& outMinute,
            int& outSecond)
        {
            int total =
                hour * 3600 + minute * 60 + second +
                addHour * 3600 + addMinute * 60 + addSecond;

            total %= (24 * 3600);
            if (total < 0) {
                total += 24 * 3600;
            }

            outHour = total / 3600;
            outMinute = (total % 3600) / 60;
            outSecond = total % 60;
        }

        std::wstring FormatDate(int year, int month, int day)
        {
            std::wostringstream oss;
            oss << year << L"-";
            if (month < 10) {
                oss << L'0';
            }
            oss << month << L"-";
            if (day < 10) {
                oss << L'0';
            }
            oss << day;
            return oss.str();
        }

        std::wstring GetWeekdayText(int year, int month, int day)
        {
            std::tm tmv{};
            tmv.tm_year = year - 1900;
            tmv.tm_mon = month - 1;
            tmv.tm_mday = day;
            tmv.tm_hour = 12;
            std::mktime(&tmv);

            static constexpr const wchar_t* kWeek[7] = {
                L"日", L"月", L"火", L"水", L"木", L"金", L"土"
            };

            if (tmv.tm_wday < 0 || tmv.tm_wday >= 7) {
                return L"--";
            }
            return kWeek[static_cast<std::size_t>(tmv.tm_wday)];
        }

        std::wstring FormatTimeRange(int sh, int sm, int eh, int em)
        {
            std::wostringstream oss;

            if (sh < 10) {
                oss << L'0';
            }
            oss << sh << L":";

            if (sm < 10) {
                oss << L'0';
            }
            oss << sm << L"-";

            if (eh < 10) {
                oss << L'0';
            }
            oss << eh << L":";

            if (em < 10) {
                oss << L'0';
            }
            oss << em;

            return oss.str();
        }

        void RebuildSearchEventTimeText_(SearchEvent& ev)
        {
            if (!ev.hasStart) {
                return;
            }

            if (ev.hasDuration) {
                int eh = 0;
                int em = 0;
                int es = 0;

                AddDuration(
                    ev.startHour,
                    ev.startMinute,
                    ev.startSecond,
                    ev.durationHour,
                    ev.durationMinute,
                    ev.durationSecond,
                    eh,
                    em,
                    es);

                ev.timeText = FormatTimeRange(
                    ev.startHour,
                    ev.startMinute,
                    eh,
                    em);
                return;
            }

            ev.timeText = FormatTimeRange(
                ev.startHour,
                ev.startMinute,
                ev.startHour,
                ev.startMinute);
        }

        std::map<std::uint16_t, std::wstring> BuildServiceMap(
            const EpgHtmlPortal::WriteOptions& opt,
            int space)
        {
            std::map<std::uint16_t, std::wstring> out;

            if (space < 0 ||
                static_cast<std::size_t>(space) >= opt.enabledServicesBySpace.size()) {
                return out;
            }

            for (const auto& e : opt.enabledServicesBySpace[static_cast<std::size_t>(space)]) {
                if (e.serviceId == 0 || e.serviceName.empty()) {
                    continue;
                }
                out.emplace(e.serviceId, e.serviceName);
            }

            return out;
        }

        std::wstring HtmlDecodeBasic_(std::wstring_view src)
        {
            std::wstring out;
            out.reserve(src.size());

            for (std::size_t i = 0; i < src.size(); ++i) {
                if (src[i] != L'&') {
                    out.push_back(src[i]);
                    continue;
                }

                const std::size_t semi = src.find(L';', i + 1);
                if (semi == std::wstring_view::npos) {
                    out.push_back(src[i]);
                    continue;
                }

                const std::wstring ent(src.substr(i, semi - i + 1));

                if (ent == L"&amp;") {
                    out.push_back(L'&');
                    i = semi;
                    continue;
                }
                if (ent == L"&lt;") {
                    out.push_back(L'<');
                    i = semi;
                    continue;
                }
                if (ent == L"&gt;") {
                    out.push_back(L'>');
                    i = semi;
                    continue;
                }
                if (ent == L"&quot;") {
                    out.push_back(L'"');
                    i = semi;
                    continue;
                }
                if (ent == L"&#39;" || ent == L"&apos;") {
                    out.push_back(L'\'');
                    i = semi;
                    continue;
                }

                if (ent.size() >= 4 && ent[1] == L'#') {
                    try {
                        int code = 0;

                        if (ent.size() >= 5 && (ent[2] == L'x' || ent[2] == L'X')) {
                            code = std::stoi(ent.substr(3, ent.size() - 4), nullptr, 16);
                        }
                        else {
                            code = std::stoi(ent.substr(2, ent.size() - 3), nullptr, 10);
                        }

                        if (code > 0 && code <= 0xFFFF) {
                            out.push_back(static_cast<wchar_t>(code));
                            i = semi;
                            continue;
                        }
                    }
                    catch (...) {
                    }
                }

                out.append(ent);
                i = semi;
            }

            return EpgHtmlPortal_Common::TrimW(std::move(out));
        }

        std::wstring ExtractHtmlAttribute_(
            std::wstring_view tag,
            std::wstring_view attrName)
        {
            const std::wstring key = std::wstring(attrName) + L"=\"";
            const std::size_t pos = tag.find(key);
            if (pos == std::wstring_view::npos) {
                return {};
            }

            const std::size_t begin = pos + key.size();
            const std::size_t end = tag.find(L'"', begin);
            if (end == std::wstring_view::npos || end <= begin) {
                return {};
            }

            return HtmlDecodeBasic_(tag.substr(begin, end - begin));
        }

        std::uint16_t ToUInt16Loose_(std::wstring_view src) noexcept
        {
            auto isWs = [](wchar_t ch) noexcept {
                return ch == L' ' ||
                    ch == L'\t' ||
                    ch == L'\r' ||
                    ch == L'\n' ||
                    ch == L'\u3000';
                };

            while (!src.empty() && isWs(src.front())) {
                src.remove_prefix(1);
            }

            while (!src.empty() && isWs(src.back())) {
                src.remove_suffix(1);
            }

            if (src.empty()) {
                return 0;
            }

            int base = 10;

            if (src.size() >= 3 &&
                src[0] == L'0' &&
                (src[1] == L'x' || src[1] == L'X')) {
                base = 16;
                src.remove_prefix(2);
            }

            std::uint32_t value = 0;
            bool any = false;

            for (const wchar_t c : src) {
                int digit = -1;

                if (c >= L'0' && c <= L'9') {
                    digit = static_cast<int>(c - L'0');
                }
                else if (base == 16 && c >= L'A' && c <= L'F') {
                    digit = 10 + static_cast<int>(c - L'A');
                }
                else if (base == 16 && c >= L'a' && c <= L'f') {
                    digit = 10 + static_cast<int>(c - L'a');
                }
                else {
                    break;
                }

                if (digit < 0 || digit >= base) {
                    break;
                }

                any = true;
                value = value * static_cast<std::uint32_t>(base) +
                    static_cast<std::uint32_t>(digit);

                if (value > 0xFFFFu) {
                    return 0;
                }
            }

            return any ? static_cast<std::uint16_t>(value) : 0;
        }

        bool IsLooseSeparatorForSearchData_(wchar_t ch) noexcept
        {
            return ch == L' ' ||
                ch == L'\t' ||
                ch == L':' ||
                ch == L'=' ||
                ch == L'\u3000';
        }

        bool IsHexDigitForSearchData_(wchar_t ch) noexcept
        {
            return (ch >= L'0' && ch <= L'9') ||
                (ch >= L'A' && ch <= L'F') ||
                (ch >= L'a' && ch <= L'f');
        }

        std::wstring ExtractLooseNumberTokenAfterKey_(
            std::wstring_view line,
            std::wstring_view key)
        {
            const std::size_t keyPos = line.find(key);
            if (keyPos == std::wstring_view::npos) {
                return {};
            }

            std::size_t pos = keyPos + key.size();

            while (pos < line.size() &&
                IsLooseSeparatorForSearchData_(line[pos])) {
                ++pos;
            }

            if (pos >= line.size()) {
                return {};
            }

            const std::size_t start = pos;

            if (pos + 2 <= line.size() &&
                line[pos] == L'0' &&
                (line[pos + 1] == L'x' || line[pos + 1] == L'X')) {
                pos += 2;

                while (pos < line.size() &&
                    IsHexDigitForSearchData_(line[pos])) {
                    ++pos;
                }

                if (pos <= start + 2) {
                    return {};
                }

                return std::wstring(line.substr(start, pos - start));
            }

            while (pos < line.size() &&
                line[pos] >= L'0' &&
                line[pos] <= L'9') {
                ++pos;
            }

            if (pos <= start) {
                return {};
            }

            return std::wstring(line.substr(start, pos - start));
        }

        std::wstring MakeHtmlEventDedupeKey_(const SearchEvent& ev)
        {
            if (ev.serviceId != 0 || ev.eventId != 0) {
                return L"sid:" + std::to_wstring(ev.serviceId) +
                    L"|eid:" + std::to_wstring(ev.eventId);
            }

            return L"svc:" + ev.serviceName +
                L"|date:" + ev.dateText +
                L"|time:" + ev.timeText +
                L"|title:" + ev.title;
        }

        std::wstring MakeSearchEventKey_(const SearchEvent& ev)
        {
            if (ev.serviceId != 0 && ev.eventId != 0) {
                return L"sid:" + std::to_wstring(ev.serviceId) +
                    L"|eid:" + std::to_wstring(ev.eventId);
            }

            return L"date:" + ev.dateText +
                L"|time:" + ev.timeText +
                L"|title:" + ev.title +
                L"|svc:" + ev.serviceName;
        }

        std::unordered_set<std::uint16_t> CollectActiveServiceIds_(
            const std::vector<SearchEvent>& htmlEvents)
        {
            std::unordered_set<std::uint16_t> out;

            for (const auto& ev : htmlEvents) {
                if (ev.serviceId != 0) {
                    out.insert(ev.serviceId);
                }
            }

            return out;
        }

        bool IsSidFallbackServiceName_(std::wstring_view s)
        {
            return s.rfind(L"SID ", 0) == 0;
        }

        std::map<std::uint16_t, std::uint16_t> BuildBackupServiceAliasMap_(
            const EpgHtmlPortal::WriteOptions& /*opt*/,
            int /*space*/,
            const std::unordered_set<std::uint16_t>& /*activeServiceIds*/)
        {
            // 現在の EpgHtmlPortal::ServiceEntry は serviceId / serviceName のみ。
            // networkId / tsId / physicalChannel が無いため、
            // ここではバックアップSIDの自動エイリアスは作らない。
            //
            // 今後、ServiceEntry または検索DB専用メタ情報に
            // physicalChannel / networkId / tsId を追加した段階で復活させる。
            return {};
        }

        void ApplyServiceAlias_(
            std::vector<SearchEvent>& events,
            const std::map<std::uint16_t, std::uint16_t>& aliasMap,
            const std::map<std::uint16_t, std::wstring>& serviceMap)
        {
            if (aliasMap.empty()) {
                return;
            }

            for (auto& ev : events) {
                const auto it = aliasMap.find(ev.serviceId);
                if (it == aliasMap.end()) {
                    continue;
                }

                ev.serviceId = it->second;

                const auto nameIt = serviceMap.find(ev.serviceId);
                if (nameIt != serviceMap.end()) {
                    ev.serviceName = nameIt->second;
                }
            }
        }

        void MergeSearchEvents_(
            std::vector<SearchEvent>& dst,
            const std::vector<SearchEvent>& src)
        {
            std::unordered_set<std::wstring> seen;
            seen.reserve(dst.size() + src.size());

            for (const auto& ev : dst) {
                seen.insert(MakeSearchEventKey_(ev));
            }

            for (const auto& ev : src) {
                const std::wstring key = MakeSearchEventKey_(ev);
                if (seen.insert(key).second) {
                    dst.push_back(ev);
                }
            }
        }

        void FilterEventsByActiveServices_(
            std::vector<SearchEvent>& events,
            const std::unordered_set<std::uint16_t>& activeServiceIds)
        {
            if (activeServiceIds.empty()) {
                return;
            }

            events.erase(
                std::remove_if(
                    events.begin(),
                    events.end(),
                    [&](const SearchEvent& ev) {
                        if (ev.serviceId == 0) {
                            return false;
                        }

                        return activeServiceIds.find(ev.serviceId) == activeServiceIds.end();
                    }),
                events.end());
        }

        std::filesystem::path MakeStandardProgramHtmlPath_(
            const EpgHtmlPortal::WriteOptions& opt,
            int space)
        {
            if (space == opt.space && !opt.currentHtmlPath.empty()) {
                return opt.currentHtmlPath;
            }

            return opt.outputDir /
                (L"epg_space" + std::to_wstring(space) + L".html");
        }

        std::filesystem::path MakeStandardBeforeProgramHtmlPath_(
            const EpgHtmlPortal::WriteOptions& opt,
            int space)
        {
            if (space == opt.space && !opt.beforeHtmlPath.empty()) {
                return opt.beforeHtmlPath;
            }

            return opt.outputDir /
                (L"epg_space" + std::to_wstring(space) + L"_before.html");
        }

        std::wstring GetTodayDateText_()
        {
            std::time_t now = std::time(nullptr);
            std::tm tmv{};

#ifdef _WIN32
            localtime_s(&tmv, &now);
#else
            tmv = *std::localtime(&now);
#endif

            return FormatDate(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        }

        bool ParseIsoDateText_(
            std::wstring_view s,
            int& y,
            int& m,
            int& d)
        {
            if (s.size() < 10) return false;

            try {
                y = std::stoi(std::wstring(s.substr(0, 4)));
                m = std::stoi(std::wstring(s.substr(5, 2)));
                d = std::stoi(std::wstring(s.substr(8, 2)));
                return y >= 1900 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
            }
            catch (...) {
                return false;
            }
        }

        std::wstring AddDaysToIsoDate_(std::wstring_view isoDate, int deltaDays)
        {
            int y = 0;
            int m = 0;
            int d = 0;

            if (!ParseIsoDateText_(isoDate, y, m, d)) {
                return {};
            }

            std::tm tmv{};
            tmv.tm_year = y - 1900;
            tmv.tm_mon = m - 1;
            tmv.tm_mday = d;
            tmv.tm_hour = 12;
            tmv.tm_isdst = -1;

            std::time_t t = std::mktime(&tmv);
            if (t == static_cast<std::time_t>(-1)) {
                return {};
            }

            t += static_cast<std::time_t>(deltaDays) * 24 * 60 * 60;

#ifdef _WIN32
            localtime_s(&tmv, &t);
#else
            tmv = *std::localtime(&t);
#endif

            return FormatDate(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        }

        std::wstring GetBroadcastBaseDateText_(int searchStartHour)
        {
            std::time_t now = std::time(nullptr);
            std::tm tmv{};

#ifdef _WIN32
            localtime_s(&tmv, &now);
#else
            tmv = *std::localtime(&now);
#endif

            // 04:00前なら前日の放送日扱い。
            if (tmv.tm_hour < searchStartHour) {
                now -= 24 * 60 * 60;
#ifdef _WIN32
                localtime_s(&tmv, &now);
#else
                tmv = *std::localtime(&now);
#endif
            }

            return FormatDate(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        }

        std::wstring MakeDateCompact_(std::wstring_view isoDate)
        {
            std::wstring out;
            out.reserve(8);

            for (wchar_t ch : isoDate) {
                if (ch >= L'0' && ch <= L'9') {
                    out.push_back(ch);
                }
            }

            return out;
        }

        bool IsIsoDateInRange_(
            std::wstring_view date,
            std::wstring_view beginInclusive,
            std::wstring_view endExclusive)
        {
            if (date.size() < 10) return false;
            return date >= beginInclusive && date < endExclusive;
        }

        bool IsPastEventByDate_(
            const SearchEvent& ev,
            std::wstring_view todayDate)
        {
            if (ev.dateText.empty()) {
                return false;
            }

            // dateText は "YYYY-MM-DD" 前提。
            // 形式が崩れている場合は現在側へ逃がす。
            if (ev.dateText.size() < 10) {
                return false;
            }

            return ev.dateText < todayDate;
        }

        void SplitEventsByToday_(
            const std::vector<SearchEvent>& src,
            std::wstring_view todayDate,
            std::vector<SearchEvent>& nowEvents,
            std::vector<SearchEvent>& pastEvents)
        {
            for (const auto& ev : src) {
                if (IsPastEventByDate_(ev, todayDate)) {
                    pastEvents.push_back(ev);
                }
                else {
                    nowEvents.push_back(ev);
                }
            }
        }

        void AppendWideAndFlush_(
            std::ofstream& ofs,
            std::wstring& buffer,
            std::wstring_view text)
        {
            buffer.append(text.data(), text.size());

            if (buffer.size() > (1u << 20)) {
                const std::string utf8 = EpgHtmlPortal_Common::WideToUtf8(buffer);
                if (!utf8.empty()) {
                    ofs.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
                }
                buffer.clear();
            }
        }

        void AppendSearchEventJs_(
            std::ofstream& ofs,
            std::wstring& body,
            const SearchEvent& ev)
        {
            AppendWideAndFlush_(ofs, body, L"    {");

            AppendWideAndFlush_(ofs, body, L"date:");
            AppendWideAndFlush_(ofs, body, EpgHtmlPortal_Common::ToJsStringLiteral(ev.dateText));
            AppendWideAndFlush_(ofs, body, L",");

            AppendWideAndFlush_(ofs, body, L"weekday:");
            AppendWideAndFlush_(ofs, body, EpgHtmlPortal_Common::ToJsStringLiteral(ev.weekdayText));
            AppendWideAndFlush_(ofs, body, L",");

            AppendWideAndFlush_(ofs, body, L"time:");
            AppendWideAndFlush_(ofs, body, EpgHtmlPortal_Common::ToJsStringLiteral(ev.timeText));
            AppendWideAndFlush_(ofs, body, L",");

            AppendWideAndFlush_(ofs, body, L"title:");
            AppendWideAndFlush_(ofs, body, EpgHtmlPortal_Common::ToJsStringLiteral(ev.title));
            AppendWideAndFlush_(ofs, body, L",");

            // summary は空なら出さない。
            // runtime 側は item.summary || '' で扱うため省略しても問題ない。
            if (!ev.summary.empty()) {
                AppendWideAndFlush_(ofs, body, L"summary:");
                AppendWideAndFlush_(ofs, body, EpgHtmlPortal_Common::ToJsStringLiteral(ev.summary));
                AppendWideAndFlush_(ofs, body, L",");
            }

            AppendWideAndFlush_(ofs, body, L"service:");
            AppendWideAndFlush_(ofs, body, EpgHtmlPortal_Common::ToJsStringLiteral(ev.serviceName));
            AppendWideAndFlush_(ofs, body, L",");

            AppendWideAndFlush_(ofs, body, L"genre:");
            AppendWideAndFlush_(ofs, body, EpgHtmlPortal_Common::ToJsStringLiteral(ev.genreName));
            AppendWideAndFlush_(ofs, body, L",");

            AppendWideAndFlush_(ofs, body, L"serviceId:");
            AppendWideAndFlush_(ofs, body, std::to_wstring(ev.serviceId));
            AppendWideAndFlush_(ofs, body, L",");

            AppendWideAndFlush_(ofs, body, L"eventId:");
            AppendWideAndFlush_(ofs, body, std::to_wstring(ev.eventId));

            AppendWideAndFlush_(ofs, body, L"},\n");
        }

        void AppendSearchDatasetObject_(
            std::ofstream& ofs,
            std::wstring& body,
            std::wstring_view objectName,
            const std::vector<std::vector<SearchEvent>>& eventsBySpace)
        {
            AppendWideAndFlush_(ofs, body, L"window.");
            AppendWideAndFlush_(ofs, body, objectName);
            AppendWideAndFlush_(ofs, body, L" = {\n");

            for (int sp = 0; sp <= 2; ++sp) {
                AppendWideAndFlush_(ofs, body, L"  '");
                AppendWideAndFlush_(ofs, body, std::to_wstring(sp));
                AppendWideAndFlush_(ofs, body, L"': [\n");

                const auto& events = eventsBySpace[static_cast<std::size_t>(sp)];
                for (const auto& ev : events) {
                    AppendSearchEventJs_(ofs, body, ev);
                }

                AppendWideAndFlush_(ofs, body, L"  ],\n");
            }

            AppendWideAndFlush_(ofs, body, L"};\n");
        }

        struct ArchiveInfo
        {
            std::wstring date;
            std::filesystem::path filePath;
            std::wstring relativeFile;
            std::size_t count = 0;
        };

        std::size_t CountEventsBySpace_(
            const std::vector<std::vector<SearchEvent>>& bySpace)
        {
            std::size_t n = 0;
            for (const auto& v : bySpace) {
                n += v.size();
            }
            return n;
        }

        bool WriteSearchDatasetFile_(
            const std::filesystem::path& jsPath,
            std::wstring_view objectName,
            const std::vector<std::vector<SearchEvent>>& eventsBySpace,
            bool writeAsCurrentPageData)
        {
            std::ofstream ofs(jsPath, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][ERROR] search js open failed: %ls",
                    jsPath.wstring().c_str());
                return false;
            }

            static constexpr unsigned char kUtf8Bom[3] = { 0xEF, 0xBB, 0xBF };
            ofs.write(reinterpret_cast<const char*>(kUtf8Bom), 3);

            std::wstring body;
            body.reserve(4096);

            AppendSearchDatasetObject_(ofs, body, objectName, eventsBySpace);

            if (writeAsCurrentPageData) {
                std::vector<std::vector<SearchEvent>> emptyPast(3);

                AppendWideAndFlush_(ofs, body, L"\n");
                AppendSearchDatasetObject_(ofs, body, L"EPG_SEARCH_DATA_PAST", emptyPast);

                AppendWideAndFlush_(
                    ofs,
                    body,
                    L"\nwindow.EPG_SEARCH_DATA_BY_PAGE = {\n"
                    L"  now: window.EPG_SEARCH_DATA,\n"
                    L"  past: window.EPG_SEARCH_DATA_PAST\n"
                    L"};\n");
            }

            if (!body.empty()) {
                const std::string utf8 = EpgHtmlPortal_Common::WideToUtf8(body);
                if (!utf8.empty()) {
                    ofs.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
                }
                body.clear();
            }

            ofs.flush();
            return ofs.good();
        }

        bool WriteSearchManifestJs_(
            const std::filesystem::path& manifestPath,
            const SearchDataSettings& settings,
            std::wstring_view baseDate,
            std::wstring_view endDate,
            const std::vector<ArchiveInfo>& archives)
        {
            std::ofstream ofs(manifestPath, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][ERROR] manifest open failed: %ls",
                    manifestPath.wstring().c_str());
                return false;
            }

            static constexpr unsigned char kUtf8Bom[3] = { 0xEF, 0xBB, 0xBF };
            ofs.write(reinterpret_cast<const char*>(kUtf8Bom), 3);

            std::wstring body;
            body.reserve(4096);

            AppendWideAndFlush_(ofs, body, L"window.EPG_SEARCH_MANIFEST = {\n");
            AppendWideAndFlush_(ofs, body, L"  mode: " + EpgHtmlPortal_Common::ToJsStringLiteral(settings.searchDataMode) + L",\n");
            AppendWideAndFlush_(ofs, body, L"  currentFile: " + EpgHtmlPortal_Common::ToJsStringLiteral(settings.currentFile) + L",\n");
            AppendWideAndFlush_(ofs, body, L"  archiveDir: " + EpgHtmlPortal_Common::ToJsStringLiteral(settings.archiveDir) + L",\n");
            AppendWideAndFlush_(ofs, body, L"  baseDate: " + EpgHtmlPortal_Common::ToJsStringLiteral(std::wstring(baseDate)) + L",\n");
            AppendWideAndFlush_(ofs, body, L"  endDate: " + EpgHtmlPortal_Common::ToJsStringLiteral(std::wstring(endDate)) + L",\n");
            AppendWideAndFlush_(ofs, body, L"  searchStartHour: " + std::to_wstring(settings.searchStartHour) + L",\n");
            AppendWideAndFlush_(ofs, body, L"  futureDays: " + std::to_wstring(settings.futureDays) + L",\n");
            AppendWideAndFlush_(ofs, body, L"  includePastHours: " + std::to_wstring(settings.includePastHours) + L",\n");
            AppendWideAndFlush_(ofs, body, L"  archiveDays: " + std::to_wstring(settings.archiveDays) + L",\n");
            AppendWideAndFlush_(ofs, body, L"  lazyLoadArchive: " + std::wstring(settings.lazyLoadArchive ? L"true" : L"false") + L",\n");
            AppendWideAndFlush_(ofs, body, L"  archives: [\n");

            for (const auto& a : archives) {
                AppendWideAndFlush_(ofs, body, L"    { date: ");
                AppendWideAndFlush_(ofs, body, EpgHtmlPortal_Common::ToJsStringLiteral(a.date));
                AppendWideAndFlush_(ofs, body, L", file: ");
                AppendWideAndFlush_(ofs, body, EpgHtmlPortal_Common::ToJsStringLiteral(a.relativeFile));
                AppendWideAndFlush_(ofs, body, L", count: ");
                AppendWideAndFlush_(ofs, body, std::to_wstring(a.count));
                AppendWideAndFlush_(ofs, body, L" },\n");
            }

            AppendWideAndFlush_(ofs, body, L"  ]\n};\n");

            // 過去日付指定時に使うための軽いローダー。
            AppendWideAndFlush_(
                ofs,
                body,
                L"\n"
                L"window.__epgSearchArchiveLoaded = window.__epgSearchArchiveLoaded || {};\n"
                L"\n"
                L"window.__epgLoadSearchArchiveByDate = function(dateText){\n"
                L"  const manifest = window.EPG_SEARCH_MANIFEST || {};\n"
                L"  const list = Array.isArray(manifest.archives) ? manifest.archives : [];\n"
                L"  const date = String(dateText || '').slice(0, 10);\n"
                L"  const hit = list.find(x => String(x && x.date || '') === date);\n"
                L"  if (!hit || !hit.file) {\n"
                L"    return Promise.resolve({ ok:false, reason:'archive-not-found', date:date });\n"
                L"  }\n"
                L"\n"
                L"  if (window.__epgSearchArchiveLoaded[hit.file]) {\n"
                L"    return Promise.resolve({\n"
                L"      ok:true,\n"
                L"      reason:'already-loaded',\n"
                L"      date:date,\n"
                L"      data:window.EPG_SEARCH_ARCHIVE_DATA || null\n"
                L"    });\n"
                L"  }\n"
                L"\n"
                L"  return new Promise(function(resolve){\n"
                L"    const s = document.createElement('script');\n"
                L"    s.src = String(hit.file) + '?t=' + Date.now();\n"
                L"    s.onload = function(){\n"
                L"      window.__epgSearchArchiveLoaded[hit.file] = true;\n"
                L"      resolve({\n"
                L"        ok:true,\n"
                L"        reason:'loaded',\n"
                L"        date:date,\n"
                L"        data:window.EPG_SEARCH_ARCHIVE_DATA || null\n"
                L"      });\n"
                L"    };\n"
                L"    s.onerror = function(){\n"
                L"      resolve({ ok:false, reason:'load-failed', date:date, file:hit.file });\n"
                L"    };\n"
                L"    document.head.appendChild(s);\n"
                L"  });\n"
                L"};\n");

            if (!body.empty()) {
                const std::string utf8 = EpgHtmlPortal_Common::WideToUtf8(body);
                if (!utf8.empty()) {
                    ofs.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
                }
                body.clear();
            }

            ofs.flush();
            return ofs.good();
        }

        std::vector<SearchEvent> LoadProgramPageHtmlEvents_(
            const std::filesystem::path& htmlPath,
            int space)
        {
            std::vector<SearchEvent> out;

            if (!std::filesystem::exists(htmlPath)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][HTML][WARN] program html not found: space=%d path=%ls",
                    space,
                    htmlPath.wstring().c_str());
                return out;
            }

            std::wstring html;
            if (!EpgHtmlPortal_Common::ReadTextFileToWide(htmlPath, html)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][HTML][WARN] program html read failed: space=%d path=%ls",
                    space,
                    htmlPath.wstring().c_str());
                return out;
            }

            std::unordered_set<std::wstring> seen;

            std::vector<std::size_t> markerOffsets;
            LocalMarkerScanStats_ markerStats{};

            if (!CollectProgramMarkerOffsets_(
                html,
                markerOffsets,
                &markerStats)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][HTML][WARN] marker scan failed: space=%d path=%ls",
                    space,
                    htmlPath.wstring().c_str());
                return out;
            }

            LogSearchMarkerScanStats_(
                markerStats,
                L"LoadProgramPageHtmlEvents.ProgramMarker");

            for (const std::size_t marker : markerOffsets) {
                const std::size_t tagBegin =
                    html.rfind(L'<', marker);

                const std::size_t tagEnd =
                    html.find(L'>', marker);

                if (tagBegin == std::wstring::npos ||
                    tagEnd == std::wstring::npos ||
                    tagEnd <= tagBegin) {
                    continue;
                }

                const std::wstring_view tag(
                    html.data() + tagBegin,
                    tagEnd - tagBegin + 1);

                SearchEvent ev{};
                ev.title = ExtractHtmlAttribute_(tag, L"data-title");
                ev.summary = ExtractHtmlAttribute_(tag, L"data-summary");
                ev.serviceName = ExtractHtmlAttribute_(tag, L"data-service");
                ev.dateText = ExtractHtmlAttribute_(tag, L"data-date");
                ev.weekdayText = ExtractHtmlAttribute_(tag, L"data-weekday");
                ev.timeText = ExtractHtmlAttribute_(tag, L"data-time");
                ev.genreName = ExtractHtmlAttribute_(tag, L"data-genre");
                ev.serviceId = ToUInt16Loose_(
                    ExtractHtmlAttribute_(tag, L"data-service-id"));
                ev.eventId = ToUInt16Loose_(
                    ExtractHtmlAttribute_(tag, L"data-event-id"));

                if (ev.serviceName.empty()) {
                    ev.serviceName =
                        ExtractHtmlAttribute_(tag, L"data-service-name");
                }

                if (ev.title.empty()) {
                    continue;
                }

                if (ev.serviceName.empty() && ev.serviceId != 0) {
                    ev.serviceName =
                        L"SID " + std::to_wstring(ev.serviceId);
                }

                if (ev.genreName.empty()) {
                    ev.genreName = L"--";
                }

                const std::wstring key =
                    MakeHtmlEventDedupeKey_(ev);

                if (!seen.insert(key).second) {
                    continue;
                }

                out.push_back(std::move(ev));
            }

            DebugLogFormat(
                L"[EPG-PORTAL][SEARCH][HTML] parsed program html: space=%d count=%zu path=%ls",
                space,
                out.size(),
                htmlPath.wstring().c_str());

            return out;
        }

        std::vector<SearchEvent> LoadMergedEventsLegacy_(
            const std::filesystem::path& filePath,
            const EpgHtmlPortal::WriteOptions& opt,
            int space)
        {
            std::vector<SearchEvent> out;

            if (!std::filesystem::exists(filePath)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][WARN] merged epg not found: space=%d path=%ls",
                    space,
                    filePath.wstring().c_str());
                return out;
            }

            std::wstring text;
            if (!EpgHtmlPortal_Common::ReadTextFileToWide(filePath, text)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][WARN] merged epg read failed: space=%d path=%ls",
                    space,
                    filePath.wstring().c_str());
                return out;
            }

            if (text.empty()) {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][WARN] merged epg empty: space=%d path=%ls",
                    space,
                    filePath.wstring().c_str());
                return out;
            }

            MergedParseStats_ parseStats{};

            {
                std::vector<std::size_t> tableOffsets;
                LocalMarkerScanStats_ tableStats{};

                if (CollectTableIdOffsets_(
                    text,
                    tableOffsets,
                    &tableStats)) {
                    parseStats.tableIdOffsetMatches =
                        static_cast<std::uint64_t>(tableOffsets.size());

                    LogSearchMarkerScanStats_(
                        tableStats,
                        L"LoadMergedEvents.TableID");
                }
                else {
                    DebugLogFormat(
                        L"[EPG-PORTAL][SEARCH][SCAN][WARN] TableID scan failed: space=%d path=%ls",
                        space,
                        filePath.wstring().c_str());
                }
            }

            const auto serviceMap =
                BuildServiceMap(opt, space);

            SearchEvent cur{};
            bool hasEvent = false;

            auto flushEvent = [&]() {
                ++parseStats.flushCalls;

                if (!hasEvent) {
                    ++parseStats.flushIgnoredNoEvent;
                    return;
                }

                RebuildSearchEventTimeText_(cur);

                if (cur.title.empty() && !cur.summary.empty()) {
                    cur.title = cur.summary;
                    ++parseStats.titleFromSummary;
                }

                if (cur.title.empty()) {
                    ++parseStats.skipNoTitle;

                    cur = SearchEvent{};
                    hasEvent = false;
                    return;
                }

                if (cur.serviceName.empty()) {
                    const auto it =
                        serviceMap.find(cur.serviceId);

                    if (it != serviceMap.end()) {
                        cur.serviceName = it->second;
                        ++parseStats.serviceMapHit;
                    }
                    else {
                        cur.serviceName =
                            L"SID " + std::to_wstring(cur.serviceId);

                        ++parseStats.serviceMapMiss;
                        ++parseStats.serviceSidFallback;
                    }
                }

                if (cur.genreName.empty()) {
                    cur.genreName = L"--";
                    ++parseStats.genreFallback;
                }

                out.push_back(cur);
                ++parseStats.accepted;

                cur = SearchEvent{};
                hasEvent = false;
                };

            std::wistringstream iss(text);
            std::wstring line;

            while (std::getline(iss, line)) {
                ++parseStats.lines;

                if (!line.empty() && line.back() == L'\r') {
                    line.pop_back();
                }

                const std::wstring trimmed =
                    EpgHtmlPortal_Common::TrimW(line);

                if (trimmed.empty()) {
                    ++parseStats.blankLines;
                    flushEvent();
                    continue;
                }

                if (trimmed.rfind(L"TableID", 0) == 0) {
                    flushEvent();

                    ++parseStats.tableIdLines;

                    hasEvent = true;
                    cur = SearchEvent{};

                    cur.serviceId = ToUInt16Loose_(
                        ExtractLooseNumberTokenAfterKey_(
                            trimmed,
                            L"ServiceID"));

                    cur.eventId = ToUInt16Loose_(
                        ExtractLooseNumberTokenAfterKey_(
                            trimmed,
                            L"EventID"));

                    continue;
                }

                if (!hasEvent) {
                    continue;
                }

                if (trimmed.rfind(L"StartTime(raw MJD+BCD40bit):", 0) == 0) {
                    int y = 0;
                    int mo = 0;
                    int d = 0;
                    int hh = 0;
                    int mm = 0;
                    int ss = 0;

                    if (ParseStartHex(trimmed, y, mo, d, hh, mm, ss)) {
                        cur.dateText =
                            FormatDate(y, mo, d);

                        cur.weekdayText =
                            GetWeekdayText(y, mo, d);

                        cur.hasStart = true;
                        cur.startHour = hh;
                        cur.startMinute = mm;
                        cur.startSecond = ss;

                        RebuildSearchEventTimeText_(cur);
                        ++parseStats.startParsed;
                    }
                    else {
                        ++parseStats.startParseFailed;
                    }

                    continue;
                }

                if (trimmed.rfind(L"Duration(HH:MM:SS):", 0) == 0) {
                    const std::wstring duration =
                        ExtractValueAfterPrefix(
                            trimmed,
                            L"Duration(HH:MM:SS):");

                    int dh = 0;
                    int dm = 0;
                    int ds = 0;

                    if (swscanf_s(
                        duration.c_str(),
                        L"%d:%d:%d",
                        &dh,
                        &dm,
                        &ds) == 3) {
                        cur.hasDuration = true;
                        cur.durationHour = dh;
                        cur.durationMinute = dm;
                        cur.durationSecond = ds;

                        RebuildSearchEventTimeText_(cur);
                        ++parseStats.durationParsed;
                    }
                    else {
                        ++parseStats.durationParseFailed;
                    }

                    continue;
                }

                if (trimmed.rfind(L"[短形式タイトル]", 0) == 0) {
                    cur.title =
                        ExtractValueAfterPrefix(
                            trimmed,
                            L"[短形式タイトル]");
                    continue;
                }

                if (trimmed.rfind(L"[短形式概要]", 0) == 0) {
                    if (SearchShouldKeepSummaryForOutput_() ||
                        (SearchShouldUseSummaryAsTitleFallback_() && cur.title.empty())) {
                        cur.summary =
                            ExtractValueAfterPrefix(
                                trimmed,
                                L"[短形式概要]");
                    }

                    continue;
                }

                if (trimmed.rfind(L"[ジャンル]", 0) == 0) {
                    const std::size_t pos =
                        trimmed.find(L"primary=0x");

                    if (pos != std::wstring::npos &&
                        pos + 10 < trimmed.size()) {
                        const wchar_t c =
                            trimmed[pos + 10];

                        int primary = 0;

                        if (c >= L'0' && c <= L'9') {
                            primary = c - L'0';
                        }
                        else if (c >= L'A' && c <= L'F') {
                            primary = 10 + (c - L'A');
                        }
                        else if (c >= L'a' && c <= L'f') {
                            primary = 10 + (c - L'a');
                        }

                        cur.genreName =
                            GenreNameFromPrimary(primary);
                    }

                    continue;
                }

                if (trimmed.rfind(L"(", 0) == 0) {
                    if (!SearchShouldKeepSummaryForOutput_() &&
                        !cur.title.empty()) {
                        continue;
                    }

                    const std::wstring ext =
                        ExtractValueAfterColon(trimmed);

                    if (!ext.empty()) {
                        if (cur.summary.empty() ||
                            cur.summary == L"　") {
                            cur.summary = ext;
                            ++parseStats.extendedSummarySeed;
                        }
                        else if (SearchShouldKeepSummaryForOutput_() &&
                            cur.summary.find(ext) == std::wstring::npos) {
                            cur.summary += L" / ";
                            cur.summary += ext;
                            ++parseStats.extendedSummaryAppend;
                        }
                    }

                    continue;
                }
            }

            flushEvent();

            DebugLogFormat(
                L"[EPG-PORTAL][SEARCH] parsed merged events: space=%d count=%zu path=%ls",
                space,
                out.size(),
                filePath.wstring().c_str());

            LogMergedParseStats_(
                space,
                filePath,
                parseStats);

            return out;
        }

        struct SearchMergedBlockRange_
        {
            std::size_t begin = 0;
            std::size_t end = 0;
        };

        std::size_t SearchLineBeginAt_(
            const std::wstring& text,
            std::size_t pos) noexcept
        {
            if (pos > text.size()) {
                pos = text.size();
            }

            while (pos > 0u) {
                const wchar_t prev = text[pos - 1u];

                if (prev == L'\n' || prev == L'\r') {
                    break;
                }

                --pos;
            }

            return pos;
        }

        bool SearchLineStartsWithTableIdAt_(
            const std::wstring& text,
            std::size_t markerPos,
            std::size_t& outBlockBegin) noexcept
        {
            outBlockBegin = 0;

            if (markerPos >= text.size()) {
                return false;
            }

            const std::size_t lineBegin =
                SearchLineBeginAt_(text, markerPos);

            std::size_t p = lineBegin;

            while (p < text.size() &&
                (text[p] == L' ' ||
                    text[p] == L'\t' ||
                    text[p] == L'\ufeff')) {
                ++p;
            }

            if (p != markerPos) {
                return false;
            }

            constexpr std::wstring_view kPrefix = L"TableID";

            if (p + kPrefix.size() > text.size()) {
                return false;
            }

            if (std::wstring_view(text.data() + p, kPrefix.size()) != kPrefix) {
                return false;
            }

            outBlockBegin = p;
            return true;
        }

        bool SearchIsBlankLineRange_(
            const std::wstring& text,
            std::size_t begin,
            std::size_t end) noexcept
        {
            if (begin > text.size()) {
                begin = text.size();
            }

            if (end > text.size()) {
                end = text.size();
            }

            if (begin > end) {
                return false;
            }

            for (std::size_t i = begin; i < end; ++i) {
                const wchar_t ch = text[i];

                if (ch != L' ' &&
                    ch != L'\t' &&
                    ch != L'\r' &&
                    ch != L'\n') {
                    return false;
                }
            }

            return true;
        }

        std::size_t SearchNextLineBegin_(
            const std::wstring& text,
            std::size_t lineEnd,
            std::size_t limit) noexcept
        {
            if (lineEnd >= limit) {
                return limit;
            }

            if (text[lineEnd] == L'\r' &&
                lineEnd + 1u < limit &&
                text[lineEnd + 1u] == L'\n') {
                return lineEnd + 2u;
            }

            return lineEnd + 1u;
        }

        std::size_t SearchFindBlockEndBeforeNext_(
            const std::wstring& text,
            std::size_t begin,
            std::size_t limit) noexcept
        {
            if (begin > text.size()) {
                begin = text.size();
            }

            if (limit > text.size()) {
                limit = text.size();
            }

            if (begin >= limit) {
                return begin;
            }

            std::size_t lineBegin = begin;
            bool firstLine = true;

            while (lineBegin < limit) {
                std::size_t lineEnd = lineBegin;

                while (lineEnd < limit &&
                    text[lineEnd] != L'\r' &&
                    text[lineEnd] != L'\n') {
                    ++lineEnd;
                }

                if (!firstLine &&
                    SearchIsBlankLineRange_(
                        text,
                        lineBegin,
                        lineEnd)) {
                    return lineBegin;
                }

                firstLine = false;

                if (lineEnd >= limit) {
                    break;
                }

                lineBegin =
                    SearchNextLineBegin_(
                        text,
                        lineEnd,
                        limit);
            }

            return limit;
        }

        std::wstring_view SearchTrimLineView_(
            std::wstring_view s) noexcept
        {
            while (!s.empty()) {
                const wchar_t ch = s.front();

                if (ch != L' ' &&
                    ch != L'\t' &&
                    ch != L'\r' &&
                    ch != L'\n' &&
                    ch != L'\ufeff' &&
                    ch != L'\u3000') {
                    break;
                }

                s.remove_prefix(1);
            }

            while (!s.empty()) {
                const wchar_t ch = s.back();

                if (ch != L' ' &&
                    ch != L'\t' &&
                    ch != L'\r' &&
                    ch != L'\n' &&
                    ch != L'\u3000') {
                    break;
                }

                s.remove_suffix(1);
            }

            return s;
        }

        bool SearchStartsWith_(
            std::wstring_view s,
            std::wstring_view prefix) noexcept
        {
            return s.size() >= prefix.size() &&
                s.substr(0, prefix.size()) == prefix;
        }

        bool SearchParseDurationView_(
            std::wstring_view line,
            int& outHour,
            int& outMinute,
            int& outSecond) noexcept
        {
            outHour = 0;
            outMinute = 0;
            outSecond = 0;

            // EPG dump の形式:
            //   Duration(HH:MM:SS): 00:30:00
            //
            // line.find(L':') を使うと、"HH:MM:SS" 内の ':' を拾ってしまう。
            // そのため、固定 prefix の後ろだけを duration 値として読む。
            constexpr std::wstring_view kPrefix =
                L"Duration(HH:MM:SS):";

            std::wstring_view s{};

            if (SearchStartsWith_(line, kPrefix)) {
                s = SearchTrimLineView_(
                    line.substr(kPrefix.size()));
            }
            else {
                // 保険。
                // 想定外形式の場合だけ最後の ':' 以降を読む。
                const std::size_t pos =
                    line.rfind(L':');

                if (pos == std::wstring_view::npos) {
                    return false;
                }

                s = SearchTrimLineView_(
                    line.substr(pos + 1u));
            }

            auto readNumber =
                [&s](int& outValue) noexcept -> bool
                {
                    outValue = 0;

                    if (s.empty() ||
                        s.front() < L'0' ||
                        s.front() > L'9') {
                        return false;
                    }

                    int value = 0;

                    while (!s.empty() &&
                        s.front() >= L'0' &&
                        s.front() <= L'9') {
                        const int digit =
                            static_cast<int>(s.front() - L'0');

                        if (value > ((0x7fffffff - digit) / 10)) {
                            return false;
                        }

                        value = value * 10 + digit;
                        s.remove_prefix(1);
                    }

                    outValue = value;
                    return true;
                };

            auto consumeColon =
                [&s]() noexcept -> bool
                {
                    if (s.empty() || s.front() != L':') {
                        return false;
                    }

                    s.remove_prefix(1);
                    return true;
                };

            int h = 0;
            int m = 0;
            int sec = 0;

            if (!readNumber(h) ||
                !consumeColon() ||
                !readNumber(m) ||
                !consumeColon() ||
                !readNumber(sec)) {
                return false;
            }

            if (h < 0 || m < 0 || sec < 0 || m > 59 || sec > 59) {
                return false;
            }

            outHour = h;
            outMinute = m;
            outSecond = sec;
            return true;
        }

        std::wstring SearchExtractValueAfterPrefixView_(
            std::wstring_view line,
            std::wstring_view prefix)
        {
            if (!SearchStartsWith_(line, prefix)) {
                return {};
            }

            return EpgHtmlPortal_Common::TrimW(
                std::wstring(line.substr(prefix.size())));
        }

        std::wstring SearchExtractValueAfterColonView_(
            std::wstring_view line)
        {
            const std::size_t pos = line.find(L':');

            if (pos == std::wstring_view::npos) {
                return {};
            }

            return EpgHtmlPortal_Common::TrimW(
                std::wstring(line.substr(pos + 1u)));
        }

        bool SearchCollectMergedEventRanges_(
            const std::wstring& text,
            std::vector<SearchMergedBlockRange_>& outRanges,
            MergedParseStats_& parseStats,
            int space,
            const std::filesystem::path& filePath)
        {
            outRanges.clear();

            if (text.empty()) {
                return true;
            }

            std::vector<std::size_t> tableOffsets;
            LocalMarkerScanStats_ tableStats{};

            if (!CollectTableIdOffsets_(
                text,
                tableOffsets,
                &tableStats)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][RANGE][WARN] TableID scan failed: space=%d path=%ls",
                    space,
                    filePath.wstring().c_str());

                return false;
            }

            parseStats.tableIdOffsetMatches =
                static_cast<std::uint64_t>(tableOffsets.size());

            LogSearchMarkerScanStats_(
                tableStats,
                L"LoadMergedEvents.TableID.Range");

            if (tableOffsets.empty()) {
                return false;
            }

            std::vector<std::size_t> starts;
            starts.reserve(tableOffsets.size());

            for (const std::size_t markerPos : tableOffsets) {
                std::size_t blockBegin = 0;

                if (!SearchLineStartsWithTableIdAt_(
                    text,
                    markerPos,
                    blockBegin)) {
                    continue;
                }

                starts.push_back(blockBegin);
            }

            if (starts.empty()) {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][RANGE][WARN] no line-head TableID: space=%d chars=%zu rawMarkers=%zu path=%ls",
                    space,
                    text.size(),
                    tableOffsets.size(),
                    filePath.wstring().c_str());

                return false;
            }

            std::sort(starts.begin(), starts.end());

            starts.erase(
                std::unique(starts.begin(), starts.end()),
                starts.end());

            outRanges.reserve(starts.size());

            for (std::size_t i = 0; i < starts.size(); ++i) {
                const std::size_t begin = starts[i];

                const std::size_t nextBegin =
                    (i + 1u < starts.size())
                    ? starts[i + 1u]
                    : text.size();

                const std::size_t end =
                    SearchFindBlockEndBeforeNext_(
                        text,
                        begin,
                        nextBegin);

                if (end > begin) {
                    outRanges.push_back(
                        SearchMergedBlockRange_{
                            begin,
                            end
                        });
                }
            }

            DebugLogFormat(
                L"[EPG-PORTAL][SEARCH][RANGE] collected: space=%d chars=%zu rawMarkers=%zu lineHead=%zu ranges=%zu path=%ls",
                space,
                text.size(),
                tableOffsets.size(),
                starts.size(),
                outRanges.size(),
                filePath.wstring().c_str());

            return !outRanges.empty();
        }

        bool SearchFinalizeMergedEvent_(
            SearchEvent& cur,
            bool hasEvent,
            const std::map<std::uint16_t, std::wstring>& serviceMap,
            MergedParseStats_& parseStats,
            std::vector<SearchEvent>& out)
        {
            ++parseStats.flushCalls;

            if (!hasEvent) {
                ++parseStats.flushIgnoredNoEvent;
                return false;
            }

            RebuildSearchEventTimeText_(cur);

            if (cur.title.empty() && !cur.summary.empty()) {
                cur.title = cur.summary;
                ++parseStats.titleFromSummary;
            }

            if (cur.title.empty()) {
                ++parseStats.skipNoTitle;
                return false;
            }

            if (cur.serviceName.empty()) {
                const auto it =
                    serviceMap.find(cur.serviceId);

                if (it != serviceMap.end()) {
                    cur.serviceName = it->second;
                    ++parseStats.serviceMapHit;
                }
                else {
                    cur.serviceName =
                        L"SID " + std::to_wstring(cur.serviceId);

                    ++parseStats.serviceMapMiss;
                    ++parseStats.serviceSidFallback;
                }
            }

            if (cur.genreName.empty()) {
                cur.genreName = L"--";
                ++parseStats.genreFallback;
            }

            out.push_back(std::move(cur));
            ++parseStats.accepted;
            return true;
        }

        void SearchParseMergedEventBlockView_(
            std::wstring_view block,
            const std::map<std::uint16_t, std::wstring>& serviceMap,
            MergedParseStats_& parseStats,
            std::vector<SearchEvent>& out)
        {
            SearchEvent cur{};
            bool hasEvent = false;

            std::size_t lineBegin = 0;

            while (lineBegin < block.size()) {
                std::size_t lineEnd = lineBegin;

                while (lineEnd < block.size() &&
                    block[lineEnd] != L'\r' &&
                    block[lineEnd] != L'\n') {
                    ++lineEnd;
                }

                std::wstring_view line =
                    SearchTrimLineView_(
                        block.substr(
                            lineBegin,
                            lineEnd - lineBegin));

                ++parseStats.lines;

                if (line.empty()) {
                    ++parseStats.blankLines;
                    break;
                }

                if (SearchStartsWith_(line, L"TableID")) {
                    if (hasEvent) {
                        SearchFinalizeMergedEvent_(
                            cur,
                            hasEvent,
                            serviceMap,
                            parseStats,
                            out);

                        cur = SearchEvent{};
                    }

                    ++parseStats.tableIdLines;

                    hasEvent = true;
                    cur = SearchEvent{};

                    cur.serviceId = ToUInt16Loose_(
                        ExtractLooseNumberTokenAfterKey_(
                            line,
                            L"ServiceID"));

                    cur.eventId = ToUInt16Loose_(
                        ExtractLooseNumberTokenAfterKey_(
                            line,
                            L"EventID"));

                    goto next_line;
                }

                if (!hasEvent) {
                    goto next_line;
                }

                if (SearchStartsWith_(line, L"StartTime(raw MJD+BCD40bit):")) {
                    int y = 0;
                    int mo = 0;
                    int d = 0;
                    int hh = 0;
                    int mm = 0;
                    int ss = 0;

                    if (ParseStartHex(line, y, mo, d, hh, mm, ss)) {
                        cur.dateText =
                            FormatDate(y, mo, d);

                        cur.weekdayText =
                            GetWeekdayText(y, mo, d);

                        cur.hasStart = true;
                        cur.startHour = hh;
                        cur.startMinute = mm;
                        cur.startSecond = ss;

                        RebuildSearchEventTimeText_(cur);
                        ++parseStats.startParsed;
                    }
                    else {
                        ++parseStats.startParseFailed;
                    }

                    goto next_line;
                }

                if (SearchStartsWith_(line, L"Duration(HH:MM:SS):")) {
                    int dh = 0;
                    int dm = 0;
                    int ds = 0;

                    if (SearchParseDurationView_(
                        line,
                        dh,
                        dm,
                        ds)) {
                        cur.hasDuration = true;
                        cur.durationHour = dh;
                        cur.durationMinute = dm;
                        cur.durationSecond = ds;

                        RebuildSearchEventTimeText_(cur);
                        ++parseStats.durationParsed;
                    }
                    else {
                        ++parseStats.durationParseFailed;
                    }

                    goto next_line;
                }

                if (SearchStartsWith_(line, L"[短形式タイトル]")) {
                    cur.title =
                        SearchExtractValueAfterPrefixView_(
                            line,
                            L"[短形式タイトル]");
                    goto next_line;
                }

                if (SearchStartsWith_(line, L"[短形式概要]")) {
                    // light mode では summary をJSへ出さない。
                    // ただし title 欠損時の titleFromSummary 保険だけは残す。
                    if (SearchShouldKeepSummaryForOutput_() ||
                        (SearchShouldUseSummaryAsTitleFallback_() && cur.title.empty())) {
                        cur.summary =
                            SearchExtractValueAfterPrefixView_(
                                line,
                                L"[短形式概要]");
                    }

                    goto next_line;
                }

                if (SearchStartsWith_(line, L"[ジャンル]")) {
                    const std::size_t pos =
                        line.find(L"primary=0x");

                    if (pos != std::wstring_view::npos &&
                        pos + 10u < line.size()) {
                        const wchar_t c = line[pos + 10u];

                        int primary = 0;

                        if (c >= L'0' && c <= L'9') {
                            primary = c - L'0';
                        }
                        else if (c >= L'A' && c <= L'F') {
                            primary = 10 + (c - L'A');
                        }
                        else if (c >= L'a' && c <= L'f') {
                            primary = 10 + (c - L'a');
                        }

                        cur.genreName =
                            GenreNameFromPrimary(primary);
                    }

                    goto next_line;
                }

                if (SearchStartsWith_(line, L"(")) {
                    // light mode かつ title が既にあるなら、
                    // 検索DBへ出さない拡張説明はここで捨てる。
                    //
                    // 前回ログでは extAppend が非常に多く、
                    // 最終的に summary を空にする設定では無駄が大きい。
                    if (!SearchShouldKeepSummaryForOutput_() &&
                        !cur.title.empty()) {
                        goto next_line;
                    }

                    const std::wstring ext =
                        SearchExtractValueAfterColonView_(line);

                    if (!ext.empty()) {
                        if (cur.summary.empty() ||
                            cur.summary == L"　") {
                            cur.summary = ext;
                            ++parseStats.extendedSummarySeed;
                        }
                        else if (SearchShouldKeepSummaryForOutput_() &&
                            cur.summary.find(ext) == std::wstring::npos) {
                            cur.summary += L" / ";
                            cur.summary += ext;
                            ++parseStats.extendedSummaryAppend;
                        }
                    }

                    goto next_line;
                }

            next_line:
                if (lineEnd >= block.size()) {
                    break;
                }

                if (block[lineEnd] == L'\r' &&
                    lineEnd + 1u < block.size() &&
                    block[lineEnd + 1u] == L'\n') {
                    lineBegin = lineEnd + 2u;
                }
                else {
                    lineBegin = lineEnd + 1u;
                }
            }

            SearchFinalizeMergedEvent_(
                cur,
                hasEvent,
                serviceMap,
                parseStats,
                out);
        }

        std::vector<SearchEvent> LoadMergedEvents(
            const std::filesystem::path& filePath,
            const EpgHtmlPortal::WriteOptions& opt,
            int space)
        {
            std::vector<SearchEvent> out;

            if (!std::filesystem::exists(filePath)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][WARN] merged epg not found: space=%d path=%ls",
                    space,
                    filePath.wstring().c_str());
                return out;
            }

            std::wstring text;
            if (!EpgHtmlPortal_Common::ReadTextFileToWide(filePath, text)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][WARN] merged epg read failed: space=%d path=%ls",
                    space,
                    filePath.wstring().c_str());
                return out;
            }

            if (text.empty()) {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][WARN] merged epg empty: space=%d path=%ls",
                    space,
                    filePath.wstring().c_str());
                return out;
            }

            MergedParseStats_ parseStats{};

            std::vector<SearchMergedBlockRange_> ranges;

            if (!SearchCollectMergedEventRanges_(
                text,
                ranges,
                parseStats,
                space,
                filePath)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][RANGE][WARN] range parser unavailable. fallback legacy: space=%d path=%ls",
                    space,
                    filePath.wstring().c_str());

                return LoadMergedEventsLegacy_(filePath, opt, space);
            }

            const auto serviceMap =
                BuildServiceMap(opt, space);

            out.reserve(ranges.size());

            for (const auto& range : ranges) {
                if (range.begin >= text.size() ||
                    range.end > text.size() ||
                    range.end <= range.begin) {
                    continue;
                }

                const std::wstring_view block(
                    text.data() + range.begin,
                    range.end - range.begin);

                SearchParseMergedEventBlockView_(
                    block,
                    serviceMap,
                    parseStats,
                    out);
            }

            DebugLogFormat(
                L"[EPG-PORTAL][SEARCH] parsed merged events range: space=%d count=%zu ranges=%zu path=%ls",
                space,
                out.size(),
                ranges.size(),
                filePath.wstring().c_str());

            LogMergedParseStats_(
                space,
                filePath,
                parseStats);

            return out;
        }
    }

    std::wstring MakeStandardMergedEpgFileName(int space)
    {
        return L"EpgMerged_space" + std::to_wstring(space) + L"_All_EIT.epg";
    }

    std::filesystem::path ResolveMergedEpgPathForSpace(
        const EpgHtmlPortal::WriteOptions& opt,
        int space)
    {
        auto existsSafe = [](const std::filesystem::path& p) -> bool {
            if (p.empty()) {
                return false;
            }

            std::error_code ec;
            const bool ok = std::filesystem::exists(p, ec);
            return ok && !ec;
            };

        auto lowerAscii = [](std::wstring s) {
            for (wchar_t& ch : s) {
                if (ch >= L'A' && ch <= L'Z') {
                    ch = static_cast<wchar_t>(ch - L'A' + L'a');
                }
            }
            return s;
            };

        auto firstPathPart = [](const std::filesystem::path& p) -> std::wstring {
            auto it = p.begin();
            if (it == p.end()) {
                return {};
            }
            return it->wstring();
            };

        auto resolveCandidate = [&](const std::filesystem::path& p) -> std::filesystem::path {
            if (p.empty()) {
                return {};
            }

            if (p.is_absolute()) {
                return p;
            }

            // すでに epg_out\EpgMerged... のような相対パスなら、
            // まずそのまま確認する。ここで見つかれば二重結合しない。
            if (existsSafe(p)) {
                return p;
            }

            // p の先頭ディレクトリ名が outputDir の末尾名と同じなら、
            // opt.outputDir / p にすると epg_out\epg_out になる。
            // 存在確認に失敗しても、二重結合より p を優先する。
            const std::wstring outName = lowerAscii(opt.outputDir.filename().wstring());
            const std::wstring firstName = lowerAscii(firstPathPart(p));

            if (!outName.empty() && !firstName.empty() && outName == firstName) {
                return p;
            }

            if (!opt.outputDir.empty()) {
                const std::filesystem::path joined = opt.outputDir / p;
                if (existsSafe(joined)) {
                    return joined;
                }

                return joined;
            }

            return p;
            };

        if (space >= 0 &&
            static_cast<std::size_t>(space) < opt.mergedEpgPathsBySpace.size()) {
            const auto& p = opt.mergedEpgPathsBySpace[static_cast<std::size_t>(space)];
            if (!p.empty()) {
                return resolveCandidate(p);
            }
        }

        return resolveCandidate(MakeStandardMergedEpgFileName(space));
    }

    bool WriteSearchDataJs(const EpgHtmlPortal::WriteOptions& opt)
    {
        const SearchDataSettings settings = LoadSearchDataSettings_();

        const std::filesystem::path currentJsPath =
            opt.outputDir / settings.currentFile;

        const std::filesystem::path manifestPath =
            opt.outputDir / settings.manifestFile;

        const std::filesystem::path archiveDirPath =
            opt.outputDir / settings.archiveDir;

        const bool buildArchiveFiles =
            ShouldBuildArchiveFiles_(settings);

        if (buildArchiveFiles) {
            std::error_code ec;
            std::filesystem::create_directories(archiveDirPath, ec);
            if (ec) {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][WARN] archive dir create failed: %ls err=%ls",
                    archiveDirPath.wstring().c_str(),
                    std::wstring(ec.message().begin(), ec.message().end()).c_str());
            }
        }

        struct SearchDbInputFile
        {
            std::filesystem::path path;
            int physicalChannel = -1;
            bool merged = false;
        };

        auto parseEpgFullPhysicalChannel =
            [](const std::filesystem::path& p, int space, int& outCh) -> bool
            {
                outCh = -1;

                const std::wstring name = p.filename().wstring();
                const std::wstring prefix =
                    L"EpgFull_space" + std::to_wstring(space) + L"_ch";
                const std::wstring suffix = L"_EIT.epg";

                if (name.size() <= prefix.size() + suffix.size()) {
                    return false;
                }

                if (name.rfind(prefix, 0) != 0) {
                    return false;
                }

                if (name.compare(
                    name.size() - suffix.size(),
                    suffix.size(),
                    suffix) != 0) {
                    return false;
                }

                const std::size_t begin = prefix.size();
                const std::size_t count =
                    name.size() - prefix.size() - suffix.size();

                if (count == 0) {
                    return false;
                }

                const std::wstring chText = name.substr(begin, count);

                for (const wchar_t ch : chText) {
                    if (ch < L'0' || ch > L'9') {
                        return false;
                    }
                }

                try {
                    outCh = std::stoi(chText);
                    return outCh >= 0;
                }
                catch (...) {
                    outCh = -1;
                    return false;
                }
            };

        auto findEpgInputFilesForSpace =
            [&](int sp) -> std::vector<SearchDbInputFile>
            {
                std::vector<SearchDbInputFile> files;

                std::error_code ec;

                if (!opt.outputDir.empty() &&
                    std::filesystem::exists(opt.outputDir, ec) &&
                    std::filesystem::is_directory(opt.outputDir, ec)) {
                    for (const auto& entry : std::filesystem::directory_iterator(opt.outputDir, ec)) {
                        if (ec) {
                            break;
                        }

                        if (!entry.is_regular_file(ec)) {
                            continue;
                        }

                        int physicalCh = -1;
                        if (!parseEpgFullPhysicalChannel(entry.path(), sp, physicalCh)) {
                            continue;
                        }

                        SearchDbInputFile in{};
                        in.path = entry.path();
                        in.physicalChannel = physicalCh;
                        in.merged = false;
                        files.push_back(std::move(in));
                    }
                }

                std::sort(
                    files.begin(),
                    files.end(),
                    [](const SearchDbInputFile& a, const SearchDbInputFile& b) {
                        if (a.physicalChannel != b.physicalChannel) {
                            return a.physicalChannel < b.physicalChannel;
                        }
                        return a.path.wstring() < b.path.wstring();
                    });

                // チャンネル別 EpgFull が存在する場合はそれを正本にする。
                if (!files.empty()) {
                    return files;
                }

                // EpgFull が無い場合だけ merged を fallback として使う。
                const std::filesystem::path mergedPath =
                    ResolveMergedEpgPathForSpace(opt, sp);

                std::error_code ec2;
                if (!mergedPath.empty() &&
                    std::filesystem::exists(mergedPath, ec2) &&
                    !ec2) {
                    SearchDbInputFile in{};
                    in.path = mergedPath;
                    in.physicalChannel = -1;
                    in.merged = true;
                    files.push_back(std::move(in));
                }

                return files;
            };

        auto makeExactEventKey =
            [](const SearchEvent& ev) -> std::wstring
            {
                return L"sid:" + std::to_wstring(ev.serviceId) +
                    L"|eid:" + std::to_wstring(ev.eventId) +
                    L"|date:" + ev.dateText +
                    L"|time:" + ev.timeText +
                    L"|title:" + ev.title;
            };

        auto makeContentEventKey =
            [](const SearchEvent& ev) -> std::wstring
            {
                return L"eid:" + std::to_wstring(ev.eventId) +
                    L"|date:" + ev.dateText +
                    L"|time:" + ev.timeText +
                    L"|title:" + ev.title;
            };

        auto isSidFallbackName =
            [](std::wstring_view s) -> bool
            {
                return s.rfind(L"SID ", 0) == 0;
            };

        auto fillServiceNameFromMap =
            [](std::vector<SearchEvent>& events,
                const std::map<std::uint16_t, std::wstring>& serviceMap)
            {
                for (auto& ev : events) {
                    if (ev.serviceId == 0) {
                        continue;
                    }

                    const auto it = serviceMap.find(ev.serviceId);
                    if (it == serviceMap.end()) {
                        if (ev.serviceName.empty()) {
                            ev.serviceName = L"SID " + std::to_wstring(ev.serviceId);
                        }
                        continue;
                    }

                    if (ev.serviceName.empty() ||
                        ev.serviceName.rfind(L"SID ", 0) == 0) {
                        ev.serviceName = it->second;
                    }
                }
            };

        auto filterToKnownServices =
            [](std::vector<SearchEvent>& events,
                const std::map<std::uint16_t, std::wstring>& serviceMap)
            {
                if (serviceMap.empty()) {
                    return;
                }

                events.erase(
                    std::remove_if(
                        events.begin(),
                        events.end(),
                        [&](const SearchEvent& ev) {
                            if (ev.serviceId == 0) {
                                return false;
                            }

                            return serviceMap.find(ev.serviceId) == serviceMap.end();
                        }),
                    events.end());
            };

        auto dedupeAndPreferNamedService =
            [&](std::vector<SearchEvent>& events)
            {
                std::vector<SearchEvent> out;
                out.reserve(events.size());

                std::unordered_set<std::wstring> exactSeen;
                exactSeen.reserve(events.size());

                std::unordered_set<std::wstring> namedContentSeen;
                namedContentSeen.reserve(events.size());

                std::map<std::wstring, std::size_t> fallbackContentIndex;

                for (auto& ev : events) {
                    if (ev.title.empty()) {
                        continue;
                    }

                    const std::wstring exactKey = makeExactEventKey(ev);
                    if (!exactSeen.insert(exactKey).second) {
                        continue;
                    }

                    const std::wstring contentKey = makeContentEventKey(ev);
                    const bool fallbackName =
                        ev.serviceName.empty() || isSidFallbackName(ev.serviceName);

                    if (fallbackName) {
                        if (namedContentSeen.find(contentKey) != namedContentSeen.end()) {
                            continue;
                        }

                        if (fallbackContentIndex.find(contentKey) != fallbackContentIndex.end()) {
                            continue;
                        }

                        fallbackContentIndex.emplace(contentKey, out.size());
                        out.push_back(std::move(ev));
                        continue;
                    }

                    const std::wstring namedKey =
                        contentKey + L"|svc:" + ev.serviceName;

                    if (!namedContentSeen.insert(namedKey).second) {
                        continue;
                    }

                    const auto fallbackIt = fallbackContentIndex.find(contentKey);
                    if (fallbackIt != fallbackContentIndex.end() &&
                        fallbackIt->second < out.size()) {
                        out[fallbackIt->second] = std::move(ev);
                        fallbackContentIndex.erase(fallbackIt);
                        continue;
                    }

                    out.push_back(std::move(ev));
                }

                events = std::move(out);
            };

        auto loadEventsFromEpgFiles =
            [&](int sp,
                const std::vector<SearchDbInputFile>& files)
            -> std::vector<SearchEvent>
            {
                std::vector<SearchEvent> allEvents;

                for (const auto& f : files) {
                    std::vector<SearchEvent> events;

                    try {
                        events = LoadMergedEvents(f.path, opt, sp);
                    }
                    catch (const std::exception& ex) {
                        DebugLogFormat(
                            L"[EPG-PORTAL][SEARCH][ERROR] epg parser exception: space=%d ch=%d merged=%d path=%ls what=%S",
                            sp,
                            f.physicalChannel,
                            f.merged ? 1 : 0,
                            f.path.wstring().c_str(),
                            ex.what());
                        events.clear();
                    }
                    catch (...) {
                        DebugLogFormat(
                            L"[EPG-PORTAL][SEARCH][ERROR] epg parser unknown exception: space=%d ch=%d merged=%d path=%ls",
                            sp,
                            f.physicalChannel,
                            f.merged ? 1 : 0,
                            f.path.wstring().c_str());
                        events.clear();
                    }

                    DebugLogFormat(
                        L"[EPG-PORTAL][SEARCH] loaded epg source: space=%d ch=%d merged=%d count=%zu path=%ls",
                        sp,
                        f.physicalChannel,
                        f.merged ? 1 : 0,
                        events.size(),
                        f.path.wstring().c_str());

                    allEvents.insert(
                        allEvents.end(),
                        std::make_move_iterator(events.begin()),
                        std::make_move_iterator(events.end()));
                }

                return allEvents;
            };

        auto loadHtmlFallbackSafe =
            [&](int sp) -> std::vector<SearchEvent>
            {
                std::vector<SearchEvent> htmlEvents;

                const std::filesystem::path currentHtmlPath =
                    MakeStandardProgramHtmlPath_(opt, sp);

                try {
                    htmlEvents = LoadProgramPageHtmlEvents_(currentHtmlPath, sp);
                }
                catch (const std::exception& ex) {
                    DebugLogFormat(
                        L"[EPG-PORTAL][SEARCH][HTML][ERROR] parser exception: space=%d path=%ls what=%S",
                        sp,
                        currentHtmlPath.wstring().c_str(),
                        ex.what());
                    htmlEvents.clear();
                }
                catch (...) {
                    DebugLogFormat(
                        L"[EPG-PORTAL][SEARCH][HTML][ERROR] parser unknown exception: space=%d path=%ls",
                        sp,
                        currentHtmlPath.wstring().c_str());
                    htmlEvents.clear();
                }

                return htmlEvents;
            };

        auto compactCurrentWithoutDrop =
            [](std::vector<std::vector<SearchEvent>>& bySpace)
            {
                if (!kSearchDataLightMode_) {
                    return;
                }

                for (auto& events : bySpace) {
                    for (auto& ev : events) {
                        CompactSearchEvent_(ev);
                    }
                }
            };

        std::vector<std::vector<SearchEvent>> currentBySpace(3);
        std::map<std::wstring, std::vector<std::vector<SearchEvent>>> archiveByDate;

        const std::wstring baseDate =
            GetBroadcastBaseDateText_(settings.searchStartHour);

        const std::wstring endDate =
            AddDaysToIsoDate_(baseDate, settings.futureDays);

        const std::wstring archiveStartDate =
            AddDaysToIsoDate_(baseDate, -settings.archiveDays);

        for (int sp = 0; sp <= 2; ++sp) {
            const std::vector<SearchDbInputFile> inputFiles =
                findEpgInputFilesForSpace(sp);

            std::vector<SearchEvent> searchEvents =
                loadEventsFromEpgFiles(sp, inputFiles);

            const auto serviceMap = BuildServiceMap(opt, sp);

            fillServiceNameFromMap(searchEvents, serviceMap);

            // channellist / enabledServicesBySpace に無いサービスは、
            // 基本的に検索DBから除外する。
            // これにより、無効化チャンネルや不要なSIDが出にくくなる。
            filterToKnownServices(searchEvents, serviceMap);

            dedupeAndPreferNamedService(searchEvents);

            // EPGファイルが無い、またはパース結果が空の場合だけHTML fallback。
            if (searchEvents.empty() &&
                (!kSearchDataLightMode_ || kEnableHtmlFallbackInLightMode_)) {
                DebugLogFormat(
                    L"[EPG-PORTAL][SEARCH][WARN] epg source empty. try html fallback: space=%d outputDir=%ls",
                    sp,
                    opt.outputDir.wstring().c_str());

                searchEvents = loadHtmlFallbackSafe(sp);
                fillServiceNameFromMap(searchEvents, serviceMap);
                filterToKnownServices(searchEvents, serviceMap);
                dedupeAndPreferNamedService(searchEvents);
            }

            std::size_t currentCount = 0;
            std::size_t archiveCount = 0;
            std::size_t ignoredFutureCount = 0;

            for (auto ev : searchEvents) {
                if (ev.dateText.empty() || ev.dateText.size() < 10) {
                    currentBySpace[static_cast<std::size_t>(sp)].push_back(std::move(ev));
                    ++currentCount;
                    continue;
                }

                if (IsIsoDateInRange_(ev.dateText, baseDate, endDate)) {
                    currentBySpace[static_cast<std::size_t>(sp)].push_back(std::move(ev));
                    ++currentCount;
                    continue;
                }

                if (ev.dateText < baseDate) {
                    if (buildArchiveFiles &&
                        (archiveStartDate.empty() || ev.dateText >= archiveStartDate)) {
                        auto& bySpace = archiveByDate[ev.dateText];
                        if (bySpace.empty()) {
                            bySpace.resize(3);
                        }

                        bySpace[static_cast<std::size_t>(sp)].push_back(std::move(ev));
                        ++archiveCount;
                    }
                    continue;
                }

                ++ignoredFutureCount;
            }

            DebugLogFormat(
                L"[EPG-PORTAL][SEARCH] build search data from epg files: space=%d files=%zu final=%zu current=%zu archive=%zu ignoredFuture=%zu base=%ls end=%ls",
                sp,
                inputFiles.size(),
                searchEvents.size(),
                currentCount,
                archiveCount,
                ignoredFutureCount,
                baseDate.c_str(),
                endDate.c_str());
        }

        compactCurrentWithoutDrop(currentBySpace);

        CompactArchiveSearchDataset_(archiveByDate);

        DebugLogFormat(
            L"[EPG-PORTAL][SEARCH][LIGHT] compact applied: current=%zu archiveDates=%zu light=%d archiveFiles=%d",
            CountEventsBySpace_(currentBySpace),
            archiveByDate.size(),
            kSearchDataLightMode_ ? 1 : 0,
            buildArchiveFiles ? 1 : 0);

        bool ok = true;

        if (!WriteSearchDatasetFile_(
            currentJsPath,
            L"EPG_SEARCH_DATA",
            currentBySpace,
            true)) {
            ok = false;
        }

        std::vector<ArchiveInfo> archiveInfos;

        if (buildArchiveFiles) {
            for (const auto& kv : archiveByDate) {
                const std::wstring& date = kv.first;
                const auto& bySpace = kv.second;

                const std::wstring compact = MakeDateCompact_(date);
                if (compact.empty()) {
                    continue;
                }

                const std::wstring fileName =
                    L"epg_search_" + compact + L".js";

                const std::filesystem::path archivePath =
                    archiveDirPath / fileName;

                if (!WriteSearchDatasetFile_(
                    archivePath,
                    L"EPG_SEARCH_ARCHIVE_DATA",
                    bySpace,
                    false)) {
                    ok = false;
                    continue;
                }

                ArchiveInfo info{};
                info.date = date;
                info.filePath = archivePath;
                info.relativeFile =
                    settings.archiveDir + L"/" + fileName;
                info.count = CountEventsBySpace_(bySpace);
                archiveInfos.push_back(std::move(info));
            }
        }

        if (!WriteSearchManifestJs_(
            manifestPath,
            settings,
            baseDate,
            endDate,
            archiveInfos)) {
            ok = false;
        }

        DebugLogFormat(
            L"[EPG-PORTAL][SEARCH] write split search data: current=%ls manifest=%ls archiveDir=%ls archives=%zu ok=%d",
            currentJsPath.wstring().c_str(),
            manifestPath.wstring().c_str(),
            archiveDirPath.wstring().c_str(),
            archiveInfos.size(),
            ok ? 1 : 0);

        return ok;
    }

    namespace
    {
        static std::wstring AutoReserveFormatTime_(
            int hour,
            int minute,
            int second)
        {
            wchar_t buf[16]{};
            swprintf_s(
                buf,
                L"%02d:%02d:%02d",
                (std::clamp)(hour, 0, 23),
                (std::clamp)(minute, 0, 59),
                (std::clamp)(second, 0, 59));

            return std::wstring(buf);
        }

        static int AutoReserveWeekdayIndexFromText_(
            const std::wstring& text)
        {
            if (text.find(L"月") != std::wstring::npos) return 0;
            if (text.find(L"火") != std::wstring::npos) return 1;
            if (text.find(L"水") != std::wstring::npos) return 2;
            if (text.find(L"木") != std::wstring::npos) return 3;
            if (text.find(L"金") != std::wstring::npos) return 4;
            if (text.find(L"土") != std::wstring::npos) return 5;
            if (text.find(L"日") != std::wstring::npos) return 6;

            return -1;
        }

        static AutoReserve::SearchProgram ToAutoReserveSearchProgram_(
            int space,
            const SearchEvent& ev)
        {
            AutoReserve::SearchProgram p{};
            p.space = space;

            p.serviceId = ev.serviceId;
            p.eventId = ev.eventId;

            p.dateText = ev.dateText;
            p.weekdayText = ev.weekdayText;
            p.timeText = ev.timeText;

            p.title = ev.title;
            p.summary = ev.summary;
            p.serviceName = ev.serviceName;
            p.genreName = ev.genreName;

            p.weekdayIndex =
                AutoReserveWeekdayIndexFromText_(ev.weekdayText);

            if (ev.hasStart) {
                p.startDate = ev.dateText;
                p.startTime =
                    AutoReserveFormatTime_(
                        ev.startHour,
                        ev.startMinute,
                        ev.startSecond);

                p.startMinutesOfDay =
                    ev.startHour * 60 + ev.startMinute;
            }

            if (ev.hasDuration) {
                p.durationSec =
                    static_cast<std::uint32_t>(
                        ev.durationHour * 3600 +
                        ev.durationMinute * 60 +
                        ev.durationSecond);

                if (ev.hasStart) {
                    const int startSec =
                        ev.startHour * 3600 +
                        ev.startMinute * 60 +
                        ev.startSecond;

                    const int endTotal =
                        startSec + static_cast<int>(p.durationSec);

                    const int dayOffset =
                        endTotal / (24 * 3600);

                    const int endSec =
                        endTotal % (24 * 3600);

                    const int endHour =
                        endSec / 3600;

                    const int endMinute =
                        (endSec % 3600) / 60;

                    const int endSecond =
                        endSec % 60;

                    p.endDate =
                        dayOffset > 0
                        ? AddDaysToIsoDate_(ev.dateText, dayOffset)
                        : ev.dateText;

                    p.endTime =
                        AutoReserveFormatTime_(
                            endHour,
                            endMinute,
                            endSecond);
                }
            }

            return p;
        }
    }

    std::vector<AutoReserve::SearchProgram> LoadCurrentAutoReserveSearchPrograms(
        const EpgHtmlPortal::WriteOptions& opt)
    {
        std::vector<AutoReserve::SearchProgram> out;

        const SearchDataSettings settings =
            LoadSearchDataSettings_();

        const std::wstring baseDate =
            GetBroadcastBaseDateText_(settings.searchStartHour);

        const std::wstring endDate =
            AddDaysToIsoDate_(baseDate, settings.futureDays);

        for (int space = 0; space <= 2; ++space) {
            const std::filesystem::path mergedPath =
                ResolveMergedEpgPathForSpace(opt, space);

            if (mergedPath.empty()) {
                continue;
            }

            std::vector<SearchEvent> events;

            try {
                events =
                    LoadMergedEvents(
                        mergedPath,
                        opt,
                        space);
            }
            catch (const std::exception& e) {
                DebugLogFormat(
                    L"[EPG-PORTAL][AUTO-SEARCH][ERROR] merged parse exception space=%d path=%ls what=%S",
                    space,
                    mergedPath.wstring().c_str(),
                    e.what());
                continue;
            }
            catch (...) {
                DebugLogFormat(
                    L"[EPG-PORTAL][AUTO-SEARCH][ERROR] merged parse unknown exception space=%d path=%ls",
                    space,
                    mergedPath.wstring().c_str());
                continue;
            }

            for (const auto& ev : events) {
                if (ev.title.empty()) {
                    continue;
                }

                if (!ev.dateText.empty() &&
                    ev.dateText.size() >= 10 &&
                    !IsIsoDateInRange_(ev.dateText, baseDate, endDate)) {
                    continue;
                }

                out.push_back(
                    ToAutoReserveSearchProgram_(
                        space,
                        ev));
            }

            DebugLogFormat(
                L"[EPG-PORTAL][AUTO-SEARCH] loaded programs space=%d count=%zu path=%ls base=%ls end=%ls",
                space,
                events.size(),
                mergedPath.wstring().c_str(),
                baseDate.c_str(),
                endDate.c_str());
        }

        DebugLogFormat(
            L"[EPG-PORTAL][AUTO-SEARCH] current search programs total=%zu",
            out.size());

        return out;
    }

    AutoReserve::RuleMatchResult FindAutoReserveMatchesFromCurrentSearchData(
        const EpgHtmlPortal::WriteOptions& opt,
        const std::vector<AutoReserve::Rule>& rules,
        const AutoReserve::RuleMatchOptions& matchOptions)
    {
        const std::vector<AutoReserve::SearchProgram> programs =
            LoadCurrentAutoReserveSearchPrograms(opt);

        AutoReserve::RuleMatchResult result =
            AutoReserve::FindMatchedPrograms(
                rules,
                programs,
                matchOptions);

        DebugLogFormat(
            L"[EPG-PORTAL][AUTO-SEARCH] match result ok=%d rules=%zu programs=%zu checked=%zu matched=%zu candidate=%zu auto=%zu",
            result.ok ? 1 : 0,
            result.ruleCount,
            result.programCount,
            result.checkedCount,
            result.matchedCount,
            result.candidateCount,
            result.autoRegisterCount);

        return result;
    }

    AutoReserve::RuleMatchResult RunAutoReserveMatchDryRunFromCurrentSearchData(
        const EpgHtmlPortal::WriteOptions& opt,
        std::size_t maxMatches)
    {
        AutoReserve::RuleStore store;

        const AutoReserve::LoadResult load =
            store.LoadFromIni(
                AutoReserve::GetDefaultRulesIniPath());

        if (!load.ok) {
            AutoReserve::RuleMatchResult result{};
            result.ok = false;
            result.message = L"auto reserve rule load failed: " + load.message;

            DebugLogFormat(
                L"[EPG-PORTAL][AUTO-SEARCH][DRYRUN][WARN] rule load failed: %ls",
                load.message.c_str());

            return result;
        }

        AutoReserve::RuleMatchOptions matchOptions{};
        matchOptions.includeCandidateRules = true;
        matchOptions.includeAutoRules = true;
        matchOptions.maxMatches = maxMatches;

        AutoReserve::RuleMatchResult result =
            FindAutoReserveMatchesFromCurrentSearchData(
                opt,
                store.GetRules(),
                matchOptions);

        DebugLogFormat(
            L"[EPG-PORTAL][AUTO-SEARCH][DRYRUN] rules=%zu programs=%zu checked=%zu matched=%zu candidate=%zu auto=%zu max=%zu",
            result.ruleCount,
            result.programCount,
            result.checkedCount,
            result.matchedCount,
            result.candidateCount,
            result.autoRegisterCount,
            maxMatches);

        const std::size_t logLimit =
            (std::min)(
                result.matches.size(),
                static_cast<std::size_t>(20));

        for (std::size_t i = 0; i < logLimit; ++i) {
            const AutoReserve::RuleProgramMatch& m =
                result.matches[i];

            DebugLogFormat(
                L"[EPG-PORTAL][AUTO-SEARCH][HIT] #%zu rule=%u auto=%d ruleName=%ls title=%ls service=%ls date=%ls time=%ls genre=%ls reason=%ls",
                i + 1,
                static_cast<unsigned>(m.ruleId),
                m.autoRegister ? 1 : 0,
                m.ruleName.c_str(),
                m.program.title.c_str(),
                m.program.serviceName.c_str(),
                m.program.dateText.c_str(),
                m.program.timeText.c_str(),
                m.program.genreName.c_str(),
                m.reason.c_str());
        }

        if (result.matches.size() > logLimit) {
            DebugLogFormat(
                L"[EPG-PORTAL][AUTO-SEARCH][HIT] omitted=%zu",
                result.matches.size() - logLimit);
        }

        return result;
    }


}    
