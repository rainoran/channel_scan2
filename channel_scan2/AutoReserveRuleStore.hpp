#pragma once
#ifndef AUTO_RESERVE_RULE_STORE_HPP
#define AUTO_RESERVE_RULE_STORE_HPP

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>


class TaskSystemConnector;

namespace EpgHtmlPortal
{
    struct WriteOptions;
}

namespace AutoReserve
{
    enum class MatchMode : int
    {
        Contains = 0,
        Exact = 1,
        StartsWith = 2,
        EndsWith = 3
    };

    enum class DuplicatePolicy : int
    {
        SkipSameEvent = 0,
        SkipSameTitleAndTime = 1,
        Allow = 2
    };

    enum class TokenJoinMode : int
    {
        Or = 0,
        And = 1
    };

    enum class ReserveActionMode : int
    {
        Candidate = 0,
        Auto = 1
    };

    struct Rule
    {
        std::uint32_t id = 0;
        bool enabled = true;
        std::wstring name;

        bool useKeyword = true;
        std::wstring keyword;
        TokenJoinMode keywordJoinMode = TokenJoinMode::Or;

        bool useExcludeKeyword = false;
        std::wstring excludeKeyword;
        TokenJoinMode excludeKeywordJoinMode = TokenJoinMode::Or;

        MatchMode titleMatchMode = MatchMode::Contains;
        bool matchTitle = true;
        bool matchSummary = false;
        bool matchExtended = false;

        int targetSpace = 0;

        bool useService = false;
        std::wstring serviceName;
        TokenJoinMode serviceMatchMode = TokenJoinMode::Or;

        bool useGenre = false;
        std::wstring genreName;
        TokenJoinMode genreMatchMode = TokenJoinMode::Or;

        bool useMinDuration = false;
        int minDurationMinutes = 0;

        bool useSearchTimeRange = false;
        int searchStartMinutes = 0;
        int searchEndMinutes = 1440;
        bool searchTimeWrap = false;

        // 旧コード互換用です。useSearchTimeRange=false の時は -1 を使います。
        int startMinutes = -1;
        int endMinutes = -1;

        bool useWeekday = false;
        std::wstring weekdays = L"1111111"; // 月火水木金土日

        int priority = 50;
        int preMarginSeconds = 0;
        int postMarginSeconds = 0;
        bool createReservationEnabled = true;
        DuplicatePolicy duplicatePolicy = DuplicatePolicy::SkipSameEvent;

        bool skipDuplicate = true;
        bool excludeRerunLikeProgram = false;
        ReserveActionMode actionMode = ReserveActionMode::Candidate;
    };

    struct LoadResult
    {
        bool ok = false;
        std::size_t loadedCount = 0;
        std::wstring message;
    };

    struct SaveResult
    {
        bool ok = false;
        std::size_t savedCount = 0;
        std::wstring message;
    };

    struct CacheResult
    {
        bool ok = false;
        bool updated = false;
        std::size_t ruleCount = 0;
        std::wstring cachePath;
        std::wstring message;
    };

    enum class RulesCacheRefreshMode : int
    {
        EnsureCurrent = 0,
        ForceRebuild = 1
    };

    class RuleStore
    {
    public:
        LoadResult LoadFromIni(const std::wstring& iniPath);
        SaveResult SaveToIni(const std::wstring& iniPath) const;

        const std::vector<Rule>& GetRules() const noexcept;
        void Clear();

        std::uint32_t AddRule(const Rule& rule);
        bool UpdateRule(const Rule& rule);
        bool RemoveRule(std::uint32_t id);

        Rule* FindRule(std::uint32_t id) noexcept;
        const Rule* FindRule(std::uint32_t id) const noexcept;

        std::uint32_t GetNextId() const noexcept;
        void SetNextId(std::uint32_t nextId) noexcept;

    private:
        std::uint32_t GenerateId_();

        std::vector<Rule> rules_;
        std::uint32_t nextId_ = 1;
    };

    std::wstring GetDefaultRulesIniPath();
    std::wstring GetDefaultRulesCacheDirectory();
    std::wstring GetDefaultRulesCachePath();

    // auto_reserve_rules.ini から派生する表示用キャッシュを管理します。
// キャッシュは UTF-8 INI 形式で
// cache\auto_reserve\auto_reserve_rules.cache へ保存されます。
    CacheResult WriteRulesCache(
        const RuleStore& store);

    // INIを再読込して無条件にキャッシュを生成します。
    // INI未作成の場合はルール0件のキャッシュを生成します。
    CacheResult RebuildRulesCache();

    // INIより古い、破損、未作成の場合だけ再生成します。
    CacheResult EnsureRulesCacheCurrent();

    // 各ユニットから利用する共通入口です。
    CacheResult RefreshRulesCache(
        RulesCacheRefreshMode mode);

    bool LoadRulesCacheUtf8(
        std::string& outCacheText,
        std::wstring* outMessage = nullptr);

    std::wstring MatchModeToString(MatchMode mode);
    MatchMode MatchModeFromInt(int value) noexcept;

    std::wstring DuplicatePolicyToString(DuplicatePolicy policy);
    DuplicatePolicy DuplicatePolicyFromInt(int value) noexcept;

    std::wstring TokenJoinModeToString(TokenJoinMode mode);
    TokenJoinMode TokenJoinModeFromString(const std::wstring& value) noexcept;

    std::wstring ReserveActionModeToString(ReserveActionMode mode);
    ReserveActionMode ReserveActionModeFromString(const std::wstring& value) noexcept;

    // POSTされた auto_reserve_rules.ini テキストを標準INIへ保存します。
// ローカルHTTP API側から呼び出す想定です。
    SaveResult SaveIniTextToDefaultRulesIni(
        const std::wstring& iniText);

    SaveResult SaveIniUtf8ToDefaultRulesIni(
        const std::string& iniUtf8);

    std::string MakeSaveResultJsonUtf8(
        const SaveResult& result);

    // application/x-www-form-urlencoded のPOST本文から iniText を取り出して保存します。
// EpgPortalReservationApiServer から直接呼び出す入口です。
    SaveResult SaveIniFormUtf8ToDefaultRulesIni(
        const std::string& formUtf8);

    struct SearchProgram
    {
        int space = 0;

        std::uint16_t networkId = 0;
        std::uint16_t tsId = 0;
        std::uint16_t serviceId = 0;
        std::uint16_t eventId = 0;

        std::wstring dateText;
        std::wstring weekdayText;
        std::wstring timeText;

        std::wstring startDate;
        std::wstring startTime;
        std::wstring endDate;
        std::wstring endTime;

        std::wstring title;
        std::wstring summary;
        std::wstring extendedText;
        std::wstring serviceName;
        std::wstring genreName;

        std::uint32_t durationSec = 0;

        int startMinutesOfDay = -1;

        // 月=0, 火=1, 水=2, 木=3, 金=4, 土=5, 日=6
        int weekdayIndex = -1;
    };

    struct RuleProgramMatch
    {
        std::uint32_t ruleId = 0;
        std::wstring ruleName;

        bool autoRegister = false;

        Rule rule;
        SearchProgram program;

        std::wstring reason;
    };

    struct RuleMatchOptions
    {
        bool includeCandidateRules = true;
        bool includeAutoRules = true;

        // 暴走防止用。0なら無制限。
        std::size_t maxMatches = 0;
    };

    struct RuleMatchResult
    {
        bool ok = false;

        std::size_t ruleCount = 0;
        std::size_t programCount = 0;
        std::size_t checkedCount = 0;

        std::size_t matchedCount = 0;
        std::size_t candidateCount = 0;
        std::size_t autoRegisterCount = 0;

        std::size_t skippedDisabledRule = 0;
        std::size_t skippedActionMode = 0;

        std::wstring message;

        std::vector<RuleProgramMatch> matches;
    };

    enum class ApplyTrigger : int
    {
        Unknown = 0,
        RuleSave = 1,
        EpgUpdate = 2,
        Manual = 3
    };

    struct CurrentSearchMatchRequest
    {
        ApplyTrigger trigger = ApplyTrigger::Unknown;
        std::wstring reason;

        // RunAutoReserveMatchDryRunFromCurrentSearchData() などへ渡す上限。
        // 0なら無制限扱い。
        std::size_t maxMatches = 1000;

        // 録画予約登録対象にするルール種別。
        bool includeCandidateRules = true;
        bool includeAutoRules = true;
    };

    struct CurrentSearchMatchSummary
    {
        bool ok = false;
        bool hasMatchedPrograms = false;
        bool shouldApplyToRecording = false;

        std::wstring reason;
        std::wstring message;

        std::size_t ruleCount = 0;
        std::size_t programCount = 0;
        std::size_t checkedCount = 0;

        std::size_t matchedCount = 0;
        std::size_t candidateCount = 0;
        std::size_t autoRegisterCount = 0;
    };

    struct CurrentSearchRegisterOptions
    {
        ApplyTrigger trigger = ApplyTrigger::Unknown;
        std::wstring reason;

        // 現在の search_data から拾う最大一致数。
        // 0なら検索側仕様に従います。
        std::size_t maxMatches = 1000;

        // candidate / auto のどちらを録画予約登録対象にするか。
        // 今回仕様では基本どちらも true。
        bool includeCandidateRules = true;
        bool includeAutoRules = true;

        // trueなら検索・重複判定だけ行い、tasks.ini へ登録しません。
        bool dryRun = false;
        bool allowRegister = true;

        bool skipDuplicate = true;
        bool protectUserBlockedReservation = true;

        // 0 = 無制限。
        std::size_t maxRegisterCount = 0;

        std::wstring reserveMode = L"record";
        std::wstring reserveSource;

        bool forceCreatedReservationEnabled = true;

        std::uint8_t requiredTuners = 1;
        int preferredTunerIndex = -1;

        bool refreshReservationDataJs = true;
    };

    struct CurrentSearchRegisterResult
    {
        bool ok = false;

        bool searchAttempted = false;
        bool searchOk = false;

        bool registrationAttempted = false;
        bool reservationJsOk = true;

        std::wstring reason;
        std::wstring message;

        CurrentSearchMatchRequest searchRequest{};
        CurrentSearchMatchSummary searchSummary{};
        RuleMatchResult matchResult{};

        std::size_t matchedCount = 0;
        std::size_t candidateCount = 0;
        std::size_t autoRegisterCount = 0;

        std::size_t registerInput = 0;
        std::size_t registerTarget = 0;
        std::size_t registered = 0;
        std::size_t dryRunSkipped = 0;
        std::size_t duplicateSkipped = 0;
        std::size_t userBlockedSkipped = 0;
        std::size_t invalidSkipped = 0;
        std::size_t limitSkipped = 0;
        std::size_t actionSkipped = 0;
        std::size_t failed = 0;

        std::wstring registerMessage;
    };

    CurrentSearchRegisterOptions MakeCurrentSearchRegisterOptions(
        ApplyTrigger trigger,
        std::size_t maxMatches = 1000);

    CurrentSearchRegisterResult RunCurrentSearchAndRegisterRecordingTasks(
        TaskSystemConnector* connector,
        const EpgHtmlPortal::WriteOptions& portalOpt,
        const CurrentSearchRegisterOptions& options) noexcept;

    std::wstring ApplyTriggerToReason(
        ApplyTrigger trigger);

    CurrentSearchMatchRequest MakeCurrentSearchMatchRequest(
        ApplyTrigger trigger,
        std::size_t maxMatches = 1000);

    CurrentSearchMatchSummary SummarizeRuleMatchResultForRecording(
        const RuleMatchResult& matchResult,
        const CurrentSearchMatchRequest& request) noexcept;


    bool IsProgramMatchedByRule(
        const Rule& rule,
        const SearchProgram& program,
        std::wstring* outReason);

    RuleMatchResult FindMatchedPrograms(
        const std::vector<Rule>& rules,
        const std::vector<SearchProgram>& programs,
        const RuleMatchOptions& options);

}

#endif // AUTO_RESERVE_RULE_STORE_HPP
